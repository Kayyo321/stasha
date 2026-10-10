#!/usr/bin/env python3
"""Bounded, fail-closed static-initializer regressions against a real compiler.

Checks exact emitted constants/relocations and source diagnostics at O0/O2/O3.
These tests do not qualify unrelated runtime pointer, union or aggregate ABI paths.
"""
import argparse
import hashlib
import platform
import json
import os
import shlex
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/harness'))
from compiler_suites import capture, INTERNAL_FAILURE


def cases():
    # Every source is independently compiled, so one accepted error cannot hide
    # behind another rejection in the same module.
    yield ('address-forward', 'stack u64 *rw p = &later; stack u64 later = 42;', None,
           [r'@p = .*global ptr @later', r'@later = .*global i64 42'])
    yield ('alias-address', 'type Word: u64; stack Word n = 1; stack Word *r p = &n;', None, [r'@p = .*global ptr @n'])
    yield ('address-nullable', 'stack u64 n = 1; stack ?u64 *r p = &n;', None, [r'@p = .*global ptr @n'])
    yield ('address-array-element', 'stack u64 a[3] = .{11,22,33}; stack u64 *r p = &a[1 + 1];', None,
           [r'@p = .*getelementptr.*(?:\[3 x i64\], ptr @a, i64 0, i64 2|i8, ptr @a, i64 16)'])
    yield ('address-one-past', 'stack u64 a[2]; stack u64 *r p = &a[2];', None,
           [r'@p = .*getelementptr.*@a'])
    yield ('address-const-read', 'const u64 a = 7; stack u64 *r p = &a;', None,
           [r'@a = .*constant i64 7', r'@p = .*global ptr @a'])
    yield ('address-nested-struct', 'type S: struct { ext u64 a[2][2]; }; stack S s = .{.a = .{[0] = .{1,2}, [1] = .{3,4}}}; stack u64 *r p = &s.a[1][1];', None,
           [r'i64 1, i64 2', r'i64 3, i64 4', r'@p = .*getelementptr.*@s'])
    yield ('string-readonly-bytes', 'stack i8 *r text = "static"; stack u8 *r bytes = "bytes";', None,
           [r'private constant \[7 x i8\] c"static\\00"', r'private constant \[6 x i8\] c"bytes\\00"'])
    yield ('array-wide-values', 'stack u64 a[2][2] = .{[0] = .{0, 18446744073709551615}, [1] = .{4294967296, 7}};', None,
           [r'i64 0, i64 -1', r'i64 4294967296, i64 7'])
    yield ('array-pointer-elements', 'stack u64 n = 3; stack u64 *r a[2] = .{&n, nil};', None,
           [r'@a = .*\[2 x ptr\] \[ptr @n, ptr null\]'])
    yield ('array-designator-expression', 'stack u64 a[4] = .{[1 + 1] = 9};', None,
           [r'\[i64 0, i64 0, i64 9, i64 0\]'])
    yield ('struct-exact-field-types', 'type S: struct { ext u8 byte; ext u64 wide; }; stack S s = .{.wide = 18446744073709551615, .byte = 255};', None,
           [r'i8 -1, i64 -1'])
    yield ('function-exact-signature', 'stack fn*(stack u64): u64 callback = &f; ext fn f(stack u64 x): u64 { ret x; }', None,
           [r'@callback = .*global ptr @f'])
    yield ('integer-pointer-cast', 'stack u64 *rw p = (u64 *rw)18446744073709551615;', None,
           [r'@p = .*inttoptr \(i64 -1 to ptr\)'])
    rejects = [
        ('string-writable', 'stack i8 *rw p = "readonly";', 'read-only byte pointer'),
        ('string-wrong-pointee', 'stack u64 *r p = "wrong";', 'incompatible'),
        ('string-wrong-depth', 'stack i8 *r *r p = "wrong";', 'incompatible'),
        ('string-function-pointer', 'stack fn*(void): void p = "wrong";', 'incompatible'),
        ('string-heap-domain', 'heap i8 *r p = "wrong";', 'heap-owned'),
        ('address-wrong-width', 'stack u8 n; stack u64 *rw p = &n;', 'incompatible'),
        ('address-wrong-signedness', 'stack u64 n; stack i64 *rw p = &n;', 'incompatible'),
        ('address-wrong-depth', 'stack u64 n; stack u64 ** p = &n;', 'incompatible'),
        ('address-array-is-not-element', 'stack u64 a[2]; stack u64 *r p = &a;', 'incompatible'),
        ('address-wrong-struct', 'type A: struct { ext u64 x; }; type B: struct { ext u64 x; }; stack A a; stack B *r p = &a;', 'incompatible'),
        ('address-const-write', 'const u64 n = 7; stack u64 *rw p = &n;', 'writable.*const or final'),
        ('address-final-write', 'final u64 n = 7; stack u64 *rw p = &n;', 'writable.*const or final'),
        ('address-const-array-write', 'const u64 a[2] = .{1,2}; stack u64 *rw p = &a[1];', 'writable.*const or final'),
        ('address-const-member-write', 'type S: struct { ext u64 x; }; const S s = .{.x = 1}; stack u64 *rw p = &s.x;', 'writable.*const or final'),
        ('address-nested-qualification', 'stack u64 *r q = nil; stack u64 ** p = &q;', 'incompatible'),
        ('address-heap-domain', 'stack u64 n; heap u64 *rw p = &n;', 'heap-owned'),
        ('address-tls', 'tls u64 n; stack u64 *rw p = &n;', 'unsupported static initializer'),
        ('address-out-of-range', 'stack u64 a[2]; stack u64 *r p = &a[3];', 'unsupported static initializer'),
        ('address-negative-index', 'stack u64 a[2]; stack u64 *r p = &a[-1];', 'unsupported static initializer'),
        ('address-past-nested-array', 'stack u64 a[2][2]; stack u64 *r p = &a[2][0];', 'unsupported static initializer'),
        ('function-heap-domain', 'heap fn*(void): u64 p = &f; fn f(void): u64 { ret 1; }', 'heap'),
        ('function-as-data', 'stack u64 *r p = &f; fn f(void): u64 { ret 1; }', 'incompatible'),
        ('function-wrong-return', 'stack fn*(void): u64 p = &f; fn f(void): u8 { ret 1; }', 'incompatible'),
        ('function-wrong-parameter', 'stack fn*(stack u64): u64 p = &f; fn f(stack i64 n): u64 { ret 1; }', 'incompatible'),
        ('cast-wrong-destination', 'stack u64 *rw p = (u8 *rw)1;', 'incompatible'),
        ('cast-permission-widening', 'stack u64 *rw p = (u64 *r)1;', 'incompatible'),
        ('array-signedness', 'const i8 neg = -1; stack u64 a[1] = .{neg};', 'global scalar initializer'),
        ('array-wrong-pointer-element', 'stack u8 n; stack u64 *r a[1] = .{&n};', 'incompatible'),
        ('array-writable-string', 'stack i8 *rw a[1] = .{"readonly"};', 'read-only byte pointer'),
        ('array-nested-literal-overflow', 'stack u8 a[1][1] = .{[0] = .{256}};', 'global scalar initializer'),
        ('array-field-designator', 'stack u64 a[2] = .{.x = 1};', 'field designator requires a struct'),
        ('array-negative-designator', 'stack u64 a[2] = .{[-1] = 1};', 'nonnegative constant index'),
        ('array-oob-designator', 'stack u64 a[2] = .{[2] = 1};', 'out of range'),
        ('array-spread-unsupported', 'stack u64 a[2] = .{..(0..2)};', 'static initializer|constant'),
        ('struct-index-designator', 'type S: struct { ext u64 x; }; stack S s = .{[0] = 1};', 'nonnegative constant index'),
        ('struct-unknown-field', 'type S: struct { ext u64 x; }; stack S s = .{.missing = 1};', 'out of range'),
        ('struct-too-many-fields', 'type S: struct { ext u64 x; }; stack S s = .{1,2};', 'out of range'),
        ('struct-field-overflow', 'type S: struct { ext u8 x; }; stack S s = .{.x = 256};', 'global scalar initializer'),
        ('array-inferred-unsupported', 'stack u64 a[] = .{1,2};', 'positive literal extent'),
        ('array-named-unsupported', 'const u64 N = 2; stack u64 a[N];', 'positive literal extent'),
        ('array-unresolved-extent', 'stack u64 a[Missing];', 'positive literal extent'),
        ('array-target-overflow', 'stack u64 a[9223372036854775807];', 'overflows|exceeds target'),
        ('global-unknown-type', 'stack Missing x;', 'unknown|cyclic'),
        ('global-void-type', 'stack void x;', 'void|unsized|inferred'),
        ('global-call-unsupported', 'stack u64 n = f(); fn f(void): u64 { ret 1; }', 'global scalar initializer'),
    ]
    for name, source, diagnostic in rejects:
        yield name, source, diagnostic, []



OBJECT_SOURCE = """@[[freestanding]]; mod static_object;
stack u64 *rw later_address = &later;
stack i8 *r label = "kernel";
stack u64 matrix[2][2] = .{[0] = .{0, 18446744073709551615}, [1] = .{4294967296, 7}};
type Packet: struct { ext u8 small; ext u64 wide; ext u64 grid[2][2]; ext u64 *r pointer; };
stack Packet packet = .{.small = 255, .wide = 18446744073709551615, .grid = .{[0] = .{1,2}, [1] = .{3,4}}, .pointer = &later};
stack u64 *r nested_address = &packet.grid[1][1];
stack u64 *r one_past = &matrix[1][2];
stack fn*(stack u64): u64 callback = &identity;
stack u64 later = 18446744073709551615;
const u64 immutable = 7;
stack u64 *r readonly_address = &immutable;
ext fn identity(stack u64 n): u64 { ret n + 1; }
"""
OBJECT_ORACLE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct Packet { uint8_t small; uint64_t wide; uint64_t grid[2][2]; const uint64_t *pointer; };
extern uint64_t later, matrix[2][2];
extern const uint64_t immutable;
extern uint64_t *later_address;
extern const uint64_t *nested_address, *one_past, *readonly_address;
extern const char *label;
extern struct Packet packet;
extern uint64_t (*callback)(uint64_t);
#define CHECK(X) do { if (!(X)) { fprintf(stderr, "static initializer check failed at line %d: %s\n", __LINE__, #X); return 1; } } while (0)
int main(void) {
    CHECK(later == UINT64_MAX && later_address == &later);
    CHECK(strcmp(label, "kernel") == 0);
    CHECK(matrix[0][0] == 0 && matrix[0][1] == UINT64_MAX);
    CHECK(matrix[1][0] == UINT64_C(4294967296) && matrix[1][1] == 7);
    CHECK(packet.small == UINT8_MAX && packet.wide == UINT64_MAX);
    CHECK(packet.grid[0][0] == 1 && packet.grid[0][1] == 2 && packet.grid[1][0] == 3 && packet.grid[1][1] == 4);
    CHECK(packet.pointer == &later && nested_address == &packet.grid[1][1] && *nested_address == 4);
    CHECK(one_past == &matrix[1][2]);
    CHECK(immutable == 7 && readonly_address == &immutable);
    CHECK(callback(41) == 42);
    *later_address = 23;
    CHECK(later == 23 && *packet.pointer == 23);
    puts("static object data and relocations: PASS");
    return 0;
}
"""

def object_oracle(args, compiler, work, opt):
    case = work / f'native-object-O{opt}'
    case.mkdir()
    source, obj, oracle, executable = case / 'input.sts', case / 'output.o', case / 'oracle.c', case / 'oracle'
    source.write_text(OBJECT_SOURCE)
    oracle.write_text(OBJECT_ORACLE)
    issues, processes = [], []
    steps = [
        ('compile', [compiler, 'obj', str(source), '-o', str(obj), '-o=' + opt]),
        ('link', shlex.split(args.cc) + ['-std=c11', str(oracle), str(obj), '-o', str(executable)]),
        ('execute', [str(executable)]),
    ]
    for stage, command in steps:
        process = capture(command, ROOT, case / stage, args.timeout)
        processes.append({'stage': stage, **process})
        if process['returncode'] != 0 or process['category'] != 'success' or INTERNAL_FAILURE.search(process['stderr']):
            issues.append(stage + ' failed, crashed, timed out, or reported an internal failure')
            break
        if stage == 'execute' and process['stdout'] != 'static object data and relocations: PASS\n':
            issues.append('object oracle output differs')
    print(('FAIL' if issues else 'PASS') + f' native-object O{opt}')
    return {'name': 'native-object', 'optimization': opt, 'issues': issues, 'processes': processes}



def module_scope_oracle(args, compiler, work, opt):
    case = work / f'module-scope-O{opt}'
    case.mkdir()
    source, ir, obj, oracle, executable = (case / p for p in ('input.sts', 'output.ll', 'output.o', 'oracle.c', 'oracle'))
    source.write_text('@[[freestanding]]; mod root; imp dep; const u64 n = 99;\n')
    (case / 'dep.sts').write_text('mod dep; const u64 n = 7; stack u64 *r p = &n; const u64 copied = n;\n')
    oracle.write_text('#include <stdint.h>\nextern const uint64_t n, dep__n, dep__copied; extern const uint64_t *dep__p;\nint main(void) { return !(n == 99 && dep__n == 7 && dep__copied == 7 && dep__p == &dep__n); }\n')
    issues, processes = [], []
    steps = [
        ('ir', [compiler, 'ir', str(source), '-o', str(ir), '-o=' + opt]),
        ('obj', [compiler, 'obj', str(source), '-o', str(obj), '-o=' + opt]),
        ('link', shlex.split(args.cc) + ['-std=c11', str(oracle), str(obj), '-o', str(executable)]),
        ('execute', [str(executable)]),
    ]
    for stage, command in steps:
        process = capture(command, ROOT, case / stage, args.timeout)
        processes.append({'stage': stage, **process})
        if process['returncode'] != 0 or process['category'] != 'success' or INTERNAL_FAILURE.search(process['stderr']):
            issues.append(stage + ' failed, crashed, timed out, or reported an internal failure')
            break
        if stage == 'ir':
            text = ir.read_text()
            if not re.search(r'@dep__p = .*global ptr @dep__n', text) or not re.search(r'@dep__copied = .*constant i64 7', text):
                issues.append('imported module initializer used a colliding root symbol')
    print(('FAIL' if issues else 'PASS') + f' module-scope O{opt}')
    return {'name': 'module-scope', 'optimization': opt, 'issues': issues, 'processes': processes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(ROOT / 'bin/stasha'))
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--skip-object-oracle', action='store_true')
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--opt-levels', default='0,2,3')
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve())
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='static-initializers-', dir=args.artifacts))
    with open(compiler, 'rb') as stream:
        compiler_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    (work / 'metadata.json').write_text(json.dumps({'compiler': compiler, 'compiler_sha256': compiler_hash, 'platform': platform.platform(), 'python': sys.version, 'optimization_levels': args.opt_levels, 'cc': args.cc}, indent=2) + '\n')
    results = []
    for opt in args.opt_levels.split(','):
        for name, source, diagnostic, patterns in cases():
            case = work / f'{name}-O{opt}'
            case.mkdir()
            path, output = case / 'input.sts', case / 'output.ll'
            path.write_text('@[[freestanding]]; mod initializers;\n' + source + '\n')
            output.write_text('stale output must be replaced or removed')
            process = capture([compiler, 'ir', str(path), '-o', str(output), '-o=' + opt], ROOT, case / 'compile', args.timeout)
            issues = []
            expected = 1 if diagnostic else 0
            if process['returncode'] != expected:
                issues.append(f"exit {process['returncode']}, expected {expected}")
            if process['category'] in ('timeout', 'crash', 'launch-error') or INTERNAL_FAILURE.search(process['stderr']):
                issues.append('timeout, compiler crash, sanitizer, or verifier failure')
            if diagnostic:
                if not re.search(diagnostic, process['stderr'], re.I):
                    issues.append('missing expected source diagnostic: ' + diagnostic)
                if output.exists():
                    issues.append('stale or failed output survived rejection')
            elif not output.is_file():
                issues.append('missing output')
            else:
                ir = output.read_text()
                for pattern in patterns:
                    if not re.search(pattern, ir):
                        issues.append('missing constant/relocation pattern: ' + pattern)
            results.append({'name': name, 'optimization': opt, 'issues': issues, 'process': process})
            print(('FAIL' if issues else 'PASS') + f' {name} O{opt}')
            for issue in issues:
                print('  ' + issue)
        if not args.skip_object_oracle:
            results.append(object_oracle(args, compiler, work, opt))
            results.append(module_scope_oracle(args, compiler, work, opt))
    (work / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    failed = sum(bool(result['issues']) for result in results)
    print(f'{len(results)-failed} passed, {failed} failed; artifacts: {work}')
    return bool(failed)


if __name__ == '__main__':
    sys.exit(main())
