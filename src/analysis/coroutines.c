#include <string.h>

#include "coroutines.h"

typedef struct binding binding_t;
struct binding {
    heap_t     self;
    const char *name;
    type_info_t type;
    binding_t  *next;
};

typedef struct {
    node_t     *ast;
    node_t     *current_fn;
    binding_t  *bindings;
    usize_t     binding_depths[256];
    usize_t     binding_depth;
    boolean_t   had_error;
} coro_analysis_t;

static boolean_t type_equals(type_info_t a, type_info_t b) {
    if (a.base != b.base) return False;
    if (a.is_pointer != b.is_pointer) return False;
    if (a.ptr_depth != b.ptr_depth) return False;
    if (a.user_name || b.user_name) {
        if (!a.user_name || !b.user_name) return False;
        if (strcmp(a.user_name, b.user_name) != 0) return False;
    }
    if (a.elem_type || b.elem_type) {
        if (!a.elem_type || !b.elem_type) return False;
        return type_equals(a.elem_type[0], b.elem_type[0]);
    }
    return True;
}

static void push_scope(coro_analysis_t *ca) {
    if (ca->binding_depth < 256)
        ca->binding_depths[ca->binding_depth++] = 0;
}

static void bind_name(coro_analysis_t *ca, const char *name, type_info_t type) {
    if (!name || !name[0] || strcmp(name, "_") == 0) return;
    heap_t h = allocate(1, sizeof(binding_t));
    binding_t *b = h.pointer;
    b->self = h;
    b->name = name;
    b->type = type;
    b->next = ca->bindings;
    ca->bindings = b;
    if (ca->binding_depth > 0)
        ca->binding_depths[ca->binding_depth - 1]++;
}

static void pop_scope(coro_analysis_t *ca) {
    if (ca->binding_depth == 0) return;
    usize_t n = ca->binding_depths[ca->binding_depth - 1];
    while (n-- > 0 && ca->bindings) {
        binding_t *next = ca->bindings->next;
        deallocate(ca->bindings->self);
        ca->bindings = next;
    }
    ca->binding_depth--;
}

static type_info_t lookup_binding(coro_analysis_t *ca, const char *name) {
    for (binding_t *b = ca->bindings; b; b = b->next)
        if (strcmp(b->name, name) == 0) return b->type;
    return NO_TYPE;
}

static node_t *find_fn_decl_in_list(node_list_t *decls, const char *name);

static node_t *find_fn_decl_in_type_methods(node_t *decl, const char *name) {
    if (!decl || decl->kind != NodeTypeDecl) return Null;
    for (usize_t i = 0; i < decl->as.type_decl.methods.count; i++) {
        node_t *method = decl->as.type_decl.methods.items[i];
        if (method->kind == NodeFnDecl && method->as.fn_decl.name
                && strcmp(method->as.fn_decl.name, name) == 0)
            return method;
    }
    return Null;
}

static node_t *find_fn_decl_in_list(node_list_t *decls, const char *name) {
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *decl = decls->items[i];
        if (decl->kind == NodeFnDecl && decl->as.fn_decl.name
                && strcmp(decl->as.fn_decl.name, name) == 0)
            return decl;
        if (decl->kind == NodeTypeDecl) {
            node_t *method = find_fn_decl_in_type_methods(decl, name);
            if (method) return method;
        }
        if (decl->kind == NodeSubMod) {
            node_t *nested = find_fn_decl_in_list(&decl->as.submod.decls, name);
            if (nested) return nested;
        }
    }
    return Null;
}

static node_t *find_fn_decl(coro_analysis_t *ca, const char *name) {
    if (!ca->ast || ca->ast->kind != NodeModule || !name) return Null;
    return find_fn_decl_in_list(&ca->ast->as.module.decls, name);
}

static type_info_t infer_expr_type(coro_analysis_t *ca, node_t *expr) {
    if (!expr) return NO_TYPE;
    switch (expr->kind) {
        case NodeIntLitExpr:  return (type_info_t){ .base = TypeI64 };
        case NodeFloatLitExpr:return (type_info_t){ .base = TypeF64 };
        case NodeBoolLitExpr: return (type_info_t){ .base = TypeBool };
        case NodeCharLitExpr: return (type_info_t){ .base = TypeI8 };
        case NodeStrLitExpr: {
            type_info_t ti = NO_TYPE;
            ti.base = TypeI8;
            ti.is_pointer = True;
            ti.ptr_depth = 1;
            ti.ptr_perm = PtrRead;
            ti.ptr_perms[0] = PtrRead;
            return ti;
        }
        case NodeIdentExpr:
            return lookup_binding(ca, expr->as.ident.name);
        case NodeCastExpr:
            return expr->as.cast_expr.target;
        case NodeAwaitExpr: {
            node_t *handle = expr->as.await_expr.handle;
            if (!handle) return NO_TYPE;
            if (handle->kind == NodeIdentExpr) {
                type_info_t ti = lookup_binding(ca, handle->as.ident.name);
                if ((ti.base == TypeFuture || ti.base == TypeStream) && ti.elem_type)
                    return ti.elem_type[0];
            }
            if (handle->kind == NodeAsyncCall) {
                node_t *fn = find_fn_decl(ca, handle->as.async_call.callee);
                if (fn && fn->as.fn_decl.return_count > 0)
                    return fn->as.fn_decl.return_types[0];
            }
            return NO_TYPE;
        }
        case NodeAsyncCall: {
            node_t *fn = find_fn_decl(ca, expr->as.async_call.callee);
            if (!fn || fn->as.fn_decl.return_count == 0) return NO_TYPE;
            if (fn->as.fn_decl.coro_flavor == CoroStream) {
                type_info_t ti = NO_TYPE;
                ti.base = TypeStream;
                if (fn->as.fn_decl.yield_type.base != TypeVoid || fn->as.fn_decl.yield_type.is_pointer) {
                    ti.elem_type = alloc_type_array(1);
                    ti.elem_type[0] = fn->as.fn_decl.yield_type;
                }
                return ti;
            }
            type_info_t ti = NO_TYPE;
            ti.base = TypeFuture;
            ti.elem_type = alloc_type_array(1);
            ti.elem_type[0] = fn->as.fn_decl.return_types[0];
            return ti;
        }
        case NodeCallExpr: {
            node_t *fn = find_fn_decl(ca, expr->as.call.callee);
            if (fn && fn->as.fn_decl.return_count > 0)
                return fn->as.fn_decl.return_types[0];
            return NO_TYPE;
        }
        default:
            return NO_TYPE;
    }
}

static void note_error(coro_analysis_t *ca) {
    ca->had_error = True;
}

static void analyze_expr(coro_analysis_t *ca, node_t *expr);
static void analyze_stmt(coro_analysis_t *ca, node_t *stmt);

static void analyze_node_list(coro_analysis_t *ca, node_list_t *nodes, boolean_t as_stmt) {
    for (usize_t i = 0; i < nodes->count; i++) {
        if (as_stmt) analyze_stmt(ca, nodes->items[i]);
        else analyze_expr(ca, nodes->items[i]);
    }
}

static void analyze_async_call_target(coro_analysis_t *ca, node_t *expr) {
    if (!expr || expr->kind != NodeAsyncCall) return;
    node_t *fn = find_fn_decl(ca, expr->as.async_call.callee);
    if (fn && fn->as.fn_decl.coro_flavor == CoroStream) {
        diag_begin_error("async.() cannot dispatch yielding async function '%s'",
                         expr->as.async_call.callee);
        diag_span(DIAG_NODE(expr), True, "yielding async functions return stream handles directly");
        diag_help("call '%s(...)' directly and store the stream.[T] result", expr->as.async_call.callee);
        diag_finish();
        note_error(ca);
    }
}

static void analyze_expr(coro_analysis_t *ca, node_t *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case NodeAsyncCall:
            analyze_async_call_target(ca, expr);
            analyze_node_list(ca, &expr->as.async_call.args, False);
            break;
        case NodeAwaitExpr:
            /* `await.next(s)` synchronously drives a stream — legal anywhere.
               Other forms still require an enclosing `async fn`. */
            if (expr->as.await_expr.is_stream_next) {
                if (ca->current_fn && ca->current_fn->as.fn_decl.is_async)
                    ca->current_fn->as.fn_decl.has_await = True;
            } else if (!ca->current_fn || !ca->current_fn->as.fn_decl.is_async) {
                diag_begin_error("'await' is only legal inside 'async fn'");
                diag_span(DIAG_NODE(expr), True, "await used here");
                diag_help("wrap this logic in an 'async fn' and await from there");
                diag_finish();
                note_error(ca);
            } else {
                ca->current_fn->as.fn_decl.has_await = True;
            }
            analyze_expr(ca, expr->as.await_expr.handle);
            if (expr->as.await_expr.is_stream_next && expr->as.await_expr.handle
                    && expr->as.await_expr.handle->kind == NodeIdentExpr) {
                type_info_t ti = lookup_binding(ca, expr->as.await_expr.handle->as.ident.name);
                if (ti.base != TypeStream) {
                    diag_begin_error("await.next(...) expects a stream.[T] handle");
                    diag_span(DIAG_NODE(expr), True, "non-stream await.next target here");
                    diag_finish();
                    note_error(ca);
                }
            }
            break;
        case NodeAwaitCombinator:
            if (!ca->current_fn || !ca->current_fn->as.fn_decl.is_async) {
                diag_begin_error("'await' is only legal inside 'async fn'");
                diag_span(DIAG_NODE(expr), True, "await combinator used here");
                diag_finish();
                note_error(ca);
            } else {
                ca->current_fn->as.fn_decl.has_await = True;
            }
            analyze_node_list(ca, &expr->as.await_combinator.handles, False);
            break;
        case NodeBinaryExpr:
            analyze_expr(ca, expr->as.binary.left);
            analyze_expr(ca, expr->as.binary.right);
            break;
        case NodeUnaryPrefixExpr:
        case NodeUnaryPostfixExpr:
            analyze_expr(ca, expr->as.unary.operand);
            break;
        case NodeCallExpr:
            analyze_node_list(ca, &expr->as.call.args, False);
            break;
        case NodeMethodCall:
            analyze_expr(ca, expr->as.method_call.object);
            analyze_node_list(ca, &expr->as.method_call.args, False);
            break;
        case NodeThreadCall:
            analyze_node_list(ca, &expr->as.thread_call.args, False);
            break;
        case NodeFutureOp:
            analyze_expr(ca, expr->as.future_op.handle);
            break;
        case NodeCompoundAssign:
            analyze_expr(ca, expr->as.compound_assign.target);
            analyze_expr(ca, expr->as.compound_assign.value);
            break;
        case NodeAssignExpr:
            analyze_expr(ca, expr->as.assign.target);
            analyze_expr(ca, expr->as.assign.value);
            break;
        case NodeIndexExpr:
            analyze_expr(ca, expr->as.index_expr.object);
            analyze_expr(ca, expr->as.index_expr.index);
            break;
        case NodeMemberExpr:
            analyze_expr(ca, expr->as.member_expr.object);
            break;
        case NodeTernaryExpr:
            analyze_expr(ca, expr->as.ternary.cond);
            analyze_expr(ca, expr->as.ternary.then_expr);
            analyze_expr(ca, expr->as.ternary.else_expr);
            break;
        case NodeCastExpr:
            analyze_expr(ca, expr->as.cast_expr.expr);
            break;
        case NodeMovExpr:
            analyze_expr(ca, expr->as.mov_expr.ptr);
            analyze_expr(ca, expr->as.mov_expr.size);
            break;
        case NodeAddrOf:
            analyze_expr(ca, expr->as.addr_of.operand);
            break;
        case NodeErrorExpr:
            analyze_node_list(ca, &expr->as.error_expr.args, False);
            break;
        case NodeExpectExpr:
            analyze_expr(ca, expr->as.expect_expr.expr);
            break;
        case NodeExpectEqExpr:
            analyze_expr(ca, expr->as.expect_eq.left);
            analyze_expr(ca, expr->as.expect_eq.right);
            break;
        case NodeExpectNeqExpr:
            analyze_expr(ca, expr->as.expect_neq.left);
            analyze_expr(ca, expr->as.expect_neq.right);
            break;
        case NodeHashExpr:
            analyze_expr(ca, expr->as.hash_expr.operand);
            break;
        case NodeEquExpr:
            analyze_expr(ca, expr->as.equ_expr.left);
            analyze_expr(ca, expr->as.equ_expr.right);
            break;
        case NodeConstructorCall:
            analyze_node_list(ca, &expr->as.ctor_call.args, False);
            break;
        case NodeErrPropCall:
            analyze_node_list(ca, &expr->as.err_prop_call.args, False);
            break;
        case NodeErrProp:
            analyze_expr(ca, expr->as.err_prop.operand);
            break;
        case NodeAnyTypeExpr:
            analyze_expr(ca, expr->as.any_type_expr.operand);
            break;
        case NodeSpreadExpr:
            analyze_expr(ca, expr->as.spread_expr.expr);
            break;
        case NodeRangeExpr:
            analyze_expr(ca, expr->as.range_expr.start);
            analyze_expr(ca, expr->as.range_expr.end);
            analyze_expr(ca, expr->as.range_expr.step);
            break;
        case NodeSliceExpr:
            analyze_expr(ca, expr->as.slice_expr.object);
            analyze_expr(ca, expr->as.slice_expr.lo);
            analyze_expr(ca, expr->as.slice_expr.hi);
            break;
        case NodeMakeExpr:
            analyze_expr(ca, expr->as.make_expr.len);
            analyze_expr(ca, expr->as.make_expr.cap);
            analyze_expr(ca, expr->as.make_expr.init);
            break;
        case NodeAppendExpr:
            analyze_expr(ca, expr->as.append_expr.slice);
            analyze_expr(ca, expr->as.append_expr.val);
            break;
        case NodeCopyExpr:
            analyze_expr(ca, expr->as.copy_expr.dst);
            analyze_expr(ca, expr->as.copy_expr.src);
            break;
        case NodeLenExpr:
        case NodeCapExpr:
            analyze_expr(ca, expr->as.len_expr.operand);
            break;
        case NodeComptimeFmt:
            analyze_node_list(ca, &expr->as.comptime_fmt.args, False);
            break;
        case NodeColonCall:
            analyze_node_list(ca, &expr->as.colon_call.args, False);
            break;
        default:
            break;
    }
}

static void analyze_stmt(coro_analysis_t *ca, node_t *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case NodeBlock:
            push_scope(ca);
            analyze_node_list(ca, &stmt->as.block.stmts, True);
            pop_scope(ca);
            break;
        case NodeVarDecl:
            if (stmt->as.var_decl.init)
                analyze_expr(ca, stmt->as.var_decl.init);
            bind_name(ca, stmt->as.var_decl.name, stmt->as.var_decl.type);
            break;
        case NodeExprStmt:
            analyze_expr(ca, stmt->as.expr_stmt.expr);
            break;
        case NodeRetStmt:
            analyze_node_list(ca, &stmt->as.ret_stmt.values, False);
            if (ca->current_fn && ca->current_fn->as.fn_decl.coro_flavor == CoroStream
                    && stmt->as.ret_stmt.values.count > 0) {
                diag_begin_error("stream coroutine '%s' cannot return a final value in v1",
                                 ca->current_fn->as.fn_decl.name);
                diag_span(DIAG_NODE(stmt), True, "use 'ret;' to end a stream coroutine");
                diag_finish();
                note_error(ca);
            }
            break;
        case NodeIfStmt:
            analyze_expr(ca, stmt->as.if_stmt.cond);
            analyze_stmt(ca, stmt->as.if_stmt.then_block);
            analyze_stmt(ca, stmt->as.if_stmt.else_block);
            break;
        case NodeWhileStmt:
            analyze_expr(ca, stmt->as.while_stmt.cond);
            analyze_stmt(ca, stmt->as.while_stmt.body);
            break;
        case NodeDoWhileStmt:
            analyze_stmt(ca, stmt->as.do_while_stmt.body);
            analyze_expr(ca, stmt->as.do_while_stmt.cond);
            break;
        case NodeForStmt:
            push_scope(ca);
            analyze_stmt(ca, stmt->as.for_stmt.init);
            analyze_expr(ca, stmt->as.for_stmt.cond);
            analyze_expr(ca, stmt->as.for_stmt.update);
            analyze_stmt(ca, stmt->as.for_stmt.body);
            pop_scope(ca);
            break;
        case NodeForeachStmt:
            analyze_expr(ca, stmt->as.foreach_stmt.slice);
            push_scope(ca);
            bind_name(ca, stmt->as.foreach_stmt.iter_name, NO_TYPE);
            analyze_stmt(ca, stmt->as.foreach_stmt.body);
            pop_scope(ca);
            break;
        case NodeInfLoop:
            analyze_stmt(ca, stmt->as.inf_loop.body);
            break;
        case NodeWithStmt:
            push_scope(ca);
            analyze_stmt(ca, stmt->as.with_stmt.decl);
            analyze_expr(ca, stmt->as.with_stmt.cond);
            analyze_stmt(ca, stmt->as.with_stmt.body);
            analyze_stmt(ca, stmt->as.with_stmt.else_block);
            pop_scope(ca);
            break;
        case NodeMatchStmt:
            analyze_expr(ca, stmt->as.match_stmt.expr);
            for (usize_t i = 0; i < stmt->as.match_stmt.arms.count; i++)
                analyze_stmt(ca, stmt->as.match_stmt.arms.items[i]);
            break;
        case NodeMatchArm:
            push_scope(ca);
            if (stmt->as.match_arm.bind_name)
                bind_name(ca, stmt->as.match_arm.bind_name, NO_TYPE);
            analyze_expr(ca, stmt->as.match_arm.guard_expr);
            analyze_stmt(ca, stmt->as.match_arm.body);
            pop_scope(ca);
            break;
        case NodeSwitchStmt:
            analyze_expr(ca, stmt->as.switch_stmt.expr);
            for (usize_t i = 0; i < stmt->as.switch_stmt.cases.count; i++)
                analyze_stmt(ca, stmt->as.switch_stmt.cases.items[i]);
            break;
        case NodeSwitchCase:
            analyze_node_list(ca, &stmt->as.switch_case.values, False);
            analyze_stmt(ca, stmt->as.switch_case.body);
            break;
        case NodeDeferStmt:
            analyze_stmt(ca, stmt->as.defer_stmt.body);
            break;
        case NodeComptimeIf:
            analyze_stmt(ca, stmt->as.comptime_if.body);
            analyze_stmt(ca, stmt->as.comptime_if.else_body);
            break;
        case NodeComptimeAssert:
            analyze_expr(ca, stmt->as.comptime_assert.expr);
            break;
        case NodeYieldExpr:
            if (!ca->current_fn || !ca->current_fn->as.fn_decl.is_async) {
                diag_begin_error("'yield' is only legal inside 'async fn'");
                diag_span(DIAG_NODE(stmt), True, "yield used here");
                diag_finish();
                note_error(ca);
                analyze_expr(ca, stmt->as.yield_expr.value);
                break;
            }
            ca->current_fn->as.fn_decl.has_yield_value = True;
            ca->current_fn->as.fn_decl.coro_flavor = CoroStream;
            analyze_expr(ca, stmt->as.yield_expr.value);
            {
                type_info_t yt = infer_expr_type(ca, stmt->as.yield_expr.value);
                if (yt.base != TypeVoid || yt.is_pointer) {
                    if (ca->current_fn->as.fn_decl.yield_type.base == TypeVoid
                            && !ca->current_fn->as.fn_decl.yield_type.is_pointer) {
                        ca->current_fn->as.fn_decl.yield_type = yt;
                    } else if (!type_equals(ca->current_fn->as.fn_decl.yield_type, yt)) {
                        diag_begin_error("yielded values in async stream '%s' must share one type",
                                         ca->current_fn->as.fn_decl.name);
                        diag_span(DIAG_NODE(stmt), True, "mismatched yield item type here");
                        diag_finish();
                        note_error(ca);
                    }
                }
            }
            break;
        case NodeYieldNowExpr:
            if (!ca->current_fn || !ca->current_fn->as.fn_decl.is_async) {
                diag_begin_error("'yield;' is only legal inside 'async fn'");
                diag_span(DIAG_NODE(stmt), True, "scheduler yield used here");
                diag_finish();
                note_error(ca);
                break;
            }
            ca->current_fn->as.fn_decl.has_yield_now = True;
            break;
        default:
            break;
    }
}

static void analyze_fn(coro_analysis_t *ca, node_t *fn) {
    if (!fn || fn->kind != NodeFnDecl || !fn->as.fn_decl.body) return;

    fn->as.fn_decl.coro_flavor = fn->as.fn_decl.is_async ? CoroTask : CoroNone;
    fn->as.fn_decl.yield_type = NO_TYPE;
    fn->as.fn_decl.has_await = False;
    fn->as.fn_decl.has_yield_value = False;
    fn->as.fn_decl.has_yield_now = False;

    node_t *prev_fn = ca->current_fn;
    binding_t *prev_bindings = ca->bindings;
    usize_t prev_depth = ca->binding_depth;

    ca->current_fn = fn;
    ca->bindings = Null;
    ca->binding_depth = 0;
    push_scope(ca);
    for (usize_t i = 0; i < fn->as.fn_decl.params.count; i++) {
        node_t *param = fn->as.fn_decl.params.items[i];
        bind_name(ca, param->as.var_decl.name, param->as.var_decl.type);
    }
    analyze_stmt(ca, fn->as.fn_decl.body);
    pop_scope(ca);

    if (fn->as.fn_decl.is_async) {
        type_info_t declared = fn->as.fn_decl.return_count > 0
            ? fn->as.fn_decl.return_types[0] : NO_TYPE;
        if (fn->as.fn_decl.has_yield_value) {
            fn->as.fn_decl.coro_flavor = CoroStream;
            if (declared.base != TypeStream) {
                diag_begin_error("yielding async function '%s' must return 'stream.[T]'",
                                 fn->as.fn_decl.name);
                diag_span(DIAG_NODE(fn), True, "change the declared return type to stream.[T]");
                diag_finish();
                note_error(ca);
            } else if (declared.elem_type
                    && (fn->as.fn_decl.yield_type.base != TypeVoid || fn->as.fn_decl.yield_type.is_pointer)
                    && !type_equals(declared.elem_type[0], fn->as.fn_decl.yield_type)) {
                diag_begin_error("declared stream item type for '%s' does not match yielded values",
                                 fn->as.fn_decl.name);
                diag_span(DIAG_NODE(fn), True, "declared stream type here");
                diag_finish();
                note_error(ca);
            }
        } else {
            fn->as.fn_decl.coro_flavor = CoroTask;
            if (declared.base == TypeStream) {
                diag_begin_error("async function '%s' declares 'stream.[T]' but never yields a value",
                                 fn->as.fn_decl.name);
                diag_span(DIAG_NODE(fn), True, "remove the stream return type or add 'yield expr;'");
                diag_finish();
                note_error(ca);
            }
        }
    }

    ca->current_fn = prev_fn;
    ca->bindings = prev_bindings;
    ca->binding_depth = prev_depth;
}

static void analyze_decl_list(coro_analysis_t *ca, node_list_t *decls) {
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *decl = decls->items[i];
        if (decl->kind == NodeFnDecl) {
            analyze_fn(ca, decl);
        } else if (decl->kind == NodeTypeDecl) {
            for (usize_t j = 0; j < decl->as.type_decl.methods.count; j++)
                analyze_fn(ca, decl->as.type_decl.methods.items[j]);
        } else if (decl->kind == NodeSubMod) {
            analyze_decl_list(ca, &decl->as.submod.decls);
        }
    }
}

result_t analyze_coroutines(node_t *ast) {
    if (!ast || ast->kind != NodeModule) return Ok;
    coro_analysis_t ca;
    memset(&ca, 0, sizeof(ca));
    ca.ast = ast;
    analyze_decl_list(&ca, &ast->as.module.decls);
    return ca.had_error ? Err : Ok;
}
