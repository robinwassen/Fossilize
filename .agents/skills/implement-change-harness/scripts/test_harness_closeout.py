"""Real source/Git lifecycle regressions for canonical completion (C01–C08)."""
import copy
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path
from harness_state import Harness, Invalid, Store, encoded, git
import test_harness_state as fixtures


class CloseoutTests(unittest.TestCase):
    # Reuse fixture mechanics without inheriting/rerunning its test methods.
    for _name, _method in vars(fixtures.HarnessTests).items():
        if callable(_method) and not _name.startswith("test_"):
            locals()[_name] = _method

    automatic_closeout = False

    def accepted_implementation(self):
        self.planned()
        self.implemented()
        self.launch("checker")
        self.act("report", self.check_report())
        self.assertEqual(self.state["phase"], "closing")

    def targets(self):
        return [{"key": self.root_key, "location": "local-docs/task.md", "evidence": "Unit assertion test passed under independent checker.", "reference": "https://github.com/example/project/compare/master...codex/test"}]

    def reopen(self):
        self.harness = Harness(self.source, clock=lambda: self.now)
        observed = self.harness.show()
        self.assertEqual(observed["tip"], self.tip)
        self.state = observed["state"]
        self.assertIsNotNone(observed["active"])

    def commit_done(self):
        target = self.state["closeout"]["targets"][0]
        path = self.source / target["location"]
        path.write_text(path.read_text().replace("**Status:** Pending implementation.", "**Status:** Done (verified proposed branch state).") + "**Completion evidence:** " + target["evidence"] + " " + target["reference"] + "\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "canonical closeout")
        return git(self.source, "rev-parse", "HEAD").decode().strip()

    def test_C01_C05_intent_record_restart_and_exact_renewal(self):
        self.accepted_implementation()
        old = self.state["repository"]["candidate_sha"]
        self.reopen()
        self.assertIn("closeout-intent", self.state["next_action"])
        self.unchanged_rejection("publish-intent", self.intent())
        self.unchanged_rejection("dispatch", {"role": "checker", "attempt_id": "premature"})
        self.act("closeout-intent", {"targets": self.targets()})
        self.reopen()
        self.assertEqual(self.state["closeout"]["stage"], "intent")
        self.unchanged_rejection("closeout-record", {"candidate_sha": old})
        candidate = self.commit_done()
        self.reopen()
        self.assertEqual(self.state["closeout"]["stage"], "intent")
        self.act("closeout-record", {"candidate_sha": candidate})
        self.assertNotEqual(candidate, old)
        self.assertTrue(all(u["accepted"] is None for u in self.state["units"]))
        self.assertIn("Done", git(self.source, "show", candidate + ":local-docs/task.md").decode())
        self.unchanged_rejection("publish-intent", self.intent())
        self.checked()
        self.assertEqual(self.state["operation"], "final")
        self.checked()
        self.assertEqual(self.state["closeout"]["candidate_sha"], candidate)
        self.assertEqual(self.state["final_acceptance"]["candidate_sha"], candidate)
        self.assertFalse(git(self.source, "status", "--porcelain").strip())

    def test_C02_split_canonical_children_and_registry_preserved(self):
        plan = self.split_plan()
        # Canonical source carries real obligations, not just heading/status stubs.
        for unit in plan["subtasks"]:
            path = self.source / "local-docs" / (unit["title"] + ".md")
            dependency = ", ".join(unit["dependencies"]) or "None"
            path.write_text(path.read_text() + "\n**Depends on:** " + dependency + "\n\n"
                            + "### Original requirement\n" + unit["definition"] + "\n\n"
                            + "### Acceptance criteria\n"
                            + ("- AC1: value() returns exactly 1.\n" if unit["title"] == "First"
                               else "- AC1: second_value() returns exactly 2 after First accepts.\n"))
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "canonical subtask criteria and dependencies")
        canonical = {path.name: path.read_text() for path in (self.source / "local-docs").glob("*.md")}
        self.assertIn("**Depends on:** " + plan["subtasks"][0]["key"], canonical["Second.md"])
        self.assertIn("AC1: value() returns exactly 1.", canonical["First.md"])
        self.launch("planner")
        self.act("report", dict(self.report(), plan=plan))
        registry = (self.source / "local-docs/backlog-keys.md").read_bytes()
        first, second = [u["key"] for u in self.state["units"]]
        self.implemented()
        self.launch("checker")
        self.act("report", self.check_report())
        self.assertEqual(self.state["units"][0]["status"], "accepted")
        self.assertEqual(self.state["units"][1]["status"], "ready")
        self.unchanged_rejection("closeout-intent", {"targets": self.targets()})
        self.assertNotIn("Done", (self.source / "local-docs/task.md").read_text())
        (self.source / "second.py").write_text("def second_value():\n    return 3\n")
        (self.source / "test_second.py").write_text("import unittest\nfrom second import second_value\nclass Second(unittest.TestCase):\n    def test_second(self):\n        self.assertEqual(second_value(), 2)\n")
        self.implemented()
        self.launch("checker")
        failed = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_second"], cwd=self.source, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"), capture_output=True, text=True, timeout=30)
        self.assertEqual(failed.returncode, 1, failed.stderr)
        self.assertIn("AssertionError: 3 != 2", failed.stderr)
        failed_log = f"runs/{self.root_key}/run-1/evidence/second-failed.log"
        self.act("report", dict(self.report(), verdict="changes_required", findings=[{
            "id": "second-failed", "status": "open", "route": "implementation",
            "affected_task": second, "evidence": "Actual test_second assertion fails: 3 != 2; see " + failed_log}],
            records={failed_log: failed.stdout + failed.stderr}, next_action="repair second unit"))
        self.assertEqual(self.state["units"][0]["status"], "accepted")
        self.assertNotEqual(self.state["units"][1]["status"], "accepted")
        self.unchanged_rejection("closeout-intent", {"targets": self.targets()})
        for name in ("task.md", "First.md", "Second.md"):
            self.assertEqual((self.source / "local-docs" / name).read_text(), canonical[name])
        (self.source / "second.py").write_text("def second_value():\n    return 2\n")
        self.implemented()
        self.launch("checker")
        self.act("resolve-finding", {"finding_id": "second-failed", "evidence": "Fresh second-unit assertion passes below."})
        result = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_second"], cwd=self.source, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"), capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = self.check_report(result)
        test_id = "test_second.Second.test_second"
        report.update(discovered=[test_id], executed=[test_id], test_mapping={"C2": {"location": "test_second.py", "tests": [test_id]}})
        report["evidence"][0]["command"][-1] = "test_second"
        self.act("report", report)
        # All three definitions are tracked; status-only closeout preserves their
        # titles, dependencies and criteria, and never rewrites allocations.
        original = canonical
        source_sha = self.state["repository"]["candidate_sha"]
        # A legacy artifact-only canonical location cannot be treated as a
        # tracked completion. Probe the real inventory, then restore this fixture.
        ledger = self.source / "local-docs/backlog-keys.md"
        ledger.write_text(ledger.read_text().replace("[task](First.md)", "harness-artifacts:First"))
        git(self.source, "rm", "local-docs/First.md")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "legacy artifact-only unit fixture")
        legacy_sha = git(self.source, "rev-parse", "HEAD").decode().strip()
        journal = json.loads(self.harness.store.records(self.tip)["allocations.json"])
        checkpoint = self.harness.show()
        with self.assertRaisesRegex(Invalid, "tracked canonical"):
            self.harness.closeout_sections(self.state, journal, legacy_sha, [{"key": first, "location": "harness-artifacts:First"}])
        self.assertEqual(self.harness.show(), checkpoint)
        git(self.source, "reset", "--hard", source_sha)
        self.closeout()
        self.assertEqual(self.state["operation"], "final")
        for name in ("task.md", "First.md", "Second.md"):
            text = (self.source / "local-docs" / name).read_text()
            self.assertIn("**Status:** Done", text)
            stripped = "".join(line for line in text.replace("**Status:** Done (verified proposed branch state; not landed on master).", "**Status:** Pending implementation.").splitlines(keepends=True) if not line.startswith("**Completion evidence:**"))
            self.assertEqual(stripped, original[name])
        self.assertEqual((self.source / "local-docs/backlog-keys.md").read_bytes(), registry)
        self.assertEqual([u["key"] for u in self.state["units"]], [first, second])
        self.launch("checker")
        report = self.check_report()
        report["test_mapping"]["C2"] = {"location": "test_second.py", "tests": ["test_second.Second.test_second"]}
        report["discovered"].append("test_second.Second.test_second")
        report["executed"].append("test_second.Second.test_second")
        # The second assertion ran above and again during renewal at this SHA;
        # run both anew for the final checker evidence.
        result = subprocess.run([sys.executable, "-m", "unittest", "-v", "test_behavior", "test_second"], cwd=self.source, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"), capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        report["records"][report["evidence"][0]["log"]] = result.stdout + result.stderr
        report["evidence"][0]["command"].append("test_second")
        self.act("report", report)
        self.assertEqual(self.state["phase"], "publishing")

    def test_C03_bad_required_verification_never_authorizes_done(self):
        self.planned()
        self.implemented()
        self.launch("checker")
        for result in ("failed", "unsupported", "unavailable", "incomplete", "skipped", "not-run", None):
            with self.subTest(result=result):
                report = self.check_report()
                report["evidence"][0]["result"] = result
                self.unchanged_rejection("report", report)
                self.unchanged_rejection("closeout-intent", {"targets": self.targets()})
        report = self.check_report()
        report["executed"] = []
        self.unchanged_rejection("report", report)
        self.assertNotIn("Done", (self.source / "local-docs/task.md").read_text())

    def test_C04_rejection_fences_dispatch_until_committed_cleanup(self):
        for route in ("implementation", "planner", "environment"):
            with self.subTest(route=route):
                # Each route gets a wholly independent repository and run.
                case = CloseoutTests("test_C03_bad_required_verification_never_authorizes_done")
                case.setUp()
                self.addCleanup(case.doCleanups)
                case.accepted_implementation()
                case.closeout()
                case.launch("checker")
                case.act("report", dict(case.report(), verdict="changes_required", findings=[{"id": "reject", "status": "open", "route": route, "evidence": "Concrete independent failure after proposed Done."}], next_action="repair"))
                case.reopen()
                self.assertEqual(case.state["closeout"]["stage"], "cleanup")
                self.assertIn("closeout-cleanup", case.state["next_action"])
                case.unchanged_rejection("publish-intent", case.intent())
                if route == "environment":
                    case.act("resume", {"observation": "prerequisite restored", "next_action": "cleanup"})
                case.unchanged_rejection("dispatch", {"role": "implementer", "attempt_id": "blocked"}, "cleanup")
                case.unchanged_rejection("closeout-cleanup", {"candidate_sha": case.state["repository"]["candidate_sha"] , "observation": "Attempt cleanup without edits."}, "cleanup requires a new committed|stale Done")
                path = case.source / "local-docs/task.md"
                # Removing only the status is a real committed counterexample:
                # the rejected evidence must keep cleanup pending across restart.
                path.write_text(path.read_text().replace("**Status:** Done (verified proposed branch state; not landed on master).", "**Status:** Pending implementation."))
                git(case.source, "add", ".")
                git(case.source, "commit", "-m", "incomplete cleanup leaves invalidated evidence")
                stale = git(case.source, "rev-parse", "HEAD").decode().strip()
                self.assertIn("**Completion evidence:**", path.read_text())
                case.unchanged_rejection("closeout-cleanup", {"candidate_sha": stale, "observation": "Status removed but evidence remains."}, "stale completion evidence")
                case.reopen()
                self.assertEqual(case.state["closeout"]["stage"], "cleanup")
                path.write_text("".join(line for line in path.read_text().splitlines(keepends=True) if not line.startswith("**Completion evidence:**")))
                git(case.source, "add", ".")
                git(case.source, "commit", "-m", "remove invalidated completion")
                clean = git(case.source, "rev-parse", "HEAD").decode().strip()
                case.act("closeout-cleanup", {"candidate_sha": clean, "observation": "Stale Done removed from tracked source."})
                self.assertEqual(case.state["closeout"]["stage"], "pending")
                self.assertNotIn("**Completion evidence:**", path.read_text())
                if route == "planner":
                    case.launch("planner")
                    plan = case.plan()
                    plan["version"] = 2
                    case.act("report", dict(case.report(), plan=plan))
                case.implemented()
                case.launch("checker")
                case.act("resolve-finding", {"finding_id": "reject", "evidence": "Fresh assertion test and source review pass."})
                case.act("report", case.check_report())
                case.closeout()
                case.checked()
                self.assertEqual(case.state["phase"], "publishing")
                self.assertEqual(path.read_text().count("**Completion evidence:**"), 1)
                self.assertEqual(case.state["closeout"]["candidate_sha"], case.state["final_acceptance"]["candidate_sha"])

    def test_C05_C06_reject_unrelated_dirty_commit_and_checkpoint_fault(self):
        self.accepted_implementation()
        self.act("closeout-intent", {"targets": self.targets()})
        candidate = self.commit_done()
        path = self.source / "behavior.py"
        path.write_text(path.read_text() + "# unrelated change\n")
        self.unchanged_rejection("closeout-record", {"candidate_sha": candidate}, "dirty")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "unrelated")
        wrong = git(self.source, "rev-parse", "HEAD").decode().strip()
        self.unchanged_rejection("closeout-record", {"candidate_sha": wrong}, "direct bookkeeping")
        # Reset only this disposable source fixture, never a user checkout.
        git(self.source, "reset", "--hard", candidate)
        task = self.source / "local-docs/task.md"
        task.write_text(task.read_text() + "Acceptance text illegally rewritten.\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "--amend", "--no-edit")
        corrupt = git(self.source, "rev-parse", "HEAD").decode().strip()
        self.unchanged_rejection("closeout-record", {"candidate_sha": corrupt}, "original source/criteria")
        git(self.source, "reset", "--hard", candidate)
        original = self.harness
        self.harness = Harness(self.source, clock=lambda: self.now, fail="commit")
        before = original.show()
        with self.assertRaisesRegex(Invalid, "injected commit") :
            self.act("closeout-record", {"candidate_sha": candidate})
        self.assertEqual(original.show(), before)
        self.harness = original
        stale_tip, stale_revision = self.tip, self.state["revision"]
        self.act("closeout-record", {"candidate_sha": candidate})
        checkpoint = self.harness.show()
        with self.assertRaisesRegex(Invalid, "artifact tip changed"):
            self.harness.apply("closeout-record", {"candidate_sha": candidate}, stale_tip, stale_revision)
        self.assertEqual(self.harness.show(), checkpoint)
        self.checked()
        self.checked()
        changed = self.commit_candidate()
        self.act("observe-source", {"candidate_sha": changed})
        self.assertEqual(self.state["closeout"]["stage"], "cleanup")
        self.unchanged_rejection("publish-intent", self.intent())

    def test_C07_one_pr_contains_backlog_after_local_merge(self):
        self.publish_ready()
        candidate = self.state["repository"]["candidate_sha"]
        detached = self.root / "final-view"
        git(self.source, "worktree", "add", "--detach", str(detached), candidate)
        self.assertIn("Completion evidence", (detached / "local-docs/task.md").read_text())
        self.act("publish-observation", self.lookup([]))
        self.act("publish-observation", {"event": "create-dispatched"})
        self.reopen()  # Response lost; only lookup is safe.
        self.act("publish-observation", self.lookup([]))
        self.unchanged_rejection("publish-observation", {"event": "create-dispatched"}, "already dispatched")
        self.act("publish-observation", self.lookup([self.pr()]))
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": False})
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": True})
        self.assertEqual(self.harness.show()["active"], None)
        self.assertEqual(git(self.repo, "rev-parse", "master").decode().strip(), self.base)
        git(self.repo, "merge", "--no-ff", "codex/test", "-m", "user merges implementation")
        self.assertIn("Done", (self.repo / "local-docs/task.md").read_text())
        self.assertEqual(git(self.repo, "show", "master:local-docs/task.md"), git(self.source, "show", candidate + ":local-docs/task.md"))
        self.assertNotIn("active.json", git(self.repo, "ls-tree", "-r", "--name-only", "master").decode())
        observations = self.state["publishing"]["observations"]
        self.assertEqual(sum(o["event"] == "create-dispatched" for o in observations), 1)

    def test_C08_schema2_pending_adoption_preserves_facts_and_completed_record(self):
        # Obtain real historical acceptance, publication and retained reports by
        # completing the preceding fixture run, rather than cloning a planned run.
        self.publish_ready()
        self.act("publish-observation", self.lookup([]))
        self.act("publish-observation", {"event": "create-dispatched"})
        self.act("publish-observation", self.lookup([self.pr()]))
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": True})
        self.assertIsNone(self.harness.show()["active"])
        historical = "runs/FOS-000025/run-1/state.json"
        records = self.harness.store.records(self.tip)
        completed = json.loads(records[historical])
        completed.pop("closeout")  # Completed pre-closeout schema-2 record.
        self.assertIsNotNone(completed["final_acceptance"])
        self.assertEqual(completed["publishing"]["stage"], "done")
        records[historical] = encoded(completed)
        self.tip = self.harness.store.checkpoint(records, self.tip, "historical schema-2 record")
        historical_records = {k: v for k, v in records.items() if k.startswith("runs/FOS-000025/")}

        # Keep its canonical definition, registration and original completed bytes.
        task = self.source / "local-docs/task.md"
        git(self.source, "mv", "local-docs/task.md", "local-docs/historical.md")
        ledger = self.source / "local-docs/backlog-keys.md"
        ledger.write_text(ledger.read_text().replace("[task](task.md)", "[task](historical.md)")
                          + "| `FOS-000026` | Root task | [task](task.md) | Allocated |\n")
        task.write_text("# FOS-000026 — Root task\n\n**Status:** Pending implementation.\nOriginal criterion AC1: value() returns exactly 1.\n")
        git(self.source, "add", ".")
        git(self.source, "commit", "-m", "next root fixture")
        self.root_key = "FOS-000026"
        self.payload["task_key"] = self.root_key
        self.payload["repository"]["input_sha"] = git(self.source, "rev-parse", "HEAD").decode().strip()
        self.state = None  # New active root, existing completed history retained.
        self.accepted_implementation()
        records = self.harness.store.records(self.tip)
        path = f"runs/{self.root_key}/run-1/state.json"
        old = json.loads(records[path])
        old.pop("closeout")
        old.update(phase="checking", operation="final", next_action="dispatch separate final checker")
        records[path] = encoded(old)
        self.assertIsNotNone(old["units"][0]["accepted"])
        accepted = copy.deepcopy(old["units"][0]["accepted"])
        old_reports = {k: v for k, v in records.items() if "/reports/" in k or "/evidence/" in k}
        self.tip = self.harness.store.checkpoint(records, self.tip, "accepted legacy schema-2 fixture")
        self.reopen()
        self.assertEqual(self.state["closeout"], {"stage": "pending"})
        self.assertEqual({k: v for k, v in self.state.items() if k != "closeout"}, old)
        self.unchanged_rejection("publish-intent", self.intent())
        self.act("closeout-intent", {"targets": self.targets()})
        # Normalization persists only pending/intent bookkeeping: prior immutable
        # identity, original acceptance, workers, history, budget and reports survive.
        for key in ("task_key", "run_id", "original", "repository", "attempts", "verification", "plan", "limits", "provenance", "units"):
            self.assertEqual(self.state[key], old[key])
        self.assertEqual(self.state["closeout"]["acceptances"], [accepted])
        candidate = self.commit_done()
        self.act("closeout-record", {"candidate_sha": candidate})
        self.assertNotEqual(candidate, accepted["candidate_sha"])
        self.assertIsNone(self.state["units"][0]["accepted"])
        self.assertIsNone(self.state["final_acceptance"])
        self.assertEqual(self.state["closeout"]["acceptances"], [accepted])
        self.unchanged_rejection("publish-intent", self.intent())
        self.reopen()
        self.launch("checker")
        stale_report = json.loads(old_reports[old["attempts"][-1]["report"]])
        self.unchanged_rejection("report", stale_report, "stale")
        self.act("report", self.check_report())  # Actual renewed assertion command.
        self.assertEqual(self.state["operation"], "final")
        renewed = copy.deepcopy(self.state["units"][0]["accepted"])
        self.assertEqual(renewed["candidate_sha"], candidate)
        self.checked()  # Separate checker attempt, another actual assertion command.
        self.assertEqual(self.state["phase"], "publishing")
        self.assertEqual(self.state["final_acceptance"]["candidate_sha"], candidate)
        self.assertNotEqual(renewed["attempt_id"], self.state["final_acceptance"]["attempt_id"])
        self.assertEqual(self.state["closeout"]["stage"], "prepared")
        self.assertEqual(self.state["limits"], old["limits"])
        retained = self.harness.store.records(self.tip)
        for key, value in {**historical_records, **old_reports}.items():
            self.assertEqual(retained[key], value)



if __name__ == "__main__":
    unittest.main()
