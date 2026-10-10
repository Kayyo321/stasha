#!/usr/bin/env python3
"""Bounded pointer qualifier regressions. Rejected programs are never executed."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/scalars'))
import run as runner
from generate import Case


def cases():
    out = []
    def add(name, body, declarations='', reject=False):
        source = ('@[[crash: disable]];\nmod qualifiers_' + name + ';\n' + declarations +
                  '\nfn main(void): i32 {\n' + body + '\nret 0;\n}\n')
        out.append(Case(name, source, outcome='rejection' if reject else 'success',
                        diagnostic=r'^error(?:\[[^\]]+\])?:[^\n]*(?:permission|writable|readable)'))
    init = 'stack i32 x = 7; stack i32 *r p = &x; '
    add('explicit_cast_widen', init + 'stack i32 *rw _q = (i32 *rw)p;', reject=True)
    add('explicit_cast_write_to_read', 'stack i32 x = 7; stack i32 *w p = &x; stack i32 *r _q = (i32 *r)p;', reject=True)
    add('explicit_cast_field', 'stack i32 x = 7; stack Box b = .{&x}; stack i32 *rw _q = (i32 *rw)b.ptr;',
        'type Box: struct { ext i32 *r ptr; };', True)
    add('explicit_cast_call', 'stack i32 x = 7; stack i32 *rw _q = (i32 *rw)identity(&x);',
        'fn identity(stack i32 *r p): i32 *r { ret p; }', True)
    nested = 'stack i32 x = 7; stack i32 *rw p = &x; stack i32 *rw *rw pp = &p; '
    add('inner_covariance', nested + 'stack i32 *r *rw _q = pp;', reject=True)
    add('inner_covariance_cast', nested + 'stack i32 *r *rw _q = (i32 *r *rw)pp;', reject=True)
    add('inner_covariance_argument', nested + 'accept(pp);',
        'fn accept(stack i32 *r *rw _p): void {}', True)
    add('inner_covariance_return', '',
        'fn convert(stack i32 *rw *rw p): i32 *r *rw { ret p; }', True)
    add('inner_covariance_assign', nested + 'stack i32 *r r = &x; stack i32 *r *rw q = &r; q = pp; stack i32 _value = q[0][0];', reject=True)
    add('deep_covariance_writable_ancestor', '',
        'fn convert(stack i32 *rw *r *rw p): i32 *r *r *rw { ret p; }', True)
    add('readonly_covariance', nested + 'stack i32 *r *r q = pp; if q[0][0] != 7 { ret 1; }')
    add('readonly_covariance_cast', nested + 'stack i32 *r *r q = (i32 *r *r)pp; if q[0][0] != 7 { ret 1; }')
    add('nested_identity', nested + 'stack i32 *rw *rw q = pp; q[0][0] = 9; if x != 9 { ret 1; }')
    add('outer_readonly_inner_writable', nested + 'stack i32 *rw *r q = pp; q[0][0] = 9; if x != 9 { ret 1; }')
    add('array_arithmetic_address_compatibility', 'stack i32 a[2] = .{7, 9}; stack i32 *rw+ p = &a[0]; if p[0] != 7 || p[1] != 9 { ret 1; }')
    add('alias_nested_invariance', nested + 'stack Restricted _q = pp;',
        'type Restricted: i32 *r *rw;', True)
    add('alias_readonly_covariance', nested + 'stack Readers q = pp; if q[0][0] != 7 { ret 1; }',
        'type Readers: i32 *r *r;')
    add('bare_star_address_compatibility', 'stack i32 x = 7; stack i32 * p = &x; if p[0] != 7 { ret 1; }')
    add('explicit_narrow_read', 'stack i32 x = 7; stack i32 *rw p = &x; stack i32 *r q = (i32 *r)p; if q[0] != 7 { ret 1; }')
    add('explicit_narrow_write', 'stack i32 x = 7; stack i32 *rw p = &x; stack i32 *w q = (i32 *w)p; q[0] = 9; if x != 9 { ret 1; }')
    add('unsafe_cast_widen', init + 'unsafe { stack i32 *rw q = (i32 *rw)p; q[0] = 9; } if x != 9 { ret 1; }')
    add('unsafe_nested_change', nested + 'unsafe { stack i32 *r *rw q = pp; if q[0][0] != 7 { ret 1; } }')
    add('unsafe_does_not_leak', init + 'unsafe { stack i32 *rw _q = (i32 *rw)p; } stack i32 *rw _r = (i32 *rw)p;', reject=True)
    slice_init = 'stack i32 a[1] = .{7}; stack []i32 v = a[:]; stack Box b = .{v}; '
    box = 'type Box: struct { ext []i32 view; };'
    add('slice_addr_readonly_container', slice_init + 'stack Box *r p = &b; stack i32 *rw q = &p.view[0]; q[0] = 9; if a[0] != 9 { ret 1; }', box)
    add('slice_addr_readonly_container_unchecked', slice_init + 'stack Box *r p = &b; unsafe { stack i32 *rw q = &p.view[unchecked: 0]; q[0] = 9; } if a[0] != 9 { ret 1; }', box)
    add('slice_addr_writeonly_container', slice_init + 'stack Box *w p = &b; stack i32 *rw _q = &p.view[0];', box, True)
    add('slice_descriptor_readonly', slice_init + 'stack Box *r p = &b; stack View *rw _q = &p.view;', 'type View: []i32; ' + box, True)
    for qualifier in ('const', 'final'):
        add(qualifier + '_slice_backing', 'stack i32 a[1] = .{7}; ' + qualifier + ' []i32 v = a[:]; stack i32 *rw q = &v[0]; q[0] = 9; if a[0] != 9 { ret 1; }')
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True, type=Path)
    p.add_argument('--opt', default='0,1,2,3')
    p.add_argument('--artifacts', required=True, type=Path)
    args = p.parse_args()
    args.compiler = args.compiler.resolve(strict=True)
    opts = [int(x) for x in args.opt.split(',')]
    if not opts or any(x not in range(4) for x in opts): p.error('optimization must be 0,1,2,3')
    args.compiler_arg = []
    args.compile_timeout, args.run_timeout = 30, 5
    args.trap_hook_object = None
    args.require_trap_reasons = False
    args.trap_hook_pattern, args.trap_exit_code = None, None
    args.trap_signal = runner.default_trap_signal()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='pointer-qualifiers-', dir=args.artifacts))
    before = runner.sha256(args.compiler)
    summary = dict(compiler=str(args.compiler), compiler_sha256=before,
                   runner_sha256=runner.sha256(__file__), results=[])
    for case in cases():
        for opt in ([0] if case.outcome == 'rejection' else opts):
            result = runner.run_case(case, opt, args, work / (case.name + '-O' + str(opt)))
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
