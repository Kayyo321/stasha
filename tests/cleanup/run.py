#!/usr/bin/env python3
"""Fail-closed hosted cleanup traces, each compile/run in a fresh directory."""
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

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
ERROR = re.compile(r'^\s*(?:error(?:\[[^\]]*\])?:|\(EE\))', re.MULTILINE | re.IGNORECASE)
WARNING = re.compile(r'^\s*warning(?:\[[^\]]*\])?:', re.MULTILINE | re.IGNORECASE)
INTERNAL = re.compile(r'LLVM IR verification failed|LLVM ERROR:|Broken (?:function|module) found|'
                      r'internal compiler error|Assertion .* failed|Assertion failed|'
                      r'AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:|'
                      r'Segmentation fault|Bus error|stack dump:|fatal signal|compiler crash', re.IGNORECASE)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def capture(command, cwd, prefix, timeout, env=None):
    """No shell; timeout terminates the entire subprocess group on POSIX."""
    command = [str(x) for x in command]
    result = dict(command=command, cwd=str(cwd), timeout_seconds=timeout)
    start = time.monotonic()
    environment = dict(os.environ, NO_COLOR='1', TERM='dumb')
    environment.update(env or {})
    result['environment_overrides'] = dict(NO_COLOR='1', TERM='dumb', **(env or {}))
    out, err = Path(str(prefix) + '.stdout'), Path(str(prefix) + '.stderr')
    with out.open('wb') as stdout, err.open('wb') as stderr:
        try:
            process = subprocess.Popen(command, cwd=cwd, env=environment, stdout=stdout,
                                       stderr=stderr, start_new_session=(os.name == 'posix'))
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
            result.update(returncode=code, category='timeout' if timed_out else ('signal' if code < 0 else 'exit'))
        except OSError as error:
            result.update(returncode=None, category='launch-error', error=str(error))
            stderr.write((str(error) + '\n').encode())
    result.update(elapsed_seconds=round(time.monotonic() - start, 6), stdout_file=str(out),
                  stderr_file=str(err), stdout=ANSI.sub('', out.read_text(errors='replace')),
                  stderr=ANSI.sub('', err.read_text(errors='replace')))
    Path(str(prefix) + '.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def load_cases(manifest=HERE / 'cases.json'):
    cases = json.loads(Path(manifest).read_text())
    if isinstance(cases, dict):
        cases = cases['cases']
    names = set()
    for case in cases:
        name = case['name']
        if name in names or not re.fullmatch(r'[A-Za-z0-9_-]+', name):
            raise ValueError('invalid or duplicate case name: ' + name)
        names.add(name)
        source = (HERE / case['source']).resolve()
        if not source.is_relative_to(HERE) or not source.is_file() or source.suffix != '.sts':
            raise ValueError('invalid source: ' + case['source'])
        if case['outcome'] not in ('success', 'rejection'):
            raise ValueError('invalid outcome: ' + name)
        if case['outcome'] == 'success' and not isinstance(case.get('stdout'), str):
            raise ValueError('missing exact stdout: ' + name)
        if case['outcome'] == 'rejection':
            re.compile(case['diagnostic'])
    return cases


def run_case(case, opt, args, work):
    work.mkdir(parents=True, exist_ok=True)
    source = work / Path(case['source']).name
    shutil.copyfile(HERE / case['source'], source)
    output = work / ('program.exe' if os.name == 'nt' else 'program')
    # Defence in depth if this helper is called with a reused directory.
    for path in (output, Path(str(output) + '.exe')):
        if path.exists() or path.is_symlink():
            path.unlink()
    pre_ir, post_ir = work / 'pre.ll', work / 'post.ll'
    for path in (pre_ir, post_ir):
        if path.exists() or path.is_symlink():
            path.unlink()
    result = dict(name=case['name'], optimization=opt, outcome=case['outcome'], status='FAIL',
                  source=str(source), source_sha256=sha256(source), problems=[])
    problems = result['problems']
    command = [args.compiler, 'build', source, '-o=' + str(opt), '-o', output] + args.compiler_arg
    if case.get('support') == 'zone_probe':
        command += ['-l', args.zone_probe_object]
    compiled = capture(command, work, work / 'compile', args.compile_timeout,
                       dict(STS_DUMP_IR_PRE=str(pre_ir), STS_DUMP_IR=str(post_ir)))
    result['compile'] = compiled
    diagnostic = compiled['stderr'] + '\n' + compiled['stdout']
    if INTERNAL.search(diagnostic):
        problems.append('compiler internal/verifier/sanitizer failure')
    if WARNING.search(diagnostic):
        problems.append('unexpected compiler warning')
    if case['outcome'] == 'rejection':
        if compiled['category'] != 'exit' or compiled['returncode'] != 1:
            problems.append('expected compiler diagnostic exit 1, not crash/timeout/other exit')
        if not ERROR.search(compiled['stderr']):
            problems.append('missing error diagnostic on stderr')
        if not re.search(case['diagnostic'], compiled['stderr'], re.IGNORECASE | re.MULTILINE):
            problems.append('missing expected diagnostic')
        if not re.search(re.escape(source.name) + r':[1-9][0-9]*(?::[1-9][0-9]*)?(?:\s|$)', compiled['stderr']):
            problems.append('missing source location on diagnostic')
        if output.exists() or Path(str(output) + '.exe').exists():
            problems.append('rejected compilation left an executable artifact')
    else:
        if compiled['category'] != 'exit' or compiled['returncode'] != 0:
            problems.append('compiler did not exit successfully')
        if ERROR.search(diagnostic):
            problems.append('compiler emitted error diagnostic')
        if not output.is_file() or output.is_symlink() or not os.access(output, os.X_OK):
            problems.append('fresh executable not produced')
        if not problems:
            result['binary_sha256'] = sha256(output)
            executed = capture([output], work, work / 'execute', args.run_timeout)
            result['run'] = executed
            if executed['category'] != 'exit' or executed['returncode'] != 0:
                problems.append('generated program did not exit 0')
            if Path(executed['stdout_file']).read_bytes() != case['stdout'].encode():
                problems.append('exact cleanup trace mismatch')
            if Path(executed['stderr_file']).read_bytes():
                problems.append('unexpected generated program stderr')
            if INTERNAL.search(executed['stderr'] + '\n' + executed['stdout']):
                problems.append('generated program sanitizer/internal failure')
    result['ir_files'] = [str(p) for p in (pre_ir, post_ir) if p.is_file()]
    result['status'] = 'PASS' if not problems else 'FAIL'
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def positive_timeout(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError('timeout must be positive and finite')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'bin/stasha')
    parser.add_argument('--opt', default='0,1,2,3')
    parser.add_argument('--zone-probe', action='store_true',
                        help='also trace and verify lexical zone release calls using the actual C runtime')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'), help='host C compiler for --zone-probe')
    parser.add_argument('--case', action='append', default=[])
    parser.add_argument('--compiler-arg', action='append', default=[])
    parser.add_argument('--compile-timeout', type=positive_timeout, default=60)
    parser.add_argument('--run-timeout', type=positive_timeout, default=10)
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args()
    try:
        cases = load_cases()
        opts = list(dict.fromkeys(int(x) for x in args.opt.split(',')))
        if not opts or any(x not in range(4) for x in opts):
            raise ValueError('--opt must contain optimization levels 0,1,2,3')
        unknown = set(args.case) - {c['name'] for c in cases}
        if unknown:
            raise ValueError('unknown --case: ' + ', '.join(sorted(unknown)))
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))
    if args.case:
        cases = [case for case in cases if case['name'] in args.case]
    if args.zone_probe:
        base = next((case for case in cases if case['name'] == 'lexical_zones'), None)
        if base is None:
            parser.error('--zone-probe requires lexical_zones in the selected cases')
        instrumented = dict(base, name='lexical_zones_instrumented', support='zone_probe')
        instrumented['stdout'] = (HERE / 'lexical_zones_instrumented.stdout').read_text()
        cases.append(instrumented)
    if args.list:
        print(json.dumps(cases, indent=2))
        return 0
    args.compiler = args.compiler.resolve()
    if not args.compiler.is_file() or not os.access(args.compiler, os.X_OK):
        parser.error('compiler missing or not executable: ' + str(args.compiler))
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix='cleanup-', dir=args.artifacts)).resolve()
    report = dict(suite='cleanup', profile='hosted', qualification='actual compiler execution',
                  compiler=str(args.compiler), compiler_sha256=sha256(args.compiler),
                  host=platform.platform(), optimizations=opts, artifacts=str(artifacts), results=[])
    if args.zone_probe:
        cc = shutil.which(args.cc)
        if not cc:
            parser.error('host C compiler unavailable: ' + args.cc)
        args.zone_probe_object = artifacts / 'zone_probe.o'
        support = capture([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-c',
                           HERE / 'zone_probe.c', '-o', args.zone_probe_object],
                          artifacts, artifacts / 'zone-probe-build', args.compile_timeout)
        report['zone_probe_build'] = support
        if support['category'] != 'exit' or support['returncode'] != 0 or not args.zone_probe_object.is_file():
            (artifacts / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            print('FAIL zone probe support build; artifacts: ' + str(artifacts), file=sys.stderr)
            return 1
    for opt in opts:
        for case in cases:
            result = run_case(case, opt, args, artifacts / ('O' + str(opt)) / case['name'])
            report['results'].append(result)
            print(f"{result['status']} O{opt} {case['name']}", flush=True)
    report['passed'] = sum(r['status'] == 'PASS' for r in report['results'])
    report['failed'] = len(report['results']) - report['passed']
    (artifacts / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['passed']} passed, {report['failed']} failed; artifacts: {artifacts}")
    return 1 if report['failed'] else 0


if __name__ == '__main__':
    sys.exit(main())
