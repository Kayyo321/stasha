#!/usr/bin/env python3
"""Generate the P1 boundary corpus and exact output oracle; no compiler needed."""
import argparse
from dataclasses import asdict, dataclass, field
import hashlib
import json
from pathlib import Path
import re

from reference import (BY_NAME, TYPES, ScalarTrap, binary, boundaries,
                       canonical_hex, cast, common_type, unary)


@dataclass
class Case:
    name: str
    source: str
    outcome: str = "success"
    stdout: str = ""
    reason: str = ""
    diagnostic: str = ""
    tags: list = field(default_factory=list)
    observations: list = field(default_factory=list)


class Program:
    def __init__(self, name):
        self.name = name
        self.decls = []
        self.body = []
        self.output = []
        self.observations = []
        self.count = 0

    def emit(self, expression, expected, typ=None, label="value"):
        key = f"{self.count:04d}_{label}"
        self.count += 1
        if typ is None:
            self.body += [f'    if ({expression}) {{ print.("{key}=1\\n"); }}',
                          f'    else {{ print.("{key}=0\\n"); }}']
            value = str(int(expected))
        else:
            self.body.append(f'    print.("{key}={{:016x}}\\n", (u64)({expression}));')
            value = canonical_hex(expected, typ)
        self.output.append(f"{key}={value}\n")
        self.observations.append(dict(label=key, expression=expression,
                                      type=typ.name if typ else "bool", expected=expected))

    def finish(self, tags=()):
        self.body += [f'    print.("SCALAR_OK:{self.name}\\n");', '    ret 0;']
        source = (f'@[[crash: disable]];\nmod scalar_{self.name};\n\n' +
                  '\n'.join(self.decls) + '\n\nfn main(void): i32 {\n' +
                  '\n'.join(self.body) + '\n}\n')
        return Case(self.name, source, stdout=''.join(self.output) + f"SCALAR_OK:{self.name}\n",
                    tags=list(tags), observations=self.observations)


def typed(value, typ):
    return f"(({typ.name})({value}))"


def boundary_case(typ):
    p = Program("boundaries_" + typ.name)
    p.decls.append(f'fn identity(stack {typ.name} x): {typ.name} {{ ret x; }}')
    for index, value in enumerate(boundaries(typ)):
        p.decls += [f'stack {typ.name} global_{index} = {value};',
                    f'const {typ.name} constant_{index} = {value};',
                    f'fn return_{index}(void): {typ.name} {{ ret {value}; }}']
        p.body.append(f'    stack {typ.name} local_{index} = {value};')
        for expr, tag in [(f'local_{index}', "local_literal"), (f'global_{index}', "global_literal"),
                          (f'constant_{index}', "const_literal"), (f'identity({value})', "argument_literal"),
                          (f'return_{index}()', "return_literal"), (f'identity(local_{index})', "argument_return")]:
            p.emit(expr, value, typ, tag)
    # Contextual literals on either side, and in nested expressions, retain width.
    p.decls.append(f'fn return_context(void): {typ.name} {{ ret ({typ.maximum} +% 1); }}')
    p.emit('return_context()', typ.wrap(typ.maximum + 1), typ, 'return_expression_context')
    p.emit(f'identity({typ.maximum} +% 1)', typ.wrap(typ.maximum + 1), typ, 'argument_expression_context')
    p.decls.append(f'stack {typ.name} hex_global = {hex(typ.maximum)};')
    p.body.append(f'    stack {typ.name} hex_local = {hex(typ.maximum)};')
    p.emit('hex_global', typ.maximum, typ, 'global_hex_literal')
    p.emit('hex_local', typ.maximum, typ, 'local_hex_literal')
    p.body.append(f'    stack {typ.name} edge = {typ.maximum};')
    for expr in ('edge + 1', '1 + edge', 'edge + (1 + 0)'):
        p.emit(expr, typ.wrap(typ.maximum + 1), typ, 'contextual_arithmetic')
    p.body.append(f'    stack {typ.name} nested = ({typ.maximum} +% 1);')
    p.emit('nested', typ.wrap(typ.maximum + 1), typ, 'declaration_context')
    return p.finish([typ.name, 'literal', 'local', 'global', 'argument', 'return', 'context'])


def arithmetic_case(typ):
    p = Program("arithmetic_" + typ.name)
    ops = ['+', '-', '*', '+%', '-%', '*%', '+!', '-!', '*!', '/', '%', '&', '|', '^',
           '==', '!=', '<', '<=', '>', '>=']
    for index, op in enumerate(ops):
        result = 'bool' if op in ('==', '!=', '<', '<=', '>', '>=') else typ.name
        p.decls.append(f'fn op_{index}(stack {typ.name} a, stack {typ.name} b): {result} {{ ret a {op} b; }}')
        for left in boundaries(typ):
            for right in boundaries(typ):
                try:
                    expected = binary(op, left, right, typ)
                except ScalarTrap:
                    continue
                p.emit(f'op_{index}({left}, {right})', expected,
                       None if result == 'bool' else typ, 'op_' + str(index))
    for index, op in enumerate(('<<', '>>')):
        p.decls.append(f'fn shift_{index}(stack {typ.name} a, stack i64 b): {typ.name} {{ ret a {op} b; }}')
        for left in boundaries(typ):
            for count in (0, 1, typ.width - 1):
                p.emit(f'shift_{index}({left}, {count})', binary(op, left, count, typ), typ, 'shift')
    p.decls.append(f'fn negate(stack {typ.name} a): {typ.name} {{ ret -a; }}')
    p.decls.append(f'fn invert(stack {typ.name} a): {typ.name} {{ ret ~a; }}')
    for value in boundaries(typ):
        p.emit(f'negate({value})', unary('-', value, typ), typ, 'negate_wrap')
        p.emit(f'invert({value})', unary('~', value, typ), typ, 'bitwise_not')
    # Compound paths must use the same signedness and width rules.
    p.body.append(f'    stack {typ.name} compound = {typ.maximum};')
    current = typ.maximum
    for op, rhs in [('+', 1), ('-', 1), ('*', 2), ('/', 2), ('%', 3), ('>>', 1), ('<<', 1)]:
        p.body.append(f'    compound {op}= {rhs};')
        current = binary(op, current, rhs, typ)
        p.emit('compound', current, typ, 'compound')
    return p.finish([typ.name, 'arithmetic', 'unsigned', 'checked', 'wrap', 'compound'])


def casts_case():
    p = Program('casts_all_widths')
    for source in TYPES:
        for destination in TYPES:
            name = 'cast_' + source.name + '_' + destination.name
            p.decls.append(f'fn {name}(stack {source.name} x): {destination.name} {{ ret ({destination.name})x; }}')
            for value in boundaries(source):
                p.emit(f'{name}({value})', cast(value, source, destination), destination, name)
    return p.finish(['cast', 'source_signedness', 'all_widths'])


def widening_case():
    p = Program('same_signedness_widening')
    for left in TYPES:
        for right in TYPES:
            if left.signed != right.signed or left == right:
                continue
            result = common_type(left, right)
            name = 'widen_' + left.name + '_' + right.name
            p.decls.append(f'fn {name}(stack {left.name} a, stack {right.name} b): {result.name} {{ ret a + b; }}')
            a, b = left.maximum, right.minimum
            p.emit(f'{name}({a}, {b})', result.wrap(a + b), result, name)
    return p.finish(['widen', 'mixed_width'])


def default_literals_case():
    p = Program('default_literals')
    for value, name in [(0, 'i32'), (2147483647, 'i32'), (2147483648, 'i64'),
                        (4294967296, 'i64'), (9223372036854775807, 'i64'),
                        (9223372036854775808, 'u64'), (18446744073709551615, 'u64'),
                        (-2147483648, 'i32'), (-2147483649, 'i64'), (-9223372036854775808, 'i64')]:
        # Unary ~ and minus observe the selected expression width before u64 output.
        typ = BY_NAME[name]
        p.emit(f'~({value})', unary('~', value, typ), typ, 'default_' + name)
        # A bare -literal has its own signed-minimum contextual rule.
        # Add zero first to test negation of an already typed expression.
        p.emit(f'-(({value}) + 0)', unary('-', value, typ), typ, 'default_negation_' + name)
    p.body += ['    stack u64 beyond_u32 = 0x100000000;']
    p.emit('beyond_u32', 4294967296, BY_NAME['u64'], 'local_above_u32')
    p.body += ['    stack u64 high = 0xffff800000000000;',
               '    stack void *address = (void *)high;',
               '    stack u64 roundtrip = (u64)address;']
    p.emit('roundtrip', 0xffff800000000000, BY_NAME['u64'], 'high_half_address_roundtrip')
    return p.finish(['literal', 'default_type', 'high_half_address'])


def compiletime_case(typ):
    p = Program('comptime_' + typ.name)
    # Constants independently checked at compilation AND observed at runtime.
    examples = [('+', typ.maximum, 1), ('-', typ.minimum, 1), ('*', typ.maximum, 2),
                ('+%', typ.maximum, 1), ('-%', typ.minimum, 1), ('*%', typ.maximum, 2),
                ('+!', 1, 1), ('-!', 1, 1), ('*!', 1, 2), ('/', typ.maximum, 2),
                ('%', typ.maximum, 3), ('>>', typ.minimum if typ.signed else typ.maximum, 1),
                ('<<', 1, typ.width - 1)]
    for index, (op, left, right) in enumerate(examples):
        rhs = str(right) if op in ('<<', '>>') else typed(right, typ)
        expression = f'({typed(left, typ)} {op} {rhs})'
        expected = binary(op, left, right, typ)
        p.decls += [f'@comptime assert.({expression} == {typed(expected, typ)});',
                    f'const {typ.name} global_expression_{index} = {expression};']
        p.emit(expression, expected, typ, 'runtime_expression')
        p.emit(f'global_expression_{index}', expected, typ, 'global_expression')
    compare = f'{typed(typ.maximum, typ)} > {typed(1, typ)}'
    p.decls.append(f'@comptime assert.({compare});')
    p.emit(compare, True, label='compare_top_bit')
    return p.finish([typ.name, 'comptime', 'global_expression'])


def short_circuit_case():
    p = Program('short_circuit')
    p.decls += ['stack i32 calls = 0;',
                'const bool nonzero_global = 2;',
                'fn truthy_literal(void): bool { ret -2; }',
                'fn touch(void): bool { calls += 1; ret true; }',
                'fn touch_number(void): i32 { calls += 1; ret 1; }',
                'fn bad_divide(stack i32 divisor): bool { ret (12 / divisor) == 0; }',
                '@comptime assert.(!(false && ((1 / 0) == 0)));',
                '@comptime assert.(true || ((1 / 0) == 0));',
                '@comptime assert.(!(false && ((1 << 32) == 0)));',
                '@comptime assert.(true || (((i8)127 +! (i8)1) == 0));']
    p.body += ['    stack bool nonzero_local = 2;', "    stack bool nonzero_char = 'B';",
               '    volatile bool no = false;', '    volatile bool yes = true;',
               '    volatile i32 zero = 0;']
    p.emit('nonzero_global && nonzero_local && nonzero_char && truthy_literal()', True, label='nonzero_boolean_contexts')
    for expression, expected in [('no && touch()', False), ('yes || touch()', True),
                                 ('no && bad_divide(zero)', False), ('yes || bad_divide(zero)', True)]:
        p.emit(expression, expected, label='inactive')
    p.emit('calls', 0, BY_NAME['i32'], 'inactive_call_count')
    p.emit('yes && touch()', True, label='active_and')
    p.emit('no || touch()', True, label='active_or')
    p.emit('calls', 2, BY_NAME['i32'], 'active_call_count')
    p.emit('touch_number() > 10 and < (12 / zero)', False, label='chain_inactive_and')
    p.emit('touch_number() > 0 or < (12 / zero)', True, label='chain_inactive_or')
    p.emit('touch_number() > 0 or < 0 and > (12 / zero)', True, label='chain_precedence')
    p.emit('calls', 5, BY_NAME['i32'], 'chain_base_evaluated_once')
    return p.finish(['short_circuit', 'comptime', 'side_effect'])


def exceptional_cases(typ):
    examples = [('add_overflow', '+!', typ.maximum, 1),
                ('subtract_overflow', '-!', typ.minimum, 1),
                ('multiply_overflow', '*!', typ.maximum, 2),
                ('division_zero', '/', 1, 0), ('remainder_zero', '%', 1, 0)]
    if typ.signed:
        examples += [('division_min_neg1', '/', typ.minimum, -1),
                     ('remainder_min_neg1', '%', typ.minimum, -1),
                     ('add_negative_overflow', '+!', typ.minimum, -1),
                     ('multiply_negative_overflow', '*!', typ.minimum, -1)]
    for direction in ('left', 'right'):
        op = '<<' if direction == 'left' else '>>'
        for label, count in [('negative', -1), ('width', typ.width), ('huge', 256), ('huge_u64', (1 << 64) - 1)]:
            examples.append((f'shift_{direction}_{label}', op, 1, count))
    for label, op, left, right in examples:
        try:
            binary(op, left, right, typ)
        except ScalarTrap as error:
            reason = str(error)
        else:
            raise AssertionError('trap fixture has valid arithmetic')
        for compound in (False, True) if op in ('/', '%', '<<', '>>') else (False,):
            name = f'trap_{typ.name}_{label}' + ('_compound' if compound else '')
            right_type = ('u64' if right > (1 << 63) - 1 else 'i64') if op in ('<<', '>>') else typ.name
            statement = f'a {op}= b;' if compound else f'stack {typ.name} _result = a {op} b;'
            source = (f'@[[crash: disable]];\nmod scalar_{name};\nfn main(void): i32 {{\n'
                      f'    volatile {typ.name} a = {left};\n'
                      f'    volatile {right_type} b = {right};\n    {statement}\n'
                      '    print.("TRAP_WAS_NOT_TAKEN\\n");\n    ret 0;\n}\n')
            yield Case(name, source, 'trap', reason=reason, tags=[typ.name, 'trap', label])
        # A constant-evaluation rejection must be a diagnostic, never a crash.
        name = f'reject_comptime_{typ.name}_{label}'
        rhs = str(right) if op in ('<<', '>>') else typed(right, typ)
        source = (f'@[[crash: disable]];\nmod scalar_{name};\n'
                  f'@comptime assert.(({typed(left, typ)} {op} {rhs}) == 0);\n'
                  'fn main(void): i32 { ret 0; }\n')
        yield Case(name, source, 'rejection', reason=reason,
                   diagnostic=r'comptime|constant|overflow|division|remainder|shift',
                   tags=[typ.name, 'comptime', 'rejection', label])


def rejection_cases():
    for typ in TYPES:
        for suffix, value in [('above', typ.maximum + 1), ('below', typ.minimum - 1)]:
            for context in ('local', 'global', 'argument', 'return'):
                name = f'reject_literal_{typ.name}_{suffix}_{context}'
                main = 'fn main(void): i32 { ret 0; }'
                if context == 'local':
                    body = f'fn main(void): i32 {{ stack {typ.name} _x = {value}; ret 0; }}'
                elif context == 'global':
                    body = f'stack {typ.name} x = {value};\n' + main
                elif context == 'argument':
                    body = (f'fn take(stack {typ.name} _x): void {{ }}\n'
                            f'fn main(void): i32 {{ take({value}); ret 0; }}')
                else:
                    body = f'fn give(void): {typ.name} {{ ret {value}; }}\n' + main
                yield Case(name, f'@[[crash: disable]];\nmod scalar_{name};\n{body}\n',
                           'rejection', diagnostic=r'literal|range|fit|overflow|represent',
                           tags=[typ.name, 'literal', 'rejection', context])
    for left in TYPES:
        for right in TYPES:
            if left.signed == right.signed:
                continue
            name = f'reject_mixed_{left.name}_{right.name}'
            source = (f'@[[crash: disable]];\nmod scalar_{name};\n'
                      f'fn main(void): i32 {{ stack {left.name} a = 1; stack {right.name} b = 1;'
                      ' print.("{:x}\\n", (u64)(a + b)); ret 0; }\n')
            yield Case(name, source, 'rejection', diagnostic=r'sign|cast|mix', tags=['mixed_signedness', 'rejection'])


def implicit_conversion_rejections():
    # A typed expression must not silently lose width/sign, even when the
    # chosen runtime value happens to fit the destination range.
    for source in TYPES:
        for destination in TYPES:
            narrowing = source.signed == destination.signed and source.width > destination.width
            sign_loss = source.signed != destination.signed and not (not source.signed and destination.signed and destination.width > source.width)
            if not narrowing and not sign_loss:
                continue
            category = 'narrowing' if narrowing else 'signedness'
            for context in ('local', 'argument', 'return'):
                name = f'reject_implicit_{category}_{source.name}_{destination.name}_{context}'
                if context == 'local':
                    body = (f'fn main(void): i32 {{ stack {source.name} source = 1; '
                            f'stack {destination.name} _result = source; ret 0; }}')
                elif context == 'argument':
                    body = (f'fn take(stack {destination.name} _x): void {{ }}\n'
                            f'fn main(void): i32 {{ stack {source.name} source = 1; take(source); ret 0; }}')
                else:
                    body = (f'fn give(stack {source.name} source): {destination.name} {{ ret source; }}\n'
                            'fn main(void): i32 { ret 0; }')
                yield Case(name, f'@[[crash: disable]];\nmod scalar_{name};\n{body}\n',
                           'rejection', diagnostic=r'narrow|conver|implicit|cast|type|sign',
                           tags=[source.name, destination.name, 'rejection', category, context])


def safe_unsigned_widening_case():
    p = Program('implicit_safe_unsigned_widening')
    for source in TYPES:
        for destination in TYPES:
            if source.signed or not destination.signed or destination.width <= source.width:
                continue
            name = 'safe_' + source.name + '_' + destination.name
            p.decls.append(f'fn {name}(stack {source.name} x): {destination.name} {{ ret x; }}')
            p.decls.append(f'fn take_{name}(stack {destination.name} x): {destination.name} {{ ret x; }}')
            p.body.append(f'    stack {source.name} source_{name} = {source.maximum};')
            p.body.append(f'    stack {destination.name} local_{name} = source_{name};')
            for expr in (f'local_{name}', f'{name}(source_{name})', f'take_{name}(source_{name})'):
                p.emit(expr, source.maximum, destination, 'range_preserving_unsigned_widen')
    return p.finish(['coercion', 'unsigned_to_wider_signed'])


def scalar_match_case():
    p = Program('match_switch_full_width')
    p.decls += [
        'fn match_unsigned(stack u64 x): i32 { match x { 0xffffffffffffffff => { ret 1; } 0 => { ret 2; } other => { if (other > 0) { ret 3; } ret 4; } } }',
        'fn match_signed(stack i64 x): i32 { match x { -0x8000000000000000 => { ret 1; } -1 => { ret 2; } _ => { ret 3; } } }',
        'fn switch_unsigned(stack u64 x): i32 { switch (x) { case 0xffffffffffffffff: { ret 1; } case 0x100000000: { ret 2; } default: { ret 3; } } }',
        'fn switch_signed(stack i64 x): i32 { switch (x) { case -9223372036854775808: { ret 1; } case -1: { ret 2; } default: { ret 3; } } }',
    ]
    for fn, values in [('match_unsigned', [(18446744073709551615, 1), (0, 2), (4294967296, 3)]),
                       ('match_signed', [(-9223372036854775808, 1), (-1, 2), (0, 3)]),
                       ('switch_unsigned', [(18446744073709551615, 1), (4294967296, 2), (0, 3)]),
                       ('switch_signed', [(-9223372036854775808, 1), (-1, 2), (0, 3)])]:
        for value, result in values:
            p.emit(f'{fn}({value})', result, BY_NAME['i32'], 'full_width_case')
    return p.finish(['match', 'switch', 'full_width', 'hex', 'negative'])


def scalar_match_rejections():
    for name, typ, arms in [('match_range', 'u8', '256 => {}'), ('match_negative_unsigned', 'u64', '-1 => {}'),
                            ('match_duplicate', 'u64', '0 => {} 0x0 => {}')]:
        source = f'@[[crash: disable]];\nmod scalar_{name}; fn main(void): i32 {{ stack {typ} x = 0; match x {{ {arms} _ => {{}} }} ret 0; }}'
        yield Case('reject_' + name, source, 'rejection', diagnostic=r'range|duplicate|literal', tags=['match', 'rejection'])


def scalar_access_paths_case():
    p = Program('source_types_access_paths')
    p.decls += [
        'type Box: struct { ext u8 small; ext u64 large; };',
        'fn widen_byte(stack u8 x): u64 { ret x; }',
        'fn select_byte(stack bool yes, stack u8 a, stack u16 b): u16 { ret yes ? a : b; }',
        'fn @comptime[T] identity_generic(stack T x): T { ret x; }',
    ]
    p.body += ['    Box box = .{ .small = 255, .large = 18446744073709551615 };',
               '    stack u8 values[2] = .{ 255, 1 };',
               '    stack u8 *r pointer = &values[0];',
               '    stack fn*(stack u8): u64 callable = widen_byte;',
               '    stack let inferred = box.large;',
               '    stack u64 via_return = widen_byte(values[0]);',
               '    values[1] = 254;',
               '    box.small = values[1];']
    for expr, value, typ in [('box.small', 254, 'u8'), ('box.large', (1 << 64) - 1, 'u64'),
                             ('values[0]', 255, 'u8'), ('values[1]', 254, 'u8'),
                             ('*pointer', 255, 'u8'), ('callable(255)', 255, 'u64'),
                             ('inferred >> 63', 1, 'u64'), ('via_return', 255, 'u64'),
                             ('select_byte(true, 255, 65535)', 255, 'u16'),
                             ('select_byte(false, 255, 65535)', 65535, 'u16'),
                             ('identity_generic.[u64](18446744073709551615) >> 63', 1, 'u64')]:
        p.emit(expr, value, BY_NAME[typ], 'source_type_path')
    return p.finish(['member', 'array', 'deref', 'fnptr', 'let', 'ternary', 'generic'])


def global_dependency_case():
    p = Program('ordered_global_constants')
    p.decls += ['const u64 first = 18446744073709551614;', 'const u64 second = first + 1;']
    p.emit('second', (1 << 64) - 1, BY_NAME['u64'], 'ordered_reference')
    return p.finish(['global', 'constant_dependency'])


def global_dependency_rejections():
    for name, decls in [
            ('forward', 'const u64 first = second; const u64 second = 7;'),
            ('self', 'const u64 first = first;'),
            ('cycle', 'const u64 first = second; const u64 second = first;')]:
        source = f'@[[crash: disable]];\nmod scalar_global_{name};\n{decls}\nfn main(void): i32 {{ ret 0; }}'
        yield Case('reject_global_' + name, source, 'rejection', diagnostic=r'constant|initializer|unsupported', tags=['global', 'constant_dependency', 'rejection'])


def cases():
    result = []
    for typ in TYPES:
        result.extend([boundary_case(typ), arithmetic_case(typ), compiletime_case(typ)])
        result.extend(exceptional_cases(typ))
    result.extend([casts_case(), widening_case(), default_literals_case(), short_circuit_case()])
    result.extend(rejection_cases())
    result.extend(implicit_conversion_rejections())
    result.extend([safe_unsigned_widening_case(), scalar_match_case(), scalar_access_paths_case()])
    result.extend(scalar_match_rejections())
    result.append(global_dependency_case())
    result.extend(global_dependency_rejections())
    if len({case.name for case in result}) != len(result):
        raise AssertionError('duplicate case identifiers')
    return result


def write_corpus(directory, selected=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    inventory = []
    for case in selected if selected is not None else cases():
        data = asdict(case)
        data['source_sha256'] = hashlib.sha256(case.source.encode()).hexdigest()
        source_name = case.name + '.sts'
        (directory / source_name).write_text(case.source)
        data['source'] = source_name
        inventory.append(data)
    (directory / 'manifest.json').write_text(json.dumps({'schema': 1, 'cases': inventory}, indent=2) + '\n')
    return inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--filter', default='.', help='regex on case name')
    args = parser.parse_args()
    selected = [case for case in cases() if re.search(args.filter, case.name)]
    if not selected:
        parser.error('filter selected no cases')
    inventory = write_corpus(args.output, selected)
    counts = {outcome: sum(case['outcome'] == outcome for case in inventory)
              for outcome in ('success', 'trap', 'rejection')}
    print(json.dumps(dict(cases=len(inventory), outcomes=counts, manifest=str(args.output / 'manifest.json'))))


if __name__ == '__main__':
    main()
