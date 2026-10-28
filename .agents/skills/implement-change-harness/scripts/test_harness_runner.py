"""W15: discovery is complete; partial or stale execution cannot claim a pass."""
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_harness_tests as runner


class RunnerTests(unittest.TestCase):
    def fixture(self):
        ids = ['fixture.Tests.test_' + str(index) for index in range(len(runner.GROUPS))]
        inventory = {'candidate_sha': 'a' * 40, 'digest': 'inventory', 'attempt_token': 'fresh', 'created': 1,
                     'ids': sorted(ids), 'groups': {group: [ids[index]] for index, group in enumerate(runner.GROUPS)},
                     'scenarios': {ids[0]: ['boundary']}, 'original_method_mapping': {'old': [ids[0]]},
                     'test_mapping': {'W15': [ids[0]]}}
        results = {group: {'version': 1, 'group': group, 'candidate_sha': 'a' * 40, 'inventory_digest': 'inventory',
                          'attempt_token': 'fresh', 'started': 2, 'finished': 3, 'successful': True,
                          'selected': inventory['groups'][group], 'executed': inventory['groups'][group][:],
                          'statuses': {item: 'passed' for item in inventory['groups'][group]},
                          'scenarios': [{'test': ids[0], 'id': 'boundary', 'status': 'passed'}] if index == 0 else []}
                   for index, group in enumerate(runner.GROUPS)}
        return inventory, results

    def test_W15_union_success_and_all_hostile_result_controls(self):
        inventory, results = self.fixture()
        self.assertEqual(runner.verify_union(inventory, results)['outcome'], 'passed')
        group = runner.GROUPS[0]
        changes = {
            'omitted-group': lambda inv, r: r.pop(group),
            'duplicate-group': lambda inv, r: r[runner.GROUPS[1]].update(group=group),
            'wrong-candidate': lambda inv, r: r[group].update(candidate_sha='b' * 40),
            'stale-digest': lambda inv, r: r[group].update(inventory_digest='old'),
            'stale-attempt': lambda inv, r: r[group].update(attempt_token='old'),
            'stale-time': lambda inv, r: r[group].update(started=0),
            'failed-group': lambda inv, r: r[group].update(successful=False),
            'skipped-test': lambda inv, r: r[group]['statuses'].update({inv['ids'][0]: 'skipped'}),
            'unfinished-test': lambda inv, r: r[group]['statuses'].update({inv['ids'][0]: 'unfinished'}),
            'empty-result': lambda inv, r: r[group].update(executed=[]),
            'unexecuted-test': lambda inv, r: r[group].update(executed=['fixture.Tests.not_run']),
            'duplicate-test': lambda inv, r: r[group]['executed'].append(r[group]['executed'][0]),
            'unfinished-result': lambda inv, r: r[group].pop('finished'),
            'timeout': lambda inv, r: r[group].update(finished=603),
            'missing-scenario': lambda inv, r: r[group].update(scenarios=[]),
            'duplicate-scenario': lambda inv, r: r[group]['scenarios'].append(r[group]['scenarios'][0]),
            'failed-scenario': lambda inv, r: r[group]['scenarios'][0].update(status='failed'),
            'wrong-scenario-owner': lambda inv, r: r[group]['scenarios'][0].update(test='unknown'),
        }
        for name, change in changes.items():
            with self.subTest(control=name):
                inv, records = copy.deepcopy(inventory), copy.deepcopy(results)
                change(inv, records)
                with self.assertRaises(ValueError):runner.verify_union(inv, records)

    def test_W15_real_full_discovery_has_unique_ownership_without_fixtures(self):
        import test_delivery_wrapup
        import test_harness_migration
        with patch.object(test_delivery_wrapup.DeliveryTests, 'setUp', side_effect=AssertionError('fixture ran')), \
                patch.object(test_harness_migration.LiveFixture, 'setup_live', side_effect=AssertionError('fixture ran')):
            inventory = runner.new_inventory()
        self.assertGreater(len(inventory['ids']), 113)
        all_owned = [item for values in inventory['groups'].values() for item in values]
        self.assertEqual(sorted(all_owned), inventory['ids'])
        self.assertEqual(len(all_owned), len(set(all_owned)))
        self.assertEqual(len(inventory['original_method_mapping']), 113)
        self.assertEqual(len(inventory['original_method_mapping'][runner.OLD_W11]), 5)
        runner.check_inventory(inventory)
        for key in ('candidate_sha', 'digest'):
            changed = copy.deepcopy(inventory);changed[key] = 'wrong'
            with self.assertRaises(ValueError):runner.check_inventory(changed)
        changed = copy.deepcopy(inventory);changed['ids'].pop();changed.pop('digest');changed['digest'] = runner.digest(changed)
        with self.assertRaises(ValueError):runner.check_inventory(changed)

    def test_W15_actual_result_records_skip_failure_duplicate_and_success(self):
        class Probe(unittest.TestCase):
            def test_pass(self):
                with self.subTest(scenario_id='probe'):self.assertTrue(True)
            def test_skip(self):self.skipTest('control')
            def test_fail(self):self.fail('control')
        suite = unittest.TestSuite(Probe(name) for name in ('test_pass', 'test_skip', 'test_fail', 'test_pass'))
        result = unittest.TextTestRunner(stream=io.StringIO(), resultclass=runner.RecordingResult).run(suite)
        self.assertFalse(result.wasSuccessful())
        self.assertEqual(len(result.executed), 4)
        self.assertEqual(len(set(result.executed)), 3)
        self.assertEqual(set(result.statuses.values()), {'passed', 'skipped', 'failed'})
        self.assertEqual(len(result.scenarios), 2)

    def test_W15_exclusive_output_rejects_stale_results_and_guidance_lists_all_groups(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.json'
            runner.write_new(path, {'fresh': True})
            with self.assertRaises(FileExistsError):runner.write_new(path, {'fresh': False})
            self.assertEqual(json.loads(path.read_text()), {'fresh': True})
        guide = (Path(__file__).resolve().parents[4] / 'local-docs/verification.md').read_text()
        for group in runner.GROUPS:
            self.assertIn(group, guide)
        for flag in ('--inventory', '--run-group', '--verify-results', '600s'):
            self.assertIn(flag, guide)


if __name__ == '__main__':
    unittest.main()
