#!/usr/bin/env python3
"""Fail-closed generated scalar compiler/execution tests, at -o=0,1,2,3."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from generate import cases, write_corpus

ROOT = Path(__file__).resolve().parents[2]
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
ERROR = re.compile(r'^\s*(?:error(?:\[[^\]]*\])?:|\(EE\))', re.MULTILINE | re.IGNORECASE)
WARNING = re.compile(r'^\s*warning(?:\[[^\]]*\])?:', re.MULTILINE | re.IGNORECASE)
INTERNAL_FAILURE = re.compile(
    r'AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|ThreadSanitizer|'
    r'runtime error:|LLVM ERROR:|LLVM IR verification failed|Broken (?:module|function)|'
    r'internal compiler error|Assertion .* failed|Assertion failed|'
    r'Segmentation fault|Bus error|stack dump:|fatal signal|compiler crash', re.IGNORECASE)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def capture(command, cwd, prefix, timeout, env=None):
    """No shell, bounded execution, and process-tree termination on timeout."""
    start = time.monotonic()
    result = dict(command=[str(part) for part in command], cwd=str(cwd), timeout_seconds=timeout)
    stdout_path, stderr_path = Path(str(prefix) + '.stdout'), Path(str(prefix) + '.stderr')
    environment = dict(os.environ, NO_COLOR='1', TERM='dumb')
    environment.update(env or {})
    result['environment_overrides'] = dict(NO_COLOR='1', TERM='dumb', **(env or {}))
    with stdout_path.open('wb') as stdout, stderr_path.open('wb') as stderr:
        try:
            process = subprocess.Popen(result['command'], cwd=cwd, env=environment,
                                       stdout=stdout, stderr=stderr, start_new_session=(os.name == 'posix'))
            timed_out = False
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                if os.name == 'posix':
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                else:
                    subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                    process.kill()
                code = process.wait()
            category = 'timeout' if timed_out else ('exit' if code >= 0 else 'signal')
            result.update(returncode=code, category=category)
        except OSError as error:
            result.update(returncode=None, category='launch-error', error=str(error))
            stderr.write((str(error) + '\n').encode())
    result.update(elapsed_seconds=round(time.monotonic() - start, 6),
                  stdout_file=str(stdout_path), stderr_file=str(stderr_path),
                  stdout=ANSI.sub('', stdout_path.read_text(errors='replace')),
                  stderr=ANSI.sub('', stderr_path.read_text(errors='replace')))
    Path(str(prefix) + '.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def default_trap_signal():
    machine = platform.machine().lower()
    if machine in ('x86_64', 'amd64', 'i386', 'i686'):
        return 'SIGILL'
    if machine in ('aarch64', 'arm64'):
        return 'SIGTRAP'
    return None


def run_case(case, opt, args, work):
    work.mkdir(parents=True, exist_ok=True)
    output = work / ('program.exe' if os.name == 'nt' else 'program')
    # A stale expected artifact must never be executed, even if a caller reuses
    # a work directory or the compiler falsely returns success without output.
    for path in (output, Path(str(output) + '.exe')):
        if path.exists() or path.is_symlink():
            path.unlink()
    source = work / (case.name + '.sts')
    source.write_text(case.source)
    result = dict(name=case.name, optimization=opt, outcome=case.outcome, status='FAIL',
                  source=str(source), source_sha256=sha256(source), expected_stdout=case.stdout,
                  expected_reason=case.reason, observations=case.observations, problems=[])
    pre_ir, post_ir = work / 'pre.ll', work / 'post.ll'
    for path in (pre_ir, post_ir):
        if path.exists() or path.is_symlink():
            path.unlink()
    command = [str(args.compiler), 'build', str(source), '-o=' + str(opt), '-o', str(output)]
    if case.outcome == 'trap' and getattr(args, 'trap_hook_object', None):
        command += ['-l', str(args.trap_hook_object)]
    command += args.compiler_arg
    # Logger/runtime artifacts must be isolated too: parallel compiler runs
    # cannot safely share the legacy cwd-relative logs rotation directory.
    compiled = capture(command, work, work / 'compile', args.compile_timeout,
                       dict(STS_DUMP_IR_PRE=str(pre_ir), STS_DUMP_IR=str(post_ir)))
    result['compile'] = compiled
    problems = result['problems']
    diagnostic = compiled['stderr'] + '\n' + compiled['stdout']
    if INTERNAL_FAILURE.search(diagnostic):
        problems.append('compiler internal/verifier/sanitizer failure')
    if WARNING.search(diagnostic):
        problems.append('unexpected compiler warning')
    if case.outcome == 'rejection':
        if compiled['category'] != 'exit' or compiled['returncode'] != 1:
            problems.append('expected compiler diagnostic exit 1, not crash/timeout/other exit')
        if not ERROR.search(compiled['stderr']):
            problems.append('missing error diagnostic on stderr')
        if not re.search(case.diagnostic, compiled['stderr'], re.IGNORECASE | re.MULTILINE):
            problems.append('missing expected diagnostic: ' + case.diagnostic)
        if not re.search(re.escape(source.name) + r':[1-9][0-9]*(?::[1-9][0-9]*)?(?:\s|$)', compiled['stderr']):
            problems.append('missing source location on diagnostic')
        if output.exists() or Path(str(output) + '.exe').exists():
            problems.append('rejected compilation left an executable artifact')
        result['status'] = 'PASS' if not problems else 'FAIL'
        return result
    if compiled['category'] != 'exit' or compiled['returncode'] != 0:
        problems.append('compiler did not exit successfully')
    if ERROR.search(diagnostic):
        problems.append('compiler emitted error diagnostic')
    if not output.is_file() or output.is_symlink() or not os.access(output, os.X_OK):
        problems.append('fresh executable not produced')
    if problems:
        return result
    result['binary_sha256'] = sha256(output)
    result['ir_files'] = [str(path) for path in (pre_ir, post_ir) if path.is_file()]
    executed = capture([str(output)], work, work / 'execute', args.run_timeout)
    result['run'] = executed
    actual_text = executed['stdout'] + '\n' + executed['stderr']
    if INTERNAL_FAILURE.search(actual_text):
        problems.append('generated program sanitizer/internal failure')
    if case.outcome == 'success':
        if executed['category'] != 'exit' or executed['returncode'] != 0:
            problems.append('generated program did not exit 0')
        if executed['stdout'] != case.stdout:
            problems.append('stdout mismatch against arbitrary-precision reference')
        if executed['stderr']:
            problems.append('unexpected generated program stderr')
    elif case.outcome == 'trap':
        # Signal-only mode validates the trap mechanism, not its semantic reason.
        # A plain nonzero exit, SIGSEGV, SIGFPE, or timeout is NEVER a passing trap.
        expected_code = args.trap_exit_code
        if expected_code is None:
            expected_code = -getattr(signal, args.trap_signal)
        if executed['category'] not in ('exit', 'signal') or executed['returncode'] != expected_code:
            problems.append('wrong trap termination: expected ' + str(expected_code))
        if executed['stdout']:
            problems.append('trap program produced unexpected stdout')
        if args.trap_hook_pattern:
            pattern = args.trap_hook_pattern.replace('{reason}', re.escape(case.reason))
            if getattr(args, 'trap_hook_object', None):
                matched = executed['stderr'] == 'STASHA_TRAP:' + case.reason + '\n'
            else:
                matched = bool(re.search(pattern, executed['stderr'], re.MULTILINE))
            if not matched:
                problems.append('missing intended trap-hook reason')
            result['trap_reason_verified'] = not problems
        else:
            if executed['stderr']:
                problems.append('unexpected stderr in signal-only trap mode')
            result['trap_reason_verified'] = False
        if args.require_trap_reasons and not result['trap_reason_verified']:
            problems.append('P1 qualification requires a verified intended hook/reason')
    else:
        problems.append('unknown outcome ' + case.outcome)
    result['status'] = 'PASS' if not problems else 'FAIL'
    return result


def finite_positive(value):
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError('timeout must be finite and positive')
    return result


def resolve_cc(name):
    executable = shutil.which(name)
    if executable is None:
        raise ValueError('host C compiler missing or not executable: ' + name)
    return Path(executable).resolve()


def build_trap_hook(directory, cc, timeout):
    directory.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name('panic_hook.c')
    copied = directory / 'panic_hook.c'
    copied.write_bytes(source.read_bytes())
    output = directory / 'panic_hook.o'
    if output.exists() or output.is_symlink():
        output.unlink()
    result = dict(status='FAIL', source=str(copied), source_sha256=sha256(copied),
                  compiler=str(cc), compiler_sha256=sha256(cc), problems=[])
    result['version'] = capture([str(cc), '--version'], directory, directory / 'cc-version', timeout)
    if (result['version']['category'] != 'exit' or result['version']['returncode'] != 0
            or not result['version']['stdout'].strip()):
        result['problems'].append('host C compiler version/provenance query failed')
    command = [str(cc), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-lto',
               '-c', str(copied), '-o', str(output)]
    compiled = capture(command, directory, directory / 'compile-hook', timeout)
    result['compile'] = compiled
    if compiled['category'] != 'exit' or compiled['returncode'] != 0:
        result['problems'].append('host C compiler did not build panic hook')
    if compiled['stderr'] or INTERNAL_FAILURE.search(compiled['stdout']):
        result['problems'].append('unexpected host compiler diagnostic')
    if not output.is_file() or output.is_symlink() or output.stat().st_size == 0:
        result['problems'].append('fresh panic hook object missing or empty')
    if not result['problems']:
        result.update(status='PASS', object=str(output), object_sha256=sha256(output))
    write_json(directory / 'build-hook.json', result)
    return result


def self_test_trap_hook(directory, cc, timeout):
    result = build_trap_hook(directory, cc, timeout)
    if result['status'] != 'PASS':
        return result
    # This deliberately supplies a weak default in a separate C translation
    # unit, proving that direct object linkage replaces it. It does not execute
    # Stasha/LLVM-generated code or claim to qualify that compiler.
    source = directory / 'weak_panic_probe.c'
    source.write_text('#include <stdint.h>\n#include <stdlib.h>\n'
        '__attribute__((weak,noreturn)) void __stasha_panic(uint32_t reason) {\n'
        '    (void)reason; __builtin_trap();\n}\n'
        'int main(int argc, char **argv) {\n'
        '    if (argc != 2) return 2;\n'
        '    __stasha_panic((uint32_t)strtoul(argv[1], 0, 10));\n}\n')
    binary_path = directory / 'hook_probe'
    compiled = capture([str(cc), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-lto',
                        str(source), result['object'], '-o', str(binary_path)],
                       directory, directory / 'compile-probe', timeout)
    result['probe_compile'] = compiled
    if compiled['category'] != 'exit' or compiled['returncode'] != 0 or compiled['stderr'] or not binary_path.is_file():
        result['problems'].append('host C weak/strong override probe failed to build')
    else:
        result['probe_runs'] = []
        for reason, name in [(1, 'overflow'), (2, 'division-by-zero'), (3, 'division-overflow'),
                             (4, 'invalid-shift'), (5, 'fp-cast-range'), (99, 'unknown:99')]:
            executed = capture([str(binary_path), str(reason)], directory,
                               directory / ('reason-' + str(reason)), timeout)
            result['probe_runs'].append(executed)
            if (executed['category'] != 'exit' or executed['returncode'] != (87 if reason == 99 else 86)
                    or executed['stdout'] or executed['stderr'] != 'STASHA_TRAP:' + name + '\n'):
                result['problems'].append('wrong hook result for reason ' + str(reason))
    result['status'] = 'FAIL' if result['problems'] else 'PASS'
    write_json(directory / 'hook-selftest.json', result)
    return result


def write_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'bin/stasha')
    parser.add_argument('--opt', default='0,1,2,3')
    parser.add_argument('--filter', default='.', help='regex on case name')
    parser.add_argument('--artifacts', type=Path, help='parent of a new unique artifact directory')
    parser.add_argument('--compile-timeout', type=finite_positive, default=60)
    parser.add_argument('--run-timeout', type=finite_positive, default=5)
    parser.add_argument('--compiler-arg', action='append', default=[])
    parser.add_argument('--build-trap-hook', action='store_true', help='build and link the test-only strong __stasha_panic override for trap cases')
    parser.add_argument('--self-test-hook', action='store_true', help='compile/run a host C-only weak/strong panic override probe; no Stasha compiler needed')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'), help='trusted installed host C compiler executable for hook builds')
    parser.add_argument('--trap-signal', default=default_trap_signal(), choices=['SIGILL', 'SIGTRAP'])
    parser.add_argument('--trap-hook-pattern', help='stderr regex; {reason} substitutes the escaped expected reason')
    parser.add_argument('--trap-exit-code', type=int, help='exact nonzero hook exit; requires --trap-hook-pattern')
    parser.add_argument('--require-trap-reasons', action='store_true', help='fail signal-only trap tests; required for full P1 acceptance')
    parser.add_argument('--list', action='store_true', help='print inventory without compiling')
    args = parser.parse_args()
    try:
        opts = [int(item) for item in args.opt.split(',')]
        matcher = re.compile(args.filter)
    except (ValueError, re.error) as error:
        parser.error(str(error))
    if not opts or any(opt not in (0, 1, 2, 3) for opt in opts) or len(set(opts)) != len(opts):
        parser.error('--opt must contain unique values from 0,1,2,3')
    selected = [case for case in cases() if matcher.search(case.name)]
    if not selected:
        parser.error('filter selected no cases; empty suites cannot pass')
    if args.list:
        print(json.dumps([dict(name=case.name, outcome=case.outcome, reason=case.reason, tags=case.tags)
                          for case in selected], indent=2))
        return 0
    if args.build_trap_hook:
        if args.trap_exit_code is not None or args.trap_hook_pattern:
            parser.error('--build-trap-hook supplies its own exit/pattern contract')
        args.trap_exit_code = 86
        args.trap_hook_pattern = '^STASHA_TRAP:{reason}$'
    if args.self_test_hook:
        try:
            cc = resolve_cc(args.cc)
        except ValueError as error:
            parser.error(str(error))
        if args.artifacts:
            args.artifacts.mkdir(parents=True, exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix='stasha-hook-selftest-', dir=args.artifacts)).resolve()
        result = self_test_trap_hook(directory, cc, args.compile_timeout)
        print(json.dumps(dict(status=result['status'], artifacts=str(directory), problems=result['problems'])))
        return 0 if result['status'] == 'PASS' else 1
    if args.trap_exit_code is not None and (args.trap_exit_code <= 0 or not args.trap_hook_pattern):
        parser.error('--trap-exit-code must be positive and requires --trap-hook-pattern')
    if args.trap_hook_pattern:
        try:
            re.compile(args.trap_hook_pattern.replace('{reason}', 'overflow'))
        except re.error as error:
            parser.error(str(error))
        if '{reason}' not in args.trap_hook_pattern:
            parser.error('--trap-hook-pattern must contain {reason}, not just a generic trap marker')
    if any(case.outcome == 'trap' for case in selected) and not args.trap_signal and args.trap_exit_code is None:
        parser.error('unsupported host trap signal; pass --trap-signal or exact hook contract')
    args.compiler = args.compiler.resolve()
    if not args.compiler.is_file() or not os.access(args.compiler, os.X_OK):
        print('FAIL: compiler missing or not executable: ' + str(args.compiler), file=sys.stderr)
        return 1
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='stasha-scalars-', dir=args.artifacts)).resolve()
    hook_build = None
    if args.build_trap_hook and any(case.outcome == 'trap' for case in selected):
        try:
            cc = resolve_cc(args.cc)
        except ValueError as error:
            print('FAIL: ' + str(error), file=sys.stderr)
            return 1
        hook_build = build_trap_hook(directory / 'panic-hook', cc, args.compile_timeout)
        if hook_build['status'] != 'PASS':
            print(json.dumps(dict(status='FAIL', hook_build=hook_build, artifacts=str(directory))))
            return 1
        args.trap_hook_object = Path(hook_build['object'])
    write_corpus(directory / 'corpus', selected)
    summary = dict(schema=1, status='RUNNING', compiler=str(args.compiler), compiler_sha256=sha256(args.compiler),
                   platform=platform.platform(), machine=platform.machine(), python=sys.version,
                   optimizations=opts, artifacts=str(directory), selected_cases=len(selected),
                   expected_executions=len(selected) * len(opts),
                   trap_contract=dict(signal=args.trap_signal, exit_code=args.trap_exit_code,
                                      hook_pattern=args.trap_hook_pattern, require_reasons=args.require_trap_reasons),
                   full_p1_trap_reason_qualification=False, hook_build=hook_build,
                   results=[])
    summary_path = directory / 'summary.json'
    write_json(summary_path, summary)
    print('Artifacts: ' + str(directory), flush=True)
    for opt in opts:
        for case in selected:
            result = run_case(case, opt, args, directory / ('O' + str(opt)) / case.name)
            result_path = directory / ('O' + str(opt)) / case.name / 'result.json'
            write_json(result_path, result)
            # Keep the summary small; full observations/streams live with each
            # case instead of being copied and rewritten on every later result.
            summary['results'].append(dict(name=case.name, optimization=opt,
                outcome=case.outcome, status=result['status'], problems=result['problems'],
                trap_reason_verified=result.get('trap_reason_verified'), result_file=str(result_path)))
            print(f"{result['status']} O{opt} {case.name}" +
                  (': ' + '; '.join(result['problems']) if result['problems'] else ''), flush=True)
            write_json(summary_path, summary)
    failed = sum(result['status'] != 'PASS' for result in summary['results'])
    trap_results = [result for result in summary['results'] if result['outcome'] == 'trap']
    summary['full_matrix_executed'] = len(selected) == len(cases()) and set(opts) == {0, 1, 2, 3}
    summary['full_p1_trap_reason_qualification'] = bool(trap_results and not failed and
        summary['full_matrix_executed'] and args.require_trap_reasons and all(result['trap_reason_verified'] for result in trap_results))
    summary.update(status='FAIL' if failed else 'PASS', failed=failed,
                   passed=len(summary['results']) - failed)
    write_json(summary_path, summary)
    print(json.dumps(dict(status=summary['status'], passed=summary['passed'], failed=failed,
                          trap_reason_qualified=summary['full_p1_trap_reason_qualification'], summary=str(summary_path))))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
