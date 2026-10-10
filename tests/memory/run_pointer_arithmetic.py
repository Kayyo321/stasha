#!/usr/bin/env python3
"""Positive typed pointer-stride regressions within live initialized allocations.

This suite does not qualify arithmetic permissions, pointer bounds, provenance,
pointer difference, or integer-plus-pointer operators. Every case is valid under
the existing pointer-left addition/subtraction contract. Pointer-slot stride is
checked by address comparison so a wrong stride never dereferences a bad pointer.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/scalars'))
import run as runner
from generate import Case


def cases():
    out = []

    def add(name, body, declarations=''):
        source = ('@[[crash: disable]];\nmod pointer_stride_' + name + ';\n' + declarations +
                  '\nfn main(void): i32 {\n' + body + '\nret 0;\n}\n')
        out.append(Case(name, source))

    for kind in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'f32', 'f64'):
        add('addition_' + kind,
            f'stack {kind} a[3] = .{{7, 9, 11}}; stack {kind} *rw+ p = &a[0]; '
            'if (p + 1)[0] != 9 { ret 1; }')
    add('subtraction_unsigned_offset',
        'stack i32 a[3] = .{7, 9, 11}; stack i32 *rw+ p = &a[2]; stack u16 step = 1; '
        'if (p - step)[0] != 9 { ret 1; }')
    add('addition_signed_negative',
        'stack i32 a[3] = .{7, 9, 11}; stack i32 *rw+ p = &a[2]; stack i8 step = -1; '
        'if (p + step)[0] != 9 { ret 1; }')
    add('subtraction_signed_negative',
        'stack i32 a[3] = .{7, 9, 11}; stack i32 *rw+ p = &a[0]; stack i8 step = -1; '
        'if (p - step)[0] != 9 { ret 1; }')
    add('chained_dereference',
        'stack i64 a[3] = .{7, 9, 11}; stack i64 *rw+ p = &a[0]; '
        'if *(p + 1 + 1) != 11 { ret 1; }')
    add('alias_pointee',
        'stack Word a[3] = .{7, 9, 11}; stack Word *rw+ p = &a[0]; '
        'if (p + 1)[0] != 9 { ret 1; }', 'type Word: u32;')
    add('field_pointer',
        'stack i32 a[3] = .{7, 9, 11}; stack Box b = .{&a[0]}; '
        'if (b.ptr + 1)[0] != 9 { ret 1; }',
        'type Box: struct { ext i32 *rw+ ptr; };')
    add('call_pointer',
        'stack i32 a[3] = .{7, 9, 11}; if (identity(&a[0]) + 1)[0] != 9 { ret 1; }',
        'fn identity(stack i32 *rw+ p): i32 *rw+ { ret p; }')
    add('struct_pointee',
        'stack Pair a[3] = .{}; stack Pair *rw+ p = &a[0]; '
        'if p + 1 != &a[1] { ret 1; }',
        'type Pair: struct { ext i32 first; ext i64 second; };')
    add('pointer_slot_pointee',
        'stack i32 x = 7; stack i32 y = 9; stack i32 z = 11; '
        'stack i32 *rw a[3] = .{&x, &y, &z}; stack i32 *rw *rw+ p = &a[0]; '
        'if p + 1 != &a[1] { ret 1; }')
    add('address_result',
        'stack i32 a[3] = .{7, 9, 11}; if (&a[0] + 1)[0] != 9 { ret 1; }')
    add('derived_store',
        'stack i32 a[3] = .{7, 9, 11}; stack i32 *rw+ p = &a[0]; '
        '(p + 1)[0] = 13; if a[0] != 7 || a[1] != 13 || a[2] != 11 { ret 1; }')
    add('derived_compound',
        'stack i32 a[3] = .{7, 9, 11}; stack i32 *rw+ p = &a[0]; '
        '(p + 1)[0] += 4; if a[0] != 7 || a[1] != 13 || a[2] != 11 { ret 1; }')
    add('operands_once',
        'stack i32 a[3] = .{7, 9, 11}; stack i32 count = 0; '
        'if (base(&a[0], &count) + offset(&count))[0] != 9 { ret 1; } '
        'if count != 2 { ret 2; }',
        'fn base(stack i32 *rw+ p, stack i32 *rw count): i32 *rw+ { count[0] += 1; ret p; }\n'
        'fn offset(stack i32 *rw count): i16 { count[0] += 1; ret 1; }')
    add('void_byte_compatibility',
        'stack u8 a[3] = .{7, 9, 11}; stack void *rw+ p = (void *rw+)&a[0]; '
        'if p + 1 != (void *rw+)&a[1] { ret 1; }')
    return out


def expected_stride(name):
    if name.startswith('addition_') and name.removeprefix('addition_') in (
            'i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'f32', 'f64'):
        kind = name.removeprefix('addition_')
        return {'f32': 'float', 'f64': 'double'}.get(kind, 'i' + kind[1:])
    if name == 'chained_dereference': return 'i64'
    if name == 'pointer_slot_pointee': return 'ptr'
    if name == 'struct_pointee': return None  # Layout checked by the runtime address oracle.
    if name == 'void_byte_compatibility': return 'i8'
    return 'i32'


def check_stride(result, work):
    """Retain the source-typed GEP as an independent pre-optimization oracle."""
    expected = expected_stride(result['name'])
    if not expected: return
    ir = work / 'pre.ll'
    if not ir.is_file() or not re.search(
            r'%ptrarith\w* = getelementptr ' + re.escape(expected) + r',', ir.read_text()):
        result['problems'].append('missing typed pointer arithmetic GEP: ' + expected)
        result['status'] = 'FAIL'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True, type=Path)
    p.add_argument('--opt', default='0,1,2,3')
    p.add_argument('--artifacts', required=True, type=Path)
    p.add_argument('--case', action='append', default=[])
    args = p.parse_args()
    args.compiler = args.compiler.resolve(strict=True)
    opts = [int(x) for x in args.opt.split(',')]
    if not opts or any(x not in range(4) for x in opts): p.error('optimization must be 0,1,2,3')
    selected = cases()
    if args.case:
        unknown = set(args.case) - {c.name for c in selected}
        if unknown: p.error('unknown case: ' + ', '.join(sorted(unknown)))
        selected = [c for c in selected if c.name in args.case]
    args.compiler_arg = []
    args.compile_timeout, args.run_timeout = 30, 5
    args.trap_hook_object = None
    args.require_trap_reasons = False
    args.trap_hook_pattern, args.trap_exit_code = None, None
    args.trap_signal = runner.default_trap_signal()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='pointer-arithmetic-', dir=args.artifacts))
    before = runner.sha256(args.compiler)
    summary = dict(compiler=str(args.compiler), compiler_sha256=before,
                   runner_sha256=runner.sha256(__file__), results=[])
    for case in selected:
        for opt in opts:
            case_work = work / (case.name + '-O' + str(opt))
            result = runner.run_case(case, opt, args, case_work)
            check_stride(result, case_work)
            summary['results'].append(result)
            print(result['status'], case.name, 'O' + str(opt), '; '.join(result['problems']))
    summary['compiler_sha256_after'] = runner.sha256(args.compiler)
    summary['counts'] = {s: sum(r['status'] == s for r in summary['results']) for s in ('PASS', 'FAIL')}
    summary['status'] = 'PASS' if summary['counts']['FAIL'] == 0 and before == summary['compiler_sha256_after'] else 'FAIL'
    (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(summary['status'], summary['counts'], work / 'summary.json')
    return summary['status'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
