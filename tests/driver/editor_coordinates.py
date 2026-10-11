#!/usr/bin/env python3
"""Bounded native editor-coordinate CLI regressions; no generated-program runs.

The compiler must run on the Python host, so ctypes.c_ulong describes its
usize_t range. Fixtures contain only ordinary functions and editor queries.
"""
import argparse
import ctypes
import hashlib
import json
import math
from pathlib import Path
import stat
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/harness'))
from compiler_suites import capture, category, INTERNAL_FAILURE

MAXIMUM = (1 << (ctypes.sizeof(ctypes.c_ulong) * 8)) - 2
SOURCE_PATH = 'coordinates.sts'
SOURCE = ('mod coordinates;\n'
          'fn target(void): i32 { ret 7; }\n'
          'fn caller(void): i32 {\n'
          '    ret target();\n'
          '}\n')
DEFINITION = dict(definition=dict(path=SOURCE_PATH, line=1, col=3,
                                  name='target', kind='function'))
COMPLETIONS = dict(completions=[dict(label=name, kind='function', detail='')
                               for name in ('target', 'caller')])
REFS = dict(refs=[dict(file=SOURCE_PATH, line=1, col=3, len=6),
                  dict(file=SOURCE_PATH, line=3, col=8, len=6)])


def diagnostic(flag, missing=False):
    message = (f'missing operand for {flag} (expected a zero-based decimal coordinate)'
               if missing else
               f'invalid operand for {flag} (expected decimal digits in range 0..{MAXIMUM})')
    return dict(diagnostics=[dict(severity='error', message=message,
                                 file='<unknown>', line=0, col=0, len=1,
                                 labels=[], notes=[])])


def cases():
    positive = [
        ('named-definition', ['--line', '3', '--col', '8'], DEFINITION),
        ('declaration', ['--line', '1', '--col', '3'], DEFINITION),
        ('zero', ['--line', '0', '--col', '0'], dict(definition=None)),
        ('leading-zeros', ['--line', '0003', '--col', '0008'], DEFINITION),
        ('repeated', ['--line', '99', '--col', '99', '--line', '3', '--col', '8'], DEFINITION),
        ('out-of-file', ['--line', '99', '--col', '99'], dict(definition=None)),
        ('maximum-line', ['--line', str(MAXIMUM), '--col', '8'], dict(definition=None)),
        ('maximum-col', ['--line', '3', '--col', str(MAXIMUM)], dict(definition=None)),
    ]
    for name, flags, expected in positive:
        yield dict(name=name, args=['definition', SOURCE_PATH, *flags], expected=expected)
    yield dict(name='flags-before-file', args=['definition', '--line', '3', '--col', '8', SOURCE_PATH],
               expected=DEFINITION)
    for command, expected, default in [('complete', COMPLETIONS, COMPLETIONS),
                                        ('refs', REFS, dict(refs=[]))]:
        yield dict(name=command + '-valid', args=[command, SOURCE_PATH, '--line', '3', '--col', '8'],
                   expected=expected)
        yield dict(name=command + '-default', args=[command, SOURCE_PATH], expected=default)
        yield dict(name=command + '-invalid', args=[command, SOURCE_PATH, '--line', 'bad'],
                   expected=diagnostic('--line'), rejection=True)
    invalid = [('empty', ''), ('alphabetic', 'nonsense'), ('suffix', '3junk'),
               ('negative', '-1'), ('positive-sign', '+3'), ('leading-space', ' 3'),
               ('trailing-space', '3 '), ('fraction', '3.0'), ('hex', '0x3'),
               ('non-ascii', '٣'), ('newline', '3\n'),
               ('conversion-overflow', str(MAXIMUM + 1)),
               ('representation-overflow', str(MAXIMUM + 2)),
               ('long-overflow', '9' * 80)]
    for flag, other, other_value in [('--line', '--col', '8'), ('--col', '--line', '3')]:
        prefix = flag[2:]
        for name, value in invalid:
            yield dict(name=prefix + '-' + name,
                       args=['definition', SOURCE_PATH, other, other_value, flag, value],
                       expected=diagnostic(flag), rejection=True)
        yield dict(name=prefix + '-missing',
                   args=['definition', SOURCE_PATH, other, other_value, flag],
                   expected=diagnostic(flag, missing=True), rejection=True)
        yield dict(name=prefix + '-next-option', args=['definition', SOURCE_PATH, flag, other, other_value],
                   expected=diagnostic(flag), rejection=True)
        yield dict(name=prefix + '-missing-source',
                   args=['definition', 'absent.sts', other, other_value, flag, 'bad'],
                   expected=diagnostic(flag), rejection=True)
        yield dict(name=prefix + '-no-source', args=['definition', flag, 'bad'],
                   expected=diagnostic(flag), rejection=True)
        yield dict(name=prefix + '-invalid-before-repeat',
                   args=['definition', SOURCE_PATH, flag, 'bad', flag, '3', other, other_value],
                   expected=diagnostic(flag), rejection=True)
        yield dict(name=prefix + '-invalid-after-repeat',
                   args=['definition', SOURCE_PATH, flag, '3', flag, 'bad', other, other_value],
                   expected=diagnostic(flag), rejection=True)


def prepare(case):
    case.mkdir()
    (case / SOURCE_PATH).write_text(SOURCE)
    (case / 'untouched.txt').write_text('editor queries must leave inputs unchanged\n')


def snapshot(case):
    result = {}
    for path in case.rglob('*'):
        mode = path.lstat().st_mode
        item = dict(mode=mode)
        if stat.S_ISLNK(mode):
            item['target'] = str(path.readlink())
        elif stat.S_ISREG(mode):
            item['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        result[str(path.relative_to(case))] = item
    return result


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError('non-JSON constant: ' + value)


def run_case(compiler, work, spec, timeout, runner):
    case = work / spec['name']
    evidence = work / (spec['name'] + '-evidence')
    evidence.mkdir()
    prepare(case)
    before = snapshot(case)
    record = dict(name=spec['name'], issues=[], inputs=before)
    issues = record['issues']
    process = runner([compiler, *spec['args']], case, evidence / 'query', timeout)
    record['process'] = process
    expected_exit = 1 if spec.get('rejection') else 0
    if process['returncode'] != expected_exit or process['category'] != category(expected_exit):
        issues.append(f"exit={process['returncode']} category={process['category']} expected={expected_exit}")
    if process['stderr']:
        issues.append('editor wrote unexpected stderr')
    if INTERNAL_FAILURE.search(process['stdout'] + '\n' + process['stderr']):
        issues.append('internal compiler, sanitizer, or runtime failure')
    try:
        actual = json.loads(process['stdout'], object_pairs_hook=unique_object,
                            parse_constant=reject_constant)
        # Canonical encoding also distinguishes booleans/floats from integer coordinates.
        if json.dumps(actual, sort_keys=True) != json.dumps(spec['expected'], sort_keys=True):
            issues.append('editor JSON differs from exact oracle')
    except ValueError as error:
        issues.append('expected exactly one valid JSON response: ' + str(error))
    # capture keeps raw streams as well; reject ANSI/control contamination it may strip.
    for stream in ('stdout', 'stderr'):
        raw_path = process.get(stream + '_file')
        if raw_path and Path(raw_path).read_bytes() != process[stream].encode():
            issues.append('editor raw ' + stream + ' differs from captured text')
    after = snapshot(case)
    for name in sorted(set(after) - set(before)):
        issues.append('unexpected generated entry: ' + name)
    for name, original in before.items():
        if after.get(name) != original:
            issues.append('input changed or disappeared: ' + name)
    return record


def run_suite(compiler, artifacts=None, timeout=15, runner=None):
    compiler = str(Path(compiler).resolve())
    if artifacts:
        artifacts = Path(artifacts)
        artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='stasha-editor-coordinates-', dir=artifacts))
    before = hashlib.sha256(Path(compiler).read_bytes()).hexdigest()
    results = [run_case(compiler, work, spec, timeout, runner or capture) for spec in cases()]
    after = hashlib.sha256(Path(compiler).read_bytes()).hexdigest()
    if before != after:
        results.append(dict(name='compiler-identity', issues=['compiler changed during qualification']))
    failed = sum(bool(result['issues']) for result in results)
    report = dict(suite='ordinary-editor-coordinates', compiler=compiler,
                  compiler_sha256_before=before, compiler_sha256_after=after,
                  coordinate_maximum=MAXIMUM,
                  passed=len(results) - failed, failed=failed, results=results)
    (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report, work


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(ROOT / 'bin/stasha'))
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--timeout', type=float, default=15)
    args = parser.parse_args(argv)
    if args.timeout <= 0 or not math.isfinite(args.timeout):
        parser.error('--timeout must be positive and finite')
    try:
        report, work = run_suite(args.compiler, args.artifacts, args.timeout)
    except (OSError, ValueError) as error:
        print('FAIL editor coordinate runner: ' + str(error), file=sys.stderr)
        return 1
    for result in report['results']:
        print(('FAIL' if result['issues'] else 'PASS') + ' ' + result['name'])
        for issue in result['issues']:
            print('  ' + issue)
    print(f"{report['passed']} passed, {report['failed']} failed; artifacts: {work}")
    return int(bool(report['failed']))


if __name__ == '__main__':
    sys.exit(main())
