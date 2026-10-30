"""Approved squash delivery, journaled on the existing orphan artifact history.

No shell commands, task-body instructions, implicit approval or persistent trust
cache. Fault callbacks are test-only and deliberately unavailable through the CLI.
"""
import copy
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

from github_issues import GitHub, IssueError, digest
from harness_state import Invalid, SHA, encoded, git, require, repository_lock, validate_state


# Test-owned coverage must enumerate these independently. Ticket stages repeat
# for every independently verified closeout target. Each checkpoint is published.
MUTATIONS = ('approval', 'merge', 'merge-verification', 'detach', 'remote-delete',
             'local-delete', 'ticket-close', 'ticket-comment', 'complete')
BOUNDARIES = ('before-intent', 'before-dispatch', 'after-effect', 'after-returned', 'after-readback')


class Pending(Exception):
    pass


def required_checks(gh, pr, head):
    args = ['pr', 'checks', pr, '--repo', gh.repository, '--required',
            '--json', 'name,state,bucket,workflow']
    try:
        result = subprocess.run([gh.executable, *args], text=True, capture_output=True, timeout=gh.timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise IssueError('required-check lookup unavailable; reconcile service before retry') from error
    if result.returncode != 0:
        # gh exits1, without JSON, for both zero total and zero required checks.
        # Generic errors are never absence. Confirm this precise CLI signal
        # against structured current PR/head/rollup before accepting an empty set.
        empty = re.fullmatch(r"no (required )?checks reported on the '(.+)' branch", result.stderr.strip())
        if result.returncode != 1 or result.stdout.strip() or not empty:
            raise IssueError('required-check lookup failed; authentication/network/API failure is not absence')
        proof = gh.call(['pr', 'view', pr, '--repo', gh.repository, '--json', 'url,headRefOid,headRefName,statusCheckRollup'])
        require(isinstance(proof, dict) and proof.get('url') == pr and proof.get('headRefOid') == head and
                proof.get('headRefName') == empty[2] and isinstance(proof.get('statusCheckRollup'), list),
                'empty required-check lookup lacks authoritative PR/head/rollup proof')
        require((proof['statusCheckRollup'] == []) == (empty[1] is None), 'zero-required and zero-total check observations conflict')
        return []
    try:
        rows = json.loads(result.stdout)
    except ValueError as error:
        raise IssueError('malformed required-check response; reconcile service before retry') from error
    require(isinstance(rows, list) and all(isinstance(row, dict) and row.get('name') and
            row.get('bucket') == 'pass' for row in rows), 'required verification missing, pending or failed')
    return sorted(rows, key=lambda row: (row.get('workflow') or '', row['name']))


class Delivery:
    def __init__(self, harness, approval, fault=None):
        self.h, self.approval, self.fault = harness, copy.deepcopy(approval), fault
        self.state = None
        self.publication = None
        self.deadline = time.monotonic() + 600

    def seam(self, stage, boundary):
        require(time.monotonic() < self.deadline, 'delivery command deadline reached; resume saved command')
        if self.fault:
            self.fault(stage, boundary, self)

    def command(self):
        a = self.approval
        script = self.h.repo / '.agents/skills/implement-change-harness/scripts/harness_state.py'
        command = ['python3', '-B', str(script), '--repo', str(self.h.repo), 'wrap-up']
        if a.get('approved'):
            command += ['--approve']
        for flag, key in (('root-issue', 'root_issue'), ('run-id', 'run_id'), ('approved-repository', 'repository'),
                          ('approved-pr', 'pr'), ('approved-head', 'head'), ('accepted-revision', 'accepted_revision')):
            command += ['--' + flag, str(a.get(key, ''))]
        return shlex.join(command)

    def result(self, outcome, reason):
        delivery = (self.state or {}).get('delivery', {})
        return {'outcome': outcome, 'delivered': outcome == 'delivered', 'summary': reason,
                'scope': self.approval, 'candidate_sha': self.approval.get('head'),
                'merge_sha': delivery.get('merge_sha'), 'checkpoint_sha': self.h.store.tip(),
                'artifact_publication': self.publication, 'resume_command': self.command()}

    def publish(self, stage):
        self.seam(stage, 'before-publication')
        self.publication = self.h.artifact_sync(self.tip, self.records, push=True)
        self.seam(stage, 'after-publication')
        if self.publication['status'] != 'confirmed':
            raise Pending('Artifact checkpoint unconfirmed; resume the same wrap-up command.')

    def checkpoint(self, stage, event):
        self.state['revision'] += 1
        self.records[self.path] = encoded(self.state)
        path = f"runs/{self.state['task_key']}/{self.state['run_id']}/evidence/delivery-{self.state['revision']}.json"
        require(path not in self.records, 'delivery evidence collision')
        self.records[path] = encoded({'operation': self.delivery['id'], 'stage': stage, 'event': event,
                                     'revision': self.state['revision'], 'step': self.delivery.get('steps', {}).get(stage)})
        self.tip = self.h.store.checkpoint(self.records, self.tip, 'Approved delivery: ' + stage + ' ' + event)
        self.publish(stage)

    def step(self, stage, observe, effect):
        self.seam(stage, 'before-intent')
        row = self.delivery['steps'].get(stage)
        if row is None:
            row = {'id': self.delivery['id'] + ':' + stage, 'status': 'intent'}
            self.delivery['steps'][stage] = row
            self.checkpoint(stage, 'intent')
        actual = observe()
        if row['status'] == 'verified':
            require(actual is not None, 'previously verified ' + stage + ' changed; decision required')
            if actual != row['observation']:
                row['conflicting_observation'] = actual
                self.checkpoint(stage, 'conflicting-readback')
                raise Invalid('previously verified ' + stage + ' identity changed; reconciliation required')
            return actual
        if actual is None:
            if stage == 'merge' and row.get('returned', {}).get('merged') is False:
                raise Invalid('Merge rejected: ' + str(row['returned'].get('message', 'service rejected squash')))
            # A sent synchronous request with lost response is always read back
            # first. A missing comment after dispatch is uncertain, not absence.
            if row.get('dispatched') and (stage == 'merge' or stage.startswith('ticket-comment')):
                raise Pending('Dispatched ' + stage + ' is not yet authoritative; reconcile service before retry.')
            self.seam(stage, 'before-dispatch')
            row.pop('not_sent', None)
            row['dispatched'] = True
            try:
                self.checkpoint(stage, 'dispatch')
            except Pending as error:
                # This process caught a failed dispatch publication before
                # calling effect(). Persist that positive no-send knowledge in
                # a new local checkpoint even when transport remains down.
                # BaseException/process death deliberately bypasses this path:
                # a restored dispatched prefix is still genuinely ambiguous.
                row.update(dispatched=False, status='intent', not_sent=True)
                self.checkpoint(stage, 'not-sent')
                raise Pending('Dispatch checkpoint unconfirmed; effect not sent. Resume the same command.') from error
            try:
                returned = effect()
            except (Invalid, IssueError, OSError, subprocess.TimeoutExpired, Pending) as error:
                # Never retain transport stderr (which can contain credentials
                # or private endpoint configuration) in published records.
                row['error'] = 'Mutation response unavailable; authoritative readback required.'
                self.checkpoint(stage, 'uncertain')
                raise Pending('Uncertain ' + stage + '; resume the same command for authoritative readback.') from error
            self.seam(stage, 'after-effect')
            row.update(status='returned', returned=returned)
            self.checkpoint(stage, 'returned')
            self.seam(stage, 'after-returned')
            if stage == 'merge' and isinstance(returned, dict) and returned.get('merged') is False:
                raise Invalid('Merge rejected: ' + str(returned.get('message', 'service rejected squash')))
            actual = observe()
            if actual is None:
                raise Pending(stage + ' not confirmed; resume the same command.')
        returned = row.get('returned')
        conflict = (stage == 'merge' and isinstance(returned, dict) and returned.get('merged') is True and returned.get('sha') != actual['merge_sha'])
        conflict = conflict or (stage.startswith('ticket-comment') and isinstance(returned, dict) and returned.get('id') is not None and returned['id'] != actual['comment_id'])
        if conflict:
            row.update(status='conflict', observation=actual)
            self.checkpoint(stage, 'conflicting-readback')
            raise Invalid(stage + ' returned identity differs from authoritative readback; reconciliation required')
        row.update(status='verified', observation=actual)
        self.checkpoint(stage, 'readback')
        self.seam(stage, 'after-readback')
        return actual

    def accepted(self):
        s, a = self.state, self.approval
        r, p = s['repository'], s['publishing']
        require(s['phase'] == 'complete' and s['status'] == 'complete' and p['stage'] == 'done', 'select a completed PR-ready run')
        require(a.get('approved') is True, 'explicit human approval to squash-merge and wrap up required')
        require(a.get('repository') == r['target_repository'] == 'robinwassen/Fossilize', 'approval repository differs')
        require(a.get('pr') == p.get('pr', {}).get('url') and re.fullmatch(r'https://github.com/robinwassen/Fossilize/pull/[1-9][0-9]*', a['pr']), 'approval PR differs')
        require(a.get('head') == r['candidate_sha'] and SHA.fullmatch(a['head']), 'approval accepted head differs')
        require(p.get('attachment', {}).get('url') == a['pr'] and p['attachment'].get('success') is True, 'verified attached PR required')
        require(r['branch'] not in (r['target_branch'], 'harness-artifacts') and not r['branch'].startswith('harness/'), 'unsafe source branch')
        expected = {key: value for key, value in (('candidate_sha', a['head']), ('base_sha', r['base_sha']), ('plan_version', s['plan']['version']))}
        require(s['final_acceptance'] and all(s['final_acceptance'].get(k) == v for k, v in expected.items()), 'final acceptance differs')
        require(s['work_units'] and all(u['status'] == 'accepted' and u['accepted'] and all(u['accepted'].get(k) == v for k,v in expected.items()) for u in s['work_units']), 'unaccepted work units')
        require(not any(f['status'] != 'resolved' for f in s['findings']), 'unresolved findings')
        for acceptance, operation, unit in [(s['final_acceptance'], 'final', None)] + [(u['accepted'], 'work_unit', u['key']) for u in s['work_units']]:
            attempts = [attempt for attempt in s['attempts'] if attempt['attempt_id'] == acceptance['attempt_id'] and attempt['role'] == 'checker' and attempt['status'] == 'complete']
            require(len(attempts) == 1, 'independent checker record missing')
            report = json.loads(self.records[attempts[0]['report']])
            require(all(report.get(key) == value for key, value in self.h.tuple_fields(attempts[0]).items()), 'independent checker report envelope differs')
            require(report.get('verdict') == 'accepted' and report['operation'] == operation and (unit is None or report['work_unit'] == unit), 'independent checker scope differs')
            require(all(report.get(key) == value for key, value in expected.items()), 'independent verification tuple differs')
            self.h.evidence(s, report, self.records)
        closeout = s['closeout']
        targets = {target['issue'] for target in closeout['targets']}
        require(closeout['stage'] == 'prepared' and all(closeout.get(k) == v for k,v in expected.items()) and targets == {s['root_issue']['url']} | {u['issue'] for u in s['work_units']}, 'independently verified closeout targets differ')
        require(len(targets) == len(closeout['targets']), 'duplicate closeout target')
        require(p.get('required_checks') is not None, 'required verification baseline missing; review and renew PR readiness')
        if p['ci_policy'] == 'required_after_pr':
            require(p.get('ci', {}).get('result') == 'passed' and p['ci'].get('evidence') and p['required_checks'], 'required hosted CI evidence missing')
        self.targets = targets

    def material(self):
        snapshot = self.state['issue_snapshot']
        current = self.gh.snapshot(snapshot['root'], snapshot['deliverables'])
        require(set(current['nodes']) == set(snapshot['nodes']), 'material prerequisite graph changed')
        for url, prior in snapshot['nodes'].items():
            row = copy.deepcopy(current['nodes'][url])
            if url in self.targets and row['state'] == 'closed' and row['state_reason'] == 'completed':
                row['state'], row['state_reason'] = prior['state'], prior['state_reason']
            require(row == prior, 'material issue input changed or ticket cancelled: ' + url)
            if url in self.targets:
                for child in set(row['children']) - self.targets:
                    child_row = self.gh.issue(child)
                    require(child_row['state'] == 'closed' and child_row['state_reason'] == 'completed', 'incomplete parent child prevents delivery closure')

    def pr(self):
        row = self.gh.api('pulls/' + self.approval['pr'].rsplit('/', 1)[1])
        r = self.state['repository']
        require(row.get('html_url') == self.approval['pr'] and row.get('base', {}).get('repo', {}).get('full_name') == r['target_repository'] and row.get('head', {}).get('repo', {}).get('full_name') == r['target_repository'], 'actual PR repository differs')
        require(row['head']['ref'] == r['branch'] and row['base']['ref'] == r['target_branch'] and row['head']['sha'] == r['candidate_sha'], 'actual PR head/base differs from approval')
        require(row.get('draft') is False and (row.get('state') == 'open' or row.get('merged') is True), 'closed-unmerged or draft PR requires decision')
        return row

    def remote_ref(self, branch):
        lines = self.transport('ls-remote', '--refs', self.endpoint, 'refs/heads/' + branch).decode().splitlines()
        require(len(lines) <= 1, 'ambiguous remote source ref')
        if not lines:
            return None
        sha, ref = lines[0].split()
        require(ref == 'refs/heads/' + branch and SHA.fullmatch(sha), 'malformed remote ref')
        return sha

    def transport(self, *args):
        try:
            return git(self.h.repo, *args, env=dict(os.environ, GIT_TERMINAL_PROMPT='0'))
        except Invalid as error:
            raise Pending('Delivery Git transport failed; restore the bound remote and resume the same command.') from error

    def local_ref(self):
        rows = git(self.h.repo, 'for-each-ref', '--format=%(objectname)', self.source_ref).decode().splitlines()
        require(len(rows) <= 1, 'ambiguous local source ref')
        return rows[0] if rows else None

    def cleanup_safe(self):
        r, head = self.state['repository'], self.approval['head']
        require(self.remote_ref(r['branch']) in (None, head), 'remote source branch advanced; preserve it')
        require(self.local_ref() in (None, head), 'local source branch changed; preserve it')
        intended = Path(r['implementation_worktree']).resolve()
        require(intended.exists(), 'implementation worktree missing; restore recovery helper')
        for entry in git(self.h.repo, 'worktree', 'list', '--porcelain').decode().strip().split('\n\n'):
            fields = dict(line.split(' ', 1) if ' ' in line else (line, '') for line in entry.splitlines())
            if fields.get('branch') == self.source_ref:
                require(Path(fields['worktree']).resolve() == intended, 'source branch occupied by unexpected worktree')
        require(git(intended, 'rev-parse', '--path-format=absolute', '--git-common-dir').decode().strip() == r['common_dir'], 'implementation worktree repository changed')
        require(git(intended, 'rev-parse', 'HEAD').decode().strip() == head, 'implementation worktree HEAD changed')
        symbolic = git(intended, 'rev-parse', '--symbolic-full-name', 'HEAD').decode().strip()
        require(symbolic in ('HEAD', self.source_ref), 'implementation worktree branch changed')
        require(not git(intended, 'status', '--porcelain', '--untracked-files=all', env=dict(os.environ, GIT_OPTIONAL_LOCKS='0')).strip(), 'implementation worktree dirty; preserve edits')

    def preflight(self):
        self.material()
        self.cleanup_safe()
        row = self.pr()
        proof = self.gh.call(['pr', 'view', self.approval['pr'], '--repo', self.gh.repository, '--json',
                              'url,headRefOid,headRefName,baseRefName,body,closingIssuesReferences'])
        r = self.state['repository']
        require(proof.get('url') == self.approval['pr'] and proof.get('headRefOid') == self.approval['head'] and
                proof.get('headRefName') == r['branch'] and proof.get('baseRefName') == r['target_branch'], 'PR closing-reference readback identity differs')
        require({issue['url'] for issue in proof['closingIssuesReferences']} == self.targets and
                all(line in proof['body'] for line in self.state['closeout']['closing_references']),
                'actual PR closing targets changed; preserve unfinished or unrelated issues')
        require(required_checks(self.gh, self.approval['pr'], self.approval['head']) == self.state['publishing']['required_checks'], 'required verification changed; renew verification/approval')
        if not row.get('merged'):
            require(row['base']['sha'] == self.state['repository']['base_sha'] and self.remote_ref(self.state['repository']['target_branch']) == self.state['repository']['base_sha'], 'target base moved; integrate and renew verification/approval')
            require(row.get('mergeable') is True and row.get('mergeable_state') in ('clean', 'has_hooks'), 'merge conditions unknown, blocked or conflicting')
        return row

    def merged(self):
        row = self.pr()
        if row.get('merged'):
            sha = row.get('merge_commit_sha')
            require(SHA.fullmatch(sha or ''), 'authoritative merge commit missing')
            self.delivery['merge_sha'] = sha
            return {'merge_sha': sha, 'pr': row['html_url']}
        return None

    def verify_merge(self):
        merged = self.merged()
        if merged is None:
            return None
        sha = merged['merge_sha']
        r = self.state['repository']
        ref = 'refs/harness/delivery/' + self.delivery['id'] + '/target'
        actual = git(self.h.repo, 'for-each-ref', '--format=%(objectname)', ref).decode().splitlines()
        # This observer is read-only. A separate durable effect fetches the
        # intended target. A changed target cannot silently reuse old readback.
        if not actual or actual != [self.remote_ref(r['target_branch'])]:
            return None
        # Rev-list excludes a commit that is not on the intended target history.
        require(sha in git(self.h.repo, 'rev-list', ref).decode().splitlines(), 'merge commit absent from intended target')
        parents = git(self.h.repo, 'rev-list', '--parents', '-n', '1', sha).decode().split()
        require(parents == [sha, r['base_sha']] and sha != r['candidate_sha'], 'merge is not approved squash at accepted base')
        tree = git(self.h.repo, 'rev-parse', sha + '^{tree}').decode().strip()
        require(tree == git(self.h.repo, 'rev-parse', r['candidate_sha'] + '^{tree}').decode().strip(), 'squash tree differs from accepted candidate')
        return dict(merged, tree=tree, parent=r['base_sha'])

    def fetch_target(self):
        r = self.state['repository']
        ref = 'refs/harness/delivery/' + self.delivery['id'] + '/target'
        self.transport('fetch', '--no-tags', '--no-write-fetch-head', self.endpoint,
                       'refs/heads/' + r['target_branch'] + ':' + ref)
        return {'ref': ref, 'sha': git(self.h.repo, 'rev-parse', ref).decode().strip()}

    def execute(self):
        self.preflight()
        self.seam('approval', 'before-intent')
        if not self.state.get('delivery'):
            self.state['delivery'] = {'id': digest(self.approval)[:24], 'approval': self.approval, 'stage': 'running', 'steps': {}}
            self.delivery = self.state['delivery']
            self.checkpoint('approval', 'intent')
        else:
            self.delivery = self.state['delivery']
        self.seam('approval', 'after-readback')
        def merge_effect():
            self.preflight()
            return self.gh.api('pulls/' + self.approval['pr'].rsplit('/',1)[1] + '/merge', 'PUT', {'merge_method': 'squash', 'sha': self.approval['head']})
        merge = self.step('merge', self.merged, merge_effect)
        self.delivery['merge_sha'] = merge['merge_sha']
        # Verification has durable intent before its fetch and authoritative read.
        self.step('merge-verification', self.verify_merge, self.fetch_target)
        self.preflight()
        intended = self.state['repository']['implementation_worktree']
        def detach():
            self.cleanup_safe()
            git(intended, 'checkout', '--detach', self.approval['head'])
            return {'head': self.approval['head'], 'detached': True}
        self.step('detach', lambda: {'head': self.approval['head']} if git(intended, 'rev-parse', '--symbolic-full-name', 'HEAD').strip() == b'HEAD' else None, detach)
        branch = self.state['repository']['branch']
        def remote_deleted():
            actual = self.remote_ref(branch)
            require(actual in (None, self.approval['head']), 'remote branch advanced; preserve it')
            return {'ref': self.source_ref, 'absent': True} if actual is None else None
        def delete_remote():
            self.cleanup_safe()
            self.transport('push', '--porcelain', '--force-with-lease=' + self.source_ref + ':' + self.approval['head'], self.endpoint, ':' + self.source_ref)
            return {'ref': self.source_ref, 'expected_sha': self.approval['head']}
        self.step('remote-delete', remote_deleted, delete_remote)
        def local_deleted():
            actual = self.local_ref()
            require(actual in (None, self.approval['head']), 'local branch advanced; preserve it')
            return {'ref': self.source_ref, 'absent': True} if actual is None else None
        def delete_local():
            self.cleanup_safe()
            return git(self.h.repo, 'update-ref', '-d', self.source_ref, self.approval['head']).decode()
        self.step('local-delete', local_deleted, delete_local)
        self.material()
        for target in self.state['closeout']['targets']:
            url = target['issue']
            n = self.gh.number(url)
            def closed(url=url):
                self.material()
                row = self.gh.issue(url)
                require(row['state'] == 'open' or (row['state'] == 'closed' and row['state_reason'] == 'completed'), 'ticket cancelled; explicit decision required')
                return {'issue': url, 'state': 'closed', 'state_reason': 'completed'} if row['state'] == 'closed' else None
            def close_ticket(n=n):
                row = self.gh.api(f'issues/{n}', 'PATCH', {'state': 'closed', 'state_reason': 'completed'})
                return {key: row.get(key) for key in ('id', 'html_url', 'state', 'state_reason')}
            self.step('ticket-close-' + str(n), closed, close_ticket)
            marker = '<!-- harness-delivery:' + self.delivery['id'] + ':' + str(n) + ' -->'
            body = ('Delivered ' + self.approval['pr'] + '\nAccepted candidate: `' + self.approval['head'] + '`\nSquash merge: `' + self.delivery['merge_sha'] + '`\nVerification and limits: ' + target['evidence'] + '\n' + marker)
            require(len(body.encode()) <= 16384, 'delivery evidence is not concise')
            def commented(n=n, body=body, marker=marker):
                rows = [row for row in self.gh.comments(n) if marker in (row.get('body') or '')]
                require(len(rows) <= 1, 'duplicate delivery comments; reconciliation required')
                if rows:
                    require(rows[0]['body'] == body and rows[0].get('id'), 'delivery comment body or identity changed')
                    return {'comment_id': rows[0]['id'], 'issue': url, 'body_digest': digest(body)}
                return None
            def post_comment(n=n, body=body):
                row = self.gh.api(f'issues/{n}/comments', 'POST', {'body': body})
                return {'id': row.get('id'), 'body_digest': digest(row.get('body'))}
            self.step('ticket-comment-' + str(n), commented, post_comment)
        self.seam('complete', 'before-intent')
        if self.delivery['stage'] != 'delivered':
            self.delivery['stage'] = 'delivered'
            self.state['publishing']['delivery'] = {'status': 'merged', 'merge_sha': self.delivery['merge_sha'], 'candidate_sha': self.approval['head']}
            self.state['publishing']['pr'].update(state='merged', merge_sha=self.delivery['merge_sha'])
            self.state['next_action'] = 'Approved squash delivery complete; retain independent acceptance and evidence.'
            self.checkpoint('complete', 'readback')
        self.seam('complete', 'after-readback')

    def run(self):
        try:
            with repository_lock(self.h.repo, blocking=bool(self.h.selection)):
                self.tip = self.h.store.tip()
                self.records = self.h.store.records(self.tip)
                active = json.loads(self.records.get('active.json', b'null'))
                self.h.validate_run_records(self.records, active)
                require(self.h.select_pointer(self.records) is None if self.h.selection else active is None, 'another active run exists; finish it before terminal delivery')
                matches = [(path, json.loads(content)) for path, content in self.records.items() if path.endswith('/state.json') and json.loads(content).get('root_issue', {}).get('url') == self.approval.get('root_issue') and json.loads(content).get('run_id') == self.approval.get('run_id')]
                require(len(matches) == 1, 'exact terminal root/run not found')
                self.path, self.state = matches[0]
                validate_state(self.state, self.h.repo)
                self.accepted()
                saved = self.state.get('delivery')
                require((saved and saved['approval'] == self.approval) or (not saved and self.approval.get('accepted_revision') == self.state['revision']), 'approval terminal revision/scope differs')
                self.delivery = saved
                self.gh = GitHub(timeout=min(60, self.state['limits']['command_timeout']))
                self.source_ref = 'refs/heads/' + self.state['repository']['branch']
                self.endpoint = self.h.validate_artifact_policy(self.h.artifact_policy(self.records))
                self.publish('resume')
                self.execute()
                self.publish('complete')
                return self.result('delivered', 'Approved squash delivery complete; exact artifacts confirmed.')
        except Pending as error:
            return self.result('incomplete', str(error))
        except (IssueError, OSError, subprocess.TimeoutExpired) as error:
            return self.result('incomplete', str(error))
        except (Invalid, KeyError, TypeError, ValueError) as error:
            return self.result('decision-needed', str(error))
