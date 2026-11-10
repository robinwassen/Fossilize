#!/usr/bin/env python3
"""Bounded explicit-assertion controls. Configure/build are separate published commands.

--mutate-source modifies one disposable candidate copy; it never edits tests or builds.
--check-mutation runs the compiled mutated runner and its healthy control.
"""
import argparse
import json
import pathlib
import subprocess

MUTATIONS = {
    'sliced-tail': ('fossilize_db.cpp',
                    'observation.state = DiscoveryState::RecoveredPrefix;',
                    'observation.state = DiscoveryState::Complete;', 2,
                    'database-discovery-sensitivity', 'truncated-zero', 'empty',
                    'D20/truncation-never-full-coverage'),
    'required-input': ('fossilize_db.cpp',
                       'if (input.required && input.state != DiscoveryState::Complete &&',
                       'if (false && input.required && input.state != DiscoveryState::Complete &&', 1,
                       'database-discovery-sensitivity', 'inputs', 'empty',
                       'D20/invalid-no-invented-errno'),
    'unread-total': ('fossilize_db.cpp',
                     'report.unread_total_known = report.discovery_complete;',
                     'report.unread_total_known = true;', 1,
                     'database-discovery-sensitivity', 'truncated-zero', 'empty',
                     'D20/truncation-never-full-coverage'),
    'selection-exclusions': ('cli/replay_selection.hpp',
                             'report.scope_satisfied &= report.hash_found;',
                             'report.excluded.clear();\n\treport.scope_satisfied &= report.hash_found;', 1,
                             'replay-selection-sensitivity', 'ranges', 'prefix-policy',
                             'D20/range-exact-selection-and-exclusions'),
    'metadata-import': ('fossilize_db.cpp',
                        'this->report = std::move(report);',
                        'for (auto &input : report.inputs) { input.state=DiscoveryState::Complete; input.total_known=true; }\n\tthis->report = std::move(report);', 1,
                        'database-discovery-sensitivity', 'metadata', 'empty',
                        'D20/metadata-no-false-completeness'),
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inventory', action='store_true')
    p.add_argument('--mutate-source', type=pathlib.Path)
    p.add_argument('--check-mutation', choices=MUTATIONS)
    p.add_argument('--mutation', choices=MUTATIONS)
    p.add_argument('--runner', type=pathlib.Path)
    p.add_argument('--case')
    p.add_argument('--healthy-case')
    p.add_argument('--expect-assertion')
    p.add_argument('--output', type=pathlib.Path)
    a = p.parse_args()
    if a.inventory:
        result = {'registered_ids': ['replay-discovery-original-defect',
                                    'database-discovery-sensitivity',
                                    'replay-selection-sensitivity'],
                  'mutations': {name: {'file': row[0], 'before': row[1], 'after': row[2],
                                       'expected_replacements': row[3], 'id': row[4],
                                       'case': row[5], 'healthy_case': row[6],
                                       'expect_assertion': row[7]}
                                for name, row in MUTATIONS.items()}}
    elif a.mutate_source and a.mutation:
        row = MUTATIONS[a.mutation]
        path = a.mutate_source / row[0]
        data = path.read_text()
        if data.count(row[1]) != row[3]:
            p.error('mutation anchor count changed; reconcile the exact patch')
        path.write_text(data.replace(row[1], row[2]))
        result = {'mutation': a.mutation, 'path': str(path), 'before': row[1],
                  'after': row[2], 'replacements': row[3]}
    elif a.check_mutation and a.runner and a.output:
        row = MUTATIONS[a.check_mutation]
        case, healthy, assertion = a.case or row[5], a.healthy_case or row[6], a.expect_assertion or row[7]
        work = a.output.parent / (a.output.stem + '-work')
        work.mkdir(parents=True, exist_ok=False)
        runs = []
        for label, selected in [('healthy', healthy), ('mutated', case)]:
            directory = work / label
            directory.mkdir()
            try:
                process = subprocess.run([str(a.runner.resolve()), selected], cwd=directory,
                                         text=True, capture_output=True, timeout=90)
                runs.append({'kind': label, 'case': selected, 'command': process.args,
                             'cwd': str(directory), 'exit_code': process.returncode,
                             'stdout': process.stdout, 'stderr': process.stderr})
            except subprocess.TimeoutExpired:
                runs.append({'kind': label, 'case': selected, 'timed_out': True})
        passed = (runs[0].get('exit_code') == 0 and runs[1].get('exit_code') == 1
                  and ('FAIL ' + assertion) in runs[1].get('stdout', ''))
        result = {'id': row[4], 'mutation': a.check_mutation, 'expected_assertion': assertion,
                  'passed': passed, 'runs': runs}
        a.output.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({'id': row[4], 'mutation': a.check_mutation, 'passed': passed}))
        return 0 if passed else 1
    else:
        p.error('choose inventory, mutate-source/mutation, or check-mutation/runner/output')
    text = json.dumps(result, indent=2) + '\n'
    if a.output:
        a.output.write_text(text)
    else:
        print(text, end='')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
