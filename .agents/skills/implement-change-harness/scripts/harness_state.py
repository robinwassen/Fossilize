#!/usr/bin/env python3
"""Local change-harness records. No agent runtime, command runner, or GitHub client."""
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

SCHEMA = 2
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
    require(KEY.fullmatch(key) and TOKEN.fullmatch(run), "invalid root/run identity")
    return f"runs/{key}/{run}/state.json"


def record_path(path):
    parts = PurePosixPath(path).parts
    require(not path.startswith("/") and ".." not in parts and str(PurePosixPath(path)) == path,
            "unsafe artifact path")
    if path in ("active.json", "allocations.json"):
        return
    require(len(parts) >= 4 and parts[0] == "runs" and KEY.fullmatch(parts[1])
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

    def tip(self):
        result = subprocess.run(["git", "-C", str(self.repo), "rev-parse", "--verify", REF],
                                capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else None

    def records(self, tip):
        if tip is None:
            return {}
        # Every ancestor must be an orphan records tree; never adopt source ancestry.
        for commit in git(self.repo, "rev-list", tip).decode().splitlines():
            if commit in self.validated_commits:
                continue  # Git objects are immutable; refs and active blobs are reread.
            listing = git(self.repo, "ls-tree", "-rz", commit)
            for entry in listing.split(b"\0"):
                if not entry:
                    continue
                meta, raw_path = entry.split(b"\t", 1)
                mode, kind, blob = meta.decode().split()
                record_path(raw_path.decode())
                require(mode == "100644" and kind == "blob", "non-regular artifact record")
                content = git(self.repo, "cat-file", "blob", blob)
                self.validate_content(content)
            self.validated_commits.add(commit)
        records = {}
        for entry in git(self.repo, "ls-tree", "-rz", tip).split(b"\0"):
            if entry:
                meta, path = entry.split(b"\t", 1)
                records[path.decode()] = git(self.repo, "cat-file", "blob", meta.decode().split()[2])
        require("active.json" in records and "allocations.json" in records,
                "existing artifact ref lacks identity records")
        return records

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
            for path, content in sorted(records.items()):
                fault("write")
                blob = git(self.repo, "hash-object", "-w", "--stdin", data=content).decode().strip()
                git(self.repo, "update-index", "--add", "--cacheinfo", "100644", blob, path, env=env)
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


def canonical_inventory(repo, journal, revision="HEAD"):
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
        require(gate["kind"] in ("subtask", "final", "both", "publication", "optional"), "invalid gate kind")
        require(all(gate.get(field) for field in ("criteria", "method", "cwd", "prerequisites", "expected")),
                "incomplete gate design")
        require(set(gate["criteria"]) <= criteria, "unknown gate criterion")
        if gate["kind"] != "optional":
            covered.update(gate["criteria"])
    require(covered == criteria, "uncovered acceptance criterion")
    case_ids = [case["id"] for case in plan["cases"]]
    require(len(case_ids) == len(set(case_ids)) and all(case.get("assertions") and case.get("location")
                                                     for case in plan["cases"]), "incomplete test design")
    tasks = plan.get("subtasks", [])
    keys = [task["key"] for task in tasks]
    require(root not in keys and len(keys) == len(set(keys)), "duplicate root/subtask definition")
    allocations = {row["key"]: row for row in journal["issued"]}
    for task in tasks:
        require(task["key"] in registry and "retired" not in registry[task["key"]]["status"],
                "subtask absent or retired registration")
        require(task["key"] in allocations and allocations[task["key"]].get("title") == task["title"],
                "subtask must match durable reservation")
        require(task.get("criteria") and task.get("definition") and set(task["dependencies"]) <= set(keys),
                "invalid dependency or definition")
    done = set()
    while len(done) < len(keys):
        ready = {task["key"] for task in tasks if set(task["dependencies"]) <= done} - done
        require(ready, "cyclic dependencies")
        done.update(ready)


def validate_state(state, repo):
    require(state.get("schema_version") == SCHEMA, "unsupported/incomplete schema; use explicit import-bootstrap")
    required = ("revision", "task_key", "run_id", "task_location", "original", "repository", "phase",
                "operation", "status", "next_action", "plan", "units", "active_unit", "attempts", "verification",
                "findings", "limits", "publishing", "final_acceptance", "provenance")
    require(all(field in state for field in required), "missing state fields")
    state_path(state["task_key"], state["run_id"])
    require(isinstance(state["revision"], int) and state["revision"] > 0 and state["original"], "invalid state revision/original")
    original = state["original"]
    require(isinstance(original, list) and all(isinstance(c, dict) and c.get("id") and c.get("requirement") for c in original)
            and len({c["id"] for c in original}) == len(original), "invalid original acceptance")
    require(state["phase"] in ("initializing", "planning", "implementing", "checking", "publishing", "complete"), "invalid phase")
    require(state["status"] in ("ready", "running", "interrupted", "awaiting_decision", "failed", "complete"), "invalid status")
    finding_ids = [finding["id"] for finding in state["findings"]]
    require(len(finding_ids) == len(set(finding_ids)), "duplicate finding in durable state")
    repository = state["repository"]
    require(repository["common_dir"] == str(common_dir(repo)), "wrong repository identity")
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
            "plan": None, "units": [], "active_unit": payload["task_key"], "attempts": [], "verification": [],
            "findings": [], "limits": limits, "publishing": {"stage": "none", "ci_policy": payload.get("ci_policy", "local")},
            "final_acceptance": None, "provenance": []}


class Harness:
    def __init__(self, repo, clock=time.time, fail=None):
        self.repo = Path(repo).resolve()
        self.store = Store(repo, fail)
        self.clock = clock

    def show(self):
        tip = self.store.tip()
        records = self.store.records(tip)
        active = json.loads(records.get("active.json", b"null"))
        state = json.loads(records[active["state"]]) if active else None
        self.validate_run_records(records, active)
        if state:
            validate_state(state, self.repo)
            self.validate_pointer(active, state)
        return {"tip": tip, "active": active, "state": state}

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
        with repository_lock(self.repo):
            tip = self.store.tip()
            require(tip == expected_tip, "artifact tip changed")
            records = self.store.records(tip)
            active = json.loads(records.get("active.json", b"null"))
            journal = json.loads(records.get("allocations.json", b'{"high_water":0,"issued":[]}'))
            state = json.loads(records[active["state"]]) if active else None
            if action == "import-bootstrap":
                require(state is not None and state.get("schema_version") == 1 and state.get("bootstrap"), "not an explicit bootstrap")
                require(state["revision"] == expected_revision, "wrong expected revision")
                return self.import_bootstrap(state, payload, records, journal, tip)
            if state:
                validate_state(state, self.repo)
                self.validate_pointer(active, state)
            self.validate_run_records(records, active)
            if action == "initialize":
                if state:
                    require((state["task_key"], state["run_id"]) == (payload["task_key"], payload["run_id"]), "another run is active")
                    require(state["revision"] == expected_revision, "wrong expected revision")
                    return {"tip": tip, "state": state, "resumed": True}
                require(expected_revision == 0, "initial revision must be zero")
                path = state_path(payload["task_key"], payload["run_id"])
                require(path not in records, "run identity already used")
                state = initial_state(self.repo, payload, self.clock())
                registry, definitions, _ = canonical_inventory(self.repo, journal)
                require(state["task_key"] in registry and state["task_key"] in definitions and
                        "retired" not in registry[state["task_key"]]["status"], "root must have an active canonical registration")
                require(state["task_location"].split("#")[0] == definitions[state["task_key"]]["location"], "wrong root canonical location")
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
            return {"tip": new_tip, "state": state}

    def import_bootstrap(self, old, payload, records, journal, tip):
        require(old["task_key"] == payload["task_key"] and old["run_id"] == payload["run_id"], "bootstrap identity conflict")
        require(old["repository"]["branch"] == payload["repository"]["branch"], "bootstrap branch conflict")
        require(old["task_location"] == payload["task_location"], "bootstrap task location conflict")
        require(old.get("plan", {}).get("acceptance_criteria") == payload["original"], "bootstrap original acceptance conflict")
        for field in ("input_sha", "base_sha"):
            require(normalize_sha(self.repo, old["repository"][field]) == normalize_sha(self.repo, payload["repository"][field]), "bootstrap SHA conflict")
        # Oldest commit containing this run is the start, never migration time.
        path = state_path(old["task_key"], old["run_id"])
        commits = git(self.repo, "log", "--format=%ct", "--reverse", tip, "--", path).decode().splitlines()
        started = int(commits[0])
        normalized = copy.deepcopy(payload)
        normalized.setdefault("limits", {})["deadline"] = started + old["limits"].get("run_timeout_hours", 4) * 3600
        normalized["limits"]["repairs"] = old["limits"].get("repair_rounds_per_subtask", 3)
        normalized["limits"]["replans"] = old["limits"].get("replans", 1)
        # Permit an already elapsed deadline: it must remain exhausted after import.
        state = initial_state(self.repo, normalized, min(self.clock(), normalized["limits"]["deadline"] - 1))
        state["revision"] = old["revision"] + 1
        state["provenance"] = [{"imported_from_tip": tip, "legacy_schema": 1, "legacy_state": old,
                                "started_at": started, "uncertainty": "legacy attempts are facts only; reconcile before redispatch"}]
        registry, _, _ = canonical_inventory(self.repo, journal)
        for row in journal["issued"]:
            if row["key"] in registry and row.get("location") == "local-docs/backlog-keys.md":
                row["location"] = registry[row["key"]]["location"]
                row["title"] = registry[row["key"]]["title"]
        if payload.get("plan"):
            require(old.get("plan") is not None, "cannot invent a legacy plan")
            require(payload["plan"]["version"] == old["plan"]["plan_version"], "bootstrap plan version conflict")
            state["plan"] = payload["plan"]
            validate_plan(state["plan"], state["original"], state["task_key"], registry, journal)
            state["units"] = self.units(state)
            state["active_unit"] = self.ready_unit(state)["key"]
        state["phase"] = "implementing" if state["plan"] else "planning"
        # Completed legacy implementation may be imported as an unverified fact.
        # It is never rewritten into a fabricated fenced attempt or acceptance.
        report_path = payload.get("implementation_report")
        if report_path:
            require(state["plan"] is not None and report_path in records, "missing legacy implementation report/plan")
            record_path(report_path)
            require(report_path.startswith(path.removesuffix("state.json") + "reports/"), "wrong legacy report run")
            report = json.loads(records[report_path])
            require(report.get("report_type") == "implementer" and report.get("plan_version") == state["plan"]["version"], "legacy implementation report identity conflict")
            known = [a for a in old.get("attempts", []) if a.get("attempt_id") == report.get("attempt_id") and a.get("role") == "implementer"]
            require(len(known) == 1 and normalize_sha(self.repo, known[0]["input_sha"]) == normalize_sha(self.repo, report["input_sha"]), "unknown legacy implementation attempt")
            candidate = normalize_sha(self.repo, report["candidate_sha"])
            self.source(state, candidate)
            state["repository"]["candidate_sha"] = candidate
            state["phase"] = "checking"
            state["provenance"].append({"legacy_implementation_report": report_path, "candidate_sha": candidate,
                                        "acceptance": "unverified; fresh independent checks required"})
            state["limits"]["implementation_counts"][state["active_unit"]] = len([a for a in old["attempts"] if a.get("role") == "implementer"])
        state["status"] = "awaiting_decision"
        state["operation"] = "bootstrap-reconciliation"
        state["next_action"] = "reconcile every legacy worker; import its report as a fact, then dispatch fresh fenced attempts"
        state["limits"]["exhausted"] = self.clock() >= state["limits"]["deadline"]
        validate_state(state, self.repo)
        records[path] = encoded(state)
        records["allocations.json"] = encoded(journal)
        new_tip = self.store.checkpoint(records, tip, f"{old['task_key']} explicit bootstrap import")
        return {"tip": new_tip, "state": state}

    @staticmethod
    def units(state):
        tasks = state["plan"].get("subtasks") or [{"key": state["task_key"], "dependencies": []}]
        return [dict(task, status="ready", accepted=None) for task in tasks]

    @staticmethod
    def ready_unit(state):
        done = {unit["key"] for unit in state["units"] if unit["status"] == "accepted"}
        ready = next((unit for unit in state["units"] if unit["status"] != "accepted"
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
        return {field: attempt[field] for field in ("role", "attempt_id", "worker_id", "input_revision", "input_sha", "candidate_sha", "base_sha", "plan_version", "operation", "unit")}

    def evidence(self, state, report, records):
        operation = report["operation"]
        plan = state["plan"]
        gates = {gate["id"]: gate for gate in plan["gates"] if gate["kind"] in (operation, "both")}
        provided = {entry["gate"]: entry for entry in report.get("evidence", [])}
        require(set(gates) <= set(provided), "missing mandatory gate")
        require(not any(finding.get("status") != "resolved" for finding in report.get("findings", [])), "unresolved finding")
        require(not any(finding.get("status") != "resolved" for finding in state["findings"]), "unresolved prior finding")
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
            if operation == "subtask" and case.get("units") and report["unit"] not in case["units"]:
                continue
            item = mapping.get(case["id"], {})
            require(item.get("location") and item.get("tests") and set(item["tests"]) <= set(executed), "missing/skipped planned test mapping")
        require(not report.get("skipped"), "mandatory planned tests were skipped")

    def reduce(self, state, action, p, journal, records):
        repository = state["repository"]
        expired = self.clock() >= state["limits"]["deadline"]
        # Reports and worker observations are facts worth preserving after expiry.
        # Forward work/publication pauses before its side effect or authorization.
        if action in ("dispatch", "command-intent", "observe-source", "publish-intent", "publish-observation", "reserve"):
            if state["limits"]["exhausted"] or expired:
                self.pause_budget(state, "run deadline or recorded budget exhausted")
                return
        if action in ("report", "acknowledge", "reconcile", "resolve-finding") and state["operation"] == "budget":
            self.restore_budget_step(state)
        if action == "reserve":
            require(state["status"] != "running", "cannot allocate during worker execution")
            registry, _, high = canonical_inventory(self.repo, journal)
            key = "FOS-" + str(high + 1).zfill(6)
            require(p.get("title") and p.get("location"), "reservation needs title/location")
            journal["issued"].append(dict(p, key=key, reserved_at=self.clock()))
            journal["high_water"] = high + 1
            state["next_action"] = f"register reserved {key} before defining it"
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
                operation = "subtask" if state["phase"] == "checking" else ("implement" if state["plan"] else "plan")
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
            repository.update(candidate_sha=candidate, base_sha=base)
            state["final_acceptance"] = None
            previous = state["publishing"]
            state["publishing"] = {"stage": "none", "ci_policy": previous["ci_policy"], "prior": previous}
            for unit in state["units"]:
                unit.update(status="ready", accepted=None)
            state.update(phase="implementing", status="ready", operation="repair", active_unit=self.ready_unit(state)["key"], next_action="implement integrated candidate, then fresh subtask and final checks")
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
        require(not state["attempts"] or state["attempts"][-1]["status"] in ("complete", "terminated"), "previous worker not terminated")
        require(TOKEN.fullmatch(p["attempt_id"]) and not any(a["attempt_id"] == p["attempt_id"] for a in state["attempts"]), "attempt ID reused")
        role = p["role"]
        require(role in ("planner", "implementer", "checker"), "invalid role")
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
            unit = next((u for u in state["units"] if u["key"] == state["active_unit"]), None)
            done = {u["key"] for u in state["units"] if u["status"] == "accepted"}
            require(unit is not None and unit["status"] != "accepted" and set(unit["dependencies"]) <= done,
                    "implementer unit dependencies are not accepted")
            count = limits["implementation_counts"].get(state["active_unit"], 0)
            if count >= 1 + limits["repairs"]:
                reason = "implementation repair budget exhausted"
        if role == "planner" and state["plan"] and limits["replan_count"] >= limits["replans"]:
            reason = "replan budget exhausted"
        if reason:
            self.pause_budget(state, reason)
            return
        if role == "implementer":
            limits["implementation_counts"][state["active_unit"]] = count + 1
        if role == "planner" and state["plan"]:
            limits["replan_count"] += 1
        repository = state["repository"]
        if role == "checker":
            require(state["operation"] in ("subtask", "final"), "checker requires explicit subtask/final operation")
            self.source(state)
        attempt = {"attempt_id": p["attempt_id"], "role": role, "worker_id": None,
                   "input_revision": state["revision"] - 1, "input_sha": repository["input_sha"] if role == "planner" else (repository["candidate_sha"] or repository["input_sha"]),
                   "candidate_sha": repository["candidate_sha"], "base_sha": repository["base_sha"],
                   "plan_version": state["plan"]["version"] if state["plan"] else 0,
                   "operation": state["operation"], "unit": state["active_unit"], "requested": requested,
                   "actual": actual, "fallback_reason": p.get("fallback_reason"), "started": self.clock(),
                   "status": "intent", "observations": []}
        state["attempts"].append(attempt)
        state.update(status="running", next_action="launch once, then acknowledge worker identity; reconcile if response is lost")

    def report(self, state, p, records, journal):
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
            registry, _, _ = canonical_inventory(self.repo, journal)
            plan = p["plan"]
            require(plan["version"] == (state["plan"]["version"] + 1 if state["plan"] else 1), "plan version not monotonic")
            validate_plan(plan, state["original"], state["task_key"], registry, journal)
            state["plan"] = plan
            state["units"] = self.units(state)
            state["active_unit"] = self.ready_unit(state)["key"]
            state["final_acceptance"] = None
            state.update(phase="implementing", operation="implement", status="ready", next_action="dispatch fresh implementer for first ready unit")
        elif role == "implementer":
            candidate = normalize_sha(self.repo, p["result_candidate_sha"])
            self.source(state, candidate)
            require(p.get("test_mapping") and p.get("commands"), "implementation report needs test mapping and actual commands")
            repository = state["repository"]
            repository["candidate_sha"] = candidate
            state["final_acceptance"] = None
            state.update(phase="checking", operation="subtask", status="ready", next_action="dispatch fresh independent checker at candidate")
        else:
            require(p["verdict"] in ("accepted", "changes_required", "unable_to_verify"), "unknown checker verdict")
            self.source(state)
            finding_ids = [finding.get("id") for finding in p.get("findings", [])]
            require(len(finding_ids) == len(set(finding_ids)), "duplicate finding in report")
            for finding in p.get("findings", []):
                require(finding.get("id") and finding.get("evidence") and finding.get("route") in ("implementation", "planner", "environment"), "incomplete finding")
                require(not any(f["id"] == finding["id"] for f in state["findings"]), "duplicate finding")
            if p["verdict"] == "accepted":
                self.evidence(state, p, combined)
                state["verification"].append(p)
                accepted = {"candidate_sha": p["candidate_sha"], "base_sha": p["base_sha"], "plan_version": p["plan_version"], "attempt_id": p["attempt_id"]}
                if state["operation"] == "final":
                    require(all(unit["status"] == "accepted" for unit in state["units"]), "unaccepted unit")
                    state["final_acceptance"] = accepted
                    state.update(phase="publishing", operation="publish", status="ready", next_action="fetch target base, verify clean tuple, persist publication intent")
                else:
                    unit = next(unit for unit in state["units"] if unit["key"] == state["active_unit"])
                    unit.update(status="accepted", accepted=accepted)
                    remaining = [u for u in state["units"] if u["status"] != "accepted"]
                    if remaining:
                        ready = self.ready_unit(state)
                        state.update(active_unit=ready["key"], phase="implementing", operation="implement", status="ready", next_action="dispatch next dependency-ready implementer")
                    else:
                        state.update(operation="final", status="ready", next_action="dispatch separate fresh final checker over combined branch")
            else:
                require(p.get("findings") and p.get("next_action"), "rejection requires findings and next action")
                state["findings"].extend(p["findings"])
                signatures = [a.get("rejection_signature") for a in state["attempts"] if a.get("rejection_signature")]
                signature = hashlib.sha256(encoded({"verdict": p["verdict"], "unit": p["unit"],
                    "findings": [{"route": f["route"], "evidence": f["evidence"]} for f in p["findings"]]})).hexdigest()
                repeated = signature in signatures and not p.get("new_evidence")
                attempt["rejection_signature"] = signature
                routes = {f["route"] for f in p["findings"]}
                phase = "planning" if "planner" in routes else "implementing"
                state.update(phase=phase, operation="replan" if phase == "planning" else "repair", status="ready", next_action=p["next_action"])
                if p["verdict"] == "unable_to_verify" or "environment" in routes:
                    state.update(phase="checking", operation=attempt["operation"])
                    state.update(status="awaiting_decision", next_action=p["next_action"])
                if repeated:
                    self.pause_budget(state, "repeated unchanged rejection")
        attempt["status"] = "complete"
        attempt["report"] = f"runs/{state['task_key']}/{state['run_id']}/reports/{attempt['attempt_id']}.json"
        require(attempt["report"] not in records, "report already exists")
        records[attempt["report"]] = encoded(p)

    def publication_guard(self, state, p):
        require(not state["limits"]["exhausted"] and self.clock() < state["limits"]["deadline"], "publication budget exhausted")
        self.source(state)
        r = state["repository"]
        accepted = state["final_acceptance"]
        require(accepted and all(accepted[k] == v for k, v in {"candidate_sha": r["candidate_sha"], "base_sha": r["base_sha"], "plan_version": state["plan"]["version"]}.items()), "final acceptance tuple changed")
        require(all(u["status"] == "accepted" for u in state["units"]) and not any(f["status"] != "resolved" for f in state["findings"]), "unaccepted work/findings")
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
            require(stage in ("lookup", "create", "attach"), "lookup not expected")
            require(p.get("all_states") is True and p.get("unambiguous") is True, "ambiguous PR lookup; lookup again")
            matches = p["matches"]
            require(len(matches) <= 1, "multiple matching PRs need decision")
            if matches:
                pr = matches[0]
                for field in ("target_repository", "target_branch", "branch"):
                    require(pr.get(field) == publishing["intent"][field], "PR destination/head/base mismatch")
                require(pr.get("state") == "open" and pr.get("draft") is False and pr.get("url"), "closed/merged/draft PR requires decision")
                publishing.update(stage="attach", pr=pr)
            else:
                require(stage != "attach", "recorded PR disappeared")
                publishing["stage"] = "create"
        elif event == "create-dispatched":
            require(stage == "create", "creation requires exact empty lookup")
            publishing["stage"] = "lookup"
            publishing["creation_intent"] = dict(p, at=self.clock())
            state["next_action"] = "create once; always exact all-state lookup to reconcile response or failure"
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
    for action in ("initialize", "import-bootstrap", "reserve", "dispatch", "acknowledge", "reconcile", "report", "resolve-finding", "budget-extension", "resume", "command-intent", "observe-source", "publish-intent", "publish-observation"):
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
    except (Invalid, KeyError, TypeError, json.JSONDecodeError, OSError) as error:
        parser.exit(2, f"harness: {error}\n")


if __name__ == "__main__":
    main()
