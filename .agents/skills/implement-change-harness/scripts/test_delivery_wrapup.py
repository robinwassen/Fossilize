"""W01–W13 approved delivery contracts; every mutation stays in disposable repos."""
import copy
from contextlib import nullcontext, redirect_stdout, redirect_stderr
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import delivery_wrapup as dw
import harness_state as hs
from harness_state import Harness, Invalid, REF, Store, encoded, git
from test_harness_migration import LiveFixture
import test_harness_issue_closeout as closeout


# Independent inventory: adding an implementation mutation without its recovery
# coverage must fail this test. Observational stages have their applicable seams.
EXPECTED_MUTATIONS = {'approval', 'merge', 'merge-verification', 'detach', 'remote-delete',
                      'local-delete', 'ticket-close', 'ticket-comment', 'complete'}
EFFECT_STAGES = ('merge', 'merge-verification', 'detach', 'remote-delete', 'local-delete', 'ticket-close-8', 'ticket-comment-8')
EFFECT_PHASES = ('before-intent', 'before-dispatch', 'after-effect', 'after-returned', 'after-readback')
READ_STAGES = ('approval', 'complete')
# Explicit, test-owned successful-path inventory. Runtime capture must match it.
PUBLICATION_POINTS = (('approval', 'intent'),) + tuple(
    (stage, event) for stage in EFFECT_STAGES
    for event in ('intent', 'dispatch', 'returned', 'readback')) + (('complete', 'readback'),)
W11_SCENARIOS = tuple((stage, event, phase) for stage, event in PUBLICATION_POINTS
                      for phase in ('before-push', 'after-push'))


def scenario_id(case, *parts):
    return case + ':' + ':'.join(parts)


def scenario_inventory():
    result = {f'test_W11_publication_shard_{shard}': [scenario_id('W11', *point)
              for index, point in enumerate(W11_SCENARIOS) if index % 4 == shard] for shard in range(4)}
    result['test_W09_W10_every_effect_boundary_process_death_and_recovery'] = [
        scenario_id('W09', stage, phase) for stage in EFFECT_STAGES for phase in EFFECT_PHASES]
    result['test_W09_observational_and_terminal_boundaries'] = [
        scenario_id('W09', stage, phase) for stage in READ_STAGES for phase in ('before-intent', 'after-readback')]
    result['test_W14_known_unsent_dispatch_publication_recovers_exactly_once'] = [
        scenario_id('W14', stage, phase) for stage in ('merge', 'ticket-comment-8') for phase in ('before-push', 'after-push')]
    return result
SCRIPT = Path(__file__).with_name('harness_state.py').resolve()


class Interrupted(BaseException):
    """Simulated process death, outside workflow exception handling."""


class FastReadGitHub(dw.GitHub):
    """Same persistent fixture protocol; amortize interpreter startup for reads.

    Mutation requests still execute the real subprocess fake-gh entrypoint and
    its real Git squash. CLI happy-path and service-response-loss tests retain
    ordinary subprocess transport for reads too. No service state is cached.
    """
    def call(self, args, payload=None):
        if '--method' in args and args[args.index('--method') + 1] != 'GET':
            return super().call(args, payload)
        output, error = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(error), patch.object(sys, 'argv', ['gh', *args]):
            try:
                runpy.run_path(str(Path(__file__).with_name('fake_gh_fixture.py')), run_name='__main__')
            except SystemExit as result:
                code = result.code
        if code != 0:
            raise dw.IssueError('gh request failed; authentication/network/API result is not absence')
        try:return json.loads(output.getvalue())
        except ValueError as caught:raise dw.IssueError('malformed gh response') from caught


class DeliveryTests(LiveFixture, unittest.TestCase):
    prepare = closeout.IssueCloseoutTests.prepare
    actual_pr_evidence = closeout.IssueCloseoutTests.actual_pr_evidence

    def setUp(self):
        self.setup_live()
        # The retained accepted source worktree contains the actual runnable
        # command, and its candidate tree differs materially from target base.
        helper = self.source / '.agents/skills/implement-change-harness/scripts'
        helper.mkdir(parents=True)
        for filename in ('harness_state.py', 'github_issues.py', 'delivery_wrapup.py'):
            shutil.copy2(Path(__file__).with_name(filename), helper / filename)
        self.parent = self.create('Incomplete parent', 'Other required obligations remain open')
        self.gh.api(f"issues/{self.parent['number']}/sub_issues", 'POST', {'sub_issue_id': self.root_issue['id']})
        candidate, self.target = self.prepare()
        self.accepted()
        intent = {key: self.state['repository'][key] for key in ('target_remote', 'target_repository', 'target_branch', 'branch')}
        self.act('publish-intent', intent)
        git(self.source, 'push', 'origin', 'codex/test')
        self.act('publish-observation', {'event': 'push', 'remote_sha': candidate})
        self.pr_url = 'https://github.com/robinwassen/Fossilize/pull/99'
        pr = dict(intent, state='open', draft=False, url=self.pr_url)
        self.update(prs={'99': {'url': self.pr_url, 'state': 'OPEN', 'isDraft': False, 'baseRefName': 'master', 'headRefName': 'codex/test', 'headRefOid': candidate, 'body': 'Closes ' + self.root_issue['html_url'], 'closingIssuesReferences': [{'url': self.root_issue['html_url']}]}})
        self.act('publish-observation', {'event': 'lookup', 'all_states': True, 'unambiguous': True, 'matches': [pr]})
        self.actual_pr_evidence(pr)
        rest = {'html_url': self.pr_url, 'state': 'open', 'draft': False, 'merged': False, 'mergeable': True, 'mergeable_state': 'clean',
                'head': {'ref': 'codex/test', 'sha': candidate, 'repo': {'full_name': 'robinwassen/Fossilize'}},
                'base': {'ref': 'master', 'sha': self.base, 'repo': {'full_name': 'robinwassen/Fossilize'}}}
        self.update(rest_prs={'99': rest}, git_repo=str(self.source), git_bare=str(self.bare))
        self.act('publish-observation', {'event': 'attachment', 'url': self.pr_url, 'success': True})
        self.path = hs.state_path(self.state['task_key'], self.state['run_id']) if self.state else 'runs/issue-8/run-1/state.json'
        # All reports/acceptance above came through real supported transitions.
        # Begin this disposable delivery fixture at one prepared checkpoint, so
        # every fresh fault/retry need not revalidate fifty setup transitions.
        # W12 independently covers long history and hostile old ancestors; the
        # existing publication-history regressions are retained unchanged.
        prepared = self.harness.store.records(self.tip)
        old_tip = self.tip
        git(self.source, 'update-ref', '-d', REF, old_tip)
        self.tip = Store(self.source).checkpoint(prepared, None, 'Prepared independently accepted delivery fixture')
        git(self.source, 'push', '--force-with-lease=' + REF + ':' + old_tip, 'origin', self.tip + ':' + REF)
        self.fixture_tip = self.tip
        self.initial_records = self.harness.store.records(self.tip)
        self.assertEqual(self.initial_records, prepared)  # every record byte survives fixture setup
        self.initial = json.loads(self.initial_records[self.path])
        self.approval = {'approved': True, 'repository': 'robinwassen/Fossilize', 'pr': self.pr_url, 'head': candidate,
                         'root_issue': self.root_issue['html_url'], 'run_id': 'run-1', 'accepted_revision': self.initial['revision']}
        self.snapshot_dir = tempfile.TemporaryDirectory(prefix='delivery-baseline-')
        self.addCleanup(self.snapshot_dir.cleanup)
        shutil.copytree(self.root, Path(self.snapshot_dir.name) / 'repo')
        self.baseline_service = self.service_file.read_bytes()

    def restore(self):
        self.restore_case(Path(self.snapshot_dir.name))

    def save_case(self, directory):
        directory.mkdir(parents=True)
        shutil.copytree(self.root, directory / 'repo')
        (directory / 'service.json').write_bytes(self.service_file.read_bytes())

    def restore_case(self, directory):
        for path in self.root.iterdir():
            shutil.rmtree(path) if path.is_dir() else path.unlink()
        shutil.copytree(directory / 'repo', self.root, dirs_exist_ok=True)
        service = directory / 'service.json'
        self.service_file.write_bytes(service.read_bytes() if service.exists() else self.baseline_service)
        self.harness = Harness(self.source)
        self.tip = self.harness.store.tip()
        self.state = copy.deepcopy(self.initial)
        self.approval = dict(self.approval, approved=True, repository='robinwassen/Fossilize', pr=self.pr_url,
                             head=self.initial['repository']['candidate_sha'], root_issue=self.root_issue['html_url'],
                             run_id='run-1', accepted_revision=self.initial['revision'])

    def mutate_state(self, change):
        harness = Harness(self.source)
        tip = harness.store.tip()
        records = harness.store.records(tip)
        state = json.loads(records[self.path])
        change(state, records)
        records[self.path] = encoded(state)
        tip = harness.store.checkpoint(records, tip, 'Disposable counterexample')
        self.assertEqual(harness.artifact_sync(tip, records, push=True)['status'], 'confirmed')

    def run_delivery(self, approval=None, fault=None):
        # Fresh instances on every attempt; no process-local recovery knowledge.
        transport = patch.object(dw, 'GitHub', FastReadGitHub) if getattr(self, 'fast_reads', False) else nullcontext()
        with transport:
            return Harness(self.source).wrap_up(approval or self.approval, fault=fault)

    def remote(self, branch):
        return git(self.repo, 'ls-remote', '--refs', str(self.bare), 'refs/heads/' + branch).decode().strip()

    def mutations(self):
        calls = self.service().get('calls', [])
        return [call for call in calls if ('--method' in call and call[call.index('--method') + 1] in ('PUT', 'PATCH', 'POST'))]

    def assert_delivered(self, result):
        self.assertEqual(result['outcome'], 'delivered', result)
        self.assertTrue(result['delivered'])
        records = Store(self.source).records(result['checkpoint_sha'])
        self.assertIn(self.fixture_tip, git(self.source, 'rev-list', result['checkpoint_sha']).decode().splitlines())
        saved = json.loads(records[self.path])
        self.assertIsNone(json.loads(records['active.json']))
        self.assertEqual(saved['delivery']['stage'], 'delivered')
        self.assertEqual(saved['repository']['candidate_sha'], self.approval['head'])
        self.assertNotEqual(result['merge_sha'], self.approval['head'])
        self.assertEqual(git(self.source, 'rev-parse', result['merge_sha'] + '^{tree}'), git(self.source, 'rev-parse', self.approval['head'] + '^{tree}'))
        self.assertEqual(git(self.source, 'rev-list', '--parents', '-n', '1', result['merge_sha']).decode().split(), [result['merge_sha'], self.base])
        self.assertIn(result['merge_sha'], self.remote('master'))
        self.assertFalse(self.remote('codex/test'))
        self.assertFalse(git(self.repo, 'for-each-ref', '--format=%(refname)', 'refs/heads/codex/test').strip())
        self.assertTrue(self.source.exists())
        self.assertEqual(git(self.source, 'rev-parse', '--symbolic-full-name', 'HEAD').strip(), b'HEAD')
        issue = self.gh.issue(self.root_issue['number'])
        self.assertEqual((issue['state'], issue['state_reason']), ('closed', 'completed'))
        comments = [row for row in self.gh.comments(self.root_issue['number']) if '<!-- harness-delivery:' in row['body']]
        self.assertEqual(len(comments), 1)
        for fact in (self.pr_url, result['merge_sha'], self.approval['head'], self.target['evidence']):
            self.assertIn(fact, comments[0]['body'])
        self.assertEqual(self.gh.issue(self.parent['number'])['state'], 'open')
        self.assertEqual(self.service()['merge_count'], 1)
        self.assertEqual(result['artifact_publication']['status'], 'confirmed')
        self.assertIn(result['checkpoint_sha'], self.remote('harness-artifacts'))
        for path, content in self.initial_records.items():
            if path != self.path:
                self.assertEqual(records[path], content, path)
        # Only delivery bookkeeping may change in the selected completed run.
        expected = copy.deepcopy(self.initial)
        for key in ('revision', 'delivery', 'next_action'):
            saved.pop(key, None)
            expected.pop(key, None)
        saved['publishing'].pop('delivery')
        self.assertEqual(saved['publishing']['pr']['state'], 'merged')
        self.assertEqual(saved['publishing']['pr'].pop('merge_sha'), result['merge_sha'])
        saved['publishing']['pr']['state'] = expected['publishing']['pr']['state']
        self.assertEqual(saved, expected)

    def test_W01_W04_W08_W10_one_subprocess_command_real_squash_terminal_transition(self):
        command = dw.Delivery(Harness(self.source), self.approval).command()
        result = subprocess.run(__import__('shlex').split(command), text=True, capture_output=True, timeout=60,
                                env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(len(result.stdout.splitlines()), 1)
        delivered = json.loads(result.stdout)
        self.assert_delivered(delivered)
        tip = delivered['checkpoint_sha']
        again = self.run_delivery()
        self.assert_delivered(again)
        self.assertEqual(again['checkpoint_sha'], tip)
        self.assertEqual(again['resume_command'], command)

    def test_W01_W13_guidance_one_approval_handoff_and_supported_recovery(self):
        root = Path(__file__).resolve().parents[4]
        skill = (root / '.agents/skills/implement-change-harness/SKILL.md').read_text()
        for phrase in ('concise verification', 'approval already', 'Decline or defer', 'wrap-up', 'accepted head', 'one supported command'):
            self.assertIn(phrase, skill)
        for file in ('local-docs/issue-workflow.md', '.agents/skills/implement-change-harness/references/roles.md', '.agents/skills/implement-change-harness/references/state.md'):
            text = (root / file).read_text()
            self.assertNotIn('harness never merges', text)
            self.assertNotIn('helper never merges', text)
        state = (root / '.agents/skills/implement-change-harness/references/state.md').read_text()
        for phrase in ('--accepted-revision', 'decision-needed', 'incomplete', 'same command', 'active.json=null'):
            self.assertIn(phrase, state)

    def test_W02_approval_scope_and_acceptance_fences_zero_effects(self):
        cases = [('approved', False), ('repository', 'ValveSoftware/Fossilize'), ('pr', self.pr_url + '0'), ('head', 'f' * 40),
                 ('root_issue', self.parent['html_url']), ('run_id', 'wrong'), ('accepted_revision', 1)]
        for key, value in cases:
            with self.subTest(scope=key):
                self.restore()
                before = len(self.mutations())
                result = self.run_delivery(dict(self.approval, **{key: value}))
                self.assertEqual(result['outcome'], 'decision-needed', result)
                self.assertEqual(len(self.mutations()), before)
                self.assertNotIn('merge_count', self.service())
        changes = {
            'attachment': lambda s, r: s['publishing'].pop('attachment'),
            'final': lambda s, r: s.update(final_acceptance=None),
            'findings': lambda s, r: s['findings'].append({'id': 'unfinished', 'status': 'open', 'evidence': 'needs repair', 'route': 'implementation', 'affected_task': self.root_issue['html_url']}),
            'unit': lambda s, r: s['work_units'][0].update(status='ready'),
            'checks': lambda s, r: s['verification'][-1].update(evidence=[]),
            'report': lambda s, r: r.__setitem__(s['attempts'][-1]['report'], encoded(dict(json.loads(r[s['attempts'][-1]['report']]), evidence=[]))),
            'baseline': lambda s, r: s['publishing'].pop('required_checks'),
            'hosted-ci': lambda s, r: s['publishing'].update(ci_policy='required_after_pr', ci={'result':'passed','evidence':'old hosted observation'}),
        }
        for name, change in changes.items():
            with self.subTest(acceptance=name):
                self.restore()
                # The verification list alone is preserved evidence; mutate the
                # authoritative report for mandatory-gate rejection as well.
                if name == 'checks':
                    change = lambda s, r: s['plan']['gates'].append(dict(s['plan']['gates'][0], id='new-required-gate'))
                self.mutate_state(change)
                before = len(self.mutations())
                result = self.run_delivery()
                self.assertEqual(result['outcome'], 'decision-needed', result)
                self.assertEqual(len(self.mutations()), before)

    def test_W03_live_preflight_drift_and_merge_rejection(self):
        changes = {
            'head': lambda p: p['head'].update(sha='f' * 40),
            'base': lambda p: p['base'].update(sha='f' * 40),
            'source-repo': lambda p: p['head']['repo'].update(full_name='other/Fossilize'),
            'target-repo': lambda p: p['base']['repo'].update(full_name='ValveSoftware/Fossilize'),
            'draft': lambda p: p.update(draft=True),
            'closed': lambda p: p.update(state='closed'),
            'unknown': lambda p: p.update(mergeable=None),
            'conflict': lambda p: p.update(mergeable=False, mergeable_state='dirty'),
            'blocked': lambda p: p.update(mergeable_state='blocked'),
        }
        for name, change in changes.items():
            with self.subTest(pr=name):
                self.restore()
                data = self.service();change(data['rest_prs']['99']);self.service_file.write_text(json.dumps(data))
                result = self.run_delivery()
                self.assertEqual(result['outcome'], 'decision-needed', result)
                self.assertNotIn('merge_count', self.service())
        for bucket in ('fail', 'pending', 'pass'):
            with self.subTest(required=bucket):
                self.restore();self.update(required_checks=[{'name': 'new required check', 'state': 'SUCCESS', 'bucket': bucket, 'workflow': 'CI'}])
                self.assertEqual(self.run_delivery()['outcome'], 'decision-needed')
                self.assertNotIn('merge_count', self.service())
        for targets in ([], [{'url': self.parent['html_url']}]):
            with self.subTest(closing_targets=targets):
                self.restore()
                data = self.service();data['prs']['99']['closingIssuesReferences'] = targets
                data['prs']['99']['body'] = '\n'.join('Closes ' + row['url'] for row in targets)
                self.service_file.write_text(json.dumps(data))
                result = self.run_delivery()
                self.assertEqual(result['outcome'], 'decision-needed', result)
                self.assertNotIn('merge_count', self.service())
                self.assertEqual(self.gh.issue(self.parent['number'])['state'], 'open')
        self.restore()
        self.gh.api(f"issues/{self.root_issue['number']}", 'PATCH', {'title': 'Changed material requirement'})
        self.assertEqual(self.run_delivery()['outcome'], 'decision-needed')
        self.assertNotIn('merge_count', self.service())
        self.restore();self.update(reject_merge=True)
        result = self.run_delivery()
        self.assertEqual(result['outcome'], 'decision-needed', result)
        self.assertIn('rejected', result['summary'].lower())
        self.assertTrue(self.remote('codex/test'))
        self.assertEqual(self.gh.issue(self.root_issue['number'])['state'], 'open')
        again = self.run_delivery()
        self.assertNotEqual(again['outcome'], 'delivered')
        self.assertEqual(self.service()['merge_count'], 1)

    def test_W04_incompatible_squash_and_already_merged_reconcile(self):
        for mode in ('wrong-tree', 'merge', 'wrong-parent', 'off-target', 'wrong-response'):
            with self.subTest(mode=mode):
                self.restore();self.update(merge_mode=mode)
                result = self.run_delivery()
                self.assertEqual(result['outcome'], 'decision-needed', result)
                self.assertIsNotNone(result['merge_sha'])
                saved = json.loads(Store(self.source).records(Store(self.source).tip())[self.path])
                self.assertEqual(saved['delivery']['merge_sha'], result['merge_sha'])
                self.assertTrue(self.remote('codex/test'))
                self.assertEqual(self.gh.issue(self.root_issue['number'])['state'], 'open')
        self.restore()
        self.gh.api('pulls/99/merge', 'PUT', {'merge_method': 'squash', 'sha': self.approval['head']})
        self.assert_delivered(self.run_delivery())

    def test_W03_W04_post_merge_requirements_and_checks_drift_preserves_branch(self):
        for kind in ('requirements', 'checks'):
            with self.subTest(kind=kind):
                self.restore()
                def drift(stage, phase, delivery):
                    if (stage, phase) == ('merge-verification', 'after-readback'):
                        data = self.service()
                        if kind == 'requirements':data['issues']['8']['title'] = 'Changed after merge'
                        else:data['required_checks'] = [{'name':'new required gate','state':'FAILURE','bucket':'fail','workflow':'CI'}]
                        self.service_file.write_text(json.dumps(data))
                result = self.run_delivery(fault=drift)
                self.assertEqual(result['outcome'], 'decision-needed', result)
                self.assertIsNotNone(result['merge_sha'])
                self.assertTrue(self.remote('codex/test'))
                self.assertEqual(self.service()['merge_count'], 1)
                self.assertEqual(self.gh.issue(self.root_issue['number'])['state'], 'open')

    def test_W03_zero_total_zero_required_and_failed_lookup_are_distinct(self):
        self.assertEqual(dw.required_checks(self.gh, self.pr_url, self.approval['head']), [])
        optional = [{'name':'optional diagnostic','state':'FAILURE','bucket':'fail','workflow':'Extra'}]
        self.update(all_checks=optional)
        self.assertEqual(dw.required_checks(self.gh, self.pr_url, self.approval['head']), [])
        data = self.service();data['rest_prs']['99']['mergeable_state'] = 'unstable';self.service_file.write_text(json.dumps(data))
        self.assertEqual(self.run_delivery()['outcome'], 'decision-needed')
        self.assertNotIn('merge_count', self.service())
        self.restore();self.update(checks_lookup_failure=True)
        before = len(self.service()['calls'])
        with self.assertRaises(dw.IssueError):dw.required_checks(self.gh, self.pr_url, self.approval['head'])
        self.assertEqual(len(self.service()['calls']) - before, 1)  # no absence fallback on auth error
        self.restore();self.update(all_checks=optional, checks_empty_signal='no checks')
        with self.assertRaises(Invalid):dw.required_checks(self.gh, self.pr_url, self.approval['head'])
        self.restore()
        data = self.service();data['prs']['99']['headRefOid'] = 'e' * 40;self.service_file.write_text(json.dumps(data))
        with self.assertRaises(Invalid):dw.required_checks(self.gh, self.pr_url, self.approval['head'])

    def test_W05_W06_dirty_changed_occupied_and_unrelated_work_preserved(self):
        for kind in ('tracked', 'untracked', 'local-ref', 'remote-ref', 'occupied'):
            with self.subTest(hazard=kind):
                self.restore()
                if kind == 'tracked':(self.source / 'behavior.py').write_text('user edit\n')
                if kind == 'untracked':(self.source / 'untracked.txt').write_bytes(b'user content\0')
                if kind == 'local-ref':
                    git(self.source, 'checkout', '--detach', self.approval['head'])
                    git(self.source, 'update-ref', 'refs/heads/codex/test', self.base, self.approval['head'])
                if kind == 'remote-ref':git(self.source, 'push', '--force-with-lease=refs/heads/codex/test:' + self.approval['head'], 'origin', self.base + ':refs/heads/codex/test')
                if kind == 'occupied':
                    git(self.source, 'checkout', '--detach', self.approval['head'])
                    git(self.repo, 'worktree', 'add', str(self.root / 'unexpected'), 'codex/test')
                before = self.files(self.source)
                result = self.run_delivery()
                self.assertEqual(result['outcome'], 'decision-needed', result)
                self.assertEqual(self.files(self.source), before)
                self.assertNotIn('merge_count', self.service())
        self.restore()
        unrelated = self.root / 'unrelated'
        git(self.repo, 'worktree', 'add', '-b', 'user/other', str(unrelated), self.base)
        (unrelated / 'behavior.py').write_bytes(b'unrelated tracked edits\n')
        (unrelated / 'private.bin').write_bytes(b'private\0bytes')
        inspection = self.root / 'artifact-view'
        git(self.repo, 'worktree', 'add', '--detach', str(inspection), self.tip)
        (inspection / 'active.json').write_bytes(b'dirty inspection\n')
        (inspection / 'scratch.txt').write_text('keep')
        original = self.files(self.repo)
        other = self.files(unrelated)
        artifact = self.files(inspection)
        self.assert_delivered(self.run_delivery())
        self.assertEqual(self.files(self.repo), original)
        self.assertEqual(self.files(unrelated), other)
        self.assertEqual(self.files(inspection), artifact)

    @staticmethod
    def files(path):
        files = {str(p.relative_to(path)): p.read_bytes() for p in path.rglob('*') if p.is_file() and '.git' not in p.relative_to(path).parts}
        files[':HEAD'] = git(path, 'rev-parse', 'HEAD')
        index = Path(git(path, 'rev-parse', '--path-format=absolute', '--git-path', 'index').decode().strip())
        files[':index'] = index.read_bytes()
        return files

    def test_W05_conditional_delete_races_preserve_advanced_refs(self):
        for stage in ('remote-delete', 'local-delete'):
            with self.subTest(stage=stage):
                self.restore()
                def advance(current, boundary, delivery):
                    if (current, boundary) == (stage, 'before-dispatch'):
                        if stage == 'remote-delete':
                            git(self.source, 'push', '--force-with-lease=refs/heads/codex/test:' + self.approval['head'], 'origin', self.base + ':refs/heads/codex/test')
                        else:git(self.source, 'update-ref', 'refs/heads/codex/test', self.base, self.approval['head'])
                result = self.run_delivery(fault=advance)
                self.assertNotEqual(result['outcome'], 'delivered')
                if stage == 'remote-delete':self.assertIn(self.base, self.remote('codex/test'))
                else:self.assertEqual(git(self.repo, 'rev-parse', 'refs/heads/codex/test').decode().strip(), self.base)
                self.assertEqual(self.gh.issue(self.root_issue['number'])['state'], 'open')

    def test_W07_auto_closed_completed_cancelled_and_comment_conflicts(self):
        self.update(auto_close=[self.root_issue['number']])
        self.assert_delivered(self.run_delivery())
        close_calls = [call for call in self.service()['calls'] if '--method' in call and call[call.index('--method')+1] == 'PATCH']
        self.assertEqual(close_calls, [])
        self.restore()
        self.gh.api(f"issues/{self.root_issue['number']}", 'PATCH', {'state': 'closed', 'state_reason': 'completed'})
        self.assert_delivered(self.run_delivery())
        self.restore()
        self.gh.api(f"issues/{self.root_issue['number']}", 'PATCH', {'state': 'closed', 'state_reason': 'not_planned'})
        self.assertEqual(self.run_delivery()['outcome'], 'decision-needed')
        self.assertNotIn('merge_count', self.service())
        for duplicate in (False, True):
            self.restore()
            marker = '<!-- harness-delivery:' + dw.digest(self.approval)[:24] + ':' + str(self.root_issue['number']) + ' -->'
            for index in range(2 if duplicate else 1):
                self.gh.api(f"issues/{self.root_issue['number']}/comments", 'POST', {'body': 'Conflicting human change ' + marker})
            result = self.run_delivery()
            self.assertEqual(result['outcome'], 'decision-needed', result)
            self.assertNotEqual(json.loads(Store(self.source).records(Store(self.source).tip())[self.path])['delivery']['stage'], 'delivered')
        self.restore();self.update(tamper_comment_response=True)
        result = self.run_delivery()
        self.assertEqual(result['outcome'], 'decision-needed', result)
        self.assertIn('returned identity', result['summary'])

    def test_W08_another_active_run_is_preserved(self):
        def other(s, records):
            records['active.json'] = encoded({'task_key': 'other', 'run_id': 'other', 'state': 'runs/other/other/state.json'})
            saved = copy.deepcopy(s);saved.update(task_key='other', run_id='other', phase='planning', status='ready')
            records['runs/other/other/state.json'] = encoded(saved)
        self.mutate_state(other)
        tip = Store(self.source).tip()
        result = self.run_delivery()
        self.assertEqual(result['outcome'], 'decision-needed')
        self.assertEqual(Store(self.source).tip(), tip)
        self.assertNotIn('merge_count', self.service())

    def test_W09_W10_every_effect_boundary_process_death_and_recovery(self):
        self.fast_reads = True
        self.assertEqual(set(dw.MUTATIONS), EXPECTED_MUTATIONS)
        prefixes = tempfile.TemporaryDirectory(prefix='delivery-prefixes-')
        self.addCleanup(prefixes.cleanup)
        def prefix(stage, phase, delivery):
            if stage in EFFECT_STAGES and phase == 'before-intent':
                self.save_case(Path(prefixes.name) / stage)
        self.assert_delivered(self.run_delivery(fault=prefix))
        covered = set()
        for stage in EFFECT_STAGES:
            for boundary in EFFECT_PHASES:
                with self.subTest(stage=stage, boundary=boundary, scenario_id=scenario_id('W09', stage, boundary)):
                    self.restore_case(Path(prefixes.name) / stage)
                    hit = []
                    def death(current, phase, delivery):
                        if (current, phase) == (stage, boundary):
                            # Every mutation has a published exact intent before
                            # dispatch; read committed records, never memory.
                            if phase != 'before-intent':
                                remote = self.remote('harness-artifacts').split()[0]
                                self.assertEqual(remote, delivery.h.store.tip())
                            hit.append((current, phase));raise Interrupted()
                    with self.assertRaises(Interrupted):self.run_delivery(fault=death)
                    self.assertEqual(hit, [(stage, boundary)])
                    covered.add((stage, boundary))
                    result = self.run_delivery()
                    self.assert_delivered(result)
                    again = self.run_delivery()
                    self.assertEqual(again['outcome'], 'delivered', again)
                    self.assertEqual(again['checkpoint_sha'], result['checkpoint_sha'])
                    self.assertEqual(self.service()['merge_count'], 1)
        self.assertEqual(covered, {(s,p) for s in EFFECT_STAGES for p in EFFECT_PHASES})

    def test_W09_observational_and_terminal_boundaries(self):
        self.fast_reads = True
        covered = set()
        for stage in READ_STAGES:
            for boundary in ('before-intent', 'after-readback'):
                with self.subTest(stage=stage, boundary=boundary, scenario_id=scenario_id('W09', stage, boundary)):
                    self.restore()
                    def death(current, phase, delivery):
                        if (current, phase) == (stage, boundary):
                            covered.add((current, phase));raise Interrupted()
                    with self.assertRaises(Interrupted):self.run_delivery(fault=death)
                    self.assert_delivered(self.run_delivery())
        self.assertEqual({stage for stage, _ in covered} | {stage.rsplit('-',1)[0] if stage.startswith('ticket-') else stage for stage in EFFECT_STAGES}, EXPECTED_MUTATIONS)

    def test_W09_W10_service_response_loss_no_duplicate_merge_or_comment(self):
        for flag in ('lose_merge_response', 'lose_close_response', 'lose_comment_response'):
            with self.subTest(response=flag):
                self.restore();self.update(**{flag: True})
                result = self.run_delivery()
                self.assertEqual(result['outcome'], 'incomplete', result)
                self.assert_delivered(self.run_delivery())
                self.assert_delivered(self.run_delivery())
        self.restore()
        def stop(stage, boundary, delivery):
            if (stage, boundary) == ('ticket-comment-8', 'after-returned'):raise Interrupted()
        with self.assertRaises(Interrupted):self.run_delivery(fault=stop)
        data = self.service();data['issues']['8']['comments_data'] = [c for c in data['issues']['8']['comments_data'] if '<!-- harness-delivery:' not in c['body']]
        self.service_file.write_text(json.dumps(data))
        result = self.run_delivery()
        self.assertEqual(result['outcome'], 'incomplete')
        self.assertEqual(self.service()['merge_count'], 1)
        changed = self.run_delivery(dict(self.approval, head='d' * 40))
        self.assertEqual(changed['outcome'], 'decision-needed')

    def publication_matrix(self, shard):
        self.fast_reads = True
        publications = []
        prefixes = tempfile.TemporaryDirectory(prefix='delivery-publications-')
        self.addCleanup(prefixes.cleanup)
        def inventory(stage, phase, delivery):
            if phase == 'before-intent':
                self.save_case(Path(prefixes.name) / stage)
            if phase == 'before-publication' and stage != 'resume':
                event = git(self.source, 'log', '-1', '--format=%s', delivery.tip).decode().strip().rsplit(' ', 1)[1]
                if (stage, event) not in publications:
                    publications.append((stage, event))
        self.assert_delivered(self.run_delivery(fault=inventory))
        self.assertEqual(tuple(publications), PUBLICATION_POINTS)
        self.assertEqual({stage.rsplit('-', 1)[0] if stage.startswith('ticket-') else stage
                          for stage, _ in publications}, EXPECTED_MUTATIONS)
        for index, (stage, event, phase) in enumerate(W11_SCENARIOS):
            if index % 4 != shard:
                continue
            with self.subTest(scenario_id=scenario_id('W11', stage, event, phase)):
                # Start BEFORE this stage's first checkpoint so the original
                # publication call and no-send handler execute in this process.
                self.restore_case(Path(prefixes.name) / stage)
                hits = []
                def response_loss(current, boundary, delivery):
                    if current == stage and boundary == 'before-publication':
                        actual = git(self.source, 'log', '-1', '--format=%s', delivery.tip).decode().strip().rsplit(' ', 1)[1]
                        if actual == event:
                            hits.append((current, actual, phase))
                            delivery.h.store.fail = phase
                result = self.run_delivery(fault=response_loss)
                self.assertEqual(hits, [(stage, event, phase)])
                self.assertEqual(result['outcome'], 'incomplete', result)
                self.assertFalse(result['delivered'])
                again = self.run_delivery()
                self.assertEqual(again['resume_command'], result['resume_command'])
                self.assert_delivered(again)
                self.assert_delivered(self.run_delivery())

    def test_W11_publication_inventory_and_rejection(self):
        self.assertEqual(len(set(W11_SCENARIOS)), len(W11_SCENARIOS))
        owned = scenario_inventory()
        self.assertEqual(sorted(item for key, values in owned.items() if key.startswith('test_W11_publication_shard_')
                                for item in values), sorted(scenario_id('W11', *point) for point in W11_SCENARIOS))
        hook = self.bare / 'hooks/pre-receive'
        hook.write_text('#!/bin/sh\nwhile read old new ref; do\n case "$ref" in refs/heads/harness-artifacts) exit 1;; esac\ndone\n');hook.chmod(0o755)
        result = self.run_delivery()
        self.assertEqual(result['outcome'], 'incomplete', result)
        self.assertNotIn('merge_count', self.service())
        self.assertEqual(result['resume_command'], dw.Delivery(Harness(self.source), self.approval).command())
        hook.unlink()
        self.assert_delivered(self.run_delivery())

    def test_W14_known_unsent_dispatch_publication_recovers_exactly_once(self):
        self.fast_reads = True
        for stage in ('merge', 'ticket-comment-8'):
            for phase in ('before-push', 'after-push'):
                with self.subTest(scenario_id=scenario_id('W14', stage, phase)):
                    self.restore()
                    hits = []
                    def reject(current, boundary, delivery):
                        row = delivery.delivery['steps'].get(stage, {}) if delivery.delivery else {}
                        if (current, boundary) == (stage, 'before-publication') and row.get('dispatched') and row['status'] == 'intent':
                            hits.append(stage);delivery.h.store.fail = phase
                    first = self.run_delivery(fault=reject)
                    self.assertEqual(hits, [stage])
                    self.assertEqual(first['outcome'], 'incomplete', first)
                    service = self.service()
                    self.assertEqual(service.get('merge_count', 0), 0 if stage == 'merge' else 1)
                    self.assertEqual([c for c in service['issues']['8'].get('comments_data', []) if '<!-- harness-delivery:' in c['body']], [])
                    saved = json.loads(Store(self.source).records(Store(self.source).tip())[self.path])
                    row = saved['delivery']['steps'][stage]
                    self.assertFalse(row['dispatched']);self.assertTrue(row['not_sent'])
                    for _ in range(2):
                        result = self.run_delivery()
                        self.assertEqual(result['resume_command'], first['resume_command'])
                        self.assert_delivered(result)

    def test_W14_ambiguous_published_dispatch_never_blindly_resends(self):
        self.fast_reads = True
        for stage in ('merge', 'ticket-comment-8'):
            for publication_boundary in ('before-publication', 'after-publication'):
                self.restore()
                def death(current, boundary, delivery):
                    if (current, boundary) == (stage, publication_boundary):
                        row = delivery.delivery['steps'].get(stage, {})
                        if row.get('dispatched') and row['status'] == 'intent':raise Interrupted()
                with self.assertRaises(Interrupted):self.run_delivery(fault=death)
                # The former W11 local dispatched prefix and a published one
                # both lack durable no-send proof after genuine process death.
                for _ in range(2):
                    result = self.run_delivery()
                    self.assertEqual(result['outcome'], 'incomplete', result)
                    self.assertIn('reconcile service', result['summary'])
                    self.assertEqual(self.service().get('merge_count', 0), 0 if stage == 'merge' else 1)
                    self.assertEqual([c for c in self.service()['issues']['8'].get('comments_data', []) if '<!-- harness-delivery:' in c['body']], [])
                    self.assertEqual(result['artifact_publication']['status'], 'confirmed')

    def test_W14_successful_comment_lost_response_delayed_and_failed_readback(self):
        self.fast_reads = True
        self.update(lose_comment_response=True)
        first = self.run_delivery()
        self.assertEqual(first['outcome'], 'incomplete')
        for observer in (lambda *a: [], lambda *a: (_ for _ in ()).throw(dw.IssueError('lookup unavailable'))):
            with patch.object(FastReadGitHub, 'comments', observer):
                result = self.run_delivery()
            self.assertEqual(result['outcome'], 'incomplete', result)
            self.assertEqual(result['resume_command'], first['resume_command'])
            self.assertEqual(self.service()['merge_count'], 1)
            self.assertEqual(len([c for c in self.service()['issues']['8']['comments_data'] if '<!-- harness-delivery:' in c['body']]), 1)
        self.assert_delivered(self.run_delivery())
        self.assert_delivered(self.run_delivery())

    def test_W11_terminal_publication_failure_is_not_delivery(self):
        def reject(stage, phase, delivery):
            if (stage, phase) == ('complete', 'before-publication'):
                hook = self.bare / 'hooks/pre-receive'
                hook.write_text('#!/bin/sh\nexit 1\n');hook.chmod(0o755)
        result = self.run_delivery(fault=reject)
        self.assertEqual(result['outcome'], 'incomplete', result)
        self.assertFalse(result['delivered'])
        state = json.loads(Store(self.source).records(Store(self.source).tip())[self.path])
        self.assertEqual(state['delivery']['stage'], 'delivered')
        (self.bare / 'hooks/pre-receive').unlink()
        self.assert_delivered(self.run_delivery())

    def test_W12_compact_result_cache_integrity_and_finite_deadlines(self):
        harness = Harness(self.source)
        records = harness.store.records(harness.store.tip())
        for index in range(30):
            records[f"runs/issue-8/run-1/evidence/history-{index}.log"] = ('historical evidence ' + str(index) + '\n').encode() * 100
            tip = harness.store.checkpoint(records, harness.store.tip(), 'Historical evidence')
        self.assertEqual(harness.artifact_sync(tip, records, push=True)['status'], 'confirmed')
        counts = {}
        original = Store.validate_content
        def counted(content):
            key = hashlib.sha256(content).hexdigest();counts[key] = counts.get(key,0) + 1
            return original(content)
        real_run = subprocess.run
        deadlines = []
        def finite(*args, **kwargs):
            deadlines.append(kwargs.get('timeout'))
            return real_run(*args, **kwargs)
        with patch.object(Store, 'validate_content', staticmethod(counted)), patch.object(subprocess, 'run', finite):
            result = self.run_delivery()
        self.assert_delivered(result)
        self.assertLess(len(json.dumps(result)), 6000)
        for path, content in records.items():
            if path.endswith('.log'):
                self.assertEqual(counts[hashlib.sha256(content).hexdigest()], 1, path)
        self.assertTrue(deadlines)
        self.assertTrue(all(value is not None and 0 < value <= 600 for value in deadlines), deadlines)
        # No persistent cache: a new helper rejects a malformed historical tree
        # even when the current tip's records are valid.
        self.restore()
        bad_blob = git(self.source, 'hash-object', '-w', '--stdin', data=b'bad ancestor\n').decode().strip()
        bad_tree = git(self.source, 'mktree', data=('100644 blob ' + bad_blob + '\tunsafe.txt\n').encode()).decode().strip()
        bad = git(self.source, 'commit-tree', bad_tree, data=b'bad ancestor\n').decode().strip()
        valid_tree = git(self.source, 'rev-parse', self.tip + '^{tree}').decode().strip()
        merged = git(self.source, 'commit-tree', valid_tree, '-p', self.tip, '-p', bad, data=b'looks valid\n').decode().strip()
        git(self.source, 'update-ref', REF, merged, self.tip)
        result = self.run_delivery()
        self.assertEqual(result['outcome'], 'decision-needed')
        self.assertIn('artifact', result['summary'])
        self.assertNotIn('merge_count', self.service())


def _publication_shard(shard):
    def test(self):
        self.publication_matrix(shard)
    test.__name__ = f'test_W11_publication_shard_{shard}'
    return test


for _shard in range(4):
    setattr(DeliveryTests, f'test_W11_publication_shard_{_shard}', _publication_shard(_shard))


if __name__ == '__main__':
    unittest.main()
