#!/usr/bin/env python3
"""Linux-only isolated research probes; does not edit project sources.

Run: python3 reproduce.py --repo /path/to/fossilize --out /tmp/fresh-probe-dir
The output directory must not already exist. Requires g++, Python 3 and /dev/full.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import resource
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    common = ['g++', '-std=c++14', '-O0', '-g', '-pthread', '-I.',
              '-Ikhronos', '-Irapidjson/include', '-Iminiz', '-Icli',
              '-Icli/volk', '-Icli/SPIRV-Headers/include/spirv/unified1']
    library = ['fossilize.cpp', 'fossilize_db.cpp', 'fossilize_application_filter.cpp',
               'varint.cpp', 'path.cpp', 'miniz/miniz.c']
    helpers = ['cli/cli_parser.cpp', 'cli/file.cpp']
    replay = ['cli/fossilize_replay.cpp', 'cli/device.cpp',
              'cli/fossilize_feature_filter.cpp', 'cli/volk/volk.c',
              'fossilize_external_replayer.cpp']

    def obj(source):
        return str(out / (source.replace('/', '_') + '.o'))

    def checked(command):
        result = subprocess.run(command, cwd=repo, capture_output=True, text=True)
        with (out / 'build.log').open('a') as log:
            log.write(json.dumps(command) + '\n' + result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'Build failed: {command}; see {out / "build.log"}')

    with ThreadPoolExecutor(max_workers=3) as pool:
        # Each command writes its own object. Retrieve every result.
        futures = [pool.submit(checked, common + ['-c', s, '-o', obj(s)])
                   for s in library + helpers + replay]
        for future in futures:
            future.result()
    base_objects = [obj(s) for s in library + helpers]
    probe_source = Path(__file__).with_name('probe.cpp')
    checked(common + ['-include', 'cli/logging.hpp', str(probe_source)]
            + base_objects + ['-o', str(out / 'probe')])
    for tool in ['convert_db', 'merge_db', 'list', 'rehash', 'prune']:
        checked(common + [f'cli/fossilize_{tool}.cpp'] + base_objects
                + ['-o', str(out / ('fossilize-' + tool.replace('_', '-')))])
    checked(['g++', '-pthread'] + [obj(s) for s in library + helpers + replay]
            + ['-ldl', '-lrt', '-o', str(out / 'fossilize-replay')])
    checked(common + ['test/fossilize_test.cpp'] + [obj(s) for s in library]
            + ['-o', str(out / 'fossilize-test')])

    results = []

    def run(label, command):
        result = subprocess.run([str(out / command[0])] + command[1:], cwd=out,
                                capture_output=True, text=True, timeout=30)
        record = dict(case=label, argv=[str(out / command[0])] + command[1:],
                      exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
        results.append(record)
        print(f'{label}: exit {result.returncode}', flush=True)
        return record

    invalid = str(out / 'invalid.foz')
    mismatch = str(out / 'version-mismatch.foz')
    run('recorder and late write failures', ['probe', invalid])
    # Retain the archive magic/name, then replace the fixed-size little-endian
    # payload header and payload. This intentionally creates a valid outer
    # stream containing one incompatible resource JSON version.
    raw = (out / 'invalid.foz').read_bytes()
    payload = b'{"version":999}'
    (out / 'version-mismatch.foz').write_bytes(
        raw[:56] + struct.pack('<IIII', len(payload), 1, 0, len(payload)) + payload)
    (out / 'truncated.foz').write_bytes(raw[:-1])
    (out / 'full.foz').symlink_to('/dev/full')
    cases = [
        ('normal null replay invalid selected compute', ['fossilize-replay', '--null-device', invalid]),
        ('master null replay invalid selected compute', ['fossilize-replay', '--null-device', '--master-process', '--num-threads', '1', invalid]),
        ('list connectivity invalid selected compute', ['fossilize-list', invalid, '--tag', '7', '--connectivity']),
        ('rehash invalid selected compute', ['fossilize-rehash', '--input-db', invalid, '--output-db', str(out / 'rehash.foz')]),
        ('convert late output failure', ['fossilize-convert-db', invalid, str(out / 'full.foz')]),
        ('list truncated selected input', ['fossilize-list', str(out / 'truncated.foz'), '--tag', '7']),
        ('normal unsupported format', ['fossilize-replay', '--null-device', mismatch]),
        ('master unsupported format', ['fossilize-replay', '--null-device', '--master-process', '--num-threads', '1', mismatch]),
        ('list unsupported format', ['fossilize-list', mismatch, '--tag', '7', '--connectivity']),
        ('rehash unsupported format', ['fossilize-rehash', '--input-db', mismatch, '--output-db', str(out / 'rehash-version.foz')]),
        ('prune unsupported format', ['fossilize-prune', '--input-db', mismatch, '--output-db', str(out / 'prune-version.foz')]),
        ('progress quiet child lost', ['fossilize-replay', '--null-device', '--progress', '--num-threads', '1', '--quiet-slave', invalid]),
    ]
    for label, command in cases:
        run(label, command)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    with (out / 'baseline-test.log').open('w') as log:
        baseline = subprocess.run([str(out / 'fossilize-test')], cwd=out,
                                  stdout=log, stderr=subprocess.STDOUT, timeout=60)
    metadata = dict(
        revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
        compiler=subprocess.check_output(['g++', '--version'], text=True).splitlines()[0],
        baseline_test_exit=baseline.returncode,
        note='Direct debug builds; no GPU; SPIR-V validation disabled; observations, not passing regression assertions.')
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
