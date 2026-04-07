#define CommonAllowStdlibAllocators
#include "editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_puts_escaped(const char *text) {
    if (!text) return;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '\\': fputs("\\\\", stdout); break;
            case '"':  fputs("\\\"", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) fprintf(stdout, "\\u%04x", *p);
                else fputc(*p, stdout);
                break;
        }
    }
}

static void json_put_string(const char *text) {
    fputc('"', stdout);
    json_puts_escaped(text ? text : "");
    fputc('"', stdout);
}

char *editor_read_stdin(void) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return Null;

    for (;;) {
        size_t avail = cap - len;
        if (avail < 1024) {
            cap *= 2;
            char *next = realloc(buf, cap);
            if (!next) {
                free(buf);
                return Null;
            }
            buf = next;
            avail = cap - len;
        }

        size_t n = fread(buf + len, 1, avail - 1, stdin);
        len += n;
        if (n == 0) break;
    }

    buf[len] = '\0';
    return buf;
}

void editor_free_buffer(char *buffer) {
    free(buffer);
}

static const char *diag_level_name(diag_level_t level) {
    switch (level) {
        case DiagError: return "error";
        case DiagWarning: return "warning";
        case DiagNote: return "note";
        case DiagHelp: return "help";
        default: return "error";
    }
}

void editor_print_diagnostics_json(void) {
    printf("{\"diagnostics\":[");
    for (usize_t i = 0; i < diag_get_captured_count(); i++) {
        const captured_diag_t *cap = diag_get_captured(i);
        const diagnostic_t *diag = &cap->diag;
        src_loc_t loc = NO_LOC;
        for (usize_t j = 0; j < diag->label_count; j++) {
            if (diag->labels[j].primary || j == 0) {
                loc = diag->labels[j].loc;
                if (diag->labels[j].primary) break;
            }
        }

        if (i > 0) printf(",");
        printf("{\"severity\":");
        json_put_string(diag_level_name(diag->level));
        printf(",\"message\":");
        json_put_string(diag->message);
        printf(",\"file\":");
        json_put_string(cap->filename);
        printf(",\"line\":%lu,\"col\":%lu,\"len\":%lu",
               (unsigned long)(loc.line > 0 ? loc.line - 1 : 0),
               (unsigned long)(loc.col > 0 ? loc.col - 1 : 0),
               (unsigned long)(loc.len > 0 ? loc.len : 1));

        printf(",\"labels\":[");
        for (usize_t j = 0; j < diag->label_count; j++) {
            const diag_label_t *lbl = &diag->labels[j];
            if (j > 0) printf(",");
            printf("{\"message\":");
            json_put_string(lbl->text);
            printf(",\"primary\":%s,\"line\":%lu,\"col\":%lu,\"len\":%lu}",
                   lbl->primary ? "true" : "false",
                   (unsigned long)(lbl->loc.line > 0 ? lbl->loc.line - 1 : 0),
                   (unsigned long)(lbl->loc.col > 0 ? lbl->loc.col - 1 : 0),
                   (unsigned long)(lbl->loc.len > 0 ? lbl->loc.len : 1));
        }
        printf("],\"notes\":[");
        for (usize_t j = 0; j < diag->note_count; j++) {
            if (j > 0) printf(",");
            printf("{\"kind\":");
            json_put_string(diag_level_name(diag->note_kinds[j]));
            printf(",\"message\":");
            json_put_string(diag->notes[j]);
            printf("}");
        }
        printf("]}");
    }
    printf("]}\n");
}

static const char *token_type_name(token_kind_t kind) {
    switch (kind) {
        case TokIntLit:
        case TokFloatLit:
            return "number";
        case TokStackStr:
        case TokHeapStr:
        case TokCharLit:
            return "string";
        case TokMod:
        case TokImp:
        case TokInt:
        case TokExt:
        case TokFn:
        case TokFor:
        case TokIf:
        case TokElse:
        case TokWhile:
        case TokDo:
        case TokInf:
        case TokRet:
        case TokBreak:
        case TokContinue:
        case TokStack:
        case TokHeap:
        case TokAtomic:
        case TokConst:
        case TokFinal:
        case TokThread:
        case TokFuture:
        case TokPrint:
        case TokVoid:
        case TokTrue:
        case TokFalse:
        case TokType:
        case TokStruct:
        case TokEnum:
        case TokLib:
        case TokFrom:
        case TokNew:
        case TokRem:
        case TokSizeof:
        case TokMatch:
        case TokDefer:
        case TokNil:
        case TokMov:
        case TokErrorType:
        case TokTest:
        case TokExpect:
        case TokExpectEq:
        case TokExpectNeq:
        case TokTestFail:
        case TokSwitch:
        case TokCase:
        case TokDefault:
        case TokUnion:
        case TokVolatile:
        case TokAsm:
        case TokTls:
        case TokRestrict:
        case TokComptimeAssert:
        case TokComptimeIf:
        case TokLet:
        case TokLibImp:
        case TokStd:
        case TokHash:
        case TokEqu:
        case TokThis:
        case TokWith:
        case TokAny:
        case TokInterface:
        case TokMacro:
            return "keyword";
        case TokI8:
        case TokI16:
        case TokI32:
        case TokI64:
        case TokU8:
        case TokU16:
        case TokU32:
        case TokU64:
        case TokF32:
        case TokF64:
        case TokBool:
            return "type";
        case TokIdent:
            return "identifier";
        case TokLParen:
        case TokRParen:
        case TokLBrace:
        case TokRBrace:
        case TokLBracket:
        case TokRBracket:
        case TokSemicolon:
        case TokColon:
        case TokComma:
        case TokDot:
        case TokDotDot:
        case TokDotDotEq:
        case TokQuestion:
        case TokFatArrow:
        case TokDotDotDot:
        case TokAt:
            return "delimiter";
        case TokEof:
            return "eof";
        case TokError:
            return "error";
        default:
            return "operator";
    }
}

void editor_print_tokens_json(const char *source, const char *path) {
    lexer_t lex;
    init_lexer(&lex, source);

    printf("{\"path\":");
    json_put_string(path);
    printf(",\"tokens\":[");

    boolean_t first = True;
    for (;;) {
        token_t tok = next_token(&lex);
        if (tok.kind == TokEof) break;

        if (!first) printf(",");
        first = False;

        printf("{\"line\":%lu,\"col\":%lu,\"len\":%lu,\"kind\":",
               (unsigned long)(tok.line > 0 ? tok.line - 1 : 0),
               (unsigned long)(tok.col > 0 ? tok.col - 1 : 0),
               (unsigned long)tok.length);
        json_put_string(token_type_name(tok.kind));
        printf(",\"text\":");
        printf("\"");
        for (usize_t i = 0; i < tok.length; i++) {
            unsigned char c = (unsigned char)tok.start[i];
            switch (c) {
                case '\\': fputs("\\\\", stdout); break;
                case '"':  fputs("\\\"", stdout); break;
                case '\n': fputs("\\n", stdout); break;
                case '\r': fputs("\\r", stdout); break;
                case '\t': fputs("\\t", stdout); break;
                default:
                    if (c < 0x20) fprintf(stdout, "\\u%04x", c);
                    else fputc(c, stdout);
                    break;
            }
        }
        printf("\"}");
    }

    printf("]}\n");
}

static const char *symbol_kind_name(const node_t *node) {
    switch (node->kind) {
        case NodeFnDecl: return node->as.fn_decl.is_method ? "method" : "function";
        case NodeVarDecl: return "variable";
        case NodeTypeDecl:
            switch (node->as.type_decl.decl_kind) {
                case TypeDeclEnum: return "enum";
                case TypeDeclAlias: return "type";
                case TypeDeclUnion: return "struct";
                case TypeDeclInterface: return "interface";
                default: return "struct";
            }
        case NodeEnumVariant: return "enumMember";
        default: return "symbol";
    }
}

static const char *symbol_name(const node_t *node) {
    switch (node->kind) {
        case NodeFnDecl: return node->as.fn_decl.name;
        case NodeVarDecl: return node->as.var_decl.name;
        case NodeTypeDecl: return node->as.type_decl.name;
        case NodeEnumVariant: return node->as.enum_variant.name;
        default: return "";
    }
}

static void print_symbol_entry(const node_t *node, const char *path, const char *container, boolean_t *first) {
    if (!node) return;
    if (!symbol_name(node) || symbol_name(node)[0] == '\0') return;

    if (!*first) printf(",");
    *first = False;

    printf("{\"name\":");
    json_put_string(symbol_name(node));
    printf(",\"kind\":");
    json_put_string(symbol_kind_name(node));
    printf(",\"container\":");
    json_put_string(container ? container : "");
    printf(",\"path\":");
    json_put_string(path);
    printf(",\"line\":%lu,\"col\":%lu",
           (unsigned long)(node->line > 0 ? node->line - 1 : 0),
           (unsigned long)(node->col > 0 ? node->col - 1 : 0));

    if (node->kind == NodeFnDecl && node->as.fn_decl.struct_name) {
        printf(",\"detail\":");
        json_put_string(node->as.fn_decl.struct_name);
    } else {
        printf(",\"detail\":\"\"");
    }

    printf("}");
}

void editor_print_symbols_json(const node_t *ast, const char *path) {
    printf("{\"path\":");
    json_put_string(path);
    printf(",\"symbols\":[");

    boolean_t first = True;
    if (ast && ast->kind == NodeModule) {
        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            const node_t *decl = ast->as.module.decls.items[i];
            switch (decl->kind) {
                case NodeFnDecl:
                case NodeVarDecl:
                case NodeTypeDecl:
                    print_symbol_entry(decl, path, "", &first);
                    break;
                default:
                    break;
            }

            if (decl->kind == NodeTypeDecl) {
                const char *container = decl->as.type_decl.name;
                for (usize_t j = 0; j < decl->as.type_decl.fields.count; j++)
                    print_symbol_entry(decl->as.type_decl.fields.items[j], path, container, &first);
                for (usize_t j = 0; j < decl->as.type_decl.methods.count; j++)
                    print_symbol_entry(decl->as.type_decl.methods.items[j], path, container, &first);
                for (usize_t j = 0; j < decl->as.type_decl.variants.count; j++)
                    print_symbol_entry(decl->as.type_decl.variants.items[j], path, container, &first);
            }
        }
    }

    printf("]}\n");
}

typedef struct {
    const node_t *decl;
} binding_entry_t;

typedef struct {
    binding_entry_t *items;
    size_t count;
    size_t cap;
} binding_list_t;

typedef struct {
    const node_t *ast;
    const node_t *current_type;
    binding_list_t scopes[64];
    size_t scope_count;
    usize_t target_line;
    usize_t target_col;
    const node_t *found;
} resolve_ctx_t;

static void binding_list_push(binding_list_t *list, const node_t *decl) {
    if (!decl) return;
    if (list->count >= list->cap) {
        size_t next_cap = list->cap ? list->cap * 2 : 8;
        binding_entry_t *next = realloc(list->items, next_cap * sizeof(binding_entry_t));
        if (!next) return;
        list->items = next;
        list->cap = next_cap;
    }
    list->items[list->count++].decl = decl;
}

static void scope_push(resolve_ctx_t *ctx) {
    if (ctx->scope_count >= 64) return;
    ctx->scopes[ctx->scope_count].items = Null;
    ctx->scopes[ctx->scope_count].count = 0;
    ctx->scopes[ctx->scope_count].cap = 0;
    ctx->scope_count++;
}

static void scope_pop(resolve_ctx_t *ctx) {
    if (ctx->scope_count == 0) return;
    binding_list_t *list = &ctx->scopes[ctx->scope_count - 1];
    if (list->items) free(list->items);
    list->items = Null;
    list->count = 0;
    list->cap = 0;
    ctx->scope_count--;
}

static void scope_add(resolve_ctx_t *ctx, const node_t *decl) {
    if (!decl || ctx->scope_count == 0) return;
    binding_list_push(&ctx->scopes[ctx->scope_count - 1], decl);
}

static const char *decl_name_for_lookup(const node_t *decl) {
    if (!decl) return Null;
    switch (decl->kind) {
        case NodeFnDecl: return decl->as.fn_decl.name;
        case NodeVarDecl: return decl->as.var_decl.name;
        case NodeTypeDecl: return decl->as.type_decl.name;
        case NodeEnumVariant: return decl->as.enum_variant.name;
        default: return Null;
    }
}

static const node_t *scope_lookup(resolve_ctx_t *ctx, const char *name) {
    if (!name) return Null;
    for (size_t si = ctx->scope_count; si > 0; si--) {
        binding_list_t *list = &ctx->scopes[si - 1];
        for (size_t i = list->count; i > 0; i--) {
            const node_t *decl = list->items[i - 1].decl;
            const char *decl_name = decl_name_for_lookup(decl);
            if (decl_name && strcmp(decl_name, name) == 0)
                return decl;
        }
    }
    return Null;
}

static const node_t *find_global_decl(const node_t *ast, const char *name, node_kind_t kind) {
    if (!ast || ast->kind != NodeModule || !name) return Null;
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        const node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != kind) continue;
        const char *decl_name = decl_name_for_lookup(decl);
        if (decl_name && strcmp(decl_name, name) == 0)
            return decl;
    }
    return Null;
}

static const node_t *find_type_decl(const node_t *ast, const char *name) {
    return find_global_decl(ast, name, NodeTypeDecl);
}

static const node_t *find_field_decl(const node_t *type_decl, const char *name) {
    if (!type_decl || type_decl->kind != NodeTypeDecl || !name) return Null;
    for (usize_t i = 0; i < type_decl->as.type_decl.fields.count; i++) {
        const node_t *field = type_decl->as.type_decl.fields.items[i];
        if (field->kind == NodeVarDecl && field->as.var_decl.name
            && strcmp(field->as.var_decl.name, name) == 0)
            return field;
    }
    return Null;
}

static const node_t *find_method_decl(const node_t *type_decl, const char *name) {
    if (!type_decl || type_decl->kind != NodeTypeDecl || !name) return Null;
    for (usize_t i = 0; i < type_decl->as.type_decl.methods.count; i++) {
        const node_t *method = type_decl->as.type_decl.methods.items[i];
        if (method->kind == NodeFnDecl && method->as.fn_decl.name
            && strcmp(method->as.fn_decl.name, name) == 0)
            return method;
    }
    return Null;
}

static boolean_t type_is_known(type_info_t ti) {
    return ti.base != TypeVoid || ti.is_pointer || ti.user_name || ti.fn_ptr_desc;
}

static type_info_t infer_expr_type(resolve_ctx_t *ctx, const node_t *expr);

static type_info_t infer_var_decl_type(resolve_ctx_t *ctx, const node_t *decl) {
    if (!decl || decl->kind != NodeVarDecl) return NO_TYPE;
    if (type_is_known(decl->as.var_decl.type))
        return decl->as.var_decl.type;
    if (decl->as.var_decl.init)
        return infer_expr_type(ctx, decl->as.var_decl.init);
    return NO_TYPE;
}

static const node_t *resolve_named_decl(resolve_ctx_t *ctx, const char *name) {
    const node_t *decl = scope_lookup(ctx, name);
    if (decl) return decl;
    decl = find_global_decl(ctx->ast, name, NodeVarDecl);
    if (decl) return decl;
    decl = find_global_decl(ctx->ast, name, NodeFnDecl);
    if (decl) return decl;
    decl = find_global_decl(ctx->ast, name, NodeTypeDecl);
    if (decl) return decl;
    return find_global_decl(ctx->ast, name, NodeEnumVariant);
}

static type_info_t infer_expr_type(resolve_ctx_t *ctx, const node_t *expr) {
    if (!expr) return NO_TYPE;
    switch (expr->kind) {
        case NodeIdentExpr: {
            const node_t *decl = resolve_named_decl(ctx, expr->as.ident.name);
            if (!decl) return NO_TYPE;
            if (decl->kind == NodeVarDecl) return infer_var_decl_type(ctx, decl);
            if (decl->kind == NodeTypeDecl) {
                type_info_t ti = NO_TYPE;
                ti.base = TypeUser;
                ti.user_name = decl->as.type_decl.name;
                return ti;
            }
            if (decl->kind == NodeEnumVariant) {
                type_info_t ti = NO_TYPE;
                ti.base = TypeUser;
                return ti;
            }
            return NO_TYPE;
        }
        case NodeIntLitExpr:   return (type_info_t){.base=TypeI64,  .is_pointer=False, .ptr_perm=PtrNone};
        case NodeFloatLitExpr: return (type_info_t){.base=TypeF64,  .is_pointer=False, .ptr_perm=PtrNone};
        case NodeBoolLitExpr:  return (type_info_t){.base=TypeBool, .is_pointer=False, .ptr_perm=PtrNone};
        case NodeStrLitExpr:
        case NodeCharLitExpr:
            return NO_TYPE;
        case NodeCallExpr: {
            const node_t *decl = resolve_named_decl(ctx, expr->as.call.callee);
            if (decl && decl->kind == NodeFnDecl && decl->as.fn_decl.return_count > 0)
                return decl->as.fn_decl.return_types[0];
            return NO_TYPE;
        }
        case NodeMethodCall: {
            type_info_t obj = infer_expr_type(ctx, expr->as.method_call.object);
            if (obj.base == TypeUser && obj.user_name) {
                const node_t *type_decl = find_type_decl(ctx->ast, obj.user_name);
                const node_t *method = find_method_decl(type_decl, expr->as.method_call.method);
                if (method && method->as.fn_decl.return_count > 0)
                    return method->as.fn_decl.return_types[0];
            }
            return NO_TYPE;
        }
        case NodeSelfMethodCall: {
            const char *type_name = expr->as.self_method_call.type_name;
            if (!type_name && ctx->current_type)
                type_name = ctx->current_type->as.type_decl.name;
            const node_t *type_decl = type_name ? find_type_decl(ctx->ast, type_name) : ctx->current_type;
            const node_t *method = find_method_decl(type_decl, expr->as.self_method_call.method);
            if (method && method->as.fn_decl.return_count > 0)
                return method->as.fn_decl.return_types[0];
            return NO_TYPE;
        }
        case NodeMemberExpr: {
            type_info_t obj = infer_expr_type(ctx, expr->as.member_expr.object);
            if (obj.base == TypeUser && obj.user_name) {
                const node_t *type_decl = find_type_decl(ctx->ast, obj.user_name);
                const node_t *field = find_field_decl(type_decl, expr->as.member_expr.field);
                if (field) return infer_var_decl_type(ctx, field);
            }
            return NO_TYPE;
        }
        case NodeSelfMemberExpr: {
            const char *type_name = expr->as.self_member.type_name;
            if (!type_name && ctx->current_type)
                type_name = ctx->current_type->as.type_decl.name;
            const node_t *type_decl = type_name ? find_type_decl(ctx->ast, type_name) : ctx->current_type;
            const node_t *field = find_field_decl(type_decl, expr->as.self_member.field);
            if (field) return infer_var_decl_type(ctx, field);
            return NO_TYPE;
        }
        case NodeConstructorCall: {
            type_info_t ti = NO_TYPE;
            ti.base = TypeUser;
            ti.user_name = expr->as.ctor_call.type_name;
            return ti;
        }
        case NodeCastExpr:
            return expr->as.cast_expr.target;
        default:
            return NO_TYPE;
    }
}

static boolean_t hit_name(const node_t *node, const char *name, usize_t line, usize_t col) {
    if (!node || !name || !node->line || !node->col) return False;
    if (node->line != line) return False;
    usize_t start = node->col;
    usize_t end = start + strlen(name);
    return col >= start && col < end;
}

static boolean_t maybe_set_found(resolve_ctx_t *ctx, const node_t *decl) {
    if (decl) {
        ctx->found = decl;
        return True;
    }
    return False;
}

static boolean_t resolve_in_expr(resolve_ctx_t *ctx, const node_t *expr);
static boolean_t resolve_in_stmt(resolve_ctx_t *ctx, const node_t *stmt);

static boolean_t resolve_in_block(resolve_ctx_t *ctx, const node_t *block) {
    if (!block || block->kind != NodeBlock) return False;
    scope_push(ctx);
    for (usize_t i = 0; i < block->as.block.stmts.count; i++) {
        const node_t *stmt = block->as.block.stmts.items[i];
        if (!stmt) continue;
        if (stmt->kind == NodeVarDecl) {
            if (stmt->as.var_decl.init && resolve_in_expr(ctx, stmt->as.var_decl.init)) {
                scope_pop(ctx);
                return True;
            }
            if (hit_name(stmt, stmt->as.var_decl.name, ctx->target_line, ctx->target_col)) {
                ctx->found = stmt;
                scope_pop(ctx);
                return True;
            }
            scope_add(ctx, stmt);
            continue;
        }
        if (stmt->kind == NodeMultiAssign) {
            for (usize_t j = 0; j < stmt->as.multi_assign.values.count; j++) {
                if (resolve_in_expr(ctx, stmt->as.multi_assign.values.items[j])) {
                    scope_pop(ctx);
                    return True;
                }
            }
            for (usize_t j = 0; j < stmt->as.multi_assign.targets.count; j++) {
                const node_t *target = stmt->as.multi_assign.targets.items[j];
                if (hit_name(target, target->as.var_decl.name, ctx->target_line, ctx->target_col)) {
                    ctx->found = target;
                    scope_pop(ctx);
                    return True;
                }
                scope_add(ctx, target);
            }
            continue;
        }
        if (resolve_in_stmt(ctx, stmt)) {
            scope_pop(ctx);
            return True;
        }
    }
    scope_pop(ctx);
    return False;
}

static boolean_t resolve_in_stmt(resolve_ctx_t *ctx, const node_t *stmt) {
    if (!stmt) return False;
    switch (stmt->kind) {
        case NodeBlock:
            return resolve_in_block(ctx, stmt);
        case NodeExprStmt:
            return resolve_in_expr(ctx, stmt->as.expr_stmt.expr);
        case NodeRetStmt:
            for (usize_t i = 0; i < stmt->as.ret_stmt.values.count; i++)
                if (resolve_in_expr(ctx, stmt->as.ret_stmt.values.items[i])) return True;
            return False;
        case NodeIfStmt:
            return resolve_in_expr(ctx, stmt->as.if_stmt.cond)
                || resolve_in_stmt(ctx, stmt->as.if_stmt.then_block)
                || resolve_in_stmt(ctx, stmt->as.if_stmt.else_block);
        case NodeForStmt:
            return resolve_in_stmt(ctx, stmt->as.for_stmt.init)
                || resolve_in_expr(ctx, stmt->as.for_stmt.cond)
                || resolve_in_expr(ctx, stmt->as.for_stmt.update)
                || resolve_in_stmt(ctx, stmt->as.for_stmt.body);
        case NodeWhileStmt:
            return resolve_in_expr(ctx, stmt->as.while_stmt.cond)
                || resolve_in_stmt(ctx, stmt->as.while_stmt.body);
        case NodeDoWhileStmt:
            return resolve_in_stmt(ctx, stmt->as.do_while_stmt.body)
                || resolve_in_expr(ctx, stmt->as.do_while_stmt.cond);
        case NodePrintStmt:
            for (usize_t i = 0; i < stmt->as.print_stmt.args.count; i++)
                if (resolve_in_expr(ctx, stmt->as.print_stmt.args.items[i])) return True;
            return False;
        case NodeWithStmt:
            return resolve_in_stmt(ctx, stmt->as.with_stmt.decl)
                || resolve_in_expr(ctx, stmt->as.with_stmt.cond)
                || resolve_in_stmt(ctx, stmt->as.with_stmt.body)
                || resolve_in_stmt(ctx, stmt->as.with_stmt.else_block);
        case NodeSwitchStmt:
            if (resolve_in_expr(ctx, stmt->as.switch_stmt.expr)) return True;
            for (usize_t i = 0; i < stmt->as.switch_stmt.cases.count; i++)
                if (resolve_in_stmt(ctx, stmt->as.switch_stmt.cases.items[i])) return True;
            return False;
        case NodeSwitchCase:
            for (usize_t i = 0; i < stmt->as.switch_case.values.count; i++)
                if (resolve_in_expr(ctx, stmt->as.switch_case.values.items[i])) return True;
            return resolve_in_stmt(ctx, stmt->as.switch_case.body);
        case NodeMatchStmt:
            if (resolve_in_expr(ctx, stmt->as.match_stmt.expr)) return True;
            for (usize_t i = 0; i < stmt->as.match_stmt.arms.count; i++)
                if (resolve_in_stmt(ctx, stmt->as.match_stmt.arms.items[i])) return True;
            return False;
        case NodeMatchArm:
            return resolve_in_stmt(ctx, stmt->as.match_arm.body);
        case NodeDeferStmt:
            return resolve_in_stmt(ctx, stmt->as.defer_stmt.body);
        case NodeVarDecl:
            if (stmt->as.var_decl.init) return resolve_in_expr(ctx, stmt->as.var_decl.init);
            return False;
        default:
            return False;
    }
}

static boolean_t resolve_in_expr(resolve_ctx_t *ctx, const node_t *expr) {
    if (!expr) return False;
    switch (expr->kind) {
        case NodeIdentExpr:
            if (hit_name(expr, expr->as.ident.name, ctx->target_line, ctx->target_col))
                return maybe_set_found(ctx, resolve_named_decl(ctx, expr->as.ident.name));
            return False;
        case NodeCallExpr:
            if (hit_name(expr, expr->as.call.callee, ctx->target_line, ctx->target_col))
                return maybe_set_found(ctx, resolve_named_decl(ctx, expr->as.call.callee));
            for (usize_t i = 0; i < expr->as.call.args.count; i++)
                if (resolve_in_expr(ctx, expr->as.call.args.items[i])) return True;
            return False;
        case NodeMethodCall: {
            if (hit_name(expr, expr->as.method_call.method, ctx->target_line, ctx->target_col)) {
                type_info_t obj = infer_expr_type(ctx, expr->as.method_call.object);
                const node_t *type_decl = (obj.base == TypeUser && obj.user_name)
                    ? find_type_decl(ctx->ast, obj.user_name) : Null;
                return maybe_set_found(ctx, find_method_decl(type_decl, expr->as.method_call.method));
            }
            if (resolve_in_expr(ctx, expr->as.method_call.object)) return True;
            for (usize_t i = 0; i < expr->as.method_call.args.count; i++)
                if (resolve_in_expr(ctx, expr->as.method_call.args.items[i])) return True;
            return False;
        }
        case NodeMemberExpr: {
            if (hit_name(expr, expr->as.member_expr.field, ctx->target_line, ctx->target_col)) {
                type_info_t obj = infer_expr_type(ctx, expr->as.member_expr.object);
                const node_t *type_decl = (obj.base == TypeUser && obj.user_name)
                    ? find_type_decl(ctx->ast, obj.user_name) : Null;
                return maybe_set_found(ctx, find_field_decl(type_decl, expr->as.member_expr.field));
            }
            return resolve_in_expr(ctx, expr->as.member_expr.object);
        }
        case NodeSelfMemberExpr: {
            if (hit_name(expr, expr->as.self_member.field, ctx->target_line, ctx->target_col)) {
                const char *type_name = expr->as.self_member.type_name
                    ? expr->as.self_member.type_name
                    : (ctx->current_type ? ctx->current_type->as.type_decl.name : Null);
                const node_t *type_decl = type_name ? find_type_decl(ctx->ast, type_name) : ctx->current_type;
                return maybe_set_found(ctx, find_field_decl(type_decl, expr->as.self_member.field));
            }
            return False;
        }
        case NodeSelfMethodCall: {
            if (hit_name(expr, expr->as.self_method_call.method, ctx->target_line, ctx->target_col)) {
                const char *type_name = expr->as.self_method_call.type_name
                    ? expr->as.self_method_call.type_name
                    : (ctx->current_type ? ctx->current_type->as.type_decl.name : Null);
                const node_t *type_decl = type_name ? find_type_decl(ctx->ast, type_name) : ctx->current_type;
                return maybe_set_found(ctx, find_method_decl(type_decl, expr->as.self_method_call.method));
            }
            for (usize_t i = 0; i < expr->as.self_method_call.args.count; i++)
                if (resolve_in_expr(ctx, expr->as.self_method_call.args.items[i])) return True;
            return False;
        }
        case NodeConstructorCall:
            if (hit_name(expr, expr->as.ctor_call.type_name, ctx->target_line, ctx->target_col))
                return maybe_set_found(ctx, find_type_decl(ctx->ast, expr->as.ctor_call.type_name));
            for (usize_t i = 0; i < expr->as.ctor_call.args.count; i++)
                if (resolve_in_expr(ctx, expr->as.ctor_call.args.items[i])) return True;
            return False;
        case NodeErrPropCall:
            if (hit_name(expr, expr->as.err_prop_call.callee, ctx->target_line, ctx->target_col))
                return maybe_set_found(ctx, resolve_named_decl(ctx, expr->as.err_prop_call.callee));
            for (usize_t i = 0; i < expr->as.err_prop_call.args.count; i++)
                if (resolve_in_expr(ctx, expr->as.err_prop_call.args.items[i])) return True;
            return False;
        case NodeAssignExpr:
            return resolve_in_expr(ctx, expr->as.assign.target)
                || resolve_in_expr(ctx, expr->as.assign.value);
        case NodeCompoundAssign:
            return resolve_in_expr(ctx, expr->as.compound_assign.target)
                || resolve_in_expr(ctx, expr->as.compound_assign.value);
        case NodeBinaryExpr:
            return resolve_in_expr(ctx, expr->as.binary.left)
                || resolve_in_expr(ctx, expr->as.binary.right);
        case NodeUnaryPrefixExpr:
        case NodeUnaryPostfixExpr:
            return resolve_in_expr(ctx, expr->as.unary.operand);
        case NodeIndexExpr:
            return resolve_in_expr(ctx, expr->as.index_expr.object)
                || resolve_in_expr(ctx, expr->as.index_expr.index);
        case NodeTernaryExpr:
            return resolve_in_expr(ctx, expr->as.ternary.cond)
                || resolve_in_expr(ctx, expr->as.ternary.then_expr)
                || resolve_in_expr(ctx, expr->as.ternary.else_expr);
        case NodeCastExpr:
            return resolve_in_expr(ctx, expr->as.cast_expr.expr);
        case NodeAddrOf:
            return resolve_in_expr(ctx, expr->as.addr_of.operand);
        case NodeMovExpr:
            return resolve_in_expr(ctx, expr->as.mov_expr.ptr)
                || resolve_in_expr(ctx, expr->as.mov_expr.size);
        case NodeAnyTypeExpr:
            return resolve_in_expr(ctx, expr->as.any_type_expr.operand);
        case NodeHashExpr:
            return resolve_in_expr(ctx, expr->as.hash_expr.operand);
        case NodeEquExpr:
            return resolve_in_expr(ctx, expr->as.equ_expr.left)
                || resolve_in_expr(ctx, expr->as.equ_expr.right);
        case NodeNewExpr:
            return resolve_in_expr(ctx, expr->as.new_expr.size);
        case NodeCompoundInit:
            for (usize_t i = 0; i < expr->as.compound_init.items.count; i++)
                if (resolve_in_expr(ctx, expr->as.compound_init.items.items[i])) return True;
            return False;
        case NodeInitField:
            return resolve_in_expr(ctx, expr->as.init_field.value);
        case NodeInitIndex:
            return resolve_in_expr(ctx, expr->as.init_index.index)
                || resolve_in_expr(ctx, expr->as.init_index.value);
        case NodeDesigInit:
            for (usize_t i = 0; i < expr->as.desig_init.values.count; i++)
                if (resolve_in_expr(ctx, expr->as.desig_init.values.items[i])) return True;
            return False;
        default:
            return False;
    }
}

static const node_t *resolve_definition_node(const node_t *ast, usize_t line, usize_t col) {
    resolve_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ast = ast;
    ctx.target_line = line;
    ctx.target_col = col;

    scope_push(&ctx);
    if (ast && ast->kind == NodeModule) {
        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            const node_t *decl = ast->as.module.decls.items[i];
            if (decl->kind == NodeFnDecl || decl->kind == NodeVarDecl
                || decl->kind == NodeTypeDecl || decl->kind == NodeEnumVariant)
                scope_add(&ctx, decl);
        }

        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            const node_t *decl = ast->as.module.decls.items[i];
            if (!decl) continue;

            if ((decl->kind == NodeFnDecl || decl->kind == NodeVarDecl || decl->kind == NodeTypeDecl
                    || decl->kind == NodeEnumVariant)
                && hit_name(decl, decl_name_for_lookup(decl), line, col)) {
                ctx.found = decl;
                break;
            }

            if (decl->kind == NodeFnDecl) {
                scope_push(&ctx);
                for (usize_t j = 0; j < decl->as.fn_decl.params.count; j++)
                    scope_add(&ctx, decl->as.fn_decl.params.items[j]);
                if (decl->as.fn_decl.is_method) {
                    const node_t fake_this = { .kind = NodeVarDecl };
                    (void)fake_this;
                }
                if (resolve_in_stmt(&ctx, decl->as.fn_decl.body)) {
                    scope_pop(&ctx);
                    break;
                }
                scope_pop(&ctx);
            } else if (decl->kind == NodeVarDecl) {
                if (decl->as.var_decl.init && resolve_in_expr(&ctx, decl->as.var_decl.init))
                    break;
            } else if (decl->kind == NodeTypeDecl) {
                const node_t *saved_type = ctx.current_type;
                ctx.current_type = decl;
                for (usize_t j = 0; j < decl->as.type_decl.fields.count; j++) {
                    const node_t *field = decl->as.type_decl.fields.items[j];
                    if (hit_name(field, field->as.var_decl.name, line, col)) {
                        ctx.found = field;
                        break;
                    }
                }
                for (usize_t j = 0; !ctx.found && j < decl->as.type_decl.methods.count; j++) {
                    const node_t *method = decl->as.type_decl.methods.items[j];
                    if (hit_name(method, method->as.fn_decl.name, line, col)) {
                        ctx.found = method;
                        break;
                    }
                    scope_push(&ctx);
                    for (usize_t k = 0; k < method->as.fn_decl.params.count; k++)
                        scope_add(&ctx, method->as.fn_decl.params.items[k]);
                    if (resolve_in_stmt(&ctx, method->as.fn_decl.body)) {
                        scope_pop(&ctx);
                        break;
                    }
                    scope_pop(&ctx);
                }
                for (usize_t j = 0; !ctx.found && j < decl->as.type_decl.variants.count; j++) {
                    const node_t *variant = decl->as.type_decl.variants.items[j];
                    if (hit_name(variant, variant->as.enum_variant.name, line, col)) {
                        ctx.found = variant;
                        break;
                    }
                }
                ctx.current_type = saved_type;
                if (ctx.found) break;
            }
        }
    }
    scope_pop(&ctx);
    return ctx.found;
}

void editor_print_definition_json(const node_t *ast, const char *path, usize_t line, usize_t col) {
    const node_t *def = resolve_definition_node(ast, line + 1, col + 1);
    printf("{\"definition\":");
    if (!def) {
        printf("null}\n");
        return;
    }
    printf("{\"path\":");
    json_put_string(def->source_file ? def->source_file : path);
    printf(",\"line\":%lu,\"col\":%lu,\"name\":",
           (unsigned long)(def->line > 0 ? def->line - 1 : 0),
           (unsigned long)(def->col > 0 ? def->col - 1 : 0));
    json_put_string(decl_name_for_lookup(def));
    printf(",\"kind\":");
    json_put_string(symbol_kind_name(def));
    printf("}}\n");
}
