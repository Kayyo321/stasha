#!/usr/bin/env python3
"""Focused FP/integer conversion complement to the integer boundary matrix.

Uses the same fail-closed runner and exact reason-5 hook. These cases do not
claim complete floating-point arithmetic or platform ABI qualification.
"""
from generate import Case, Program
from reference import BY_NAME
import run as runner


def cases():
    result = []
    for fp in ('f32', 'f64'):
        p = Program('fp_conversion_' + fp)
        p.body += [f'    volatile {fp} zero = 0.0;', f'    stack {fp} nan = zero / zero;',
                   '    stack u64 high = 18446744073709551615;', '    stack i64 negative = -1;']
        p.emit(f'({fp})high > 0.0', True, label='unsigned_to_float')
        p.emit(f'({fp})negative < 0.0', True, label='signed_to_float')
        p.emit('(bool)nan', True, label='nan_truth')
        p.emit('nan != nan', True, label='nan_inequality')
        p.emit('nan ? 1 : 2', 1, BY_NAME['i32'], 'nan_ternary_truth')
        p.emit('zero ? 1 : 2', 2, BY_NAME['i32'], 'zero_ternary_false')
        for name, value in [('i8', -128), ('u8', 128), ('i16', -32768), ('u16', 32768),
                            ('i32', -2147483648), ('u32', 2147483648),
                            ('i64', -9223372036854775808), ('u64', 9223372036854775808)]:
            p.decls.append(f'fn convert_{name}(stack {fp} x): {name} {{ ret ({name})x; }}')
            p.emit(f'convert_{name}({value}.0)', value, BY_NAME[name], 'integer_boundary')
            p.emit(f'convert_{name}(1.75)', 1, BY_NAME[name], 'truncate_toward_zero')
        result.append(p.finish(['fp_conversion', fp]))
        for label, target, expression in [('nan', 'i32', 'zero / zero'),
                ('positive_infinity', 'u64', '1.0 / zero'), ('negative_infinity', 'i64', '-1.0 / zero'),
                ('unsigned_negative', 'u64', '-0.5'), ('signed_upper', 'i64', '9223372036854775808.0'),
                ('unsigned_upper', 'u64', '18446744073709551616.0')]:
            name = f'trap_fp_{fp}_{label}'
            source = (f'@[[crash: disable]];\nmod scalar_{name};\nfn main(void): i32 {{\n'
                      f'    volatile {fp} zero = 0.0;\n'
                      f'    volatile {fp} value = ({fp})({expression});\n'
                      f'    stack {target} _result = ({target})value;\n'
                      '    print.("TRAP_WAS_NOT_TAKEN\\n");\n    ret 0;\n}\n')
            # zero must be observed even for constant-boundary inputs.
            source = source.replace('    volatile ' + fp + ' value',
                                    '    if (zero != 0.0) { ret 2; }\n    volatile ' + fp + ' value')
            result.append(Case(name, source, 'trap', reason='fp-cast-range', tags=['fp_conversion', fp, label]))
    return result


if __name__ == '__main__':
    runner.cases = cases
    runner.main()
