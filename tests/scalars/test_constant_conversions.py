#!/usr/bin/env python3
"""Exact source-constant conversion and implicit pointer conversion regressions.

Uses the fail-closed P1 runner; acceptance must be identical at O0, O1, O2, O3.
No expectation is derived from LLVM constant folding or machine integer overflow.
"""
from generate import Case, Program
from reference import BY_NAME, TYPES
import run as runner


def exact_boundaries(destination):
    p = Program('constant_fits_' + destination.name)
    p.decls.append(f'fn take(stack {destination.name} x): {destination.name} {{ ret x; }}')
    index = 0
    for source in TYPES:
        low = max(source.minimum, destination.minimum)
        high = min(source.maximum, destination.maximum)
        values = {low, high, 0, 1, -1}
        values = sorted(v for v in values if source.fits(v) and destination.fits(v))
        for value in values:
            stem = f'v{index}'
            index += 1
            p.decls += [f'int const {source.name} {stem} = {value};',
                        f'const {destination.name} global_{stem} = {stem};',
                        f'fn return_{stem}(void): {destination.name} {{ ret {stem}; }}']
            p.body += [f'    stack {destination.name} local_{stem} = {stem};',
                       f'    stack {destination.name} cast_{stem} = ({source.name})({value});',
                       f'    local_{stem} = ({source.name})({value});']
            for expr, context in [(f'global_{stem}', 'global'), (f'local_{stem}', 'assignment'),
                                  (f'cast_{stem}', 'typed_cast'), (f'take({stem})', 'argument'),
                                  (f'return_{stem}()', 'return')]:
                p.emit(expr, value, destination, source.name + '_' + context)
    return p.finish(['constant_conversion', 'all_source_widths', destination.name])


def sizeof_and_expressions():
    p = Program('constant_sizeof_expressions')
    p.decls += ['type Word: u64;',
                'type Pair: struct { ext u32 a; ext u32 b; };',
                'const u32 size_global = sizeof.(Word);',
                'const i32 signed_size_global = sizeof.(Pair);',
                'int const i64 minus_one = -1;',
                'int const u64 high_unsigned = 18446744073709551615;',
                'const u32 difference_global = high_unsigned - 18446744073709551615;',
                'const u32 sign_extended_global = minus_one + 2;',
                'fn take(stack u32 n): u32 { ret n; }',
                'fn @comptime[T] measure(void): u32 { ret sizeof.(T); }',
                'fn size_return(void): u32 { ret sizeof.(Pair); }']
    p.body += ['    stack u32 size_local = sizeof.(Word);',
               '    stack i8 small_size = sizeof.(Word);',
               '    stack u32 sizes[3] = .{sizeof.(u8), sizeof.(u32), sizeof.(u64)};',
               '    stack Pair pair = .{ .a = sizeof.(u32), .b = sizeof.(u64) };',
               '    stack u32 difference_local = high_unsigned - 18446744073709551615;',
               '    stack u32 sign_extended_local = minus_one + 2;',
               '    stack u32 wrapped = ((u64)18446744073709551615) +% 1;',
               '    stack i8 selected = true ? ((i64)-128) : ((i64)127);',
               '    stack u32 shifted = ((u64)1) << 31;',
               '    size_local = sizeof.(Pair);']
    for expr, value in [('size_global', 8), ('signed_size_global', 8), ('size_local', 8),
                        ('small_size', 8), ('sizes[0]', 1), ('sizes[1]', 4), ('sizes[2]', 8),
                        ('pair.a', 4), ('pair.b', 8), ('take(sizeof.(Pair))', 8),
                        ('size_return()', 8), ('take(sizeof.(u8) + 3)', 4),
                        ('difference_global', 0), ('difference_local', 0),
                        ('sign_extended_global', 1), ('sign_extended_local', 1),
                        ('wrapped', 0), ('selected', -128), ('shifted', 2147483648),
                        ('measure.[u8]()', 1), ('measure.[u64]()', 8), ('measure.[Pair]()', 8)]:
        p.emit(expr, value, BY_NAME['i64'] if value < 0 else BY_NAME['u64'], 'exact_expression')
    return p.finish(['constant_conversion', 'sizeof', 'source_signedness', 'expressions'])


def reject(name, declarations, body, diagnostic=r'conver|constant|out of range'):
    return Case(name, f'@[[crash: disable]];\nmod scalar_{name};\n{declarations}\n'
                f'fn main(void): i32 {{\n{body}\n    ret 0;\n}}\n',
                'rejection', diagnostic=diagnostic,
                tags=['constant_conversion', 'rejection'])


def range_rejections():
    # Test destination boundaries against both full-width source signednesses.
    # Values are proven invalid with Python's exact integers before generation.
    for target in TYPES:
        for source in (BY_NAME['i64'], BY_NAME['u64']):
            candidates = {target.minimum - 1, target.maximum + 1, source.minimum, source.maximum}
            for index, value in enumerate(sorted(v for v in candidates if source.fits(v) and not target.fits(v))):
                for context in ('global', 'local'):
                    name = f'reject_exact_{source.name}_{target.name}_{index}_{context}'
                    declarations = f'int const {source.name} original = {value};'
                    conversion = f'{target.name} _converted = original;'
                    if context == 'global':
                        yield reject(name, declarations + '\nconst ' + conversion, '')
                    else:
                        yield reject(name, declarations, '    stack ' + conversion)
    # Equal low bits must not prove a signed value fits an unsigned target.
    for source in (t for t in TYPES if t.signed):
        yield reject('reject_negative_' + source.name + '_u64',
                     f'int const {source.name} negative = -1;', '    stack u64 _converted = negative;')
    for name, target, expression in [
            ('sizeof_u8', 'u8', 'sizeof.(u64) * 32'),
            ('sizeof_i8', 'i8', 'sizeof.(u64) * 16'),
            ('u64_to_i64', 'i64', '(u64)9223372036854775808'),
            ('signed_to_unsigned', 'u32', '(i64)-1'),
            ('underflow_to_unsigned', 'u32', '((i64)-128) - 1'),
            ('negative_magnitude', 'i8', '(i64)-129'),
            ('positive_magnitude', 'i8', '(u64)128'),
            ('large_shift', 'u32', '((u64)1) << 32')]:
        yield reject('reject_expression_' + name, '', f'    stack {target} _converted = {expression};')
    # Local loads and function calls are not source constant expressions,
    # including ones that an optimizer can certainly fold to a fitting value.
    for storage in ('stack', 'const', 'final', 'volatile'):
        yield reject('reject_runtime_' + storage, '',
                     f'    {storage} u64 original = 1;\n    stack u32 _converted = original;', r'implicit integer conversion')
    yield reject('reject_runtime_call', 'fn known(void): u64 { ret 1; }',
                 '    stack u32 _converted = known();', r'implicit integer conversion')
    yield reject('reject_mixed_constant_arithmetic', 'const i32 a = 1; const u32 b = 1;',
                 '    stack i32 _converted = a + b;', r'mixed signed and unsigned')
    # A link-time strong definition may replace a weak initializer with an
    # out-of-range value. No optimization level may use it as an exact proof.
    for context in ('global', 'local', 'arithmetic', 'argument', 'return'):
        declarations = '@weak const u64 original = 1;'
        body = ''
        if context == 'global':
            declarations += '\nconst u8 _converted = original;'
        elif context == 'argument':
            declarations += '\nfn take(stack u8 _value): void { }'
            body = '    take(original);'
        elif context == 'return':
            declarations += '\nfn give(void): u8 { ret original; }'
        else:
            body = '    stack u8 _converted = original' + (' + 1;' if context == 'arithmetic' else ';')
        yield reject('reject_weak_constant_' + context, declarations, body)
    for context in ('local', 'arithmetic', 'argument', 'return'):
        declarations = 'ext const u64 original = 1;'
        body = ''
        if context == 'argument':
            declarations += '\nfn take(stack u8 _value): void { }'
            body = '    take(original);'
        elif context == 'return':
            declarations += '\nfn give(void): u8 { ret original; }'
        else:
            body = '    stack u8 _converted = original' + (' + 1;' if context == 'arithmetic' else ';')
        yield reject('reject_preemptible_constant_' + context, declarations, body)


def pointer_rejections():
    sources = [('string', '', '"text"'),
               ('address', 'stack i32 value = 1;', '&value'),
               ('pointer', 'stack i32 value = 1; stack i32 *r pointer = &value;', 'pointer'),
               ('array', 'stack i32 values[2] = .{1, 2};', 'values'),
               ('function', '', 'callee')]
    for label, setup, expression in sources:
        for context in ('local', 'assignment', 'argument', 'return'):
            declarations = 'fn callee(void): i32 { ret 0; }\nfn take(stack u64 _n): void { }'
            if context == 'local':
                body = f'    {setup}\n    stack u64 _converted = {expression};'
            elif context == 'assignment':
                body = f'    {setup}\n    stack u64 target = 0; target = {expression};'
            elif context == 'argument':
                body = f'    {setup}\n    take({expression});'
            else:
                declarations += f'\nfn bad(void): u64 {{ {setup} ret {expression}; }}'
                body = ''
            yield reject(f'reject_pointer_{label}_{context}', declarations, body,
                         r'implicit pointer-to-integer conversion')
    yield reject('reject_pointer_string_bool', '', '    stack bool _converted = "text";',
                 r'implicit pointer-to-integer conversion')


def explicit_pointer_casts():
    p = Program('explicit_pointer_integer_casts')
    p.body += ['    stack i32 value = 73;', '    stack i32 *r pointer = &value;',
               '    stack u64 address = (u64)pointer;',
               '    stack i32 *r roundtrip = (i32 *r)address;',
               '    stack u64 text_address = (u64)"text";',
               '    stack i8 *r text = (i8 *r)text_address;']
    p.emit('*roundtrip', 73, BY_NAME['i32'], 'pointer_roundtrip')
    p.emit('*text', ord('t'), BY_NAME['i8'], 'string_roundtrip')
    p.emit('address != 0', True, label='nonzero_pointer')
    p.emit('text_address != 0', True, label='nonzero_string')
    return p.finish(['pointer_conversion', 'explicit_cast'])


def cases():
    result = [exact_boundaries(destination) for destination in TYPES]
    result += [sizeof_and_expressions(), explicit_pointer_casts()]
    result += list(range_rejections()) + list(pointer_rejections())
    return result


if __name__ == '__main__':
    runner.cases = cases
    runner.main()
