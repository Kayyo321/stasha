#include <string.h>
#include "kernel.h"

typedef struct {
    node_t *module;
    const sts_target_spec *target;
    unsigned unsafe_depth;
    unsigned function_depth;
    unsigned parameter_depth;
    unsigned field_depth;
    unsigned recursion;
    boolean_t failed;
} kernel_analysis;

static void kernel_error(kernel_analysis *k, node_t *n, const char *message) {
    diag_begin_error("kernel profile: %s", message);
    if (n) diag_span(DIAG_NODE(n), True, "not qualified in the K0 profile");
    diag_finish();
    k->failed = True;
}
static node_t *kernel_find(kernel_analysis *k, node_kind_t kind, const char *name) {
    for (usize_t i = 0; i < k->module->as.module.decls.count; ++i) {
        node_t *n = k->module->as.module.decls.items[i];
        if (n->kind != kind) continue;
        const char *candidate = kind == NodeFnDecl ? n->as.fn_decl.name : n->as.type_decl.name;
        if (candidate && name && strcmp(candidate, name) == 0) return n;
    }
    return Null;
}
static type_info_t kernel_resolve(kernel_analysis *k, type_info_t t) {
    for (unsigned i = 0; i < 64 && t.base == TypeUser; ++i) {
        node_t *decl = kernel_find(k, NodeTypeDecl, t.user_name);
        if (!decl || decl->as.type_decl.decl_kind != TypeDeclAlias) return t;
        type_info_t next = decl->as.type_decl.alias_type;
        if (t.is_pointer && !next.is_pointer) {
            next.is_pointer = True;
            next.ptr_depth = t.ptr_depth;
            next.ptr_perm = t.ptr_perm;
        }
        t = next;
    }
    if (t.base == TypeUser) {
        node_t *decl = kernel_find(k, NodeTypeDecl, t.user_name);
        if (decl && decl->as.type_decl.decl_kind == TypeDeclAlias) t.base = TypeError;
    }
    return t;
}
static void kernel_type(kernel_analysis *k, node_t *n, type_info_t original, boolean_t abi) {
    type_info_t t = kernel_resolve(k, original);
    if (t.base == TypeF32 || t.base == TypeF64 || t.base == TypeSlice ||
        t.base == TypeFuture || t.base == TypeStream || t.base == TypeZone ||
        t.base == TypeVa || t.base == TypeClosure || t.base == TypeFnPtr ||
        t.base == TypeInfer || t.base == TypeError) {
        kernel_error(k, n, "type requires an unqualified runtime, inference, or FP/SIMD facility");
        return;
    }
    if (abi && !t.is_pointer && t.base == TypeUser)
        kernel_error(k, n, "function interfaces are limited to scalar and pointer types; pass an aggregate by pointer");
}
static void kernel_walk(kernel_analysis *k, node_t *n);
static void kernel_list(kernel_analysis *k, node_list_t *list) {
    for (usize_t i = 0; i < list->count; ++i) kernel_walk(k, list->items[i]);
}
static boolean_t kernel_memory_hook(const char *name) {
    return name && (!strcmp(name, "memcpy") || !strcmp(name, "memmove") || !strcmp(name, "memset"));
}
static void kernel_headers(kernel_analysis *k, node_t *n) {
    if (!n->headers) return;
    for (usize_t i = 0; i < n->headers->count; ++i) {
        const char *key = n->headers->items[i].key;
        if (!strcmp(key, "export_name") && kernel_memory_hook(n->headers->items[i].str_val))
            kernel_error(k, n, "memory runtime hook names are reserved for reviewed external C/assembly implementations");
        if (!strcmp(key, "features") || !strcmp(key, "target") || !strcmp(key, "cpu") || !strcmp(key, "org"))
            kernel_error(k, n, "source header cannot override kernel machine or image-address policy");
        if (!strcmp(key, "export_name") && n->kind == NodeFnDecl &&
            !strcmp(n->as.fn_decl.name, "__stasha_panic"))
            kernel_error(k, n, "panic hook export name cannot be changed");
        if (!strcmp(key, "export_name") && n->headers->items[i].str_val &&
            !strncmp(n->headers->items[i].str_val, "__stasha_", 9))
            kernel_error(k, n, "export names beginning __stasha_ are reserved for the compiler runtime ABI");
    }
}
static void kernel_walk(kernel_analysis *k, node_t *n) {
    if (!n) return;
    if (++k->recursion > 512) {
        kernel_error(k, n, "analysis nesting limit exceeded");
        --k->recursion;
        return;
    }
    kernel_headers(k, n);
#define WALK(x) kernel_walk(k, (x))
#define LIST(x) kernel_list(k, &(x))
    switch (n->kind) {
    case NodeModule: LIST(n->as.module.decls); break;
    case NodeFnDecl:
        if (kernel_memory_hook(n->as.fn_decl.name))
            kernel_error(k, n, "memory runtime hook names are reserved for reviewed external C/assembly implementations");
        if (!strcmp(n->as.fn_decl.name, "__stasha_panic")) {
            boolean_t correct = n->as.fn_decl.return_count == 1 &&
                n->as.fn_decl.params.count == 1 && !n->as.fn_decl.is_variadic;
            if (correct) {
                type_info_t r = kernel_resolve(k, n->as.fn_decl.return_types[0]);
                type_info_t p = kernel_resolve(k, n->as.fn_decl.params.items[0]->as.var_decl.type);
                correct = r.base == TypeVoid && !r.is_pointer && p.base == TypeU32 && !p.is_pointer;
            }
            if (!correct) kernel_error(k, n, "panic hook must have the exact external ABI void(u32)");
        }
        if (!strcmp(n->as.fn_decl.name, "__stasha_panic") && n->as.fn_decl.body)
            kernel_error(k, n, "panic hook definitions require a reviewed nonreturning C/assembly implementation");
        if (n->as.fn_decl.is_async || n->as.fn_decl.is_variadic ||
            n->as.fn_decl.is_typed_variadic || n->as.fn_decl.is_method ||
            n->as.fn_decl.type_param_count || n->as.fn_decl.return_count > 1)
            kernel_error(k, n, "async, variadic, method, generic, and multi-return function interfaces are not yet qualified");
        if (!n->as.fn_decl.body && strcmp(n->as.fn_decl.name, "__stasha_panic") != 0)
            kernel_error(k, n, "external function imports require a qualified hook; K0 only imports __stasha_panic");
        for (usize_t i = 0; i < n->as.fn_decl.return_count; ++i)
            kernel_type(k, n, n->as.fn_decl.return_types[i], True);
        ++k->parameter_depth;
        LIST(n->as.fn_decl.params);
        --k->parameter_depth;
        ++k->function_depth;
        WALK(n->as.fn_decl.body);
        --k->function_depth;
        break;
    case NodeVarDecl:
        if (!k->function_depth && !k->field_depth && kernel_memory_hook(n->as.var_decl.name))
            kernel_error(k, n, "memory runtime hook names are reserved for reviewed external C/assembly implementations");
        kernel_type(k, n, n->as.var_decl.type, k->parameter_depth != 0);
        if (n->as.var_decl.storage == StorageHeap ||
            (n->as.var_decl.flags & (VdeclAtomic | VdeclVolatile | VdeclTls | VdeclRestrict)))
            kernel_error(k, n, "heap, TLS, atomic, volatile, and restrict-qualified storage are not yet qualified");
        if (k->function_depth && !k->parameter_depth && !k->field_depth && !n->as.var_decl.init)
            kernel_error(k, n, "local variables require explicit initialization");
        if (k->parameter_depth && (n->as.var_decl.flags & VdeclArray))
            kernel_error(k, n, "array function parameters are not qualified; pass an explicit pointer and length");
        WALK(n->as.var_decl.init);
        break;
    case NodeTypeDecl:
        if (n->as.type_decl.type_param_count || n->as.type_decl.methods.count ||
            n->as.type_decl.impl_iface_count || n->as.type_decl.decl_kind == TypeDeclInterface)
            kernel_error(k, n, "generic, method-bearing, and interface types are not yet qualified");
        if (n->as.type_decl.decl_kind == TypeDeclAlias)
            kernel_type(k, n, n->as.type_decl.alias_type, False);
        ++k->field_depth;
        LIST(n->as.type_decl.fields);
        --k->field_depth;
        LIST(n->as.type_decl.variants);
        break;
    case NodeEnumVariant:
        if (n->as.enum_variant.has_payload)
            kernel_error(k, n, "tagged payload enums are not yet qualified in K0");
        break;
    case NodeImpDecl: break; /* resolved declarations are also walked */
    case NodeLib: case NodeLibImp: case NodeCHeader:
        kernel_error(k, n, "archive, hosted-library, and C-header imports need a separately audited freestanding interface");
        break;
    case NodeBlock: LIST(n->as.block.stmts); break;
    case NodeForStmt:
        WALK(n->as.for_stmt.init); WALK(n->as.for_stmt.cond);
        WALK(n->as.for_stmt.update); WALK(n->as.for_stmt.body); break;
    case NodeWhileStmt: WALK(n->as.while_stmt.cond); WALK(n->as.while_stmt.body); break;
    case NodeDoWhileStmt: WALK(n->as.do_while_stmt.body); WALK(n->as.do_while_stmt.cond); break;
    case NodeInfLoop: WALK(n->as.inf_loop.body); break;
    case NodeIfStmt:
        WALK(n->as.if_stmt.cond); WALK(n->as.if_stmt.then_block); WALK(n->as.if_stmt.else_block); break;
    case NodeRetStmt: LIST(n->as.ret_stmt.values); break;
    case NodeBreakStmt: case NodeContinueStmt: break;
    case NodeExprStmt: WALK(n->as.expr_stmt.expr); break;
    case NodeDeferStmt: WALK(n->as.defer_stmt.body); break;
    case NodeSwitchStmt: WALK(n->as.switch_stmt.expr); LIST(n->as.switch_stmt.cases); break;
    case NodeSwitchCase: LIST(n->as.switch_case.values); WALK(n->as.switch_case.body); break;
    case NodeComptimeIf:
        /* Validate both branches: an inactive branch is not an escape hatch
         * for changing the declared capability boundary across targets. */
        WALK(n->as.comptime_if.body); WALK(n->as.comptime_if.else_body); break;
    case NodeComptimeAssert: WALK(n->as.comptime_assert.expr); break;
    case NodeCompoundInit: LIST(n->as.compound_init.items); break;
    case NodeInitField: WALK(n->as.init_field.value); break;
    case NodeInitIndex: WALK(n->as.init_index.index); WALK(n->as.init_index.value); break;
    case NodeDesigInit: LIST(n->as.desig_init.values); break;
    case NodeBinaryExpr: WALK(n->as.binary.left); WALK(n->as.binary.right); break;
    case NodeUnaryPrefixExpr: case NodeUnaryPostfixExpr:
        if (n->as.unary.op == TokStar && !k->unsafe_depth)
            kernel_error(k, n, "raw-pointer dereference requires an explicit unsafe block");
        WALK(n->as.unary.operand); break;
    case NodeCallExpr: {
        node_t *fn = kernel_find(k, NodeFnDecl, n->as.call.callee);
        if (!fn) kernel_error(k, n, "calls must name a declared direct function; implicit hosted builtins are unavailable");
        LIST(n->as.call.args); break;
    }
    case NodeCompoundAssign: WALK(n->as.compound_assign.target); WALK(n->as.compound_assign.value); break;
    case NodeAssignExpr: WALK(n->as.assign.target); WALK(n->as.assign.value); break;
    case NodeIdentExpr: case NodeIntLitExpr: case NodeCharLitExpr:
    case NodeBoolLitExpr: case NodeNilExpr: break;
    case NodeStrLitExpr:
        if (n->as.str_lit.is_heap) kernel_error(k, n, "heap strings are unavailable");
        break;
    case NodeIndexExpr:
        if (!k->unsafe_depth)
            kernel_error(k, n, "indexed memory access requires an explicit unsafe block until bounds and provenance are qualified");
        WALK(n->as.index_expr.object); WALK(n->as.index_expr.index); break;
    case NodeFlaggedIndex:
        if (!k->unsafe_depth) kernel_error(k, n, "unchecked access requires an explicit unsafe block");
        WALK(n->as.flagged_index.object); WALK(n->as.flagged_index.index); break;
    case NodeMemberExpr:
        if (!k->unsafe_depth) kernel_error(k, n, "member memory access requires an explicit unsafe block in initial K0");
        WALK(n->as.member_expr.object); break;
    case NodeTernaryExpr:
        WALK(n->as.ternary.cond); WALK(n->as.ternary.then_expr); WALK(n->as.ternary.else_expr); break;
    case NodeCastExpr: kernel_type(k, n, n->as.cast_expr.target, False); WALK(n->as.cast_expr.expr); break;
    case NodeSizeofExpr: kernel_type(k, n, n->as.sizeof_expr.type, False); break;
    case NodeAddrOf:
        if (k->function_depth && !k->unsafe_depth) kernel_error(k, n, "address-taking requires an explicit unsafe block in initial K0");
        WALK(n->as.addr_of.operand); break;
    case NodeUnsafeBlock: ++k->unsafe_depth; WALK(n->as.unsafe_block.body); --k->unsafe_depth; break;
    default:
        kernel_error(k, n, "language feature is not yet qualified in the K0 profile; use the hosted profile or an audited external boundary");
        break;
    }
#undef WALK
#undef LIST
    --k->recursion;
}

result_t analyze_kernel_profile(node_t *ast, const sts_target_spec *target) {
    kernel_analysis k = {.module = ast, .target = target};
    if (!ast || ast->kind != NodeModule) return Err;
    if (strcmp(target->arch, "x86_64") || target->pointer_bits != 64 || target->format != StsFormatELF)
        kernel_error(&k, ast, "initial qualification supports only 64-bit x86 ELF");
    if (ast->as.module.has_org)
        kernel_error(&k, ast, "org is not implemented; supply a verified linker script for image addresses");
    kernel_walk(&k, ast);
    return k.failed ? Err : Ok;
}
