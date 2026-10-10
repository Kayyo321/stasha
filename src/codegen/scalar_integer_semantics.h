#ifndef STASHA_SCALAR_INTEGER_SEMANTICS_H
#define STASHA_SCALAR_INTEGER_SEMANTICS_H
/* Pure finite-width integer constant engine. Requires token_kind_t/boolean_t
 * from the AST headers. Shared directly with the sanitizer differential test;
 * no LLVM state, target-host long, signed overflow, or unchecked shift. */
#include <stdint.h>
static boolean_t sts_scalar_fold_integer(token_kind_t op, uint64_t x, uint64_t y,
        unsigned w, boolean_t u, unsigned right_width, boolean_t right_unsigned,
        uint64_t *value, boolean_t *boolean_result, unsigned *reason) {
    *reason = 0; *boolean_result = False;
    if (!w || w > 64 || !right_width || right_width > 64) return False;
    uint64_t mask = w == 64 ? UINT64_MAX : (UINT64_C(1) << w) - 1;
    uint64_t sign = UINT64_C(1) << (w - 1), result = 0;
    boolean_t checked = False, overflow = False;
    x &= mask;
    if (op != TokLtLt && op != TokGtGt) y &= mask;
    uint64_t ax = !u && (x & sign) ? (UINT64_C(0) - x) & mask : x;
    uint64_t ay = !u && (y & sign) ? (UINT64_C(0) - y) & mask : y;
        switch (op) {
        case TokPlusBang: checked = True; /* fall through */
        case TokPlus: case TokPlusPercent:
            result = (x + y) & mask;
            overflow = u ? y > mask - x : (!( (x ^ y) & sign) && ((x ^ result) & sign)); break;
        case TokMinusBang: checked = True; /* fall through */
        case TokMinus: case TokMinusPercent:
            result = (x - y) & mask;
            overflow = u ? x < y : (((x ^ y) & sign) && ((x ^ result) & sign)); break;
        case TokStarBang: checked = True; /* fall through */
        case TokStar: case TokStarPercent: {
            result = (x * y) & mask;
            uint64_t limit = u ? mask : ((x ^ y) & sign) ? sign : sign - 1;
            overflow = ay && ax > limit / ay; break;
        }
        case TokSlash: case TokPercent:
            if (!y) { *reason = 2; return False; }
            if (!u && x == sign && y == mask) { *reason = 3; return False; }
            result = op == TokSlash ? ax / ay : ax % ay;
            if (!u && ((op == TokSlash ? x ^ y : x) & sign)) result = (UINT64_C(0) - result) & mask;
            break;
        case TokLtLt: case TokGtGt:
            if ((!right_unsigned && (y & (UINT64_C(1) << (right_width - 1)))) || y >= w) { *reason = 4; return False; }
            if (op == TokLtLt) result = (x << (unsigned)y) & mask;
            else {
                result = x >> (unsigned)y;
                if (!u && (x & sign) && y) result |= mask ^ (mask >> (unsigned)y);
            }
            break;
        case TokAmp: result = x & y; break; case TokPipe: result = x | y; break; case TokCaret: result = x ^ y; break;
        case TokEqEq: result = x == y; *boolean_result = True; break;
        case TokBangEq: result = x != y; *boolean_result = True; break;
        case TokLt: case TokGt: case TokLtEq: case TokGtEq:
            if (!u) { x ^= sign; y ^= sign; }
            result = op == TokLt ? x < y : op == TokGt ? x > y : op == TokLtEq ? x <= y : x >= y;
            *boolean_result = True; break;
        default: return False;
        }
    if (checked && overflow) { *reason = 1; return False; }
    *value = result; return True;
}
#endif
