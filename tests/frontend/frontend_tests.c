/* Standalone lexer/parser regressions. No LLVM headers or libraries required. */
#define CommonAllowStdlibAllocators
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser/parser.h"

static unsigned checks;
static const char *case_name = "setup";
static const char *test_file = "frontend-test.sts";
static pp_stream_t *stream;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL [%s] at %s:%d: %s\n", \
                case_name, __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static node_t *parse_case(const char *source, boolean_t preprocessed) {
    restore_diagnostic_counts(0, 0);
    diag_clear_captured();
    diag_set_render_enabled(False);
    diag_register_source(test_file, source);
    diag_set_file(test_file);
    diag_set_source(source);
    stream = preprocessed ? pp_process(source, test_file, NULL, 0) : NULL;
    CHECK(!preprocessed || stream);
    return preprocessed ? parse_from_stream(stream) : parse(source);
}

static void finish_case(void) {
    ast_free_all();
    if (stream) pp_stream_free(stream);
    stream = NULL;
}

static void expect_rejected(const char *name, const char *source,
                            const char *diagnostic) {
    case_name = name;
    for (int mode = 0; mode < 2; mode++) {
        CHECK(parse_case(source, (boolean_t)mode) == NULL);
        CHECK(get_error_count() > 0);
        boolean_t found = False;
        for (usize_t i = 0; i < diag_get_captured_count(); i++) {
            const captured_diag_t *d = diag_get_captured(i);
            if (strstr(d->diag.message, diagnostic)) {
                found = True;
                CHECK(d->diag.label_count > 0);
                CHECK(d->diag.labels[0].loc.line > 0);
                CHECK(d->diag.labels[0].loc.col > 0);
            }
        }
        CHECK(found);
        finish_case();
    }
}

static void expect_accepted(const char *name, const char *source) {
    case_name = name;
    for (int mode = 0; mode < 2; mode++) {
        CHECK(parse_case(source, (boolean_t)mode) != NULL);
        CHECK(get_error_count() == 0);
        finish_case();
    }
}

static void expect_literal(const char *literal, uint64_t magnitude,
                           boolean_t negative) {
    char source[256];
    snprintf(source, sizeof(source), "mod front; stack u64 value = %s;", literal);
    case_name = literal;
    for (int mode = 0; mode < 2; mode++) {
        node_t *module = parse_case(source, (boolean_t)mode);
        CHECK(module && get_error_count() == 0);
        CHECK(module->as.module.decls.count == 1);
        node_t *decl = module->as.module.decls.items[0];
        CHECK(decl->kind == NodeVarDecl);
        node_t *value = decl->as.var_decl.init;
        if (negative) {
            CHECK(value && value->kind == NodeUnaryPrefixExpr);
            CHECK(value->as.unary.op == TokMinus);
            value = value->as.unary.operand;
        }
        CHECK(value && value->kind == NodeIntLitExpr);
        CHECK(value->as.int_lit.value == magnitude);
        CHECK(value->col > 0 && value->len > 0);
        finish_case();
    }
}

static void test_match_literal(const char *text, uint64_t magnitude, boolean_t negative) {
    char source[320];
    snprintf(source, sizeof(source), "mod front; fn main(void): i32 { match 0 { %s => { ret 1; } _ => { ret 0; } } }", text);
    case_name = text;
    for (int mode = 0; mode < 2; ++mode) {
        node_t *module = parse_case(source, (boolean_t)mode);
        CHECK(module && !get_error_count());
        node_t *fn = module->as.module.decls.items[0];
        CHECK(fn->kind == NodeFnDecl);
        node_t *match = fn->as.fn_decl.body->as.block.stmts.items[0];
        CHECK(match->kind == NodeMatchStmt);
        node_t *value = match->as.match_stmt.arms.items[0]->as.match_arm.literal_expr;
        if (negative) {
            CHECK(value && value->kind == NodeUnaryPrefixExpr && value->as.unary.op == TokMinus);
            value = value->as.unary.operand;
        }
        CHECK(value && value->kind == NodeIntLitExpr);
        CHECK(value->as.int_lit.value == magnitude);
        finish_case();
    }
}

static void test_literals(void) {
    test_match_literal("18446744073709551615", UINT64_MAX, False);
    test_match_literal("0xffffffffffffffff", UINT64_MAX, False);
    test_match_literal("-9223372036854775808", UINT64_C(9223372036854775808), True);
    test_match_literal("-0x8000000000000000", UINT64_C(9223372036854775808), True);
    expect_rejected("match magnitude overflow", "mod t; fn f(void): void { match 0 { 18446744073709551616 => {} } }", "exceeds maximum supported magnitude");

    const uint64_t boundaries[] = {
        0, 1, 127, 128, 255, 256, 32767, 32768, 65535, 65536,
        UINT64_C(2147483647), UINT64_C(2147483648), UINT64_C(4294967295),
        UINT64_C(4294967296), UINT64_C(9223372036854775807),
        UINT64_C(9223372036854775808), UINT64_MAX
    };
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        char literal[64];
        snprintf(literal, sizeof(literal), "%.1" PRIu64, boundaries[i]);
        expect_literal(literal, boundaries[i], False);
        snprintf(literal, sizeof(literal), "0x%" PRIx64, boundaries[i]);
        expect_literal(literal, boundaries[i], False);
        snprintf(literal, sizeof(literal), "0X%" PRIX64, boundaries[i]);
        expect_literal(literal, boundaries[i], False);
    }
    expect_literal("00018446744073709551615", UINT64_MAX, False);
    expect_literal("-9223372036854775808", UINT64_C(9223372036854775808), True);
    expect_literal("-0x8000000000000000", UINT64_C(9223372036854775808), True);
    expect_rejected("u64 overflow decimal", "mod t; stack u64 x = 18446744073709551616;",
                    "exceeds maximum supported magnitude");
    expect_rejected("u64 overflow hex", "mod t; stack u64 x = 0x10000000000000000;",
                    "exceeds maximum supported magnitude");
    expect_rejected("u64 overflow negative magnitude", "mod t; stack i64 x = -18446744073709551616;",
                    "exceeds maximum supported magnitude");
    expect_rejected("hex without digit", "mod t; stack i64 x = 0x;",
                    "expected hexadecimal digit");
    expect_rejected("uppercase hex without digit", "mod t; stack i64 x = 0X;",
                    "expected hexadecimal digit");
    expect_rejected("hex invalid first digit", "mod t; stack i64 x = 0xG;",
                    "expected hexadecimal digit");
    expect_rejected("array dimension overflow", "mod t; stack u8 x[18446744073709551615];",
                    "out of range for array dimension");
    expect_rejected("bitfield width overflow", "mod t; type S: struct { u64 x: 18446744073709551615; };",
                    "out of range for bitfield width");
    expect_rejected("alignment overflow", "mod t; type S: @align(18446744073709551615) struct { u8 x; };",
                    "out of range for alignment");
    expect_rejected("zero bitfield width", "mod t; type S: struct { u64 x: 0; };",
                    "bitfield width must be greater than zero");
    expect_rejected("zero hexadecimal bitfield width", "mod t; type S: struct { u64 x: 0x0; };",
                    "bitfield width must be greater than zero");
    expect_rejected("zero alignment", "mod t; type S: @align(0) struct { u8 x; };",
                    "alignment must be greater than zero");
    expect_rejected("zero hexadecimal alignment", "mod t; type S: @align(0x0) struct { u8 x; };",
                    "alignment must be greater than zero");
    expect_rejected("empty alignment", "mod t; type S: @align() struct { u8 x; };",
                    "expected an integer literal for alignment");
    expect_accepted("omitted alignment and bitfield", "mod t; type S: struct { u8 x; };");
    expect_accepted("positive alignment and bitfield syntax", "mod t; type S: @align(1) struct { u8 x: 1; };");
    expect_rejected("fileheader overflow", "@[[org: 18446744073709551615]]; mod t;",
                    "out of range for fileheader value");
    expect_rejected("fileheader condition overflow", "@[[if: pointer_width == 18446744073709551615]]; mod t;",
                    "out of range for fileheader condition");
    /* Fileheader decimals now use the same grammar as expression literals;
       a leading zero does not silently switch strtol() to octal. */
    case_name = "fileheader decimal with leading zero";
    for (int mode = 0; mode < 2; mode++) {
        node_t *m = parse_case("@[[org: 010]]; mod t;", (boolean_t)mode);
        CHECK(m && !get_error_count());
        CHECK(m->headers && m->headers->count == 1);
        CHECK(m->headers->items[0].int_val == 10);
        finish_case();
    }
}

static void test_discarded_lexer_errors(void) {
    const char *prefixes[] = {
        "mod t; int macro let unused! = ",
        "mod t; ext macro let unused! = ",
        "mod t; int macro fn unused! { () => { ",
        "mod t; ext macro fn unused! { () => { ",
        "mod t; int macro unused! { () => { ",
        "mod t; ext macro unused! { () => { ",
        "mod t; @comptime if false { ",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        char source[256];
        snprintf(source, sizeof(source), "%s/* unterminated", prefixes[i]);
        case_name = "lexical failure in discarded macro or conditional";
        CHECK(parse_case(source, True) == NULL);
        CHECK(get_error_count() == 1);
        CHECK(diag_get_captured_count() == 1);
        const captured_diag_t *d = diag_get_captured(0);
        CHECK(d && strcmp(d->diag.message, "unterminated block comment") == 0);
        CHECK(strcmp(d->filename, test_file) == 0);
        CHECK(d->diag.labels[0].loc.line == 1);
        CHECK(d->diag.labels[0].loc.col == strlen(prefixes[i]) + 1);
        finish_case();
    }
    case_name = "malformed hex in unused macro";
    CHECK(parse_case("mod t; int macro let unused! = 0x;", True) == NULL);
    CHECK(get_error_count() == 1);
    CHECK(strstr(diag_get_captured(0)->diag.message, "expected hexadecimal digit"));
    finish_case();
}

static void test_comments(void) {
    case_name = "unterminated comment source location";
    const char *source = "mod t;\n  /* outer\n /* nested */\n";
    for (int mode = 0; mode < 2; mode++) {
        CHECK(parse_case(source, (boolean_t)mode) == NULL);
        CHECK(get_error_count() == 1);
        const captured_diag_t *d = diag_get_captured(0);
        CHECK(d && strcmp(d->diag.message, "unterminated block comment") == 0);
        CHECK(d->diag.labels[0].loc.line == 2);
        CHECK(d->diag.labels[0].loc.col == 3);
        finish_case();
    }
    expect_accepted("nested comments", "/* a /* b */ c */mod t;/* done */");
    expect_accepted("line comment EOF", "mod t; // done");
    expect_rejected("bare open comment", "/*", "unterminated block comment");
    expect_rejected("partly closed comment", "mod t; /* /* */", "unterminated block comment");
    expect_rejected("EOF comment star", "mod t; /* *", "unterminated block comment");
    expect_rejected("trailing comment", "mod t; fn main(void): i32 { ret 0; } /* open",
                    "unterminated block comment");
    case_name = "lexer reports incomplete comment once";
    lexer_t lex;
    init_lexer(&lex, " \n /* unfinished");
    token_t t = next_token(&lex);
    CHECK(t.kind == TokError && t.line == 2 && t.col == 2);
    CHECK(next_token(&lex).kind == TokEof);
    CHECK(next_token(&lex).kind == TokEof);
}

static void check_eof_token(const char *source, const char *file,
                            usize_t line, usize_t col) {
    CHECK(stream && stream->count > 0);
    for (int i = 0; i < stream->count - 1; i++)
        CHECK(stream->tokens[i].tok.kind != TokEof);
    const pp_token_t *eof = &stream->tokens[stream->count - 1];
    CHECK(eof->tok.kind == TokEof);
    CHECK(eof->tok.start == source + strlen(source));
    CHECK(eof->tok.length == 0);
    CHECK(eof->tok.file == file);
    CHECK(eof->tok.line == line);
    CHECK(eof->tok.col == col);
    CHECK(eof->expansion == NULL);
}

static void check_eof_diagnostic(const char *file, const char *expected,
                                 usize_t line, usize_t col) {
    CHECK(get_error_count() == 1);
    CHECK(diag_get_captured_count() == 1);
    const captured_diag_t *d = diag_get_captured(0);
    CHECK(d && strcmp(d->filename, file) == 0);
    char message[128];
    snprintf(message, sizeof(message), "unexpected end of file, expected %s", expected);
    CHECK(strcmp(d->diag.message, message) == 0);
    CHECK(d->diag.label_count == 1);
    CHECK(d->diag.labels[0].loc.line == line);
    CHECK(d->diag.labels[0].loc.col == col);
    CHECK(d->diag.labels[0].loc.len == 1);
}

static void test_eof_locations(void) {
    const struct {
        const char *name, *source, *expected;
        usize_t line, col;
    } cases[] = {
        {"empty source EOF", "", "'mod'", 1, 1},
        {"newline-only EOF", "\n", "'mod'", 2, 1},
        {"whitespace-only EOF", " \t\n  ", "'mod'", 2, 3},
        {"line-comment-only EOF", "// done", "'mod'", 1, 8},
        {"block-comment-only EOF", "/* done */\n", "'mod'", 2, 1},
        {"module semicolon EOF", "mod t", "';'", 1, 6},
        {"module semicolon after newline", "mod t\n", "';'", 2, 1},
        {"module semicolon after whitespace", "mod t \t", "';'", 1, 8},
        {"module semicolon after line comment", "mod t // done", "';'", 1, 14},
        {"module semicolon after block comment", "mod t /* done */\n  ", "';'", 2, 3},
        {"closing brace EOF", "mod t;\nfn f(void): void {\n    ret;\n", "'}'", 4, 1},
        {"closing brace after comment", "mod t;\nfn f(void): void {\n    ret;\n  /* done */  ", "'}'", 4, 15},
        {"complete module EOF", "mod t;", NULL, 1, 7},
        {"complete module newline EOF", "mod t;\n", NULL, 2, 1},
        {"complete module whitespace EOF", "mod t; \t", NULL, 1, 9},
        {"complete module line-comment EOF", "mod t; // done", NULL, 1, 15},
        {"complete module block-comment EOF", "mod t; /* done */\n  ", NULL, 2, 3},
        {"complete function EOF", "mod t;\nfn f(void): void {\n    ret;\n}\n", NULL, 5, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        case_name = cases[i].name;
        for (int mode = 0; mode < 2; mode++) {
            node_t *module = parse_case(cases[i].source, (boolean_t)mode);
            if (cases[i].expected) {
                CHECK(module == NULL);
                check_eof_diagnostic(test_file, cases[i].expected,
                                     cases[i].line, cases[i].col);
            } else {
                CHECK(module != NULL);
                CHECK(get_error_count() == 0);
                CHECK(diag_get_captured_count() == 0);
            }
            if (mode)
                check_eof_token(cases[i].source, test_file, cases[i].line, cases[i].col);
            finish_case();
        }
    }
}

static void test_eof_file_ownership(void) {
    /* Exercise the per-file parser entry used by source imports. The stream
       borrows its source/path through parsing; captured diagnostics must still
       own their filename and coordinates after those buffers are released. */
    const struct { const char *file, *source, *expected; usize_t line; } cases[] = {
        {"imported/empty.sts", "", "'mod'", 1},
        {"imported/module.sts", "mod imported.module\n", "';'", 2},
    };
    case_name = "EOF imported-file diagnostic ownership";
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int mode = 0; mode < 2; mode++) {
            char *file = strdup(cases[i].file);
            char *source = strdup(cases[i].source);
            CHECK(file && source);
            restore_diagnostic_counts(0, 0);
            diag_clear_captured();
            diag_set_render_enabled(False);
            diag_register_source(test_file, "mod caller;\n");
            diag_set_file(test_file);
            stream = mode ? pp_process(source, file, NULL, 0) : NULL;
            if (mode) {
                CHECK(stream);
                CHECK(parse_from_stream(stream) == NULL);
                check_eof_token(source, file, cases[i].line, 1);
            } else {
                diag_set_file(file);
                CHECK(parse(source) == NULL);
            }
            check_eof_diagnostic(file, cases[i].expected, cases[i].line, 1);
            finish_case();
            memset(file, 'x', strlen(file));
            memset(source, 'x', strlen(source));
            free(file);
            free(source);
            diag_clear_sources();
            CHECK(strcmp(diag_get_file(), cases[i].file) == 0);
            check_eof_diagnostic(cases[i].file, cases[i].expected, cases[i].line, 1);
            diag_set_file(test_file);
        }
    }
}

/* Literal byte spelling and decoding are unchanged by position tracking. The
   end coordinates below are independent expectations after a "\r\n \t" prefix. */
static const struct literal_case {
    const char *name, *raw, *value;
    token_kind_t kind;
    usize_t end_line, end_col;
} literal_cases[] = {
    {"double quoted raw LF", "\"a\nb\"", "a\nb", TokStackStr, 3, 3},
    {"single quoted raw LF", "'a\nb'", "a\nb", TokStackStr, 3, 3},
    {"double quoted escaped physical LF", "\"a\\\nb\"", "a\nb", TokStackStr, 3, 3},
    {"single quoted escaped physical LF", "'a\\\nb'", "a\nb", TokStackStr, 3, 3},
    {"double quoted raw CRLF", "\"a\r\nb\"", "a\r\nb", TokStackStr, 3, 3},
    {"single quoted raw CRLF", "'a\r\nb'", "a\r\nb", TokStackStr, 3, 3},
    {"double quoted escaped CRLF", "\"a\\\r\nb\"", "a\r\nb", TokStackStr, 3, 3},
    {"single quoted escaped CRLF", "'a\\\r\nb'", "a\r\nb", TokStackStr, 3, 3},
    {"mixed multiple physical lines", "\"a\n\tb\\\n\tc\r\nd\"", "a\n\tb\n\tc\r\nd", TokStackStr, 5, 3},
    {"string lone CR", "\"a\rb\"", "a\rb", TokStackStr, 2, 8},
    {"string escaped lone CR", "\"a\\\rb\"", "a\rb", TokStackStr, 2, 9},
    {"string tab byte column", "\"a\tb\"", "a\tb", TokStackStr, 2, 8},
    {"string UTF-8 byte column", "\"\xc3\xa9\"", "\xc3\xa9", TokStackStr, 2, 7},
    {"textual newline escape", "\"a\\nb\"", "a\nb", TokStackStr, 2, 9},
    {"escaped double quote", "\"a\\\"b\"", "a\"b", TokStackStr, 2, 9},
    {"escaped single quote", "'a\\'b'", "a'b", TokStackStr, 2, 9},
    {"backtick raw LF", "`\n`", "\n", TokCharLit, 3, 2},
    {"backtick escaped physical LF", "`\\\n`", "\n", TokCharLit, 3, 2},
    {"backtick raw CR", "`\r`", "\r", TokCharLit, 2, 6},
    {"backtick escaped physical CR", "`\\\r`", "\r", TokCharLit, 2, 7},
    {"backtick textual newline escape", "`\\n`", "\n", TokCharLit, 2, 7},
    {"backtick escaped delimiter", "`\\``", "`", TokCharLit, 2, 7},
    {"dollar double raw LF", "$\"a\nb\"", "a\nb", TokDollarStr, 3, 3},
    {"dollar single raw LF", "$'a\nb'", "a\nb", TokDollarStr, 3, 3},
    {"dollar double escaped physical LF", "$\"a\\\nb\"", "a\\\nb", TokDollarStr, 3, 3},
    {"dollar single escaped physical LF", "$'a\\\nb'", "a\\\nb", TokDollarStr, 3, 3},
    {"dollar double raw CRLF", "$\"a\r\nb\"", "a\r\nb", TokDollarStr, 3, 3},
    {"dollar single raw CRLF", "$'a\r\nb'", "a\r\nb", TokDollarStr, 3, 3},
    {"dollar double escaped CRLF", "$\"a\\\r\nb\"", "a\\\r\nb", TokDollarStr, 3, 3},
    {"dollar single escaped CRLF", "$'a\\\r\nb'", "a\\\r\nb", TokDollarStr, 3, 3},
    {"dollar multiple physical lines", "$'a\n\tb\\\n\tc'", "a\n\tb\\\n\tc", TokDollarStr, 4, 4},
    {"dollar lone CR", "$'a\rb'", "a\rb", TokDollarStr, 2, 9},
};

static void test_literal_tokens(void) {
    for (size_t i = 0; i < sizeof(literal_cases) / sizeof(literal_cases[0]); i++) {
        const struct literal_case *c = &literal_cases[i];
        case_name = c->name;
        for (int following = 0; following < 2; following++) {
            char source[256];
            snprintf(source, sizeof(source), "\r\n \t%s%s", c->raw,
                     following ? "\tmark" : "");
            lexer_t lex;
            init_lexer(&lex, source);
            token_t t = next_token(&lex);
            CHECK(t.kind == c->kind);
            CHECK(t.start == source + 4);
            CHECK(t.length == strlen(c->raw));
            CHECK(memcmp(t.start, c->raw, t.length) == 0);
            CHECK(t.line == 2 && t.col == 3 && t.file == NULL);
            CHECK(lex.line == c->end_line);
            CHECK((usize_t)(lex.current - lex.line_start) + 1 == c->end_col);
            if (following) {
                t = next_token(&lex);
                CHECK(t.kind == TokIdent && t.length == 4);
                CHECK(t.start == source + 4 + strlen(c->raw) + 1);
                CHECK(memcmp(t.start, "mark", 4) == 0);
                CHECK(t.line == c->end_line && t.col == c->end_col + 1);
            }
            for (int repeat = 0; repeat < 2; repeat++) {
                t = next_token(&lex);
                CHECK(t.kind == TokEof && t.length == 0);
                CHECK(t.start == source + strlen(source));
                CHECK(t.line == c->end_line);
                CHECK(t.col == c->end_col + (following ? 5 : 0));
            }
        }
    }

    case_name = "physical LF counted once across comments and literals";
    const char *source = "\r\n \t/* outer\n /* nested\r\n */ done */\r\n"
                         "\t\"a\\\nb\" // tail\r\n\t`\\\n`\n\r x";
    lexer_t lex;
    init_lexer(&lex, source);
    token_t t = next_token(&lex);
    CHECK(t.kind == TokStackStr && t.line == 5 && t.col == 2);
    t = next_token(&lex);
    CHECK(t.kind == TokCharLit && t.line == 7 && t.col == 2);
    t = next_token(&lex);
    CHECK(t.kind == TokIdent && t.line == 9 && t.col == 3);
    t = next_token(&lex);
    CHECK(t.kind == TokEof && t.line == 9 && t.col == 4);
}

static void test_literal_values(void) {
    for (size_t i = 0; i < sizeof(literal_cases) / sizeof(literal_cases[0]); i++) {
        const struct literal_case *c = &literal_cases[i];
        char source[256];
        case_name = c->name;
        /* Parentheses force the parser to save and restore the full lexer
           while distinguishing grouped expressions from casts. */
        for (int wrapped = 0; wrapped < 2; wrapped++) {
            snprintf(source, sizeof(source), "mod t;\n\tstack i32 value = %s%s%s;",
                     wrapped ? "(" : "", c->raw, wrapped ? ")" : "");
            for (int mode = 0; mode < 2; mode++) {
                node_t *m = parse_case(source, (boolean_t)mode);
                CHECK(m && !get_error_count());
                CHECK(m->as.module.decls.count == 1);
                node_t *n = m->as.module.decls.items[0]->as.var_decl.init;
                CHECK(n && n->line == 2);
                if (c->kind == TokStackStr) {
                    CHECK(n->kind == NodeStrLitExpr && !n->as.str_lit.is_heap);
                    CHECK(n->as.str_lit.len == strlen(c->value));
                    CHECK(memcmp(n->as.str_lit.value, c->value, strlen(c->value) + 1) == 0);
                } else if (c->kind == TokCharLit) {
                    CHECK(n->kind == NodeCharLitExpr);
                    CHECK(n->as.char_lit.value == c->value[0]);
                } else {
                    CHECK(n->kind == NodeIdentExpr);
                    CHECK(strcmp(n->as.ident.name, c->value) == 0);
                    CHECK(n->col == 20 + (usize_t)wrapped && n->len == strlen(c->raw));
                }
                finish_case();
            }
        }
    }
}

static void test_unterminated_literal_locations(void) {
    const struct {
        const char *raw, *message;
        usize_t line, col;
    } cases[] = {
        {"\"a\nb", "unterminated string", 3, 2},
        {"'a\\\nb", "unterminated string", 3, 2},
        {"$\"a\\\r\nb", "unterminated string", 3, 2},
        {"\"a\\", "unterminated string", 2, 6},
        {"`\n", "unterminated char literal", 3, 1},
        {"`\\\n", "unterminated char literal", 3, 1},
        {"`\\", "unterminated char literal", 2, 5},
    };
    case_name = "unterminated literals retain physical cursor anchor";
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[128];
        snprintf(source, sizeof(source), "\r\n \t%s", cases[i].raw);
        lexer_t lex;
        init_lexer(&lex, source);
        token_t t = next_token(&lex);
        CHECK(t.kind == TokError && strcmp(t.start, cases[i].message) == 0);
        CHECK(t.length == strlen(cases[i].message));
        CHECK(t.line == cases[i].line && t.col == cases[i].col);
        t = next_token(&lex);
        CHECK(t.kind == TokEof && t.start == source + strlen(source));
        CHECK(t.line == cases[i].line && t.col == cases[i].col);
    }
    /* CRLF is two character bytes, so it remains invalid in a backtick
       literal. The cursor is still on LF when the missing-close error occurs. */
    const char *crlf[] = {"`\r\n`", "`\\\r\n`"};
    for (size_t i = 0; i < 2; i++) {
        lexer_t lex;
        init_lexer(&lex, crlf[i]);
        token_t t = next_token(&lex);
        CHECK(t.kind == TokError && strcmp(t.start, "unterminated char literal") == 0);
        CHECK(t.line == 1 && t.col == 3 + i);
        CHECK(*lex.current == '\n');
    }
}

static void check_expression_diagnostic(const char *file, usize_t line, usize_t col) {
    CHECK(get_error_count() == 1 && diag_get_captured_count() == 1);
    const captured_diag_t *d = diag_get_captured(0);
    CHECK(d && strcmp(d->filename, file) == 0);
    CHECK(strcmp(d->diag.message, "expected an expression, found ';'") == 0);
    CHECK(d->diag.label_count == 1);
    CHECK(d->diag.labels[0].loc.line == line);
    CHECK(d->diag.labels[0].loc.col == col);
    CHECK(d->diag.labels[0].loc.len == 1);
}

static void test_literal_continuation_diagnostics(void) {
    const struct { const char *raw; usize_t line, col; } cases[] = {
        {"\"a\nb\"", 3, 6}, {"'a\\\nb'", 3, 6},
        {"\"a\\\r\nb\"", 3, 6}, {"`\n`", 3, 5},
        {"`\\\n`", 3, 5}, {"$'a\\\nb'", 3, 6},
    };
    case_name = "ordinary expression error on literal continuation line";
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[256];
        snprintf(source, sizeof(source), "mod t;\nstack i32 value = %s + ;", cases[i].raw);
        for (int mode = 0; mode < 2; mode++) {
            CHECK(parse_case(source, (boolean_t)mode) == NULL);
            check_expression_diagnostic(test_file, cases[i].line, cases[i].col);
            finish_case();
        }
    }

    /* The per-file entry used for imports must not keep the caller's file.
       Captured diagnostics own their path/coordinates after input cleanup. */
    for (int mode = 0; mode < 2; mode++) {
        char *file = strdup("imported/literal.sts");
        char *source = strdup("mod imported.literal;\nstack i32 value = \"a\\\nb\" + ;");
        CHECK(file && source);
        restore_diagnostic_counts(0, 0);
        diag_clear_captured();
        diag_set_render_enabled(False);
        diag_register_source(test_file, "mod caller;\n");
        diag_set_file(test_file);
        stream = mode ? pp_process(source, file, NULL, 0) : NULL;
        if (mode) {
            CHECK(stream);
            boolean_t found = False;
            for (int i = 0; i < stream->count; i++) {
                const pp_token_t *pt = &stream->tokens[i];
                if (pt->tok.kind != TokStackStr) continue;
                CHECK(!found);
                found = True;
                CHECK(pt->tok.file == file && pt->tok.start == strstr(source, "\"a"));
                CHECK(pt->tok.line == 2 && pt->tok.col == 19 && pt->tok.length == 6);
                CHECK(pt->expansion == NULL);
            }
            CHECK(found);
            CHECK(parse_from_stream(stream) == NULL);
        } else {
            diag_set_file(file);
            CHECK(parse(source) == NULL);
        }
        check_expression_diagnostic(file, 3, 6);
        finish_case();
        memset(file, 'x', strlen(file));
        memset(source, 'x', strlen(source));
        free(file);
        free(source);
        diag_clear_sources();
        check_expression_diagnostic("imported/literal.sts", 3, 6);
        diag_set_file(test_file);
    }
}

static void test_literal_macro_ownership(void) {
    case_name = "imported macro literal definition and invocation locations";
    char *file = strdup("imported/values.sts");
    char *source = strdup("mod imported.values;\next macro let shared! = \"A\\\nB\";\n");
    const char *caller = "mod caller;\nstack i32 value = shared.!;\n";
    CHECK(file && source);
    restore_diagnostic_counts(0, 0);
    diag_clear_captured();
    diag_set_render_enabled(False);
    pp_stream_t *definition = pp_process(source, file, NULL, 0);
    CHECK(definition && !get_error_count());
    pp_macro_set_t *imports[] = {pp_get_exports(definition)};
    CHECK(imports[0] && imports[0]->let_count == 1);
    const macro_tok_t *stored = &imports[0]->lets[0].tokens[0];
    CHECK(stored->kind == TokStackStr && stored->text == strstr(source, "\"A"));
    CHECK(stored->file == file && stored->line == 2 && stored->col == 25);
    stream = pp_process(caller, test_file, imports, 1);
    CHECK(stream && !get_error_count());
    boolean_t found = False;
    for (int i = 0; i < stream->count; i++) {
        const pp_token_t *pt = &stream->tokens[i];
        if (pt->tok.kind != TokStackStr) continue;
        CHECK(!found);
        found = True;
        CHECK(pt->tok.start != stored->text);
        CHECK(pt->tok.length == 6 && memcmp(pt->tok.start, "\"A\\\nB\"", 6) == 0);
        CHECK(pt->tok.file == file && pt->tok.line == 2 && pt->tok.col == 25);
        CHECK(pt->expansion && strcmp(pt->expansion->macro_name, "shared") == 0);
        CHECK(pt->expansion->call_file == test_file);
        CHECK(pt->expansion->call_line == 2 && pt->expansion->call_col == 19);
        CHECK(pt->expansion->parent == NULL);
    }
    CHECK(found);
    check_eof_token(caller, test_file, 3, 1);
    node_t *m = parse_from_stream(stream);
    CHECK(m && !get_error_count());
    node_t *n = m->as.module.decls.items[0]->as.var_decl.init;
    CHECK(n && n->kind == NodeStrLitExpr && n->line == 2);
    CHECK(n->as.str_lit.len == 3 && memcmp(n->as.str_lit.value, "A\nB", 4) == 0);
    finish_case();
    pp_stream_free(definition);
    free(file);
    free(source);
    diag_clear_sources();
    diag_set_file(test_file);
}

static const char *format_trailing_message =
    "unexpected trailing token in format placeholder expression";

/* Independent oracle walks the complete original file, not a token or child
   parser coordinate. The marker names the exact first unconsumed raw token. */
static src_loc_t format_marker_loc(const char *source, const char *marker, usize_t len) {
    const char *at = strstr(source, marker);
    CHECK(at);
    src_loc_t loc = SRC_LOC(1, 1, len);
    for (const char *s = source; s < at; s++) {
        if (*s == '\n') { loc.line++; loc.col = 1; }
        else loc.col++;
    }
    return loc;
}

static void check_format_diagnostic(const char *file, src_loc_t loc) {
    CHECK(get_error_count() == 1 && diag_get_captured_count() == 1);
    const captured_diag_t *d = diag_get_captured(0);
    CHECK(d && strcmp(d->filename, file) == 0);
    CHECK(strcmp(d->diag.message, format_trailing_message) == 0);
    CHECK(d->diag.label_count == 1);
    CHECK(d->diag.labels[0].loc.line == loc.line);
    CHECK(d->diag.labels[0].loc.col == loc.col);
    CHECK(d->diag.labels[0].loc.len == loc.len);
}

static void make_format_source(char *out, size_t size, const char *raw,
                               char quote, int print_path) {
    if (print_path)
        snprintf(out, size, "mod t;\nfn main(void): void { print.(@%c%s%c); }",
                 quote, raw, quote);
    else
        snprintf(out, size, "mod t;\nstack i8 *rw value = @%c%s%c;", quote, raw, quote);
}

static void test_format_trailing_tokens(void) {
    const struct { const char *raw, *marker; usize_t len; } cases[] = {
        {"{1 2}", "2}", 1}, {"{1, 2}", ", 2}", 1},
        {"{1; 2}", "; 2}", 1}, {"{1)}", ")}", 1},
        {"{1 trailing}", "trailing", 8},
        {"{1 /* completed */ trailing}", "trailing", 8},
        {"{1 // completed\n trailing}", "trailing", 8},
        {"{1\n trailing}", "trailing", 8},
        {"before\n\t\xc3\xa9{1 trailing}", "trailing", 8},
        {"before\r\n\t\xc3\xa9{1 trailing}", "trailing", 8},
        {"before\\\n\t\xc3\xa9{1 trailing}", "trailing", 8},
        {"before\\\r\n\t\xc3\xa9{1 trailing}", "trailing", 8},
        {"\t\xc3\xa9\r{1 trailing}", "trailing", 8},
        {"{1 trailing:08x}", "trailing", 8},
        {"ok={3} bad={1 trailing} end={4}", "trailing", 8},
        {"{1:q}", ":q", 1}, {"{1:xx}", ":xx", 1},
        {"{1:x + 1}", ":x", 1}, {"{1:x()}", ":x", 1},
        {"{1:x 2}", ":x", 1}, {"{1:x:x}", ":x", 1},
        {"{1:x:08x}", ":x", 1}, {"{1:x:.4}", ":x", 1},
        {"{1:x }", ":x", 1}, {"{1:x /* done */}", ":x", 1},
        {"{helpers:x():q}", ":q", 1},
        {"{helpers:x():x:08x}", ":x:08x", 1},
    };
    case_name = "format placeholders reject trailing tokens at original byte spans";
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int path = 0; path < 2; path++) {
            for (int quote = 0; quote < 2; quote++) {
                char source[512];
                make_format_source(source, sizeof(source), cases[i].raw, quote ? '"' : '\'', path);
                src_loc_t loc = format_marker_loc(source, cases[i].marker, cases[i].len);
                for (int mode = 0; mode < 2; mode++) {
                    CHECK(parse_case(source, (boolean_t)mode) == NULL);
                    check_format_diagnostic(test_file, loc);
                    finish_case();
                }
            }
        }
    }

    /* These children already fail, sometimes with an unconsumed token. The
       caller must propagate that failure without adding its own diagnostic. */
    const char *child_errors[] = {"{1 + }", "{1 + ; 2}", "{1 /*}"};
    case_name = "format placeholder child errors are not duplicated";
    for (size_t i = 0; i < sizeof(child_errors) / sizeof(child_errors[0]); i++) {
        for (int path = 0; path < 2; path++) {
            char source[256];
            make_format_source(source, sizeof(source), child_errors[i], '\'', path);
            for (int mode = 0; mode < 2; mode++) {
                CHECK(parse_case(source, (boolean_t)mode) == NULL);
                CHECK(get_error_count() == 1 && diag_get_captured_count() == 1);
                CHECK(strcmp(diag_get_captured(0)->diag.message, format_trailing_message) != 0);
                finish_case();
            }
        }
    }
}

/* The template is a separate arena string, not a view into the raw spelling. */
static void check_canonical_format(node_t *n, const char *raw, const char *canonical,
                                   usize_t argc, boolean_t on_heap) {
    CHECK(n && n->kind == NodeComptimeFmt);
    CHECK(n->as.comptime_fmt.fmt && n->as.comptime_fmt.template_fmt);
    CHECK(n->as.comptime_fmt.fmt != n->as.comptime_fmt.template_fmt);
    CHECK(n->as.comptime_fmt.fmt_len == strlen(raw));
    CHECK(memcmp(n->as.comptime_fmt.fmt, raw, strlen(raw) + 1) == 0);
    CHECK(n->as.comptime_fmt.template_len == strlen(canonical));
    CHECK(n->as.comptime_fmt.template_len <= n->as.comptime_fmt.fmt_len);
    CHECK(memcmp(n->as.comptime_fmt.template_fmt, canonical, strlen(canonical) + 1) == 0);
    CHECK(n->as.comptime_fmt.args.count == argc);
    CHECK(n->as.comptime_fmt.on_heap == on_heap);
}

static void test_format_canonical_templates(void) {
    const struct { const char *raw, *canonical; usize_t argc; } cases[] = {
        {"", "", 0},
        {"plain % \\% \\{literal\\} \\\\ \\' \\\" \\n\\t\\0\\q",
         "plain % \\% \\{literal\\} \\\\ \\' \\\" \\n\\t\\0\\q", 0},
        {"line\nline\r\nline\\\nline\\\r\nend", "line\nline\r\nline\\\nline\\\r\nend", 0},
        {"{helpers:x()}|{helpers:x():x}|{helpers:x():X}|{helpers:x():08x}",
         "{}|{:x}|{:X}|{:08x}", 4},
        {"{greeter:Builder:company()}|{greeter:Builder:company():x}|"
         "{greeter:Builder:company():X}|{greeter:Builder:company():08x}",
         "{}|{:x}|{:X}|{:08x}", 4},
        {"\\{kept\\}%{1}\\n{2}\\t{3}\\\\{4}\\0{5}\\q{6}",
         "\\{kept\\}%{}\\n{}\\t{}\\\\{}\\0{}\\q{}", 6},
        {"first\n{1}\r\n{2}\\\n{3}\\\r\n{4}end", "first\n{}\r\n{}\\\n{}\\\r\n{}end", 4},
        /* Delimiter preservation only: these raw specs are not validated C
           conversions and must never be executed as native format probes. */
        {"{1:0\\}8x}|{2:0(})8x}|{3:0[}]8x}", "{:0\\}8x}|{:0(})8x}|{:0[}]8x}", 3},
    };
    case_name = "canonical template preserves raw bytes, selected slots and storage";
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int quote = 0; quote < 2; quote++) {
            for (int storage = 0; storage < 2; storage++) {
                char source[1024];
                snprintf(source, sizeof(source), "mod t; stack i8 *rw value = %s@%c%s%c;",
                         storage ? "heap " : "", quote ? '"' : '\'', cases[i].raw,
                         quote ? '"' : '\'');
                for (int mode = 0; mode < 2; mode++) {
                    node_t *m = parse_case(source, (boolean_t)mode);
                    CHECK(m && !get_error_count());
                    node_t *n = m->as.module.decls.items[0]->as.var_decl.init;
                    check_canonical_format(n, cases[i].raw, cases[i].canonical,
                                           cases[i].argc, (boolean_t)storage);
                    for (usize_t arg = 0; arg < cases[i].argc; arg++) {
                        node_t *a = n->as.comptime_fmt.args.items[arg];
                        if (i == 3 || i == 4) {
                            CHECK(a->kind == NodeColonCall && a->as.colon_call.args.count == 0);
                            CHECK(strcmp(a->as.colon_call.module_name, i == 3 ? "helpers" : "greeter") == 0);
                            CHECK(strcmp(a->as.colon_call.method_name, i == 3 ? "x" : "company") == 0);
                            if (i == 3) CHECK(a->as.colon_call.type_name == NULL);
                            else CHECK(strcmp(a->as.colon_call.type_name, "Builder") == 0);
                        } else {
                            CHECK(a->kind == NodeIntLitExpr && a->as.int_lit.value == arg + 1);
                        }
                    }
                    finish_case();
                }
            }
            char inline_source[1024];
            make_format_source(inline_source, sizeof(inline_source), cases[i].raw,
                               quote ? '"' : '\'', 1);
            for (int mode = 0; mode < 2; mode++) {
                node_t *m = parse_case(inline_source, (boolean_t)mode);
                CHECK(m && !get_error_count());
                node_t *n = m->as.module.decls.items[0]->as.fn_decl.body->as.block.stmts.items[0];
                CHECK(n->kind == NodePrintStmt);
                CHECK(n->as.print_stmt.fmt_len == strlen(cases[i].canonical));
                CHECK(strcmp(n->as.print_stmt.fmt, cases[i].canonical) == 0);
                CHECK(n->as.print_stmt.args.count == cases[i].argc);
                finish_case();
            }
        }
    }
}

static void test_format_template_ownership(void) {
    const char *raw = "\\{kept\\}%{helpers:x():08x}|{42}\\n";
    const char *canonical = "\\{kept\\}%{:08x}|{}\\n";
    case_name = "canonical and raw format strings survive caller and stream release";
    for (int storage = 0; storage < 2; storage++) {
        for (int mode = 0; mode < 2; mode++) {
            char source_buf[256];
            snprintf(source_buf, sizeof(source_buf), "mod t; stack i8 *rw value = %s@'%s';",
                     storage ? "heap " : "", raw);
            char *source = strdup(source_buf);
            CHECK(source);
            node_t *m = parse_case(source, (boolean_t)mode);
            CHECK(m && !get_error_count());
            node_t *n = m->as.module.decls.items[0]->as.var_decl.init;
            node_t copied = *n;
            CHECK(copied.as.comptime_fmt.fmt == n->as.comptime_fmt.fmt);
            CHECK(copied.as.comptime_fmt.template_fmt == n->as.comptime_fmt.template_fmt);
            CHECK(copied.as.comptime_fmt.args.items == n->as.comptime_fmt.args.items);
            if (stream) { pp_stream_free(stream); stream = NULL; }
            memset(source, 'x', strlen(source));
            free(source);
            diag_clear_sources();
            check_canonical_format(n, raw, canonical, 2, (boolean_t)storage);
            check_canonical_format(&copied, raw, canonical, 2, (boolean_t)storage);
            CHECK(strcmp(copied.as.comptime_fmt.args.items[0]->as.colon_call.module_name, "helpers") == 0);
            CHECK(strcmp(copied.as.comptime_fmt.args.items[0]->as.colon_call.method_name, "x") == 0);
            CHECK(copied.as.comptime_fmt.args.items[1]->as.int_lit.value == 42);
            finish_case();
        }
    }

    case_name = "canonical macro format owns bytes after definition and expansion release";
    char *file = strdup("imported/canonical.sts");
    char *source = strdup("mod imported.canonical; ext macro let shared! = @'\\{kept\\}%{helpers:x():08x}|{42}\\n';");
    char *caller = strdup("mod caller; stack i8 *rw value = shared.!;");
    CHECK(file && source && caller);
    restore_diagnostic_counts(0, 0);
    diag_clear_captured();
    diag_set_render_enabled(False);
    pp_stream_t *definition = pp_process(source, file, NULL, 0);
    CHECK(definition && !get_error_count());
    pp_macro_set_t *imports[] = {pp_get_exports(definition)};
    CHECK(imports[0] && imports[0]->let_count == 1);
    stream = pp_process(caller, test_file, imports, 1);
    CHECK(stream && !get_error_count());
    node_t *m = parse_from_stream(stream);
    CHECK(m && !get_error_count());
    node_t copied = *m->as.module.decls.items[0]->as.var_decl.init;
    pp_stream_free(stream); stream = NULL;
    pp_stream_free(definition);
    memset(file, 'x', strlen(file)); memset(source, 'x', strlen(source));
    memset(caller, 'x', strlen(caller));
    free(file); free(source); free(caller);
    diag_clear_sources();
    diag_set_file(test_file);
    check_canonical_format(&copied, raw, canonical, 2, False);
    CHECK(strcmp(copied.as.comptime_fmt.args.items[0]->as.colon_call.module_name, "helpers") == 0);
    CHECK(strcmp(copied.as.comptime_fmt.args.items[0]->as.colon_call.method_name, "x") == 0);
    CHECK(copied.as.comptime_fmt.args.items[1]->as.int_lit.value == 42);
    finish_case();
}

static void test_format_valid_expressions(void) {
    const char *raw = "\\{literal\\}\\n{1 + (2 * 3)} {f(1, 2)} {p.x} "
                      "{greeter:Builder:company()} {value:08x} {3.14159:.4} "
                      "{ 7 /* nested /* ok */ comment */ } {8 // completed\n} "
                      "{value = 9} {5 |> double} {(true ? 1 : 2)}";
    const char *rewritten = "\\{literal\\}\\n{} {} {} {} {:08x} {:.4} {} {} {} {} {}";
    case_name = "complete format expressions preserve AST, comments, specs and static colon";
    for (int path = 0; path < 2; path++) {
        for (int quote = 0; quote < 2; quote++) {
            char source[1024];
            make_format_source(source, sizeof(source), raw, quote ? '"' : '\'', path);
            for (int mode = 0; mode < 2; mode++) {
                node_t *m = parse_case(source, (boolean_t)mode);
                CHECK(m && !get_error_count());
                node_t *decl = m->as.module.decls.items[0];
                node_t *n = path ? decl->as.fn_decl.body->as.block.stmts.items[0]
                                 : decl->as.var_decl.init;
                CHECK(n->kind == (path ? NodePrintStmt : NodeComptimeFmt));
                const char *fmt = path ? n->as.print_stmt.fmt : n->as.comptime_fmt.fmt;
                usize_t fmt_len = path ? n->as.print_stmt.fmt_len : n->as.comptime_fmt.fmt_len;
                CHECK(fmt_len == strlen(path ? rewritten : raw));
                CHECK(strcmp(fmt, path ? rewritten : raw) == 0);
                if (!path) check_canonical_format(n, raw, rewritten, 11, False);
                node_list_t args = path ? n->as.print_stmt.args : n->as.comptime_fmt.args;
                CHECK(args.count == 11);
                CHECK(args.items[0]->kind == NodeBinaryExpr && args.items[0]->as.binary.op == TokPlus);
                CHECK(args.items[0]->as.binary.right->kind == NodeBinaryExpr);
                CHECK(args.items[0]->as.binary.right->as.binary.op == TokStar);
                CHECK(args.items[1]->kind == NodeCallExpr && args.items[1]->as.call.args.count == 2);
                CHECK(strcmp(args.items[1]->as.call.callee, "f") == 0);
                CHECK(args.items[2]->kind == NodeMemberExpr && strcmp(args.items[2]->as.member_expr.field, "x") == 0);
                CHECK(args.items[3]->kind == NodeColonCall);
                CHECK(strcmp(args.items[3]->as.colon_call.module_name, "greeter") == 0);
                CHECK(strcmp(args.items[3]->as.colon_call.type_name, "Builder") == 0);
                CHECK(strcmp(args.items[3]->as.colon_call.method_name, "company") == 0);
                CHECK(args.items[4]->kind == NodeIdentExpr && strcmp(args.items[4]->as.ident.name, "value") == 0);
                CHECK(args.items[5]->kind == NodeFloatLitExpr);
                CHECK(args.items[6]->kind == NodeIntLitExpr && args.items[6]->as.int_lit.value == 7);
                CHECK(args.items[7]->kind == NodeIntLitExpr && args.items[7]->as.int_lit.value == 8);
                CHECK(args.items[8]->kind == NodeAssignExpr && args.items[8]->as.assign.value->as.int_lit.value == 9);
                CHECK(args.items[9]->kind == NodeCallExpr && strcmp(args.items[9]->as.call.callee, "double") == 0);
                CHECK(args.items[9]->as.call.args.count == 1 && args.items[9]->as.call.args.items[0]->as.int_lit.value == 5);
                CHECK(args.items[10]->kind == NodeTernaryExpr);
                finish_case();
            }
        }
    }
}

static void test_format_alphabetic_specs(void) {
    const char *raw = "{255:x}|{255:X}|{7:b}|{7:o}|{value:x}|{helpers:x()}|"
                      "{helpers:x():x}|{greeter:Builder:company():X}";
    const char *rewritten = "{:x}|{:X}|{:b}|{:o}|{:x}|{}|{:x}|{:X}";
    case_name = "terminal alphabetic specs preserve complete static access first";
    for (int path = 0; path < 2; path++) {
        for (int quote = 0; quote < 2; quote++) {
            char source[512];
            make_format_source(source, sizeof(source), raw, quote ? '"' : '\'', path);
            for (int mode = 0; mode < 2; mode++) {
                node_t *m = parse_case(source, (boolean_t)mode);
                CHECK(m && !get_error_count());
                node_t *decl = m->as.module.decls.items[0];
                node_t *n = path ? decl->as.fn_decl.body->as.block.stmts.items[0]
                                 : decl->as.var_decl.init;
                const char *fmt = path ? n->as.print_stmt.fmt : n->as.comptime_fmt.fmt;
                usize_t len = path ? n->as.print_stmt.fmt_len : n->as.comptime_fmt.fmt_len;
                CHECK(strcmp(fmt, path ? rewritten : raw) == 0);
                CHECK(len == strlen(path ? rewritten : raw));
                if (!path) check_canonical_format(n, raw, rewritten, 8, False);
                node_list_t args = path ? n->as.print_stmt.args : n->as.comptime_fmt.args;
                CHECK(args.count == 8);
                for (int i = 0; i < 4; i++) {
                    CHECK(args.items[i]->kind == NodeIntLitExpr);
                    CHECK(args.items[i]->as.int_lit.value == (i < 2 ? 255 : 7));
                }
                CHECK(args.items[4]->kind == NodeIdentExpr && strcmp(args.items[4]->as.ident.name, "value") == 0);
                for (int i = 5; i < 8; i++) {
                    CHECK(args.items[i]->kind == NodeColonCall);
                    CHECK(args.items[i]->as.colon_call.args.count == 0);
                    CHECK(strcmp(args.items[i]->as.colon_call.module_name, i < 7 ? "helpers" : "greeter") == 0);
                    CHECK(strcmp(args.items[i]->as.colon_call.method_name, i < 7 ? "x" : "company") == 0);
                }
                CHECK(args.items[5]->as.colon_call.type_name == NULL);
                CHECK(args.items[6]->as.colon_call.type_name == NULL);
                CHECK(strcmp(args.items[7]->as.colon_call.type_name, "Builder") == 0);
                finish_case();
            }
        }
    }
    expect_accepted("heap format terminal spec", "mod t; fn f(void): void { heap i8 *rw s = heap @'{255:x}'; }");
}

static void test_format_file_ownership(void) {
    case_name = "format trailing diagnostic owns imported-file path and byte span";
    for (int path = 0; path < 2; path++) {
        for (int mode = 0; mode < 2; mode++) {
            char *file = strdup("imported/format.sts");
            char source_buf[256];
            make_format_source(source_buf, sizeof(source_buf), "first\\\r\n\t\xc3\xa9{1 trailing}", '\'', path);
            char *source = strdup(source_buf);
            CHECK(file && source);
            src_loc_t loc = format_marker_loc(source, "trailing", 8);
            restore_diagnostic_counts(0, 0);
            diag_clear_captured();
            diag_set_render_enabled(False);
            diag_register_source(test_file, "mod caller;\n");
            diag_set_file(test_file);
            stream = mode ? pp_process(source, file, NULL, 0) : NULL;
            if (mode) {
                CHECK(stream && parse_from_stream(stream) == NULL);
            } else {
                diag_set_file(file);
                CHECK(parse(source) == NULL);
            }
            check_format_diagnostic(file, loc);
            finish_case();
            memset(file, 'x', strlen(file));
            memset(source, 'x', strlen(source));
            free(file);
            free(source);
            diag_clear_sources();
            check_format_diagnostic("imported/format.sts", loc);
            diag_set_file(test_file);
        }
    }
}

static void test_format_macro_ownership(void) {
    case_name = "format macro trailing token uses owned definition-file diagnostic";
    for (int path = 0; path < 2; path++) {
        char *file = strdup("imported/format_macro.sts");
        char *source = strdup("mod imported.format_macro;\next macro let shared! = @'first\\\n\t\xc3\xa9{1 trailing}';\n");
        const char *caller = path ? "mod caller;\nfn main(void): void { print.(shared.!); }\n"
                                  : "mod caller;\nstack i8 *rw value = shared.!;\n";
        CHECK(file && source);
        src_loc_t loc = format_marker_loc(source, "trailing", 8);
        restore_diagnostic_counts(0, 0);
        diag_clear_captured();
        diag_set_render_enabled(False);
        pp_stream_t *definition = pp_process(source, file, NULL, 0);
        CHECK(definition && !get_error_count());
        pp_macro_set_t *imports[] = {pp_get_exports(definition)};
        CHECK(imports[0] && imports[0]->let_count == 1);
        stream = pp_process(caller, test_file, imports, 1);
        CHECK(stream && !get_error_count());
        boolean_t found = False;
        for (int i = 0; i < stream->count; i++) {
            const pp_token_t *pt = &stream->tokens[i];
            if (pt->tok.kind != TokStackStr) continue;
            CHECK(!found);
            found = True;
            CHECK(pt->tok.file == file && pt->tok.start != strstr(source, "'first"));
            CHECK(pt->tok.length == strlen("'first\\\n\t\xc3\xa9{1 trailing}'"));
            CHECK(memcmp(pt->tok.start, "'first\\\n\t\xc3\xa9{1 trailing}'", pt->tok.length) == 0);
            CHECK(pt->expansion && strcmp(pt->expansion->call_file, test_file) == 0);
        }
        CHECK(found && parse_from_stream(stream) == NULL);
        check_format_diagnostic(file, loc);
        CHECK(strcmp(diag_get_file(), test_file) == 0);
        finish_case();
        pp_stream_free(definition);
        memset(file, 'x', strlen(file));
        memset(source, 'x', strlen(source));
        free(file);
        free(source);
        diag_clear_sources();
        check_format_diagnostic("imported/format_macro.sts", loc);
        CHECK(strcmp(diag_get_file(), test_file) == 0);
    }
}

/* Generate resource-limit cases without checking enormous source fixtures in. */
static char *nested_source(const char *prefix, const char *open,
                           const char *middle, const char *close,
                           const char *suffix, size_t depth) {
    size_t n = strlen(prefix) + depth * (strlen(open) + strlen(close))
             + strlen(middle) + strlen(suffix) + 1;
    char *result = calloc(n, 1);
    CHECK(result);
    char *p = result;
#define APPEND(s) do { size_t len = strlen(s); memcpy(p, (s), len); p += len; } while (0)
    APPEND(prefix);
    for (size_t i = 0; i < depth; i++) APPEND(open);
    APPEND(middle);
    for (size_t i = 0; i < depth; i++) APPEND(close);
    APPEND(suffix);
#undef APPEND
    return result;
}

static void test_resource_limits(void) {
    const struct { const char *name, *prefix, *open, *middle, *close, *suffix; } cases[] = {
        {"parentheses", "mod t; stack u64 x = ", "(", "1", ")", ";"},
        {"unary", "mod t; stack u64 x = ", "! ", "1", "", ";"},
        {"assignment", "mod t; fn f(void): void { ", "x = ", "1", "", "; }"},
        {"ternary", "mod t; stack u64 x = ", "1 ? 1 : ", "1", "", ";"},
        {"slice types", "mod t; stack ", "[]", "u64", "", " x;"},
        {"blocks", "mod t; fn f(void): void ", "{", "ret;", "}", ""},
        {"else-if", "mod t; fn f(void): void { ", "if true {} else ", "if true {}", "", " }"},
        {"submodules", "mod t; ", "mod sub {", "stack i32 x;", "}", ""},
        {"compile-time else-if", "mod t; ", "@comptime if os == \"linux\" {} else ", "@comptime if os == \"linux\" {}", "", ""},
        {"zone expressions", "mod t; stack u64 x = ", "new.(1) in ", "z", "", ";"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *source = nested_source(cases[i].prefix, cases[i].open, cases[i].middle,
                                     cases[i].close, cases[i].suffix, 16);
        expect_accepted(cases[i].name, source);
        free(source);
        source = nested_source(cases[i].prefix, cases[i].open, cases[i].middle,
                               cases[i].close, cases[i].suffix, 1024);
        expect_rejected(cases[i].name, source, "parser nesting limit exceeded");
        free(source);
    }
    char *source = nested_source("", "/*", "nested", "*/", "mod t;", 8192);
    expect_accepted("iterative deep block comment", source);
    free(source);
    source = nested_source("mod t; stack u64 x = ", "0", "18446744073709551615", "", ";", 8192);
    case_name = "long zero-prefixed literal";
    for (int mode = 0; mode < 2; mode++) {
        node_t *m = parse_case(source, (boolean_t)mode);
        CHECK(m && !get_error_count());
        CHECK(m->as.module.decls.items[0]->as.var_decl.init->as.int_lit.value == UINT64_MAX);
        finish_case();
    }
    free(source);
    source = nested_source("mod t; stack u64 x = ", "9", "9", "", ";", 8192);
    expect_rejected("long overflowing literal", source, "exceeds maximum supported magnitude");
    free(source);
}

int main(void) {
    test_literals();
    test_comments();
    test_eof_locations();
    test_eof_file_ownership();
    test_literal_tokens();
    test_literal_values();
    test_unterminated_literal_locations();
    test_literal_continuation_diagnostics();
    test_literal_macro_ownership();
    test_format_trailing_tokens();
    test_format_valid_expressions();
    test_format_alphabetic_specs();
    test_format_canonical_templates();
    test_format_template_ownership();
    test_format_file_ownership();
    test_format_macro_ownership();
    test_discarded_lexer_errors();
    test_resource_limits();
    expect_accepted("typed variadic dispatch dotted syntax",
                    "mod t; fn f(...typed): void { stack va args; "
                    "va.foreach.(args) { |v| match v { i32 x => {} _ => {} } }; }");
    expect_accepted("variadic typed iteration bracket syntax",
                    "mod t; fn f(stack i32 n, ...): void { stack va args; "
                    "va.foreach.[i32](n, args) { |v| print.('{}', v); }; }");
    expect_rejected("stray top-level brace makes progress", "mod t; }",
                    "expected a type");
    expect_rejected("recovery after semicolon makes progress", "mod t; ; }",
                    "expected a type");
    expect_rejected("malformed alias recovery makes progress", "mod t; type S: s: { u64} };",
                    "expected ';'");
    expect_rejected("submodule recovery makes progress", "mod t; mod sub { type S: s: { u64} }; }",
                    "expected ';'");
    case_name = "tracked allocation cleanup";
    CHECK(scan_and_deallocate() == Ok);
    printf("PASS: %u frontend checks (legacy and preprocessed parser)\n", checks);
    return 0;
}
