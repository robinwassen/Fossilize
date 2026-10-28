"""Historical P01–P11 fixtures; live-schema assertion bodies execute in issue_artifacts."""
import copy
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import legacy_state as hs
from legacy_state import Invalid, Store, REF, POLICY, encoded, git, state_path
import test_harness_state as fixtures
from test_harness_state import Harness


class ArtifactTests(unittest.TestCase):
    # Fixture operations use production APIs and actual Git; no local-only switch.
    for _name, _method in vars(fixtures.HarnessTests).items():
        if callable(_method) and not _name.startswith("test_"):
            locals()[_name] = _method

    def remote_tip(self, bare=None):
        output = git(self.source, "ls-remote", str(bare or self.bare), REF).split()
        return output[0].decode() if output else None

    def assert_remote(self):
        self.assertEqual(self.remote_tip(), self.tip)
        local = self.harness.store.records(self.tip)
        for path, content in local.items():
            self.assertEqual(git(self.bare, "show", self.tip + ":" + path), content, path)
        return local

    def raw(self, action, payload=None):
        result = self.harness.apply(action, payload or {}, self.tip, self.state["revision"] if self.state else 0)
        self.tip, self.state = result["tip"], result["state"]
        return result

    def command(self):
        return {"command": "actual fixture verification", "cwd": str(self.source), "timeout": 1}

    def clone_writer(self):
        path = self.root / "writer"
        git(self.source, "clone", "--no-checkout", str(self.bare), str(path))
        git(path, "config", "user.name", "Other writer")
        git(path, "config", "user.email", "writer@example.invalid")
        git(path, "fetch", "origin", REF + ":" + REF)
        return path

    def writer_checkpoint(self, writer, parent, records):
        tip = Store(writer).checkpoint(records, parent, "Other writer checkpoint")
        git(writer, "push", "origin", tip + ":" + REF)
        return tip

    def reject_hook(self):
        hook = self.bare / "hooks/pre-receive"
        hook.write_text("#!/bin/sh\nexit 1\n")
        hook.chmod(0o755)
        return hook

    def test_P01_initial_orphan_exact_nonforce_destination_and_fresh_clone(self):
        self.assertIsNone(self.remote_tip())
        calls = []
        real_run = subprocess.run
        def observe(args, **kwargs):
            calls.append(args)
            return real_run(args, **kwargs)
        with patch.object(hs.subprocess, "run", side_effect=observe):
            result = self.initialize()
        self.assertEqual(result["artifact_publication"]["status"], "confirmed")
        policy = self.assert_remote()["allocations.json"]
        policy = json.loads(policy)[POLICY]
        self.assertEqual((policy["remote"], policy["repository"], policy["ref"]), ("origin", "example/project", REF))
        pushes = [c for c in calls if "push" in c]
        self.assertEqual(len(pushes), 1)
        self.assertEqual(pushes[0][-1], self.tip + ":" + REF)
        self.assertNotIn("--force", pushes[0])
        self.assertTrue(any("ls-remote" in c for c in calls[calls.index(pushes[0]) + 1:]))
        self.assertEqual(git(self.bare, "rev-parse", "master").decode().strip(), self.base)
        self.assertEqual(git(self.source, "rev-parse", "HEAD").decode().strip(), self.base)
        self.assertEqual(len(git(self.source, "rev-list", "--parents", self.tip).split()), 1)
        clone = self.clone_writer()
        self.assertEqual(Store(clone).records(self.tip), self.harness.store.records(self.tip))
        roots = git(clone, "rev-list", "--max-parents=0", REF).split()
        self.assertNotIn(self.base.encode(), roots)

    def test_P02_every_mutation_class_cumulative_remote_bytes(self):
        original_act = self.act
        actions = []
        def checked(action, payload=None):
            result = original_act(action, payload)
            self.assertEqual(result["artifact_publication"]["status"], "confirmed", result)
            self.assert_remote()
            actions.append(action)
            return result
        self.act = checked
        self.initialize()
        self.act("reserve", {"title": "Reserved fixture", "location": "local-docs/reserved.md"})
        self.launch("planner")
        attempt = self.state["attempts"][-1]["attempt_id"]
        self.act("reconcile", {"attempt_id": attempt, "liveness": "terminated", "observation": "Fixture worker terminated", "next_action": "fresh planner"})
        self.launch("planner")
        self.act("report", dict(self.report(), plan=self.plan()))
        self.implemented()
        self.checked()
        self.checked()
        self.act("publish-intent", self.intent())
        git(self.source, "push", "origin", "codex/test")
        self.act("publish-observation", {"event": "push", "remote_sha": self.state["repository"]["candidate_sha"]})
        self.act("publish-observation", self.lookup([]))
        self.act("publish-observation", {"event": "create-dispatched"})
        self.act("publish-observation", self.lookup([self.pr()]))
        result = self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": True})
        self.assertTrue(result["workflow_complete"])
        for action in ("initialize", "reserve", "dispatch", "acknowledge", "report", "reconcile", "closeout-intent", "closeout-record", "publish-intent", "publish-observation"):
            self.assertIn(action, actions)
        self.assertIsNone(json.loads(self.assert_remote()["active.json"]))
        self.assertTrue(any("/evidence/" in path for path in self.assert_remote()))

    def test_P03_explicit_existing_history_adoption_schema1_and_empty_root(self):
        # Genuine completed role/report/closeout lifecycle, followed by active run.
        self.publish_ready()
        self.act("publish-observation", self.lookup([self.pr()]))
        self.act("publish-observation", {"event": "attachment", "url": self.pr()["url"], "success": True})
        completed = self.harness.store.records(self.tip)
        self.state = None
        self.payload["run_id"] = "run-2"
        self.initialize()
        current = self.harness.store.records(self.tip)
        # Reconstruct a pre-policy fixture history, preserving actual old records.
        for rows in (completed, current):
            journal = json.loads(rows["allocations.json"])
            journal.pop(POLICY)
            rows["allocations.json"] = encoded(journal)
        git(self.source, "update-ref", "-d", REF)
        git(self.bare, "update-ref", "-d", REF)
        tree = git(self.source, "mktree", data=b"").decode().strip()
        empty = git(self.source, "commit-tree", tree, data=b"Historical empty orphan root\n").decode().strip()
        git(self.source, "update-ref", REF, empty)
        legacy = copy.deepcopy(completed)
        path = state_path(self.root_key, "run-1")
        old = json.loads(legacy[path]); old["schema_version"] = 1
        legacy[path] = encoded(old)
        legacy_journal = json.loads(legacy["allocations.json"])
        legacy_journal["high_water"] = 25
        legacy_journal["issued"] = [{"key": self.root_key, "location": "local-docs/backlog-keys.md"}]
        legacy["allocations.json"] = encoded(legacy_journal)
        first = Store(self.source).checkpoint(legacy, empty, "Historical schema 1")
        # Preserve the supported explicit bootstrap placeholder reconciliation.
        for rows in (completed, current):
            imported = json.loads(rows[path])
            imported["provenance"].append({"imported_from_tip": first, "legacy_schema": 1, "legacy_state": old})
            rows[path] = encoded(imported)
            journal = json.loads(rows["allocations.json"])
            journal["high_water"] = 25
            journal["issued"] = [{"key": self.root_key, "location": "local-docs/task.md", "title": "Root task"}]
            rows["allocations.json"] = encoded(journal)
        second = Store(self.source).checkpoint(completed, first, "Historical completed schema 2")
        self.tip = Store(self.source).checkpoint(current, second, "Active pre-policy schema 2")
        old_tip = self.tip
        state_before = copy.deepcopy(self.state)
        blocked = self.raw("command-intent", self.command())
        self.assertEqual(blocked["artifact_publication"]["outcome"], "policy-adoption-required")
        self.assertEqual(self.tip, old_tip)
        result = self.raw("adopt-artifacts", {"reviewed_tip": old_tip, "review_summary": "All unique synthetic historical records reviewed semantically; no secrets/transcripts."})
        self.assertEqual(result["artifact_publication"]["status"], "confirmed", result)
        self.assertEqual(self.state, state_before)
        self.assertIn(old_tip, git(self.bare, "rev-list", REF).decode())
        self.assertIn(first, git(self.bare, "rev-list", REF).decode())
        self.assertIn(empty, git(self.bare, "rev-list", REF).decode())
        for path, content in current.items():
            if path != "allocations.json":
                self.assertEqual(git(self.bare, "show", REF + ":" + path), content)
        self.assert_remote()
        # A later descendant fetched from a second clone validates all ancestors.
        writer = self.clone_writer()
        rows = Store(writer).records(self.tip)
        rows[f"runs/{self.root_key}/run-2/evidence/other.txt"] = b"Another writer observation\n"
        remote = self.writer_checkpoint(writer, self.tip, rows)
        result = self.raw("publish-artifacts")
        self.assertEqual(result["artifact_publication"].get("adopted_sha"), remote, result)
        self.assertEqual(self.tip, remote)

    def test_P04_stale_dirty_inspection_and_user_checkout_preserved(self):
        self.initialize()
        view = self.root / "inspection"
        git(self.source, "worktree", "add", "--detach", str(view), self.tip)
        self.harness.store.fail = "before-push"
        result = self.raw("command-intent", self.command())
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        (view / "active.json").write_text("unexpected tracked edit\n")
        (view / "private.txt").write_text("unexpected untracked bytes\n")
        (self.repo / "behavior.py").write_text("user partial work\n")
        before = {p: git(p, "status", "--porcelain").decode() for p in (view, self.repo)}
        index = git(view, "write-tree")
        self.harness = Harness(self.source, clock=lambda: self.now)
        result = self.raw("publish-artifacts")
        self.assertEqual(result["artifact_publication"]["status"], "confirmed")
        self.assertEqual(index, git(view, "write-tree"))
        for p in (view, self.repo):
            self.assertEqual(git(p, "status", "--porcelain").decode(), before[p])
        self.assertEqual((view / "active.json").read_text(), "unexpected tracked edit\n")
        self.assertEqual((view / "private.txt").read_text(), "unexpected untracked bytes\n")
        self.assertEqual((self.repo / "behavior.py").read_text(), "user partial work\n")
        self.assertNotIn("private.txt", self.assert_remote())
        for path in self.assert_remote():
            hs.record_path(path)

    def test_P05_idempotent_status_retry_and_ancestor_pending(self):
        self.initialize()
        tip, revision = self.tip, self.state["revision"]
        for _ in range(2):
            self.harness = Harness(self.source, clock=lambda: self.now)
            result = self.raw("publish-artifacts")
            self.assertEqual(result["artifact_publication"]["outcome"], "already-published")
            self.assertEqual((self.tip, self.state["revision"]), (tip, revision))
            self.assertEqual(self.harness.show()["artifact_publication"]["status"], "confirmed")
        self.harness.store.fail = "before-push"
        self.raw("reserve", {"title": "Reservation", "location": "local-docs/future.md"})
        self.assertNotEqual(self.tip, tip)
        self.assertEqual(self.remote_tip(), tip)
        self.assertEqual(self.harness.show()["artifact_publication"]["outcome"], "local-checkpoint-pending")
        pending = self.tip
        self.harness.store.fail = None
        self.raw("publish-artifacts")
        self.assertEqual(self.tip, pending)
        self.assertEqual(len(json.loads(self.assert_remote()["allocations.json"])["issued"]), 1)

    def test_P06_rejection_network_failure_barrier_safety_report_and_recovery(self):
        self.initialize()
        self.launch("planner")
        remote_before = self.tip
        hook = self.reject_hook()
        report = dict(self.report(), plan=self.plan(), records={f"runs/{self.root_key}/run-1/evidence/safe.log": "Actual sanitized fixture evidence\n"})
        result = self.raw("report", report)
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertEqual(result["artifact_publication"]["checkpoint_sha"], self.tip)
        self.assertEqual(result["artifact_publication"]["destination"]["ref"], REF)
        self.assertEqual(result["artifact_publication"]["retry"]["action"], "publish-artifacts")
        pending, before = self.tip, copy.deepcopy(self.state)
        blocked = self.raw("reserve", {"title": "Must not allocate", "location": "x"})
        self.assertEqual(blocked["artifact_publication"]["status"], "incomplete")
        self.assertEqual((self.tip, self.state), (pending, before))
        self.assertEqual(self.remote_tip(), remote_before)
        self.assertIn(report["attempt_id"] + ".json", self.state["attempts"][-1]["report"])
        hook.unlink()
        self.harness = Harness(self.source, clock=lambda: self.now)
        self.assertEqual(self.raw("publish-artifacts")["artifact_publication"]["status"], "confirmed")
        self.assertEqual((self.tip, self.state), (pending, before))
        moved = self.root / "temporarily-offline.git"
        self.bare.rename(moved)
        try:
            result = self.raw("command-intent", self.command())
            self.assertEqual(result["artifact_publication"]["status"], "incomplete")
            self.assertEqual(self.tip, pending)  # Preflight failed before forward mutation.
        finally:
            moved.rename(self.bare)
        self.assertEqual(self.raw("publish-artifacts")["artifact_publication"]["status"], "confirmed")

    def test_P07_interruption_before_push_and_after_success_without_return(self):
        self.initialize()
        for seam in ("before-push", "after-push"):
            with self.subTest(seam=seam):
                old = self.tip
                self.harness.store.fail = seam
                result = self.raw("command-intent", self.command())
                self.assertEqual(result["artifact_publication"]["status"], "incomplete")
                checkpoint, state = self.tip, copy.deepcopy(self.state)
                self.assertNotEqual(checkpoint, old)
                self.assertEqual(self.remote_tip(), checkpoint if seam == "after-push" else old)
                self.harness = Harness(self.source, clock=lambda: self.now)
                result = self.raw("publish-artifacts")
                self.assertEqual(result["artifact_publication"]["status"], "confirmed")
                self.assertEqual((self.tip, self.state), (checkpoint, state))
                self.assertEqual(len(self.state["attempts"]), 0)
                self.assert_remote()

    def test_P08_second_clone_descendant_adoption_then_divergent_allocations(self):
        self.initialize()
        writer = self.clone_writer()
        parent = self.tip
        rows = Store(writer).records(parent)
        journal = json.loads(rows["allocations.json"])
        journal.update(high_water=26, issued=[{"key": "FOS-000026", "title": "Other reservation", "location": "local-docs/other.md"}])
        rows["allocations.json"] = encoded(journal)
        remote = self.writer_checkpoint(writer, parent, rows)
        # show is read-only, including refs; adoption is a locked explicit action.
        self.assertEqual(self.harness.show()["artifact_publication"]["outcome"], "remote-advance-pending")
        self.assertEqual(Store(self.source).tip(), parent)
        result = self.raw("publish-artifacts")
        self.assertEqual(result["artifact_publication"]["outcome"], "remote-advance-adopted", result)
        self.assertEqual(self.tip, remote)
        self.raw("publish-artifacts")
        self.harness.store.fail = "before-push"
        self.raw("reserve", {"title": "Local reservation", "location": "local-docs/local.md"})
        local = self.tip
        rows = Store(writer).records(remote)
        journal = json.loads(rows["allocations.json"])
        journal["high_water"] = 27
        journal["issued"].append({"key": "FOS-000027", "title": "Concurrent reservation", "location": "local-docs/concurrent.md"})
        rows["allocations.json"] = encoded(journal)
        other = self.writer_checkpoint(writer, remote, rows)
        self.harness.store.fail = None
        result = self.raw("publish-artifacts")
        self.assertIn("divergent", result["artifact_publication"]["detail"])
        self.assertEqual((self.tip, self.remote_tip()), (local, other))
        self.assertEqual(git(self.source, "rev-parse", "refs/harness/observed/" + other).decode().strip(), other)
        blocked = self.raw("command-intent", self.command())
        self.assertEqual(blocked["artifact_publication"]["status"], "incomplete")
        self.assertEqual(self.tip, local)
        self.assertEqual(json.loads(Store(self.source).records(local)["allocations.json"])["issued"][-1]["title"], "Local reservation")
        self.assertEqual(json.loads(Store(writer).records(other)["allocations.json"])["issued"][-1]["title"], "Concurrent reservation")

    def raw_commit(self, rows, parent, mode="100644", extra_parent=None):
        import tempfile
        with tempfile.TemporaryDirectory(prefix="bad-remote-index-") as temporary:
            env = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / "index"))
            git(self.source, "read-tree", "--empty", env=env)
            for path, content in rows.items():
                blob = git(self.source, "hash-object", "-w", "--stdin", data=content).decode().strip()
                git(self.source, "update-index", "--add", "--cacheinfo", mode if path == "evil.txt" else "100644", blob, path, env=env)
            tree = git(self.source, "write-tree", env=env).decode().strip()
            args = ["commit-tree", tree, "-p", parent]
            if extra_parent:
                args += ["-p", extra_parent]
            return git(self.source, *args, data=b"Adversarial remote fixture\n").decode().strip()

    def test_P08c_historical_allocations_survive_every_parent_edge(self):
        self.initialize()
        parent = self.tip
        writer = self.clone_writer()
        original = Store(writer).records(parent)
        rows = dict(original)
        journal = json.loads(rows["allocations.json"])
        journal.update(high_water=27, issued=[{
            "key": "FOS-000026", "title": "Permanent remote reservation",
            "location": "local-docs/reserved.md"}])
        rows["allocations.json"] = encoded(journal)
        issued = self.writer_checkpoint(writer, parent, rows)
        git(self.source, "fetch", "origin", REF)
        mutations = {
            "removed reservation": dict(journal, issued=[]),
            "rewritten reservation": dict(journal, issued=[dict(journal["issued"][0], title="Reused meaning")]),
            "high-water regression": dict(journal, high_water=26),
        }
        for name, changed in mutations.items():
            with self.subTest(case=name):
                bad_rows = dict(rows, **{"allocations.json": encoded(changed)})
                # The issue is historical: tip-vs-local comparison alone accepts all three.
                bad = self.raw_commit(bad_rows, issued)
                git(self.source, "push", "origin", bad + ":" + REF)
                result = self.raw("publish-artifacts")
                self.assertEqual(result["artifact_publication"]["status"], "incomplete")
                self.assertEqual((self.tip, self.remote_tip()), (parent, bad))
                self.assertIn("ancestor drops issued allocations", result["artifact_publication"]["detail"])
                self.assertEqual(git(self.source, "rev-parse", "refs/harness/observed/" + bad).decode().strip(), bad)
                self.raw("reserve", {"title": "Must remain blocked", "location": "local-docs/blocked.md"})
                self.assertEqual(self.tip, parent)
                self.assertEqual(json.loads(Store(writer).records(issued)["allocations.json"]), journal)
                git(self.bare, "update-ref", REF, issued, bad)  # Disposable fixture restoration.
        # A merge must retain allocations from its second parent too.
        merged_bad = self.raw_commit(original, parent, extra_parent=issued)
        git(self.source, "push", "origin", merged_bad + ":" + REF)
        result = self.raw("publish-artifacts")
        self.assertEqual((self.tip, self.remote_tip()), (parent, merged_bad))
        self.assertIn("ancestor drops issued allocations", result["artifact_publication"]["detail"])
        # The same guard applies before publishing a locally constructed bad history.
        git(self.bare, "update-ref", REF, parent, merged_bad)
        git(self.source, "update-ref", REF, merged_bad, parent)
        self.tip = merged_bad
        result = self.raw("publish-artifacts")
        self.assertEqual(self.remote_tip(), parent)
        self.assertIn("ancestor drops issued allocations", result["artifact_publication"]["detail"])
        git(self.source, "update-ref", REF, parent, merged_bad)
        self.tip = parent
        # A compatible merge preserving both parents' journal remains adoptable.
        merged_good = self.raw_commit(rows, parent, extra_parent=issued)
        git(self.source, "push", "origin", merged_good + ":" + REF)
        result = self.raw("publish-artifacts")
        self.assertEqual(result["artifact_publication"]["outcome"], "remote-advance-adopted")
        self.assertEqual(self.tip, merged_good)
        self.assertEqual(self.raw("publish-artifacts")["artifact_publication"]["status"], "confirmed")
        self.raw("reserve", {"title": "Next permanent reservation", "location": "local-docs/next.md"})
        final = json.loads(self.assert_remote()["allocations.json"])
        self.assertEqual(final["issued"][:-1], journal["issued"])
        self.assertEqual(final["issued"][-1]["key"], "FOS-000028")

    def test_P09_invalid_remote_history_destination_and_upstream_preservation(self):
        self.initialize()
        parent = self.tip
        original = self.harness.store.records(parent)
        cases = []
        def row(name, path, value, **kwargs):
            rows = copy.deepcopy(original); rows[path] = value
            cases.append((name, rows, kwargs))
        row("unexpected path", "source.cpp", b"source bytes\n")
        row("symlink", "evil.txt", b"target", mode="120000")
        row("binary", f"runs/{self.root_key}/run-1/evidence/binary.log", b"bad\0bytes")
        row("oversize", f"runs/{self.root_key}/run-1/evidence/huge.log", b"x" * (hs.MAX_RECORD + 1))
        row("malformed JSON", "active.json", b"{")
        row("wrong pointer", "active.json", encoded({"task_key": self.root_key, "run_id": "wrong", "state": state_path(self.root_key, "run-1")}))
        row("missing active state", "active.json", encoded({"task_key": self.root_key, "run_id": "absent", "state": state_path(self.root_key, "absent")}))
        wrong_repository = json.loads(original[state_path(self.root_key, "run-1")])
        wrong_repository["repository"]["target_repository"] = "wrong/repository"
        row("wrong run repository", state_path(self.root_key, "run-1"), encoded(wrong_repository))
        row("source ancestry", "active.json", original["active.json"], extra_parent=self.base)
        for name, rows, kwargs in cases:
            with self.subTest(case=name):
                bad = self.raw_commit(rows, parent, **kwargs)
                git(self.source, "push", "origin", bad + ":" + REF)
                result = self.raw("publish-artifacts")
                self.assertEqual(result["artifact_publication"]["status"], "incomplete", result)
                self.assertEqual((self.tip, self.remote_tip()), (parent, bad))
                blocked = self.raw("reserve", {"title": "blocked", "location": "x"})
                self.assertEqual(self.tip, parent)
                self.assertEqual(blocked["artifact_publication"]["status"], "incomplete")
                git(self.bare, "update-ref", REF, parent, bad)  # Fixture restoration only.
        sentinel = self.root / "upstream.git"
        git(self.source, "init", "--bare", str(sentinel))
        git(self.source, "remote", "add", "upstream", str(sentinel))
        before = git(sentinel, "show-ref", "--head") if self.remote_tip(sentinel) else b""
        git(self.source, "remote", "set-url", "origin", str(sentinel))
        result = self.raw("publish-artifacts")
        self.assertIn("endpoint changed", result["artifact_publication"]["detail"])
        self.assertIsNone(self.remote_tip(sentinel))
        git(self.source, "remote", "set-url", "origin", str(self.bare))
        policy = json.loads(original["allocations.json"])[POLICY]
        for change in ({"remote": "upstream"}, {"ref": "refs/heads/master"}, {"repository": "wrong/repository"}):
            with self.assertRaises(Invalid):
                self.harness.validate_artifact_policy(dict(policy, **change))
        self.assertEqual(before, b"")
        self.assertEqual(self.raw("publish-artifacts")["artifact_publication"]["status"], "confirmed")

    def test_P09b_pushurl_observation_uses_bound_push_endpoint(self):
        push_bare = self.root / "push.git"
        git(self.source, "init", "--bare", str(push_bare))
        git(self.source, "remote", "set-url", "--push", "origin", str(push_bare))
        result = self.initialize()
        self.assertEqual(result["artifact_publication"]["status"], "confirmed", result)
        self.assertIsNone(self.remote_tip(self.bare))
        self.assertEqual(self.remote_tip(push_bare), self.tip)
        self.assertEqual(self.harness.show()["artifact_publication"]["remote_sha"], self.tip)
        # Changing only push endpoint is independently fenced by recorded hash.
        git(self.source, "remote", "set-url", "--push", "origin", str(self.bare))
        result = self.raw("publish-artifacts")
        self.assertIn("endpoint changed", result["artifact_publication"]["detail"])
        self.assertEqual(self.remote_tip(push_bare), self.tip)
        self.assertIsNone(self.remote_tip(self.bare))

    def test_P10_actual_postpush_advance_beats_success_and_caller_sha(self):
        self.initialize()
        writer = self.clone_writer()
        real_run = subprocess.run
        raced = []
        def advance_after_push(args, **kwargs):
            completed = real_run(args, **kwargs)
            if len(args) > 4 and args[1:3] == ["-C", str(self.source)] and args[3] == "push" and not raced and completed.returncode == 0:
                checkpoint = args[-1].split(":", 1)[0]
                raced.append(checkpoint)
                git(writer, "fetch", "origin", REF + ":" + REF)
                rows = Store(writer).records(checkpoint)
                rows[f"runs/{self.root_key}/run-1/evidence/race.txt"] = b"Competing advance after push success\n"
                raced.append(self.writer_checkpoint(writer, checkpoint, rows))
            return completed
        with patch.object(hs.subprocess, "run", side_effect=advance_after_push):
            result = self.raw("command-intent", self.command())
        self.assertEqual(len(raced), 2)
        self.assertEqual(self.tip, raced[0])
        self.assertEqual(self.remote_tip(), raced[1])
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertEqual(result["artifact_publication"]["remote_sha"], raced[1])
        self.assertIn("changed after push", result["artifact_publication"]["detail"])
        result = self.raw("publish-artifacts", {"remote_sha": raced[0]})
        self.assertEqual(result["artifact_publication"]["outcome"], "remote-advance-adopted")
        self.assertEqual(self.tip, raced[1])
        self.assertEqual(self.raw("publish-artifacts", {"remote_sha": "0" * 40})["artifact_publication"]["status"], "confirmed")
        self.assert_remote()

    def test_P11_terminal_null_active_failure_restart_and_finite_completion(self):
        self.publish_ready()
        self.act("publish-observation", self.lookup([]))
        self.act("publish-observation", {"event": "create-dispatched"})
        self.act("publish-observation", self.lookup([self.pr()]))
        url = self.pr()["url"]
        accepted = copy.deepcopy(self.state["final_acceptance"])
        before = self.tip
        hook = self.reject_hook()
        result = self.raw("publish-observation", {"event": "attachment", "url": url, "success": True})
        terminal = self.tip
        self.assertNotEqual(terminal, before)
        self.assertEqual(self.remote_tip(), before)
        self.assertEqual(self.state["phase"], "complete")
        self.assertEqual(self.state["final_acceptance"], accepted)
        self.assertFalse(result["workflow_complete"])
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertEqual(result["artifact_publication"]["retry"]["expected_revision"], 0)
        rows = Store(self.source).records(terminal)
        self.assertIsNone(json.loads(rows["active.json"]))
        state_bytes = rows[state_path(self.root_key, "run-1")]
        self.harness = Harness(self.source, clock=lambda: self.now)
        observed = self.harness.show()
        self.assertIsNone(observed["active"])
        self.assertFalse(observed["workflow_complete"])
        self.state = None
        blocked = self.raw("initialize", dict(self.payload, run_id="next-run"))
        self.assertFalse(blocked["workflow_complete"])
        self.assertEqual(self.tip, terminal)
        hook.unlink()
        result = self.raw("publish-artifacts")
        self.assertTrue(result["workflow_complete"])
        self.assertEqual(self.tip, terminal)
        for _ in range(2):
            self.assertTrue(self.harness.show()["workflow_complete"])
            self.assertTrue(self.raw("publish-artifacts")["workflow_complete"])
            self.assertEqual(self.tip, terminal)
        clone = self.clone_writer()
        remote_rows = Store(clone).records(terminal)
        self.assertIsNone(json.loads(remote_rows["active.json"]))
        self.assertEqual(remote_rows[state_path(self.root_key, "run-1")], state_bytes)
        final = json.loads(state_bytes)
        self.assertEqual(final["publishing"]["pr"]["url"], url)
        self.assertEqual(sum(e["event"] == "create-dispatched" for e in final["publishing"]["observations"]), 1)
        self.assertEqual(git(self.source, "rev-parse", terminal + "^").decode().strip(), before)

    def test_P06b_safety_facts_retained_behind_pending_publication(self):
        self.initialize()
        self.harness.store.fail = "before-push"
        model = {"model": "gpt-6.1-sol", "effort": "high"}
        result = self.raw("dispatch", {"role": "planner", "attempt_id": "pending-planner", "requested": model,
                                       "actual": model, "catalog": {"gpt-6.1-sol": ["high"]}})
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        dispatched = self.tip
        result = self.raw("acknowledge", {"attempt_id": "pending-planner", "worker_id": "fixture-worker"})
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertNotEqual(self.tip, dispatched)
        result = self.raw("report", dict(self.report(), plan=self.plan()))
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        pending, state = self.tip, copy.deepcopy(self.state)
        self.assertEqual(self.state["attempts"][-1]["status"], "complete")
        self.assertNotEqual(self.remote_tip(), pending)
        result = self.raw("dispatch", {"role": "implementer"})
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertEqual((self.tip, self.state), (pending, state))
        self.harness = Harness(self.source, clock=lambda: self.now)
        self.assertEqual(self.raw("publish-artifacts")["artifact_publication"]["status"], "confirmed")
        self.assertEqual((self.tip, self.state), (pending, state))
        self.assert_remote()

    def test_P08b_evolved_remote_worker_budget_state_requires_reconciliation(self):
        self.initialize()
        parent = self.tip
        writer = self.clone_writer()
        rows = Store(writer).records(parent)
        path = state_path(self.root_key, "run-1")
        changed = json.loads(rows[path])
        changed["revision"] += 1
        changed["limits"]["deadline"] += 1000
        rows[path] = encoded(changed)
        remote = self.writer_checkpoint(writer, parent, rows)
        result = self.raw("publish-artifacts")
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertIn("run state changed", result["artifact_publication"]["detail"])
        self.assertEqual((self.tip, self.remote_tip()), (parent, remote))
        self.assertEqual(self.state["limits"]["deadline"], self.payload.get("limits", {}).get("deadline", self.now + 4 * 3600))

    def test_P09c_invalid_ancestor_and_invalid_local_checkpoint_cannot_publish(self):
        self.initialize()
        parent = self.tip
        original = self.harness.store.records(parent)
        malformed = dict(original, **{"active.json": b"{"})
        bad = self.raw_commit(malformed, parent)
        apparently_valid = self.raw_commit(original, bad)
        git(self.source, "push", "origin", apparently_valid + ":" + REF)
        result = self.raw("publish-artifacts")
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertEqual((self.tip, self.remote_tip()), (parent, apparently_valid))
        git(self.bare, "update-ref", REF, parent, apparently_valid)
        # A manually installed local records ref gets the same semantic checks.
        local_rows = dict(original)
        invalid_state = json.loads(local_rows[state_path(self.root_key, "run-1")])
        invalid_state["schema_version"] = 99
        local_rows[state_path(self.root_key, "run-1")] = encoded(invalid_state)
        local_bad = self.raw_commit(local_rows, parent)
        git(self.source, "update-ref", REF, local_bad, parent)
        result = Harness(self.source, clock=lambda: self.now).apply("publish-artifacts", {}, local_bad, self.state["revision"])
        self.assertEqual(result["artifact_publication"]["status"], "incomplete")
        self.assertEqual(self.remote_tip(), parent)

    def test_P09d_concurrent_endpoint_reread_cannot_retarget_publication(self):
        self.initialize()
        sentinel = self.root / "retarget.git"
        git(self.source, "init", "--bare", str(sentinel))
        original_git = hs.git
        changed = []
        def retarget_on_reread(repo, *args, **kwargs):
            if args == ("remote", "get-url", "--push", "origin") and not changed:
                original_git(self.source, "remote", "set-url", "origin", str(sentinel))
                changed.append(True)
            return original_git(repo, *args, **kwargs)
        try:
            with patch.object(hs, "git", side_effect=retarget_on_reread):
                result = self.raw("publish-artifacts")
            self.assertTrue(changed)
            self.assertEqual(result["artifact_publication"]["status"], "incomplete")
            self.assertIn("changed during binding", result["artifact_publication"]["detail"])
            self.assertIsNone(self.remote_tip(sentinel))
            self.assertEqual(self.remote_tip(self.bare), self.tip)
        finally:
            original_git(self.source, "remote", "set-url", "origin", str(self.bare))


if __name__ == "__main__":
    unittest.main()
