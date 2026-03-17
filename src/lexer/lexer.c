#include <string.h>
#include "lexer.h"

static boolean_t is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static boolean_t is_digit(char c) {
    return c >= '0' && c <= '9';
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
    tok.kind = kind;
    tok.start = lex->start;
    tok.length = (usize_t)(lex->current - lex->start);
    tok.line = lex->line;
    return tok;
}

static token_t error_token(lexer_t *lex, const char *msg) {
    token_t tok;
    tok.kind = TokError;
    tok.start = msg;
    tok.length = (usize_t)strlen(msg);
    tok.line = lex->line;
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
                break;
            case '(':
                if (peek_next(lex) == '*') {
                    advance(lex);
                    advance(lex);
                    int depth = 1;
                    while (!is_at_end(lex) && depth > 0) {
                        if (peek(lex) == '(' && peek_next(lex) == '*') {
                            advance(lex); advance(lex);
                            depth++;
                        } else if (peek(lex) == ')') {
                            advance(lex);
                            depth--;
                        } else {
                            if (peek(lex) == '\n') lex->line++;
                            advance(lex);
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
    KW("int", TokInt);
    KW("ext", TokExt);
    KW("fn", TokFn);
    KW("for", TokFor);
    KW("if", TokIf);
    KW("else", TokElse);
    KW("while", TokWhile);
    KW("ret", TokRet);
    KW("stack", TokStack);
    KW("heap", TokHeap);
    KW("atomic", TokAtomic);
    KW("gpu", TokGpu);
    KW("cpu", TokCpu);
    KW("debug", TokDebug);
    KW("void", TokVoid);
    KW("i8", TokI8);
    KW("i16", TokI16);
    KW("i32", TokI32);
    KW("i64", TokI64);
    KW("str", TokStr);
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
    while (is_digit(peek(lex))) advance(lex);
    return make_token(lex, TokIntLit);
}

static token_t scan_string(lexer_t *lex, char quote) {
    while (peek(lex) != quote && !is_at_end(lex)) {
        if (peek(lex) == '\n') lex->line++;
        advance(lex);
    }
    if (is_at_end(lex)) return error_token(lex, "unterminated string");
    advance(lex);
    return make_token(lex, quote == '\'' ? TokStackStr : TokHeapStr);
}

void init_lexer(lexer_t *lex, const char *source) {
    lex->start = source;
    lex->current = source;
    lex->line = 1;
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
        case ';': return make_token(lex, TokSemicolon);
        case ':': return make_token(lex, TokColon);
        case ',': return make_token(lex, TokComma);
        case '.': return make_token(lex, TokDot);
        case '+':
            if (match(lex, '+')) return make_token(lex, TokPlusPlus);
            if (match(lex, '=')) return make_token(lex, TokPlusEq);
            return make_token(lex, TokPlus);
        case '-':
            if (match(lex, '-')) return make_token(lex, TokMinusMinus);
            if (match(lex, '=')) return make_token(lex, TokMinusEq);
            return make_token(lex, TokMinus);
        case '*': return make_token(lex, TokStar);
        case '/': return make_token(lex, TokSlash);
        case '=':
            if (match(lex, '=')) return make_token(lex, TokEqEq);
            return make_token(lex, TokEq);
        case '!':
            if (match(lex, '=')) return make_token(lex, TokBangEq);
            return make_token(lex, TokBang);
        case '<':
            if (match(lex, '=')) return make_token(lex, TokLtEq);
            return make_token(lex, TokLt);
        case '>':
            if (match(lex, '=')) return make_token(lex, TokGtEq);
            return make_token(lex, TokGt);
        case '\'': return scan_string(lex, '\'');
        case '"': return scan_string(lex, '"');
    }

    return error_token(lex, "unexpected character");
}
