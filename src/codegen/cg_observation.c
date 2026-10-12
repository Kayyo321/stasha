/* Native sequential leaf test-gate observation. This source inventory is built
 * independently of lowering and LLVM reachability, and defaults to rejection. */
#include <stdarg.h>

/* The compiler and standalone codegen unity probes share the unchanged SHA
 * implementation. Keep the old heap API enforcement and macro namespace intact;
 * generated Stasha images do not acquire this compiler-side dependency. */
#pragma push_macro("malloc")
#pragma push_macro("realloc")
#pragma push_macro("free")
#pragma push_macro("K")
#pragma push_macro("ROR")
#undef malloc
#undef realloc
#undef free
#undef K
#undef ROR
#define K sts_observation_sha256_constants
#include "../../std/crypto/sha256_rt.c"
#pragma pop_macro("ROR")
#pragma pop_macro("K")
#pragma pop_macro("free")
#pragma pop_macro("realloc")
#pragma pop_macro("malloc")


static boolean_t obs_error(node_t *n, const char *what) {
    diag_begin_error("unsupported by observation profile: %s", what);
    if (n) diag_span(SRC_LOC(n->observation_line ? n->observation_line : n->line, 1, 1), True, "here");
    diag_finish();
    return False;
}

result_t codegen_observation_check_source(const char *source, const char *path) {
    lexer_t lex;
    init_lexer(&lex, source);
    for (;;) {
        token_t t = next_token(&lex);
        if (t.kind == TokEof) return Ok;
        if (t.kind == TokError) return Err;
        switch (t.kind) {
            case TokImp: case TokLib: case TokLibImp: case TokCHeader: case TokMacro:
                diag_set_file(path);
                diag_begin_error("unsupported by observation profile: imports, libraries, C providers and macros");
                diag_span(SRC_LOC(t.line, t.col, t.length), True, "provider or macro rejected before preprocessing");
                diag_finish();
                return Err;
            default: break;
        }
    }
}

result_t codegen_observation_check_providers(node_t *ast, const sts_observation_config *cfg) {
    if (!cfg) return Ok;
    if (!ast || ast->kind != NodeModule) return obs_error(ast, "missing root module") ? Ok : Err;
    if (ast->observation_expanded || ast->from_lib || ast->as.module.freestanding ||
        ast->as.module.kernel_profile || ast->as.module.has_org)
        return obs_error(ast, "macro/provider/freestanding module") ? Ok : Err;
    for (usize_t i = 0; i < ast->as.module.decls.count; ++i) {
        node_t *d = ast->as.module.decls.items[i];
        switch (d->kind) {
            case NodeFnDecl: case NodeVarDecl: case NodeTestBlock: case NodeTypeDecl:
            case NodeComptimeAssert: break;
            default: return obs_error(d, "top-level declaration or provider") ? Ok : Err;
        }
    }
    return Ok;
}

typedef struct { const char *name; type_info_t type; unsigned array_rank; } obs_binding;
typedef struct {
    cg_t *cg;
    const char *owner;
    uint32_t owner_test;
    unsigned reason;
    unsigned depth;
    type_info_t return_type, match_type;
    obs_binding *bindings;
    size_t binding_count, binding_capacity;
} obs_walk;

static boolean_t obs_origin(obs_walk *w, node_t *n) {
    if (!n || !n->observation_file || !n->observation_line ||
        n->observation_line > w->cg->observation_config->source_line_count ||
        n->observation_end_line < n->observation_line ||
        n->observation_end_line > w->cg->observation_config->source_line_count ||
        strcmp(n->observation_file, w->cg->source_file) || n->observation_expanded ||
        n->from_lib || n->module_name || n->generic_use || n->is_c_extern)
        return obs_error(n, "unknown or non-root source origin, expansion, provider or generic use");
    return True;
}

static boolean_t obs_headers(cg_t *cg, node_t *n, boolean_t *skip) {
    *skip = False;
    if (!n->headers) return True;
    for (usize_t i = 0; i < n->headers->count; ++i) {
        fh_entry_t *e = &n->headers->items[i];
        if (e->vkind != FhCond || (strcmp(e->key, "if") && strcmp(e->key, "require")) ||
            e->cond.op == FhCondNone || e->cond.op == FhCondAlwaysFalse)
            return obs_error(n, "unqualified source header attribute");
    }
    *skip = cg_fh_skip_decl(cg, n->headers, n->line);
    return True;
}

static boolean_t obs_type(node_t *n, type_info_t t, boolean_t allow_void) {
    if (t.base < TypeVoid || t.base > TypeF64 || t.generic_use || t.fn_ptr_desc ||
        t.elem_type || t.user_name || t.ptr_depth < 0 || t.ptr_depth > 8 ||
        (t.base == TypeVoid && !t.is_pointer && !allow_void))
        return obs_error(n, "non-primitive type or function value");
    return True;
}

static boolean_t obs_binding_name(const char *name) {
    if (!name || !*name || strlen(name) > 127) return False;
    const unsigned char *p=(const unsigned char *)name;
    if (!((*p>='A' && *p<='Z') || (*p>='a' && *p<='z') || *p=='_')) return False;
    for (++p;*p;++p)
        if (!((*p>='A' && *p<='Z') || (*p>='a' && *p<='z') ||
              (*p>='0' && *p<='9') || *p=='_')) return False;
    return True;
}

static boolean_t obs_reserved(const char *name) {
    static const char *names[] = {"malloc","calloc","realloc","free","printf","fprintf",
        "puts","fputs","putchar","fwrite","memcpy","memmove","memset","memcmp","strcmp",
        "strlen","abort","exit","_Exit","snprintf", "stderr", "stdout"};
    if (!obs_binding_name(name) || !strncmp(name, "__", 2)) return True;
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
        if (!strcmp(name, names[i])) return True;
    return False;
}

static node_t *obs_any_function(cg_t *cg, const char *name) {
    node_t *found = Null;
    for (usize_t i = 0; i < cg->ast->as.module.decls.count; ++i) {
        node_t *d = cg->ast->as.module.decls.items[i];
        if (d->kind == NodeFnDecl && !strcmp(d->as.fn_decl.name, name)) found = d;
    }
    return found;
}
static node_t *obs_function(cg_t *cg, const char *name) {
    node_t *found = obs_any_function(cg,name);
    if (!found || !found->as.fn_decl.body || found->as.fn_decl.type_param_count ||
        found->as.fn_decl.is_method || found->as.fn_decl.is_async ||
        !strcmp(name, "main") || cg_fh_skip_decl(cg, found->headers, found->line)) return Null;
    return found;
}

static boolean_t obs_bound(obs_walk *w, const char *name) {
    for (size_t i = w->binding_count; i > 0; --i)
        if (!strcmp(w->bindings[i-1].name, name)) return True;
    return False;
}

static boolean_t obs_bind(obs_walk *w, node_t *n) {
    if (obs_reserved(n->as.var_decl.name)) return obs_error(n, "binding must be a nonreserved ASCII identifier of at most 127 bytes");
    if (w->binding_count == w->binding_capacity) {
        size_t cap = w->binding_capacity ? w->binding_capacity * 2 : 64;
        if (cap > STASHA_OBSERVATION_MAX_SITES) return obs_error(n, "source binding capacity");
        obs_binding *p = (realloc)(w->bindings, cap * sizeof(*p));
        if (!p) return obs_error(n, "source inventory allocation failure");
        w->bindings = p; w->binding_capacity = cap;
    }
    w->bindings[w->binding_count++] = (obs_binding){n->as.var_decl.name, n->as.var_decl.type,
        (n->as.var_decl.flags & VdeclArray) ? (unsigned)(n->as.var_decl.array_ndim > 0 ? n->as.var_decl.array_ndim : 1) : 0};
    return True;
}

/* Bounded source typing for admission, never a replacement for semantic typing.
 * In particular it runs for statements the lowerer skips after a terminator. */
static type_info_t obs_expr_type(obs_walk *w, node_t *n, unsigned depth) {
    type_info_t t=NO_TYPE; t.base=TypeInfer;
    if(!n || depth>1024)return t;
    switch(n->kind) {
        case NodeIdentExpr:
            for(size_t i=w->binding_count;i>0;--i)if(!strcmp(w->bindings[i-1].name,n->as.ident.name)) {
                t=w->bindings[i-1].type;
                if(w->bindings[i-1].array_rank) {t.ptr_depth=(t.is_pointer?(t.ptr_depth?t.ptr_depth:1):0)+(int)w->bindings[i-1].array_rank;t.is_pointer=True;}
                return t;
            }
            return t;
        case NodeNewExpr: case NodeNilExpr: t.base=TypeVoid;t.is_pointer=True;t.ptr_depth=1;return t;
        case NodeStrLitExpr: t.base=TypeI8;t.is_pointer=True;t.ptr_depth=1;return t;
        case NodeMovExpr: return obs_expr_type(w,n->as.mov_expr.ptr,depth+1);
        case NodeCastExpr: return n->as.cast_expr.target;
        case NodeCallExpr: {node_t*f=obs_function(w->cg,n->as.call.callee);return f && f->as.fn_decl.return_count==1?f->as.fn_decl.return_types[0]:t;}
        case NodeAddrOf:
            t=obs_expr_type(w,n->as.addr_of.operand,depth+1);t.ptr_depth=(t.is_pointer?(t.ptr_depth?t.ptr_depth:1):0)+1;t.is_pointer=True;return t;
        case NodeIndexExpr:
            t=obs_expr_type(w,n->as.index_expr.object,depth+1);if(t.is_pointer){t.ptr_depth=t.ptr_depth>0?t.ptr_depth-1:0;t.is_pointer=t.ptr_depth>0;}return t;
        case NodeUnaryPrefixExpr: case NodeUnaryPostfixExpr:
            t=obs_expr_type(w,n->as.unary.operand,depth+1);
            if(n->as.unary.op==TokStar && t.is_pointer){t.ptr_depth=t.ptr_depth>0?t.ptr_depth-1:0;t.is_pointer=t.ptr_depth>0;}
            else if(n->as.unary.op==TokBang){t.base=TypeBool;t.is_pointer=False;t.ptr_depth=0;}
            return t;
        case NodeTernaryExpr: {
            type_info_t a=obs_expr_type(w,n->as.ternary.then_expr,depth+1),b=obs_expr_type(w,n->as.ternary.else_expr,depth+1);
            if(a.is_pointer != b.is_pointer)return t;
            return a;
        }
        case NodeAssignExpr:return obs_expr_type(w,n->as.assign.target,depth+1);
        case NodeCompoundAssign:return obs_expr_type(w,n->as.compound_assign.target,depth+1);
        case NodeBinaryExpr:
            if(n->as.binary.op==TokPlus || n->as.binary.op==TokMinus) {
                t=obs_expr_type(w,n->as.binary.left,depth+1);
                if(t.is_pointer)return t;
            }
            t=NO_TYPE;t.base=TypeI64;return t;
        case NodeBoolLitExpr:t.base=TypeBool;return t;
        case NodeFloatLitExpr:t.base=TypeF64;return t;
        case NodeIntLitExpr:case NodeCharLitExpr:case NodeSizeofExpr:t.base=TypeI64;return t;
        default:return t;
    }
}
static boolean_t obs_conversion(obs_walk *w,node_t *value,type_info_t target) {
    if(value && target.is_pointer && !obs_expr_type(w,value,0).is_pointer)
        return obs_error(value,"integer-to-pointer or unknown pointer construction");
    return True;
}

static void obs_exclude(obs_walk *w, node_t *n, unsigned reason) {
    if (!n || !n->observation_line) return;
    uint32_t end = (uint32_t)n->observation_end_line;
    if (end > w->cg->observation_config->source_line_count) end = w->cg->observation_config->source_line_count;
    for (uint32_t line = (uint32_t)n->observation_line; line <= end; ++line)
        if (w->cg->observation_manifest->exclusions[line] != STS_OBS_ELIGIBLE)
            w->cg->observation_manifest->exclusions[line] = (uint8_t)reason;
}

static void obs_anchor(obs_walk *w, node_t *n) {
    if (!n || w->reason != STS_OBS_ELIGIBLE) return;
    n->observation_hit_id = (uint32_t)n->observation_line;
    w->cg->observation_manifest->exclusions[n->observation_line] = STS_OBS_ELIGIBLE;
}

static boolean_t obs_site(obs_walk *w, node_t *n, uint32_t kind) {
    sts_observation_manifest *m = w->cg->observation_manifest;
    if (m->site_count >= STASHA_OBSERVATION_MAX_SITES) return obs_error(n, "allocation-site capacity");
    /* Unreachable source is classified, but cannot emit an allocation event. */
    if (w->reason != STS_OBS_ELIGIBLE && w->reason != STS_OBS_TEST) return True;
    uint32_t id = m->site_count++;
    m->sites[id] = (sts_observation_site){(uint32_t)n->observation_line,kind,w->owner_test,Null};
    m->sites[id].owner = strdup(w->owner ? w->owner : "global");
    if (!m->sites[id].owner) return obs_error(n, "allocation-site owner storage");
    n->observation_site_id = id + 1;
    return True;
}

static boolean_t obs_terminates(cg_t *cg, node_t *n) {
    if (!n) return False;
    if (n->kind == NodeRetStmt || n->kind == NodeBreakStmt || n->kind == NodeContinueStmt) return True;
    if (n->kind == NodeBlock) {
        for (usize_t i = 0; i < n->as.block.stmts.count; ++i)
            if (obs_terminates(cg,n->as.block.stmts.items[i])) return True;
    }
    if (n->kind == NodeComptimeIf) {
        const char *key=n->as.comptime_if.key;
        boolean_t selected=(!strcmp(key,"os") || !strcmp(key,"platform"))
            ? sts_target_os_matches(&cg->target_spec,n->as.comptime_if.value)
            : !strcmp(key,"arch") && sts_target_arch_matches(&cg->target_spec,n->as.comptime_if.value);
        return obs_terminates(cg,selected?n->as.comptime_if.body:n->as.comptime_if.else_body);
    }
    return False;
}

static boolean_t obs_node(obs_walk *w, node_t *n, boolean_t statement);
static boolean_t obs_list(obs_walk *w, node_list_t list) {
    for (usize_t i = 0; i < list.count; ++i) if (!obs_node(w,list.items[i],False)) return False;
    return True;
}
static boolean_t obs_static(node_t *n) {
    if (!n) return True;
    switch (n->kind) {
        case NodeIntLitExpr: case NodeFloatLitExpr: case NodeCharLitExpr:
        case NodeBoolLitExpr: case NodeNilExpr: case NodeSizeofExpr: return True;
        case NodeStrLitExpr: return !n->as.str_lit.is_heap;
        case NodeUnaryPrefixExpr: return obs_static(n->as.unary.operand);
        case NodeBinaryExpr: return obs_static(n->as.binary.left) && obs_static(n->as.binary.right);
        case NodeCastExpr: return obs_static(n->as.cast_expr.expr) && !n->as.cast_expr.target.is_pointer;
        case NodeCompoundInit:
            for (usize_t i=0;i<n->as.compound_init.items.count;++i)
                if (!obs_static(n->as.compound_init.items.items[i])) return False;
            return True;
        case NodeInitIndex: return obs_static(n->as.init_index.index) && obs_static(n->as.init_index.value);
        default: return False;
    }
}

static boolean_t obs_variable(obs_walk *w, node_t *n, boolean_t runtime) {
    if (!obs_type(n,n->as.var_decl.type,False) || n->as.var_decl.attr_flags ||
        n->as.var_decl.bitfield_width ||
        (n->as.var_decl.flags & ~(VdeclConst|VdeclFinal|VdeclArray)) ||
        ((n->as.var_decl.flags & VdeclArray) && n->as.var_decl.type.is_pointer) ||
        (n->as.var_decl.storage == StorageHeap && !n->as.var_decl.type.is_pointer))
        return obs_error(n, "unsupported storage, declaration attribute or implicit allocation");
    if (n->as.var_decl.array_ndim < 0 || n->as.var_decl.array_ndim > 8)
        return obs_error(n,"array rank");
    for (int i=0;i<n->as.var_decl.array_ndim;++i)
        if (n->as.var_decl.array_sizes[i] < 0)
            return obs_error(n,"non-fixed array extent");
    if (!runtime && !obs_static(n->as.var_decl.init)) return obs_error(n,"non-static global initialization");
    if (!(n->as.var_decl.flags & VdeclArray) && !obs_conversion(w,n->as.var_decl.init,n->as.var_decl.type)) return False;
    boolean_t discarded=!strcmp(n->as.var_decl.name,"_");
    unsigned saved_reason=w->reason;
    if(discarded) {w->reason=STS_OBS_NONEXEC;obs_exclude(w,n,STS_OBS_NONEXEC);}
    else if (runtime) obs_anchor(w,n);
    boolean_t ok=obs_node(w,n->as.var_decl.init,False);
    w->reason=saved_reason;
    if(!ok)return False;
    return discarded?True:obs_bind(w,n);
}

static boolean_t obs_node_inner(obs_walk *w, node_t *n, boolean_t statement) {
    if (!obs_origin(w,n)) return False;
    if (n->headers) return obs_error(n,"statement/expression header attribute");
    if (statement && n->kind != NodeVarDecl && n->kind != NodeBlock && n->kind != NodeDeferStmt &&
        n->kind != NodeComptimeIf && n->kind != NodeComptimeAssert) obs_anchor(w,n);
#define ON(x) do { if (!obs_node(w,(x),False)) return False; } while (0)
#define OS(x) do { if (!obs_node(w,(x),True)) return False; } while (0)
#define OA(x) do { obs_anchor(w,(x)); ON(x); } while (0)
    switch (n->kind) {
        case NodeBlock: {
            size_t saved = w->binding_count; unsigned reason = w->reason;
            for (usize_t i=0;i<n->as.block.stmts.count;++i) {
                node_t *s=n->as.block.stmts.items[i];
                if (w->reason == STS_OBS_UNREACHABLE) obs_exclude(w,s,STS_OBS_UNREACHABLE);
                OS(s);
                if (obs_terminates(w->cg,s)) w->reason=STS_OBS_UNREACHABLE;
            }
            w->binding_count=saved; w->reason=reason; return True;
        }
        case NodeVarDecl: return obs_variable(w,n,True);
        case NodeExprStmt: ON(n->as.expr_stmt.expr); return True;
        case NodeRetStmt:
            for(usize_t i=0;i<n->as.ret_stmt.values.count;++i)
                if(!obs_conversion(w,n->as.ret_stmt.values.items[i],w->return_type))return False;
            return obs_list(w,n->as.ret_stmt.values);
        case NodeBreakStmt: case NodeContinueStmt: return True;
        case NodePrintStmt: return obs_list(w,n->as.print_stmt.args);
        case NodeIfStmt: {
            OA(n->as.if_stmt.cond); OS(n->as.if_stmt.then_block); OS(n->as.if_stmt.else_block); return True;
        }
        case NodeForStmt: {
            size_t saved=w->binding_count;
            if (n->as.for_stmt.init && n->as.for_stmt.init->kind==NodeVarDecl) OS(n->as.for_stmt.init);
            else OA(n->as.for_stmt.init);
            OA(n->as.for_stmt.cond); OA(n->as.for_stmt.update); OS(n->as.for_stmt.body);
            w->binding_count=saved; return True;
        }
        case NodeWhileStmt: OA(n->as.while_stmt.cond); OS(n->as.while_stmt.body); return True;
        case NodeDoWhileStmt: OS(n->as.do_while_stmt.body); OA(n->as.do_while_stmt.cond); return True;
        case NodeInfLoop: OS(n->as.inf_loop.body); return True;
        case NodeDeferStmt: OS(n->as.defer_stmt.body); return True;
        case NodeMatchStmt: {
            OA(n->as.match_stmt.expr);type_info_t old=w->match_type;
            w->match_type=obs_expr_type(w,n->as.match_stmt.expr,0);
            if(w->match_type.is_pointer || w->match_type.base<TypeBool || w->match_type.base>TypeU64)
                return obs_error(n,"match discriminant must be primitive integer");
            boolean_t ok=obs_list(w,n->as.match_stmt.arms);w->match_type=old;return ok;
        }
        case NodeMatchArm: {
            if(n->as.match_arm.is_wildcard && n->as.match_arm.guard_expr)
                return obs_error(n,"wildcard match guards are not lowered by this compiler");
            if (n->is_any_arm || n->any_type_name || n->any_bind_name || n->as.match_arm.enum_name || n->as.match_arm.variant_name)
                return obs_error(n,"non-literal integer match arm");
            size_t saved=w->binding_count;
            if(n->as.match_arm.bind_name) {
                node_t binding={.kind=NodeVarDecl,.line=n->line,.observation_line=n->observation_line};
                binding.as.var_decl.name=n->as.match_arm.bind_name;binding.as.var_decl.type=w->match_type;
                if(!obs_bind(w,&binding))return False;
            }
            ON(n->as.match_arm.literal_expr); OA(n->as.match_arm.guard_expr); OS(n->as.match_arm.body);
            w->binding_count=saved;return True;
        }
        case NodeSwitchStmt:
            OA(n->as.switch_stmt.expr); return obs_list(w,n->as.switch_stmt.cases);
        case NodeSwitchCase: {
            unsigned old=w->reason;w->reason=STS_OBS_NONEXEC;
            boolean_t ok=obs_list(w,n->as.switch_case.values);w->reason=old;
            if(!ok)return False;
            OS(n->as.switch_case.body); return True;
        }
        case NodeComptimeIf: {
            if ((n->as.comptime_if.body && n->as.comptime_if.body->kind!=NodeBlock) ||
                (n->as.comptime_if.else_body && n->as.comptime_if.else_body->kind!=NodeBlock))
                return obs_error(n,"chained compile-time conditional body");
            boolean_t selected;
            const char *key=n->as.comptime_if.key;
            if (!strcmp(key,"os") || !strcmp(key,"platform")) selected=sts_target_os_matches(&w->cg->target_spec,n->as.comptime_if.value);
            else if (!strcmp(key,"arch")) selected=sts_target_arch_matches(&w->cg->target_spec,n->as.comptime_if.value);
            else return obs_error(n,"unknown compile-time selector");
            node_t *active=selected?n->as.comptime_if.body:n->as.comptime_if.else_body;
            node_t *inactive=selected?n->as.comptime_if.else_body:n->as.comptime_if.body;
            obs_exclude(w,inactive,STS_OBS_INACTIVE); OS(active); return True;
        }
        case NodeComptimeAssert: {
            if (!obs_static(n->as.comptime_assert.expr)) return obs_error(n,"non-static compile-time assertion");
            unsigned old=w->reason;w->reason=STS_OBS_NONEXEC;obs_exclude(w,n,STS_OBS_NONEXEC);
            boolean_t ok=obs_node(w,n->as.comptime_assert.expr,False);w->reason=old;return ok;
        }
        case NodeBinaryExpr:
            if (n->as.binary.op==TokAmpAmp || n->as.binary.op==TokPipePipe) {
                OA(n->as.binary.left); OA(n->as.binary.right);
            } else { ON(n->as.binary.left); ON(n->as.binary.right); }
            return True;
        case NodeUnaryPrefixExpr: case NodeUnaryPostfixExpr: ON(n->as.unary.operand); return True;
        case NodeAssignExpr:
            ON(n->as.assign.target); ON(n->as.assign.value);
            return obs_conversion(w,n->as.assign.value,obs_expr_type(w,n->as.assign.target,0));
        case NodeCompoundAssign: ON(n->as.compound_assign.target); ON(n->as.compound_assign.value); return True;
        case NodeCallExpr: {
            if (obs_bound(w,n->as.call.callee) || obs_reserved(n->as.call.callee) || !obs_function(w->cg,n->as.call.callee))
                return obs_error(n,"call must resolve to an admitted, directly named source function");
            node_t *f=obs_function(w->cg,n->as.call.callee);
            for(usize_t i=0;i<n->as.call.args.count && i<f->as.fn_decl.params.count;++i)
                if(!obs_conversion(w,n->as.call.args.items[i],f->as.fn_decl.params.items[i]->as.var_decl.type))return False;
            return obs_list(w,n->as.call.args);
        }
        case NodeIdentExpr:
            if (!obs_bound(w,n->as.ident.name) && (obs_reserved(n->as.ident.name) || obs_any_function(w->cg,n->as.ident.name)))
                return obs_error(n,"native symbol or source function used as a value");
            return True;
        case NodeIntLitExpr: case NodeFloatLitExpr: case NodeCharLitExpr:
        case NodeBoolLitExpr: case NodeNilExpr: return True;
        case NodeStrLitExpr:
            return n->as.str_lit.is_heap ? obs_error(n,"implicit heap string allocation") : True;
        case NodeIndexExpr: ON(n->as.index_expr.object); ON(n->as.index_expr.index); return True;
        case NodeTernaryExpr:
            OA(n->as.ternary.cond); OA(n->as.ternary.then_expr); OA(n->as.ternary.else_expr); return True;
        case NodeCastExpr:
            if (!obs_type(n,n->as.cast_expr.target,False)) return False;
            if(!obs_conversion(w,n->as.cast_expr.expr,n->as.cast_expr.target))return False;
            ON(n->as.cast_expr.expr); return True;
        case NodeSizeofExpr: return obs_type(n,n->as.sizeof_expr.type,True);
        case NodeAddrOf: ON(n->as.addr_of.operand); return True;
        case NodeNewExpr:
            if (!obs_site(w,n,STS_OBS_NEW)) return False;
            ON(n->as.new_expr.size); return True;
        case NodeMovExpr:
            if (n->as.mov_expr.zone_name) return obs_error(n,"zone resize/move");
            if (!obs_site(w,n,STS_OBS_MOV)) return False;
            ON(n->as.mov_expr.ptr); ON(n->as.mov_expr.size); return True;
        case NodeRemStmt:
            if (!obs_site(w,n,STS_OBS_REM)) return False;
            ON(n->as.rem_stmt.ptr); return True;
        case NodeCompoundInit: return obs_list(w,n->as.compound_init.items);
        case NodeInitIndex: ON(n->as.init_index.index); ON(n->as.init_index.value); return True;
        case NodeExpectExpr: ON(n->as.expect_expr.expr); return True;
        case NodeExpectEqExpr: ON(n->as.expect_eq.left); ON(n->as.expect_eq.right); return True;
        case NodeExpectNeqExpr: ON(n->as.expect_neq.left); ON(n->as.expect_neq.right); return True;
        case NodeTestFailExpr:
            if (!n->as.test_fail.message || n->as.test_fail.message->kind!=NodeStrLitExpr)
                return obs_error(n,"test failure requires literal message");
            ON(n->as.test_fail.message); return True;
        default: return obs_error(n,"unclassified syntax or runtime family");
    }
#undef ON
#undef OS
#undef OA
}
static boolean_t obs_node(obs_walk *w, node_t *n, boolean_t statement) {
    if (!n) return True;
    if (++w->depth > 1024) { --w->depth; return obs_error(n,"source inventory nesting limit"); }
    boolean_t result=obs_node_inner(w,n,statement);
    --w->depth; return result;
}

static boolean_t cg_observation_prepare(cg_t *cg) {
    if (!cg->observation_config) return True;
    const sts_observation_config *cfg=cg->observation_config;
    sts_observation_manifest *m=cg->observation_manifest;
    if (!m || !cg->test_mode || codegen_observation_check_providers(cg->ast,cfg)!=Ok) return False;
    if (!cfg->source_line_count || cfg->source_line_count>1000000)
        return obs_error(cg->ast,"source line inventory capacity");
    m->exclusions=(malloc)((size_t)cfg->source_line_count+1);
    m->lines=calloc(STASHA_OBSERVATION_MAX_LINES,sizeof(*m->lines));
    m->sites=calloc(STASHA_OBSERVATION_MAX_SITES,sizeof(*m->sites));
    m->tests=calloc(STASHA_OBSERVATION_MAX_TESTS,sizeof(*m->tests));
    cg->observation_emitted=calloc((size_t)cfg->source_line_count+1,1);
    if (!m->exclusions || !m->lines || !m->sites || !m->tests || !cg->observation_emitted)
        return obs_error(cg->ast,"source inventory allocation failure");
    memset(m->exclusions,STS_OBS_NONEXEC,(size_t)cfg->source_line_count+1);
    obs_walk w={.cg=cg,.owner_test=UINT32_MAX,.reason=STS_OBS_ELIGIBLE};
    boolean_t ok=False,skip=False;
    if (!obs_origin(&w,cg->ast) || !obs_headers(cg,cg->ast,&skip) || skip) goto done;
    /* Globals are visible in all admitted bodies, independent of source order. */
    for (usize_t i=0;i<cg->ast->as.module.decls.count;++i) {
        node_t *d=cg->ast->as.module.decls.items[i];
        const char *name=d->kind==NodeFnDecl?d->as.fn_decl.name:d->kind==NodeVarDecl?d->as.var_decl.name:Null;
        if (name) {
            if (obs_reserved(name) || (d->kind==NodeVarDecl && !strcmp(name,"main"))) {
                obs_error(d,"declaration must be a nonreserved ASCII identifier of at most 127 bytes"); goto done;
            }
            for (usize_t j=0;j<i;++j) {
                node_t *prev=cg->ast->as.module.decls.items[j];
                const char *other=prev->kind==NodeFnDecl?prev->as.fn_decl.name:prev->kind==NodeVarDecl?prev->as.var_decl.name:Null;
                if (other && !strcmp(name,other)) { obs_error(d,"duplicate top-level symbol"); goto done; }
            }
        }
        if (!obs_origin(&w,d) || !obs_headers(cg,d,&skip)) goto done;
        if (skip) { obs_exclude(&w,d,STS_OBS_INACTIVE); continue; }
        if (d->kind==NodeVarDecl) {
            unsigned saved=w.reason; w.reason=STS_OBS_NONEXEC;
            if (!obs_variable(&w,d,False)) goto done;
            w.reason=saved;
        }
    }
    size_t globals=w.binding_count;
    for (usize_t i=0;i<cg->ast->as.module.decls.count;++i) {
        node_t *d=cg->ast->as.module.decls.items[i];
        if (!obs_headers(cg,d,&skip)) goto done;
        if (skip || d->kind==NodeVarDecl) continue;
        w.binding_count=globals; w.owner_test=UINT32_MAX; w.reason=STS_OBS_ELIGIBLE;
        if (d->kind==NodeFnDecl) {
            if (d->as.fn_decl.is_method || d->as.fn_decl.is_variadic || d->as.fn_decl.is_typed_variadic ||
                d->as.fn_decl.is_async || d->as.fn_decl.type_param_count || d->as.fn_decl.attr_flags ||
                d->as.fn_decl.return_count!=1 || !d->as.fn_decl.body ||
                !obs_type(d,d->as.fn_decl.return_types[0],True)) {
                obs_error(d,"function signature, generic, extern or lifecycle"); goto done;
            }
            if (!strcmp(d->as.fn_decl.name,"main")) { obs_exclude(&w,d,STS_OBS_ROOT_MAIN); continue; }
            w.owner=d->as.fn_decl.name;w.return_type=d->as.fn_decl.return_types[0];
            for (usize_t k=0;k<d->as.fn_decl.params.count;++k) {
                node_t *param=d->as.fn_decl.params.items[k];
                if (!obs_origin(&w,param)) goto done;
                if (param->headers || param->as.var_decl.init || param->as.var_decl.flags ||
                    param->as.var_decl.attr_flags || param->as.var_decl.bitfield_width || param->as.var_decl.array_ndim ||
                    param->as.var_decl.storage<StorageDefault || param->as.var_decl.storage>StorageHeap) {
                    obs_error(param,"unsupported parameter storage or metadata");goto done;
                }
                if (!obs_type(param,param->as.var_decl.type,False) || !obs_bind(&w,param)) goto done;
            }
            if (!obs_node(&w,d->as.fn_decl.body,True)) goto done;
        } else if (d->kind==NodeTestBlock) {
            if (m->test_count>=STASHA_OBSERVATION_MAX_TESTS) { obs_error(d,"test inventory capacity"); goto done; }
            w.owner_test=m->test_count; w.owner=d->as.test_block.name; w.reason=STS_OBS_TEST;w.return_type=NO_TYPE;
            m->tests[m->test_count++]=(sts_observation_test){(uint32_t)d->observation_line,strdup(w.owner)};
            if (!m->tests[m->test_count-1].name) { obs_error(d,"test-name allocation failure"); goto done; }
            obs_exclude(&w,d,STS_OBS_TEST);
            if (!obs_node(&w,d->as.test_block.body,True)) goto done;
        } else if (d->kind==NodeComptimeAssert) {
            w.reason=STS_OBS_NONEXEC; if (!obs_node(&w,d,False)) goto done;
        } else { obs_error(d,"unqualified top-level type or syntax"); goto done; }
    }
    for (uint32_t line=1;line<=cfg->source_line_count;++line) if (m->exclusions[line]==STS_OBS_ELIGIBLE) {
        if (m->line_count>=STASHA_OBSERVATION_MAX_LINES) { obs_error(cg->ast,"coverage line capacity"); goto done; }
        m->lines[m->line_count++]=line;
    }
    if (!m->test_count) { obs_error(cg->ast,"zero discovered tests"); goto done; }
    ok=True;
done:
    (free)(w.bindings); return ok;
}

static LLVMValueRef cg_observation_call(cg_t *cg, unsigned op, LLVMValueRef *args, unsigned count) {
    LLVMValueRef fn=cg->observation_fns[op];
    return LLVMBuildCall2(cg->builder,LLVMGlobalGetValueType(fn),fn,args,count,
        (op==ObsNew||op==ObsMov||op==ObsFinish)?"obs.result":"");
}
static void cg_observation_declare(cg_t *cg) {
    if (!cg->observation_config) return;
    LLVMTypeRef p=LLVMPointerTypeInContext(cg->ctx,0),u=LLVMInt32TypeInContext(cg->ctx),z=LLVMInt64TypeInContext(cg->ctx),v=LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef init[]={p,p,p,p,p,u,u,u,u},one[]={u},newargs[]={z,u},movargs[]={p,z,u},remargs[]={p,u},finish[]={z,z};
    LLVMTypeRef types[]={LLVMFunctionType(v,init,9,0),LLVMFunctionType(v,one,1,0),LLVMFunctionType(v,one,1,0),
        LLVMFunctionType(v,one,1,0),LLVMFunctionType(p,newargs,2,0),LLVMFunctionType(p,movargs,3,0),
        LLVMFunctionType(v,remargs,2,0),LLVMFunctionType(u,finish,2,0)};
    const char *names[]={"__stasha_obs_init","__stasha_obs_test_begin","__stasha_obs_test_end","__stasha_obs_hit",
        "__stasha_obs_new","__stasha_obs_mov","__stasha_obs_rem","__stasha_obs_finish"};
    for (unsigned i=0;i<ObsFunctionCount;++i) cg->observation_fns[i]=LLVMAddFunction(cg->module,names[i],types[i]);
    cg->observation_pass=LLVMAddGlobal(cg->module,z,"__stasha_obs_assert_pass");
    cg->observation_fail=LLVMAddGlobal(cg->module,z,"__stasha_obs_assert_fail");
    LLVMSetInitializer(cg->observation_pass,LLVMConstInt(z,0,0));
    LLVMSetInitializer(cg->observation_fail,LLVMConstInt(z,0,0));
    LLVMSetLinkage(cg->observation_pass,LLVMInternalLinkage);
    LLVMSetLinkage(cg->observation_fail,LLVMInternalLinkage);
}
static void cg_observation_hit(cg_t *cg,node_t *n) {
    if (!cg->observation_config || !n || !n->observation_hit_id) return;
    uint32_t line=n->observation_hit_id,lo=0,hi=cg->observation_manifest->line_count;
    while(lo<hi) {uint32_t mid=lo+(hi-lo)/2;if(cg->observation_manifest->lines[mid]<line)lo=mid+1;else hi=mid;}
    if(lo>=cg->observation_manifest->line_count || cg->observation_manifest->lines[lo]!=line) {obs_error(n,"missing coverage manifest line");return;}
    cg->observation_emitted[line]=1;
    if (!(cg->observation_config->enabled_mask & STASHA_OBSERVATION_COVERAGE)) return;
    LLVMValueRef arg=LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),lo,0);
    cg_observation_call(cg,ObsHit,&arg,1);
}
static LLVMValueRef cg_observation_site_arg(cg_t *cg,node_t *n) {
    if(!n->observation_site_id) obs_error(n,"allocation operation absent from source inventory");
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),n->observation_site_id?n->observation_site_id-1:UINT32_MAX,0);
}
static void cg_observation_assertion(cg_t *cg,boolean_t passed) {
    if(!cg->observation_config)return;
    LLVMTypeRef t=LLVMInt64TypeInContext(cg->ctx);
    LLVMValueRef slot=passed?cg->observation_pass:cg->observation_fail;
    LLVMValueRef old=LLVMBuildLoad2(cg->builder,t,slot,"obs.assertions");
    LLVMValueRef full=LLVMBuildICmp(cg->builder,LLVMIntEQ,old,LLVMConstInt(t,UINT64_MAX,0),"obs.full");
    LLVMBasicBlockRef bad=LLVMAppendBasicBlockInContext(cg->ctx,cg->current_fn,"obs.assert.overflow");
    LLVMBasicBlockRef next=LLVMAppendBasicBlockInContext(cg->ctx,cg->current_fn,"obs.assert.next");
    LLVMBuildCondBr(cg->builder,full,bad,next);
    LLVMPositionBuilderAtEnd(cg->builder,bad);
    LLVMValueRef trap=LLVMGetNamedFunction(cg->module,"llvm.trap");
    LLVMTypeRef tt=LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx),Null,0,0);
    if(!trap)trap=LLVMAddFunction(cg->module,"llvm.trap",tt);
    LLVMBuildCall2(cg->builder,tt,trap,Null,0,""); LLVMBuildUnreachable(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder,next);
    LLVMBuildStore(cg->builder,LLVMBuildAdd(cg->builder,old,LLVMConstInt(t,1,0),"obs.increment"),slot);
}

static void obs_hash_text(sts_sha256_ctx *ctx,const char *s) {sts_sha256_update(ctx,(const uint8_t *)s,(uint64_t)strlen(s));}
static boolean_t cg_observation_seal(cg_t *cg) {
    if(!cg->observation_config)return True;
    const sts_observation_config *c=cg->observation_config;sts_observation_manifest *m=cg->observation_manifest;
    sts_sha256_ctx hash;sts_sha256_init(&hash);char buf[256];
    obs_hash_text(&hash,"stasha-observation-map-v1\n");obs_hash_text(&hash,c->source_id);obs_hash_text(&hash,"\n");
    obs_hash_text(&hash,c->config_id);obs_hash_text(&hash,"\n");
    snprintf(buf,sizeof(buf),"source_path %zu:",strlen(c->source_path));obs_hash_text(&hash,buf);
    obs_hash_text(&hash,c->source_path);obs_hash_text(&hash,"\n");
    snprintf(buf,sizeof(buf),"inventory %u %u %u\n",m->test_count,m->line_count,m->site_count);obs_hash_text(&hash,buf);
    for(uint32_t i=0;i<m->line_count;++i) {
        if(!cg->observation_emitted[m->lines[i]])return obs_error(cg->ast,"eligible source line was not lowered");
        snprintf(buf,sizeof(buf),"line %u %u\n",i,m->lines[i]);obs_hash_text(&hash,buf);
    }
    for(uint32_t i=0;i<m->test_count;++i){snprintf(buf,sizeof(buf),"test %u %u %zu:",i,m->tests[i].line,strlen(m->tests[i].name));obs_hash_text(&hash,buf);obs_hash_text(&hash,m->tests[i].name);obs_hash_text(&hash,"\n");}
    for(uint32_t i=0;i<m->site_count;++i){sts_observation_site*s=&m->sites[i];snprintf(buf,sizeof(buf),"site %u %u %u %u %zu:",i,s->line,s->kind,s->owner_test,strlen(s->owner));obs_hash_text(&hash,buf);obs_hash_text(&hash,s->owner);obs_hash_text(&hash,"\n");}
    sts_sha256_update(&hash,m->exclusions,(uint64_t)c->source_line_count+1);
    sts_observation_digest_finish(&hash,m->map_id);return True;
}
static void cg_observation_init_main(cg_t *cg) {
    if(!cg->observation_config)return;
    const sts_observation_config*c=cg->observation_config;sts_observation_manifest*m=cg->observation_manifest;
    const char*ids[]={c->run_id,c->source_id,m->map_id,c->config_id,c->compiler_id};LLVMValueRef args[9];
    for(unsigned i=0;i<5;++i)args[i]=LLVMBuildGlobalStringPtr(cg->builder,ids[i],"obs.identity");
    uint32_t counts[]={c->enabled_mask,m->test_count,m->line_count,m->site_count};
    for(unsigned i=0;i<4;++i)args[i+5]=LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),counts[i],0);
    cg_observation_call(cg,ObsInit,args,9);
}
static void cg_observation_test_event(cg_t *cg,uint32_t test,boolean_t end) {
    if(!cg->observation_config)return;
    LLVMValueRef arg=LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),test,0);cg_observation_call(cg,end?ObsEnd:ObsBegin,&arg,1);
}
static LLVMValueRef cg_observation_finish(cg_t *cg,LLVMValueRef exit_code) {
    if(!cg->observation_config)return exit_code;
    LLVMTypeRef t=LLVMInt64TypeInContext(cg->ctx);
    LLVMValueRef args[]={LLVMBuildLoad2(cg->builder,t,cg->observation_pass,"obs.passed"),LLVMBuildLoad2(cg->builder,t,cg->observation_fail,"obs.failed")};
    LLVMValueRef status=cg_observation_call(cg,ObsFinish,args,2);
    LLVMValueRef bad=LLVMBuildICmp(cg->builder,LLVMIntNE,status,LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),0,0),"obs.incomplete");
    return LLVMBuildOr(cg->builder,exit_code,bad,"obs.exit");
}

static boolean_t cg_observation_call_guard(cg_t *cg,node_t *n) {
    if(!cg->observation_config)return True;
    symbol_t *s=cg_lookup(cg,n->as.call.callee);
    node_t *body=obs_function(cg,n->as.call.callee);
    if(!body || !s || !LLVMIsAFunction(s->value))
        return obs_error(n,"call resolution escaped admitted source body");
    return True;
}
static boolean_t cg_observation_expr_guard(cg_t *cg,node_t *n) {
    if(!cg->observation_config)return True;
    if(n->kind==NodeIdentExpr) {
        symbol_t*s=cg_lookup(cg,n->as.ident.name);
        if(s && LLVMIsAFunction(s->value))return obs_error(n,"function value");
    }
    if(n->kind==NodeCastExpr && n->as.cast_expr.target.is_pointer) {
        type_info_t from=scalar_expr_type(cg,n->as.cast_expr.expr,NO_TYPE);
        if(!from.is_pointer && n->as.cast_expr.expr->kind!=NodeNilExpr)
            return obs_error(n,"integer-to-pointer construction");
    }
    return True;
}
static boolean_t cg_observation_audit(cg_t *cg) {
    if(!cg->observation_config)return True;
    static const char*allowed[]={"printf","fprintf","puts","putchar","fputs","fwrite","memcmp","memcpy","memmove","memset",
        "__crash_runtime_install","__crash_runtime_register_block","__stasha_obs_init","__stasha_obs_test_begin","__stasha_obs_test_end",
        "__stasha_obs_hit","__stasha_obs_new","__stasha_obs_mov","__stasha_obs_rem","__stasha_obs_finish"};
    for(LLVMValueRef fn=LLVMGetFirstFunction(cg->module);fn;fn=LLVMGetNextFunction(fn))
        for(LLVMBasicBlockRef b=LLVMGetFirstBasicBlock(fn);b;b=LLVMGetNextBasicBlock(b))
            for(LLVMValueRef in=LLVMGetFirstInstruction(b);in;in=LLVMGetNextInstruction(in)) {
                if(LLVMGetInstructionOpcode(in)==LLVMIntToPtr)return obs_error(cg->ast,"generated integer-to-pointer construction");
                if(LLVMGetInstructionOpcode(in)!=LLVMCall && LLVMGetInstructionOpcode(in)!=LLVMInvoke)continue;
                LLVMValueRef callee=LLVMGetCalledValue(in);
                if(!LLVMIsAFunction(callee))return obs_error(cg->ast,"generated indirect call or inline assembly");
                if(!LLVMIsDeclaration(callee))continue;
                const char*name=LLVMGetValueName(callee);boolean_t admitted=False;
                /* Qualified compiler intrinsics have no user-call spelling in this profile. */
                if(!strncmp(name,"llvm.",5)) {
                    const char*prefixes[]={"llvm.memcpy.","llvm.memmove.","llvm.memset.","llvm.trap","llvm.ctlz.","llvm.sadd.with.overflow.","llvm.ssub.with.overflow.","llvm.smul.with.overflow.","llvm.uadd.with.overflow.","llvm.usub.with.overflow.","llvm.umul.with.overflow.","llvm.lifetime.start.","llvm.lifetime.end.","llvm.dbg.","llvm.assume","llvm.abs.","llvm.smax.","llvm.smin.","llvm.umax.","llvm.umin.","llvm.fshl.","llvm.fshr."};
                    for(size_t k=0;k<sizeof(prefixes)/sizeof(prefixes[0]);++k)if(!strncmp(name,prefixes[k],strlen(prefixes[k])))admitted=True;
                }
                for(size_t k=0;k<sizeof(allowed)/sizeof(allowed[0]);++k)if(!strcmp(name,allowed[k]))admitted=True;
                if(!admitted){diag_begin_error("unsupported by observation profile: generated external call '%s'",name);diag_finish();return False;}
            }
    return True;
}
