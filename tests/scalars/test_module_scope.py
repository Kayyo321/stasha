#!/usr/bin/env python3
"""Module-aware scalar reads/writes/calls and exact export symbol regression."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import run as runner

DEP = '''mod dep;
stack u64 n = 7;
stack u64 numbers[2] = .{1, 2};
stack u64 canary = 55;
fn adjust(stack u64 x): u64 { ret x + 1; }
ext fn mutate(void): u64 {
    stack u64 before = n;
    n += 1;
    n = n + adjust(1);
    numbers[1] += 1;
    stack u64 *r pointer = &n;
    ret before ^ (n << 8) ^ (numbers[1] << 16) ^ (*pointer << 24);
}
@[[export_name: "canary"]]
ext fn exported(void): u64 { ret 23; }
'''
ROOT_SOURCE = '''@[[crash: disable]];
mod root;
imp dep;
stack u64 n = 99;
stack u64 numbers[2] = .{100, 200};
fn adjust(stack u64 x): u64 { ret x + 100; }
ext fn root_probe(void): u64 {
    stack u64 observed = dep.mutate();
    if (n != 99 || numbers[1] != 200) { ret 0; }
    ret observed;
}
'''
DRIVER = '''#include <stdint.h>
#include <stdio.h>
extern uint64_t root_probe(void);
extern uint64_t canary(void);
int main(void) {
    uint64_t first = root_probe(), second = root_probe();
    if (first != UINT64_C(0x0a030a07) || second != UINT64_C(0x0d040d0a) || canary() != 23) return 1;
    puts("MODULE_SCOPE_OK");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--artifacts', type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='scalar-module-scope-', dir=args.artifacts))
    results = []
    for opt in range(4):
        case = work / ('O' + str(opt)); case.mkdir()
        (case / 'dep.sts').write_text(DEP)
        source = case / 'root.sts'; source.write_text(ROOT_SOURCE)
        driver = case / 'driver.c'; driver.write_text(DRIVER)
        obj = case / 'module.o'; exe = case / 'probe'
        result = dict(optimization=opt, status='FAIL', processes=[], problems=[])
        for stage, command in [('compile', [compiler, 'obj', source, '-o=' + str(opt), '-o', obj]),
                               ('link', [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', driver, obj, '-o', exe]),
                               ('execute', [exe])]:
            process = runner.capture(command, case, case / stage, 60)
            result['processes'].append(process)
            if process['returncode'] != 0 or process['category'] != 'exit':
                result['problems'].append(stage + ' failed'); break
            if runner.INTERNAL_FAILURE.search(process['stderr']) or runner.WARNING.search(process['stderr']):
                result['problems'].append(stage + ' unexpected diagnostic'); break
            if stage == 'execute' and (process['stdout'] != 'MODULE_SCOPE_OK\n' or process['stderr']):
                result['problems'].append('exact runtime oracle mismatch')
        if not result['problems']: result['status'] = 'PASS'
        print(result['status'] + f' module lookup O{opt}')
        results.append(result)
    summary = dict(status='PASS' if all(r['status'] == 'PASS' for r in results) else 'FAIL',
                   compiler=str(compiler), compiler_sha256=runner.sha256(compiler), results=results)
    (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(work / 'summary.json')
    return 0 if summary['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
