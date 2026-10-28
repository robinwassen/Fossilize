"""D01/D02 and live C/P invariants: issue evidence, one PR, no closure before merge."""
import copy
from pathlib import Path
import unittest
from github_issues import IssueError
from harness_state import Harness, Invalid, git
from issue_migration import Migration
from unittest.mock import patch
from test_harness_migration import LiveFixture
from test_issue_migration import RecordingJournal


class IssueCloseoutTests(LiveFixture,unittest.TestCase):
    def setUp(self):self.setup_live()
    def prepare(self):
        self.planned();candidate=self.implemented();self.accepted()
        self.assertEqual(self.state['phase'],'closing')
        target={'issue':self.root_issue['html_url'],'evidence':'Independent fixture checker: one assertion passed; exact candidate verified.',
                'reference':'https://github.com/robinwassen/Fossilize/compare/master...codex/test'}
        self.act('closeout-intent',{'targets':[target]})
        marker=self.state['closeout']['marker']
        migration=Migration(self.source,{'source_sha':self.base},self.gh,RecordingJournal())
        migration.comment('ready-'+candidate,self.root_issue['number'],target['evidence']+'\n'+target['reference']+'\n'+self.state['closeout']['binding']+'\nVerified PR readiness; issue remains open until default-branch merge.\n'+marker)
        self.act('closeout-record',{'candidate_sha':candidate})
        return candidate,target

    def actual_pr_evidence(self, pr):
        marker='<!-- harness-pr:'+self.state['run_id']+':'+self.state['repository']['candidate_sha']+' -->'
        self.gh.api(f"issues/{self.root_issue['number']}/comments",'POST',{'body':'Verified candidate '+self.state['repository']['candidate_sha']+' proposed in '+pr['url']+'; issue remains open until merge. '+marker})
        self.act('publish-observation',{'event':'issue-evidence'})

    def test_D01_readiness_without_source_edit_then_separate_final_acceptance(self):
        candidate,target=self.prepare()
        self.assertEqual(self.gh.issue(self.root_issue['number'])['state'],'open')
        self.assertEqual(self.state['operation'],'final');self.assertIsNone(self.state['final_acceptance'])
        self.assertEqual(git(self.source,'rev-parse','HEAD').decode().strip(),candidate)
        self.accepted();self.assertEqual(self.state['phase'],'publishing')
        self.assertEqual(self.state['final_acceptance']['candidate_sha'],candidate)
        self.assertEqual(self.gh.issue(self.root_issue['number'])['state'],'open')

    def test_D01_incomplete_epic_child_prevents_closing_reference(self):
        child=self.create('Incomplete child')
        self.gh.api(f"issues/{self.root_issue['number']}/sub_issues",'POST',{'sub_issue_id':child['id']})
        self.planned();self.implemented();self.accepted()
        with self.assertRaisesRegex(Invalid,'incomplete child'):
            self.act('closeout-intent',{'targets':[{'issue':self.root_issue['html_url'],'evidence':'Passed current implementation tests','reference':'https://github.com/robinwassen/Fossilize/compare/master...codex/test'}]})
        self.assertEqual(self.gh.issue(self.root_issue['number'])['state'],'open')

    def test_D01_stale_candidate_invalidates_issue_readiness_and_requires_supersession(self):
        candidate,target=self.prepare();next_candidate=self.commit_candidate()
        self.act('observe-source',{'candidate_sha':next_candidate})
        self.assertEqual(self.state['closeout']['stage'],'cleanup');self.assertIsNone(self.state['final_acceptance'])
        with self.assertRaises(Invalid):self.act('closeout-cleanup',{'observation':'Claim cleanup without service evidence'})
        self.gh.api(f"issues/{self.root_issue['number']}/comments",'POST',{'body':'Superseded readiness: new candidate requires new independent checks. '+self.state['closeout']['marker']})
        self.act('closeout-cleanup',{'observation':'Read back marked superseding evidence; unrelated comments retained'})
        self.assertEqual(self.state['closeout']['stage'],'pending');self.assertEqual(self.gh.issue(self.root_issue['number'])['state'],'open')

    def reject_final(self, route, finding_id):
        self.launch('checker')
        self.act('report',dict(self.report(),verdict='changes_required',findings=[{
            'id':finding_id,'status':'open','evidence':'Required verification needs renewed design or execution.',
            'route':route,'affected_task':self.root_issue['html_url']}],next_action='Renew required verification'))

    def supersede_ready(self):
        self.gh.api(f"issues/{self.root_issue['number']}/comments",'POST',{
            'body':'Superseded readiness; fresh independent verification required. '+self.state['closeout']['marker']})
        self.act('closeout-cleanup',{'observation':'Read back exact marked supersession; preserve prior comments'})

    def accept_rechecked(self, finding_id):
        self.launch('checker')
        self.act('resolve-finding',{'finding_id':finding_id,'evidence':'Fresh independent fixture assertion passed'})
        # Run the actual assertion/evidence path using this already-active checker.
        with patch.object(self,'launch',return_value=None):self.accepted()

    def test_D01_R01_final_planner_rejection_cleanup_preserves_replan_route(self):
        self.prepare();old_original=copy.deepcopy(self.state['original'])
        self.reject_final('planner','missing-design')
        self.assertEqual((self.state['phase'],self.state['operation']),('planning','replan'))
        self.harness=Harness(self.source,clock=lambda:self.now)
        self.supersede_ready()
        self.assertEqual((self.state['phase'],self.state['operation']),('planning','replan'))
        self.assertIsNone(self.state['final_acceptance'])
        self.launch('planner')
        self.assertEqual(self.state['attempts'][-1]['operation'],'replan')
        plan=self.plan();plan['version']=2
        self.act('report',dict(self.report(),plan=plan))
        self.assertTrue(all(unit['accepted'] is None for unit in self.state['work_units']))
        self.assertEqual(self.state['original'],old_original)
        self.implemented();self.accept_rechecked('missing-design')
        self.assertEqual(self.state['phase'],'closing')
        self.assertEqual(self.state['limits']['replan_count'],1)

    def test_D01_same_candidate_renewed_generation_restart_replay_preserves_comments(self):
        candidate,target=self.prepare();old=copy.deepcopy(self.state['closeout'])
        human=self.gh.api(f"issues/{self.root_issue['number']}/comments",'POST',{'body':'Unrelated human comment: keep exactly.'})
        self.reject_final('implementation','renew-check')
        self.supersede_ready()
        historical=self.gh.comments(self.root_issue['number'])
        self.harness=Harness(self.source,clock=lambda:self.now)
        self.launch('implementer')
        self.act('report',dict(self.report(),result_candidate_sha=candidate,test_mapping={'C1':'test_behavior.Behavior.test_value'},commands=[{'command':'renewed fixture assertion','exit_code':0}]))
        self.accept_rechecked('renew-check')
        renewed=dict(target,evidence='Fresh independent verification: renewed assertion passed at unchanged candidate.')
        self.act('closeout-intent',{'targets':[renewed]})
        current=copy.deepcopy(self.state['closeout'])
        self.assertNotEqual(current['marker'],old['marker'])
        self.assertNotEqual(current['generation'],old['generation'])
        self.assertEqual(current['candidate_sha'],old['candidate_sha'])
        self.harness=Harness(self.source,clock=lambda:self.now)
        self.act('closeout-intent',{'targets':[renewed]})
        self.assertEqual(self.state['closeout'],current)
        changed=dict(renewed,evidence='Changed in-flight body must not silently reuse the operation identity.')
        with self.assertRaises(Invalid):self.act('closeout-intent',{'targets':[changed]})
        # Stable journal operation identity derives from the saved generation marker.
        migration=Migration(self.source,{'source_sha':self.base},self.gh,RecordingJournal())
        operation='ready-'+current['marker']
        body=renewed['evidence']+' '+renewed['reference']+' '+current['binding']+' '+current['marker']
        migration.comment(operation,self.root_issue['number'],body)
        self.harness=Harness(self.source,clock=lambda:self.now)
        migration=Migration(self.source,{'source_sha':self.base},self.gh,RecordingJournal())
        migration.comment(operation,self.root_issue['number'],body)
        comments=self.gh.comments(self.root_issue['number'])
        self.assertEqual(len(comments),len(historical)+1)
        self.assertEqual(comments[:-1],historical)
        self.assertIn(human,comments)
        self.assertEqual(sum(current['marker'] in row['body'] for row in comments),1)
        self.assertEqual(sum(old['marker'] in row['body'] for row in comments),2)
        self.act('closeout-record',{'candidate_sha':candidate});self.accepted()
        self.assertEqual(self.state['final_acceptance']['candidate_sha'],candidate)
        self.assertEqual(self.gh.issue(self.root_issue['number'])['state'],'open')

    def test_P11_live_terminal_null_active_failure_restart_and_retry(self):
        candidate,target=self.prepare();self.accepted()
        intent={key:self.state['repository'][key] for key in ('target_remote','target_repository','target_branch','branch')}
        self.act('publish-intent',intent);git(self.source,'push','origin','codex/test')
        self.act('publish-observation',{'event':'push','remote_sha':candidate})
        pr={'target_repository':'robinwassen/Fossilize','target_branch':'master','branch':'codex/test','state':'open','draft':False,'url':'https://github.com/robinwassen/Fossilize/pull/99'}
        self.update(prs={'99':{'url':pr['url'],'state':'OPEN','isDraft':False,'baseRefName':'master','headRefName':'codex/test','headRefOid':candidate,'body':'Closes '+self.root_issue['html_url'],'closingIssuesReferences':[{'url':self.root_issue['html_url']}]}})
        self.act('publish-observation',{'event':'lookup','all_states':True,'unambiguous':True,'matches':[pr]})
        self.actual_pr_evidence(pr)
        self.harness.store.fail='before-push'
        result=self.act('publish-observation',{'event':'attachment','url':pr['url'],'success':True})
        terminal=self.tip;self.assertFalse(result['workflow_complete'])
        self.harness=Harness(self.source,clock=lambda:self.now)
        observed=self.harness.show();self.assertIsNone(observed['active']);self.assertIsNone(observed['state'])
        result=self.harness.apply('publish-artifacts',{},terminal,0)
        self.assertTrue(result['workflow_complete']);self.assertEqual(result['tip'],terminal)
        self.assertEqual(self.gh.issue(self.root_issue['number'])['state'],'open')

    def test_D01_readback_requires_exact_base_plan_input_binding(self):
        self.planned();candidate=self.implemented();self.accepted()
        target={'issue':self.root_issue['html_url'],'evidence':'Required independent test passed','reference':'https://github.com/robinwassen/Fossilize/compare/master...codex/test'}
        self.act('closeout-intent',{'targets':[target]})
        self.gh.api(f"issues/{self.root_issue['number']}/comments",'POST',{'body':target['evidence']+' '+target['reference']+' '+self.state['closeout']['marker']})
        with self.assertRaisesRegex(Invalid,'binding'):self.act('closeout-record',{'candidate_sha':candidate})
        self.assertIsNone(self.state['final_acceptance'])

    def test_D01_split_units_renew_at_combined_candidate_without_new_implementation(self):
        a=self.create('A');b=self.create('B')
        for row in (a,b):self.gh.api(f"issues/{self.root_issue['number']}/sub_issues",'POST',{'sub_issue_id':row['id']})
        self.gh.api(f"issues/{b['number']}/dependencies/blocked_by",'POST',{'issue_id':a['id']})
        self.act('initialize',self.payload);self.launch('planner');plan=self.plan()
        plan['work_units']=[{'key':row['html_url'],'issue':row['html_url'],'title':row['title'],'definition':row['body'],'dependencies':[] if row==a else [a['html_url']],'criteria':['AC1']} for row in (a,b)]
        self.act('report',dict(self.report(),plan=plan))
        self.implemented();self.accepted();candidate=self.implemented();self.accepted()
        counts=dict(self.state['limits']['implementation_counts'])
        targets=[{'issue':url,'evidence':'Independent fixture gate passed','reference':'https://github.com/robinwassen/Fossilize/compare/master...codex/test'} for url in (self.root_issue['html_url'],a['html_url'],b['html_url'])]
        self.act('closeout-intent',{'targets':targets})
        self.assertEqual(self.state['operation'],'work_unit');self.assertEqual(self.state['phase'],'checking')
        self.accepted();self.assertEqual(self.state['phase'],'checking');self.accepted()
        self.assertEqual(self.state['limits']['implementation_counts'],counts)
        self.assertTrue(all(unit['accepted']['candidate_sha']==candidate for unit in self.state['work_units']))
        self.act('closeout-intent',{'targets':targets})
        for target in targets:self.gh.api(f"issues/{self.gh.number(target['issue'])}/comments",'POST',{'body':target['evidence']+' '+target['reference']+' '+self.state['closeout']['binding']+' '+self.state['closeout']['marker']})
        self.act('closeout-record',{'candidate_sha':candidate});self.accepted()
        self.assertEqual(self.state['final_acceptance']['candidate_sha'],candidate)
        self.assertEqual(set(self.state['closeout']['closing_references']),{'Closes '+target['issue'] for target in targets})

    def test_D02_real_remote_one_pr_response_loss_open_until_user_merge(self):
        candidate,target=self.prepare();self.accepted()
        intent={key:self.state['repository'][key] for key in ('target_remote','target_repository','target_branch','branch')}
        self.act('publish-intent',intent);git(self.source,'push','origin','codex/test')
        self.act('publish-observation',{'event':'push','remote_sha':candidate})
        self.act('publish-observation',{'event':'lookup','all_states':True,'unambiguous':True,'matches':[]})
        self.act('publish-observation',{'event':'create-dispatched'})
        # Persistent fake PR store represents a lost creation response; lookup
        # observes the same single PR rather than repeating creation.
        pr={'target_repository':'robinwassen/Fossilize','target_branch':'master','branch':'codex/test','state':'open','draft':False,'url':'https://github.com/robinwassen/Fossilize/pull/99'}
        self.update(prs={'99':{'url':pr['url'],'state':'OPEN','isDraft':False,'baseRefName':'master','headRefName':'codex/test','headRefOid':candidate,'body':'Closes '+self.root_issue['html_url'],'closingIssuesReferences':[{'url':self.root_issue['html_url']}]}})
        self.harness=Harness(self.source,clock=lambda:self.now)
        self.act('publish-observation',{'event':'lookup','all_states':True,'unambiguous':True,'matches':[pr]})
        with self.assertRaises(Invalid):self.act('publish-observation',{'event':'create-dispatched'})
        self.actual_pr_evidence(pr)
        done=self.act('publish-observation',{'event':'attachment','url':pr['url'],'success':True})
        self.assertTrue(done['workflow_complete']);self.assertEqual(self.gh.issue(self.root_issue['number'])['state'],'open')
        # Only explicit simulated user merge marks issue delivery complete. The
        # helper's completed run and accepted work units did not perform it.
        git(self.repo,'merge','--ff-only',candidate);git(self.repo,'push','origin','master')
        self.gh.api(f"issues/{self.root_issue['number']}",'PATCH',{'state':'closed','state_reason':'completed'})
        self.assertEqual(self.gh.issue(self.root_issue['number'])['state_reason'],'completed')
