#!/usr/bin/env python3
"""Deterministic, bounded whole-compiler mutations. Never execute generated code.

Unlike fuzz_frontend.py, this exercises the immutable compiler through IR emission
and independently verifies successful artifacts with llvm-as. Run with explicit
--compiler, --compiler-sha256, --llvm-as and --artifacts paths. All seed snapshots,
mutations, source hashes, commands, diagnostics and artifacts are retained.
"""
import argparse
import ast
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import resource
import selectors
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
RANDOM_SEED = 0x535441534841
TOKENS = ['(', ')', '[', ']', '{', '}', ';', ':', ',', '0x',
          '18446744073709551616', '/*', '*/', '!', '@', 'stack', 'fn',
          '?', '"', '[]', 'ret', 'void', 'nil', '*rw', '..', '=>']
FIXTURES = [
    'tests/diag/warn_unused_variable.sts',
    'tests/diag/error_missing_semicolon.sts',
    'tests/diag/parser_recovery_multi_error.sts',
    'tests/cleanup/aggregate_multi_return.sts',
    'tests/cleanup/task_return_values.sts',
    'tests/cleanup/defer_nested_control.sts',
    'tests/diag/coro_mixed_yield_types.sts',
    'tests/cleanup/nested_switch.sts',
    'tests/cleanup/for_init_lifetime.sts',
    'tests/cleanup/lexical_zones.sts',
    'tests/fileheaders/section_placement.sts',
]
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
INTERNAL = re.compile(
    r'LLVM IR verification failed|LLVM ERROR:|Broken (?:function|module) found|'
    r'internal compiler error|Assertion .* failed|Assertion failed|'
    r'(?:Address|UndefinedBehavior|Leak|Memory|Thread)Sanitizer|'
    r'runtime error:|Segmentation fault|Bus error|stack dump:|'
    r'fatal signal|compiler crash|LLVM (?:coroutine|optimization) pass.*failed', re.I)
ERROR = re.compile(r'^error(?:\[[^\]]+\])?:', re.M)
LEXEME = re.compile(r'//[^\n]*|/\*.*?\*/|\s+|[A-Za-z_][A-Za-z_0-9]*|'
                    r'\d+|[^\s]', re.S)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def run_bounded(command, cwd, prefix, timeout, output_limit):
    """Drain bounded pipes; kill the process group on wall/output resource limits."""
    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_CPU, (math.ceil(timeout), math.ceil(timeout) + 1))
        resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))
        resource.setrlimit(resource.RLIMIT_FSIZE, (8 << 20, 8 << 20))
    env = os.environ.copy()
    for key in ('STS_DUMP_IR', 'STS_DUMP_IR_PRE'):
        env.pop(key, None)
    env.update({'LC_ALL': 'C', 'NO_COLOR': '1'})
    start = time.monotonic()
    streams = {'stdout': bytearray(), 'stderr': bytearray()}
    reason = None
    try:
        process = subprocess.Popen(command, cwd=cwd, env=env,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            start_new_session=True, preexec_fn=limits)
    except OSError as error:
        return {'command': command, 'returncode': None, 'reason': 'launch-error',
                'elapsed_seconds': time.monotonic() - start, 'error': str(error)}
    selector = selectors.DefaultSelector()
    for name, pipe in (('stdout', process.stdout), ('stderr', process.stderr)):
        os.set_blocking(pipe.fileno(), False)
        selector.register(pipe, selectors.EVENT_READ, name)
    while selector.get_map():
        if time.monotonic() - start >= timeout and reason is None:
            reason = 'timeout'
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
        for key, _ in selector.select(0.025):
            block = os.read(key.fileobj.fileno(), 16384)
            if not block:
                selector.unregister(key.fileobj)
                key.fileobj.close()
                continue
            stream = streams[key.data]
            room = output_limit - len(stream)
            stream.extend(block[:max(0, room)])
            if len(block) > room and reason is None:
                reason = 'output-limit'
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
    selector.close()
    returncode = process.wait()
    record = {'command': command, 'returncode': returncode, 'reason': reason,
              'elapsed_seconds': round(time.monotonic() - start, 6)}
    for name, data in streams.items():
        output = prefix.with_suffix('.' + name)
        output.write_bytes(data)
        record[name + '_path'] = str(output)
        record[name + '_bytes'] = len(data)
        record[name + '_sha256'] = digest(data)
    return record


def compile_case(case, source, args):
    case.mkdir()
    input_file = case / 'input.sts'
    output_file = case / 'result.ll'
    input_file.write_text(source)
    command = [str(args.compiler), 'ir', str(input_file), '-o', str(output_file), '-o=0']
    process = run_bounded(command, case, case / 'compile', args.timeout, args.output_limit)
    text = ''.join(Path(process[k + '_path']).read_text(errors='replace')
                   for k in ('stdout', 'stderr') if k + '_path' in process)
    text = ANSI.sub('', text)
    issues = []
    rc = process['returncode']
    if process['reason']:
        issues.append(process['reason'])
    if rc not in (0, 1):
        issues.append('abnormal-exit:' + str(rc))
    match = INTERNAL.search(text)
    if match:
        issues.append('internal-failure:' + match.group())
    verification = None
    if rc == 0:
        if ERROR.search(text):
            issues.append('success-with-error-diagnostic')
        if not output_file.is_file() or output_file.stat().st_size == 0:
            issues.append('success-without-nonempty-artifact')
        elif not issues:
            verification = run_bounded([str(args.llvm_as), str(output_file), '-o',
                                       str(case / 'verified.bc')], case, case / 'verify',
                                       args.timeout, args.output_limit)
            if verification['returncode'] != 0 or verification['reason']:
                issues.append('invalid-ir-artifact')
    elif rc == 1:
        if not ERROR.search(text):
            issues.append('rejection-without-error-diagnostic')
        if output_file.exists():
            issues.append('rejection-left-artifact')
    result = {'source_sha256': digest(source.encode()), 'source_bytes': len(source.encode()),
              'process': process, 'issues': issues, 'verification': verification,
              'status': 'failure' if issues else 'accepted' if rc == 0 else 'rejected'}
    if output_file.is_file():
        result['artifact_sha256'] = digest(output_file.read_bytes())
        result['artifact_bytes'] = output_file.stat().st_size
    write_json(case / 'result.json', result)
    return result


def load_seeds():
    path = ROOT / 'tests/frontend/fuzz_frontend.py'
    tree = ast.parse(path.read_text())
    existing = next(ast.literal_eval(node.value) for node in tree.body
                    if isinstance(node, ast.Assign) and any(
                        isinstance(target, ast.Name) and target.id == 'seeds'
                        for target in node.targets))
    seeds = [{'origin': 'tests/frontend/fuzz_frontend.py:seeds[' + str(i) + ']',
              'source': source, 'fixture_sha256': digest(path.read_bytes())}
             for i, source in enumerate(existing)]
    for name in FIXTURES:
        path = ROOT / name
        seeds.append({'origin': name, 'source': path.read_text(),
                      'fixture_sha256': digest(path.read_bytes())})
    return seeds


def mutate(source, rng, token_boundaries):
    edits = []
    for _ in range(1 + rng.randrange(3)):
        if token_boundaries:
            starts = [m.start() for m in LEXEME.finditer(source)
                      if not m.group().isspace() and not m.group().startswith(('//', '/*'))]
            position = rng.choice(starts + [len(source)])
        else:
            position = rng.randrange(len(source) + 1)
        operation = rng.randrange(3)
        remove = 0 if operation == 0 else rng.randrange(1, 8)
        insert = '' if operation == 1 else rng.choice(TOKENS)
        deleted = source[position:position + remove]
        updated = source[:position] + insert + source[position + remove:]
        edits.append({'operation': ('insert', 'delete', 'replace')[operation],
                      'position': position, 'deleted': deleted, 'inserted': insert,
                      'before_sha256': digest(source.encode()),
                      'after_sha256': digest(updated.encode())})
        source = updated
    return source, edits


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--compiler-sha256', required=True)
    p.add_argument('--llvm-as', type=Path, required=True)
    p.add_argument('--artifacts', type=Path, required=True,
                   help='New, nonexistent output directory (will not overwrite a prior run).')
    p.add_argument('--timeout', type=float, default=3.0)
    p.add_argument('--output-limit', type=int, default=256 * 1024)
    args = p.parse_args()
    args.compiler = args.compiler.resolve()
    args.llvm_as = args.llvm_as.resolve()
    args.artifacts = args.artifacts.resolve()
    if args.timeout <= 0 or args.output_limit < 1024:
        p.error('invalid resource limits')
    actual = digest(args.compiler.read_bytes())
    if actual != args.compiler_sha256:
        p.error('compiler checksum mismatch: ' + actual)
    args.artifacts.mkdir(parents=True, exist_ok=False)
    corpus_dir = args.artifacts / 'seeds'
    corpus_dir.mkdir()
    seeds = load_seeds()
    for index, seed in enumerate(seeds):
        path = corpus_dir / f'seed-{index:02d}.sts'
        path.write_text(seed['source'])
        seed.update({'index': index, 'snapshot': str(path),
                     'source_sha256': digest(seed['source'].encode())})
    metadata = {'schema': 1, 'started_utc': datetime.now(timezone.utc).isoformat(),
                'compiler': str(args.compiler), 'compiler_sha256': actual,
                'llvm_as': str(args.llvm_as), 'llvm_as_sha256': digest(args.llvm_as.read_bytes()),
                'random_seed': RANDOM_SEED, 'random_seed_hex': hex(RANDOM_SEED),
                'mutation_count': 256, 'mutations_per_case': [1, 3],
                'timeout_seconds': args.timeout, 'output_limit_bytes_per_stream': args.output_limit,
                'address_space_limit_bytes': 2 << 30, 'file_limit_bytes': 8 << 20,
                'command_mode': 'ir -o=0; generated code never executed',
                'harness_sha256': digest(Path(__file__).read_bytes()),
                'seeds': [{k: v for k, v in s.items() if k != 'source'} for s in seeds]}
    write_json(args.artifacts / 'manifest.json', metadata)
    controls = []
    control_dir = args.artifacts / 'controls'
    control_dir.mkdir()
    for i, seed in enumerate(seeds):
        result = compile_case(control_dir / f'seed-{i:02d}', seed['source'], args)
        result['seed_index'] = i
        controls.append(result)
        if result['issues']:
            print(f'FAIL control {i}: ' + '; '.join(result['issues']), flush=True)
    rng = random.Random(RANDOM_SEED)
    results = []
    cases_dir = args.artifacts / 'mutations'
    cases_dir.mkdir()
    for case_number in range(256):
        seed_index = case_number % len(seeds)
        source, edits = mutate(seeds[seed_index]['source'], rng, case_number % 2 == 0)
        case = cases_dir / f'case-{case_number:03d}'
        result = compile_case(case, source, args)
        mutation = {'case_number': case_number, 'seed_index': seed_index,
                    'token_boundary_positions': case_number % 2 == 0, 'edits': edits}
        result.update(mutation)
        write_json(case / 'mutation.json', mutation)
        results.append(result)
        if result['issues']:
            print(f'FAIL mutation {case_number}: ' + '; '.join(result['issues']), flush=True)
        elif case_number % 32 == 31:
            print(f'Completed {case_number + 1}/256 mutations', flush=True)
    after = digest(args.compiler.read_bytes())
    counts = dict(Counter(r['status'] for r in results))
    controls_counts = dict(Counter(r['status'] for r in controls))
    passed = not counts.get('failure') and not controls_counts.get('failure') and after == actual
    summary = {'passed': passed, 'counts': counts, 'control_counts': controls_counts,
               'compiler_sha256_after': after, 'compiler_unchanged': after == actual,
               'finished_utc': datetime.now(timezone.utc).isoformat(),
               'scope': 'Bounded deterministic robustness only; not exhaustive syntax/semantic safety or sanitizer coverage.'}
    write_json(args.artifacts / 'summary.json', summary)
    write_json(args.artifacts / 'results.json', {'summary': summary, 'controls': controls,
                                              'mutations': results})
    print(json.dumps(summary, indent=2))
    print('Artifacts: ' + str(args.artifacts))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
