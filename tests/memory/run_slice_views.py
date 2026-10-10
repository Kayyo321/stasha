#!/usr/bin/env python3
"""Focused hosted slice-view construction contracts.

Known arrays and slices use half-open ranges satisfying 0 <= lo <= hi <= cap.
Reslicing may expand beyond the old len up to cap. Bounds retain source width
and signedness until checked; unsafe suppresses the guard. Raw-pointer slices
retain their legacy, unqualified capacity behavior and are tested only against
live backing storage. No out-of-storage pointer is dereferenced by this corpus.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import tempfile

import run_access_contracts as access


def cases():
    result = []
    for typ in ('i16', 'i32', 'i64', 'f64'):
        for path in ('member', 'call', 'index'):
            decl = ('type View: []' + typ + ';\n'
                    'type Box: struct { ext []' + typ + ' view; };\n'
                    'fn get(stack Box *r box, stack i32 *rw calls): View { '
                    'calls[0] += 1; ret box.view; }')
            setup = ('stack ' + typ + ' a[3] = .{4660, 9000, 11000};\n'
                     'stack []' + typ + ' view = a[:];\n'
                     'stack Box _box = .{.view = view};\n'
                     'stack [][]' + typ + ' _views = make.{view};\n'
                     'stack i32 calls = 0; stack i32 select = 0;\n'
                     'stack i32 lo = 1; stack i32 hi = 2; stack i32 index = 0;\n')
            source = {'member': '_box.view', 'call': 'get(&_box, &calls)',
                      'index': '_views[select++]'}[path]
            element = source + '[lo++:hi++][index++]'
            for operation in ('read', 'store', 'compound'):
                statement = {'read': 'if (' + element + ' != 9000) { ret 1; }',
                             'store': element + ' = 10000;',
                             'compound': element + ' += 1000;'}[operation]
                expected = '9000' if operation == 'read' else '10000'
                body = (setup + statement + '\n'
                        'if (a[0] != 4660 || a[1] != ' + expected + ' || a[2] != 11000) { ret 2; }\n'
                        'if (lo != 2 || hi != 3 || index != 1 || calls != ' +
                        ('1' if path == 'call' else '0') + ' || select != ' +
                        ('1' if path == 'index' else '0') + ') { ret 3; }\nret 0;')
                name = 'reslice_' + path + '_' + typ + '_' + operation
                result.append(access.Case(name, access.program(name, body, decl)))

    # Array-valued fields have array storage even though their source type is
    # the element type. Check the borrowed view and its original backing values.
    for typ in ('i16', 'i32', 'i64', 'f64'):
        for operation in ('read', 'store', 'compound'):
            name = 'array_member_' + typ + '_' + operation
            element = 'box.values[lo++:hi++][index++]'
            statement = {'read': 'if (' + element + ' != 9000) { ret 1; }',
                         'store': element + ' = 10000;',
                         'compound': element + ' += 1000;'}[operation]
            expected = '9000' if operation == 'read' else '10000'
            body = ('stack Box box = .{.values = .{4660, 9000, 11000}};\n'
                    'stack i32 lo = 1; stack i32 hi = 2; stack i32 index = 0;\n' +
                    statement + '\nif (box.values[0] != 4660 || box.values[1] != ' +
                    expected + ' || box.values[2] != 11000) { ret 2; }\n'
                    'if (lo != 2 || hi != 3 || index != 1) { ret 3; }\n'
                    'stack []' + typ + ' tail = box.values[1:];\n'
                    'if (len.(tail) != 2 || cap.(tail) != 2 || tail[1] != 11000) { ret 4; } ret 0;')
            decl = 'type Box: struct { ext ' + typ + ' values[3]; };'
            result.append(access.Case(name, access.program(name, body, decl)))

    for name, body in (
        ('expand_to_capacity',
         'stack i32 a[4] = .{1, 2, 3, 4}; stack []i32 short = a[0:1];\n'
         'stack []i32 expanded = short[1:4];\n'
         'if (len.(expanded) != 3 || cap.(expanded) != 3 || expanded[2] != 4) { ret 1; }\nret 0;'),
        ('empty_at_capacity',
         'stack i32 a[2] = .{1, 2}; stack []i32 view = a[0:1];\n'
         'stack []i32 empty = view[2:2];\n'
         'if (len.(empty) != 0 || cap.(empty) != 0) { ret 1; } ret 0;'),
        ('empty_nil',
         'stack []i32 view = nil; stack []i32 empty = view[:];\n'
         'if (len.(empty) != 0 || cap.(empty) != 0) { ret 1; } ret 0;'),
        ('default_bounds_nested',
         'stack i32 a[4] = .{1, 2, 3, 4}; stack []i32 view = a[:];\n'
         'stack []i32 tail = view[1:]; stack []i32 head = tail[:2];\n'
         'if (len.(tail) != 3 || cap.(tail) != 3 || len.(head) != 2 || cap.(head) != 3) { ret 1; }\n'
         'if (view[1:3][1:2][0] != 3) { ret 2; } ret 0;'),
        ('unsigned_narrow_bounds',
         'stack i32 a[256] = .{}; a[128] = 9000; stack []i32 view = a[:];\n'
         'stack u8 lo = 128; stack u8 hi = 129;\n'
         'stack []i32 sub = view[lo:hi];\n'
         'if (sub[0] != 9000 || len.(sub) != 1 || cap.(sub) != 128) { ret 1; } ret 0;'),
        ('struct_element_width',
         'stack Entry first = .{11, 4660}; stack Entry second = .{22, 9000};\n'
         'stack Entry a[2] = .{first, second};\n'
         'stack []Entry view = a[:]; stack Box box = .{.view = view};\n'
         'box.view[1:2][0].value += 1000;\n'
         'if (a[0].pad != 11 || a[0].value != 4660 || a[1].pad != 22 || a[1].value != 10000) { ret 1; } ret 0;'),
        ('raw_pointer_typed_width',
         'stack i32 a[3] = .{4660, 9000, 11000}; stack i32 *rw ptr = &a[0];\n'
         'stack []i32 view = ptr[1:2];\n'
         'if (view[0] != 9000) { ret 1; } view[0] += 1000;\n'
         'if (a[0] != 4660 || a[1] != 10000 || a[2] != 11000) { ret 2; } ret 0;'),
        ('unsafe_range_suppressed',
         'stack i32 a[2] = .{1, 2}; stack []i32 view = a[:];\n'
         'unsafe { stack []i32 reversed = view[1:0];\n'
         'if (cap.(reversed) != 1) { ret 1; } } ret 0;'),
    ):
        decl = ('type Entry: struct { ext i64 pad; ext i32 value; };\n'
                'type Box: struct { ext []Entry view; };' if name == 'struct_element_width' else '')
        result.append(access.Case(name, access.program(name, body, decl)))

    for name, typ, lo, hi, source in (
        ('negative_lo_i8', 'i8', '-1', '1', 'view'),
        ('negative_lo_i32', 'i32', '-1', '1', 'view'),
        ('negative_lo_i64', 'i64', '-1', '1', 'view'),
        ('negative_hi_i64', 'i64', '0', '-1', 'view'),
        ('negative_both', 'i32', '-2', '-1', 'view'),
        ('reversed_signed', 'i32', '2', '1', 'view'),
        ('reversed_unsigned', 'u64', '2', '1', 'view'),
        ('past_capacity_signed', 'i64', '0', '4', 'view'),
        ('past_capacity_unsigned', 'u8', '0', '255', 'view'),
        ('wide_lo_before_narrowing', 'u64', '4294967296', '4294967297', 'view'),
        ('wide_hi_before_narrowing', 'u64', '0', '4294967297', 'view'),
        ('max_unsigned_bound', 'u64', '0', '18446744073709551615', 'view'),
        ('array_past_capacity', 'u32', '0', '4', 'a'),
        ('array_negative_lower', 'i32', '-1', '1', 'a'),
    ):
        body = ('stack i32 a[3] = .{1, 2, 3}; stack []i32 ' +
                ('_view' if source == 'a' else 'view') + ' = a[0:1];\n'
                'stack ' + typ + ' lo = ' + lo + '; stack ' + typ + ' hi = ' + hi + ';\n'
                'stack []i32 _invalid = ' + source + '[lo:hi]; ret 0;')
        result.append(access.Case(name, access.program(name, body), outcome='trap',
                                  reason='slice range', observations=[
                                      'The invalid view is never dereferenced; a construction guard must trap.']))

    for path, expression in (('member', '_box.view'), ('call', 'get(&_box)'),
                             ('index', '_views[0]')):
        name = 'range_' + path + '_past_capacity'
        decl = ('type View: []i32;\n'
                'type Box: struct { ext []i32 view; };\n'
                'fn get(stack Box *r box): View { ret box.view; }')
        body = ('stack i32 a[3] = .{1, 2, 3}; stack []i32 view = a[0:1];\n'
                'stack Box _box = .{.view = view}; stack [][]i32 _views = make.{view};\n'
                'stack []i32 _invalid = ' + expression + '[0:4]; ret 0;')
        result.append(access.Case(name, access.program(name, body, decl),
                                  outcome='trap', reason='slice range'))

    for bound in ('lower', 'upper'):
        name = 'noninteger_' + bound
        expr = 'value:1' if bound == 'lower' else '0:value'
        body = ('stack i32 a[2] = .{1, 2}; stack f64 value = 0.5;\n'
                'stack []i32 _invalid = a[' + expr + ']; ret 0;')
        result.append(access.Case(name, access.program(name, body), outcome='rejection',
                                  diagnostic=r'^error(?:\[[^\]]+\])?:[^\n]*integer'))
    name = 'non_slice_aggregate'
    result.append(access.Case(name, access.program(name,
        'stack Pair pair = .{1, 2}; stack []i32 _invalid = pair[0:1]; ret 0;',
        'type Pair: struct { ext i32 first; ext i32 second; };'), outcome='rejection',
        diagnostic=r'^error(?:\[[^\]]+\])?:[^\n]*resolved slice element type'))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--opt', default='0,2')
    parser.add_argument('--case', action='append', default=[])
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args()
    selected = [case for case in cases() if not args.case or case.name in args.case]
    if not selected or set(args.case) - {case.name for case in selected}:
        parser.error('empty or unknown case selection')
    if args.list:
        print(json.dumps([{'name': case.name, 'outcome': case.outcome} for case in selected], indent=2))
        return 0
    opts = [int(opt) for opt in args.opt.split(',')]
    if not opts or any(opt not in range(4) for opt in opts):
        parser.error('optimization levels must be in 0,1,2,3')
    args.compiler = args.compiler.resolve(strict=True)
    args.compiler_arg = []
    args.compile_timeout, args.run_timeout = 30, 5
    args.trap_hook_object = None
    args.require_trap_reasons = False
    args.trap_hook_pattern, args.trap_exit_code = None, None
    args.trap_signal = access.scalar_runner.default_trap_signal()
    if not args.trap_signal:
        parser.error('no qualified native trap signal on this host')
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='slice-views-', dir=args.artifacts))
    before = access.scalar_runner.sha256(args.compiler)
    summary = {'compiler': str(args.compiler), 'compiler_sha256': before,
               'utc': datetime.now(timezone.utc).isoformat(), 'host': platform.platform(),
               'runner_sha256': access.scalar_runner.sha256(__file__),
               'source': access.provenance(), 'results': []}
    for case in selected:
        for opt in ([0] if case.outcome == 'rejection' else opts):
            case_dir = work / (case.name + '-O' + str(opt))
            result = access.scalar_runner.run_case(case, opt, args, case_dir)
            if case.outcome == 'trap' and result.get('compile', {}).get('returncode') == 0:
                pre = case_dir / 'pre.ll'
                ir = pre.read_text() if pre.is_file() else ''
                if 'slice range out of bounds' not in ir or 'call void @llvm.trap' not in ir:
                    result['problems'].append('missing slice construction guard diagnostic and trap IR')
                result['status'] = 'PASS' if not result['problems'] else 'FAIL'
                result['trap_reason_verified'] = False
                result['trap_evidence'] = 'Exact native trap signal plus range-specific pre-optimization guard IR.'
            print(result['status'], case.name, 'O' + str(opt), '; '.join(result['problems']))
            summary['results'].append(result)
    summary['compiler_sha256_after'] = access.scalar_runner.sha256(args.compiler)
    summary['compiler_unchanged'] = before == summary['compiler_sha256_after']
    summary['status'] = 'PASS' if summary['compiler_unchanged'] and all(
        item['status'] == 'PASS' for item in summary['results']) else 'FAIL'
    summary['counts'] = {status: sum(item['status'] == status for item in summary['results'])
                         for status in ('PASS', 'FAIL')}
    (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(summary['status'], summary['counts'], work / 'summary.json')
    return 0 if summary['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
