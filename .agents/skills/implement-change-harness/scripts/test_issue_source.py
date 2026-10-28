"""S01–S03 subprocess gh authority/counterexample checks."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from github_issues import GitHub, IssueError


class ServiceFixture:
    def setup_service(self):
        self.service_tmp=tempfile.TemporaryDirectory(prefix='issue-service-');self.addCleanup(self.service_tmp.cleanup)
        self.service_root=Path(self.service_tmp.name)
        self.service_file=self.service_root/'service.json';self.service_file.write_text(json.dumps({'issues':{}}))
        executable=self.service_root/'gh'
        executable.write_text('#!/bin/sh\nexec python3 "'+str(Path(__file__).with_name('fake_gh_fixture.py'))+'" "$@"\n');executable.chmod(0o755)
        self.environment=patch.dict(os.environ,{'FAKE_GH_STATE':str(self.service_file),'PATH':str(self.service_root)+os.pathsep+os.environ['PATH'],'PYTHONDONTWRITEBYTECODE':'1'})
        self.environment.start();self.addCleanup(self.environment.stop)
        self.gh=GitHub()

    def service(self):return json.loads(self.service_file.read_text())
    def update(self,**values):
        state=self.service();state.update(values);self.service_file.write_text(json.dumps(state))
    def create(self,title='Root',body='Requirements AC1'):
        return self.gh.api('issues','POST',{'title':title,'body':body})


class IssueSourceTests(ServiceFixture,unittest.TestCase):
    def setUp(self):self.setup_service()

    def test_S01_url_numeric_full_identity_paginated_snapshot(self):
        root=self.create();a=self.create('A');b=self.create('B')
        self.gh.api(f"issues/{root['number']}/dependencies/blocked_by",'POST',{'issue_id':a['id']})
        self.gh.api(f"issues/{root['number']}/dependencies/blocked_by",'POST',{'issue_id':b['id']})
        one=self.gh.snapshot(root['number']);two=self.gh.snapshot(root['html_url'])
        self.assertEqual(one,two);self.assertEqual(len(one['nodes']),3);self.assertFalse(self.gh.ready(one))
        self.assertEqual(one['nodes'][one['root']]['id'],root['id'])
        self.assertTrue(all('robinwassen/Fossilize' in next(arg for arg in call if arg.startswith('repos/')) for call in self.service()['calls'] if 'graphql' not in call))

    def test_S01_wrong_repo_pr_auth_and_cycle_fail_closed(self):
        root=self.create();a=self.create('A')
        for invalid in ['https://github.com/ValveSoftware/Fossilize/issues/8','https://github.com/robinwassen/Fossilize/pull/8','8?x',0]:
            with self.assertRaises(IssueError):self.gh.snapshot(invalid)
        state=self.service();state['issues'][str(root['number'])]['pull_request']={};self.service_file.write_text(json.dumps(state))
        with self.assertRaises(IssueError):self.gh.snapshot(root['number'])
        state=self.service();del state['issues'][str(root['number'])]['pull_request'];self.service_file.write_text(json.dumps(state))
        self.update(auth_failure=True)
        with self.assertRaises(IssueError):self.gh.snapshot(root['number'])
        self.gh.api(f"issues/{root['number']}/dependencies/blocked_by",'POST',{'issue_id':a['id']})
        self.gh.api(f"issues/{a['number']}/dependencies/blocked_by",'POST',{'issue_id':root['id']})
        with self.assertRaisesRegex(IssueError,'cyclic'):self.gh.snapshot(root['number'])

    def test_S03_transitive_cancelled_readiness_and_material_drift(self):
        root=self.create();a=self.create('A');b=self.create('B')
        self.gh.api(f"issues/{root['number']}/dependencies/blocked_by",'POST',{'issue_id':a['id']})
        self.gh.api(f"issues/{a['number']}/dependencies/blocked_by",'POST',{'issue_id':b['id']})
        for row in (a,b):self.gh.api(f"issues/{row['number']}",'PATCH',{'state':'closed','state_reason':'completed'})
        before=self.gh.snapshot(root['number']);self.assertTrue(self.gh.ready(before))
        self.gh.api(f"issues/{b['number']}",'PATCH',{'state_reason':'not_planned'})
        self.assertFalse(self.gh.ready(self.gh.snapshot(root['number'])))
        with self.assertRaises(IssueError):self.gh.unchanged(before)
        self.gh.api(f"issues/{b['number']}",'PATCH',{'state_reason':'completed'})
        self.gh.api(f"issues/{root['number']}",'PATCH',{'title':'Conservative title change invalidates snapshot'})
        with self.assertRaises(IssueError):self.gh.unchanged(before)

    def test_S03_parent_membership_is_not_blocking(self):
        parent=self.create();root=self.create()
        self.gh.api(f"issues/{parent['number']}/sub_issues",'POST',{'sub_issue_id':root['id']})
        snap=self.gh.snapshot(root['number']);self.assertTrue(self.gh.ready(snap));self.assertEqual(snap['nodes'][snap['root']]['parent'],parent['html_url'])
