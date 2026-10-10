#!/usr/bin/env python3
"""I1b.3a immediate contracts; reject/deferred fixtures are never executed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
from i1b3a_cases import matrix, inventory, LEVELS
from run_i0 import Runner, inventory_issues


def load_inventory():
    return json.loads(Path(__file__).with_name('i1b3a_inventory.json').read_text())


def schema_issues(cases):
    issues = []
    if not cases:
        return ['empty source fixture list']
    for case in cases:
        prefix = str(case.name)+': '
        if not isinstance(case.name, str) or not re.fullmatch(r'[a-z0-9][a-z0-9-]*', case.name):
            issues.append(prefix+'invalid fixture name')
        if not isinstance(case.source, str) or not case.source.strip():
            issues.append(prefix+'missing source')
        if not isinstance(case.files, dict) or any(
                not isinstance(name, str) or not name or Path(name).is_absolute() or
                '..' in Path(name).parts or name == 'input.sts' or not isinstance(text, str)
                for name, text in case.files.items()):
            issues.append(prefix+'invalid support-file mapping')
        if any(not isinstance(value, str) or not value for value in
               (case.route, case.source_contract, case.boundary, case.expected_diagnostic_file)):
            issues.append(prefix+'missing route/source/boundary/file metadata')
            continue
        if case.category not in ('valid', 'reject', 'deferred'):
            issues.append(prefix+'unknown category')
            continue
        if case.execute is not (case.category == 'valid') or case.compile_only is not (case.category != 'valid'):
            issues.append(prefix+'execution policy contradicts category')
        if case.category == 'valid':
            if (type(case.expected_exit) is not int or case.expected_exit != 0 or
                    case.expected_stdout != '' or case.expected_stderr != '' or
                    case.diagnostic is not None or case.pattern is not None or
                    case.expected_span is not None or case.seam_span is not None):
                issues.append(prefix+'valid fixture lacks exact zero/empty runtime oracle')
        else:
            if any(value is not None for value in
                   (case.expected_exit, case.expected_stdout, case.expected_stderr)):
                issues.append(prefix+'compile-only fixture has runtime oracle')
            if case.category == 'reject':
                spans = (case.expected_span, case.seam_span, case.expected_diagnostic_span)
                valid_spans = all(isinstance(span, tuple) and len(span) == 2 and
                                  all(type(value) is int and value >= 1 for value in span)
                                  for span in spans)
                expected = 'interface immediate '+case.route+': definite nominal mismatch'
                if (case.route not in ('argument', 'return') or case.diagnostic != expected or
                        case.pattern != re.escape(expected) or not valid_spans or
                        case.expected_diagnostic_file not in ('input.sts', *case.files)):
                    issues.append(prefix+'rejection lacks exact owned diagnostic/file/span oracle')
            elif (case.diagnostic is not None or case.pattern is not None or
                  case.expected_span is not None or case.seam_span is not None or not case.boundary):
                issues.append(prefix+'deferred fixture lacks explicit compile-only boundary')
    return issues


def run(compiler, artifacts):
    artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='i1b3a-', dir=artifacts))
    try:
        cases = matrix()
        issues = schema_issues(cases)
    except (AssertionError, AttributeError, IndexError, TypeError, ValueError) as error:
        cases = []
        issues = ['malformed source fixture schema: '+str(error)]
    try:
        declared = load_inventory()
        if not isinstance(declared, list) or any(not isinstance(row, dict) or not isinstance(row.get('name'), str) for row in declared):
            issues.append('invalid frozen inventory schema')
        elif not issues:
            issues += inventory_issues([case.name for case in cases], [row['name'] for row in declared])
            if not issues and declared != json.loads(json.dumps(inventory(cases))):
                issues.append('source/route/oracle inventory differs from frozen inventory')
    except (OSError, ValueError, TypeError) as error:
        issues.append('cannot read frozen inventory: '+str(error))
    # Inventory and execution policy are admission checks. Never merely record
    # drift then execute a formerly negative/deferred source as a new positive.
    if issues:
        (work / 'summary.json').write_text(json.dumps(dict(
            compiler=str(compiler), inventory_issues=issues, results=[]), indent=2)+'\n')
        print(f'FAIL before compiler execution: {issues}; evidence={work}')
        return True
    runner = Runner(compiler, work)
    sha = hashlib.sha256(Path(runner.compiler).read_bytes()).hexdigest()
    for case in cases:
        for level in LEVELS:
            name = f'{case.name}-O{level}'
            runner.case(name, case.source, pattern=case.pattern,
                        execute=case.category == 'valid', flags=[f'-o={level}'],
                        files=case.files, locfile=case.expected_diagnostic_file)
            result = runner.results[-1]
            result.update(category=case.category, route=case.route,
                          boundary=case.boundary, execute=case.execute)
            if case.category == 'reject':
                stderr = (work / name / 'compile.stderr').read_text()
                loc = re.search(re.escape('error: '+case.diagnostic) +
                                r'\n\s*--> (.*?):(\d+):(\d+)\n', stderr)
                if not loc or Path(loc[1]).name != case.expected_diagnostic_file or (
                        int(loc[2]), int(loc[3])) != case.expected_diagnostic_span:
                    result['issues'].append('exact owned diagnostic file/span differs')
            # Deferred acceptance is a deliberately unclosed boundary, never a
            # positive execution or an immediate/stored-history safety proof.
            if case.category != 'valid' and (work / name / 'execute.json').exists():
                result['issues'].append('compile-only execution prohibition violated')
    if sha != hashlib.sha256(Path(runner.compiler).read_bytes()).hexdigest():
        issues.append('compiler changed during qualification')
    summary = dict(compiler=runner.compiler, compiler_sha256=sha,
                   inventory_issues=issues, results=runner.results)
    (work / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    for result in runner.results:
        if result['issues']:
            print('FAIL', result['name'], result['issues'])
    print(f'{sum(not r["issues"] for r in runner.results)}/{len(runner.results)} checks passed; inventory={issues}; evidence={work}')
    return bool(issues or any(result['issues'] for result in runner.results))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(run(args.compiler, args.artifacts))
