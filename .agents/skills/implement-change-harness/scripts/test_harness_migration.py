"""V01–V03 schema migration and S02/R01 live issue workflow checks."""
import copy
import json
import os
import sys
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch
import test_harness_state as legacy_fixtures
from test_issue_source import ServiceFixture
from github_issues import GitHub, IssueError
from harness_state import Harness, Invalid, git, encoded, state_path


class LiveFixture(ServiceFixture):
    write_registry=legacy_fixtures.HarnessTests.write_registry
    def setup_live(self):
        legacy_fixtures.HarnessTests.setUp(self)
        self.legacy=self.harness;self.setup_service()
        self.root_issue=self.create(body=self.original[0]['requirement'])
        self.harness=Harness(self.source,clock=lambda:self.now)
        self.payload['repository']['target_repository']='robinwassen/Fossilize'
        self.payload['root_issue']=self.root_issue['html_url']
        self.payload.pop('task_key');self.payload.pop('task_location')
    act=legacy_fixtures.HarnessTests.act
    commit_candidate=legacy_fixtures.HarnessTests.commit_candidate
    def plan(self):
        plan=legacy_fixtures.HarnessTests.plan(self);plan['work_units']=plan.pop('subtasks');return plan
    def launch(self,role):
        self.counter+=1;model={'model':'gpt-6.1-sol','effort':'high'}
        payload={'role':role,'attempt_id':role+'-'+str(self.counter),'requested':model,'actual':model,'catalog':{'gpt-6.1-sol':['high']}}
        self.act('dispatch',payload)
        if self.state['status']=='running':self.act('acknowledge',{'attempt_id':payload['attempt_id'],'worker_id':'worker/'+payload['attempt_id']})
    def report(self):return self.harness.tuple_fields(self.state['attempts'][-1])
    def planned(self):
        self.act('initialize',self.payload);self.launch('planner');self.act('report',dict(self.report(),plan=self.plan()))
    def implemented(self):
        self.launch('implementer');candidate=self.commit_candidate()
        self.act('report',dict(self.report(),result_candidate_sha=candidate,test_mapping={'C1':'test_behavior.Behavior.test_value'},commands=[{'command':'python unittest','exit_code':0}]))
        return candidate
    def accepted(self,operation=None):
        self.launch('checker');envelope=self.report()
        executed=subprocess.run([sys.executable,'-m','unittest','-v','test_behavior'],cwd=self.source,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'),capture_output=True,text=True,timeout=60)
        self.assertEqual(executed.returncode,0,executed.stderr)
        self.assertIn('test_behavior.Behavior.test_value',executed.stderr)
        path=f"runs/{self.state['task_key']}/{self.state['run_id']}/evidence/check-{self.counter}.log"
        item={'gate':'test','result':'passed','exit_code':0,'command':'python -m unittest -v test_behavior','cwd':str(self.source),'environment':'disposable Python/Git fixture','log':path,'criteria':['AC1'],**{key:envelope[key] for key in ('candidate_sha','base_sha','plan_version','attempt_id')}}
        self.act('report',dict(envelope,verdict='accepted',findings=[],discovered=['test_behavior.Behavior.test_value'],executed=['test_behavior.Behavior.test_value'],skipped=[],test_mapping={'C1':{'location':'test_behavior.py','tests':['test_behavior.Behavior.test_value']}},evidence=[item],records={path:executed.stdout+executed.stderr}))


class LiveHarnessTests(LiveFixture,unittest.TestCase):
    def setUp(self):self.setup_live()

    def test_S02_unsplit_issue_initialization_and_snapshot_authority(self):
        self.planned()
        self.assertEqual(self.state['schema_version'],3);self.assertEqual(self.state['task_key'],'issue-'+str(self.root_issue['number']))
        self.assertEqual(len(self.state['work_units']),1);self.assertEqual(self.state['work_units'][0]['issue'],self.root_issue['html_url'])
        self.assertNotIn('units',self.state);self.assertNotIn('subtasks',self.state['plan'])
        journal=json.loads(self.harness.store.records(self.tip)['allocations.json']);self.assertEqual(journal['issued'],[])
        with self.assertRaises(Invalid):self.act('reserve',{'title':'Never allocate','location':'ignored'})

    def test_S01_resume_and_planner_dispatch_fail_closed_when_gh_unavailable(self):
        self.act('initialize',self.payload);tip=self.tip
        self.update(auth_failure=True)
        with self.assertRaises(IssueError):self.act('initialize',self.payload)
        self.assertEqual(self.harness.store.tip(),tip)
        self.update(auth_failure=True)
        with self.assertRaises(IssueError):self.launch('planner')
        self.assertEqual(self.harness.store.tip(),tip);self.assertEqual(self.state['attempts'],[])
        self.assertTrue(self.act('initialize',self.payload)['resumed'])

    def test_S03_planner_cannot_silently_adopt_requirement_changes(self):
        self.act('initialize',self.payload);self.launch('planner');tip=self.tip
        self.gh.api(f"issues/{self.root_issue['number']}",'PATCH',{'body':'Changed mandatory requirement'})
        with self.assertRaisesRegex(Invalid,'reconciliation'):self.act('report',dict(self.report(),plan=self.plan()))
        self.assertEqual(self.harness.store.tip(),tip)
        self.act('reconcile',{'attempt_id':self.state['attempts'][-1]['attempt_id'],'liveness':'terminated','observation':'Planner returned; old-input report fenced','next_action':'reconcile changed task then fresh planner'})
        self.act('reconcile-input',{'snapshot':self.gh.snapshot(self.root_issue['number']),'observation':'Explicit task input change; original acceptance remains preserved'})
        self.assertEqual(self.state['phase'],'planning');self.assertEqual(self.state['operation'],'plan')
        self.assertEqual(self.state['original'],self.original)

    def test_S02_split_requires_native_parent_and_complete_criteria(self):
        child=self.create('Child');self.act('initialize',self.payload);self.launch('planner')
        plan=self.plan();plan['work_units']=[{'key':child['html_url'],'issue':child['html_url'],'title':'Child','definition':child['body'],'dependencies':[],'criteria':['AC1']}]
        with self.assertRaises(Invalid):self.act('report',dict(self.report(),plan=plan))
        self.gh.api(f"issues/{self.root_issue['number']}/sub_issues",'POST',{'sub_issue_id':child['id']})
        self.act('report',dict(self.report(),plan=plan));self.assertEqual(self.state['work_units'][0]['issue'],child['html_url'])

    def test_R01_stale_report_fenced_required_evidence_final_gate_and_budget(self):
        self.planned();self.launch('implementer');candidate=self.commit_candidate();envelope=self.report()
        wrong=dict(envelope,input_revision=0,result_candidate_sha=candidate,test_mapping={'C1':'x'},commands=[{'command':'x'}])
        with self.assertRaises(Invalid):self.act('report',wrong)
        self.act('report',dict(envelope,result_candidate_sha=candidate,test_mapping={'C1':'x'},commands=[{'command':'x'}]))
        self.launch('checker')
        with self.assertRaises(Invalid):self.act('report',dict(self.report(),verdict='accepted',findings=[]))
        self.assertIsNone(self.state['final_acceptance']);self.assertEqual(self.state['phase'],'checking')
        self.now=self.state['limits']['deadline']+1
        self.act('command-intent',{'command':'never execute','cwd':str(self.source),'timeout':600})
        self.assertEqual(self.state['operation'],'budget')

    def test_S03_live_drift_reconciliation_invalidates_acceptances(self):
        self.planned();self.implemented()
        self.gh.api(f"issues/{self.root_issue['number']}",'PATCH',{'body':'New mandatory behavior'})
        with self.assertRaises(IssueError):self.launch('checker')
        self.act('reconcile-input',{'snapshot':self.gh.snapshot(self.root_issue['number']),'observation':'Explicit requirement change requires all new checks'})
        self.assertIsNone(self.state['final_acceptance']);self.assertTrue(all(unit['accepted'] is None for unit in self.state['work_units']))
        self.assertEqual(self.state['operation'],'repair')

    def test_R01_live_split_dependency_order_scoped_findings_and_child_drift(self):
        a=self.create('A');b=self.create('B')
        for row in (a,b):self.gh.api(f"issues/{self.root_issue['number']}/sub_issues",'POST',{'sub_issue_id':row['id']})
        self.gh.api(f"issues/{b['number']}/dependencies/blocked_by",'POST',{'issue_id':a['id']})
        self.act('initialize',self.payload);self.launch('planner');plan=self.plan()
        plan['work_units']=[{'key':row['html_url'],'issue':row['html_url'],'title':row['title'],'definition':row['body'],'dependencies':[] if row==a else [a['html_url']],'criteria':['AC1']} for row in (b,a)]
        self.act('report',dict(self.report(),plan=plan))
        self.assertEqual(self.state['active_work_unit'],a['html_url'])
        self.implemented();self.launch('checker')
        finding={'id':'later-unit','status':'open','evidence':'B must retain additional verified behavior.','route':'implementation','affected_task':b['html_url']}
        self.act('report',dict(self.report(),verdict='changes_required',findings=[finding],next_action='Repair within bounds; B owns its unresolved finding'))
        self.implemented();self.accepted()
        self.assertEqual(self.state['active_work_unit'],b['html_url']);self.assertEqual(self.state['findings'][0]['status'],'open')
        self.gh.api(f"issues/{b['number']}",'PATCH',{'body':'Changed B requirements'})
        previous_tip=self.tip
        with self.assertRaises(IssueError):self.launch('implementer')
        self.assertEqual(self.harness.store.tip(),previous_tip)
        self.assertIsNone(self.state['final_acceptance'])


class StateMigrationTests(LiveFixture,unittest.TestCase):
    def setUp(self):
        self.setup_live()
        self.harness=self.legacy
        self.payload.update(task_key=self.root_key,task_location='local-docs/task.md')
        self.payload.pop('root_issue')
        self.act('initialize',self.payload)
        self.launch('planner')
        plan=legacy_fixtures.HarnessTests.plan(self)
        self.act('report',dict(self.report(),plan=plan))
        self.old_original=copy.deepcopy(self.original)

    def migrate(self,active=False):
        if active:self.launch('implementer')
        old=copy.deepcopy(self.state);before=self.harness.store.records(self.tip)
        snapshot=self.gh.snapshot(self.root_issue['number'])
        payload={'legacy_root':self.root_key,'run_id':self.state['run_id'],'original':self.original,'mapping':{self.root_key:self.root_issue['html_url']},'snapshot':snapshot}
        if active:payload['bridge']=self.report()
        self.harness=Harness(self.source,clock=lambda:self.now)
        result=self.act('migrate-state',payload)
        return old,before,payload,result

    def test_V01_idle_migration_retains_history_allocations_original_deadline(self):
        old,before,payload,result=self.migrate()
        self.assertEqual(self.state['task_key'],old['task_key']);self.assertEqual(self.state['run_id'],old['run_id'])
        self.assertEqual(self.state['original'],old['original']);self.assertEqual(self.state['limits'],old['limits'])
        after=self.harness.store.records(self.tip)
        for path,content in before.items():
            if not path.endswith('/state.json'):self.assertEqual(after[path],content)
        self.assertEqual(self.state['provenance'][-1]['legacy_state'],old)
        self.assertEqual(self.state['provenance'][-1]['legacy_high_water'],25)
        self.assertEqual(result['artifact_publication']['status'],'confirmed')
        self.assertEqual(self.state['plan']['version'],1)

    def test_V03_completed_logical_run_cannot_reinitialize_under_new_storage_token(self):
        self.migrate()
        # Build a completed historical-record fixture; this tests identity reuse,
        # not acceptance/publication conduct (covered by issue closeout tests).
        records=self.harness.store.records(self.tip);completed=copy.deepcopy(self.state)
        completed.update(phase='complete',status='complete',operation='complete')
        records[state_path(self.root_key,'run-1')]=encoded(completed);records['active.json']=encoded(None)
        self.tip=self.harness.store.checkpoint(records,self.tip,'completed migrated identity fixture')
        publication=self.harness.artifact_sync(self.tip,records,push=True,adopt=True)
        self.assertEqual(publication['status'],'confirmed',publication)
        payload=dict(self.payload,root_issue=self.root_issue['html_url'])
        payload.pop('task_key');payload.pop('task_location');before=self.tip
        with self.assertRaisesRegex(Invalid,'root/run identity already used'):
            self.harness.apply('initialize',payload,self.tip,0)
        self.assertEqual(self.harness.store.tip(),before)

    def test_V02_active_exact_bridge_once_original_report_bytes(self):
        old,before,payload,result=self.migrate(active=True)
        candidate=self.commit_candidate()
        report=dict(payload['bridge'],result_candidate_sha=candidate,test_mapping={'C1':'test_behavior.Behavior.test_value'},commands=[{'command':'python unittest','exit_code':0}])
        wrong=dict(report,worker_id='other')
        with self.assertRaises(Invalid):self.act('report',wrong)
        self.act('report',report)
        self.assertEqual(self.state['operation'],'work_unit');self.assertTrue(self.state['legacy_report_bridge']['consumed'])
        path=self.state['attempts'][-1]['report'];self.assertEqual(self.harness.store.records(self.tip)[path],encoded(report))
        with self.assertRaises(Invalid):self.act('report',report)
        self.launch('checker');self.assertIn('work_unit',self.report());self.assertNotIn('unit',self.report())
        self.assertEqual(self.state['limits']['implementation_counts'][self.root_issue['html_url']],1)

    def test_V03_cleanup_restart_without_source_definitions_and_fresh_clone_rebind(self):
        old,before,payload,result=self.migrate()
        git(self.source,'rm','-r','local-docs');git(self.source,'commit','-m','remove historical task definitions')
        self.harness=Harness(self.source,clock=lambda:self.now)
        self.assertEqual(self.harness.show()['state']['root_issue']['url'],self.root_issue['html_url'])
        candidate=git(self.source,'rev-parse','HEAD').decode().strip()
        git(self.source,'push','origin','codex/test')
        clone=self.root/'fresh-clone';git(self.source,'clone',str(self.bare),str(clone));git(clone,'checkout','codex/test')
        git(clone,'config','user.name','Fresh');git(clone,'config','user.email','fresh@example.invalid')
        git(clone,'fetch','origin','harness-artifacts:refs/heads/harness-artifacts')
        fresh=Harness(clone,clock=lambda:self.now)
        with self.assertRaises(Invalid):fresh.show()
        reb=fresh.apply('rebind-execution',{'root_issue':self.root_issue['html_url'],'run_id':self.state['run_id'],'old_common_dir':self.state['repository']['common_dir'],'candidate_sha':candidate},self.tip,self.state['revision'])
        self.assertEqual(reb['artifact_publication']['status'],'confirmed')
        read=fresh.show()['state'];self.assertEqual(read['limits']['deadline'],old['limits']['deadline']);self.assertEqual(read['original'],self.old_original)
        self.assertFalse((clone/'local-docs').exists());self.assertEqual(read['repository']['implementation_worktree'],str(clone))

    def test_V01_conflicting_original_mapping_or_active_checker_reject_atomically(self):
        self.launch('implementer');oldtip=self.tip;envelope=self.report();snapshot=self.gh.snapshot(self.root_issue['number'])
        self.harness=Harness(self.source,clock=lambda:self.now)
        payload={'legacy_root':self.root_key,'run_id':self.state['run_id'],'original':[{'id':'AC1','requirement':'weakened'}],'mapping':{self.root_key:self.root_issue['html_url']},'snapshot':snapshot,'bridge':envelope}
        with self.assertRaises(Invalid):self.act('migrate-state',payload)
        self.assertEqual(self.harness.store.tip(),oldtip)


class BootstrapIssueMigrationTests(LiveFixture,unittest.TestCase):
    def setUp(self):self.setup_live()

    def test_V01_schema1_import_worker_reconciliation_then_source_free_issue_state(self):
        legacy_payload=dict(self.payload,task_key=self.root_key,task_location='local-docs/task.md')
        legacy_payload.pop('root_issue')
        old={'schema_version':1,'revision':4,'task_key':self.root_key,'run_id':'run-1','task_location':'local-docs/task.md',
             'phase':'implementing','operation':'implementer_dispatch','status':'running',
             'repository':dict(legacy_payload['repository'],input_sha=self.base[:8]),'subtasks':[],
             'plan':{'acceptance_criteria':self.original,'plan_version':1},
             'attempts':[{'attempt_id':'old','role':'implementer','input_sha':self.base[:8],'worker_id':'old/worker','status':'running'}],
             'verification':[],'findings':[],'limits':{'repair_rounds_per_subtask':3,'replans':1,'run_timeout_hours':4},
             'publishing':{'pr_url':None},'bootstrap':{'reason':'historical fixture'}}
        path=state_path(self.root_key,'run-1')
        report_path=f'runs/{self.root_key}/run-1/reports/old.json'
        rows={'active.json':encoded({'task_key':self.root_key,'run_id':'run-1','state':path}),
              'allocations.json':encoded({'high_water':25,'issued':[{'key':self.root_key,'location':'local-docs/backlog-keys.md'}]}),path:encoded(old),
              report_path:encoded({'report_type':'implementer','attempt_id':'old','plan_version':1,'candidate_sha':self.base,'input_sha':self.base,'fact':'historical completed worker report'})}
        self.tip=self.harness.store.checkpoint(rows,None,'schema1 fixture ancestor');self.state=old
        original_tip=self.tip
        payload=dict(legacy_payload,reviewed_tip=self.tip,review_summary='Reviewed synthetic schema1 records',plan=legacy_fixtures.HarnessTests.plan(self),implementation_report=report_path)
        self.act('import-bootstrap',payload)
        deadline=self.state['limits']['deadline'];self.assertEqual(self.state['schema_version'],2)
        request={'legacy_root':self.root_key,'run_id':'run-1','original':self.original,'mapping':{self.root_key:self.root_issue['html_url']},'snapshot':self.gh.snapshot(self.root_issue['number'])}
        with self.assertRaisesRegex(Invalid,'termination'):self.act('migrate-state',request)
        self.act('migrate-state',dict(request,terminated_workers=['old/worker'],observation='Runtime fixture confirms old worker terminated'))
        self.assertEqual(self.state['schema_version'],3);self.assertEqual(self.state['limits']['deadline'],deadline)
        self.assertEqual(self.state['provenance'][-1]['bootstrap_reconciliation']['terminated_workers'],['old/worker'])
        self.assertEqual(self.harness.store.records(self.tip)[report_path],rows[report_path])
        git(self.source,'merge-base','--is-ancestor',original_tip,self.tip)
        git(self.source,'rm','-r','local-docs');git(self.source,'commit','-m','remove fixture task definitions')
        self.assertEqual(Harness(self.source).show()['state']['original'],self.original)

    def test_V01_legacy_mutation_api_is_fixture_only(self):
        import legacy_state
        with self.assertRaisesRegex(legacy_state.Invalid,'historical task mutation API disabled'):
            legacy_state.Harness(self.source).apply('initialize',{},None,0)
