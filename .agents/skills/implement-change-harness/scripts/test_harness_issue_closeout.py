"""D01/D02 and live C/P invariants: issue evidence, one PR, no closure before merge."""
import copy
from pathlib import Path
import unittest
from github_issues import IssueError
from harness_state import Harness, Invalid, git
from issue_migration import Migration
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
        migration.comment('ready-'+candidate,self.root_issue['number'],target['evidence']+'\n'+target['reference']+'\nVerified PR readiness; issue remains open until default-branch merge.\n'+marker)
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
