#!/usr/bin/env python3
"""Linux research probes. A matched defect is an observation, not product conformance.
Builds in a new --out directory; never starts Steam or uses GPU workloads.
"""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import resource
import subprocess

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=HERE.parents[2])
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    # Extracted helpers are historical evidence; refuse to test stale copies after source edits.
    production_linux = (repo / 'cli/fossilize_replay_linux.hpp').read_text()
    def function_text(text, signature):
        start = text.index(signature)
        return text[start:text.index('\n}', start) + 2].strip()
    helper_signatures = {
        'blacklist_pipe.cpp': ['static bool write_all(', 'static void send_faulty_modules_and_close('],
        'blacklist_control.cpp': ['static bool write_all(', 'static void send_faulty_modules_and_close('],
        'stale_pipe.cpp': ['bool ProcessProgress::process_once('],
        'cancel_control.cpp': ['bool ProcessProgress::process_shutdown(', 'static void shutdown_processes('],
        'cancel_live_control.cpp': ['bool ProcessProgress::process_shutdown(', 'static void shutdown_processes('],
    }
    for source, signatures in helper_signatures.items():
        for signature in signatures:
            if function_text(production_linux, signature) != function_text((HERE / source).read_text(), signature):
                raise RuntimeError('Extracted helper no longer matches production: ' + source + ': ' + signature)
    common = ['g++', '-std=c++14', '-O0', '-g', '-pthread', '-I.', '-Ikhronos']
    commands = []
    def build(name, source, extra=(), production=False):
        cmd = common + list(extra) + [str(HERE / source)]
        if production:
            cmd += ['fossilize_external_replayer.cpp', 'path.cpp', '-lrt']
        cmd += ['-o', str(out / name)]
        p = subprocess.run(cmd, cwd=repo, text=True, capture_output=True, timeout=120)
        commands.append(dict(argv=cmd, exit=p.returncode, stdout=p.stdout, stderr=p.stderr))
        (out / 'build.json').write_text(json.dumps(commands, indent=2) + '\n')
        if p.returncode:
            raise RuntimeError('Build failed: ' + name)
    build('external', 'external_probe.cpp', ['-Wl,--wrap=pipe'], True)
    build('owner-lock', 'owner_lock.cpp', production=True)
    build('cancel-actual', 'cancel_actual.cpp', [
        '-ffunction-sections', '-fdata-sections', '-Irapidjson/include', '-Iminiz',
        '-Icli', '-Icli/volk', '-Icli/SPIRV-Headers/include/spirv/unified1',
        '-Wl,--gc-sections', '-ldl'])
    for name in ['blacklist_pipe', 'stale_pipe', 'cancel_control',
                 'cancel_live_control', 'blacklist_control']:
        build(name, name + '.cpp')
    cases = [
        ('external', ['wait-interrupt'], 1, ['wait_status=0 child_alive=1 handle=-1']),
        ('external', ['thread'], 0, ['host_alive=1 child_signaled=1 signal=9']),
        ('external', ['status'], 1, ['done=1 status=-10 expected=37']),
        ('external', ['wait-steals-child'], 1, ['wait_status=42', 'unrelated_waitpid=-1', 'unrelated_errno=10']),
        ('external', ['pipe-failure'], 0, ['start=0', 'leaked=2']),
        ('external', ['inherited-control'], 0, ['after_A_destroyed_A_alive=1 after_B_killed_A_exited=1']),
        ('owner-lock', [], 0, ['poll_returned=0 subject_alive=1', 'wait_channel=futex']),
        ('cancel-actual', [], 0, ['production_shutdown_called=1', 'expect_kill=1 replacement_requested=1']),
        ('blacklist_pipe', [], 0, ['returned_before_SIGTERM=0 returned_after_SIGTERM=0 controller_exited=0']),
        ('stale_pipe', [], 0, ['stale_event_dispatch_returned=0']),
        ('cancel_control', [], 0, ['cancelled=1 actual_exit=2 restart_requested=1']),
        ('cancel_live_control', [], 0, ['restart_requested=0']),
        ('blacklist_control', [], 0, ['returned_before_SIGTERM=1 returned_after_SIGTERM=1']),
    ]
    results = []
    for exe, flags, expected_exit, fragments in cases:
        cmd = [str(out / exe)] + flags
        p = subprocess.run(cmd, cwd=out, capture_output=True, text=True, timeout=10)
        matched = p.returncode == expected_exit and all(s in p.stdout for s in fragments)
        results.append(dict(case=' '.join([exe] + flags), argv=cmd, exit=p.returncode,
                            expected_exit=expected_exit, stdout=p.stdout, stderr=p.stderr,
                            matched_expected_observation=matched))
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(results[-1]['case'] + ': ' + ('observation matched' if matched else 'UNEXPECTED'), flush=True)
    paths = ['cli/fossilize_replay.cpp', 'cli/fossilize_replay_linux.hpp',
             'cli/fossilize_replay_windows.hpp', 'fossilize_external_replayer.cpp',
             'fossilize_external_replayer.hpp', 'fossilize_external_replayer_linux.hpp',
             'fossilize_external_replayer_windows.hpp', 'platform/futex_wrapper_linux.hpp']
    metadata = dict(revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
                    kernel=platform.release(), compiler=subprocess.check_output(['g++', '--version'], text=True).splitlines()[0],
                    source_sha256={p: hashlib.sha256((repo / p).read_bytes()).hexdigest() for p in paths},
                    scope='Linux CPU-only source/API/mechanism probes; no full CLI, Steam, GPU, Windows, Apple or Android runtime validation.',
                    note='Exit1 from three external probes is the expected violated product assertion. Matched observations do not mean product tests pass.')
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    if not all(r['matched_expected_observation'] for r in results):
        raise SystemExit('Unexpected observation; inspect results.json')

if __name__ == '__main__':
    main()
