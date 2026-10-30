"""P01-P05: ordinary live parallelism, bounded processes and historical readers."""
import copy
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import threading
import unittest
from unittest.mock import patch

import harness_state as hs
from harness_state import Harness, Invalid, Store, encoded, git, state_path
from test_harness_migration import LiveFixture
import test_harness_issue_closeout as closeout
import verification_lock as vl
from issue_migration import Journal
from github_issues import IssueError

WRAPPER = Path(vl.__file__).resolve()


class ParallelTests(LiveFixture, unittest.TestCase):
    actual_pr_evidence = closeout.IssueCloseoutTests.actual_pr_evidence

    def setUp(self):
        self.setup_live()
        self.payload['paths'] = self.paths(self.source, 'a')
        self.harness = self.selected(self.source, self.root_issue, 'run-1')

    def paths(self, source, name):
        return {'source_worktree': str(source), 'build_directory': str(self.root / ('build-' + name)),
                'output_directory': str(self.root / ('output-' + name))}

    def selected(self, source, issue, run):
        return Harness(source, clock=lambda: self.now, root_issue=issue['html_url'], run_id=run)

    def second(self):
        issue = self.create('Second run')
        source = self.root / 'source-b'
        git(self.source, 'worktree', 'add', '-b', 'codex/second', str(source), self.base)
        payload = copy.deepcopy(self.payload)
        payload.pop('original',None)
        payload.update(root_issue=issue['html_url'], run_id='run-b', paths=self.paths(source, 'b'))
        payload['repository'].update(branch='codex/second', implementation_worktree=str(source))
        harness = self.selected(source, issue, 'run-b')
        result = harness.apply('initialize', payload, self.harness.store.tip(), 0)
        self.assertEqual(result['artifact_publication']['status'], 'confirmed', result)
        return harness, result, payload, issue

    def command(self, name='probe', **extra):
        return dict(dict(command='fixture assertion ' + name, cwd=str(self.source), timeout=600), **extra)

    def prepare(self):
        self.planned(); candidate = self.implemented(); self.accepted()
        repository = self.state['repository']
        target = {'issue':self.root_issue['html_url'], 'evidence':'Independent fixture checker passed at the selected candidate.',
                  'reference':'https://github.com/'+repository['target_repository']+'/compare/'+repository['target_branch']+'...'+repository['branch']}
        self.act('closeout-intent',{'targets':[target]})
        body = target['evidence']+' '+target['reference']+' '+self.state['closeout']['binding']+' '+self.state['closeout']['marker']
        self.gh.api(f"issues/{self.root_issue['number']}/comments",'POST',{'body':body})
        self.act('closeout-record',{'candidate_sha':candidate})
        return candidate,target

    def complete_selected_run(self):
        candidate, target = self.prepare()
        self.accepted()
        intent = {k:self.state['repository'][k] for k in ('target_remote','target_repository','target_branch','branch')}
        self.act('publish-intent', intent); git(self.source, 'push', 'origin', intent['branch'])
        self.act('publish-observation', {'event':'push','remote_sha':candidate})
        pr = dict(intent, state='open', draft=False, url='https://github.com/robinwassen/Fossilize/pull/99')
        self.update(prs={'99':{'url':pr['url'],'state':'OPEN','isDraft':False,'baseRefName':'master','headRefName':intent['branch'],'headRefOid':candidate,'body':'Closes '+self.root_issue['html_url'],'closingIssuesReferences':[{'url':self.root_issue['html_url']}]}})
        self.act('publish-observation', {'event':'lookup','all_states':True,'unambiguous':True,'matches':[pr]})
        self.actual_pr_evidence(pr)
        done = self.act('publish-observation', {'event':'attachment','url':pr['url'],'success':True})
        self.assertTrue(done['workflow_complete'])
        retry = self.harness.apply('publish-artifacts', {}, self.tip, self.state['revision'])
        self.assertTrue(retry['workflow_complete'])
        return candidate

    def test_P01_selected_runs_interleave_and_complete(self):
        self.act('initialize', self.payload)
        other, result, payload, issue = self.second()
        other_path = state_path(result['state']['task_key'], 'run-b')
        sentinel = Path(payload['paths']['output_directory'])
        sentinel.mkdir(); (sentinel / 'keep').write_text('second output')
        other_tip = result['tip']
        other.apply('command-intent', self.command('b'), other_tip, result['state']['revision'])
        self.act('command-intent', self.command('a'))  # stale global observation, unchanged selected run
        self.assertEqual(len(self.harness.show()['active_runs']), 2)
        with self.assertRaisesRegex(Invalid, 'select'):
            Harness(self.source).show()
        conflicting = copy.deepcopy(payload)
        third = self.create('Conflicting paths')
        conflicting.update(root_issue=third['html_url'], run_id='conflict')
        bad = self.selected(payload['repository']['implementation_worktree'], third, 'conflict')
        with self.assertRaisesRegex(Invalid, 'conflicting live writable path'):
            bad.apply('initialize', conflicting, self.harness.store.tip(), 0)
        # Existing independent-role workflow all the way to attached PR, with B live.
        before = self.harness.store.records(self.harness.store.tip())
        saved = {p:v for p,v in before.items() if p.startswith(other_path.rsplit('/', 1)[0] + '/')}
        candidate = self.complete_selected_run()
        after = self.harness.store.records(self.tip)
        self.assertTrue(all(after[p] == value for p,value in saved.items()))
        self.assertEqual(list(self.harness.show()['active_runs']), [other_path])
        self.assertEqual((sentinel / 'keep').read_text(), 'second output')
        self.assertEqual(self.state['final_acceptance']['candidate_sha'], candidate)
        self.assertNotEqual(self.state['attempts'][-1]['worker_id'], self.state['attempts'][-2]['worker_id'])

    def test_P02_unrelated_artifact_updates_reload_without_lost_records(self):
        self.act('initialize', self.payload)
        self.launch('planner'); self.act('report', dict(self.report(), plan=self.plan()))
        self.launch('implementer'); envelope = self.report()
        tip, revision = self.tip, self.state['revision']
        other, result, payload, issue = self.second()
        rows = self.harness.store.records(result['tip'])
        other_path = state_path(result['state']['task_key'], 'run-b')
        self.act('command-intent', self.command())
        self.assertEqual(self.harness.store.records(self.tip)[other_path], rows[other_path])
        self.assertIn(result['tip'], git(self.source, 'rev-list', self.tip).decode().splitlines())
        current = self.tip
        with self.assertRaisesRegex(Invalid, 'selected run changed'):
            self.harness.apply('command-intent', self.command(), tip, revision)
        for field in ('worker_id','candidate_sha','base_sha','plan_version','work_unit'):
            wrong = dict(envelope, result_candidate_sha=self.base, test_mapping={'C1':'test_behavior'}, commands=[{'command':'fixture'}])
            wrong[field] = 'wrong'
            with self.assertRaises(Invalid):
                self.harness.apply('report', wrong, current, self.state['revision'])
            self.assertEqual(self.harness.store.tip(), current)
        with self.assertRaisesRegex(Invalid, 'artifact tip changed'):
            self.harness.apply('command-intent', self.command(), self.base, self.state['revision'])
        # Shared publication policy and selected evidence are part of the CAS boundary.
        records = self.harness.store.records(current)
        path = f"runs/{self.state['task_key']}/run-1/evidence/kept.json"
        self.act('records-only', {'records':{path:{'kept':True}}})
        with self.assertRaises(Invalid):
            self.harness.apply('records-only', {'records':{path:{'kept':False}}}, self.tip, self.state['revision'])
        with self.assertRaisesRegex(Invalid, 'selected run records changed'):
            self.harness.apply('command-intent', self.command(), current, self.state['revision'])
        # Synthetic incompatible shared policy is rejected before any mutation.
        checkpoint = self.tip; changed = self.harness.store.records(checkpoint)
        journal = json.loads(changed['allocations.json']); journal[hs.POLICY]['review_summary'] += ' changed'
        changed['allocations.json'] = encoded(journal)
        altered = self.harness.store.checkpoint(changed, checkpoint, 'policy conflict fixture')
        with self.assertRaisesRegex(Invalid, 'shared artifact policy changed'):
            self.harness.apply('command-intent', self.command(), checkpoint, self.state['revision'])
        self.assertEqual(self.harness.store.tip(), altered)

    def wait_file(self, path, process=None, timeout=10):
        end = time.monotonic() + timeout
        while not path.exists() and time.monotonic() < end:
            if process and process.poll() is not None:
                self.fail('wrapper exited before readiness: ' + str(process.returncode))
            time.sleep(.01)
        self.assertTrue(path.exists(), 'bounded readiness wait expired: ' + str(path))

    def start(self, repo, command, timeout=5, receipt=None, wrapper=WRAPPER, intent='probe'):
        args = [sys.executable, '-B', str(wrapper), '--repo', str(repo), '--timeout', str(timeout)]
        if receipt:
            args += ['--receipt', str(receipt), '--intent-id', intent]
        process = subprocess.Popen(args + ['--'] + command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        self.addCleanup(cleanup)
        return process

    def holder(self, repo):
        entered, release = self.root / 'holder-entered', self.root / 'holder-release'
        code = "import pathlib,time; p=pathlib.Path; p(%r).touch(); end=time.monotonic()+20\nwhile not p(%r).exists() and time.monotonic()<end: time.sleep(.01)\nassert p(%r).exists()" % (str(entered),str(release),str(release))
        process = self.start(repo, [sys.executable, '-c', code], timeout=25)
        self.wait_file(entered, process)
        return process, release

    def test_P03_blocking_wrapper_serializes_and_preserves_results(self):
        holder, release = self.holder(self.source)
        entered = self.root / 'second-entered'
        receipt = self.root / 'receipt.json'
        child = [sys.executable, '-c', "from pathlib import Path; Path(%r).touch(); raise SystemExit(7)" % str(entered)]
        second = self.start(self.source, child, timeout=.4, receipt=receipt)
        time.sleep(.7)  # exceeds child deadline while blocked; child must still be unstarted
        self.assertIsNone(second.poll()); self.assertFalse(entered.exists())
        release.touch()
        self.assertEqual(holder.communicate(timeout=10), (b'',b'')); self.assertEqual(holder.returncode,0)
        second.communicate(timeout=10); self.assertEqual(second.returncode,7); self.assertTrue(entered.exists())
        measured = json.loads(receipt.read_text())
        self.assertGreater(measured['wait_seconds'], .4); self.assertEqual(measured['exit_code'],7)
        success = self.start(self.source, [sys.executable,'-c','pass'])
        success.communicate(timeout=10); self.assertEqual(success.returncode,0)
        slow = self.start(self.source, [sys.executable,'-c','import time;time.sleep(2)'], timeout=.15)
        slow.communicate(timeout=10); self.assertEqual(slow.returncode,124)
        self.assertTrue(vl.lock_path(self.source).is_file())

    def test_P04_wait_reloads_run_and_excludes_only_contention(self):
        self.now = time.time()
        self.payload['limits'] = {'deadline':self.now+10}
        self.act('initialize', self.payload)
        other, other_result, payload, issue = self.second()
        b_limits = copy.deepcopy(other_result['state']['limits'])
        # A copied package path and another worktree use exactly the same inode.
        moved = self.root / 'moved.py'; shutil.copy2(WRAPPER, moved)
        self.assertEqual(vl.lock_path(self.source), vl.lock_path(payload['paths']['source_worktree']))
        holder, release = self.holder(payload['paths']['source_worktree'])
        entered = self.root / 'waiter-entered'; receipt = self.root / 'waiter-receipt.json'
        child = [sys.executable,'-c',"from pathlib import Path; Path(%r).touch()" % str(entered)]
        self.now = time.time()
        self.act('command-intent', self.command(intent_id='wait-a',child_command=child,timeout=.5))
        self.act('command-intent', self.command(intent_id='batch-b',child_command=child,timeout=.5))
        original_deadline = self.state['limits']['deadline']
        counts = copy.deepcopy(self.state['limits']['implementation_counts'])
        waiter = self.start(self.source, child, .5, receipt, moved, 'wait-a')
        # Artifact update while verification is blocked proves the locks are separate.
        result = other.apply('records-only', {'records':{f"runs/{other_result['state']['task_key']}/run-b/evidence/while-waiting.txt":'B advanced while A waited\n'}}, other_result['tip'],other_result['state']['revision'])
        self.assertEqual(result['artifact_publication']['status'],'confirmed')
        time.sleep(max(0, original_deadline-time.time()+.2))
        self.assertIsNone(waiter.poll()); self.assertFalse(entered.exists())
        lock = vl.lock_path(self.source); inode = lock.stat().st_ino
        release.touch(); holder.communicate(timeout=10); waiter.communicate(timeout=10)
        self.assertEqual(waiter.returncode,0); self.assertTrue(entered.exists())
        self.assertEqual(lock.stat().st_ino,inode)
        self.now = time.time()
        measured = json.loads(receipt.read_text())
        self.act('command-wait', measured)  # stale A global tip reloads B records
        self.assertFalse(self.state['limits']['exhausted'])
        self.assertAlmostEqual(self.state['limits']['deadline'], original_deadline+measured['wait_seconds'])
        self.assertEqual(self.state['limits']['implementation_counts'], counts)
        self.assertEqual(other.show()['state']['limits'], b_limits)
        with self.assertRaisesRegex(Invalid,'already accounted'):
            self.act('command-wait',measured)
        # A prepublished later command is eligible under the deadline extended by A.
        self.assertGreater(time.time(), original_deadline)
        receipt_b = self.root / 'batch-b.json'
        batch = self.start(self.source, child, .5, receipt_b, intent='batch-b')
        batch.communicate(timeout=10); self.assertEqual(batch.returncode,0)
        self.now = time.time(); self.act('command-wait',json.loads(receipt_b.read_text()))
        self.act('command-intent', self.command('continued automatically'))
        self.assertNotEqual(self.state['operation'],'budget')
        # Short artifact mutations also block automatically and credit only contention.
        scripts = str(Path(hs.__file__).parent)
        for mode in ('mutation','read-only'):
            ready, released, starting = [self.root/(mode+'-'+part) for part in ('ready','release','starting')]
            holder_code = "import sys,time;sys.path.insert(0,%r);from verification_lock import file_lock;from pathlib import Path\nwith file_lock(%r):\n Path(%r).touch()\n end=time.monotonic()+15\n while not Path(%r).exists() and time.monotonic()<end: time.sleep(.01)" % (scripts,str(hs.common_dir(self.source)/'harness-run.lock'),str(ready),str(released))
            artifact_holder = subprocess.Popen([sys.executable,'-c',holder_code])
            self.addCleanup(lambda p=artifact_holder: p.kill() if p.poll() is None else None)
            self.wait_file(ready,artifact_holder)
            action = "h.apply('command-intent',%r,%r,%r)" % (self.command(mode),self.tip,self.state['revision']) if mode == 'mutation' else 'h.show()'
            worker_code = "import sys,time,json;sys.path.insert(0,%r);from harness_state import Harness;from pathlib import Path;h=Harness(%r,root_issue=%r,run_id='run-1');Path(%r).touch();print(json.dumps(%s))" % (scripts,str(self.source),self.root_issue['html_url'],str(starting),action)
            contender = subprocess.Popen([sys.executable,'-c',worker_code],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            self.addCleanup(lambda p=contender: p.kill() if p.poll() is None else None)
            self.wait_file(starting,contender)
            deadline_before = self.state['limits']['deadline']
            time.sleep(max(.2,deadline_before-time.time()+.15))
            self.assertIsNone(contender.poll())
            released.touch(); artifact_holder.wait(timeout=10)
            output,error = contender.communicate(timeout=15)
            self.assertEqual(contender.returncode,0,error)
            observed = json.loads(output)
            self.now = time.time()
            if mode == 'mutation':
                self.tip,self.state = observed['tip'],observed['state']
                credited = self.state['limits']['artifact_waits'][-1]['wait_seconds']
                self.assertAlmostEqual(self.state['limits']['deadline'],deadline_before+credited)
            else:
                self.assertEqual(observed['state'],self.state)
                receipt_artifact = observed['artifact_wait']
                with self.assertRaisesRegex(Invalid,'another selected run'):
                    other.apply('artifact-wait',receipt_artifact,other.store.tip(),other.show()['state']['revision'])
                self.act('artifact-wait',receipt_artifact)
                with self.assertRaisesRegex(Invalid,'already accounted'):
                    self.act('artifact-wait',receipt_artifact)
            self.assertFalse(self.state['limits']['exhausted'])
        # Non-mutating publication/resume returns must surface measured lock
        # contention too. Mock only this short lock boundary; actual process
        # contention is already established above.
        for action,payload in (('publish-artifacts',{}),('initialize',self.payload)):
            canonical = copy.deepcopy(self.state)
            @contextmanager
            def contended(repo, blocking=False):
                self.now += 1.0
                yield 1.0
            self.now = time.time()
            with patch('harness_state.repository_lock',contended):
                observed = self.harness.apply(action,payload,self.tip,self.state['revision'])
            self.assertEqual(observed['state'],canonical)
            self.assertIsNotNone(observed['artifact_wait'])
            self.act('artifact-wait',observed['artifact_wait'])
        # Give this independent journal subsection its own original fixture
        # budget, measured immediately after initialization rather than consumed
        # by the preceding deadline-crossing experiments.
        journal_issue = self.create('Journal wait')
        journal_source = self.root/'source-journal'
        git(self.source,'worktree','add','-b','codex/journal',str(journal_source),self.base)
        journal_payload = copy.deepcopy(self.payload); journal_payload.pop('original',None)
        self.now = time.time()
        journal_payload.update(root_issue=journal_issue['html_url'],run_id='journal',paths=self.paths(journal_source,'journal'),limits={'deadline':self.now+10})
        journal_payload['repository'].update(branch='codex/journal',implementation_worktree=str(journal_source))
        self.harness = self.selected(journal_source,journal_issue,'journal')
        initialized = self.harness.apply('initialize',journal_payload,self.harness.store.tip(),0)
        self.root_issue,self.source,self.payload = journal_issue,journal_source,journal_payload
        self.tip,self.state = initialized['tip'],initialized['state']
        self.assertGreater(self.state['limits']['deadline']-time.time(),5,'journal fixture setup consumed its fresh execution budget')
        # The coordinator settles Journal waits outside the short lock, then the
        # same live dispatch reacquires/reloads and proceeds without another call.
        settled, effects = [], []
        def settle(receipt):
            with hs.repository_lock(self.source):
                pass  # Callback must run outside the lock it needs for settlement.
            self.now = time.time()
            tip = self.harness.store.tip()
            selected = self.harness.selected_record(self.harness.store.records(tip))
            result = self.harness.apply('artifact-wait',receipt,tip,selected['revision'])
            self.tip,self.state = result['tip'],result['state']
            settled.append(receipt)
        journal = Journal(self.source,root_issue=self.root_issue['html_url'],run_id=self.state['run_id'],settle_wait=settle)
        journal.dispatch(lambda: effects.append('uncontended'))
        self.assertEqual(settled,[])  # No microsecond settle/reacquire loop.
        with self.assertRaisesRegex(IssueError,'settle_wait callback'):
            Journal(self.source,root_issue=self.root_issue['html_url'],run_id=self.state['run_id']).dispatch(lambda: effects.append('missing callback'))
        self.assertEqual(effects,['uncontended']); effects.clear()
        ready,released = self.root/'journal-ready',self.root/'journal-release'
        holder_code = "import sys,time;sys.path.insert(0,%r);from verification_lock import file_lock;from pathlib import Path\nwith file_lock(%r):\n Path(%r).touch()\n end=time.monotonic()+15\n while not Path(%r).exists() and time.monotonic()<end:time.sleep(.01)" % (scripts,str(hs.common_dir(self.source)/'harness-run.lock'),str(ready),str(released))
        artifact_holder = subprocess.Popen([sys.executable,'-c',holder_code])
        self.addCleanup(lambda: artifact_holder.kill() if artifact_holder.poll() is None else None)
        self.wait_file(ready,artifact_holder)
        before_journal_deadline = self.state['limits']['deadline']
        effects_at_release = []
        def release_journal():
            effects_at_release.extend(effects)
            released.touch()
        timer = threading.Timer(max(.2,before_journal_deadline-time.time()+.15),release_journal)
        timer.start(); self.addCleanup(timer.cancel)
        journal.dispatch(lambda: effects.append('after automatic settlement'))
        timer.join(timeout=5); artifact_holder.wait(timeout=10)
        self.assertEqual(effects_at_release,[])
        self.assertEqual(effects,['after automatic settlement']); self.assertEqual(len(settled),1)
        self.assertAlmostEqual(self.state['limits']['deadline'],before_journal_deadline+settled[0]['wait_seconds'])
        self.assertEqual(other.show()['state']['limits'],b_limits)
        canonical_before = self.harness.store.records(self.tip)[state_path(self.state['task_key'],self.state['run_id'])]
        journal.append('journal-wait-fact',{'observed':'dispatch returned automatically'})
        self.tip = self.harness.store.tip()
        self.assertEqual(self.harness.store.records(self.tip)[state_path(self.state['task_key'],self.state['run_id'])],canonical_before)
        # Execution elapsed after acquisition is never credited; genuine exhaustion remains.
        self.now = self.state['limits']['deadline'] + 1
        self.act('command-intent',self.command('expired'))
        self.assertTrue(self.state['limits']['exhausted'])
        with self.assertRaisesRegex(IssueError,'budget exhausted'):
            journal.dispatch(lambda: effects.append('expired'))
        self.assertEqual(effects,['after automatic settlement'])
        with self.assertRaises(Invalid):self.act('command-wait',dict(measured,intent_id='unknown'))

    def test_P05_retained_history_and_workflow_integration(self):
        self.act('initialize', self.payload)
        records = self.harness.store.records(self.tip)
        path = state_path(self.state['task_key'],self.state['run_id'])
        original_live_state = copy.deepcopy(self.state)
        historical = copy.deepcopy(self.state)
        historical.update(phase='complete',status='failed',operation='abandoned',abandonment={'success':False})
        records[path] = encoded(historical)
        report_path = path.rsplit('/',1)[0]+'/reports/historical.json'
        records[report_path] = encoded({'unchanged':'historical evidence'})
        # Exercise both historical singleton/null and empty/nonempty version2 trees.
        pointer = {'task_key':self.state['task_key'],'run_id':'run-1','state':path}
        for active in (None, {'version':2,'runs':{}}):
            records['active.json'] = encoded(active)
            self.tip = self.harness.store.checkpoint(records,self.tip,'Historical abandoned fixture')
        self.assertFalse(self.harness.show()['workflow_complete'])
        retained_tip = self.tip
        retained_blobs = git(self.source,'ls-tree','-r',retained_tip).decode()
        self.assertEqual(self.harness.artifact_sync(self.tip,records,push=True)['status'],'confirmed')
        other, result, payload, issue = self.second()
        self.harness = other
        self.source = Path(payload['repository']['implementation_worktree'])
        self.root_issue,self.payload,self.tip,self.state = issue,payload,result['tip'],result['state']
        self.original = copy.deepcopy(self.state['original'])
        self.act('command-intent',self.command())
        self.complete_selected_run()
        final = self.harness.store.records(self.harness.store.tip())
        self.assertEqual(final[path], records[path]); self.assertEqual(final[report_path],records[report_path])
        self.assertIn(retained_tip,git(self.source,'rev-list',self.harness.store.tip()).decode().splitlines())
        self.assertEqual(git(self.source,'ls-tree','-r',retained_tip).decode(),retained_blobs)
        self.harness.remote_history(self.harness.store.tip(),final,self.harness.artifact_policy(final),preserve=False)
        for active in (pointer, {'version':2,'runs':{pointer['state']:pointer}}):
            live = dict(records); live[path] = encoded(original_live_state); live['active.json'] = encoded(active)
            self.harness.validate_run_records(live,active)
        with self.assertRaisesRegex(Invalid,'index key'):
            self.harness.validate_run_records(live,{'version':2,'runs':{'wrong-key':pointer}})
        # Selected delivery's finite progression clock starts after blocking lock
        # acquisition. Advance mocked time beyond 600s while acquiring, then
        # still enforce exactly 600s of actual progression. No external effect.
        import delivery_wrapup as delivery_module
        clock = [100.0]
        class CheckedTimer(BaseException):
            pass
        @contextmanager
        def delayed_delivery_lock(repo,blocking=False):
            self.assertTrue(blocking)
            clock[0] += 701
            yield 701
        with patch('delivery_wrapup.time.monotonic',side_effect=lambda:clock[0]), patch('delivery_wrapup.repository_lock',delayed_delivery_lock):
            delivery = delivery_module.Delivery(self.harness,{'root_issue':self.root_issue['html_url'],'run_id':self.state['run_id']})
            def checked_timer():
                self.assertEqual(delivery.deadline,clock[0]+600)
                delivery.seam('timer-control','before')
                clock[0] += 601
                with self.assertRaisesRegex(Invalid,'deadline reached'):
                    delivery.seam('timer-control','after')
                raise CheckedTimer()
            with patch.object(delivery,'accepted',side_effect=checked_timer), self.assertRaises(CheckedTimer):
                delivery.run()
        import run_harness_tests as runner
        discovered, spec = runner.specification()
        for case in ('P01','P02','P03','P04','P05'):
            ids = [name for name in spec['groups']['regression-extra'] if name.startswith('test_harness_parallel.') and 'test_'+case+'_' in name]
            self.assertEqual(len(ids),1)
        skill = Path(__file__).parents[1]
        for filename in ('SKILL.md','references/state.md','references/roles.md'):
            text = (skill/filename).read_text()
            self.assertIn('verification_lock.py',text); self.assertIn('root-issue',text)
