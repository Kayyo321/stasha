#ifndef LexerH
#define LexerH

#include "../common/common.h"

typedef enum {
    /* literals */
    TokIntLit,
    TokFloatLit,
    TokStackStr,
    TokHeapStr,
    TokCharLit,
    TokIdent,

    /* keywords */
    TokMod,
    TokImp,
    TokInt,
    TokExt,
    TokFn,
    TokFor,
    TokIf,
    TokElse,
    TokWhile,
    TokDo,
    TokInf,
    TokRet,
    TokBreak,
    TokContinue,
    TokStack,
    TokHeap,
    TokAtomic,
    TokConst,
    TokFinal,
    TokGpu,
    TokCpu,
    TokPrint,
    TokVoid,
    TokTrue,
    TokFalse,
    TokType,
    TokStruct,
    TokEnum,
    TokLib,
    TokFrom,
    TokNew,
    TokRem,
    TokSizeof,
    TokMatch,
    TokDefer,
    TokNil,
    TokMov,
    TokErrorType,   /* error (the type keyword) */
    TokTest,        /* test 'name' { ... } */
    TokExpect,      /* expect.(expr) */
    TokExpectEq,    /* expect_eq.(a, b) */
    TokExpectNeq,   /* expect_neq.(a, b) */
    TokTestFail,    /* test_fail.('msg') */

    /* new keywords */
    TokSwitch,      /* switch */
    TokCase,        /* case */
    TokDefault,     /* default */
    TokUnion,       /* union */
    TokVolatile,    /* volatile */
    TokAsm,         /* asm */
    TokTls,         /* tls */
    TokRestrict,    /* restrict */
    TokComptimeAssert, /* comptime_assert */
    TokComptimeIf,  /* comptime_if */
    TokLet,         /* let (type-inferred multi-assign) */

    /* type keywords */
    TokI8,
    TokI16,
    TokI32,
    TokI64,
    TokU8,
    TokU16,
    TokU32,
    TokU64,
    TokF32,
    TokF64,
    TokBool,

    /* arithmetic */
    TokPlus,
    TokMinus,
    TokStar,
    TokSlash,
    TokPercent,

    /* compound assignment */
    TokPlusEq,
    TokMinusEq,
    TokStarEq,
    TokSlashEq,
    TokPercentEq,
    TokAmpEq,
    TokPipeEq,
    TokCaretEq,
    TokLtLtEq,
    TokGtGtEq,

    /* increment/decrement */
    TokPlusPlus,
    TokMinusMinus,

    /* comparison */
    TokLt,
    TokGt,
    TokLtEq,
    TokGtEq,
    TokEqEq,
    TokBangEq,

    /* logical */
    TokAmpAmp,
    TokPipePipe,
    TokBang,

    /* bitwise */
    TokAmp,
    TokPipe,
    TokCaret,
    TokTilde,
    TokLtLt,
    TokGtGt,

    /* assignment */
    TokEq,

    /* delimiters */
    TokLParen,
    TokRParen,
    TokLBrace,
    TokRBrace,
    TokLBracket,
    TokRBracket,
    TokSemicolon,
    TokColon,
    TokComma,
    TokDot,
    TokQuestion,

    TokFatArrow,    /* => */
    TokDotDotDot,   /* ... (variadic) */
    TokAt,          /* @ (attributes) */

    /* wrapping arithmetic: +% -% *% */
    TokPlusPercent,
    TokMinusPercent,
    TokStarPercent,

    /* trapping arithmetic: +! -! *! */
    TokPlusBang,
    TokMinusBang,
    TokStarBang,

    TokEof,
    TokError,

    /* added after initial release — keep at end to avoid shifting existing values */
    TokLibImp,      /* libimp */
    TokStd,         /* std (stdlib source specifier) */
} token_kind_t;

typedef struct {
    token_kind_t kind;
    const char *start;
    usize_t length;
    usize_t line;
} token_t;

typedef struct {
    const char *start;
    const char *current;
    usize_t line;
} lexer_t;

void init_lexer(lexer_t *lex, const char *source);
token_t next_token(lexer_t *lex);

#endif
