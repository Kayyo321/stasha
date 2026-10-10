/* Calls the production constant-operation engine without LLVM or codegen. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "../../src/ast/ast.h"
#include "../../src/codegen/scalar_integer_semantics.h"

int main(void) {
    const char *names[] = { "+", "-", "*", "+%", "-%", "*%", "+!", "-!", "*!", "/", "%", "<<", ">>", "&", "|", "^", "==", "!=", "<", ">", "<=", ">=" };
    const token_kind_t ops[] = { TokPlus, TokMinus, TokStar, TokPlusPercent, TokMinusPercent, TokStarPercent, TokPlusBang, TokMinusBang, TokStarBang, TokSlash, TokPercent, TokLtLt, TokGtGt, TokAmp, TokPipe, TokCaret, TokEqEq, TokBangEq, TokLt, TokGt, TokLtEq, TokGtEq };
    char name[8]; unsigned width, u, rw, ru; uint64_t x, y;
    while (scanf("%7s %u %u %u %u %" SCNu64 " %" SCNu64, name, &width, &u, &rw, &ru, &x, &y) == 7) {
        unsigned i; for (i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) if (!strcmp(name, names[i])) break;
        if (i == sizeof(ops) / sizeof(ops[0])) return 2;
        uint64_t value; boolean_t boolean_result; unsigned reason;
        if (sts_scalar_fold_integer(ops[i], x, y, width, u != 0, rw, ru != 0, &value, &boolean_result, &reason))
            printf("ok %" PRIu64 " %u\n", value, (unsigned)boolean_result);
        else printf("trap %u\n", reason);
    }
    return ferror(stdin) ? 3 : 0;
}
