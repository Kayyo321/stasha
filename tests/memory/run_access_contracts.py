#!/usr/bin/env python3
"""Bounded hosted P3 access-contract regressions; unsafe inputs never run."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/scalars'))
import run as scalar_runner
from generate import Case


def program(name, body='', declarations=''):
    return ('@[[crash: disable]];\nmod memory_' + name + ';\n' + declarations +
            '\nfn main(void): i32 {\n' + body + '\n}\n')


def cases():
    result = []
    def reject(name, body='', declarations='', diagnostic='permission|read-only|write-only|writable|readable'):
        result.append(Case(name, program(name, body or 'ret 0;', declarations),
                           outcome='rejection', diagnostic=r'^error(?:\[[^\]]+\])?:[^\n]*(?:' + diagnostic + ')'))
    pair = 'stack i32 x = 7;\nstack i32 *{permission} p = &x;\n'
    for path, read, store, compound in (
        ('deref', '*.(p)', '*.(p) = 9;', '*.(p) += 1;'),
        ('index', 'p[0]', 'p[0] = 9;', 'p[0] += 1;'),
        ('member', 'p.value', 'p.value = 9;', 'p.value += 1;'),
    ):
        declarations = 'type Box: struct { ext i32 value; };\n' if path == 'member' else ''
        init = ('stack Box x = .{.value = 7};\nstack Box *{permission} p = &x;\n'
                if path == 'member' else pair)
        reject('readonly_' + path + '_store', init.replace('{permission}', 'r') + store + '\nret 0;', declarations)
        reject('writeonly_' + path + '_read', init.replace('{permission}', 'w') + 'ret ' + read + ';', declarations)
        reject('writeonly_' + path + '_compound', init.replace('{permission}', 'w') + compound + '\nret 0;', declarations)
    reject('readonly_deref_compound', pair.format(permission='r') + '*.(p) += 1;\nret 0;')
    reject('readonly_index_compound', pair.format(permission='r') + 'p[0] += 1;\nret 0;')
    for path, expression, typ in (('deref', '*.(p)', 'i32'), ('index', 'p[0]', 'i32'), ('member', 'p.value', 'Box')):
        decl = 'type Box: struct { ext i32 value; };\n' if typ == 'Box' else ''
        decl += 'ext fn unchecked_param(stack ?' + typ + ' *rw p): i32 { ret ' + expression + '; }\n'
        reject('nullable_' + path, declarations=decl, diagnostic='nullable|nil-check|possibly.*nil|may be nil')
    reject('known_nil_index', 'stack i32 *rw p = nil;\np = nil;\nret p[0];', diagnostic='dereference of nil|nil pointer')
    reject('known_nil_deref', 'stack i32 *rw p = nil;\np = nil;\nret *.(p);', diagnostic='dereference of nil|nil pointer')
    reject('known_nil_store', 'stack i32 *rw p = nil;\np = nil;\np[0] = 1;\nret 0;', diagnostic='dereference of nil|nil pointer')
    reject('direct_stack_escape', declarations='fn escape(void): stack i32 *rw { stack i32 x = 7; ret &x; }', diagnostic='cannot return.*(stack|local)|escape|outlive')
    reject('alias_stack_escape', declarations='fn escape(void): stack i32 *rw { stack i32 x = 7; stack i32 *rw p = &x; ret p; }', diagnostic='cannot return.*(stack|local)|escape|outlive')
    reject('direct_scope_escape', 'stack i32 *rw p = nil;\n{ stack i32 x = 7; p = &x; }\nret 0;', diagnostic='outlive|scope mismatch|lifetime')
    reject('alias_scope_escape', 'stack i32 *rw p = nil;\n{ stack i32 x = 7; stack i32 *rw q = &x; p = q; }\nret 0;', diagnostic='outlive|scope mismatch|lifetime')
    reject('freed_pointer_read', 'heap i32 *rw p = new.(sizeof.(i32));\np[0] = 7;\nrem.(p);\nret p[0];', diagnostic='use-after-free|freed pointer|closed provenance')
    reject('freed_alias_read', 'heap i32 *rw p = new.(sizeof.(i32));\np[0] = 7;\nheap i32 *rw q = p;\nrem.(p);\nret q[0];', diagnostic='use-after-free|freed pointer|closed provenance')
    backing = 'stack i32 backing[2] = .{7, 8};\nstack []i32 view = backing[0:1];\n'
    for access, body in [('read', 'ret view[1];'), ('store', 'view[1] = 9;\nret 0;'), ('compound', 'view[1] += 1;\nret 0;')]:
        name = 'slice_oob_' + access
        result.append(Case(name, program(name, backing + body), outcome='trap', reason='slice bounds',
                           observations=['Index is outside view length but inside the two-element live backing array.']))
    name = 'slice_in_bounds'
    result.append(Case(name, program(name, backing + 'view[0] = 9;\nview[0] += 1;\nif (view[0] != 10 || backing[0] != 10 || backing[1] != 8) { ret 1; }\nprint.("MEMORY_OK\\n");\nret 0;'), stdout='MEMORY_OK\n'))
    name = 'slice_unsafe'
    result.append(Case(name, program(name, backing + 'unsafe { view[1] = 9; view[1] += 1; if (view[1] != 10) { ret 1; } }\nif (backing[1] != 10) { ret 2; }\nprint.("MEMORY_OK\\n");\nret 0;'), stdout='MEMORY_OK\n'))
    name = 'nullable_guarded'
    result.append(Case(name, program(name, 'stack i32 x = 7;\nif (guarded(&x) != 7 || guarded(nil) != 0) { ret 1; }\nprint.("MEMORY_OK\\n");\nret 0;',
        'fn guarded(stack ?i32 *rw p): i32 { if (p != nil) { ret *.(p); } ret 0; }'), stdout='MEMORY_OK\n'))
    for name, expression, decl, setup in (
        ('typed_member_deref', '*.(box.ptr)', 'type Box: struct { ext i32 *rw ptr; };',
         'stack Box box = .{.ptr = &x};'),
        ('typed_call_deref', '*.(identity(&x))',
         'fn identity(stack i32 *rw p): stack i32 *rw { ret p; }', ''),
    ):
        body = ('stack i32 x = 4660;\n' + setup + '\nstack i32 observed = ' + expression +
                ';\nprint.("value={}\\n", observed);\nif (observed != 4660) { ret 1; }\nret 0;')
        result.append(Case(name, program(name, body, decl), stdout='value=4660\n'))
    name = 'typed_deref_writes'
    decl = ('type Box: struct { ext i32 *rw ptr; };\n'
            'fn identity(stack i32 *rw p): stack i32 *rw { ret p; }')
    body = ('stack i32 x = 0; stack Box box = .{.ptr = &x};\n'
            '*.(box.ptr) = 4660; if (x != 4660) { ret 1; }\n'
            '*.(identity(&x)) = 9000; if (x != 9000) { ret 2; }\n'
            '*.(box.ptr) += 1000; if (x != 10000) { ret 3; }\n'
            '*.(identity(&x)) += 1000; if (x != 11000) { ret 4; }\n'
            'print.("MEMORY_OK\\n"); ret 0;')
    result.append(Case(name, program(name, body, decl), stdout='MEMORY_OK\n'))
    name = 'writable_pointee_readonly_container'
    decl = 'type Box: struct { ext i32 *rw ptr; };'
    body = ('stack i32 x = 7; stack Box box = .{.ptr = &x}; stack Box *r p = &box;\n'
            'p.ptr[0] = 9000; p.ptr[0] += 1000; if (x != 10000) { ret 1; }\n'
            'print.("MEMORY_OK\\n"); ret 0;')
    result.append(Case(name, program(name, body, decl), stdout='MEMORY_OK\n'))
    name = 'slice_unsigned_index'
    body = ('stack i32 backing[256] = .{}; stack []i32 view = backing[:];\n'
            'stack u8 index = 128; view[index] = 9000; view[index] += 1000;\n'
            'if (view[index] != 10000 || backing[128] != 10000) { ret 1; }\n'
            'print.("MEMORY_OK\\n"); ret 0;')
    result.append(Case(name, program(name, body), stdout='MEMORY_OK\n'))
    for operation, statement in [('read', 'ret box.view[1];'), ('store', 'box.view[1] = 9; ret 0;'),
                                  ('compound', 'box.view[1] += 1; ret 0;')]:
        name = 'slice_member_oob_' + operation
        body = ('stack i32 backing[2] = .{7, 8}; stack []i32 view = backing[0:1];\n'
                'stack Box box = .{.view = view};\n' + statement)
        result.append(Case(name, program(name, body, 'type Box: struct { ext []i32 view; };'),
                           outcome='trap', reason='slice bounds'))
    reject('nonpointer_deref', 'stack i32 x = 7; ret *.(x);', diagnostic='dereference.*pointer|pointer.*dereference')
    reject('writeonly_container_pointer_slot', declarations=
           'type Box: struct { ext i32 *rw ptr; };\n'
           'fn mutate(stack Box *w p): void { p.ptr[0] = 9; }')
    reject('double_pointer_method', declarations=
           'type Box: struct { ext i32 x; ext fn set(void): void { this.x = 5; } };\n'
           'fn mutate(stack Box *rw *rw p): void { p.set(); }',
           diagnostic='method receiver.*multiple pointer levels|explicit dereference')
    reject('receiver_effect_readonly_mutation', declarations=
           'type Box: struct { ext i32 x; ext fn set(void): void { this.x = 5; } };\n'
           'fn mutate(stack Box *r p): void { p.set(); }')
    reject('readonly_index_address', 'stack i32 x = 7; stack i32 *r p = &x;\n'
           'stack i32 *rw q = &p[0]; *.(q) = 8; ret 0;')
    # These destinations are intentionally unused: keep the strict no-warning
    # oracle while isolating the invalid conversion/address operation.
    reject('readonly_field_conversion',
           'stack i32 x = 7; stack Box b = .{&x}; stack i32 *rw _p = b.ptr; ret 0;',
           'type Box: struct { ext i32 *r ptr; };\n')
    reject('readonly_call_conversion',
           'stack i32 x = 7; stack i32 *rw _p = identity(&x); ret 0;',
           'fn identity(stack i32 *r p): i32 *r { ret p; }\n')
    for qualifier in ('const', 'final'):
        reject(qualifier + '_array_address', qualifier + ' i32 a[1] = .{7};\n'
               'stack i32 *rw _p = &a[0]; ret 0;')
        reject(qualifier + '_field_address', qualifier + ' Box a = .{7};\n'
               'stack i32 *rw _p = &a.value; ret 0;',
               'type Box: struct { ext i32 value; };\n')
    result.append(Case('unchecked_slice_store_compound', program('unchecked_slice_store_compound',
                      'stack i32 a[2] = .{7, 8}; stack []i32 view = a[0:1];\n'
                      'view[unchecked: 1] = 11; view[unchecked: 1] += 2;\n'
                      'if a[1] != 13 { ret 1; } ret 0;')))
    return result


def provenance():
    def git(*args):
        p = subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True, timeout=10)
        return {'returncode': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}
    relevant = ['src/codegen/cg_expr.c', 'src/codegen/cg_safety.c', 'src/codegen/cg_scalar.c',
                'src/codegen/cg_stmt.c', 'src/codegen/cg_symtab.c', 'src/codegen/codegen.c',
                'wiki/Safety-System.md', 'wiki/Kernel-Readiness-Implementation-Plan.md']
    return {'revision': git('rev-parse', 'HEAD'), 'status': git('status', '--short'),
            'reviewed_file_sha256': {p: scalar_runner.sha256(ROOT / p) for p in relevant},
            'note': 'Working-tree fingerprints describe review inputs, not a claimed compiler build snapshot.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True, type=Path)
    p.add_argument('--opt', default='0,2')
    p.add_argument('--case', action='append', default=[])
    p.add_argument('--artifacts', type=Path)
    p.add_argument('--list', action='store_true')
    p.add_argument('--scope', choices=['all', 'access'], default='all',
                   help='access excludes separately scoped nullable and provenance/escape analyses')
    args = p.parse_args()
    selected = [c for c in cases() if (not args.case or c.name in args.case) and
                (args.scope == 'all' or not c.name.startswith(('nullable_', 'known_nil_', 'direct_', 'alias_', 'freed_', 'receiver_effect_')))]
    if not selected or set(args.case) - {c.name for c in selected}: p.error('empty or unknown case selection')
    if args.list:
        print(json.dumps([{'name': c.name, 'outcome': c.outcome} for c in selected], indent=2)); return 0
    opts = [int(o) for o in args.opt.split(',')]
    if not opts or any(o not in range(4) for o in opts): p.error('optimization levels must be in 0,1,2,3')
    args.compiler = args.compiler.resolve(strict=True)
    args.compiler_arg = []
    args.compile_timeout, args.run_timeout = 30, 5
    args.trap_hook_object = None
    args.require_trap_reasons = False
    args.trap_hook_pattern, args.trap_exit_code = None, None
    args.trap_signal = scalar_runner.default_trap_signal()
    if not args.trap_signal: p.error('no qualified native trap signal on this host')
    if args.artifacts: args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='memory-access-', dir=args.artifacts))
    before = scalar_runner.sha256(args.compiler)
    summary = {'compiler': str(args.compiler), 'compiler_sha256': before,
               'utc': datetime.now(timezone.utc).isoformat(), 'host': platform.platform(),
               'runner_sha256': scalar_runner.sha256(__file__), 'source': provenance(), 'results': []}
    summary['version'] = scalar_runner.capture([args.compiler, '--version'], work, work / 'version', 10)
    for c in selected:
        # Diagnostics are frontend contracts; run each once, independent of optimizer.
        for opt in ([0] if c.outcome == 'rejection' else opts):
            case_dir = work / (c.name + '-O' + str(opt))
            result = scalar_runner.run_case(c, opt, args, case_dir)
            if c.outcome == 'trap' and result.get('compile', {}).get('returncode') == 0:
                pre = case_dir / 'pre.ll'
                ir = pre.read_text() if pre.is_file() else ''
                if not ('slice index out of bounds' in ir and 'call void @llvm.trap' in ir):
                    result['problems'].append('missing slice bounds diagnostic and trap IR')
                result['status'] = 'PASS' if not result['problems'] else 'FAIL'
                result['trap_reason_verified'] = False
                result['trap_evidence'] = 'Exact native trap termination plus source-specific pre-optimization guard IR; no hook reason assertion.'
            print(result['status'], c.name, 'O' + str(opt), '; '.join(result['problems']))
            summary['results'].append(result)
    summary['compiler_sha256_after'] = scalar_runner.sha256(args.compiler)
    summary['compiler_unchanged'] = before == summary['compiler_sha256_after']
    summary['status'] = 'PASS' if summary['compiler_unchanged'] and all(r['status'] == 'PASS' for r in summary['results']) else 'FAIL'
    summary['counts'] = {status: sum(r['status'] == status for r in summary['results']) for status in ('PASS', 'FAIL')}
    (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(summary['status'], summary['counts'], work / 'summary.json')
    return 0 if summary['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
