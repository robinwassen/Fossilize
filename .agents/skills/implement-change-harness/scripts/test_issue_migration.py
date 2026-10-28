"""I/M cases: faithful inventory and durable idempotent external effects."""
import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch
import test_harness_state as legacy_fixtures
from test_issue_source import ServiceFixture
from github_issues import IssueError, digest
from harness_state import Harness, Invalid, git, repository_lock
from issue_migration import Migration, Journal, inventory, check_cycles, MARKER


class RecordingJournal:
    def __init__(self):self.entries={};self.fail=False
    def get(self,name):return self.entries.get(name)
    def append(self,name,value):
        if self.fail:raise IssueError('publication incomplete')
        if name in self.entries:
            if self.entries[name]!=value:raise IssueError('journal conflict')
        self.entries[name]=copy.deepcopy(value)


class IssueMigrationTests(ServiceFixture,unittest.TestCase):
    def setUp(self):
        self.setup_service();self.journal=RecordingJournal()
        self.inv={'source_sha':'a'*40,'tasks':{}}
        self.migration=Migration('.',self.inv,self.gh,self.journal)

    def test_M02_create_lost_response_reconcile_and_no_duplicates(self):
        self.update(lose_create_response=True)
        first=self.migration.definition('FOS-000001','Original task','Exact original acceptance')
        second=Migration('.',self.inv,self.gh,self.journal).definition('FOS-000001','Original task','Exact original acceptance')
        self.assertEqual(first,second);self.assertEqual(len(self.service()['issues']),1)
        creates=[call for call in self.service()['calls'] if 'POST' in call]
        self.assertEqual(len(creates),1);self.assertEqual(len(self.journal.entries),2)
        self.assertIn('intent',next(iter(self.journal.entries)))

    def test_M02_comment_lost_response_and_partial_auth_fail_no_duplicates(self):
        row=self.create();self.update(lose_comment_response=True)
        self.migration.comment('ready',row['number'],'Verified candidate; still open')
        self.migration.comment('ready',row['number'],'Verified candidate; still open')
        self.assertEqual(len(self.service()['issues'][str(row['number'])]['comments_data']),1)
        self.update(auth_failure=True)
        with self.assertRaises(IssueError):self.migration.definition('another','Title','Body')
        self.assertEqual(len(self.service()['issues']),1)

    def test_M03_unrelated_body_and_comment_edits_preserved(self):
        row=self.migration.definition('FOS-000001','Title','Original')
        state=self.service();state['issues'][str(row['number'])]['body']+='User edit';self.service_file.write_text(json.dumps(state))
        with self.assertRaises(IssueError):self.migration.definition('FOS-000001','Title','Original')
        self.assertTrue(self.service()['issues'][str(row['number'])]['body'].endswith('User edit'))
        self.migration.comment('comment',row['number'],'Managed evidence')
        state=self.service();state['issues'][str(row['number'])]['comments_data'][0]['body']+='User edit';self.service_file.write_text(json.dumps(state))
        with self.assertRaises(IssueError):self.migration.comment('comment',row['number'],'Managed evidence')
        self.assertEqual(len(self.service()['issues']),1)

    def test_M01_unpublished_intent_blocks_dispatch(self):
        self.journal.fail=True
        with self.assertRaises(IssueError):self.migration.definition('root','Title','Body')
        self.assertEqual(self.service()['issues'],{})

    def test_I03_missing_and_cyclic_graph_rejected(self):
        with self.assertRaises(IssueError):check_cycles({'a':['missing']})
        with self.assertRaises(IssueError):check_cycles({'a':['b'],'b':['a']})
        check_cycles({'a':[],'b':['a'],'c':['b']})

    def test_M02_malformed_timeout_and_after_identity_observation_loss_reconcile(self):
        for index,failure in enumerate(('malformed_create','timeout_create')):
            self.update(**{failure:True});self.gh.timeout=.5
            self.migration.definition('failure-'+str(index),'Task '+str(index),'Original criteria')
        class LoseObservation(RecordingJournal):
            def append(self,name,value):
                if name.endswith('-observed') and not getattr(self,'lost',False):
                    self.lost=True;raise IssueError('crash after identity before observation')
                super().append(name,value)
        journal=LoseObservation();migration=Migration('.',self.inv,self.gh,journal)
        with self.assertRaises(IssueError):migration.definition('returned','Task','Original')
        migration.definition('returned','Task','Original')
        self.assertEqual(len(self.service()['issues']),3)
        self.assertEqual(len([call for call in self.service()['calls'] if 'POST' in call]),3)

    def test_M02_durable_returned_identity_prevents_recreation_after_deletion(self):
        row=self.migration.definition('created','Task','Original obligation')
        returned=[value for name,value in self.journal.entries.items() if name.endswith('-returned')]
        self.assertEqual(returned[0]['returned_identity']['id'],row['id'])
        state=self.service();state['issues']={};self.service_file.write_text(json.dumps(state))
        with self.assertRaisesRegex(IssueError,'previous returned identity'):
            self.migration.definition('created','Task','Original obligation')
        self.assertEqual(self.service()['issues'],{})

    def test_I02_I03_I04_M03_all_definitions_before_native_edges_full_audit(self):
        tasks={}
        for number in range(1,29):
            key=f'FOS-{number:06}'
            dependencies=[] if number in (1,25) else [f'FOS-{number-1:06}']
            tasks[key]={'key':key,'title':'Synthetic requirement '+str(number),'original':f'Original {key}\n- Mandatory automated acceptance {number}.\n',
                'location':'local-docs/legacy-fixture.md','dependencies':dependencies,'dependency_reasons':['Fixture dependency reason'] if dependencies else [],
                'group':'observability' if number<=24 else 'harness'}
        inv={'source_sha':'a'*40,'repository':'robinwassen/Fossilize','tasks':tasks,'shared_observability_requirements':'All synthetic compatibility and verification gates remain mandatory.',
             'unkeyed':[{'original':'Historic fixture report','disposition':'Overlaps existing tasks; external Steam scope remains external.'}],'research_disposition':'Historical supporting evidence.', 'legacy_delivery_evidence':{f'FOS-{number:06}':{'pr':f'https://github.com/robinwassen/Fossilize/pull/{pr}','accepted':{'candidate_sha':'b'*40,'attempt_id':'historical-checker'},'executed':['historical.test'],'skipped':[]} for number,pr in ((25,2),(26,5),(27,6))}}
        result=Migration('.',inv,self.gh,self.journal).run()
        self.assertEqual(result['tasks'],28);self.assertEqual(result['epics'],2)
        self.assertEqual(len(result['mapping']),30);self.assertEqual(result['edges'],26)
        calls=self.service()['calls']
        creates=[index for index,call in enumerate(calls) if 'POST' in call and any(arg.endswith('/issues') for arg in call)]
        relations=[index for index,call in enumerate(calls) if 'POST' in call and any('/sub_issues' in arg or '/blocked_by' in arg for arg in call)]
        self.assertLess(max(creates),min(relations))
        self.assertEqual(len(creates),30)
        for key in ('FOS-000025','FOS-000026','FOS-000027'):
            self.assertEqual(result['snapshots'][key]['state_reason'],'completed')
        self.assertEqual(result['snapshots']['harness']['state'],'open')
        self.assertEqual(result['snapshots']['FOS-000028']['state'],'open')


class RestrictedJournalTests(unittest.TestCase):
    def setUp(self):legacy_fixtures.HarnessTests.setUp(self)
    act=legacy_fixtures.HarnessTests.act
    write_registry=legacy_fixtures.HarnessTests.write_registry

    def test_M01_real_git_append_only_cas_byte_preservation_and_publication(self):
        self.act('initialize',self.payload)
        helper=Harness(self.source)
        old=helper.store.records(self.tip)
        path=f"runs/{self.root_key}/run-1/evidence/intent.json"
        result=helper.apply('records-only',{'records':{path:{'destination':'github.com/robinwassen/Fossilize','operation':'create','expected':'absent'}}},self.tip,self.state['revision'])
        self.assertEqual(result['artifact_publication']['status'],'confirmed')
        new=helper.store.records(result['tip'])
        self.assertEqual({key:value for key,value in new.items() if key!=path},old)
        self.assertEqual(git(self.source,'ls-remote','origin','refs/heads/harness-artifacts').decode().split()[0],result['tip'])
        with self.assertRaises(Invalid):helper.apply('records-only',{'records':{path:{}}},result['tip'],self.state['revision'])
        with self.assertRaises(Invalid):helper.apply('records-only',{'records':{'active.json':{}}},result['tip'],self.state['revision'])
        with self.assertRaises(Invalid):helper.apply('records-only',{'records':{path+'x':{}}},self.tip,self.state['revision'])

    def test_M01_replay_is_read_only_but_pending_publication_still_bars_dispatch(self):
        self.act('initialize',self.payload)
        journal=Journal(self.source)
        journal.append('existing',{'fact':'already committed'})
        tip,records,state=journal.records()
        journal.harness.store.fail='before-push'
        path=f"runs/{self.root_key}/run-1/evidence/pending.json"
        result=journal.harness.apply('records-only',{'records':{path:{'pending':'intent'}}},tip,state['revision'])
        self.assertEqual(result['artifact_publication']['status'],'incomplete')
        pending=journal.harness.store.tip()
        with patch.object(journal.harness,'artifact_sync',side_effect=AssertionError('inspection must not publish')):
            journal.append('existing',{'fact':'already committed'})
        self.assertEqual(journal.harness.store.tip(),pending)
        with patch('issue_migration.time.time',return_value=1000):
            with self.assertRaises(IssueError):journal.authorize()
            journal.harness.store.fail=None
            current,rows,_=journal.records()
            publication=journal.harness.artifact_sync(current,rows,push=True,adopt=True)
            self.assertEqual(publication['status'],'confirmed',publication)
            journal.authorize()
        self.assertEqual(journal.harness.store.tip(),pending)
        self.assertEqual(git(self.source,'ls-remote','origin','refs/heads/harness-artifacts').decode().split()[0],pending)

    def test_M01_external_dispatch_uses_same_canonical_lock(self):
        self.act('initialize',self.payload)
        journal=Journal(self.source);calls=[]
        with repository_lock(self.source),patch('issue_migration.time.time',return_value=1000):
            with self.assertRaisesRegex(Invalid,'held'):journal.dispatch(lambda:calls.append('forbidden'))
        self.assertEqual(calls,[])
        def external_request():
            tip,records,state=journal.records()
            path=f"runs/{self.root_key}/run-1/evidence/interleaved.json"
            with self.assertRaisesRegex(Invalid,'held'):
                Harness(self.source).apply('records-only',{'records':{path:{'forbidden':'interleaving'}}},tip,state['revision'])
            calls.append('authorized')
        with patch('issue_migration.time.time',return_value=1000):
            journal.dispatch(external_request)
        self.assertEqual(calls,['authorized'])

    def test_I01_I02_immutable_inventory_retains_full_sections_and_registry_union(self):
        self.act('initialize',self.payload)
        (self.source/'local-docs/known-issues').mkdir()
        (self.source/'local-docs/known-issues/steam-shader-replay-observability.md').write_text('Historical Steam-owned scheduling report; Fossilize overlaps existing task.')
        (self.source/'local-docs/reviews').mkdir()
        (self.source/'local-docs/reviews/observability-sweep.md').write_text('## Ordered implementation backlog\n\nShared mandatory criteria and compatibility.\n\n1. **FOS-000001 — First task.** no dependencies; exact requirement.\n\n   - Exact automated acceptance bullet.\n\n## Dependency and delivery notes\nCritical path evidence.\n## Adversarial recheck\nHistorical discussion.\n## Remaining verification\nLinux/Windows debug/release requirements.\n')
        registry=self.source/'local-docs/backlog-keys.md'
        registry.write_text(registry.read_text()+'\n| `FOS-000001` | First task | [task](reviews/observability-sweep.md) | Allocated |\n| `FOS-000026` | Imported reservation | historical removed item | Retired |\n')
        # Retired allocations have disposition but cannot become an active task.
        git(self.source,'add','.');git(self.source,'commit','-m','historical inventory fixture')
        revision=git(self.source,'rev-parse','HEAD').decode().strip()
        result=inventory(self.source,revision)
        self.assertEqual(set(result['tasks']),{'FOS-000001',self.root_key})
        self.assertEqual(result['registry_high_water'],26)
        self.assertEqual(result['historical_journal']['high_water'],0)
        self.assertIn('Exact automated acceptance bullet.',result['tasks']['FOS-000001']['original'])
        self.assertIn('Linux/Windows debug/release requirements.',result['shared_observability_requirements'])
        self.assertIn('Shared mandatory criteria and compatibility.',result['shared_observability_requirements'])
        self.assertNotIn('Historical discussion.',result['shared_observability_requirements'])
        self.assertTrue(result['artifact_state_inventory'])
