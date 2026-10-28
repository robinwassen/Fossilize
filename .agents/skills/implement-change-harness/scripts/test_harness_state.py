"""Historical schema2 compatibility T01–T17 fixtures; live issue equivalents are separate."""
import copy
import json
import multiprocessing
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from legacy_state import (Harness as HistoricalHarness, Invalid, REF, Store, canonical_inventory, common_dir,
                           detached_worktree, encoded, git, repository_lock, state_path, validate_state)


class Harness(HistoricalHarness):
    """Only disposable compatibility fixtures opt into the historical API."""
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs, fixture_only=True)


def lock_contender(repo, ready, proceed, output):
    # Distinct process, same common-dir inode; events establish contention order.
    with repository_lock(repo):
        ready.set()
        proceed.wait(15)
        output.put("released")


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="harness-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "original"
        self.repo.mkdir()
        git(self.repo, "init", "-b", "master")
        git(self.repo, "config", "user.name", "Harness Test")
        git(self.repo, "config", "user.email", "harness@example.invalid")
        (self.repo / "local-docs").mkdir()
        self.root_key = "FOS-000025"
        self.registry_rows = [f"| `{self.root_key}` | Root task | [task](task.md) | Allocated |"]
        self.write_registry(self.repo)
        (self.repo / "local-docs/task.md").write_text("# FOS-000025 — Root task\n\n**Status:** Pending implementation.\nReferences to FOS-000025 are not definitions.\n")
        (self.repo / "behavior.py").write_text("def value():\n    return 1\n")
        (self.repo / "test_behavior.py").write_text("import unittest\nfrom behavior import value\nclass Behavior(unittest.TestCase):\n    def test_value(self):\n        self.assertEqual(value(), 1)\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-m", "input")
        self.base = git(self.repo, "rev-parse", "HEAD").decode().strip()
        self.bare = self.root / "origin.git"
        git(self.repo, "init", "--bare", str(self.bare))
        git(self.repo, "remote", "add", "origin", str(self.bare))
        git(self.repo, "push", "origin", "master")
        self.source = self.root / "source"
        git(self.repo, "worktree", "add", "-b", "codex/test", str(self.source), self.base)
        self.now = 1000
        self.harness = Harness(self.source, clock=lambda: self.now)
        self.original = [{"id": "AC1", "requirement": "observable behavior and durable records"}]
        self.payload = {"task_key": self.root_key, "run_id": "run-1", "task_location": "local-docs/task.md",
                        "original": self.original, "repository": {"input_sha": self.base, "base_sha": self.base,
                        "target_remote": "origin", "target_repository": "example/project", "target_branch": "master",
                        "branch": "codex/test", "implementation_worktree": str(self.source)}}
        self.tip = None
        self.state = None
        self.counter = 0

    def write_registry(self, repo):
        (repo / "local-docs/backlog-keys.md").write_text("# Registry\n\n| Key | Item | Canonical location | Allocation state |\n|---|---|---|---|\n" + "\n".join(self.registry_rows) + "\n")

    def act(self, action, payload=None):
        result = self.harness.apply(action, payload or {}, self.tip, self.state["revision"] if self.state else 0)
        self.tip, self.state = result["tip"], result["state"]
        return result

    def initialize(self):
        return self.act("initialize", self.payload)

    def plan(self):
        return {"version": 1, "original": self.original, "scope": "one behavior", "non_goals": ["unrelated"],
                "implementation": ["implement and test behavior"],
                "cases": [{"id": "C1", "assertions": "value equals one", "location": "test_behavior.py"}],
                "gates": [{"id": "test", "kind": "both", "criteria": ["AC1"], "method": "python unittest",
                           "cwd": str(self.source), "prerequisites": "Python and Git", "expected": "one assertion passes"}],
                "subtasks": []}

    def cleanup(self):
        for target in self.state["closeout"]["targets"]:
            path = self.source / target["location"]
            path.write_text("".join(line for line in path.read_text().replace("**Status:** Done (verified proposed branch state; not landed on master).", "**Status:** Pending implementation.").splitlines(keepends=True) if not line.startswith("**Completion evidence:**")))
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "remove invalid completion")
        candidate = git(self.source, "rev-parse", "HEAD").decode().strip()
        self.act("closeout-cleanup", {"candidate_sha": candidate, "observation": "Canonical pending source restored."})

    def launch(self, role, **overrides):
        if getattr(self, "automatic_closeout", True):
            if self.state and self.state["phase"] == "closing":
                self.closeout()
            if self.state and self.state.get("closeout", {}).get("stage") == "cleanup" and role in ("implementer", "planner"):
                self.cleanup()
        self.counter += 1
        model = {"model": "gpt-6.1-sol", "effort": "high"}
        payload = {"role": role, "attempt_id": f"{role}-{self.counter}", "requested": model, "actual": model,
                   "catalog": {"gpt-6.1-sol": ["high", "medium"]}}
        payload.update(overrides)
        self.act("dispatch", payload)
        if self.state["status"] == "running":
            self.act("acknowledge", {"attempt_id": payload["attempt_id"], "worker_id": "worker/" + payload["attempt_id"]})
        return payload

    def report(self):
        return self.harness.tuple_fields(self.state["attempts"][-1])

    def planned(self, plan=None):
        self.initialize()
        self.launch("planner")
        self.act("report", dict(self.report(), plan=plan or self.plan()))

    def commit_candidate(self, value=1):
        (self.source / "behavior.py").write_text(f"def value():\n    return {value}\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "--allow-empty", "-m", "candidate")
        return git(self.source, "rev-parse", "HEAD").decode().strip()

    def implemented(self, candidate=None):
        self.launch("implementer")
        candidate = candidate or self.commit_candidate()
        self.act("report", dict(self.report(), result_candidate_sha=candidate,
                                test_mapping={"C1": "test_behavior.Behavior.test_value"},
                                commands=[{"command": "python unittest", "exit_code": 0}]))
        return candidate

    def run_behavior(self):
        env = os.environ.copy()
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        result = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_behavior"], cwd=self.source,
                                env=env, capture_output=True, text=True)
        self.assertIn("test_value", result.stderr)
        return result

    def check_report(self, result=None):
        result = result or self.run_behavior()
        report = self.report()
        log = f"runs/{self.root_key}/run-1/evidence/{report['attempt_id']}.log"
        test_id = "test_behavior.Behavior.test_value"
        identity = {key: report[key] for key in ("candidate_sha", "base_sha", "plan_version", "attempt_id")}
        report.update(verdict="accepted", discovered=[test_id], executed=[test_id], skipped=[], findings=[],
                      test_mapping={"C1": {"location": "test_behavior.py:4", "tests": [test_id]}},
                      evidence=[dict(identity, gate="test", result="passed" if result.returncode == 0 else "failed",
                                     exit_code=result.returncode, command=[sys.executable, "-m", "unittest", "-v", "test_behavior"],
                                     cwd=str(self.source), environment={"python": sys.version}, log=log, criteria=["AC1"])],
                      records={log: result.stdout + result.stderr})
        return report

    def closeout(self, renew=True):
        targets = [{"key": self.root_key, "location": "local-docs/task.md", "evidence": "Assertion-bearing fixture test passed; independent unit acceptance.", "reference": "https://github.com/example/project/compare/master...codex/test"}]
        targets += [{"key": u["key"], "location": "local-docs/" + u["title"] + ".md", "evidence": targets[0]["evidence"], "reference": targets[0]["reference"]} for u in self.state["units"] if u["key"] != self.root_key]
        self.act("closeout-intent", {"targets": targets})
        for target in targets:
            path = self.source / target["location"]
            text = path.read_text().replace("**Status:** Pending implementation.", "**Status:** Done (verified proposed branch state; not landed on master).")
            text += "**Completion evidence:** " + target["evidence"] + " " + target["reference"] + "\n"
            path.write_text(text)
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "close canonical backlog")
        candidate = git(self.source, "rev-parse", "HEAD").decode().strip()
        self.act("closeout-record", {"candidate_sha": candidate})
        while renew and self.state["operation"] == "subtask":
            self.launch("checker")
            report = self.check_report()
            if "C2" in {c["id"] for c in self.state["plan"]["cases"]}:
                env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
                result = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_second"], cwd=self.source, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                second = "test_second.Second.test_second"
                report["discovered"].append(second)
                report["executed"].append(second)
                report["test_mapping"]["C2"] = {"location": "test_second.py", "tests": [second]}
                report["records"][report["evidence"][0]["log"]] += result.stdout + result.stderr
                report["evidence"][0]["command"].append("test_second")
            self.act("report", report)

    def checked(self):
        self.launch("checker")
        self.act("report", self.check_report())
        if self.state["phase"] == "closing":
            self.closeout()

    def accepted(self):
        self.planned()
        self.implemented()
        self.checked()
        self.assertEqual(self.state["operation"], "final")
        self.checked()
        self.assertEqual(self.state["phase"], "publishing")

    def intent(self):
        return {field: self.state["repository"][field] for field in ("target_remote", "target_repository", "target_branch", "branch")}

    def publish_ready(self):
        self.accepted()
        self.act("publish-intent", self.intent())
        git(self.source, "push", "origin", "codex/test")
        self.act("publish-observation", {"event": "push", "remote_sha": self.state["repository"]["candidate_sha"]})

    def lookup(self, matches):
        return {"event": "lookup", "all_states": True, "unambiguous": True, "matches": matches}

    def pr(self, **changes):
        pr = dict(self.intent(), state="open", draft=False, url="https://example.invalid/project/pull/1")
        pr.update(changes)
        return pr

    def unchanged_rejection(self, action, payload, pattern=None):
        before = self.harness.show()
        with self.assertRaisesRegex(Invalid, pattern or ".*"):
            self.act(action, payload)
        self.assertEqual(self.harness.show(), before)


    def test_T03b_duplicate_durable_finding_identity_is_invalid(self):
        self.planned()
        malformed = copy.deepcopy(self.state)
        finding = {"id": "same", "status": "open", "route": "implementation", "evidence": "failure"}
        malformed["findings"] = [finding, copy.deepcopy(finding)]
        with self.assertRaisesRegex(Invalid, "duplicate finding"):
            validate_state(malformed, self.source)
        # A single resolved record remains valid.
        malformed["findings"] = [dict(finding, status="resolved")]
        validate_state(malformed, self.source)

    def test_T10b_same_report_duplicate_findings_rejected_and_resolution_recovers(self):
        self.planned()
        self.implemented()
        self.launch("checker")
        finding = {"id": "same", "status": "open", "route": "implementation", "evidence": "concrete failure"}
        rejected = dict(self.report(), verdict="changes_required", findings=[finding, copy.deepcopy(finding)], next_action="repair")
        self.unchanged_rejection("report", rejected, "duplicate finding")
        rejected["findings"].pop()
        self.act("report", rejected)
        self.implemented()
        self.launch("checker")
        self.unchanged_rejection("report", self.check_report(), "unresolved prior finding")
        self.unchanged_rejection("report", dict(self.report(), verdict="changes_required", findings=[finding], next_action="repair"), "duplicate finding")
        self.act("resolve-finding", {"finding_id": "same", "evidence": "fresh checker reran concrete reproducer successfully"})
        self.act("report", self.check_report())
        self.assertEqual(self.state["operation"], "closeout")
        self.assertEqual([f["status"] for f in self.state["findings"]], ["resolved"])

    def test_T13b_non_topological_plan_replan_reset_and_dispatch_readiness(self):
        plan = self.split_plan()
        first, second = [u["key"] for u in plan["subtasks"]]
        plan["subtasks"].reverse()
        self.launch("planner")
        self.act("report", dict(self.report(), plan=plan))
        self.assertEqual(self.state["active_unit"], first)
        self.implemented()
        self.launch("checker")
        self.act("report", dict(self.report(), verdict="changes_required", findings=[
            {"id": "plan-change", "status": "open", "route": "planner", "evidence": "add assertion detail"}], next_action="replan"))
        self.launch("planner")
        plan["version"] = 2
        self.act("report", dict(self.report(), plan=plan))
        self.assertEqual(self.state["active_unit"], first)
        self.act("observe-source", {"candidate_sha": self.commit_candidate()})
        self.assertEqual(self.state["active_unit"], first)
        # Dispatch independently rejects a malformed selection, even if selection logic regresses.
        bad = copy.deepcopy(self.state)
        bad["active_unit"] = second
        before = copy.deepcopy(bad)
        model = {"model": "gpt-6.1-sol", "effort": "high"}
        with self.assertRaisesRegex(Invalid, "dependencies"):
            self.harness.dispatch(bad, {"role": "implementer", "attempt_id": "unready",
                                      "requested": model, "actual": model, "catalog": {"gpt-6.1-sol": ["high"]}})
        self.assertEqual(bad, before)
        self.implemented()
        self.launch("checker")
        self.act("resolve-finding", {"finding_id": "plan-change", "evidence": "fresh checker verified added assertion"})
        self.act("report", self.check_report())
        self.assertEqual(self.state["active_unit"], second)
        self.launch("implementer")
        self.assertEqual(self.state["attempts"][-1]["unit"], second)

    def test_T09c_late_final_report_retained_but_requires_extension(self):
        self.planned()
        self.implemented()
        self.checked()
        self.launch("checker")
        report = self.check_report()
        self.now = self.state["limits"]["deadline"]
        self.act("report", report)
        self.assertEqual(self.state["status"], "awaiting_decision")
        self.assertEqual(self.state["operation"], "budget")
        self.assertTrue(self.state["limits"]["exhausted"])
        self.assertEqual(self.state["attempts"][-1]["status"], "complete")
        self.assertEqual(json.loads(self.harness.store.records(self.tip)[self.state["attempts"][-1]["report"]]), report)
        self.assertIsNotNone(self.harness.show()["active"])
        self.act("publish-intent", self.intent())
        self.assertEqual(self.state["publishing"]["stage"], "none")
        self.unchanged_rejection("resume", {"observation": "time passed", "next_action": "publish"}, "non-budget")
        self.act("budget-extension", {"deadline": self.now + 100, "authorization": "human approved", "reason": "finish publication"})
        self.assertEqual((self.state["operation"], self.state["status"]), ("publish", "ready"))
        self.act("publish-intent", self.intent())
        git(self.source, "push", "origin", "codex/test")
        self.act("publish-observation", {"event": "push", "remote_sha": self.state["repository"]["candidate_sha"]})
        self.act("publish-observation", self.lookup([self.pr()]))
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": True})
        self.assertIsNone(self.harness.show()["active"])

    def test_T09d_expired_command_pauses_live_attempt_and_preserves_late_report(self):
        self.planned()
        self.implemented()
        self.launch("checker")
        report = self.check_report()
        command_count = len(self.state["limits"]["commands"])
        self.now = self.state["limits"]["deadline"]
        self.act("command-intent", {"command": "python unittest", "cwd": str(self.source), "timeout": 30})
        self.assertEqual(len(self.state["limits"]["commands"]), command_count)
        self.assertEqual(self.state["operation"], "budget")
        self.assertEqual(self.state["attempts"][-1]["status"], "running")
        self.act("report", report)
        self.assertEqual(self.state["operation"], "budget")
        self.assertEqual(self.state["attempts"][-1]["status"], "complete")
        self.assertEqual(self.state["limits"]["resume_operation"], "closeout")
        self.unchanged_rejection("budget-extension", {"repairs": 4, "authorization": "human approved", "reason": "insufficient deadline"}, "unexpired deadline")
        self.act("budget-extension", {"deadline": self.now + 100, "authorization": "human approved", "reason": "continue"})
        self.assertEqual((self.state["operation"], self.state["status"]), ("closeout", "ready"))
        self.assertIn("closeout-intent", self.state["next_action"])
        self.closeout()
        self.checked()
        self.assertEqual(self.state["phase"], "publishing")

    def test_T15b_deadline_stops_publication_at_every_stage(self):
        self.payload["ci_policy"] = "required_after_pr"
        self.accepted()
        self.act("publish-intent", self.intent())
        git(self.source, "push", "origin", "codex/test")
        events = [{"event": "push", "remote_sha": self.state["repository"]["candidate_sha"]},
                  self.lookup([]), {"event": "create-dispatched"}, self.lookup([self.pr()]),
                  {"event": "attachment", "url": self.pr()["url"], "success": True},
                  {"event": "ci", "result": "passed", "evidence": "required hosted checks passed"}]
        for event in events:
            with self.subTest(event=event["event"]):
                publication = copy.deepcopy(self.state["publishing"])
                self.now = self.state["limits"]["deadline"] + 1
                self.act("publish-observation", event)
                self.assertEqual(self.state["publishing"], publication)
                self.assertEqual(self.state["operation"], "budget")
                self.assertIsNotNone(self.harness.show()["active"])
                self.act("budget-extension", {"deadline": self.now + 100, "authorization": "human approved", "reason": "continue exact publication stage"})
                self.act("publish-observation", event)
        self.assertEqual(self.state["status"], "complete")

    def test_T01_happy_path_real_checkpoint_and_source_preservation(self):
        (self.repo / "private.txt").write_text("user untracked")
        (self.repo / "behavior.py").write_text("user edits\n")
        before = (git(self.repo, "status", "--porcelain"), git(self.repo, "rev-parse", "HEAD"), (common_dir(self.repo) / "index").read_bytes())
        self.publish_ready()
        self.act("publish-observation", self.lookup([self.pr()]))
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": True})
        self.assertEqual(self.state["phase"], "complete")
        records = self.harness.store.records(self.tip)
        self.assertIsNone(json.loads(records["active.json"]))
        self.assertEqual(json.loads(records[state_path(self.root_key, "run-1")]), self.state)
        self.assertGreater(self.state["revision"], 15)
        self.assertEqual(before, (git(self.repo, "status", "--porcelain"), git(self.repo, "rev-parse", "HEAD"), (common_dir(self.repo) / "index").read_bytes()))
        self.assertEqual((self.repo / "private.txt").read_text(), "user untracked")
        self.assertEqual(git(self.source, "diff", self.base, "--name-only").decode(), "local-docs/task.md\n")

    def test_T02_failure_boundaries_and_real_cas_loser(self):
        self.initialize()
        before = self.harness.show()
        for stage in ("write", "tree", "commit", "ref"):
            with self.subTest(stage=stage):
                broken = Harness(self.source, clock=lambda: self.now, fail=stage)
                with self.assertRaisesRegex(Invalid, "injected"):
                    broken.apply("command-intent", {"command": "test", "cwd": str(self.source), "timeout": 1}, self.tip, self.state["revision"])
                self.assertEqual(Harness(self.source).show(), before)
        records = self.harness.store.records(self.tip)
        old = self.tip
        self.act("command-intent", {"command": "test", "cwd": str(self.source), "timeout": 1})
        with self.assertRaisesRegex(Invalid, "tip changed"):
            Store(self.source).checkpoint(records, old, "loser")
        self.assertEqual(Store(self.source).tip(), self.tip)
        # Race after the preliminary check: Git's actual expected-old update rejects.
        old = self.tip
        winner = git(self.source, "commit-tree", old + "^{tree}", "-p", old, data=b"concurrent winner\n").decode().strip()
        original_git = git
        def race(repo, *args, **kwargs):
            if args[0] == "update-ref":
                original_git(repo, "update-ref", REF, winner, old)
            return original_git(repo, *args, **kwargs)
        with patch("legacy_state.git", side_effect=race):
            with self.assertRaisesRegex(Invalid, "update-ref"):
                Store(self.source).checkpoint(records, old, "CAS loser")
        self.tip = winner
        self.assertEqual(Harness(self.source).show()["state"], self.state)
        # A dirty artifact worktree is just a view, ignored on restart.
        view = self.root / "artifacts"
        git(self.repo, "worktree", "add", str(view), "harness-artifacts")
        (view / "active.json").write_text("corrupt uncommitted view")
        git(view, "add", "active.json")
        self.assertEqual(Harness(self.source).show()["state"], self.state)

    def test_T03_invalid_state_paths_schema_and_source_branch_adoption(self):
        self.initialize()
        self.unchanged_rejection("unknown", {})
        with self.assertRaises(Invalid):
            self.harness.apply("reserve", {"title": "x", "location": "x"}, self.tip, 999)
        for path in ("../escape.json", "/absolute.json", "source.cpp", "runs/FOS-000025/run-1/evidence/../../x", "runs/FOS-000025/run-1/evidence/link"):
            self.unchanged_rejection("command-intent", {"command": "test", "cwd": "x", "timeout": 1, "records": {path: "bad"}})
        records = self.harness.store.records(self.tip)
        state = copy.deepcopy(self.state)
        for field, value in (("schema_version", 77), ("task_key", "../wrong"), ("repository", {})):
            malformed = copy.deepcopy(state)
            malformed[field] = value
            records[state_path(self.root_key, "run-1")] = encoded(malformed)
            old = self.tip
            bad = self.harness.store.checkpoint(records, old, "malformed fixture")
            with self.assertRaises((Invalid, KeyError)):
                Harness(self.source).show()
            git(self.source, "update-ref", REF, old, bad)
        records = self.harness.store.records(self.tip)
        records["active.json"] = encoded(None)
        bad = self.harness.store.checkpoint(records, self.tip, "orphan unfinished run fixture")
        with self.assertRaisesRegex(Invalid, "orphan unfinished"):
            Harness(self.source).show()
        git(self.source, "update-ref", REF, self.tip, bad)
        # Symlink and source-tree ancestors are refused, including legacy branches.
        git(self.source, "update-ref", REF, self.base, self.tip)
        with self.assertRaisesRegex(Invalid, "records only"):
            Harness(self.source).show()
        git(self.source, "update-ref", REF, self.tip, self.base)
        env = os.environ.copy()
        env["GIT_INDEX_FILE"] = str(self.root / "bad-index")
        git(self.source, "read-tree", self.tip, env=env)
        blob = git(self.source, "hash-object", "-w", "--stdin", data=b"elsewhere").decode().strip()
        git(self.source, "update-index", "--add", "--cacheinfo", "120000", blob, "runs/FOS-000025/run-1/evidence/link.log", env=env)
        tree = git(self.source, "write-tree", env=env).decode().strip()
        bad = git(self.source, "commit-tree", tree, "-p", self.tip, data=b"symlink fixture\n").decode().strip()
        git(self.source, "update-ref", REF, bad, self.tip)
        with self.assertRaisesRegex(Invalid, "non-regular"):
            Harness(self.source).show()

    def test_T04_process_worktree_lock_and_single_active_run(self):
        ready, proceed = multiprocessing.Event(), multiprocessing.Event()
        output = multiprocessing.Queue()
        process = multiprocessing.Process(target=lock_contender, args=(self.repo, ready, proceed, output))
        process.start()
        self.addCleanup(lambda: process.terminate() if process.is_alive() else None)
        self.assertTrue(ready.wait(10))
        try:
            with self.assertRaisesRegex(Invalid, "lock is held"):
                self.initialize()
        finally:
            proceed.set()
            process.join(10)
        self.assertEqual(process.exitcode, 0)
        self.assertEqual(output.get(timeout=2), "released")
        self.initialize()
        self.assertTrue(self.act("initialize", self.payload)["resumed"])
        self.unchanged_rejection("initialize", dict(self.payload, run_id="other"), "another run")
        self.assertIsNotNone(self.harness.show()["active"])

    def test_T05_dynamic_registry_reservations_archives_conflicts(self):
        self.registry_rows.append("| `FOS-000030` | Retired task | [archive](archive.md) | Retired |")
        self.write_registry(self.source)
        (self.source / "local-docs/archive.md").write_text("# Historical references\nFOS-000030 and FOS-000025\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "retired allocation")
        self.initialize()
        self.act("reserve", {"title": "Reserved", "location": "harness-artifacts:runs/subtask"})
        records = self.harness.store.records(self.tip)
        journal = json.loads(records["allocations.json"])
        self.assertEqual(journal["issued"][-1]["key"], "FOS-000031")
        # No registry update occurred; restart still skips the interrupted reservation.
        self.harness = Harness(self.source, clock=lambda: self.now)
        self.act("reserve", {"title": "Next", "location": "harness-artifacts:runs/next"})
        journal = json.loads(self.harness.store.records(self.tip)["allocations.json"])
        self.assertEqual(journal["issued"][-1]["key"], "FOS-000032")
        (self.source / "local-docs/duplicate.md").write_text("# FOS-000025 — Duplicate\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "invalid duplicate fixture")
        self.unchanged_rejection("reserve", {"title": "x", "location": "x"}, "duplicate canonical")
        (self.source / "local-docs/duplicate.md").write_text("# FOS-000099 — Unregistered\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "invalid missing registration")
        self.unchanged_rejection("reserve", {"title": "x", "location": "x"}, "unregistered")
        (self.source / "local-docs/duplicate.md").unlink()
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "restore fixture")
        journal["issued"][-1]["key"] = self.root_key
        with self.assertRaisesRegex(Invalid, "meaning conflict"):
            canonical_inventory(self.source, journal)
        (self.source / "local-docs/task.md").write_text("# FOS-000025 — Root task **depends on FOS-000099;**\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "dangling dependency fixture")
        self.unchanged_rejection("reserve", {"title": "x", "location": "x"}, "dangling")
        (self.source / "local-docs/task.md").write_text("# FOS-000025 — Root task **depends on FOS-000030;**\n")
        (self.source / "local-docs/archive.md").write_text("# FOS-000030 — Retired task **depends on FOS-000025;**\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "cyclic dependency fixture")
        self.unchanged_rejection("reserve", {"title": "x", "location": "x"}, "cyclic")

    def test_T06_detached_exact_views_dirty_source_and_changed_candidate(self):
        self.planned()
        planner = self.root / "planner"
        detached_worktree(self.source, planner, self.base)
        self.assertEqual(git(planner, "rev-parse", "HEAD").decode().strip(), self.base)
        self.assertNotEqual(subprocess.run(["git", "-C", str(planner), "symbolic-ref", "-q", "HEAD"], capture_output=True).returncode, 0)
        candidate = self.implemented()
        checker = self.root / "checker"
        detached_worktree(self.source, checker, candidate)
        self.assertEqual(git(checker, "rev-parse", "HEAD").decode().strip(), candidate)
        with self.assertRaisesRegex(Invalid, "already exists"):
            detached_worktree(self.source, checker, self.base)
        self.checked()
        self.checked()
        (self.source / "untracked").write_text("preserve")
        self.unchanged_rejection("publish-intent", self.intent(), "dirty")
        self.assertEqual((self.source / "untracked").read_text(), "preserve")
        (self.source / "untracked").unlink()
        new = self.commit_candidate()
        self.unchanged_rejection("publish-intent", self.intent(), "HEAD changed")
        self.act("observe-source", {"candidate_sha": new})
        self.assertIsNone(self.state["final_acceptance"])
        self.assertEqual(self.state["phase"], "implementing")

    def test_T07_dispatch_ack_loss_live_unknown_and_termination_fence(self):
        self.initialize()
        model = {"model": "gpt-6.1-sol", "effort": "high"}
        dispatch = {"role": "planner", "attempt_id": "lost", "requested": model, "actual": model, "catalog": {"gpt-6.1-sol": ["high"]}}
        self.act("dispatch", dispatch)
        self.assertEqual(self.state["attempts"][-1]["status"], "intent")
        for liveness in ("live", "unknown"):
            self.act("reconcile", {"attempt_id": "lost", "liveness": liveness, "observation": "runtime status snapshot", "next_action": "confirm worker termination"})
            self.unchanged_rejection("dispatch", dict(dispatch, attempt_id="replacement"), "live/ambiguous")
        self.act("reconcile", {"attempt_id": "lost", "liveness": "terminated", "observation": "runtime cancellation confirmed", "next_action": "replace planner"})
        self.launch("planner")
        stale = self.report()
        stale["attempt_id"] = "lost"
        self.unchanged_rejection("report", dict(stale, plan=self.plan()), "stale")
        self.act("report", dict(self.report(), plan=self.plan()))

    def test_T08_each_stale_tuple_field_rejected_ack_revision_supported(self):
        self.planned()
        self.implemented()
        self.launch("checker")
        report = self.check_report()
        self.assertLess(report["input_revision"], self.state["revision"])
        for field in ("attempt_id", "worker_id", "input_revision", "input_sha", "candidate_sha", "base_sha", "plan_version", "role", "operation", "unit"):
            bad = copy.deepcopy(report)
            bad[field] = 999 if isinstance(bad[field], int) else "stale"
            self.unchanged_rejection("report", bad, "stale")
        self.act("report", report)
        self.launch("checker")
        self.unchanged_rejection("report", report, "stale")

    def test_T09_models_fallback_deadline_repair_replan_and_extension(self):
        self.initialize()
        bad = {"role": "planner", "attempt_id": "unsupported", "requested": {"model": "missing", "effort": "high"},
               "actual": {"model": "missing", "effort": "high"}, "catalog": {"supported": ["high"]}}
        self.unchanged_rejection("dispatch", bad, "unsupported")
        bad["actual"] = {"model": "supported", "effort": "high"}
        self.unchanged_rejection("dispatch", bad, "fallback")
        bad["fallback_reason"] = "requested model unavailable; approved supported fallback"
        self.act("dispatch", bad)
        self.assertEqual(self.state["attempts"][-1]["requested"]["model"], "missing")
        self.assertEqual(self.state["attempts"][-1]["actual"]["model"], "supported")
        self.act("reconcile", {"attempt_id": "unsupported", "liveness": "terminated", "observation": "confirmed", "next_action": "retry"})
        self.now = self.state["limits"]["deadline"]
        self.launch("planner")
        self.assertEqual(self.state["operation"], "budget")
        self.assertIn("deadline", self.state["next_action"])
        self.unchanged_rejection("budget-extension", {"deadline": self.now + 100}, "authorization")
        self.act("budget-extension", {"deadline": self.now + 100, "authorization": "human requested extension", "reason": "continue"})
        self.launch("planner")
        self.act("report", dict(self.report(), plan=self.plan()))
        # Four initial/repair implementations allowed; fifth refuses deterministically.
        for index in range(4):
            self.implemented()
            self.launch("checker")
            rejected = dict(self.report(), verdict="changes_required", findings=[{"id": f"repair-{index}", "status": "open", "route": "implementation", "evidence": f"wrong value in distinct probe {index}"}], next_action="repair implementation")
            self.act("report", rejected)
        self.launch("implementer")
        self.assertEqual(self.state["operation"], "budget")
        self.assertIn("repair", self.state["next_action"])
        self.act("budget-extension", {"repairs": 4, "authorization": "human approved", "reason": "one additional repair"})
        self.implemented()
        self.launch("checker")
        self.act("report", dict(self.report(), verdict="changes_required", findings=[{"id": "plan-gap", "status": "open", "route": "planner", "evidence": "unplanned risk"}], next_action="replan"))
        self.launch("planner")
        plan = self.plan()
        plan["version"] = 2
        self.act("report", dict(self.report(), plan=plan))
        self.act("budget-extension", {"repairs": 5, "authorization": "human approved", "reason": "replan implementation"})
        self.implemented()
        self.launch("checker")
        self.act("report", dict(self.report(), verdict="changes_required", findings=[{"id": "plan-gap-2", "status": "open", "route": "planner", "evidence": "new missing coverage"}], next_action="replan again"))
        self.launch("planner")
        self.assertIn("replan budget", self.state["next_action"])

    def test_T10_required_evidence_completeness_and_optional_unavailable(self):
        self.planned()
        self.implemented()
        self.launch("checker")
        report = self.check_report()
        mutations = [lambda r: r.update(evidence=[]), lambda r: r.update(discovered=[]), lambda r: r.update(executed=[]),
                     lambda r: r.update(test_mapping={}), lambda r: r.update(skipped=["C1"]),
                     lambda r: r["evidence"][0].pop("log"), lambda r: r["evidence"][0].update(exit_code=1),
                     lambda r: r["evidence"][0].update(candidate_sha=self.base),
                     lambda r: r.update(findings=[{"id": "bad", "status": "open", "route": "implementation", "evidence": "weakened assertion"}])]
        for result in ("failed", "unsupported", "incomplete", "not-run"):
            mutations.append(lambda r, result=result: r["evidence"][0].update(result=result))
        for mutation in mutations:
            bad = copy.deepcopy(report)
            mutation(bad)
            self.unchanged_rejection("report", bad)
        # Optional diagnostics may be unavailable, separately recorded.
        report["evidence"].append({"gate": "optional-profile", "result": "unsupported", "reason": "tool missing"})
        self.act("report", report)
        self.assertEqual(self.state["operation"], "closeout")

    def test_T09b_repeated_unchanged_rejection_stops_early(self):
        self.planned()
        for index in range(2):
            self.implemented()
            self.launch("checker")
            self.act("report", dict(self.report(), verdict="changes_required", findings=[
                {"id": f"repeat-{index}", "status": "open", "route": "implementation", "evidence": "same failure with no new evidence"}], next_action="repair"))
        self.assertTrue(self.state["limits"]["exhausted"])
        self.assertEqual(self.state["status"], "awaiting_decision")
        self.assertIn("repeated unchanged", self.state["next_action"])
        self.unchanged_rejection("resume", {"observation": "try again", "next_action": "retry"}, "non-budget")

    def test_T11_real_faulty_reproducer_and_weakened_test_finding(self):
        self.planned()
        self.implemented(self.commit_candidate(value=2))
        self.launch("checker")
        failure = self.run_behavior()
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("2 != 1", failure.stderr)
        self.unchanged_rejection("report", self.check_report(failure), "not passed")
        # Independent semantic finding blocks acceptance even if someone weakens test.
        self.act("report", dict(self.report(), verdict="changes_required", findings=[{"id": "original-defect", "status": "open", "route": "implementation", "evidence": failure.stderr}], next_action="fix original defect and retain assertion"))
        self.implemented(self.commit_candidate(value=1))
        self.launch("checker")
        success = self.run_behavior()
        self.assertEqual(success.returncode, 0)
        self.act("resolve-finding", {"finding_id": "original-defect", "evidence": "fresh rerun asserts value=1"})
        weakened = self.check_report(success)
        weakened["findings"] = [{"id": "weakened", "status": "open", "route": "implementation", "evidence": "checker inspection: assertion removed"}]
        self.unchanged_rejection("report", weakened, "unresolved")
        self.act("report", self.check_report(success))

    def test_T12_repair_versus_replan_and_no_implementer_acceptance(self):
        self.planned()
        self.implemented()
        self.launch("checker")
        self.act("report", dict(self.report(), verdict="changes_required", findings=[{"id": "missing-test", "status": "open", "route": "implementation", "evidence": "planned C1 missing"}], next_action="implement planned test"))
        self.assertEqual(self.state["phase"], "implementing")
        self.unchanged_rejection("dispatch", {"role": "planner", "attempt_id": "wrong"}, "role")
        self.implemented()
        self.launch("checker")
        self.act("report", dict(self.report(), verdict="changes_required", findings=[{"id": "missing-plan", "status": "open", "route": "planner", "evidence": "missing necessary test design"}], next_action="version test plan"))
        self.assertEqual(self.state["phase"], "planning")
        self.launch("planner")
        changed = self.plan()
        changed["version"] = 2
        changed["original"] = [{"id": "AC1", "requirement": "weakened"}]
        self.unchanged_rejection("report", dict(self.report(), plan=changed), "preserve original")
        changed["original"] = self.original
        self.act("report", dict(self.report(), plan=changed))
        self.assertIsNone(self.state["final_acceptance"])
        self.assertTrue(all(unit["accepted"] is None for unit in self.state["units"]))

    def split_plan(self):
        self.initialize()
        keys = []
        for title in ("First", "Second"):
            self.act("reserve", {"title": title, "location": "local-docs/" + title + ".md"})
            row = json.loads(self.harness.store.records(self.tip)["allocations.json"])["issued"][-1]
            keys.append(row["key"])
            self.registry_rows.append(f"| `{row['key']}` | {title} | [task]({title}.md) | Allocated |")
        self.write_registry(self.source)
        for key, title in zip(keys, ("First", "Second")):
            (self.source / ("local-docs/" + title + ".md")).write_text(f"# {key} — {title}\n**Status:** Pending implementation.\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "register synthetic subtasks")
        plan = self.plan()
        self.assertEqual(keys, ["FOS-000026", "FOS-000027"])
        plan["cases"][0]["units"] = [keys[0]]
        plan["cases"].append({"id": "C2", "assertions": "second feature equals two", "location": "test_second.py", "units": [keys[1]]})
        plan["subtasks"] = [{"key": keys[0], "title": "First", "definition": "first behavior", "dependencies": [], "criteria": ["AC1"]},
                            {"key": keys[1], "title": "Second", "definition": "second behavior", "dependencies": [keys[0]], "criteria": ["AC1"]}]
        return plan

    def test_T13_sequential_split_dependency_validation_final_regression(self):
        plan = self.split_plan()
        self.launch("planner")
        for modification in ("cycle", "dangling", "unregistered", "root", "retired"):
            bad = copy.deepcopy(plan)
            if modification == "cycle":
                bad["subtasks"][0]["dependencies"] = [bad["subtasks"][1]["key"]]
            elif modification == "dangling":
                bad["subtasks"][0]["dependencies"] = ["FOS-999999"]
            elif modification == "unregistered":
                bad["subtasks"][0]["key"] = "FOS-999999"
            elif modification == "root":
                bad["subtasks"][0]["key"] = self.root_key
            else:
                key = bad["subtasks"][0]["key"]
                old = (self.source / "local-docs/backlog-keys.md").read_text()
                (self.source / "local-docs/backlog-keys.md").write_text(old.replace(f"{key}` | First | [task](First.md) | Allocated", f"{key}` | First | [task](First.md) | Retired"))
                git(self.source, "add", ".")
                git(self.source, "commit", "-m", "retired fixture")
            self.unchanged_rejection("report", dict(self.report(), plan=bad))
            if modification == "retired":
                (self.source / "local-docs/backlog-keys.md").write_text(old)
                git(self.source, "add", ".")
                git(self.source, "commit", "-m", "restore registry")
        first, second = [unit["key"] for unit in plan["subtasks"]]
        # Readiness must also survive final repair with a non-topological plan.
        plan["subtasks"].reverse()
        self.act("report", dict(self.report(), plan=plan))
        self.assertEqual(self.state["active_unit"], first)
        self.launch("implementer")
        self.unchanged_rejection("dispatch", {"role": "implementer", "attempt_id": "parallel"}, "live/ambiguous")
        self.act("report", dict(self.report(), result_candidate_sha=self.commit_candidate(), test_mapping={"C1": "test_behavior.py"}, commands=[{"exit_code": 0}]))
        self.checked()
        self.assertEqual(self.state["active_unit"], second)
        self.launch("implementer")
        (self.source / "test_second.py").write_text("import unittest\nfrom behavior import value\nclass Second(unittest.TestCase):\n    def test_second(self):\n        self.assertEqual(value(), 2)\n")
        self.act("report", dict(self.report(), result_candidate_sha=self.commit_candidate(value=2), test_mapping={"C2": "test_second.py"}, commands=[{"exit_code": 0}]))
        self.launch("checker")
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        second_run = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_second"], cwd=self.source, env=env, capture_output=True, text=True)
        self.assertEqual(second_run.returncode, 0)
        second_report = self.check_report(second_run)
        second_id = "test_second.Second.test_second"
        second_report.update(discovered=[second_id], executed=[second_id], test_mapping={"C2": {"location": "test_second.py:4", "tests": [second_id]}})
        second_report["evidence"][0]["command"][-1] = "test_second"
        self.act("report", second_report)
        self.closeout(renew=False)
        self.assertTrue(all(u["status"] == "ready" for u in self.state["units"]))
        self.assertEqual(self.state["operation"], "subtask")
        # The independent integration run finds an effect outside subtask evidence.
        self.launch("checker")
        failure = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_behavior", "test_second"], cwd=self.source, env=env, capture_output=True, text=True)
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("2 != 1", failure.stderr)
        self.assertIn("test_second", failure.stderr)
        self.act("report", dict(self.report(), verdict="changes_required", findings=[{"id": "integration", "status": "open", "route": "implementation", "evidence": failure.stderr}], next_action="repair integrated behavior"))
        self.assertIsNone(self.state["final_acceptance"])
        self.unchanged_rejection("publish-intent", self.intent(), "final acceptance")
        self.assertEqual(self.state["active_unit"], first)
        self.assertTrue(all(u["status"] == "ready" and u["accepted"] is None for u in self.state["units"]))
        self.assertEqual(self.state["plan"], plan)
        # Repair the integrated implementation without weakening either assertion.
        # The dependency must accept again before the dependent may dispatch.
        self.launch("implementer")
        self.assertEqual(self.state["attempts"][-1]["unit"], first)
        (self.source / "behavior.py").write_text("def value():\n    return 1\ndef second_value():\n    return 2\n")
        (self.source / "test_second.py").write_text("import unittest\nfrom behavior import second_value\nclass Second(unittest.TestCase):\n    def test_second(self):\n        self.assertEqual(second_value(), 2)\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "repair integrated behavior")
        corrected = git(self.source, "rev-parse", "HEAD").decode().strip()
        self.act("report", dict(self.report(), result_candidate_sha=corrected,
                                test_mapping={"C1": "test_behavior.py", "C2": "test_second.py"}, commands=[{"exit_code": 0}]))
        self.unchanged_rejection("dispatch", {"role": "implementer", "attempt_id": "skip-recheck"}, "role")
        self.launch("checker")
        success = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_behavior", "test_second"], cwd=self.source, env=env, capture_output=True, text=True)
        self.assertEqual(success.returncode, 0, success.stderr)
        self.unchanged_rejection("report", self.check_report(), "unresolved prior finding")
        self.act("resolve-finding", {"finding_id": "integration", "evidence": success.stderr})
        self.act("report", self.check_report())
        self.assertEqual(self.state["active_unit"], second)
        self.assertEqual(self.state["operation"], "implement")
        self.launch("implementer")
        self.assertEqual(self.state["attempts"][-1]["unit"], second)
        self.act("report", dict(self.report(), result_candidate_sha=corrected, test_mapping={"C2": "test_second.py"}, commands=[{"exit_code": 0}]))
        self.launch("checker")
        second_run = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_second"], cwd=self.source, env=env, capture_output=True, text=True)
        self.assertEqual(second_run.returncode, 0, second_run.stderr)
        renewed = self.check_report(second_run)
        renewed.update(discovered=[second_id], executed=[second_id], test_mapping={"C2": {"location": "test_second.py:4", "tests": [second_id]}})
        renewed["evidence"][0]["command"][-1] = "test_second"
        self.act("report", renewed)
        self.closeout()
        corrected = self.state["repository"]["candidate_sha"]
        subtask_attempt = renewed["attempt_id"]
        self.assertEqual(self.state["operation"], "final")
        self.assertIsNone(self.state["final_acceptance"])
        self.unchanged_rejection("publish-intent", self.intent(), "final acceptance")
        self.launch("checker")
        final_run = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_behavior", "test_second"], cwd=self.source, env=env, capture_output=True, text=True)
        self.assertEqual(final_run.returncode, 0, final_run.stderr)
        final = self.check_report(final_run)
        behavior_id = "test_behavior.Behavior.test_value"
        final.update(discovered=[behavior_id, second_id], executed=[behavior_id, second_id],
                     test_mapping={"C1": {"location": "test_behavior.py:4", "tests": [behavior_id]},
                                   "C2": {"location": "test_second.py:4", "tests": [second_id]}})
        final["evidence"][0]["command"].append("test_second")
        self.assertEqual(final["operation"], "final")
        self.assertNotEqual(final["attempt_id"], subtask_attempt)
        self.act("report", final)
        self.assertEqual(self.state["phase"], "publishing")
        self.assertTrue(all(u["accepted"]["candidate_sha"] == corrected for u in self.state["units"]))
        self.assertEqual(self.state["final_acceptance"]["candidate_sha"], corrected)
        self.assertEqual(self.state["limits"]["implementation_counts"], {first: 2, second: 2})

    def test_T13c_unsplit_final_rejection_repairs_and_renews_both_checks(self):
        self.planned()
        self.launch("implementer")
        (self.source / "final_behavior.py").write_text("def final_value():\n    return 1\n")
        (self.source / "test_final.py").write_text("import unittest\nfrom final_behavior import final_value\nclass Final(unittest.TestCase):\n    def test_final(self):\n        self.assertEqual(final_value(), 2)\n")
        candidate = self.commit_candidate()
        self.act("report", dict(self.report(), result_candidate_sha=candidate,
                                test_mapping={"C1": "test_behavior.py"}, commands=[{"exit_code": 0}]))
        self.checked()
        self.launch("checker")
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        command = [sys.executable, "-m", "unittest", "-v", "test_behavior", "test_final"]
        failure = subprocess.run(command, cwd=self.source, env=env, capture_output=True, text=True)
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("1 != 2", failure.stderr)
        self.act("report", dict(self.report(), verdict="changes_required", findings=[
            {"id": "final-defect", "status": "open", "route": "implementation", "evidence": failure.stderr}], next_action="repair final behavior"))
        self.unchanged_rejection("publish-intent", self.intent(), "final acceptance")
        self.launch("implementer")
        self.assertEqual(self.state["attempts"][-1]["unit"], self.root_key)
        self.assertEqual(len(self.state["units"]), 1)
        self.assertEqual(self.state["units"][0]["status"], "ready")
        self.assertIsNone(self.state["units"][0]["accepted"])
        self.assertEqual(self.state["plan"]["subtasks"], [])
        (self.source / "final_behavior.py").write_text("def final_value():\n    return 2\n")
        corrected = self.commit_candidate()
        self.act("report", dict(self.report(), result_candidate_sha=corrected,
                                test_mapping={"C1": "test_behavior.py"}, commands=[{"exit_code": 0}]))
        self.launch("checker")
        success = subprocess.run(command, cwd=self.source, env=env, capture_output=True, text=True)
        self.assertEqual(success.returncode, 0, success.stderr)
        self.unchanged_rejection("report", self.check_report(), "unresolved prior finding")
        self.act("resolve-finding", {"finding_id": "final-defect", "evidence": success.stderr})
        self.act("report", self.check_report())
        self.closeout()
        corrected = self.state["repository"]["candidate_sha"]
        subtask_attempt = self.state["attempts"][-1]["attempt_id"]
        self.assertEqual(self.state["operation"], "final")
        self.assertIsNone(self.state["final_acceptance"])
        self.unchanged_rejection("publish-intent", self.intent(), "final acceptance")
        self.launch("checker")
        final_run = subprocess.run(command, cwd=self.source, env=env, capture_output=True, text=True)
        self.assertEqual(final_run.returncode, 0, final_run.stderr)
        final = self.check_report(final_run)
        final_id = "test_final.Final.test_final"
        final["discovered"].append(final_id)
        final["executed"].append(final_id)
        final["evidence"][0]["command"] = command
        self.assertEqual(final["operation"], "final")
        self.assertNotEqual(final["attempt_id"], subtask_attempt)
        self.act("report", final)
        self.assertEqual(self.state["phase"], "publishing")
        self.assertEqual(self.state["final_acceptance"]["candidate_sha"], corrected)
        self.assertEqual(self.state["limits"]["implementation_counts"], {self.root_key: 2})

    def test_T13d_later_unit_final_finding_allows_scoped_prerequisite_repair(self):
        plan = self.split_plan()
        first, second = [unit["key"] for unit in plan["subtasks"]]
        plan["subtasks"].reverse()
        self.launch("planner")
        self.act("report", dict(self.report(), plan=plan))
        self.implemented()
        self.checked()
        self.launch("implementer")
        (self.source / "second.py").write_text("def second_value(x):\n    return x+1\n")
        (self.source / "test_second.py").write_text("import unittest\nfrom second import second_value\nclass Second(unittest.TestCase):\n    def test_second(self):\n        self.assertEqual(second_value(1), 2)\n")
        (self.source / "test_final.py").write_text("import unittest\nfrom second import second_value\nclass Final(unittest.TestCase):\n    def test_final(self):\n        self.assertEqual(second_value(2), 4)\n")
        candidate = self.commit_candidate()
        self.act("report", dict(self.report(), result_candidate_sha=candidate,
                                test_mapping={"C2": "test_second.py"}, commands=[{"exit_code": 0}]))

        def run(modules):
            result = subprocess.run([sys.executable, "-B", "-m", "unittest", "-v", *modules],
                                    cwd=self.source, capture_output=True, text=True, timeout=30)
            self.assertIn("Ran", result.stderr)
            return result

        behavior_id = "test_behavior.Behavior.test_value"
        second_id = "test_second.Second.test_second"
        final_id = "test_final.Final.test_final"

        def checked_report(modules, mapping):
            result = run(modules)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = self.check_report(result)
            ids = [test for tests in mapping.values() for test in tests]
            report.update(discovered=ids, executed=ids,
                          test_mapping={case: {"location": tests[0].split(".")[0] + ".py:5", "tests": tests}
                                        for case, tests in mapping.items()})
            report["evidence"][0]["command"] = [sys.executable, "-B", "-m", "unittest", "-v", *modules]
            return report

        self.launch("checker")
        self.act("report", checked_report(["test_second"], {"C2": [second_id]}))
        self.launch("checker")
        failure = run(["test_behavior", "test_second", "test_final"])
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("3 != 4", failure.stderr)
        finding = {"id": "later-only", "affected_task": second, "status": "open",
                   "route": "implementation", "evidence": failure.stderr}
        self.act("report", dict(self.report(), verdict="changes_required", findings=[finding],
                                next_action="repair Second within its unit after renewed prerequisite check"))
        self.assertEqual(self.state["active_unit"], first)
        self.assertTrue(all(u["status"] == "ready" and u["accepted"] is None for u in self.state["units"]))
        self.harness = Harness(self.source, clock=lambda: self.now)
        self.assertEqual(self.harness.show()["state"], self.state)
        self.unchanged_rejection("publish-intent", self.intent(), "final acceptance")

        # Reconcile stale status before the prerequisite repair; source behavior is unchanged.
        self.cleanup()
        candidate = self.state["repository"]["candidate_sha"]
        # First honors its scope: return the same commit without fixing Second.
        unchanged = git(self.source, "rev-parse", "HEAD"), (self.source / "second.py").read_bytes()
        self.implemented(candidate)
        self.launch("checker")
        passing = checked_report(["test_behavior"], {"C1": [behavior_id]})
        final_probe = dict(passing, operation="final")
        with self.assertRaisesRegex(Invalid, "unresolved prior finding"):
            self.harness.evidence(self.state, final_probe, self.harness.store.records(self.tip))
        self.act("report", passing)
        self.assertEqual(unchanged, (git(self.source, "rev-parse", "HEAD"), (self.source / "second.py").read_bytes()))
        self.assertEqual(self.state["findings"], [finding])
        self.assertEqual(self.state["active_unit"], second)
        self.assertNotEqual(run(["test_final"]).returncode, 0)

        # Only the dependent implementer changes Second, retaining every assertion.
        self.launch("implementer")
        self.assertEqual(self.report()["unit"], second)
        (self.source / "second.py").write_text("def second_value(x):\n    return x*2\n")
        git(self.source, "add", "second.py")
        git(self.source, "commit", "-m", "repair Second final assertion")
        corrected = git(self.source, "rev-parse", "HEAD").decode().strip()
        self.act("report", dict(self.report(), result_candidate_sha=corrected,
                                test_mapping={"C2": "test_second.py"}, commands=[{"exit_code": 0}]))
        self.launch("checker")
        success = checked_report(["test_second", "test_final"], {"C2": [second_id, final_id]})
        self.unchanged_rejection("report", success, "unresolved prior finding")
        self.act("resolve-finding", {"finding_id": "later-only", "evidence": success["records"][success["evidence"][0]["log"]]})
        self.assertEqual(self.state["findings"][0]["affected_task"], second)
        self.assertEqual(self.state["findings"][0]["status"], "resolved")
        self.act("report", success)
        self.closeout()
        corrected = self.state["repository"]["candidate_sha"]
        subtask_attempt = success["attempt_id"]
        self.assertEqual(self.state["operation"], "final")
        self.unchanged_rejection("publish-intent", self.intent(), "final acceptance")
        self.launch("checker")
        final = checked_report(["test_behavior", "test_second", "test_final"],
                               {"C1": [behavior_id], "C2": [second_id, final_id]})
        self.assertNotEqual(final["attempt_id"], subtask_attempt)
        self.assertEqual(final["operation"], "final")
        self.act("report", final)
        self.assertEqual(self.state["final_acceptance"]["candidate_sha"], corrected)
        # Even after final acceptance, publication guard rejects any reopened finding.
        blocked = copy.deepcopy(self.state)
        blocked["findings"][0]["status"] = "open"
        with self.assertRaisesRegex(Invalid, "unaccepted work/findings"):
            self.harness.publication_guard(blocked, self.intent())
        self.act("publish-intent", self.intent())
        self.assertEqual(self.state["publishing"]["stage"], "push")

    def test_T10c_finding_scope_validation_dependency_branch_and_replan(self):
        plan = self.split_plan()
        first, second = [unit["key"] for unit in plan["subtasks"]]
        self.launch("planner")
        self.act("report", dict(self.report(), plan=plan))
        self.implemented()
        self.launch("checker")
        finding = {"id": "scoped", "affected_task": second, "status": "open",
                   "route": "planner", "evidence": "Second needs a stronger planned assertion"}
        for scope in (None, "", "FOS-999999", [], 27):
            bad = dict(finding, affected_task=scope)
            self.unchanged_rejection("report", dict(self.report(), verdict="changes_required", findings=[bad],
                                                    next_action="replan"), "finding scope")
            malformed = copy.deepcopy(self.state)
            malformed["findings"] = [bad]
            with self.assertRaisesRegex(Invalid, "finding scope"):
                validate_state(malformed, self.source)

        passed = self.check_report()
        records = dict(self.harness.store.records(self.tip))
        records.update({path: content.encode() for path, content in passed["records"].items()})
        for scope in (first, self.root_key, "missing"):
            blocked = copy.deepcopy(self.state)
            blocked["findings"] = [dict(finding)]
            if scope == "missing":
                blocked["findings"][0].pop("affected_task")
            else:
                blocked["findings"][0]["affected_task"] = scope
            # Legacy missing scope remains valid and blocks as root-wide.
            validate_state(blocked, self.source)
            with self.assertRaisesRegex(Invalid, "unresolved prior finding"):
                self.harness.evidence(blocked, passed, records)

        # A finding in a dependency blocks its dependent, even after prior acceptance.
        dependent = dict(passed, unit=second)
        blocked = copy.deepcopy(self.state)
        blocked["findings"] = [dict(finding, affected_task=first)]
        with self.assertRaisesRegex(Invalid, "unresolved prior finding"):
            self.harness.evidence(blocked, dependent, records)
        # An independent branch finding does not taint First's subtask evidence.
        blocked["units"][1]["dependencies"] = []
        blocked["findings"] = [dict(finding)]
        self.harness.evidence(blocked, passed, records)

        self.act("report", dict(self.report(), verdict="changes_required", findings=[finding], next_action="replan"))
        self.launch("planner")
        replanned = copy.deepcopy(plan)
        replanned.update(version=2, subtasks=[])
        replanned["cases"] = [dict(plan["cases"][0], units=[self.root_key])]
        self.act("report", dict(self.report(), plan=replanned))
        self.assertEqual(self.state["findings"], [finding])
        self.assertEqual(self.harness.show()["state"], self.state)
        self.implemented()
        self.launch("checker")
        # Removing a unit in a replan cannot hide its permanently attributed finding.
        self.unchanged_rejection("report", self.check_report(), "unresolved prior finding")
        # Resolution arguments cannot narrow its permanent affected task.
        self.unchanged_rejection("resolve-finding", {"finding_id": "scoped", "affected_task": self.root_key,
                                                    "evidence": "verified"}, "finding scope")
        self.act("resolve-finding", {"finding_id": "scoped", "evidence": "fresh checker verified revised assertion"})
        self.assertEqual(self.state["findings"][0]["affected_task"], second)
        self.act("report", self.check_report())

    def test_T14_required_environment_unavailable_cannot_be_waived(self):
        plan = self.plan()
        plan["gates"].append(dict(plan["gates"][0], id="gpu", prerequisites="physical GPU and driver"))
        self.planned(plan)
        self.implemented()
        self.launch("checker")
        report = self.check_report()
        report["evidence"].append(dict(report["evidence"][0], gate="gpu", result="unsupported", reason="no physical GPU"))
        self.unchanged_rejection("report", report, "not passed")
        self.act("report", dict(self.report(), verdict="unable_to_verify", findings=[{"id": "gpu-missing", "status": "open", "route": "environment", "evidence": "required physical GPU unavailable"}], next_action="provide GPU/driver environment and rerun mandatory gate"))
        self.assertEqual(self.state["status"], "awaiting_decision")
        self.assertIn("GPU", self.state["next_action"])
        self.assertIsNotNone(self.harness.show()["active"])

    def test_T15_publication_order_wrong_tuple_moving_base_push_conflict(self):
        self.accepted()
        self.unchanged_rejection("publish-observation", {"event": "push", "remote_sha": self.base}, "intent first")
        for field in ("target_repository", "target_remote", "target_branch", "branch"):
            self.unchanged_rejection("publish-intent", dict(self.intent(), **{field: "wrong"}), "destination")
        self.act("publish-intent", self.intent())
        self.assertEqual(self.state["publishing"]["stage"], "push")
        self.unchanged_rejection("publish-observation", self.lookup([]), "not expected")
        self.unchanged_rejection("publish-observation", {"event": "push", "remote_sha": self.base}, "conflict")
        git(self.source, "push", "origin", "codex/test")
        self.act("publish-observation", {"event": "push", "remote_sha": self.state["repository"]["candidate_sha"]})
        (self.repo / "new-base").write_text("upstream moved")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-m", "master advances")
        git(self.repo, "push", "origin", "master")
        self.unchanged_rejection("publish-observation", self.lookup([]), "base moved")
        self.assertEqual(git(self.source, "rev-parse", "HEAD").decode().strip(), self.state["repository"]["candidate_sha"])

    def test_T16_idempotent_pr_response_loss_ambiguity_and_attachment_retry(self):
        self.publish_ready()
        self.act("publish-observation", self.lookup([]))
        self.act("publish-observation", {"event": "create-dispatched"})
        fake_transport = {"creates": 0, "prs": []}
        # External service succeeds but response is lost. Restart only performs lookup.
        fake_transport["creates"] += 1
        fake_transport["prs"].append(self.pr())
        self.harness = Harness(self.source, clock=lambda: self.now)
        self.unchanged_rejection("publish-observation", {"event": "create-dispatched"}, "empty lookup")
        self.unchanged_rejection("publish-observation", dict(self.lookup([]), unambiguous=False), "ambiguous")
        for matches in ([self.pr(), self.pr()], [self.pr(state="closed")], [self.pr(state="merged")], [self.pr(branch="wrong")], [self.pr(target_repository="wrong")], [self.pr(target_branch="wrong")]):
            self.unchanged_rejection("publish-observation", self.lookup(matches))
        self.act("publish-observation", self.lookup(fake_transport["prs"]))
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": False})
        self.assertEqual(self.state["publishing"]["stage"], "attach")
        self.unchanged_rejection("publish-observation", {"event": "create-dispatched"})
        self.assertEqual(fake_transport["creates"], 1)
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": True})
        self.assertIsNone(self.harness.show()["active"])
        self.assertEqual(self.state["status"], "complete")

    def test_T17_explicit_bootstrap_import_history_uncertainty_and_conflicts(self):
        # Shape matches actual pre-helper bootstrap, without borrowing actual records.
        old = {"schema_version": 1, "revision": 4, "task_key": self.root_key, "run_id": "run-1",
               "task_location": "local-docs/task.md", "phase": "implementing", "operation": "implementer_dispatch", "status": "running",
               "repository": dict(self.payload["repository"], input_sha=self.base[:8]), "subtasks": [],
               "plan": {"acceptance_criteria": self.original, "plan_version": 1},
               "attempts": [{"attempt_id": "old", "role": "implementer", "input_sha": self.base[:8], "worker_id": "old/worker", "status": "running"}],
               "verification": [], "findings": [], "limits": {"repair_rounds_per_subtask": 3, "replans": 1, "run_timeout_hours": 4},
               "publishing": {"pr_url": None}, "bootstrap": {"reason": "helper absent"}}
        path = state_path(self.root_key, "run-1")
        records = {"active.json": encoded({"task_key": self.root_key, "run_id": "run-1", "state": path}),
                   "allocations.json": encoded({"high_water": 25, "issued": [{"key": self.root_key, "location": "local-docs/backlog-keys.md"}]}),
                   path: encoded(old), "runs/FOS-000025/run-1/reports/old.json": encoded({"report_type": "implementer", "attempt_id": "old",
                       "plan_version": 1, "candidate_sha": self.base, "input_sha": self.base, "fact": "legacy report"})}
        self.tip = Store(self.source).checkpoint(records, None, "legacy bootstrap")
        self.state = old
        legacy_tip = self.tip
        with self.assertRaisesRegex(Invalid, "import-bootstrap"):
            self.harness.show()
        payload = dict(self.payload, reviewed_tip=legacy_tip, review_summary="Reviewed synthetic legacy fixture records", plan=self.plan(), implementation_report="runs/FOS-000025/run-1/reports/old.json")
        self.unchanged_import_rejection(dict(payload, run_id="conflict"))
        self.unchanged_import_rejection(dict(payload, original=[{"id": "AC1", "requirement": "weakened"}]))
        self.unchanged_import_rejection(dict(payload, repository=dict(payload["repository"], base_sha="bad")))
        self.unchanged_import_rejection(dict(payload, implementation_report="runs/FOS-000025/other/reports/old.json"))
        self.act("import-bootstrap", payload)
        self.assertEqual(git(self.source, "rev-parse", self.tip + "^").decode().strip(), legacy_tip)
        self.assertEqual(self.state["repository"]["input_sha"], self.base)
        self.assertEqual(self.state["provenance"][0]["legacy_state"], old)
        self.assertEqual(self.state["original"], self.original)
        self.assertEqual(self.state["attempts"], [])
        self.assertEqual(self.state["repository"]["candidate_sha"], self.base)
        self.assertEqual(self.state["limits"]["implementation_counts"][self.root_key], 1)
        self.assertEqual(self.state["verification"], [])
        self.assertIsNone(self.state["final_acceptance"])
        self.assertEqual(self.state["status"], "awaiting_decision")
        self.assertEqual(Store(self.source).records(self.tip)["runs/FOS-000025/run-1/reports/old.json"], records["runs/FOS-000025/run-1/reports/old.json"])
        started = int(git(self.source, "show", "-s", "--format=%ct", legacy_tip).decode())
        self.assertEqual(self.state["limits"]["deadline"], started + 4 * 3600)
        self.unchanged_rejection("reconcile", {"observation": "unknown", "terminated_workers": [], "next_action": "retry"}, "terminated")
        self.act("reconcile", {"observation": "runtime status confirms old worker done", "terminated_workers": ["old/worker"], "next_action": "dispatch fresh implementer"})
        self.assertEqual(self.state["status"], "ready")
        self.assertEqual(self.state["operation"], "subtask")

    def unchanged_import_rejection(self, payload):
        before = self.tip
        with self.assertRaises(Invalid):
            self.harness.apply("import-bootstrap", payload, self.tip, self.state["revision"])
        self.assertEqual(self.harness.store.tip(), before)


if __name__ == "__main__":
    unittest.main()
