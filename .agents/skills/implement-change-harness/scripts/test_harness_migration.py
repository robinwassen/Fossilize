"""V01–V03 schema migration and S02/R01 live issue workflow checks."""
import copy
import json
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
        self.launch('checker');envelope=self.report();path=f"runs/{self.state['task_key']}/{self.state['run_id']}/evidence/check-{self.counter}.log"
        item={'gate':'test','result':'passed','exit_code':0,'command':'python -m unittest -v test_behavior','cwd':str(self.source),'environment':'disposable Python/Git fixture','log':path,'criteria':['AC1'],**{key:envelope[key] for key in ('candidate_sha','base_sha','plan_version','attempt_id')}}
        self.act('report',dict(envelope,verdict='accepted',findings=[],discovered=['test_behavior.Behavior.test_value'],executed=['test_behavior.Behavior.test_value'],skipped=[],test_mapping={'C1':{'location':'test_behavior.py','tests':['test_behavior.Behavior.test_value']}},evidence=[item],records={path:'One assertion test passed in isolated fixture.'}))


class LiveHarnessTests(LiveFixture,unittest.TestCase):
    def setUp(self):self.setup_live()

    def test_S02_unsplit_issue_initialization_and_snapshot_authority(self):
        self.planned()
        self.assertEqual(self.state['schema_version'],3);self.assertEqual(self.state['task_key'],'issue-'+str(self.root_issue['number']))
        self.assertEqual(len(self.state['work_units']),1);self.assertEqual(self.state['work_units'][0]['issue'],self.root_issue['html_url'])
        self.assertNotIn('units',self.state);self.assertNotIn('subtasks',self.state['plan'])
        journal=json.loads(self.harness.store.records(self.tip)['allocations.json']);self.assertEqual(journal['issued'],[])
        with self.assertRaises(Invalid):self.act('reserve',{'title':'Never allocate','location':'ignored'})

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
        self.launch('implementer');candidate=self.commit_candidate();self.act('report',dict(self.report(),result_candidate_sha=candidate,test_mapping={'C1':'test_behavior.Behavior.test_value'},commands=[{'command':'python unittest','exit_code':0}]))
        with self.assertRaises(IssueError):self.launch('checker')
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
