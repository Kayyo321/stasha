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
    TokDebug,
    TokVoid,
    TokTrue,
    TokFalse,
    TokType,
    TokStruct,
    TokEnum,
    TokCinclude,
    TokNew,
    TokRem,
    TokSizeof,
    TokMatch,
    TokDefer,
    TokNil,
    TokMov,

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

    TokEof,
    TokError,
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
