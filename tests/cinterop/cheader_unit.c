/* C header import regression tests; no LLVM dependency. */
#define CommonAllowStdlibAllocators
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "cinterop/cheader.c"

static unsigned checks;
#define CHECK(test) do { checks++; if (!(test)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
    exit(1); \
} } while (0)

static void literal(const char *text, c_type_kind_t type, uint64_t magnitude,
                    boolean_t negative) {
    c_const_t result = {0};
    CHECK(ch_parse_macro_integer(text, &result));
    CHECK(!result.invalid);
    CHECK(result.type == type);
    CHECK(result.magnitude == magnitude);
    CHECK(result.negative == negative);
}

static void rejected_literal(const char *text) {
    c_const_t result = {0};
    CHECK(ch_parse_macro_integer(text, &result));
    CHECK(result.invalid);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    literal("-1", CTypeInt, 1, True);
    literal("(-2147483647)", CTypeInt, INT32_MAX, True);
    literal("0xffffffffU", CTypeUInt, UINT32_MAX, False);
    literal("-1U", CTypeUInt, UINT32_MAX, False);
    literal("18446744073709551615ULL", CTypeULongLong, UINT64_MAX, False);
    literal("0xffffffffffffffffULL", CTypeULongLong, UINT64_MAX, False);
    literal("(-1ULL)", CTypeULongLong, UINT64_MAX, False);
    literal("9223372036854775807LL", CTypeLongLong, INT64_MAX, False);
    literal("-9223372036854775807LL", CTypeLongLong, INT64_MAX, True);
    literal("0xffffffffffffffff", sizeof(long) == 8 ? CTypeULong : CTypeULongLong,
            UINT64_MAX, False);
    literal("4294967295", sizeof(long) == 8 ? CTypeLong : CTypeLongLong,
            UINT32_MAX, False);
    rejected_literal("18446744073709551616ULL");
    rejected_literal("0x10000000000000000ULL");
    rejected_literal("9223372036854775808LL");
    rejected_literal("18446744073709551615");
    const char *unsupported[] = {"(1 << 3)", "(1)+(2)", "1.5", "OTHER_MACRO", "0x", "1uul"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        c_const_t result = {0};
        CHECK(!ch_parse_macro_integer(unsupported[i], &result));
    }
    boolean_t ok = False;
    char min_value[64];
    snprintf(min_value, sizeof(min_value), "%ld", LONG_MIN);
    CHECK(ch_parse_int_literal(min_value, &ok) == LONG_MIN && ok);
    ch_parse_int_literal("18446744073709551616", &ok);
    CHECK(!ok);

    cheader_result_t result;
    char resolved[2048];
    CHECK(parse_cheader_file("support/parser_ownership.h", NULL, argv[1],
                            &result, resolved, sizeof(resolved)));
    CHECK(result.fn_count == 1);
    CHECK(result.struct_count == 1);
    CHECK(result.enum_count == 1);
    free_cheader_result(&result);
    CHECK(scan_and_deallocate() == Ok);

    CHECK(parse_cheader_file("support/parser_integer_types.h", NULL, argv[1],
                            &result, resolved, sizeof(resolved)));
    const c_type_kind_t kinds[] = {CTypeShort, CTypeUShort, CTypeLongLong,
        CTypeLong, CTypeUInt, CTypeULongLong, CTypeSChar};
    CHECK(result.fn_count == sizeof(kinds) / sizeof(kinds[0]));
    for (usize_t i = 0; i < result.fn_count; i++) {
        CHECK(result.fns[i].ret.kind == kinds[i]);
        CHECK(result.fns[i].param_count == 1);
        CHECK(result.fns[i].params[0].type.kind == kinds[i]);
    }
    free_cheader_result(&result);
    CHECK(scan_and_deallocate() == Ok);

    CHECK(parse_cheader_file("support/parser_spaced_include.h", NULL, argv[1],
                            &result, resolved, sizeof(resolved)));
    CHECK(result.fn_count == 7);
    CHECK(ch_result_has_const(&result, "UINT64_LIMIT"));
    free_cheader_result(&result);
    CHECK(scan_and_deallocate() == Ok);

#if defined(__linux__)
    CHECK(parse_cheader_file("errno.h", NULL, argv[1], &result, resolved, sizeof(resolved)));
    CHECK(ch_result_has_const(&result, "ENOENT"));
    CHECK(ch_result_has_const(&result, "EEXIST"));
    CHECK(ch_result_has_const(&result, "EINVAL"));
    free_cheader_result(&result);
    CHECK(scan_and_deallocate() == Ok);
#endif

    CHECK(parse_cheader_file("support/parser_callback_parameters.h", NULL, argv[1],
                            &result, resolved, sizeof(resolved)));
    CHECK(result.tdef_count == 8);
    CHECK(result.fn_count == 5);
    for (usize_t i = 2; i < 5; i++) {
        c_type_t *fp = &result.tdefs[i].actual;
        CHECK(fp->kind == CTypeFunctionPtr);
        CHECK(fp->fn_param_count == 1);
        CHECK(fp->fn_params[0].type.kind == CTypePointer);
        CHECK(fp->fn_params[0].type.elem->kind == CTypeInt);
    }
    CHECK(result.tdefs[5].actual.fn_params[0].type.unsupported_parameter);
    CHECK(result.tdefs[6].actual.fn_params[0].type.unsupported_parameter);
    CHECK(result.fns[0].params[0].type.kind == CTypePointer);
    CHECK(result.fns[1].params[0].type.unsupported_parameter);
    for (usize_t i = 2; i < 5; i++) {
        CHECK(result.fns[i].params[0].type.kind == CTypePointer);
        CHECK(result.fns[i].params[0].type.is_const);
        CHECK(result.fns[i].params[0].type.elem->is_const);
    }
    free_cheader_result(&result);
    CHECK(scan_and_deallocate() == Ok);

    CHECK(!parse_cheader_file("does-not-exist-regression.h", NULL, argv[1],
                             &result, resolved, sizeof(resolved)));
    CHECK(ch_active_result == NULL);
    CHECK(scan_and_deallocate() == Ok);

    CHECK(parse_cheader_file("string.h", NULL, argv[1], &result, resolved, sizeof(resolved)));
    CHECK(result.fn_count > 0);
    for (usize_t i = 0; i < result.fn_count; i++) CHECK(!result.fns[i].from_user_header);
    free_cheader_result(&result);
    CHECK(scan_and_deallocate() == Ok);
    printf("C header parser: %u checks passed\n", checks);
    return 0;
}
