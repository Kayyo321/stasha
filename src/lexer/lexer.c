#include <string.h>
#include "lexer.h"

static boolean_t is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static boolean_t is_digit(char c) {
    return c >= '0' && c <= '9';
}

static boolean_t is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static boolean_t is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static boolean_t is_at_end(lexer_t *lex) {
    return *lex->current == '\0';
}

static char advance(lexer_t *lex) {
    return *lex->current++;
}

static char peek(lexer_t *lex) {
    return *lex->current;
}

static char peek_next(lexer_t *lex) {
    if (is_at_end(lex)) return '\0';
    return lex->current[1];
}

static boolean_t match(lexer_t *lex, char expected) {
    if (is_at_end(lex) || *lex->current != expected) return False;
    lex->current++;
    return True;
}

static token_t make_token(lexer_t *lex, token_kind_t kind) {
    token_t tok;
    tok.kind   = kind;
    tok.start  = lex->start;
    tok.length = (usize_t)(lex->current - lex->start);
    tok.line   = lex->line;
    tok.col    = (usize_t)(lex->start - lex->line_start) + 1;
    tok.file   = Null;  /* caller fills this in after lexing */
    return tok;
}

static token_t error_token(lexer_t *lex, const char *msg) {
    token_t tok;
    tok.kind   = TokError;
    tok.start  = msg;
    tok.length = (usize_t)strlen(msg);
    tok.line   = lex->line;
    tok.col    = (usize_t)(lex->current - lex->line_start) + 1;
    tok.file   = Null;
    return tok;
}

static void skip_whitespace(lexer_t *lex) {
    for (;;) {
        char c = peek(lex);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(lex);
                break;
            case '\n':
                lex->line++;
                advance(lex);
                lex->line_start = lex->current;
                break;
            case '/':
                if (peek_next(lex) == '/') {
                    /* line comment: skip to end of line */
                    advance(lex); advance(lex);
                    while (!is_at_end(lex) && peek(lex) != '\n')
                        advance(lex);
                    break;
                }
                if (peek_next(lex) == '*') {
                    /* block comment: supports nesting */
                    advance(lex); advance(lex);
                    int depth = 1;
                    while (!is_at_end(lex) && depth > 0) {
                        if (peek(lex) == '/' && peek_next(lex) == '*') {
                            advance(lex); advance(lex);
                            depth++;
                        } else if (peek(lex) == '*' && peek_next(lex) == '/') {
                            advance(lex); advance(lex);
                            depth--;
                        } else {
                            char ch = peek(lex);
                            advance(lex);
                            if (ch == '\n') {
                                lex->line++;
                                lex->line_start = lex->current;
                            }
                        }
                    }
                    break;
                }
                return;
            default:
                return;
        }
    }
}

static token_kind_t identifier_kind(const char *start, usize_t len) {
    #define KW(s, k) if (len == sizeof(s) - 1 && memcmp(start, s, sizeof(s) - 1) == 0) return k

    KW("mod", TokMod);
    KW("imp", TokImp);
    KW("int", TokInt);
    KW("ext", TokExt);
    KW("fn", TokFn);
    KW("for", TokFor);
    KW("if", TokIf);
    KW("else", TokElse);
    KW("while", TokWhile);
    KW("do", TokDo);
    KW("inf", TokInf);
    KW("ret", TokRet);
    KW("break", TokBreak);
    KW("continue", TokContinue);
    KW("stack", TokStack);
    KW("heap", TokHeap);
    KW("atomic", TokAtomic);
    KW("const", TokConst);
    KW("final", TokFinal);
    KW("thread", TokThread);
    KW("future", TokFuture);
    KW("print", TokPrint);
    KW("void", TokVoid);
    KW("true", TokTrue);
    KW("false", TokFalse);
    KW("type", TokType);
    KW("struct", TokStruct);
    KW("enum", TokEnum);
    KW("lib", TokLib);
    KW("from", TokFrom);
    KW("new", TokNew);
    KW("sizeof", TokSizeof);
    KW("rem", TokRem);
    KW("match", TokMatch);
    KW("defer", TokDefer);
    KW("nil", TokNil);
    KW("mov", TokMov);
    KW("error", TokErrorType);
    KW("test", TokTest);
    KW("expect", TokExpect);
    KW("expect_eq", TokExpectEq);
    KW("expect_neq", TokExpectNeq);
    KW("test_fail", TokTestFail);
    KW("switch", TokSwitch);
    KW("case", TokCase);
    KW("default", TokDefault);
    KW("union", TokUnion);
    KW("volatile", TokVolatile);
    KW("asm", TokAsm);
    KW("tls", TokTls);
    KW("restrict", TokRestrict);
    KW("comptime_assert", TokComptimeAssert);
    KW("comptime_if", TokComptimeIf);
    KW("let", TokLet);
    KW("return", TokRet);  /* alias for ret */
    KW("libimp", TokLibImp);
    KW("cheader", TokCHeader);
    KW("std", TokStd);
    KW("hash", TokHash);
    KW("equ", TokEqu);
    KW("this", TokThis);
    KW("with", TokWith);
    KW("any", TokAny);
    KW("interface", TokInterface);
    KW("macro", TokMacro);
    KW("make",      TokMake);
    KW("append",    TokAppend);
    KW("copy",      TokCopy);
    KW("len",       TokLen);
    KW("cap",       TokCap);
    KW("zone",      TokZone);
    KW("unsafe",    TokUnsafe);
    KW("unchecked", TokUnchecked);
    /* "frees" is NOT a keyword — parsed contextually as TokIdent after '@' */

    KW("i8", TokI8);
    KW("i16", TokI16);
    KW("i32", TokI32);
    KW("i64", TokI64);
    KW("u8", TokU8);
    KW("u16", TokU16);
    KW("u32", TokU32);
    KW("u64", TokU64);
    KW("f32", TokF32);
    KW("f64", TokF64);
    KW("bool", TokBool);

    #undef KW
    return TokIdent;
}

static token_t scan_identifier(lexer_t *lex) {
    while (is_alnum(peek(lex))) advance(lex);
    usize_t len = (usize_t)(lex->current - lex->start);
    return make_token(lex, identifier_kind(lex->start, len));
}

static token_t scan_number(lexer_t *lex) {
    /* hex literal: 0x... */
    if (lex->start[0] == '0' && (peek(lex) == 'x' || peek(lex) == 'X')) {
        advance(lex); /* consume 'x' */
        while (is_hex_digit(peek(lex))) advance(lex);
        return make_token(lex, TokIntLit);
    }

    while (is_digit(peek(lex))) advance(lex);

    /* check for float: digits followed by '.' and more digits */
    if (peek(lex) == '.' && is_digit(peek_next(lex))) {
        advance(lex); /* consume '.' */
        while (is_digit(peek(lex))) advance(lex);
        return make_token(lex, TokFloatLit);
    }

    return make_token(lex, TokIntLit);
}

static token_t scan_string(lexer_t *lex, char quote) {
    while (peek(lex) != quote && !is_at_end(lex)) {
        if (peek(lex) == '\\') {
            advance(lex); /* skip backslash */
            if (!is_at_end(lex)) advance(lex); /* skip escaped char */
            continue;
        }
        if (peek(lex) == '\n') {
            lex->line++;
            advance(lex);
            lex->line_start = lex->current;
            continue;
        }
        advance(lex);
    }
    if (is_at_end(lex)) return error_token(lex, "unterminated string");
    advance(lex);
    return make_token(lex, TokStackStr); /* both '' and "" produce stack strings */
}

static token_t scan_char_lit(lexer_t *lex) {
    if (peek(lex) == '\\') {
        advance(lex);
        if (is_at_end(lex)) return error_token(lex, "unterminated char literal");
        advance(lex);
    } else {
        if (is_at_end(lex)) return error_token(lex, "unterminated char literal");
        advance(lex);
    }
    if (peek(lex) != '`') return error_token(lex, "unterminated char literal");
    advance(lex);
    return make_token(lex, TokCharLit);
}

void init_lexer(lexer_t *lex, const char *source) {
    lex->start      = source;
    lex->current    = source;
    lex->line_start = source;
    lex->line       = 1;
}

token_t next_token(lexer_t *lex) {
    skip_whitespace(lex);
    lex->start = lex->current;

    if (is_at_end(lex)) return make_token(lex, TokEof);

    char c = advance(lex);

    if (is_alpha(c)) return scan_identifier(lex);
    if (is_digit(c)) return scan_number(lex);

    switch (c) {
        case '(': return make_token(lex, TokLParen);
        case ')': return make_token(lex, TokRParen);
        case '{': return make_token(lex, TokLBrace);
        case '}': return make_token(lex, TokRBrace);
        case '[': return make_token(lex, TokLBracket);
        case ']': return make_token(lex, TokRBracket);
        case ';': return make_token(lex, TokSemicolon);
        case ':': return make_token(lex, TokColon);
        case ',': return make_token(lex, TokComma);
        case '.':
            if (peek(lex) == '.' && peek_next(lex) == '.') {
                advance(lex); advance(lex);
                return make_token(lex, TokDotDotDot);
            }
            if (peek(lex) == '.') {
                advance(lex);
                if (match(lex, '=')) return make_token(lex, TokDotDotEq);
                return make_token(lex, TokDotDot);
            }
            if (peek(lex) == '=' && peek_next(lex) == '=') {
                advance(lex); advance(lex);
                return make_token(lex, TokDotEqEq);
            }
            return make_token(lex, TokDot);
        case '@': return make_token(lex, TokAt);
        case '?': return make_token(lex, TokQuestion);
        case '~': return make_token(lex, TokTilde);

        case '+':
            if (match(lex, '+')) return make_token(lex, TokPlusPlus);
            if (match(lex, '=')) return make_token(lex, TokPlusEq);
            if (match(lex, '%')) return make_token(lex, TokPlusPercent);
            if (match(lex, '!')) return make_token(lex, TokPlusBang);
            return make_token(lex, TokPlus);
        case '-':
            if (match(lex, '-')) return make_token(lex, TokMinusMinus);
            if (match(lex, '=')) return make_token(lex, TokMinusEq);
            if (match(lex, '%')) return make_token(lex, TokMinusPercent);
            if (match(lex, '!')) return make_token(lex, TokMinusBang);
            return make_token(lex, TokMinus);
        case '*':
            if (match(lex, '=')) return make_token(lex, TokStarEq);
            if (match(lex, '%')) return make_token(lex, TokStarPercent);
            if (match(lex, '!')) return make_token(lex, TokStarBang);
            return make_token(lex, TokStar);
        case '/':
            if (match(lex, '=')) return make_token(lex, TokSlashEq);
            return make_token(lex, TokSlash);
        case '%':
            if (match(lex, '=')) return make_token(lex, TokPercentEq);
            return make_token(lex, TokPercent);

        case '&':
            if (match(lex, '&')) return make_token(lex, TokAmpAmp);
            if (match(lex, '=')) return make_token(lex, TokAmpEq);
            return make_token(lex, TokAmp);
        case '|':
            if (match(lex, '|')) return make_token(lex, TokPipePipe);
            if (match(lex, '=')) return make_token(lex, TokPipeEq);
            return make_token(lex, TokPipe);
        case '^':
            if (match(lex, '=')) return make_token(lex, TokCaretEq);
            return make_token(lex, TokCaret);

        case '=':
            if (match(lex, '>')) return make_token(lex, TokFatArrow);
            if (match(lex, '=')) return make_token(lex, TokEqEq);
            return make_token(lex, TokEq);
        case '!':
            if (match(lex, '=')) return make_token(lex, TokBangEq);
            return make_token(lex, TokBang);

        case '<':
            if (match(lex, '<')) {
                if (match(lex, '=')) return make_token(lex, TokLtLtEq);
                return make_token(lex, TokLtLt);
            }
            if (match(lex, '=')) return make_token(lex, TokLtEq);
            return make_token(lex, TokLt);
        case '>':
            if (match(lex, '>')) {
                if (match(lex, '=')) return make_token(lex, TokGtGtEq);
                return make_token(lex, TokGtGt);
            }
            if (match(lex, '=')) return make_token(lex, TokGtEq);
            return make_token(lex, TokGt);

        case '\'': return scan_string(lex, '\'');
        case '"':  return scan_string(lex, '"');
        case '`':  return scan_char_lit(lex);
    }

    return error_token(lex, "unexpected character");
}
