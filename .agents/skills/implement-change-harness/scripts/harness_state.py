#!/usr/bin/env python3
"""Issue-based change-harness records with exact Git publication and bounded gh task reads."""
import argparse
import copy
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
import time
from urllib.parse import urlsplit
from github_issues import GitHub, IssueError, digest

SCHEMA = 3
KEY = re.compile(r"FOS-\d{6,}\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
TOKEN = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}\Z")
REF = "refs/heads/harness-artifacts"
MAX_RECORD = 1024 * 1024


class Invalid(ValueError):
    pass


def require(ok, message):
    if not ok:
        raise Invalid(message)


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode()


def git(repo, *args, data=None, env=None):
    result = subprocess.run(["git", "-C", str(repo), *args], input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    if result.returncode:
        raise Invalid("git " + args[0] + ": " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def common_dir(repo):
    return Path(git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir").decode().strip()).resolve()


@contextmanager
def repository_lock(repo):
    try:
        import fcntl
    except ImportError as error:
        raise Invalid("unsupported platform: POSIX flock is required") from error
    # Never unlink this file: different inodes would admit two writers.
    with (common_dir(repo) / "harness-run.lock").open("a+") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise Invalid("repository harness lock is held; reconcile its owner") from error
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def state_path(key, run):
    require(TOKEN.fullmatch(key) and TOKEN.fullmatch(run), "invalid root/run identity")
    return f"runs/{key}/{run}/state.json"


def record_path(path):
    parts = PurePosixPath(path).parts
    require(not path.startswith("/") and ".." not in parts and str(PurePosixPath(path)) == path,
            "unsafe artifact path")
    if path in ("active.json", "allocations.json"):
        return
    require(len(parts) >= 4 and parts[0] == "runs" and TOKEN.fullmatch(parts[1])
            and TOKEN.fullmatch(parts[2]), "artifact tree must contain records only")
    require((len(parts) == 4 and parts[3] == "state.json") or
            (len(parts) == 5 and parts[3] in ("reports", "evidence") and
             TOKEN.fullmatch(parts[4]) and parts[4].endswith((".json", ".txt", ".log"))),
            "invalid artifact record location")


class Store:
    """Git ref is authoritative; independent temporary index, then update-ref CAS."""
    def __init__(self, repo, fail=None):
        self.repo = Path(repo).resolve()
        self.fail = fail  # deterministic fault seam for tests, never a CLI option
        self.validated_commits = set()
        self.record_cache = {}
        self.blob_cache = {}

    def tip(self):
        result = subprocess.run(["git", "-C", str(self.repo), "rev-parse", "--verify", REF],
                                capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else None

    def records(self, tip):
        if tip is None:
            return {}
        if tip in self.record_cache:
            return dict(self.record_cache[tip])
        listings = {}
        blobs = set()
        for commit in git(self.repo, "rev-list", tip).decode().splitlines():
            if commit in self.validated_commits:
                continue
            rows = []
            for entry in git(self.repo, "ls-tree", "-rz", commit).split(b"\0"):
                if not entry:
                    continue
                meta, raw_path = entry.split(b"\t", 1)
                mode, kind, blob = meta.decode().split()
                path = raw_path.decode()
                record_path(path)
                require(mode == "100644" and kind == "blob", "non-regular artifact record")
                rows.append((path, blob))
                if blob not in self.blob_cache:
                    blobs.add(blob)
            listings[commit] = rows
        if blobs:
            # One batch preserves per-blob validation while avoiding a process for
            # every historical path. Git object IDs and cached bytes are immutable.
            stream = git(self.repo, "cat-file", "--batch", data=("\n".join(sorted(blobs)) + "\n").encode())
            offset = 0
            for expected in sorted(blobs):
                end = stream.index(b"\n", offset)
                identity, kind, size = stream[offset:end].decode().split()
                require(identity == expected and kind == 'blob', 'invalid artifact blob response')
                size = int(size)
                content = stream[end+1:end+1+size]
                self.validate_content(content)
                require(len(content) == size and stream[end+1+size:end+2+size] == b"\n", 'truncated artifact blob')
                self.blob_cache[expected] = content
                offset = end + size + 2
        for commit, rows in listings.items():
            records = {path:self.blob_cache[blob] for path,blob in rows}
            if records:
                require('active.json' in records and 'allocations.json' in records, 'existing artifact ref lacks identity records')
            self.record_cache[commit] = records
            self.validated_commits.add(commit)
        return dict(self.record_cache[tip])

    @staticmethod
    def validate_content(content):
        require(len(content) <= MAX_RECORD and b"\0" not in content, "oversize/binary artifact")
        try:
            content.decode("utf8")
        except UnicodeError as error:
            raise Invalid("artifact is not UTF-8") from error

    def checkpoint(self, records, expected_tip, message):
        require(self.tip() == expected_tip, "artifact tip changed")
        for path, content in records.items():
            record_path(path)
            self.validate_content(content)
            if path.endswith(".json"):
                json.loads(content)
        def fault(stage):
            if self.fail == stage:
                raise Invalid("injected " + stage + " failure")
        with tempfile.TemporaryDirectory(prefix="harness-index-") as temporary:
            env = os.environ.copy()
            env["GIT_INDEX_FILE"] = str(Path(temporary) / "index")
            git(self.repo, "read-tree", "--empty", env=env)
            entries = []
            for path, content in sorted(records.items()):
                fault("write")
                identity = hashlib.sha1(b"blob " + str(len(content)).encode() + b"\0" + content).hexdigest()
                if identity not in self.blob_cache:
                    identity = git(self.repo, "hash-object", "-w", "--stdin", data=content).decode().strip()
                    self.blob_cache[identity] = content
                entries.append(("100644 " + identity + "\t" + path).encode() + b"\0")
            git(self.repo, "update-index", "-z", "--index-info", data=b"".join(entries), env=env)
            fault("tree")
            tree = git(self.repo, "write-tree", env=env).decode().strip()
            fault("commit")
            args = ["commit-tree", tree]
            if expected_tip:
                args += ["-p", expected_tip]
            commit = git(self.repo, *args, data=(message + "\n").encode()).decode().strip()
            fault("ref")
            git(self.repo, "update-ref", REF, commit, expected_tip or "0" * 40)
        return commit


def historical_inventory(repo, journal, revision):
    require(SHA.fullmatch(revision), "historical inventory requires immutable source SHA")
    """Project Markdown grammar: ledger rows and keyed headings/bold list items."""
    registry = {}
    definitions = {}
    files = git(repo, "ls-tree", "-r", "--name-only", revision, "local-docs").decode().splitlines()
    for path in files:
        if not path.endswith(".md"):
            continue
        content = git(repo, "show", f"{revision}:{path}").decode()
        for line in content.splitlines():
            if path == "local-docs/backlog-keys.md":
                match = re.match(r"\|\s*`(FOS-\d{6,})`\s*\|\s*(.*?)\s*\|\s*(.*?)\s*\|\s*(.*?)\s*\|", line)
                if match:
                    key, title, location, status = match.groups()
                    require(key not in registry, "duplicate registry key")
                    link = re.search(r"\]\(([^)]+)\)", location)
                    location = str(PurePosixPath("local-docs") / link[1]) if link else location
                    registry[key] = {"key": key, "title": title, "location": location, "status": status.lower()}
                continue
            match = re.match(r"(?:#{1,6}\s+|\d+\.\s+\*\*)(FOS-\d{6,})\s+[—–-]\s+(.+)", line)
            if match:
                key, title = match.groups()
                require(key not in definitions, "duplicate canonical definition: " + key)
                dependency = re.search(r"\bdepends on (.*?)(?:;|\*\*|$)", line)
                dependencies = re.findall(r"FOS-\d{6,}", dependency[1]) if dependency else []
                definitions[key] = {"location": path, "title": title.split("**")[0].rstrip(". "),
                                    "dependencies": dependencies}
    issued = {row["key"]: row for row in journal["issued"]}
    require(len(issued) == len(journal["issued"]), "duplicate allocation journal entry")
    for key, definition in definitions.items():
        require(key in registry, "unregistered canonical definition: " + key)
        require(registry[key]["location"].split("#")[0] == definition["location"], "canonical location conflict")
        require(registry[key]["title"] == definition["title"], "canonical meaning conflict")
        require(set(definition["dependencies"]) <= set(definitions), "dangling backlog dependency")
    done = set()
    while len(done) < len(definitions):
        ready = {key for key, row in definitions.items() if set(row["dependencies"]) <= done} - done
        require(ready, "cyclic backlog dependencies")
        done.update(ready)
    for key, row in issued.items():
        require(KEY.fullmatch(key), "invalid allocated key")
        if key in registry and row.get("title"):
            require(row["title"] == registry[key]["title"], "allocation meaning conflict")
    all_keys = set(registry) | set(issued) | set(definitions)
    high = max([journal.get("high_water", 0)] + [int(key[4:]) for key in all_keys])
    return registry, definitions, high


def validate_plan(plan, original, root, registry, journal):
    require(plan.get("version", 0) > 0 and plan.get("original") == original,
            "plan must preserve original acceptance")
    for name in ("scope", "non_goals", "implementation", "cases", "gates"):
        require(name in plan, "missing plan field: " + name)
    require(plan["gates"] and plan["cases"], "plan requires gates and assertion cases")
    ids = [gate["id"] for gate in plan["gates"]]
    require(len(ids) == len(set(ids)), "duplicate gate")
    criteria = {row["id"] for row in original}
    covered = set()
    for gate in plan["gates"]:
        require(gate["kind"] in ("work_unit", "final", "both", "publication", "optional"), "invalid gate kind")
        require(all(gate.get(field) for field in ("criteria", "method", "cwd", "prerequisites", "expected")),
                "incomplete gate design")
        require(set(gate["criteria"]) <= criteria, "unknown gate criterion")
        if gate["kind"] != "optional":
            covered.update(gate["criteria"])
    require(covered == criteria, "uncovered acceptance criterion")
    case_ids = [case["id"] for case in plan["cases"]]
    require(len(case_ids) == len(set(case_ids)) and all(case.get("assertions") and case.get("location")
                                                     for case in plan["cases"]), "incomplete test design")
    tasks = plan.get("work_units", [])
    keys = [task["key"] for task in tasks]
    require(root not in keys and len(keys) == len(set(keys)), "duplicate root/work_unit definition")
    for task in tasks:
        require(task["key"] in registry and task.get("issue") == task["key"], "work unit must reference a known issue")
        require(task.get("criteria") and task.get("definition") and set(task["dependencies"]) <= set(keys), "invalid dependency or definition")
    if tasks:
        require(set().union(*(set(task["criteria"]) for task in tasks)) == criteria, "split work units must cover every root criterion")
    done = set()
    while len(done) < len(keys):
        ready = {task["key"] for task in tasks if set(task["dependencies"]) <= done} - done
        require(ready, "cyclic dependencies")
        done.update(ready)


def validate_finding_scope(finding, root, keys):
    # Old schema-2 records omitted attribution. They remain root-wide forever.
    affected = finding.get("affected_task", root)
    require(isinstance(affected, str) and affected in keys | {root}, "invalid finding scope")
    return affected


def finding_applies(state, finding, operation, unit):
    affected = finding.get("affected_task", root_identity(state))
    units = {item["key"]: item for item in state["work_units"]}
    # Final/publication are global. An orphaned attribution after a replan also
    # remains global rather than silently disappearing with its former unit.
    if operation != "work_unit" or affected == root_identity(state) or affected not in units:
        return True
    require(unit in units, "unknown finding scope check unit")
    relevant, pending = set(), [unit]
    while pending:
        key = pending.pop()
        if key not in relevant:
            require(key in units, "invalid finding scope dependency")
            relevant.add(key)
            pending.extend(units[key]["dependencies"])
    return affected in relevant


def validate_state(state, repo, execution=True):
    require(state.get("schema_version") == SCHEMA, "unsupported schema; use explicit migrate-state/import-bootstrap")
    required = ("revision", "task_key", "run_id", "task_location", "original", "repository", "phase",
                "operation", "status", "next_action", "plan", "work_units", "active_work_unit", "attempts", "verification",
                "findings", "limits", "publishing", "final_acceptance", "provenance")
    require(all(field in state for field in required), "missing state fields")
    if "closeout" in state:
        require(state["closeout"].get("stage") in ("pending", "intent", "prepared", "cleanup"), "invalid closeout stage")
    state_path(state["task_key"], state["run_id"])
    require(isinstance(state["revision"], int) and state["revision"] > 0 and state["original"], "invalid state revision/original")
    original = state["original"]
    require(isinstance(original, list) and all(isinstance(c, dict) and c.get("id") and c.get("requirement") for c in original)
            and len({c["id"] for c in original}) == len(original), "invalid original acceptance")
    require(state["phase"] in ("initializing", "planning", "implementing", "checking", "closing", "publishing", "complete"), "invalid phase")
    require(state["status"] in ("ready", "running", "interrupted", "awaiting_decision", "failed", "complete"), "invalid status")
    finding_ids = [finding["id"] for finding in state["findings"]]
    require(len(finding_ids) == len(set(finding_ids)), "duplicate finding in durable state")
    known_units = {unit["key"] for unit in (state["plan"] or {}).get("work_units", [])}
    for entry in state["provenance"]:
        known_units.update(unit["key"] for unit in entry.get("superseded_plan", {}).get("work_units", []))
    for finding in state["findings"]:
        validate_finding_scope(finding, root_identity(state), known_units)
    issue = state.get('root_issue', {})
    require(issue.get('repository')=='robinwassen/Fossilize' and issue.get('id') and issue.get('node_id'), 'missing canonical issue identity')
    require(GitHub().number(issue.get('url')) == issue.get('number'), 'root issue identity mismatch')
    snapshot = state.get('issue_snapshot', {})
    require(snapshot.get('root')==issue['url'], 'wrong immutable issue snapshot')
    require(snapshot.get('digest')==digest({key:value for key,value in snapshot.items() if key!='digest'}), 'issue snapshot digest mismatch')
    units = state['work_units']
    keys = {unit['key'] for unit in units}
    require(len(keys)==len(units) and all(unit.get('issue')==unit['key'] and set(unit['dependencies'])<=keys for unit in units), 'invalid work unit to issue mapping')
    repository = state["repository"]
    require(not execution or repository["common_dir"] == str(common_dir(repo)), "wrong repository identity")
    for field in ("input_sha", "base_sha"):
        require(SHA.fullmatch(repository[field]), "invalid repository SHA")
    require(repository.get("candidate_sha") is None or SHA.fullmatch(repository["candidate_sha"]), "invalid candidate SHA")
    require(TOKEN.fullmatch(repository["target_remote"]) and
            re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository["target_repository"]), "invalid publication identity")
    for field in ("branch", "target_branch"):
        git(repo, "check-ref-format", "refs/heads/" + repository[field])
    require(repository["target_remote"] and repository["target_repository"] and repository["target_branch"]
            and repository["branch"] and repository["implementation_worktree"], "incomplete repository identity")
    require(Path(repository["implementation_worktree"]).resolve() != common_dir(repo), "invalid worktree")


def normalize_sha(repo, value):
    return git(repo, "rev-parse", "--verify", value + "^{commit}").decode().strip()


def initial_state(repo, payload, now):
    repository = copy.deepcopy(payload["repository"])
    repository["common_dir"] = str(common_dir(repo))
    for field in ("input_sha", "base_sha"):
        repository[field] = normalize_sha(repo, repository[field])
    repository["candidate_sha"] = None
    limits = {"repairs": 3, "replans": 1, "deadline": now + 4 * 3600, "command_timeout": 600,
              "implementation_counts": {}, "replan_count": 0, "extensions": [], "commands": [], "exhausted": False}
    limits.update(payload.get("limits", {}))
    require(limits["deadline"] > now and limits["command_timeout"] > 0, "invalid initial budget")
    return {"schema_version": SCHEMA, "revision": 1, "task_key": payload["task_key"], "run_id": payload["run_id"],
            "task_location": payload["task_location"], "original": payload["original"], "repository": repository,
            "phase": "planning", "operation": "plan", "status": "ready", "next_action": "dispatch fresh planner",
            "plan": None, "work_units": [], "active_work_unit": payload["task_key"], "attempts": [], "verification": [],
            "findings": [], "limits": limits, "publishing": {"stage": "none", "ci_policy": payload.get("ci_policy", "local")},
            "final_acceptance": None, "provenance": []}


POLICY = "artifact_publication"


def artifact_destination(repo, repository):
    """Bind a single actual push endpoint without persisting credentials/config."""
    require(repository["target_remote"] == "origin", "artifact destination must be origin")
    urls = git(repo, "remote", "get-url", "--push", "--all", "origin").decode().splitlines()
    require(len(urls) == 1, "artifact origin must have exactly one push endpoint")
    endpoint = urls[0]
    # For hosted endpoints check the declared owner/repository too. Local file
    # transports are useful for offline repositories and real bare-remote tests.
    if "://" in endpoint and not endpoint.startswith("file://"):
        parsed = urlsplit(endpoint)
        require(parsed.scheme in ("https", "ssh"), "unsupported artifact transport")
        identity = parsed.path.strip("/").removesuffix(".git")
        require(identity == repository["target_repository"], "artifact repository endpoint mismatch")
    elif re.match(r"[^/]+@[^:]+:", endpoint):
        require(endpoint.split(":", 1)[1].strip("/").removesuffix(".git") == repository["target_repository"],
                "artifact repository endpoint mismatch")
    else:
        require(endpoint.startswith(("/", "./", "../", "file://")), "unsupported artifact endpoint")
    return {"version": 1, "remote": "origin", "repository": repository["target_repository"],
            "ref": REF, "endpoint_sha256": hashlib.sha256(endpoint.encode()).hexdigest()}


def root_identity(state):
    return state["root_issue"]["url"]


def normalize_live(value, mapping):
    """Explicit schema-2 codec. Immutable source/report/provenance records are not rewritten."""
    if isinstance(value, list):
        return [normalize_live(item, mapping) for item in value]
    if isinstance(value, dict):
        names = {'subtasks':'work_units', 'units':'work_units', 'active_unit':'active_work_unit', 'unit':'work_unit'}
        return {names.get(key,key): (item if key in ('original','provenance') else normalize_live(item,mapping)) for key,item in value.items()}
    if isinstance(value, str):
        if value == 'subtask':
            return 'work_unit'
        return mapping.get(value, value)
    return value


class Harness:
    def __init__(self, repo, clock=time.time, fail=None):
        self.repo = Path(repo).resolve()
        self.store = Store(repo, fail)
        self.clock = clock
        self.validated_artifact_history = set()

    def artifact_policy(self, records):
        return json.loads(records.get("allocations.json", b"{}" )).get(POLICY)

    def validate_artifact_policy(self, policy):
        require(isinstance(policy, dict) and policy.get("version") == 1 and policy.get("ref") == REF
                and policy.get("remote") == "origin", "invalid artifact publication policy")
        require(policy.get("review_summary") and (policy.get("reviewed_tip") is None
                or SHA.fullmatch(policy["reviewed_tip"])), "invalid artifact privacy review identity")
        for path, content in self.store.records(self.store.tip()).items():
            if path.endswith("/state.json"):
                repository = json.loads(content).get("repository", {})
                require(repository.get("target_remote") == "origin" and repository.get("target_repository") == policy["repository"],
                        "artifact repository identity differs from retained run")
        actual = artifact_destination(self.repo, {"target_remote": "origin", "target_repository": policy["repository"]})
        require(all(policy.get(k) == v for k, v in actual.items()), "artifact endpoint changed; restore recorded destination and retry")
        endpoint = git(self.repo, "remote", "get-url", "--push", "origin").decode().strip()
        require(hashlib.sha256(endpoint.encode()).hexdigest() == policy["endpoint_sha256"],
                "artifact endpoint changed during binding; restore recorded destination and retry")
        return endpoint

    def remote_history(self, tip, local_records, policy, preserve=True, bound=True):
        """Validate every adopted ancestor, then ensure no local durable fact is lost."""
        remote_records = self.store.records(tip)
        policy_identity = hashlib.sha256(encoded(policy)).hexdigest()
        for ancestry in git(self.repo, "rev-list", "--parents", tip).decode().splitlines():
            commit, *parents = ancestry.split()
            cache_key = (commit, policy_identity)
            if cache_key in self.validated_artifact_history:
                continue
            if not git(self.repo, "ls-tree", "-r", "--name-only", commit).strip():
                require(len(git(self.repo, "rev-list", "--parents", "-n", "1", commit).split()) == 1,
                        "empty non-root artifact ancestor")
                self.validated_artifact_history.add(cache_key)
                continue  # Historical orphan bootstrap root accepted by Store.
            rows = self.store.records(commit)
            active = json.loads(rows["active.json"])
            require(active is None or (isinstance(active, dict) and active.get("state") in rows),
                    "remote active pointer lacks a state record")
            self.validate_run_records(rows, active)
            journal = json.loads(rows["allocations.json"])
            require(isinstance(journal.get("high_water"), int) and journal["high_water"] >= 0
                    and isinstance(journal.get("issued"), list), "malformed remote allocation identity")
            keys = [r["key"] for r in journal["issued"]]
            require(all(KEY.fullmatch(k) for k in keys) and len(keys) == len(set(keys))
                    and all(int(k[4:]) <= journal["high_water"] for k in keys), "invalid remote allocation identity")
            for path, content in rows.items():
                if path.endswith(".json"):
                    json.loads(content)
                if path.endswith("/state.json"):
                    state = json.loads(content)
                    require(state.get("schema_version") in (1, 2, 3), "invalid remote state schema")
                    repository = state.get("repository", {})
                    require(repository.get("target_remote") == "origin" and repository.get("target_repository") == policy["repository"],
                            "remote run repository differs from bound artifact destination")
                    if state.get("schema_version") == 3:
                        validate_state(state, self.repo, execution=False)
                    elif state.get("schema_version") == 2:
                        import legacy_state
                        historical = copy.deepcopy(state)
                        historical['repository']['common_dir'] = str(common_dir(self.repo))
                        legacy_state.validate_state(historical, self.repo)
                    if active and active["state"] == path:
                        self.validate_pointer(active, state)
            # Permanent reservations survive every ancestry edge, including each
            # merge parent. Comparing only final local/remote journals misses an
            # intermediate issue followed by removal, rewriting or high-water loss.
            for parent in parents:
                if not git(self.repo, "ls-tree", "-r", "--name-only", parent).strip():
                    continue  # Empty orphan bootstrap root is validated separately.
                parent_rows = self.store.records(parent)
                parent_journal = json.loads(parent_rows["allocations.json"])
                retained = journal["issued"][:len(parent_journal["issued"])]
                expected = copy.deepcopy(parent_journal["issued"])
                if retained != expected:
                    # Explicit schema-1 import historically resolved registry
                    # placeholders. Verify that exact migration, never waive
                    # retention merely because an ancestor has an old schema.
                    for path, content in rows.items():
                        if not path.endswith("/state.json") or path not in parent_rows:
                            continue
                        old_state, state = json.loads(parent_rows[path]), json.loads(content)
                        imports = [p for p in state.get("provenance", [])
                                   if p.get("imported_from_tip") == parent and p.get("legacy_schema") == 1
                                   and p.get("legacy_state") == old_state]
                        if old_state.get("schema_version") != 1 or state.get("schema_version") != 2 or not imports:
                            continue
                        revision = state["repository"].get("candidate_sha") or state["repository"]["input_sha"]
                        registry, _, _ = historical_inventory(self.repo, parent_journal, revision)
                        for row in expected:
                            registered = registry.get(row["key"])
                            if (registered and row.get("location") == "local-docs/backlog-keys.md"
                                    and (not row.get("title") or row["title"] == registered["title"])):
                                row.update(location=registered["location"], title=registered["title"])
                require(retained == expected and journal["high_water"] >= parent_journal["high_water"],
                        "artifact ancestor drops issued allocations")
            old_policy = journal.get(POLICY)
            if old_policy:
                require(old_policy == policy, "remote artifact policy identity conflict")
            self.validated_artifact_history.add(cache_key)
        if bound:
            require(self.artifact_policy(remote_records) == policy, "remote publication policy missing/changed")
        if not preserve:
            return remote_records
        old_journal = json.loads(local_records["allocations.json"])
        new_journal = json.loads(remote_records["allocations.json"])
        require(new_journal["issued"][:len(old_journal["issued"])] == old_journal["issued"]
                and new_journal["high_water"] >= old_journal["high_water"], "remote drops issued allocations")
        for path, content in local_records.items():
            if path in ("active.json", "allocations.json"):
                continue
            require(path in remote_records, "remote drops local record")
            if path.endswith("/state.json"):
                old, new = json.loads(content), json.loads(remote_records[path])
                for key in ("task_key", "run_id", "original", "task_location"):
                    require(new.get(key) == old.get(key), "remote changes immutable run identity")
                require(new["revision"] >= old["revision"], "remote state revision regressed")
                require(content == remote_records[path],
                        "remote run state changed; preserve both tips and coordinate explicit reconciliation before adoption")
                if old.get("phase") == "complete":
                    require(content == remote_records[path], "remote rewrites completed run")
            else:
                require(content == remote_records[path], "remote rewrites append-only record")
        return remote_records

    def artifact_sync(self, tip, records, push=False, adopt=False):
        """One bounded exact-ref attempt; no committed acknowledgements or force."""
        policy = self.artifact_policy(records)
        result = {"status": "incomplete", "checkpoint_sha": tip, "remote_sha": None,
                  "destination": policy, "next_action": "adopt-artifacts with reviewed_tip and semantic review summary"}
        if not policy:
            result["outcome"] = "policy-adoption-required"
            return result
        result["next_action"] = "publish-artifacts with current expected tip/revision; inspect show first"
        deadline = time.monotonic() + 60
        def transport(*args):
            try:
                completed = subprocess.run(["git", "-C", str(self.repo), *args], capture_output=True,
                                           timeout=max(0.001, min(20, deadline - time.monotonic())),
                                           env=dict(os.environ, GIT_TERMINAL_PROMPT="0"))
            except subprocess.TimeoutExpired as error:
                raise Invalid("artifact transport deadline exceeded") from error
            require(completed.returncode == 0, "artifact transport failed (" + args[0] + "); restore network/authentication or inspect remote rejection")
            return completed.stdout
        def observe():
            lines = transport("ls-remote", "--refs", endpoint, REF).decode().splitlines()
            require(len(lines) <= 1, "ambiguous remote artifact ref")
            if not lines:
                return None
            fields = lines[0].split()
            require(len(fields) == 2 and fields[1] == REF and SHA.fullmatch(fields[0]), "malformed remote artifact ref")
            return fields[0]
        try:
            endpoint = self.validate_artifact_policy(policy)
            self.remote_history(tip, records, policy, preserve=False)
            remote = observe()
            result["remote_sha"] = remote
            if remote and remote != tip:
                # Dedicated fetch ref preserves the observed competing tip for inspection.
                fetched = "refs/harness/observed/" + remote
                transport("fetch", "--no-tags", "--no-write-fetch-head", endpoint, REF + ":" + fetched)
                require(git(self.repo, "rev-parse", fetched).decode().strip() == remote, "remote advanced during fetch; retry observation")
                ancestors = git(self.repo, "rev-list", remote).decode().splitlines()
                if tip in ancestors:
                    self.remote_history(remote, records, policy)
                    if not adopt:
                        result.update(outcome="remote-advance-pending", next_action="publish-artifacts to validate and adopt remote advance")
                        return result
                    git(self.repo, "update-ref", REF, remote, tip)
                    result.update(outcome="remote-advance-adopted", adopted_sha=remote,
                                  next_action="show, then retry intended action at adopted tip/revision")
                    return result
                if remote not in git(self.repo, "rev-list", tip).decode().splitlines():
                    self.remote_history(remote, records, policy, preserve=False)
                    raise Invalid("divergent artifact history; preserve both tips, inspect refs/harness/observed/" + remote
                        + " and local checkpoint; coordinate writers and explicitly reconcile before retry")
            if remote == tip:
                result.update(status="confirmed", outcome="already-published", next_action="continue saved operation")
                return result
            if not push:
                result["outcome"] = "local-checkpoint-pending"
                return result
            if self.store.fail == "before-push":
                raise Invalid("injected interruption before artifact push")
            transport("push", "--porcelain", endpoint, tip + ":" + REF)
            if self.store.fail == "after-push":
                raise Invalid("injected interruption after artifact push; confirmation unknown")
            actual = observe()
            result["remote_sha"] = actual
            require(actual == tip, "remote artifact tip changed after push; reconcile actual remote before continuing")
            result.update(status="confirmed", outcome="published", next_action="continue saved operation")
        except (Invalid, OSError, ValueError, KeyError, TypeError) as error:
            result.update(outcome="publication-incomplete", detail=str(error))
        return result

    def issue_registry(self, state):
        urls = {root_identity(state)} | {unit['key'] for unit in (state.get('plan') or {}).get('work_units', [])}
        return {url: GitHub().issue(url) for url in urls}

    def migrate_state(self, old, payload, tip, journal):
        require(old.get('schema_version') == 2, 'migrate-state requires explicit schema2 input; schema1 uses import-bootstrap first')
        require(payload.get('legacy_root') == old['task_key'] and payload.get('run_id') == old['run_id'], 'migration identity mismatch')
        require(payload.get('original') == old['original'], 'migration must preserve exact original acceptance')
        mapping = payload['mapping']
        require(len(set(mapping.values())) == len(mapping), 'duplicate issue mapping')
        require(old['task_key'] in mapping, 'missing root issue mapping')
        snapshot = GitHub().snapshot(mapping[old['task_key']], [mapping[unit['key']] for unit in old['units']])
        require(snapshot == payload['snapshot'], 'live migration snapshot differs')
        require(GitHub.ready(snapshot), 'migration root prerequisites incomplete')
        if old['operation'] == 'bootstrap-reconciliation':
            imported = next(entry for entry in old['provenance'] if entry.get('legacy_schema') == 1)
            workers = {attempt['worker_id'] for attempt in imported['legacy_state'].get('attempts', []) if attempt.get('worker_id')}
            require(payload.get('observation') and set(payload.get('terminated_workers', [])) >= workers,
                    'concrete termination evidence for every schema1 worker required before issue migration')
        active = [a for a in old['attempts'] if a['status'] not in ('complete','terminated')]
        bridge = None
        if active:
            require(len(active) == 1 and active[0]['role'] == 'implementer' and active[0]['status'] == 'running', 'only acknowledged implementer may bridge migration')
            import legacy_state
            bridge = legacy_state.Harness.tuple_fields(active[0])
            require(payload.get('bridge') == bridge, 'exact acknowledged dispatch envelope required')
        registry, definitions, high = historical_inventory(self.repo, journal, old['repository']['input_sha'])
        required = {old['task_key']} | {unit['key'] for unit in old['units']}
        require(required <= set(mapping), 'missing live unit mapping')
        for key in required:
            GitHub().number(mapping[key])
        state = normalize_live(copy.deepcopy(old), mapping)
        state['schema_version'] = 3
        # The original storage/run token and all artifact paths are immutable.
        state['task_key'] = old['task_key']
        state['run_id'] = old['run_id']
        state['revision'] += 1
        root = snapshot['nodes'][snapshot['root']]
        state['root_issue'] = {k:root[k] for k in ('url','id','node_id','repository','number')}
        state['task_location'] = root['url']
        state['issue_snapshot'] = snapshot
        for unit in state['work_units']:
            unit.update(issue=unit['key'], status='ready', accepted=None)
        state['limits']['implementation_counts'] = {mapping.get(key,key):value for key,value in old['limits']['implementation_counts'].items()}
        state['final_acceptance'] = None
        state['closeout'] = {'stage':'pending'}
        state['provenance'].append({'migration_from_tip':tip, 'legacy_schema':2, 'legacy_state':old,
            'registry_union':registry, 'definitions':definitions, 'legacy_high_water':high,
            'mapping':mapping, 'normalization':'schema2 task/run identity retained; live work units reference issues'})
        state['legacy_report_bridge'] = {'envelope':bridge, 'consumed':False} if bridge else None
        if old['operation']=='bootstrap-reconciliation':
            state['provenance'][-1]['bootstrap_reconciliation']={'observation':payload['observation'],'terminated_workers':payload['terminated_workers']}
        if not active:
            state.update(phase='implementing' if state['plan'] else 'planning', operation='implement' if state['plan'] else 'plan', status='ready')
        validate_state(state, self.repo)
        return state

    def artifact_result(self, tip, records, state, push=True, **extra):
        publication = self.artifact_sync(tip, records, push=push, adopt=push)
        return self.publication_result(tip, records, state, publication, **extra)

    def publication_result(self, tip, records, state, publication, **extra):
        actual_tip = self.store.tip()
        if actual_tip != tip:
            records = self.store.records(actual_tip)
            active = json.loads(records["active.json"])
            state = json.loads(records[active["state"]]) if active else None
        publication["retry"] = {"action": "publish-artifacts", "expected_tip": actual_tip,
                                "expected_revision": state["revision"] if state and json.loads(records["active.json"]) else 0, "payload": {}}
        if publication.get("outcome") == "policy-adoption-required":
            publication["retry"].update(action="adopt-artifacts", payload={"reviewed_tip": actual_tip},
                                        required_payload=["review_summary", "repository if active pointer is null"])
        terminal = bool(records) and json.loads(records["active.json"]) is None
        return dict(tip=actual_tip, state=state, artifact_publication=publication,
                    workflow_complete=terminal and publication["status"] == "confirmed", **extra)

    def show(self):
        tip = self.store.tip()
        records = self.store.records(tip)
        active = json.loads(records.get("active.json", b"null"))
        state = json.loads(records[active["state"]]) if active else None
        self.validate_run_records(records, active)
        if state:
            self.normalize_closeout(state)
            validate_state(state, self.repo)
            self.validate_pointer(active, state)
        return dict(self.artifact_result(tip, records, state, push=False), active=active) if tip else {"tip": tip, "active": active, "state": state}

    @staticmethod
    def validate_run_records(records, active):
        for path, content in records.items():
            if path.endswith("/state.json"):
                record = json.loads(content)
                require(path == state_path(record["task_key"], record["run_id"]), "state path identity conflict")
                require((active is not None and path == active["state"]) or record.get("phase") == "complete",
                        "orphan unfinished run in existing artifact tree")

    @staticmethod
    def validate_pointer(active, state):
        require(active == {"task_key": state["task_key"], "run_id": state["run_id"],
                           "state": state_path(state["task_key"], state["run_id"])}, "active pointer identity conflict")
        require(state["phase"] != "complete", "completed run retains active pointer")

    def apply(self, action, payload, expected_tip, expected_revision):
        if action == "import-bootstrap":
            # Explicit historical schema1 -> schema2 import is isolated; it never
            # serves normal task reads. Follow immediately with migrate-state.
            import legacy_state
            try:
                return legacy_state.Harness(self.repo, clock=self.clock).apply(action,payload,expected_tip,expected_revision)
            except legacy_state.Invalid as error:
                raise Invalid(str(error)) from error
        with repository_lock(self.repo):
            tip = self.store.tip()
            require(tip == expected_tip, "artifact tip changed")
            records = self.store.records(tip)
            active = json.loads(records.get("active.json", b"null"))
            journal = json.loads(records.get("allocations.json", b'{"high_water":0,"issued":[]}'))
            state = json.loads(records[active["state"]]) if active else None
            if action == "records-only":
                require(state is not None and state["revision"] == expected_revision, "wrong expected revision/no active run")
                publication = self.artifact_sync(tip, records, adopt=True)
                if publication["status"] != "confirmed":
                    return self.publication_result(tip, records, state, publication)
                additions = payload.get("records", {})
                require(additions and set(payload) == {"records"}, "records-only accepts bounded evidence only")
                prefix = f"runs/{state['task_key']}/{state['run_id']}/evidence/"
                for path, value in additions.items():
                    record_path(path)
                    require(path.startswith(prefix) and path not in records, "append-only active-run evidence required")
                    records[path] = value.encode() if isinstance(value, str) else encoded(value)
                tip = self.store.checkpoint(records, tip, "Append external-operation evidence; canonical state unchanged")
                return self.artifact_result(tip, records, state)
            if action == "migrate-state":
                require(state is not None and state["revision"] == expected_revision, "wrong migration revision")
                publication = self.artifact_sync(tip, records, adopt=True)
                require(publication["status"] == "confirmed", "publish current artifact checkpoint before migration")
                state = self.migrate_state(state, payload, tip, journal)
                records[active["state"]] = encoded(state)
                tip = self.store.checkpoint(records, tip, "Explicit issue/work-unit schema migration")
                return self.artifact_result(tip, records, state)
            if action == "rebind-execution":
                require(state is not None and state.get("schema_version") == 3 and state["revision"] == expected_revision, "wrong rebind identity")
                require(payload.get("root_issue") == state["root_issue"]["url"] and payload.get("run_id") == state["run_id"] and payload.get("old_common_dir") == state["repository"]["common_dir"], "explicit prior execution identity required")
                require(artifact_destination(self.repo, state["repository"]) == {k:v for k,v in journal[POLICY].items() if k in ('version','remote','repository','ref','endpoint_sha256')}, "rebind destination changed")
                old = copy.deepcopy(state["repository"])
                state["repository"]["common_dir"] = str(common_dir(self.repo))
                state["repository"]["implementation_worktree"] = str(self.repo)
                self.source(state, payload["candidate_sha"])
                state["provenance"].append({"execution_rebind": old})
                state["revision"] += 1
                records[active["state"]] = encoded(state)
                tip = self.store.checkpoint(records, tip, "Explicit execution-location rebind")
                return self.artifact_result(tip, records, state)
            if action in ("publish-artifacts", "adopt-artifacts"):
                require((state["revision"] if state else 0) == expected_revision, "wrong expected revision")
                if action == "adopt-artifacts":
                    require(not journal.get(POLICY), "artifact policy already bound")
                    require(payload.get("reviewed_tip") == tip and payload.get("review_summary"),
                            "explicit all-history semantic privacy review required at exact tip")
                    repository = payload.get("repository") or (state or {}).get("repository")
                    require(repository is not None, "explicit repository identity required")
                    policy = artifact_destination(self.repo, repository)
                    policy.update(reviewed_tip=tip, review_summary=payload["review_summary"])
                    self.remote_history(tip, records, policy, preserve=False, bound=False)
                    journal[POLICY] = policy
                    records["allocations.json"] = encoded(journal)
                    tip = self.store.checkpoint(records, tip, "Adopt reviewed artifact publication policy")
                return self.artifact_result(tip, records, state)
            if tip and journal.get(POLICY):
                publication = self.artifact_sync(tip, records, adopt=True)
                # Liveness/late report facts may be retained during an outage, but
                # no dispatch, allocation, source publication or completion advances.
                safety = action in ("report", "acknowledge", "reconcile", "budget-extension")
                if publication["status"] != "confirmed" and (not safety or publication.get("adopted_sha")):
                    return self.publication_result(tip, records, state, publication)
            elif tip and action != "import-bootstrap":
                return self.artifact_result(tip, records, state, push=False)
            if state:
                self.normalize_closeout(state)
                validate_state(state, self.repo)
                self.validate_pointer(active, state)
            self.validate_run_records(records, active)
            if action == "initialize":
                payload = copy.deepcopy(payload)
                require(payload['repository']['target_repository']=='robinwassen/Fossilize', 'explicit fork publication destination required')
                number = GitHub().number(payload["root_issue"])
                payload["task_key"] = "issue-" + str(number)
                payload["task_location"] = f"https://github.com/robinwassen/Fossilize/issues/{number}"
                if state:
                    require((root_identity(state), state["run_id"]) == (payload["task_location"], payload["run_id"]), "another run is active")
                    require(state["revision"] == expected_revision, "wrong expected revision")
                    GitHub().unchanged(state["issue_snapshot"])
                    return self.artifact_result(tip, records, state, resumed=True)
                require(expected_revision == 0, "initial revision must be zero")
                historical_runs=[json.loads(content) for location,content in records.items() if location.endswith('/state.json')]
                legacy_identities={}
                for historical in historical_runs:
                    for entry in historical.get('provenance',[]):
                        for key,url in entry.get('mapping',{}).items():
                            if KEY.fullmatch(key):
                                require(key not in legacy_identities or legacy_identities[key]==url, 'conflicting historical task identity')
                                legacy_identities[key]=url
                for historical in historical_runs:
                    identity=historical.get('root_issue',{}).get('url') or legacy_identities.get(historical['task_key'])
                    require((identity,historical['run_id']) != (payload['task_location'],payload['run_id']), 'root/run identity already used across storage migration')
                path = state_path(payload["task_key"], payload["run_id"])
                require(path not in records, "run identity already used")
                snapshot = GitHub().snapshot(payload['root_issue'])
                require(GitHub.ready(snapshot), 'root prerequisites are incomplete/cancelled')
                root = snapshot['nodes'][snapshot['root']]
                require(root['state']=='open', 'reopen completed/cancelled root before a new implementation run')
                derived = [{'id':'AC1','requirement':root['body']}]
                require('original' not in payload or payload['original'] == derived, 'new run original must be exact issue body snapshot')
                payload['original'] = derived
                state = initial_state(self.repo, payload, self.clock())
                if not journal.get(POLICY):
                    journal[POLICY] = artifact_destination(self.repo, state["repository"])
                    journal[POLICY].update(reviewed_tip=None, review_summary="New orphan records; no prior history")
                require(journal[POLICY]["repository"] == state["repository"]["target_repository"], "wrong artifact repository identity")
                state.update(root_issue={k: root[k] for k in ('url','id','node_id','repository','number')},
                             issue_snapshot=snapshot, task_location=root['url'], active_work_unit=root['url'])
            else:
                require(state is not None, "no active run")
                require(state["revision"] == expected_revision, "wrong expected revision")
                state = copy.deepcopy(state)
                state["revision"] += 1
                self.reduce(state, action, payload, journal, records)
            validate_state(state, self.repo)
            path = state_path(state["task_key"], state["run_id"])
            records[path] = encoded(state)
            records["allocations.json"] = encoded(journal)
            records["active.json"] = encoded(None if state["phase"] == "complete" else
                                             {"task_key": state["task_key"], "run_id": state["run_id"], "state": path})
            new_tip = self.store.checkpoint(records, tip, f"{state['task_key']} {action} revision {state['revision']}")
            return self.artifact_result(new_tip, records, state)

    @staticmethod
    def units(state):
        tasks = state["plan"].get("work_units") or [{"key": root_identity(state), "issue": root_identity(state), "dependencies": []}]
        return [dict(task, status="ready", accepted=None) for task in tasks]

    @staticmethod
    def ready_unit(state):
        done = {unit["key"] for unit in state["work_units"] if unit["status"] == "accepted"}
        ready = next((unit for unit in state["work_units"] if unit["status"] != "accepted"
                      and set(unit["dependencies"]) <= done), None)
        require(ready is not None, "no dependency-ready unit")
        return ready

    @staticmethod
    def pause_budget(state, reason):
        limits = state["limits"]
        limits["exhausted"] = True
        if state["operation"] == "budget":
            return
        limits.update(resume_operation=state["operation"], resume_status=state["status"],
                      resume_next_action=state["next_action"])
        state.update(status="awaiting_decision", operation="budget",
                     next_action="request explicit user budget extension: " + reason
                     + "; then " + limits.get("resume_next_action", "resume saved operation"))

    @staticmethod
    def restore_budget_step(state):
        limits = state["limits"]
        state.update(operation=limits.pop("resume_operation", "implement"),
                     status=limits.pop("resume_status", "ready"),
                     next_action=limits.pop("resume_next_action", "dispatch within extended budget"))

    def source(self, state, candidate=None):
        path = Path(state["repository"]["implementation_worktree"])
        require(common_dir(path) == common_dir(self.repo), "source worktree belongs to another repository")
        head = normalize_sha(path, "HEAD")
        require(git(path, "symbolic-ref", "--short", "HEAD").decode().strip() == state["repository"]["branch"], "implementation branch changed")
        require(not git(path, "status", "--porcelain", "--untracked-files=all").strip(), "source is dirty/untracked")
        require(head == (candidate or state["repository"]["candidate_sha"]), "source HEAD changed; recheck required")
        return head

    @staticmethod
    def current_attempt(state):
        require(state["attempts"] and state["attempts"][-1]["status"] in ("intent", "running", "ambiguous"), "no active attempt")
        return state["attempts"][-1]

    @staticmethod
    def tuple_fields(attempt):
        return {field: attempt[field] for field in ("role", "attempt_id", "worker_id", "input_revision", "input_sha", "candidate_sha", "base_sha", "plan_version", "operation", "work_unit")}

    def evidence(self, state, report, records):
        operation = report["operation"]
        plan = state["plan"]
        gates = {gate["id"]: gate for gate in plan["gates"] if gate["kind"] in (operation, "both")}
        provided = {entry["gate"]: entry for entry in report.get("evidence", [])}
        require(set(gates) <= set(provided), "missing mandatory gate")
        require(not any(finding.get("status") != "resolved" for finding in report.get("findings", [])), "unresolved finding")
        require(not any(finding.get("status") != "resolved" and
                        finding_applies(state, finding, operation, report["work_unit"])
                        for finding in state["findings"]), "unresolved prior finding")
        for gate_id in gates:
            entry = provided[gate_id]
            require(entry.get("result") == "passed" and entry.get("exit_code") == 0, "mandatory evidence is not passed")
            require(all(entry.get(field) for field in ("command", "cwd", "environment", "log", "criteria")), "incomplete gate evidence")
            require(set(gates[gate_id]["criteria"]) <= set(entry["criteria"]), "missing criterion evidence")
            require(entry["log"] in records, "missing evidence log")
            for field in ("candidate_sha", "base_sha", "plan_version", "attempt_id"):
                require(entry.get(field) == report[field], "evidence identity mismatch")
        discovered = report.get("discovered", [])
        executed = report.get("executed", [])
        require(discovered and executed and len(set(executed)) == len(executed) and set(executed) <= set(discovered), "zero/invalid discovered or executed tests")
        mapping = report.get("test_mapping", {})
        for case in plan["cases"]:
            if operation == "work_unit" and case.get("work_units") and report["work_unit"] not in case["work_units"]:
                continue
            item = mapping.get(case["id"], {})
            require(item.get("location") and item.get("tests") and set(item["tests"]) <= set(executed), "missing/skipped planned test mapping")
        require(not report.get("skipped"), "mandatory planned tests were skipped")

    @staticmethod
    def normalize_closeout(state):
        # Schema-2 adoption is pending only: never fabricate historical acceptance.
        if "closeout" not in state:
            state["closeout"] = {"stage": "pending"}

    @staticmethod
    def invalidate_closeout(state, reason):
        closeout = state.get("closeout", {"stage": "pending"})
        if closeout["stage"] in ("intent", "prepared", "cleanup"):
            state["closeout"] = dict(closeout, stage="cleanup", reason=reason, resume_phase=state["phase"])
            state["final_acceptance"] = None
            state["next_action"] = "journal superseding issue evidence, then closeout-cleanup"

    def closeout_action(self, state, action, p, journal):
        require(state['status'] == 'ready', 'closeout requires idle ready state')
        self.source(state)
        GitHub().unchanged(state['issue_snapshot'])
        c = state.get('closeout', {'stage':'pending'})
        r = state['repository']
        if action == 'closeout-intent':
            require(c['stage'] == 'pending' and state['work_units'] and all(unit['status']=='accepted' for unit in state['work_units']), 'accepted work units required')
            require(not any(f['status']!='resolved' for f in state['findings']), 'unresolved closeout findings')
            accepted_tuple={'candidate_sha':r['candidate_sha'],'base_sha':r['base_sha'],'plan_version':state['plan']['version']}
            if any(not unit['accepted'] or any(unit['accepted'].get(key)!=value for key,value in accepted_tuple.items()) for unit in state['work_units']):
                for unit in state['work_units']:unit.update(status='ready',accepted=None)
                state['renewing_work_units']=True
                state.update(phase='checking',operation='work_unit',active_work_unit=self.ready_unit(state)['key'],next_action='checker-only renewal of every work unit at combined candidate; then closeout-intent')
                return
            keys = {root_identity(state)} | {unit['issue'] for unit in state['work_units']}
            targets = p.get('targets', [])
            require(len(targets)==len(keys) and {target['issue'] for target in targets}==keys, 'exact root/work-unit evidence targets required')
            for target in targets:
                require(target.get('evidence') and target.get('reference') == 'https://github.com/'+r['target_repository']+'/compare/'+r['target_branch']+'...'+r['branch'], 'passing evidence and honest source compare required')
                issue = GitHub().issue(target['issue'])
                require(issue['state']=='open', 'PR readiness requires open task')
                # Uncovered required children prohibit epic closing references.
                required = set(issue['children']) - keys
                require(all((lambda child: child['state']=='closed' and child['state_reason']=='completed')(GitHub().issue(url)) for url in required), 'incomplete child prevents root completion')
            state['closeout'] = {'stage':'intent','candidate_sha':r['candidate_sha'],'base_sha':r['base_sha'],
                'plan_version':state['plan']['version'],'targets':targets,'closing_references':['Closes '+target['issue'] for target in targets], 'binding':'Candidate: `'+r['candidate_sha']+'`; base: `'+r['base_sha']+'`; plan: `'+str(state['plan']['version'])+'`; issue input: `'+state['issue_snapshot']['digest']+'`.', 'marker':'<!-- harness-ready:'+state['run_id']+':'+digest({'candidate':r['candidate_sha'],'base':r['base_sha'],'plan':state['plan']['version'],'input':state['issue_snapshot']['digest']})+' -->'}
            state.update(phase='closing',operation='closeout',next_action='journal concise readiness comments on exact issue targets; keep issues open; then closeout-record')
        elif action == 'closeout-record':
            require(c['stage']=='intent' and p.get('candidate_sha')==c['candidate_sha'], 'exact prepared candidate required')
            marker = c['marker']
            for target in c['targets']:
                comments = GitHub().comments(target['issue'])
                matches = [comment for comment in comments if marker in (comment.get('body') or '')]
                require(len(matches)==1 and target['evidence'] in matches[0]['body'] and target['reference'] in matches[0]['body'] and c['binding'] in matches[0]['body'], 'read back exact managed readiness evidence and candidate/base/plan/input binding')
            c['stage']='prepared'
            state.update(phase='checking',operation='final',next_action='dispatch separate fresh final checker at unchanged candidate')
        else:
            require(c['stage']=='cleanup' and p.get('observation'), 'stale issue evidence requires explicit invalidation observation')
            marker = c['marker']
            for target in c['targets']:
                comments = GitHub().comments(target['issue'])
                require(any(marker in (comment.get('body') or '') and 'superseded' in comment['body'].lower() for comment in comments), 'journal a marked superseding comment before cleanup')
            state['provenance'].append({'invalidated_closeout':c,'observation':p['observation']})
            state['closeout']={'stage':'pending'}
            state.update(phase='implementing',operation='repair',next_action='fresh implementation/checks after invalidating stale readiness')

    def reduce(self, state, action, p, journal, records):
        repository = state["repository"]
        expired = self.clock() >= state["limits"]["deadline"]
        # Reports and worker observations are facts worth preserving after expiry.
        # Forward work/publication pauses before its side effect or authorization.
        if action in ("dispatch", "command-intent", "observe-source", "publish-intent", "publish-observation", "reserve", "closeout-intent", "closeout-record", "closeout-cleanup"):
            if state["limits"]["exhausted"] or expired:
                self.pause_budget(state, "run deadline or recorded budget exhausted")
                return
        if action in ("report", "acknowledge", "reconcile", "resolve-finding") and state["operation"] == "budget":
            self.restore_budget_step(state)
        if action == "reconcile-input":
            require(state['status'] != 'running', 'reconcile live worker before issue input change')
            snapshot = GitHub().snapshot(root_identity(state), [unit['issue'] for unit in state['work_units']])
            require(p.get('observation') and p.get('snapshot') == snapshot, 'exact live snapshot and reconciliation observation required')
            require(GitHub.ready(snapshot), 'incomplete/cancelled prerequisite remains blocking')
            state['provenance'].append({'prior_issue_snapshot':state['issue_snapshot'],'reconciliation':p['observation']})
            state['issue_snapshot'] = snapshot
            state['final_acceptance'] = None
            self.invalidate_closeout(state, 'requirements or relationships changed')
            for unit in state['work_units']:unit.update(status='ready',accepted=None)
            state.update(phase='implementing' if state['plan'] else 'planning',operation='repair' if state['plan'] else 'plan',status='ready',next_action='renew plan/implementation and independent checks against reconciled input')
        elif action in ("closeout-intent", "closeout-record", "closeout-cleanup"):
            self.closeout_action(state, action, p, journal)
        elif action == "reserve":
            raise Invalid('FOS allocation retired; create persistent GitHub sub-issue using journaled external operations')
        elif action == "dispatch":
            self.dispatch(state, p)
        elif action == "acknowledge":
            attempt = self.current_attempt(state)
            require(attempt["status"] == "intent" and p["attempt_id"] == attempt["attempt_id"] and p.get("worker_id"), "wrong acknowledgement")
            attempt.update(worker_id=p["worker_id"], status="running")
        elif action == "reconcile":
            require(p.get("observation") and p.get("next_action"), "reconciliation requires concrete observation/action")
            if state["operation"] == "bootstrap-reconciliation":
                legacy = state["provenance"][0]["legacy_state"].get("attempts", [])
                require(set(p.get("terminated_workers", [])) >= {a["worker_id"] for a in legacy if a.get("worker_id")}, "all legacy workers must be definitively terminated")
                state["provenance"].append({"reconciliation": p})
                operation = "work_unit" if state["phase"] == "checking" else ("implement" if state["plan"] else "plan")
                state.update(status="ready", operation=operation, next_action=p["next_action"])
            else:
                attempt = self.current_attempt(state)
                require(p["attempt_id"] == attempt["attempt_id"], "wrong reconciled attempt")
                require(p["liveness"] in ("live", "unknown", "terminated"), "invalid liveness")
                attempt["observations"].append(p)
                if p["liveness"] == "terminated":
                    attempt["status"] = "terminated"
                    state.update(status="ready", next_action=p["next_action"])
                else:
                    attempt["status"] = "ambiguous"
                    state.update(status="awaiting_decision", next_action=p["next_action"])
        elif action == "report":
            self.report(state, p, records, journal)
        elif action == "resolve-finding":
            require(state["phase"] == "checking" and state["status"] == "running", "only fresh checker may recheck finding")
            require(p.get("evidence") and p.get("finding_id"), "missing resolution evidence")
            self.current_attempt(state)
            finding = next((f for f in state["findings"] if f["id"] == p["finding_id"]), None)
            require(finding is not None, "unknown finding")
            require(p.get("affected_task", finding.get("affected_task", root_identity(state))) ==
                    finding.get("affected_task", root_identity(state)), "cannot change finding scope")
            finding.update(status="resolved", recheck=p, attempt_id=state["attempts"][-1]["attempt_id"])
        elif action == "budget-extension":
            require(p.get("authorization") and p.get("reason"), "explicit user authorization required")
            require(p.get("deadline", state["limits"]["deadline"]) >= state["limits"]["deadline"], "extension cannot reduce budget")
            for field in ("deadline", "repairs", "replans"):
                if field in p:
                    require(p[field] >= state["limits"][field], "extension cannot reduce limit")
                    state["limits"][field] = p[field]
            require(self.clock() < state["limits"]["deadline"], "extension must provide an unexpired deadline")
            state["limits"]["extensions"].append(p)
            state["limits"]["exhausted"] = False
            if state["status"] == "awaiting_decision" and state["operation"] == "budget":
                self.restore_budget_step(state)
        elif action == "resume":
            require(state["status"] == "awaiting_decision" and not state["limits"]["exhausted"]
                    and state["operation"] != "bootstrap-reconciliation", "resume requires resolved non-budget decision")
            require(p.get("observation") and p.get("next_action"), "record resolved prerequisite and next action")
            require(not state["attempts"] or state["attempts"][-1]["status"] == "complete", "worker must be reconciled first")
            state["provenance"].append({"decision": p})
            state.update(status="ready", next_action=p["next_action"])
        elif action == "command-intent":
            require(p.get("command") and p.get("cwd") and 0 < p["timeout"] <= state["limits"]["command_timeout"], "invalid command deadline")
            state["limits"]["commands"].append(dict(p, started=self.clock()))
        elif action == "observe-source":
            require(state["status"] != "running", "cannot change source tuple during worker execution")
            candidate = normalize_sha(self.repo, p["candidate_sha"])
            self.source(state, candidate)
            base = normalize_sha(self.repo, p.get("base_sha", repository["base_sha"]))
            require(candidate != repository["candidate_sha"] or base != repository["base_sha"], "source tuple unchanged")
            self.invalidate_closeout(state, "source/base changed")
            repository.update(candidate_sha=candidate, base_sha=base)
            state["final_acceptance"] = None
            previous = state["publishing"]
            state["publishing"] = {"stage": "none", "ci_policy": previous["ci_policy"], "prior": previous}
            for unit in state["work_units"]:
                unit.update(status="ready", accepted=None)
            state.update(phase="implementing", status="ready", operation="repair", active_work_unit=self.ready_unit(state)["key"], next_action="implement integrated candidate, then fresh work_unit and final checks")
        elif action == "publish-intent":
            require(state["phase"] == "publishing" and state["final_acceptance"], "final acceptance required")
            self.publication_guard(state, p)
            require(state["publishing"]["stage"] == "none", "publication intent already exists")
            state["publishing"].update(stage="push", intent=dict(p, candidate_sha=repository["candidate_sha"], base_sha=repository["base_sha"], plan_version=state["plan"]["version"]))
            state["next_action"] = "push implementation without force, then record remote head"
        elif action == "publish-observation":
            self.publish(state, p)
        else:
            raise Invalid("unknown transition: " + action)
        if state.get("closeout", {}).get("stage") == "cleanup":
            state["next_action"] = "journal superseding issue evidence, then closeout-cleanup"
        if action != "budget-extension" and (expired or state["limits"]["exhausted"]):
            self.pause_budget(state, "run deadline exhausted")
        # Coordinator supplies bounded text records with each action; never paths on disk.
        for path, content in p.get("records", {}).items():
            record_path(path)
            prefix = f"runs/{state['task_key']}/{state['run_id']}/"
            require(path.startswith(prefix + "reports/") or path.startswith(prefix + "evidence/"), "cannot overwrite canonical state")
            require(path not in records, "records are append-only")
            records[path] = content.encode() if isinstance(content, str) else encoded(content)

    def dispatch(self, state, p):
        require(state["status"] == "ready", "worker is live/ambiguous or run needs decision")
        require(state.get("closeout", {}).get("stage") != "cleanup", "closeout cleanup required before dispatch")
        require(not state["attempts"] or state["attempts"][-1]["status"] in ("complete", "terminated"), "previous worker not terminated")
        require(TOKEN.fullmatch(p["attempt_id"]) and not any(a["attempt_id"] == p["attempt_id"] for a in state["attempts"]), "attempt ID reused")
        role = p["role"]
        require(role in ("planner", "implementer", "checker"), "invalid role")
        GitHub().unchanged(state["issue_snapshot"])
        expected_role = {"planning": "planner", "implementing": "implementer", "checking": "checker"}.get(state["phase"])
        require(role == expected_role, "role does not match phase")
        requested = p["requested"]
        actual = p["actual"]
        catalog = p["catalog"]
        require(actual["model"] in catalog and actual["effort"] in catalog[actual["model"]], "unsupported actual model/effort")
        require(requested == actual or p.get("fallback_reason"), "supported fallback must be explicit")
        limits = state["limits"]
        reason = None
        if limits["exhausted"] or self.clock() >= limits["deadline"]:
            reason = "run deadline exhausted"
        if role == "implementer":
            unit = next((u for u in state["work_units"] if u["key"] == state["active_work_unit"]), None)
            done = {u["key"] for u in state["work_units"] if u["status"] == "accepted"}
            require(unit is not None and unit["status"] != "accepted" and set(unit["dependencies"]) <= done,
                    "implementer unit dependencies are not accepted")
            count = limits["implementation_counts"].get(state["active_work_unit"], 0)
            if count >= 1 + limits["repairs"]:
                reason = "implementation repair budget exhausted"
        if role == "planner" and state["plan"] and limits["replan_count"] >= limits["replans"]:
            reason = "replan budget exhausted"
        if reason:
            self.pause_budget(state, reason)
            return
        if role == "implementer":
            limits["implementation_counts"][state["active_work_unit"]] = count + 1
        if role == "planner" and state["plan"]:
            limits["replan_count"] += 1
        repository = state["repository"]
        if role == "checker":
            require(state["operation"] in ("work_unit", "final"), "checker requires explicit work_unit/final operation")
            self.source(state)
        attempt = {"attempt_id": p["attempt_id"], "role": role, "worker_id": None,
                   "input_revision": state["revision"] - 1, "input_sha": repository["input_sha"] if role == "planner" else (repository["candidate_sha"] or repository["input_sha"]),
                   "candidate_sha": repository["candidate_sha"], "base_sha": repository["base_sha"],
                   "plan_version": state["plan"]["version"] if state["plan"] else 0,
                   "operation": state["operation"], "work_unit": state["active_work_unit"], "requested": requested,
                   "actual": actual, "fallback_reason": p.get("fallback_reason"), "started": self.clock(),
                   "status": "intent", "observations": []}
        state["attempts"].append(attempt)
        state.update(status="running", next_action="launch once, then acknowledge worker identity; reconcile if response is lost")

    def report(self, state, p, records, journal):
        original_report = copy.deepcopy(p)
        bridge = state.get('legacy_report_bridge')
        bridged = False
        if 'unit' in p:
            require(bridge and not bridge['consumed'] and all(p.get(key)==value for key,value in bridge['envelope'].items()), 'old report requires exact unconsumed migration bridge')
            migration = next(entry for entry in reversed(state['provenance']) if 'migration_from_tip' in entry)
            p = normalize_live(p, migration['mapping'])
            bridged = True
        attempt = self.current_attempt(state)
        require(attempt["status"] == "running", "report requires acknowledged live attempt")
        require(all(p.get(k) == value for k, value in self.tuple_fields(attempt).items()), "stale report identity tuple")
        role = attempt["role"]
        # Include new logs before validation; rejected changes never reach the ref.
        combined = dict(records)
        for path, value in p.get("records", {}).items():
            record_path(path)
            combined[path] = value.encode() if isinstance(value, str) else encoded(value)
        if role == "planner":
            plan = p["plan"]
            urls = {root_identity(state)} | {work["key"] for work in plan.get("work_units", [])}
            registry = {url:GitHub().issue(url) for url in urls}
            require(plan["version"] == (state["plan"]["version"] + 1 if state["plan"] else 1), "plan version not monotonic")
            validate_plan(plan, state["original"], root_identity(state), registry, journal)
            for work in plan.get('work_units', []):
                require(registry[work['key']]['parent'] == root_identity(state), 'split work unit requires native root parent')
                require(set(work['criteria']) <= {row['id'] for row in state['original']}, 'unknown split criterion')
                require(set(work['dependencies']) == set(registry[work['key']]['blocked_by']) & urls, 'work-unit dependencies must match native deliverable prerequisites')
            if state["plan"]:
                # Preserve validated permanent attributions even if the new plan
                # removes their units. Full prior plans remain in role reports.
                state["provenance"].append({"superseded_plan": {
                    "version": state["plan"]["version"], "work_units": state["plan"].get("work_units", [])}})
            self.invalidate_closeout(state, "plan changed")
            state["plan"] = plan
            state["work_units"] = self.units(state)
            snapshot = GitHub().snapshot(root_identity(state), [unit['issue'] for unit in state['work_units']])
            require(GitHub.ready(snapshot), 'external work-unit prerequisites incomplete/cancelled')
            planned_urls={unit['issue'] for unit in state['work_units']}
            for url, prior in state['issue_snapshot']['nodes'].items():
                if url not in snapshot['nodes']:continue
                current=snapshot['nodes'][url]
                prior=copy.deepcopy(prior)
                if url==root_identity(state):
                    require(set(prior['children']) <= set(current['children']) and set(current['children'])-set(prior['children']) <= planned_urls, 'unplanned hierarchy drift requires input reconciliation')
                    prior['children']=current['children']
                require(current==prior, 'material requirement/prerequisite change requires explicit input reconciliation')
            state['provenance'].append({'prior_issue_snapshot':state['issue_snapshot'], 'snapshot_reason':'validated plan deliverables and native relationships'})
            state['issue_snapshot'] = snapshot
            state["active_work_unit"] = self.ready_unit(state)["key"]
            state["final_acceptance"] = None
            state.update(phase="implementing", operation="implement", status="ready", next_action="dispatch fresh implementer for first ready unit")
        elif role == "implementer":
            candidate = normalize_sha(self.repo, p["result_candidate_sha"])
            self.source(state, candidate)
            require(p.get("test_mapping") and p.get("commands"), "implementation report needs test mapping and actual commands")
            repository = state["repository"]
            repository["candidate_sha"] = candidate
            state["final_acceptance"] = None
            state.update(phase="checking", operation="work_unit", status="ready", next_action="dispatch fresh independent checker at candidate")
        else:
            require(p["verdict"] in ("accepted", "changes_required", "unable_to_verify"), "unknown checker verdict")
            self.source(state)
            finding_ids = [finding.get("id") for finding in p.get("findings", [])]
            require(len(finding_ids) == len(set(finding_ids)), "duplicate finding in report")
            planned_keys = {unit["key"] for unit in state["plan"].get("work_units", [])}
            if p.get("findings"):
                registry = self.issue_registry(state)
            for finding in p.get("findings", []):
                require(finding.get("id") and finding.get("evidence") and finding.get("route") in ("implementation", "planner", "environment"), "incomplete finding")
                require(not any(f["id"] == finding["id"] for f in state["findings"]), "duplicate finding")
                affected = validate_finding_scope(finding, root_identity(state), planned_keys)
                require(affected in registry and registry[affected]["state_reason"] != "not_planned", "unknown issue finding scope")
            if p["verdict"] == "accepted":
                self.evidence(state, p, combined)
                state["verification"].append(p)
                accepted = {"candidate_sha": p["candidate_sha"], "base_sha": p["base_sha"], "plan_version": p["plan_version"], "attempt_id": p["attempt_id"]}
                if state["operation"] == "final":
                    require(all(unit["status"] == "accepted" for unit in state["work_units"]), "unaccepted unit")
                    require(state.get("closeout", {}).get("stage") == "prepared" and state["closeout"].get("candidate_sha") == p["candidate_sha"], "prepared closeout required")
                    state["final_acceptance"] = accepted
                    state.update(phase="publishing", operation="publish", status="ready", next_action="fetch target base, verify clean tuple, persist publication intent")
                else:
                    unit = next(unit for unit in state["work_units"] if unit["key"] == state["active_work_unit"])
                    unit.update(status="accepted", accepted=accepted)
                    remaining = [u for u in state["work_units"] if u["status"] != "accepted"]
                    if remaining:
                        ready = self.ready_unit(state)
                        state.update(active_work_unit=ready["key"], phase="checking" if (state.get("closeout", {}).get("stage") == "prepared" or state.get("renewing_work_units")) else "implementing", operation="work_unit" if (state.get("closeout", {}).get("stage") == "prepared" or state.get("renewing_work_units")) else "implement", status="ready", next_action="dispatch next dependency-ready role")
                    else:
                        if state.get("closeout", {}).get("stage") == "prepared":
                            state.update(operation="final", status="ready", next_action="dispatch separate fresh final checker over combined branch")
                        else:
                            state.pop("renewing_work_units", None)
                            state.update(phase="closing", operation="closeout", status="ready", next_action="persist closeout-intent before journaled issue evidence")
            else:
                require(p.get("findings") and p.get("next_action"), "rejection requires findings and next action")
                state["findings"].extend(p["findings"])
                signatures = [a.get("rejection_signature") for a in state["attempts"] if a.get("rejection_signature")]
                signature = hashlib.sha256(encoded({"verdict": p["verdict"], "work_unit": p["work_unit"],
                    "findings": [{"route": f["route"], "evidence": f["evidence"]} for f in p["findings"]]})).hexdigest()
                repeated = signature in signatures and not p.get("new_evidence")
                attempt["rejection_signature"] = signature
                routes = {f["route"] for f in p["findings"]}
                phase = "planning" if "planner" in routes else "implementing"
                state.update(phase=phase, operation="replan" if phase == "planning" else "repair", status="ready", next_action=p["next_action"])
                if p["verdict"] == "unable_to_verify" or "environment" in routes:
                    state.update(phase="checking", operation=attempt["operation"])
                    state.update(status="awaiting_decision", next_action=p["next_action"])
                elif attempt["operation"] == "final" and phase == "implementing":
                    # Integration findings can affect any unit. Recheck the whole
                    # dependency chain rather than retaining stale unit approvals.
                    for unit in state["work_units"]:
                        unit.update(status="ready", accepted=None)
                    state["final_acceptance"] = None
                    state["active_work_unit"] = self.ready_unit(state)["key"]
                self.invalidate_closeout(state, "checker rejected proposed completion")
                if repeated:
                    self.pause_budget(state, "repeated unchanged rejection")
        attempt["status"] = "complete"
        attempt["report"] = f"runs/{state['task_key']}/{state['run_id']}/reports/{attempt['attempt_id']}.json"
        require(attempt["report"] not in records, "report already exists")
        records[attempt["report"]] = encoded(original_report)
        if bridged:
            bridge['consumed'] = True

    def publication_guard(self, state, p):
        GitHub().unchanged(state["issue_snapshot"])
        require(not state["limits"]["exhausted"] and self.clock() < state["limits"]["deadline"], "publication budget exhausted")
        self.source(state)
        require(state.get("closeout", {}).get("stage") == "prepared" and state["closeout"].get("candidate_sha") == state["repository"]["candidate_sha"], "prepared closeout required")
        r = state["repository"]
        accepted = state["final_acceptance"]
        require(accepted and all(accepted[k] == v for k, v in {"candidate_sha": r["candidate_sha"], "base_sha": r["base_sha"], "plan_version": state["plan"]["version"]}.items()), "final acceptance tuple changed")
        require(all(u["status"] == "accepted" for u in state["work_units"]) and not any(f["status"] != "resolved" for f in state["findings"]), "unaccepted work/findings")
        for field in ("target_remote", "target_repository", "target_branch", "branch"):
            require(p.get(field) == r[field], "wrong publication destination/head/base")
        remote_base = git(self.repo, "ls-remote", r["target_remote"], "refs/heads/" + r["target_branch"]).decode().split()
        require(remote_base and remote_base[0] == r["base_sha"], "target base moved; integrate and recheck")
        if state["publishing"]["stage"] not in ("none", "push"):
            remote_head = git(self.repo, "ls-remote", r["target_remote"], "refs/heads/" + r["branch"]).decode().split()
            require(remote_head and remote_head[0] == r["candidate_sha"], "remote head moved; never force push")

    def publish(self, state, p):
        require(state["phase"] == "publishing" and state["publishing"].get("intent"), "persist publication intent first")
        publishing = state["publishing"]
        self.publication_guard(state, publishing["intent"])
        stage = publishing["stage"]
        event = p["event"]
        if event == "push":
            require(stage == "push", "push not expected")
            r = state["repository"]
            remote = git(self.repo, "ls-remote", r["target_remote"], "refs/heads/" + r["branch"]).decode().split()
            require(remote and remote[0] == r["candidate_sha"] and p["remote_sha"] == remote[0], "remote push conflict or wrong SHA; never force push")
            publishing.update(stage="lookup", remote_sha=remote[0])
        elif event == "lookup":
            require(stage in ("lookup", "create", "issue-evidence", "attach"), "lookup not expected")
            require(p.get("all_states") is True and p.get("unambiguous") is True, "ambiguous PR lookup; lookup again")
            matches = p["matches"]
            require(len(matches) <= 1, "multiple matching PRs need decision")
            if matches:
                pr = matches[0]
                for field in ("target_repository", "target_branch", "branch"):
                    require(pr.get(field) == publishing["intent"][field], "PR destination/head/base mismatch")
                require(pr.get("state") == "open" and pr.get("draft") is False and pr.get("url"), "closed/merged/draft PR requires decision")
                actual = GitHub().call(['pr','view',pr['url'],'--repo','robinwassen/Fossilize','--json','url,state,isDraft,baseRefName,headRefName,headRefOid,body,closingIssuesReferences'])
                r = state['repository']
                require(actual['url']==pr['url'] and actual['state']=='OPEN' and actual['isDraft'] is False and actual['baseRefName']==r['target_branch'] and actual['headRefName']==r['branch'] and actual['headRefOid']==r['candidate_sha'], 'actual PR identity/head/base differs from accepted candidate')
                closing = {issue['url'] for issue in actual['closingIssuesReferences']}
                expected = {target['issue'] for target in state['closeout']['targets']}
                require(closing == expected and all(line in actual['body'] for line in state['closeout']['closing_references']), 'implementation PR must close exactly fully verified task targets')
                publishing.update(stage="issue-evidence", pr=pr)
                state['next_action']='journal actual implementation PR URL on each closeout issue, then publish-observation event=issue-evidence'
            else:
                require(stage not in ("issue-evidence", "attach"), "recorded PR disappeared")
                publishing["stage"] = "create"
        elif event == "create-dispatched":
            require(stage == "create", "creation requires exact empty lookup")
            require(not publishing.get("creation_intent"), "creation already dispatched; reconcile existing PR or require a decision")
            publishing["stage"] = "lookup"
            publishing["creation_intent"] = dict(p, at=self.clock())
            state["next_action"] = "create once; always exact all-state lookup to reconcile response or failure"
        elif event == 'issue-evidence':
            require(stage == 'issue-evidence', 'record matching PR before issue evidence')
            marker = '<!-- harness-pr:'+state['run_id']+':'+state['repository']['candidate_sha']+' -->'
            for target in state['closeout']['targets']:
                comments = GitHub().comments(target['issue'])
                matches = [comment for comment in comments if marker in (comment.get('body') or '')]
                require(len(matches)==1 and publishing['pr']['url'] in matches[0]['body'] and state['repository']['candidate_sha'] in matches[0]['body'], 'read back one managed actual-PR evidence comment per task')
            publishing.update(stage='attach',issue_evidence={'marker':marker,'pr':publishing['pr']['url']})
        elif event == "attachment":
            require(stage == "attach" and p.get("url") == publishing["pr"]["url"], "attachment requires recorded matching PR")
            publishing["attachment"] = p
            if p.get("success"):
                if publishing["ci_policy"] == "required_after_pr":
                    publishing["stage"] = "ci_pending"
                    state.update(status="awaiting_decision", next_action="record required hosted CI result")
                else:
                    publishing["stage"] = "done"
                    state.update(phase="complete", operation="complete", status="complete", next_action="PR opened and attached; no merge")
            else:
                state["next_action"] = "retry attachment only"
        elif event == "ci":
            require(stage == "ci_pending" and p.get("result") == "passed" and p.get("evidence"), "required CI not passed")
            publishing.update(stage="done", ci=p)
            state.update(phase="complete", operation="complete", status="complete", next_action="PR opened, attached and required CI passed; no merge")
        else:
            raise Invalid("unknown publication event")
        publishing.setdefault("observations", []).append(p)


def detached_worktree(repo, path, sha):
    """Create only a new detached view; never reset/clean/reuse an existing directory."""
    require(not Path(path).exists(), "worktree destination already exists")
    sha = normalize_sha(repo, sha)
    git(repo, "worktree", "add", "--detach", str(Path(path).resolve()), sha)
    return sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("show")
    worktree = sub.add_parser("detached-worktree")
    worktree.add_argument("path")
    worktree.add_argument("sha")
    for action in ("records-only", "migrate-state", "rebind-execution", "reconcile-input", "publish-artifacts", "adopt-artifacts", "initialize", "import-bootstrap", "dispatch", "acknowledge", "reconcile", "report", "resolve-finding", "budget-extension", "resume", "command-intent", "observe-source", "publish-intent", "publish-observation", "closeout-intent", "closeout-record", "closeout-cleanup"):
        command = sub.add_parser(action)
        command.add_argument("--payload", required=True, help="JSON file; use - for stdin")
        command.add_argument("--expected-tip", required=True, help="full artifact commit or none")
        command.add_argument("--expected-revision", required=True, type=int)
    args = parser.parse_args()
    try:
        if args.action == "show":
            result = Harness(args.repo).show()
        elif args.action == "detached-worktree":
            with repository_lock(args.repo):
                result = {"sha": detached_worktree(args.repo, args.path, args.sha)}
        else:
            import sys
            payload = json.load(sys.stdin) if args.payload == "-" else json.loads(Path(args.payload).read_text())
            result = Harness(args.repo).apply(args.action, payload, None if args.expected_tip == "none" else args.expected_tip, args.expected_revision)
        print(encoded(result).decode(), end="")
        if result.get("artifact_publication", {}).get("status") == "incomplete" and args.action != "show":
            parser.exit(3)
    except (Invalid, IssueError, KeyError, TypeError, json.JSONDecodeError, OSError) as error:
        parser.exit(2, f"harness: {error}\n")


if __name__ == "__main__":
    main()
