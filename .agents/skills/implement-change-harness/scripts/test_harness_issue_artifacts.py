"""R01/P live schema3 equivalents reuse unchanged artifact counterexample bodies.

The imported tests' globals bind the current helper. Fixture task source is gh;
legacy allocator/Markdown closeout tests remain explicit compatibility coverage.
"""
import copy
import json
import types
import unittest
from unittest.mock import patch
import harness_state as hs
from harness_state import Harness, Invalid, Store, REF, POLICY, encoded, git, state_path
import test_harness_artifacts as historical
from test_harness_migration import LiveFixture


def live_body(name):
    original=getattr(historical.ArtifactTests,name)
    bindings=dict(original.__globals__)
    bindings.update(hs=hs,Harness=Harness,Invalid=Invalid,Store=Store,state_path=state_path)
    return types.FunctionType(original.__code__,bindings,name,original.__defaults__,original.__closure__)


class LiveArtifactTests(LiveFixture,unittest.TestCase):
    def setUp(self):self.setup_live()
    def initialize(self):
        result=self.act('initialize',self.payload);self.root_key=self.state['task_key'];return result
    for name in ('remote_tip','assert_remote','raw','command','clone_writer','writer_checkpoint','reject_hook','raw_commit'):
        locals()[name]=live_body(name)
    for name in ('test_P04_stale_dirty_inspection_and_user_checkout_preserved',
                 'test_P07_interruption_before_push_and_after_success_without_return',
                 'test_P09b_pushurl_observation_uses_bound_push_endpoint',
                 'test_P09c_invalid_ancestor_and_invalid_local_checkpoint_cannot_publish',
                 'test_P09d_concurrent_endpoint_reread_cannot_retarget_publication',
                 'test_P10_actual_postpush_advance_beats_success_and_caller_sha'):
        locals()[name]=live_body(name)

    def test_P01_P05_live_initial_publication_and_idempotent_retry(self):
        result=self.initialize();self.assertEqual(result['artifact_publication']['status'],'confirmed')
        rows=self.assert_remote();policy=json.loads(rows['allocations.json'])[POLICY]
        self.assertEqual(policy['repository'],'robinwassen/Fossilize')
        self.assertEqual(json.loads(rows['allocations.json'])['issued'],[])
        tip=self.tip;self.raw('publish-artifacts');self.assertEqual(self.tip,tip)
        self.assertEqual(len(git(self.source,'rev-list','--parents',tip).split()),1)

    def test_P06_live_rejection_bars_dispatch_and_recovery_preserves_state(self):
        self.initialize();hook=self.reject_hook()
        result=self.raw('command-intent',self.command());pending=self.tip
        self.assertEqual(result['artifact_publication']['status'],'incomplete')
        before=copy.deepcopy(self.state)
        result=self.raw('dispatch',{'role':'planner','attempt_id':'never-dispatch'})
        self.assertEqual(result['artifact_publication']['status'],'incomplete');self.assertEqual(self.state,before)
        hook.unlink();self.raw('publish-artifacts');self.assertEqual(self.tip,pending);self.assert_remote()

    def test_P08_live_evidence_descendant_adoption_and_divergence(self):
        self.initialize();writer=self.clone_writer();parent=self.tip
        rows=Store(writer).records(parent);path=f"runs/{self.root_key}/run-1/evidence/other.json"
        rows[path]=encoded({'fact':'another writer evidence'})
        remote=self.writer_checkpoint(writer,parent,rows)
        result=self.raw('publish-artifacts');self.assertEqual(result['artifact_publication']['outcome'],'remote-advance-adopted')
        self.assertEqual(self.tip,remote);self.raw('publish-artifacts')
        self.harness.store.fail='before-push';self.raw('command-intent',self.command());local=self.tip
        rows=Store(writer).records(remote);rows[path+'x.txt']=b'Competing evidence\n'
        # Use allowed filename; the suffix is text evidence, not task state.
        other=self.writer_checkpoint(writer,remote,rows)
        self.harness.store.fail=None;result=self.raw('publish-artifacts')
        self.assertEqual(result['artifact_publication']['status'],'incomplete')
        self.assertIn('divergent',result['artifact_publication']['detail'])
        self.assertEqual(self.tip,local);self.assertEqual(self.remote_tip(),other)
