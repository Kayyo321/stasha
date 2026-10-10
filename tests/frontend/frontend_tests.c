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
    test_discarded_lexer_errors();
    test_resource_limits();
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
