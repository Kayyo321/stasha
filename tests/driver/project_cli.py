#!/usr/bin/env python3
"""Bounded ordinary executable/project CLI regressions, with strict oracles.

Every command gets a fresh working directory. This runner does not exercise or
qualify project grammar, flag precedence, libraries, or archive behavior.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/harness'))
from compiler_suites import capture, category, INTERNAL_FAILURE

PROJECT_OUTPUT = 'bin/project-app'
PROJECT_STDOUT = 'project-cli-root=73\n'
FILE_STDOUT = 'project-cli-file=91\n'
PROJECT_EXIT = 37
FILE_EXIT = 29
PROJECT_SOURCE = "mod main; fn main(void): i32 { print.('project-cli-root=73\\n'); ret 37; }\n"
FILE_SOURCE = "mod explicit; fn main(void): i32 { print.('project-cli-file=91\\n'); ret 29; }\n"
TIMESTAMP = re.compile(r'(?m)^((?:\(EE\) )?)\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] ')


def cases():
    for name, args in [('bare', []), ('build', ['build'])]:
        yield dict(name='project-' + name, args=args, project=True,
                   output=PROJECT_OUTPUT, stdout=PROJECT_STDOUT, exit=PROJECT_EXIT)
        yield dict(name='no-project-' + name, args=args, project=False,
                   diagnostic='expected <file.sts> (or run from a project directory with sts.sproj)')
        yield dict(name='missing-source-' + name, args=args, project=True,
                   missing=True, diagnostic="could not open file 'src/main.sts'")
    for project in (False, True):
        suffix = '-with-project' if project else ''
        yield dict(name='explicit-file' + suffix, args=['src/explicit.sts'],
                   project=project, explicit=True, output='a.out',
                   stdout=FILE_STDOUT, exit=FILE_EXIT)
        yield dict(name='explicit-build' + suffix,
                   args=['build', 'src/explicit.sts', '-o', 'bin/file-app'],
                   project=project, explicit=True, output='bin/file-app',
                   stdout=FILE_STDOUT, exit=FILE_EXIT)
    for name, flag in [('help', '--help'), ('short-help', '-h'), ('version', '--version')]:
        yield dict(name=name + '-with-project', args=[flag], project=True,
                   early=name)


def prepare(case, spec):
    case.mkdir()
    (case / 'src').mkdir()
    (case / 'bin').mkdir()
    if spec['project']:
        (case / 'sts.sproj').write_text('main = "src/main.sts"\nbinary = "bin/project-app"\n')
        if not spec.get('missing'):
            (case / 'src/main.sts').write_text(PROJECT_SOURCE)
    if spec.get('explicit'):
        (case / 'src/explicit.sts').write_text(FILE_SOURCE)


def files(case):
    return {str(path.relative_to(case)): path for path in case.rglob('*')
            if path.is_file() or path.is_symlink()}


def process_issues(process, expected_exit):
    issues = []
    if process['returncode'] != expected_exit or process['category'] != category(expected_exit):
        issues.append(f"exit={process['returncode']} category={process['category']} expected={expected_exit}")
    if INTERNAL_FAILURE.search(process['stdout'] + '\n' + process['stderr']):
        issues.append('internal compiler, sanitizer, or runtime failure')
    return issues


def expected_compile_messages(spec):
    messages = []
    if spec['project'] and not spec.get('explicit'):
        messages.append("project: main='src/main.sts' module=(root)")
    if 'diagnostic' in spec:
        messages.append('(EE) ' + spec['diagnostic'])
    else:
        source = 'src/explicit.sts' if spec.get('explicit') else 'src/main.sts'
        messages.extend(('generating code', 'linking',
                         f"compiled '{source}' -> '{spec['output']}'"))
    return messages


def run_case(compiler, work, spec, timeout, runner):
    case = work / spec['name']
    evidence = work / (spec['name'] + '-evidence')
    evidence.mkdir()
    prepare(case, spec)
    initial = files(case)
    inputs = {name: hashlib.sha256(path.read_bytes()).hexdigest()
              for name, path in initial.items()}
    output = case / spec['output'] if 'output' in spec else None
    record = dict(name=spec['name'], issues=[], processes=[], inputs=inputs)
    issues = record['issues']
    # Never accept a pre-existing expected output as evidence of compilation.
    if output and (output.exists() or output.is_symlink()):
        issues.append('expected output already exists before compilation')
        return record
    process = runner([compiler, *spec['args']], case, evidence / 'compile', timeout)
    record['processes'].append(dict(stage='compile', **process))
    issues.extend(process_issues(process, 1 if 'diagnostic' in spec else 0))
    if 'early' in spec:
        if process['stderr']:
            issues.append('early flag wrote unexpected stderr')
        if spec['early'] == 'version':
            if not re.fullmatch(r'stasha [^\s]+ \(ABI \d+\)\n', process['stdout']):
                issues.append('unexpected version output')
        elif not (process['stdout'].startswith('stasha ') and
                  '\nUsage:\n' in process['stdout'] and
                  '\nProject mode (sts.sproj in current directory):\n' in process['stdout']):
            issues.append('missing help output')
    elif process['stdout']:
        issues.append('compiler wrote unexpected stdout')
    if 'diagnostic' in spec and spec['diagnostic'] not in process['stderr']:
        issues.append('missing expected diagnostic: ' + spec['diagnostic'])
    if 'early' not in spec:
        messages = TIMESTAMP.sub(r'\1', process['stderr']).splitlines()
        if 'diagnostic' in spec:
            if messages and re.fullmatch(r'error: aborting due to [1-9]\d* previous errors?', messages[-1]):
                messages.pop()
            else:
                issues.append('missing ordinary error summary')
        if messages != expected_compile_messages(spec):
            issues.append('compiler stderr differs from expected diagnostic/log sequence')
    if output:
        if output.is_symlink() or not output.is_file() or not os.access(output, os.X_OK):
            issues.append('missing regular executable at expected output path')
        elif not issues:
            runtime = runner([str(output)], case, evidence / 'execute', timeout)
            record['processes'].append(dict(stage='execute', **runtime))
            issues.extend(process_issues(runtime, spec['exit']))
            if runtime['stdout'] != spec['stdout'] or runtime['stderr']:
                issues.append('executable output differs from exact oracle')
    final = files(case)
    # Compiler logging is expected for ordinary commands; early flags bypass it.
    allowed = set(initial)
    if 'early' not in spec:
        allowed.update(('logs/.rotation-lock', 'logs/log0-last.log'))
    if output:
        allowed.add(spec['output'])
    for name in sorted(set(final) - allowed):
        issues.append('unexpected generated file: ' + name)
    for name, expected in inputs.items():
        if name not in final or final[name].is_symlink() or hashlib.sha256(final[name].read_bytes()).hexdigest() != expected:
            issues.append('input changed or disappeared: ' + name)
    return record


def run_suite(compiler, artifacts=None, timeout=60, runner=None):
    compiler = str(Path(compiler).resolve())
    if artifacts:
        artifacts = Path(artifacts)
        artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='stasha-project-cli-', dir=artifacts))
    before = hashlib.sha256(Path(compiler).read_bytes()).hexdigest()
    results = [run_case(compiler, work, spec, timeout, runner or capture) for spec in cases()]
    after = hashlib.sha256(Path(compiler).read_bytes()).hexdigest()
    if before != after:
        results.append(dict(name='compiler-identity', issues=['compiler changed during qualification']))
    help_results = {result['name']: result for result in results}
    long_help = help_results['help-with-project']['processes'][0]['stdout']
    short_help = help_results['short-help-with-project']['processes'][0]['stdout']
    if long_help != short_help:
        help_results['short-help-with-project']['issues'].append('--help and -h outputs differ')
    failed = sum(bool(result['issues']) for result in results)
    report = dict(suite='ordinary-project-cli', compiler=compiler,
                  compiler_sha256_before=before, compiler_sha256_after=after,
                  passed=len(results) - failed, failed=failed, results=results)
    (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report, work


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(ROOT / 'bin/stasha'))
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args(argv)
    if args.timeout <= 0 or not math.isfinite(args.timeout):
        parser.error('--timeout must be positive and finite')
    try:
        report, work = run_suite(args.compiler, args.artifacts, args.timeout)
    except (OSError, ValueError) as error:
        print('FAIL project CLI runner: ' + str(error), file=sys.stderr)
        return 1
    for result in report['results']:
        print(('FAIL' if result['issues'] else 'PASS') + ' ' + result['name'])
        for issue in result['issues']:
            print('  ' + issue)
    print(f"{report['passed']} passed, {report['failed']} failed; artifacts: {work}")
    return int(bool(report['failed']))


if __name__ == '__main__':
    sys.exit(main())
