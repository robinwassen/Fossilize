#!/usr/bin/env python3
"""Complete bounded unittest groups, with fresh discovery and exact-union proof.

Inventory imports test modules but never runs fixtures. Each group is a separate
process using isolated tempfile fixtures. Evidence files are exclusive-created;
reuse of stale attempt output is rejected rather than silently overwritten.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import unittest
import uuid

SCRIPTS = Path(__file__).resolve().parent
LEGACY_MODULES = ('test_harness_artifacts', 'test_harness_closeout', 'test_harness_issue_artifacts',
                  'test_harness_issue_closeout', 'test_harness_migration', 'test_harness_state',
                  'test_issue_guidance', 'test_issue_migration', 'test_issue_source')
GROUPS = tuple('regression-' + module for module in LEGACY_MODULES) + (
    'delivery-ordinary', 'delivery-w09', 'delivery-w11-0', 'delivery-w11-1',
    'delivery-w11-2', 'delivery-w11-3', 'regression-extra')
OLD_W11 = 'test_delivery_wrapup.DeliveryTests.test_W11_every_checkpoint_publication_response_loss_and_rejection'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def flattened(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flattened(item)
        else:
            yield item


def owner(test_id):
    module, _, method = test_id.split('.', 2)
    if module in LEGACY_MODULES:
        return 'regression-' + module
    if module == 'test_delivery_wrapup':
        match = re.fullmatch(r'test_W11_publication_shard_([0-3])', method)
        if match:
            return 'delivery-w11-' + match[1]
        if method == 'test_W09_W10_every_effect_boundary_process_death_and_recovery':
            return 'delivery-w09'
        return 'delivery-ordinary'
    return 'regression-extra'


def discovery():
    loader = unittest.TestLoader()
    tests = list(flattened(loader.discover(str(SCRIPTS), pattern='test_*.py')))
    require(not loader.errors, 'unittest discovery errors: ' + '\n'.join(loader.errors))
    ids = [test.id() for test in tests]
    require(ids and len(ids) == len(set(ids)), 'empty or duplicate full discovery')
    return {test.id(): test for test in tests}


def specification():
    tests = discovery()
    ids = sorted(tests)
    import test_delivery_wrapup as delivery
    scenarios = {f'test_delivery_wrapup.DeliveryTests.{method}': values
                 for method, values in delivery.scenario_inventory().items()}
    require(set(scenarios) <= set(ids), 'scenario owner absent from discovery')
    scenario_ids = [scenario for values in scenarios.values() for scenario in values]
    require(len(scenario_ids) == len(set(scenario_ids)), 'duplicate scenario owner')
    original = json.loads((SCRIPTS / 'harness_original_tests.json').read_text())
    original_ids = [item for values in original.values() for item in values]
    require(len(original_ids) == 113 and len(set(original_ids)) == 113, 'original 113-method accounting changed')
    replacement = ['test_delivery_wrapup.DeliveryTests.test_W11_publication_inventory_and_rejection'] + [
        f'test_delivery_wrapup.DeliveryTests.test_W11_publication_shard_{index}' for index in range(4)]
    compatibility = {item: replacement if item == OLD_W11 else [item] for item in original_ids}
    require(all(set(values) <= set(ids) for values in compatibility.values()), 'original method removed without coverage')
    mapping = {f'W{case:02}': [test_id for test_id in ids if re.search(r'(?:_|^)' + f'W{case:02}' + r'(?:_|$)', test_id.rsplit('.', 1)[1])]
               for case in range(1, 16)}
    require(all(mapping.values()), 'planned W01-W15 case lacks discovered test')
    return tests, {'ids': ids, 'groups': {group: [item for item in ids if owner(item) == group] for group in GROUPS},
                   'scenarios': scenarios, 'test_mapping': mapping, 'original_method_mapping': compatibility}


def candidate():
    return subprocess.check_output(['git', '-C', str(SCRIPTS), 'rev-parse', 'HEAD'], text=True).strip()


def new_inventory():
    _, spec = specification()
    result = dict(spec, version=1, candidate_sha=candidate(), attempt_token=uuid.uuid4().hex, created=time.time())
    result['digest'] = digest(result)
    return result


def check_inventory(inventory):
    require(isinstance(inventory, dict), 'invalid inventory')
    unsigned = dict(inventory)
    actual_digest = unsigned.pop('digest', None)
    require(actual_digest == digest(unsigned), 'inventory digest differs')
    require(inventory.get('version') == 1 and inventory.get('attempt_token'), 'inventory identity missing')
    require(inventory.get('candidate_sha') == candidate(), 'wrong candidate inventory')
    tests, spec = specification()
    require(all(inventory.get(key) == value for key, value in spec.items()), 'fresh full discovery differs from inventory')
    return tests


class RecordingResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.executed = []
        self.statuses = {}
        self.scenarios = []

    def startTest(self, test):
        self.executed.append(test.id())
        self.statuses[test.id()] = 'unfinished'
        super().startTest(test)

    def addSuccess(self, test):
        self.statuses[test.id()] = 'passed'
        super().addSuccess(test)

    def addFailure(self, test, error):
        self.statuses[test.id()] = 'failed'
        super().addFailure(test, error)

    def addError(self, test, error):
        self.statuses[test.id()] = 'error'
        super().addError(test, error)

    def addSkip(self, test, reason):
        self.statuses[test.id()] = 'skipped'
        super().addSkip(test, reason)

    def addExpectedFailure(self, test, error):
        self.statuses[test.id()] = 'expected-failure'
        super().addExpectedFailure(test, error)

    def addUnexpectedSuccess(self, test):
        self.statuses[test.id()] = 'unexpected-success'
        super().addUnexpectedSuccess(test)

    def addSubTest(self, test, subtest, error):
        if 'scenario_id' in subtest.params:
            self.scenarios.append({'test': test.id(), 'id': subtest.params['scenario_id'],
                                   'status': 'passed' if error is None else 'failed'})
        if error is not None:
            self.statuses[test.id()] = 'failed'
        super().addSubTest(test, subtest, error)


def write_new(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x') as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write('\n')


def run_group(group, inventory):
    tests = check_inventory(inventory)
    require(group in GROUPS, 'unknown group')
    selected = inventory['groups'][group]
    started = time.time()
    result = unittest.TextTestRunner(verbosity=2, resultclass=RecordingResult).run(
        unittest.TestSuite(tests[item] for item in selected))
    return {'version': 1, 'group': group, 'candidate_sha': candidate(), 'inventory_digest': inventory['digest'],
            'attempt_token': inventory['attempt_token'], 'started': started, 'finished': time.time(),
            'selected': selected, 'executed': result.executed, 'statuses': result.statuses,
            'scenarios': result.scenarios, 'successful': result.wasSuccessful() and
            all(status == 'passed' for status in result.statuses.values())}


def verify_union(inventory, results):
    """Validate actual execution records; pure core is covered by hostile controls."""
    require(set(results) == set(GROUPS), 'missing or extra group results')
    executed, scenario_ids = [], []
    for group in GROUPS:
        result = results[group]
        selected = inventory['groups'][group]
        require(result.get('version') == 1 and result.get('group') == group, 'wrong group identity')
        require(result.get('candidate_sha') == inventory['candidate_sha'], 'wrong candidate result')
        require(result.get('inventory_digest') == inventory['digest'] and result.get('attempt_token') == inventory['attempt_token'], 'stale inventory result')
        require(isinstance(result.get('started'), (float, int)) and isinstance(result.get('finished'), (float, int)) and
                inventory['created'] <= result['started'] <= result['finished'] and result['finished'] - result['started'] <= 600,
                'unfinished, stale or over-deadline result')
        require(result.get('successful') is True, 'failed group')
        require(result.get('selected') == selected and sorted(result.get('executed', [])) == selected and
                len(result['executed']) == len(set(result['executed'])), 'missing, duplicate, empty or unexecuted test')
        require(result.get('statuses') == {item: 'passed' for item in selected}, 'skipped, failed or unfinished test')
        expected = {(item, scenario) for item in selected for scenario in inventory['scenarios'].get(item, [])}
        actual = [(row.get('test'), row.get('id')) for row in result.get('scenarios', [])]
        require(len(actual) == len(set(actual)) and set(actual) == expected and
                all(row.get('status') == 'passed' for row in result['scenarios']), 'missing, duplicate or failed boundary scenario')
        executed.extend(result['executed'])
        scenario_ids.extend(row['id'] for row in result['scenarios'])
    require(sorted(executed) == inventory['ids'] and len(executed) == len(set(executed)), 'execution union differs from discovery')
    expected_scenarios = [item for values in inventory['scenarios'].values() for item in values]
    require(Counter(scenario_ids) == Counter(expected_scenarios), 'scenario union differs from inventory')
    return {'outcome': 'passed', 'candidate_sha': inventory['candidate_sha'], 'inventory_digest': inventory['digest'],
            'discovered': inventory['ids'], 'executed': sorted(executed), 'scenarios': sorted(scenario_ids),
            'groups': list(GROUPS), 'original_methods_accounted': len(inventory['original_method_mapping']),
            'test_mapping': inventory['test_mapping']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', nargs='?', const=True)
    parser.add_argument('--run-group', choices=GROUPS)
    parser.add_argument('--verify-results')
    parser.add_argument('--output')
    args = parser.parse_args()
    try:
        if args.verify_results:
            require(not args.inventory and not args.run_group and not args.output, 'invalid verifier arguments')
            directory = Path(args.verify_results)
            inventory = json.loads((directory / 'inventory.json').read_text())
            check_inventory(inventory)
            results = {group: json.loads((directory / (group + '.json')).read_text()) for group in GROUPS}
            result = verify_union(inventory, results)
            write_new(directory / 'union.json', result)
        elif args.run_group:
            require(isinstance(args.inventory, str) and args.output, 'group needs inventory path and output')
            require(not Path(args.output).exists(), 'refuse stale output')
            result = run_group(args.run_group, json.loads(Path(args.inventory).read_text()))
            write_new(args.output, result)
            require(result['successful'], 'group did not pass')
        else:
            require(args.inventory is True and args.output, 'inventory needs --inventory --output PATH')
            result = new_inventory()
            write_new(args.output, result)
        print(json.dumps({'outcome': result.get('outcome', 'recorded'), 'candidate_sha': result['candidate_sha'],
                          'output': args.output or str(Path(args.verify_results) / 'union.json')}, sort_keys=True))
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
