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
    TokThread,  /* thread — parallel dispatch to thread pool */
    TokFuture,  /* future — handle to an async result       */
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
    TokDotDot,
    TokDotDotEq,
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
    TokCHeader,     /* cheader */
    TokStd,         /* std (stdlib source specifier) */
    TokHash,        /* hash */
    TokEqu,         /* equ */
    TokThis,        /* this — self-reference inside method bodies */
    TokWith,        /* with — scoped binding statement */
    TokAny,         /* any — inline tagged-union type */
    TokInterface,   /* interface — interface declaration */
    TokMacro,       /* macro — preprocessor macro keyword */

    /* slice builtins — keep at end to avoid shifting existing values */
    TokMake,        /* make   */
    TokAppend,      /* append */
    TokCopy,        /* copy   */
    TokLen,         /* len    */
    TokCap,         /* cap    */

    /* memory-safety keywords — added for new safety redesign */
    TokZone,        /* zone — lexical/manual memory zone                    */
    TokUnsafe,      /* unsafe — suppresses safety checks inside the block   */
    TokUnchecked,   /* unchecked — opt-out of bounds check in buf[unchecked: i] */
    /* NOTE: @frees is intentionally NOT a keyword — it is parsed as an    */
    /* identifier after '@' to avoid conflicts with struct field names.     */

    TokDotEqEq,     /* .== — infix universal equality operator              */
} token_kind_t;

typedef struct {
    token_kind_t kind;
    const char  *start;
    usize_t      length;
    usize_t      line;
    usize_t      col;   /* 1-based column of the first character of this token */
    const char  *file;  /* source file path; NULL = set by caller after lexing */
} token_t;

typedef struct {
    const char *start;       /* beginning of entire source buffer            */
    const char *current;     /* current scan position                        */
    const char *line_start;  /* pointer to first char of the current line    */
    usize_t line;
} lexer_t;

void init_lexer(lexer_t *lex, const char *source);
token_t next_token(lexer_t *lex);

#endif
