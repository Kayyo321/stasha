/* Parser metadata survives both parser paths and shallow AST copies. */
#define CommonAllowStdlibAllocators
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser/parser.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n", __LINE__, #x); exit(1); } checks++; } while (0)
static unsigned checks;
int main(void) {
    const char *source =
        "mod meta;\n"
        "type I: interface { get(void): heap i32 *r; };\n"
        "type Box: @comptime[T.[I, Other]] struct { ext T x; };\n"
        "ext fn @comptime[T.[I]] id(stack T x): stack T *r;\n"
        "type Use: Box.[i32 *r, []u64, fn*(stack i8): heap i32 *r, Box.[i32]];\n"
        "type Literal: Box_G_ptr;\n"
        "fn caller(void): void { stack i32 n = id.[i32](3); "
        "stack i32 ctor = Box.[i32].(1); stack i32 err = id.[i32].?(1); "
        "stack i32 init = Box.[i32] { .x = 1 }; stack i32 meth = Box.[i32].new(1); "
        "stack i32 pipe = 1 |> id.[i32]; }\n"
        "type AnyArgs: any.[fn*(void): heap i32 *r, i32, u32, bool, i64, u64];\n"
        "type NullableArgs: Box.[?fn*(void): i32, ?closure.[void]: i32, ?[]i32];\n"
        "ext fn old_order(void): ?heap i32 *r;\n";
    for (int stream_mode = 0; stream_mode < 2; stream_mode++) {
        restore_diagnostic_counts(0, 0);
        diag_set_render_enabled(False);
        diag_register_source("metadata-source.sts", source);
        diag_set_file("metadata-source.sts"); diag_set_source(source);
        pp_stream_t *stream = stream_mode ? pp_process(source,"metadata-source.sts",NULL,0) : NULL;
        node_t *ast = stream_mode ? parse_from_stream(stream) : parse(source);
        if (!ast || get_error_count())
            for (usize_t i=0;i<diag_get_captured_count();i++)
                fprintf(stderr,"%s\n",diag_get_captured(i)->diag.message);
        CHECK(ast && !get_error_count());
        CHECK(ast->as.module.decls.count == 9);
        node_t **d = ast->as.module.decls.items;
        type_info_t ret = d[0]->as.type_decl.methods.items[0]->as.fn_decl.return_types[0];
        CHECK(ret.written_storage == StorageHeap && ret.ptr_perm == PtrRead);
        CHECK(ret.source_loc.line == 2 && ret.source_loc.col > 0);
        CHECK(!strcmp(ret.source_loc.file,"metadata-source.sts"));
        generic_use_t *constraints = d[1]->as.type_decl.type_param_meta[0].constraints;
        CHECK(constraints && constraints->count == 2);
        CHECK(!strcmp(constraints->args[0].user_name,"I"));
        CHECK(!strcmp(constraints->args[1].user_name,"Other"));
        CHECK(d[2]->as.fn_decl.type_param_meta[0].constraints->count == 1);
        CHECK(d[2]->as.fn_decl.return_types[0].written_storage == StorageStack);
        type_info_t type = d[3]->as.type_decl.alias_type;
        generic_use_t *use = type.generic_use;
        CHECK(use && use->count == 4 && !strcmp(use->base_name,"Box"));
        CHECK(use->source_loc.line == 5 && use->source_loc.col == 11);
        CHECK(use->args[0].is_pointer && use->args[0].ptr_perm == PtrRead);
        CHECK(use->args[1].base == TypeSlice && use->args[1].elem_type->base == TypeU64);
        CHECK(use->args[2].fn_ptr_desc->params[0].storage == StorageStack);
        CHECK(use->args[2].fn_ptr_desc->ret_type.written_storage == StorageHeap);
        CHECK(use->args[3].generic_use->args[0].base == TypeI32);
        CHECK(!d[4]->as.type_decl.alias_type.generic_use);
        node_t *init = d[5]->as.fn_decl.body->as.block.stmts.items[0]->as.var_decl.init;
        CHECK(init->kind == NodeCallExpr);
        CHECK(init->generic_use->args[0].base == TypeI32);
        node_list_t *stmts = &d[5]->as.fn_decl.body->as.block.stmts;
        CHECK(stmts->count == 6);
        node_kind_t kinds[] = {NodeCallExpr, NodeConstructorCall, NodeErrPropCall,
                              NodeDesigInit, NodeMethodCall, NodeCallExpr};
        for (usize_t i = 0; i < stmts->count; i++) {
            node_t *expr = stmts->items[i]->as.var_decl.init;
            CHECK(expr->kind == kinds[i]);
            generic_use_t *application = expr->kind == NodeMethodCall
                ? expr->as.method_call.object->generic_use : expr->generic_use;
            CHECK(application && application->args[0].base == TypeI32);
            CHECK(application->source_loc.file && application->source_loc.col > 0);
        }
        generic_use_t *any = d[6]->as.type_decl.alias_type.generic_use;
        CHECK(any && !strcmp(any->base_name,"any") && any->count == 6);
        CHECK(any->args[0].fn_ptr_desc->ret_type.written_storage == StorageHeap);
        generic_use_t *nullable = d[7]->as.type_decl.alias_type.generic_use;
        CHECK(nullable && nullable->count == 3);
        for (usize_t i=0;i<3;i++) CHECK(nullable->args[i].written_nullable);
        CHECK(d[8]->as.fn_decl.return_types[0].nullable);
        CHECK(d[8]->as.fn_decl.return_types[0].written_storage == StorageHeap);
        node_t copy = *d[3];
        CHECK(copy.as.type_decl.alias_type.generic_use == use);
        CHECK(!strcmp(copy.as.type_decl.alias_type.generic_use->base_name,"Box"));
        generic_use_t overflow = {0};
        overflow.count = overflow.capacity = SIZE_MAX / sizeof(type_info_t);
        overflow.source_loc = use->source_loc;
        CHECK(!ast_generic_arg_push(&overflow, NO_TYPE));
        CHECK(get_error_count() == 1);
        ast_free_all(); if (stream) pp_stream_free(stream);
    }
    printf("PASS: %u metadata checks\n",checks);
    return 0;
}
