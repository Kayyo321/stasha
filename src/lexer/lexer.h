#ifndef LexerH
#define LexerH

#include "../common/common.h"

typedef enum {
    TokIntLit,
    TokStackStr,
    TokHeapStr,
    TokIdent,

    TokMod,
    TokInt,
    TokExt,
    TokFn,
    TokFor,
    TokIf,
    TokElse,
    TokWhile,
    TokRet,
    TokStack,
    TokHeap,
    TokAtomic,
    TokGpu,
    TokCpu,
    TokDebug,
    TokVoid,

    TokI8,
    TokI16,
    TokI32,
    TokI64,
    TokStr,
    TokBool,

    TokPlus,
    TokMinus,
    TokStar,
    TokSlash,
    TokPlusEq,
    TokMinusEq,
    TokPlusPlus,
    TokMinusMinus,
    TokLt,
    TokGt,
    TokLtEq,
    TokGtEq,
    TokEqEq,
    TokBangEq,
    TokEq,
    TokBang,

    TokLParen,
    TokRParen,
    TokLBrace,
    TokRBrace,
    TokSemicolon,
    TokColon,
    TokComma,
    TokDot,

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
