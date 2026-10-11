#!/usr/bin/env python3
"""Fixed bounded float-precision qualification; initial review uses staged phases.

No rejection or baseline standalone format is ever linked or executed. The IR
phase is compiler-only. The native phase requires its unchanged successful
receipt; an independent reviewer grants initial execution outside this runner.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tests/harness'))
from compiler_suites import capture, category, INTERNAL_FAILURE

BASELINE_SHA256 = '782c3005a127ccdb6a0c5ac39b77562f5ebb77a242e7e5534f4db8e8b00f15bd'
OPTIMIZATIONS = (0, 2)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_identities():
    paths = set(path for path in (ROOT / 'src').rglob('*') if path.is_file())
    paths.update(path for path in HERE.rglob('*')
                 if path.is_file() and '__pycache__' not in path.parts)
    paths.update(ROOT / name for name in ('Makefile', 'tests/harness/compiler_suites.py',
                  'scripts/qualify_linux.sh', 'tests/harness/test_qualification_script.py'))
    return {str(path.relative_to(ROOT)): digest(path) for path in sorted(paths)}


def inventory():
    data = json.loads((HERE / 'float_precision_cases.json').read_text())
    # The two large exact strings are integer mathematics, not candidate/libc output.
    assert data['extrema_integer_oracle']['binary64'] == str(((1 << 53) - 1) * (1 << 971))
    assert data['extrema_integer_oracle']['binary32'] == str(((1 << 24) - 1) * (1 << 104))
    assert data['max_runtime_precision'] == 20 and data['max_runtime_width'] == 40
    return data


def executable(value):
    found = shutil.which(str(value))
    if not found or not Path(found).is_file():
        raise ValueError('missing executable prerequisite: ' + str(value))
    return str(Path(found).resolve())


def raw_stream(process, name):
    path = process.get(name + '_file')
    return Path(path).read_bytes() if path else process.get(name, '').encode()


def process_issues(process, exit_code=0, expected_stdout=None, empty_stderr=False):
    issues = []
    if process.get('returncode') != exit_code or process.get('category') != category(exit_code):
        issues.append(f"exit={process.get('returncode')} category={process.get('category')} expected={exit_code}")
    if INTERNAL_FAILURE.search(process.get('stdout', '') + '\n' + process.get('stderr', '')):
        issues.append('internal compiler, sanitizer, or runtime failure')
    if expected_stdout is not None and raw_stream(process, 'stdout') != expected_stdout:
        issues.append('stdout differs from fixed exact bytes')
    if empty_stderr and raw_stream(process, 'stderr'):
        issues.append('unexpected stderr')
    return issues


def invoke(runner, command, cwd, evidence, name, timeout, extra_env=None):
    return runner(list(map(str, command)), cwd, evidence / name, timeout,
                  extra_env=extra_env)


def helper_phase(cc, work, timeout, runner):
    results = []
    for name, flags in [('plain', []), ('ubsan', ['-fsanitize=undefined']),
                        ('asan-ubsan', ['-fsanitize=address,undefined'])]:
        case = work / ('helper-' + name); case.mkdir()
        evidence = work / ('helper-' + name + '-evidence'); evidence.mkdir()
        output = case / 'format-spec-tests'
        command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fno-omit-frame-pointer', *flags, HERE / 'format_spec_tests.c', '-o', output]
        compiled = invoke(runner, command, case, evidence, 'compile', timeout)
        record = dict(name='helper-' + name, compile=compiled,
                      issues=process_issues(compiled))
        if not output.is_file() or output.stat().st_size == 0:
            record['issues'].append('fresh helper executable missing or empty')
        if not record['issues']:
            record['executable_sha256'] = digest(output)
            env = dict(ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            run = invoke(runner, [output], case, evidence, 'run', timeout, env)
            record['runtime'] = run
            record['issues'] += process_issues(run, empty_stderr=True)
            marker = re.fullmatch(rb'PASS: ([1-9][0-9]*) fixed format helper checks\n',
                                  raw_stream(run, 'stdout'))
            if not marker:
                record['issues'].append('missing exact helper success marker')
            else:
                record['check_count'] = int(marker.group(1))
        results.append(record)
    return results


def ir_checks(text, name, opt):
    issues = []
    if name != 'float_precision_native' or opt != 0:
        return issues
    for function, expected_extensions, capacity in [('promote_once', 1, 329), ('direct_f64', 0, 328)]:
        match = re.search(r'^define[^\n]*@[^\n(]*' + function + r'\([^\n]*\).*?^}',
                          text, re.M | re.S)
        if not match:
            issues.append('missing review function: ' + function)
            continue
        body = match.group(0)
        if len(re.findall(r'\bfpext float\b', body)) != expected_extensions:
            issues.append(function + ': wrong f32 extension count')
        if len(re.findall(r'\bcall\b[^\n]*@snprintf\(', body)) != 1:
            issues.append(function + ': expected one snprintf call')
        if f'alloca [{capacity} x i8]' not in body or f'i64 {capacity},' not in body:
            issues.append(function + ': fixed array/call bound differs from independent arithmetic')
    return issues


def ir_phase(compiler, baseline, work, timeout, runner):
    results = []
    for spec in inventory()['runtime']:
        for opt in OPTIMIZATIONS:
            name = spec['name'] + '-O' + str(opt)
            case = work / ('ir-' + name); case.mkdir()
            evidence = work / ('ir-' + name + '-evidence'); evidence.mkdir()
            source = case / Path(spec['source']).name
            shutil.copyfile(HERE / spec['source'], source)
            output = case / 'candidate.ll'
            process = invoke(runner, [compiler, 'ir', source.name, f'-o={opt}', '-o', output],
                             case, evidence, 'candidate', timeout)
            record = dict(name='ir-' + name, compile=process, issues=process_issues(process),
                          source=spec['source'], optimization=opt)
            if not output.is_file() or output.stat().st_size == 0:
                record['issues'].append('fresh IR artifact missing or empty')
            elif not record['issues']:
                record.update(ir_path=str(output), ir_sha256=digest(output))
                record['issues'] += ir_checks(output.read_text(), spec['name'], opt)
            # A's only accepted use is the ordinary INLINE control, never standalone.
            if baseline and spec['name'] == 'float_precision_inline' and not record['issues']:
                old = case / 'baseline.ll'
                prior = invoke(runner, [baseline, 'ir', source.name, f'-o={opt}', '-o', old],
                               case, evidence, 'baseline', timeout)
                record['baseline_compile'] = prior
                record['issues'] += process_issues(prior)
                if not old.is_file() or old.stat().st_size == 0:
                    record['issues'].append('fresh baseline inline IR missing or empty')
                else:
                    record['baseline_ir_sha256'] = digest(old)
                    record['baseline_ir_path'] = str(old)
                    if old.read_bytes() != output.read_bytes():
                        record['issues'].append('ordinary inline IR differs byte-for-byte from pinned A')
            results.append(record)
    for spec in inventory()['rejections']:
        case = work / ('reject-' + spec['name']); case.mkdir()
        evidence = work / ('reject-' + spec['name'] + '-evidence'); evidence.mkdir()
        source = case / Path(spec['source']).name
        shutil.copyfile(HERE / spec['source'], source)
        initial_source = digest(source)
        output = case / 'must-not-exist.ll'
        process = invoke(runner, [compiler, 'ir', source.name, '-o=0', '-o', output],
                         case, evidence, 'compile-only', timeout)
        issues = process_issues(process, 1)
        text = process.get('stdout', '') + '\n' + process.get('stderr', '')
        if len(re.findall(r'^error(?:\[[^\]]+\])?: ' + re.escape(spec['message']) + r'$', text, re.M)) != 1:
            issues.append('expected exactly one frozen primary diagnostic')
        anchor = f"{source.name}:{spec['line']}"
        if spec['column']:
            anchor += ':' + str(spec['column'])
        # A column-zero AST location is honestly line-only. Reject invented
        # columns, other source basenames, and longer line-number prefixes.
        anchor_pattern = r'^[ \t]*-->[ \t]+(?:[^\n]*[/\\])?' + re.escape(anchor) + r'[ \t]*$'
        if len(re.findall(anchor_pattern, text, re.M)) != 1:
            issues.append('missing exact real outer-expression source anchor: ' + anchor)
        other_errors = re.findall(r'^error(?:\[[^\]]+\])?: (.+)$', text, re.M)
        if any(message != spec['message'] and not message.startswith('aborting due to ')
               for message in other_errors):
            issues.append('unexpected additional primary diagnostic')
        # Same bounded logging policy as tests/driver/project_cli.py, with
        # directory/type checks too. Logging never licenses candidate outputs.
        allowed_logs = {'logs', 'logs/.rotation-lock', 'logs/log0-last.log'}
        generated = []
        driver_logs = []
        for path in sorted(case.rglob('*')):
            if path == source:
                continue
            relative = str(path.relative_to(case))
            regular_log = (path.is_dir() if relative == 'logs' else path.is_file())
            if relative in allowed_logs and regular_log and not path.is_symlink():
                driver_logs.append(relative)
            else:
                generated.append(relative)
        if generated:
            issues.append('rejection generated artifacts: ' + ', '.join(generated))
        if source.is_symlink() or not source.is_file() or digest(source) != initial_source:
            issues.append('rejection source changed or disappeared')
        results.append(dict(name='reject-' + spec['name'], compile=process, issues=issues,
                            source=spec['source'], expected=spec, generated=generated,
                            driver_logs=driver_logs, runtime_executed=False))
    return results


def validate_ir_receipt(path, compiler, baseline, identities):
    report = json.loads(Path(path).read_text())
    if report.get('phase') != 'ir' or report.get('failed') != 0 or report.get('passed') != 8:
        raise ValueError('native requires a complete successful staged IR receipt')
    if report.get('source_identities') != identities:
        raise ValueError('source or oracle identities changed after IR qualification')
    if report.get('compiler_sha256') != digest(compiler):
        raise ValueError('compiler changed after IR qualification')
    expected_baseline = digest(baseline) if baseline else None
    if report.get('baseline_sha256') != expected_baseline:
        raise ValueError('baseline selection changed after IR qualification')
    expected_names = {f"ir-{item['name']}-O{opt}" for item in inventory()['runtime']
                      for opt in OPTIMIZATIONS}
    expected_names.update('reject-' + item['name'] for item in inventory()['rejections'])
    names = [item['name'] for item in report['results']]
    if len(names) != len(expected_names) or set(names) != expected_names:
        raise ValueError('IR receipt has incomplete or duplicate case inventory')
    paths = []
    for item in report['results']:
        if item.get('issues'):
            raise ValueError('IR receipt contains a failed case')
        if item['name'].startswith('ir-'):
            ir = Path(item['ir_path'])
            if digest(ir) != item['ir_sha256']:
                raise ValueError('reviewed IR changed: ' + str(ir))
            paths.append(str(ir))
            if 'baseline_ir_path' in item and digest(item['baseline_ir_path']) != item['baseline_ir_sha256']:
                raise ValueError('baseline inline IR changed after comparison')
    if len(paths) != 4:
        raise ValueError('IR receipt has incomplete candidate artifact inventory')
    return dict(path=str(Path(path).resolve()), sha256=digest(path), candidate_ir=paths)


def native_phase(compiler, baseline, cc, work, timeout, runner):
    results = []
    support = work / 'support'; support.mkdir()
    evidence = work / 'support-evidence'; evidence.mkdir()
    obj = support / 'float_precision_values.o'
    compiled = invoke(runner, [cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                              '-fno-fast-math', '-c', HERE / 'float_precision_values.c', '-o', obj],
                      support, evidence, 'compile', timeout)
    issues = process_issues(compiled)
    if not obj.is_file() or obj.stat().st_size == 0:
        issues.append('fresh support object missing or empty')
    results.append(dict(name='native-support', compile=compiled, issues=issues,
                        object_sha256=digest(obj) if obj.is_file() else None))
    if issues:
        return results
    for spec in inventory()['runtime']:
        compilers = [('candidate', compiler)]
        if baseline and spec['name'] == 'float_precision_inline':
            compilers.append(('baseline-inline', baseline))
        for label, selected in compilers:
            for opt in OPTIMIZATIONS:
                name = label + '-' + spec['name'] + '-O' + str(opt)
                case = work / name; case.mkdir()
                evidence = work / (name + '-evidence'); evidence.mkdir()
                source = case / Path(spec['source']).name
                shutil.copyfile(HERE / spec['source'], source)
                output = case / 'program'
                command = [selected, 'build', source.name, f'-o={opt}', '-o', output]
                if spec['shim']:
                    command += ['-l', obj, '-l', '-lm']
                compiled = invoke(runner, command, case, evidence, 'compile', timeout)
                record = dict(name=name, compile=compiled, issues=process_issues(compiled),
                              source=spec['source'], optimization=opt)
                if not output.is_file() or output.stat().st_size == 0:
                    record['issues'].append('fresh native executable missing or empty')
                if not record['issues']:
                    record['executable_sha256'] = digest(output)
                    run = invoke(runner, [output], case, evidence, 'run', timeout)
                    record['runtime'] = run
                    expected = spec['stdout'].encode()
                    record['expected_stdout_sha256'] = hashlib.sha256(expected).hexdigest()
                    record['issues'] += process_issues(run, expected_stdout=expected, empty_stderr=True)
                results.append(record)
    return results


def run_suite(phase, compiler=None, cc='clang', baseline=None, artifacts=None,
              ir_receipt=None, timeout=30, runner=capture, helper_cc=None):
    if artifacts:
        artifacts = Path(artifacts); artifacts.mkdir(parents=True, exist_ok=True)
    # Fresh directory means old successful outputs are never accepted on a rerun.
    work = Path(tempfile.mkdtemp(prefix='stasha-format-' + phase + '-', dir=artifacts))
    identities = source_identities()
    report = dict(suite='bounded-float-precision', phase=phase,
                  time_utc=datetime.now(timezone.utc).isoformat(),
                  source_identities=identities, compiler_sha256=digest(compiler) if compiler else None,
                  baseline_sha256=digest(baseline) if baseline else None,
                  cc_sha256=digest(cc) if cc else None, compiler=compiler, cc=cc,
                  helper_cc=helper_cc, helper_cc_sha256=digest(helper_cc) if helper_cc else None,
                  host=dict(system=platform.system(), machine=platform.machine(), libc=platform.libc_ver()),
                  environment_assumption=inventory()['qualification'], results=[])
    if baseline and report['baseline_sha256'] != BASELINE_SHA256:
        raise ValueError('baseline is not the frozen A compiler')
    if phase == 'helper':
        if not helper_cc:
            raise ValueError('pure helper phase requires an explicit helper compiler')
        report['results'] = helper_phase(helper_cc, work, timeout, runner)
    elif phase == 'ir':
        report['results'] = ir_phase(compiler, baseline, work, timeout, runner)
    elif phase == 'native':
        report['ir_gate'] = validate_ir_receipt(ir_receipt, compiler, baseline, identities)
        report['results'] = native_phase(compiler, baseline, cc, work, timeout, runner)
    else:
        raise ValueError('invalid individual phase: ' + phase)
    if source_identities() != identities:
        report['results'].append(dict(name='source-identity', issues=['sources changed during phase']))
    for name, value in [('compiler', compiler), ('baseline', baseline), ('cc', cc),
                        ('helper_cc', helper_cc)]:
        if value and digest(value) != report[name + '_sha256']:
            report['results'].append(dict(name=name + '-identity', issues=[name + ' changed during phase']))
    report['failed'] = sum(bool(item['issues']) for item in report['results'])
    report['passed'] = len(report['results']) - report['failed']
    (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report, work


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=('helper', 'ir', 'native', 'all'), default='all')
    parser.add_argument('--compiler', default=str(ROOT / 'bin/stasha'))
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'),
                        help='native C shim compiler (pinned Clang in qualification)')
    parser.add_argument('--helper-cc', default=os.environ.get('HOST_CC', 'gcc'),
                        help='pure C helper/sanitizer compiler; defaults to HOST_CC or gcc')
    parser.add_argument('--baseline', help='optional frozen A compiler; only ordinary INLINE is compared')
    parser.add_argument('--ir-receipt', type=Path, help='successful --phase ir results.json required for native')
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--timeout', type=float, default=30)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 60:
        parser.error('--timeout must be positive, finite, and at most 60 seconds')
    if args.phase == 'native' and not args.ir_receipt:
        parser.error('--phase native requires --ir-receipt')
    if platform.system() != 'Linux' or platform.machine() != 'x86_64' or platform.libc_ver()[0] != 'glibc':
        parser.error('requires qualified Linux x86-64 glibc hosted environment')
    try:
        compiler = executable(args.compiler) if args.phase != 'helper' else None
        cc = executable(args.cc) if args.phase in ('native', 'all') else None
        helper_cc = executable(args.helper_cc) if args.phase in ('helper', 'all') else None
        baseline = executable(args.baseline) if args.baseline else None
        phases = ('helper', 'ir', 'native') if args.phase == 'all' else (args.phase,)
        receipt = args.ir_receipt
        for phase in phases:
            report, work = run_suite(phase, compiler, cc, baseline, args.artifacts, receipt, args.timeout,
                                     helper_cc=helper_cc)
            for item in report['results']:
                print(('FAIL ' if item['issues'] else 'PASS ') + item['name'])
                for issue in item['issues']:
                    print('  ' + issue)
            print(f"{report['passed']} passed, {report['failed']} failed; artifacts: {work}")
            if report['failed']:
                return 1
            if phase == 'ir':
                receipt = work / 'results.json'
        return 0
    except (OSError, ValueError, KeyError, AssertionError) as error:
        print('FAIL float precision runner: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
