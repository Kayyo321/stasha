#!/usr/bin/env python3
"""C pointee-const mapping: C-backed positives and compile-only negatives."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/scalars'))
import run as runner

HEADER = '''typedef char Letter;
typedef const Letter ReadLetter;
typedef ReadLetter AliasReadLetter;
typedef char *MutablePointer;
int read_text(const char *p);
int read_alias(const Letter *p);
int read_chain(AliasReadLetter *p);
int mutate(char *const p);
int mutate_alias(const MutablePointer p);
const char *read_value(void);
AliasReadLetter *read_alias_value(void);
struct Opaque;
const struct Opaque *read_handle(void);
struct Opaque *const writable_handle(void);
typedef const struct Opaque *ReadOpaque;
ReadOpaque read_alias_handle(void);
typedef int (*Reader)(const char *p);
Reader get_reader(void);
'''
# Fixed regression for const qualification beyond the former 128-link cap.
HEADER += 'typedef const char Deep0;\n' + ''.join(
    f'typedef Deep{i - 1} Deep{i};\n' for i in range(1, 131))
HEADER += 'Deep130 *deep_read_value(void);\n'
C = '''#include "qualifiers.h"
#include <stdio.h>
int read_text(const char *p) { return p[0]; }
int read_alias(const Letter *p) { return p[0]; }
int read_chain(AliasReadLetter *p) { return p[0]; }
int mutate(char *const p) { *p = 66; return *p; }
int mutate_alias(const MutablePointer p) { *p = 67; return *p; }
static const char value[] = "A";
const char *read_value(void) { return value; }
AliasReadLetter *read_alias_value(void) { return value; }
Deep130 *deep_read_value(void) { return value; }
struct Opaque { int value; };
static struct Opaque object = {7};
const struct Opaque *read_handle(void) { return &object; }
struct Opaque *const writable_handle(void) { return &object; }
ReadOpaque read_alias_handle(void) { return &object; }
Reader get_reader(void) { return read_text; }
extern int probe(void);
int main(void) { int result=probe(); if(result) return result; puts("C_QUALIFIERS_OK"); return 0; }
'''
PREFIX = '@[[freestanding, crash: disable]];\ncheader "qualifiers.h" search "SEARCH";\n'
STS = '''@[[abi: c]] ext fn probe(void): i32 {
 if read_text("A") != 65 { ret 1; }
 if read_alias("A") != 65 { ret 2; }
 if read_chain("A") != 65 { ret 3; }
 stack i8 x = 65;
 if mutate(&x) != 66 || x != 66 { ret 4; }
 if mutate_alias(&x) != 67 || x != 67 { ret 5; }
 stack i8 *r a = read_value();
 stack i8 *r b = read_alias_value();
 stack i8 *r deep = deep_read_value();
 if a[0] != 65 || b[0] != 65 || deep[0] != 65 { ret 6; }
 stack void *r h = read_handle();
 stack void *r j = read_alias_handle();
 stack void *rw k = writable_handle();
 if h == nil || j == nil || k == nil { ret 7; }
 stack Reader f = get_reader();
 if f("A") != 65 { ret 8; }
 ret 0;
}
'''
NEGATIVE = {
 'mutable_parameter': 'mutate("A");',
 'mutable_typedef_parameter': 'mutate_alias("A");',
 'deep_readonly_return': 'stack i8 *rw _p = deep_read_value();',
 'readonly_return': 'stack i8 *rw _p = read_value();',
 'readonly_alias_return': 'stack i8 *rw _p = read_alias_value();',
 'readonly_opaque_return': 'stack void *rw _p = read_handle();',
 'readonly_alias_opaque_return': 'stack void *rw _p = read_alias_handle();',
}


def process_problems(process, expected_exit=0):
    """Shared fail-closed process gates, also applied to diagnostic rejections."""
    problems = []
    text = process['stderr'] + '\n' + process['stdout']
    if process['category'] != 'exit' or process['returncode'] != expected_exit:
        problems.append('unexpected process category or exit status')
    if runner.INTERNAL_FAILURE.search(text): problems.append('internal/verifier/sanitizer failure')
    if runner.WARNING.search(text) or re.search(r'\bwarning:', text, re.I): problems.append('unexpected warning')
    if expected_exit == 0 and runner.ERROR.search(text): problems.append('unexpected error diagnostic')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compiler', type=Path, required=True)
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--artifacts', type=Path, required=True)
    args = ap.parse_args()
    compiler = args.compiler.resolve(strict=True)
    args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='c-pointer-qualifiers-', dir=args.artifacts)).resolve()
    (work / 'qualifiers.h').write_text(HEADER)
    (work / 'oracle.c').write_text(C)
    prefix = PREFIX.replace('SEARCH', str(work))
    (work / 'positive.sts').write_text(prefix + STS)
    before = runner.sha256(compiler)
    report = dict(compiler=str(compiler), compiler_sha256=before, runner_sha256=runner.sha256(__file__), rows=[])
    report['c_compiler'] = runner.capture([args.cc, '--version'], work, work / 'cc-version', 10)
    for opt in range(4):
        name = 'positive-O' + str(opt)
        obj, exe = work / (name + '.o'), work / name
        row = dict(name=name, problems=[], processes=[])
        compiled = runner.capture([compiler, 'obj', work / 'positive.sts', '-o=' + str(opt), '-o', obj], work, work / (name + '-compile'), 60)
        row['processes'].append(compiled)
        row['problems'] += process_problems(compiled)
        if row['problems'] or not obj.is_file() or obj.is_symlink():
            row['problems'].append('positive compilation failed')
        else:
            linked = runner.capture([args.cc, '-O' + str(opt), work / 'oracle.c', obj, '-o', exe], work, work / (name + '-link'), 60)
            row['processes'].append(linked)
            row['problems'] += process_problems(linked)
            if linked['stderr']: row['problems'].append('unexpected linker stderr')
            if row['problems'] or not exe.is_file() or exe.is_symlink() or not os.access(exe, os.X_OK):
                row['problems'].append('C oracle link failed')
            else:
                ran = runner.capture([exe], work, work / (name + '-run'), 5)
                row['processes'].append(ran)
                row['problems'] += process_problems(ran)
                if ran['stderr']: row['problems'].append('unexpected runtime stderr')
                if ran['stdout'] != 'C_QUALIFIERS_OK\n':
                    row['problems'].append('C oracle runtime mismatch')
        report['rows'].append(row)
    for name, body in NEGATIVE.items():
        source, obj = work / (name + '.sts'), work / (name + '.o')
        source.write_text(prefix + 'ext fn invalid(void): void { ' + body + ' }\n')
        compiled = runner.capture([compiler, 'obj', source, '-o', obj], work, work / name, 60)
        problems = process_problems(compiled, 1)
        if not runner.ERROR.search(compiled['stderr']): problems.append('missing error diagnostic')
        if not re.search(r'^error[^\n]*permission', compiled['stderr'], re.M): problems.append('missing permission diagnostic')
        if not re.search(re.escape(source.name) + r':[1-9][0-9]*(?::[1-9][0-9]*)?(?:\s|$)', compiled['stderr']): problems.append('missing source location')
        if obj.exists(): problems.append('unexpected object artifact')
        # Negative programs are never linked or executed, even when accepted.
        report['rows'].append(dict(name=name, problems=problems, processes=[compiled]))
    report['input_sha256'] = {p.name: runner.sha256(p) for p in work.iterdir() if p.suffix in ('.c', '.h', '.sts')}
    report['compiler_sha256_after'] = runner.sha256(compiler)
    report['counts'] = {s: sum(bool(r['problems']) == (s == 'FAIL') for r in report['rows']) for s in ('PASS', 'FAIL')}
    report['status'] = 'PASS' if not report['counts']['FAIL'] and before == report['compiler_sha256_after'] else 'FAIL'
    (work / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    for row in report['rows']: print('FAIL' if row['problems'] else 'PASS', row['name'], '; '.join(row['problems']))
    print(report['status'], report['counts'], work / 'summary.json')
    return report['status'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
