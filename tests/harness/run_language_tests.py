#!/usr/bin/env python3
"""Compile AND execute Stasha test blocks using fresh, retained artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from compiler_suites import ROOT, capture, compiler_problems, INTERNAL_FAILURE


def evaluate_runtime(result):
    problems = []
    if result['returncode'] != 0 or result['category'] in ('timeout', 'crash', 'launch-error'):
        problems.append('test executable did not exit successfully')
    if INTERNAL_FAILURE.search(result['stdout'] + '\n' + result['stderr']):
        problems.append('runtime sanitizer/internal failure')
    summaries = re.findall(r'^([0-9]+) passed, ([0-9]+) failed\s*$', result['stdout'], re.M)
    if len(summaries) != 1 or int(summaries[0][0]) == 0 or int(summaries[0][1]) != 0:
        problems.append('missing, empty, duplicate or failing test summary')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('compiler', type=Path)
    ap.add_argument('source', type=Path)
    ap.add_argument('--artifacts', type=Path, default=ROOT/'build/qualification/language')
    ap.add_argument('--timeout', type=float, default=60)
    args, flags = ap.parse_known_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=args.source.stem+'-', dir=args.artifacts))
    compiler, source = args.compiler.resolve(), args.source.resolve()
    output = work / ('program.exe' if os.name == 'nt' else 'program')
    compiled = capture([str(compiler), 'test', str(source), '-o', str(output), *flags],
                       ROOT, work/'compile', args.timeout)
    problems = compiler_problems(compiled, {'outcome':'success', 'source':str(source)})
    if not output.is_file() or output.is_symlink(): problems.append('fresh test executable missing')
    report = {'compiler_sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),
              'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
              'compile':compiled, 'problems':problems}
    print(compiled['stdout'], end=''); print(compiled['stderr'], end='', file=sys.stderr)
    if not problems:
        executed = capture([str(output)], work, work/'run', args.timeout)
        report['run'] = executed
        problems.extend(evaluate_runtime(executed))
        print(executed['stdout'], end=''); print(executed['stderr'], end='', file=sys.stderr)
    (work/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Retained language-test artifacts:', work)
    for problem in problems: print('FAIL:', problem, file=sys.stderr)
    return bool(problems)

if __name__ == '__main__': sys.exit(main())
