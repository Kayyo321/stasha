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


_Static_assert(NodeFnDecl==STS_OBS_DECL_FN && NodeVarDecl==STS_OBS_DECL_VAR &&
    NodeTypeDecl==STS_OBS_DECL_TYPE && NodeImpDecl==STS_OBS_DECL_IMPORT &&
    NodeComptimeAssert==STS_OBS_DECL_COMPTIME_ASSERT && NodeTestBlock==STS_OBS_DECL_TEST,
    "observation v2 declaration schema must match reviewed AST enum");

static const sts_observation_graph *obs_diagnostic_graph;

static boolean_t obs_error(node_t *n, const char *what) {
    const char *saved = diag_get_file();
    if (n && n->observation_file) diag_set_file(n->observation_file);
    diag_begin_error("unsupported by observation profile: %s", what);
    if (n) diag_span(SRC_LOC(n->observation_line ? n->observation_line : n->line,
        n->observation_col ? n->observation_col : 1, 1), True, "here");
    if(n && obs_diagnostic_graph && n->observation_file_id_plus_one>1) {
        uint32_t file=n->observation_file_id_plus_one-1;
        for(uint32_t i=0;i<obs_diagnostic_graph->edge_count;++i) {
            const sts_observation_edge*e=&obs_diagnostic_graph->edges[i];
            if(e->child_file_id==file) {
                diag_note("root import occurrence %u (%s) at %s:%u:%u",e->ordinal,
                    e->requested_module,obs_diagnostic_graph->files[0].lookup_path,e->line,e->col);
                break;
            }
        }
    }
    diag_finish();
    if (saved) diag_set_file(saved);
    return False;
}

static result_t obs_token_error(const char *path, token_t t, const char *what) {
    const char *saved = diag_get_file(); diag_set_file(path);
    diag_begin_error("unsupported by observation profile: %s", what);
    diag_span(SRC_LOC(t.line,t.col,t.length),True,"rejected before preprocessing/parsing");
    diag_finish(); if (saved) diag_set_file(saved); return Err;
}
/* Audit the complete original tokens, before parse_dotted_name or parse_imp
 * can discard aliases or truncate spellings. This never opens an import. */
result_t codegen_observation_check_source_file(const char *source, const char *path, int is_root) {
    lexer_t lex; init_lexer(&lex,source);
    unsigned braces=0; boolean_t boundary=True;
    token_kind_t previous=TokEof;
    for (;;) {
        token_t t=next_token(&lex);
        if(t.kind==TokEof)return Ok;
        if(t.kind==TokError)return obs_token_error(path,t,"invalid original token");
        if(t.kind==TokIdent && t.length>STS_OBS_IDENTIFIER_LIMIT)
            return obs_token_error(path,t,"identifier exceeds 127 bytes");
        if(previous==TokTest && (t.kind==TokStackStr || t.kind==TokHeapStr) &&
            (t.length<2 || t.length-2>STS_OBS_LABEL_LIMIT))
            return obs_token_error(path,t,"test label exceeds 4096 bytes");
        switch(t.kind) {
            case TokLib: case TokLibImp: case TokCHeader: case TokMacro:
                return obs_token_error(path,t,"libraries, C providers and macros");
            case TokMod: case TokImp: {
                if(t.kind==TokImp && (!is_root || braces || !boundary))
                    return obs_token_error(path,t,"imports must be unconditional plain root declarations; leaf imports are unsupported");
                size_t size=0; boolean_t component=True;
                for (;;) {
                    token_t q=next_token(&lex);
                    if(component) {
                        if(q.kind!=TokIdent || !q.length || q.length>STS_OBS_IDENTIFIER_LIMIT)
                            return obs_token_error(path,q,"module component must be an ASCII identifier of at most 127 bytes");
                        for(usize_t j=0;j<q.length;++j) {
                            unsigned char c=(unsigned char)q.start[j];
                            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||(j&&c>='0'&&c<='9')))
                                return obs_token_error(path,q,"module component must be ASCII");
                        }
                        size+=q.length; component=False;
                    } else if(q.kind==TokDot) {++size;component=True;}
                    else if(q.kind==TokSemicolon) break;
                    else return obs_token_error(path,q,q.kind==TokEq?"explicit import alias syntax":"invalid complete module spelling");
                    if(size>STS_OBS_MODULE_LIMIT)
                        return obs_token_error(path,q,"complete dotted module spelling exceeds 255 bytes");
                }
                boundary=True;previous=TokSemicolon;continue;
            }
            case TokLBrace: ++braces;boundary=False;break;
            case TokRBrace: if(braces)--braces;boundary=braces==0;break;
            case TokSemicolon: boundary=braces==0;break;
            default: boundary=False;break;
        }
        previous=t.kind;
    }
}
result_t codegen_observation_check_source(const char *source,const char *path) {
    return codegen_observation_check_source_file(source,path,1);
}
result_t codegen_observation_check_providers(node_t *ast, const sts_observation_config *cfg) {
    if(!cfg)return Ok;
    if(!ast || ast->kind!=NodeModule)return obs_error(ast,"missing source module")?Ok:Err;
    if(ast->observation_expanded || ast->from_lib || ast->as.module.freestanding ||
       ast->as.module.kernel_profile || ast->as.module.has_org ||
       (ast->observation_file_id_plus_one>1 && ast->headers))
        return obs_error(ast,"macro/provider/freestanding or imported file-wide header")?Ok:Err;
    for(usize_t i=0;i<ast->as.module.decls.count;++i) {
        node_t*d=ast->as.module.decls.items[i];
        switch(d->kind) {
            case NodeImpDecl:
                if(d->headers || d->observation_file_id_plus_one!=1)
                    return obs_error(d,"only plain root imports are admitted")?Ok:Err;
                break;
            case NodeFnDecl:case NodeVarDecl:case NodeTestBlock:case NodeTypeDecl:case NodeComptimeAssert:break;
            default:return obs_error(d,"top-level declaration or provider")?Ok:Err;
        }
    }
    return Ok;
}

typedef struct {const char*name;type_info_t type;unsigned array_rank;uint32_t file_id;boolean_t global;} obs_binding;
typedef struct {
    cg_t *cg;
    const char *owner;
    uint32_t owner_test, file_id, decl_id, anchor_kind;
    unsigned reason, depth;
    boolean_t global_binding;
    type_info_t return_type,match_type;
    obs_binding *bindings;
    size_t binding_count,binding_capacity;
} obs_walk;

static boolean_t obs_origin(obs_walk*w,node_t*n) {
    const sts_observation_graph*g=w->cg->observation_config->graph;
    if(!n || !g || !n->observation_file_id_plus_one || n->observation_file_id_plus_one>g->file_count)
        return obs_error(n,"missing graph-issued source origin");
    const sts_observation_source*f=&g->files[n->observation_file_id_plus_one-1];
    if(!n->observation_file || !n->observation_line || n->observation_line>f->physical_line_count ||
       n->observation_end_line<n->observation_line || n->observation_end_line>f->physical_line_count ||
       (n->observation_end_line==n->observation_line && n->observation_end_col<n->observation_col) ||
       (n->kind!=NodeModule && (!n->observation_col || !n->observation_end_col)) ||
       strcmp(n->observation_file,f->canonical_path) || n->observation_expanded || n->from_lib ||
       n->generic_use || n->is_c_extern)
        return obs_error(n,"invalid graph source span, expansion, provider or generic use");
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

static const char *obs_decl_name(node_t*d) {
    return d->kind==NodeFnDecl?d->as.fn_decl.name:d->kind==NodeVarDecl?d->as.var_decl.name:Null;
}
static node_t *obs_decl_in_file(cg_t*cg,uint32_t file,const char*name) {
    for(usize_t i=0;i<cg->ast->as.module.decls.count;++i) {
        node_t*d=cg->ast->as.module.decls.items[i];const char*s=obs_decl_name(d);
        if(s && d->observation_file_id_plus_one==file+1 && !strcmp(s,name) &&
           !cg_fh_skip_decl(cg,d->headers,d->line))return d;
    }
    return Null;
}
static node_t *obs_visible_decl(obs_walk*w,const char*name) {
    node_t*d=obs_decl_in_file(w->cg,w->file_id,name);
    return d?d:(w->file_id?obs_decl_in_file(w->cg,0,name):Null);
}
static obs_binding *obs_binding_lookup(obs_walk*w,const char*name) {
    for(size_t i=w->binding_count;i>0;--i)
        if(!w->bindings[i-1].global && !strcmp(w->bindings[i-1].name,name))return &w->bindings[i-1];
    for(unsigned pass=0;pass<(w->file_id?2u:1u);++pass) {
        uint32_t file=pass?0:w->file_id;
        for(size_t i=w->binding_count;i>0;--i)
            if(w->bindings[i-1].global && w->bindings[i-1].file_id==file && !strcmp(w->bindings[i-1].name,name))return &w->bindings[i-1];
        /* A module function also shadows the root's variable. */
        if(obs_decl_in_file(w->cg,file,name))return Null;
    }
    return Null;
}
static boolean_t obs_bound(obs_walk*w,const char*name) {return obs_binding_lookup(w,name)!=Null;}

/* Shape admission is separate from pointer-like expression typing. Consume at
 * most seven ordinary indexes: an admitted rank is at most eight, and a query
 * must leave a dimension. Concrete named/inferred extents belong to lowering. */
static unsigned obs_array_query_rank(obs_walk*w,node_t*n) {
    unsigned indexed=0;
    while(n && n->kind==NodeIndexExpr) {
        if(++indexed>=8)return 0;
        n=n->as.index_expr.object;
    }
    if(!n || n->kind!=NodeIdentExpr)return 0;
    obs_binding*b=obs_binding_lookup(w,n->as.ident.name);
    if(!b || b->type.is_pointer || b->array_rank>8 || b->array_rank<=indexed)return 0;
    return b->array_rank-indexed;
}

static node_t *obs_selected_function(obs_walk*w,node_t*n) {
    node_t*d=Null;
    if(n->kind==NodeCallExpr) {
        if(obs_bound(w,n->as.call.callee) || obs_reserved(n->as.call.callee))return Null;
        d=obs_visible_decl(w,n->as.call.callee);
    } else if(n->kind==NodeMethodCall) {
        node_t*obj=n->as.method_call.object;
        if(!obj || obj->kind!=NodeIdentExpr || obs_bound(w,obj->as.ident.name) ||
           obs_visible_decl(w,obj->as.ident.name))return Null;
        const sts_observation_graph*g=w->cg->observation_config->graph;
        uint32_t found=UINT32_MAX;
        for(uint32_t i=1;i<g->file_count;++i) {
            const char*module=g->files[i].requested_module;
            const char*last=strrchr(module,'.');last=last?last+1:module;
            if(!strcmp(last,obj->as.ident.name)){if(found!=UINT32_MAX)return Null;found=i;}
        }
        if(found==UINT32_MAX)return Null;
        d=obs_decl_in_file(w->cg,found,n->as.method_call.method);
    }
    if(!d || d->kind!=NodeFnDecl || !d->as.fn_decl.body || d->as.fn_decl.type_param_count ||
       d->as.fn_decl.is_method || d->as.fn_decl.is_async ||
       (d->observation_file_id_plus_one==1 && !strcmp(d->as.fn_decl.name,"main")) ||
       (d->observation_file_id_plus_one!=w->file_id+1 && d->as.fn_decl.linkage!=LinkageExternal))return Null;
    return d;
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
        (n->as.var_decl.flags & VdeclArray) ? (unsigned)(n->as.var_decl.array_ndim > 0 ? n->as.var_decl.array_ndim : 1) : 0, w->file_id,w->global_binding};
    return True;
}

/* Bounded source typing for admission, never a replacement for semantic typing.
 * In particular it runs for statements the lowerer skips after a terminator. */
static type_info_t obs_expr_type(obs_walk *w, node_t *n, unsigned depth) {
    type_info_t t=NO_TYPE; t.base=TypeInfer;
    if(!n || depth>1024)return t;
    switch(n->kind) {
        case NodeIdentExpr: {
            obs_binding*b=obs_binding_lookup(w,n->as.ident.name);
            if(b){t=b->type;if(b->array_rank){t.ptr_depth=(t.is_pointer?(t.ptr_depth?t.ptr_depth:1):0)+(int)b->array_rank;t.is_pointer=True;}}
            return t;
        }
        case NodeNewExpr: case NodeNilExpr: t.base=TypeVoid;t.is_pointer=True;t.ptr_depth=1;return t;
        case NodeStrLitExpr: t.base=TypeI8;t.is_pointer=True;t.ptr_depth=1;return t;
        case NodeMovExpr: return obs_expr_type(w,n->as.mov_expr.ptr,depth+1);
        case NodeCastExpr: return n->as.cast_expr.target;
        case NodeCallExpr: case NodeMethodCall: {node_t*f=obs_selected_function(w,n);return f && f->as.fn_decl.return_count==1?f->as.fn_decl.return_types[0]:t;}
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
        case NodeLenExpr:case NodeCapExpr:t.base=TypeI32;return t;
        case NodeIntLitExpr:case NodeCharLitExpr:case NodeSizeofExpr:t.base=TypeI64;return t;
        default:return t;
    }
}
static boolean_t obs_conversion(obs_walk *w,node_t *value,type_info_t target) {
    if(value && target.is_pointer && !obs_expr_type(w,value,0).is_pointer)
        return obs_error(value,"integer-to-pointer or unknown pointer construction");
    return True;
}

static void obs_exclude(obs_walk*w,node_t*n,unsigned reason) {
    if(!n || !n->observation_file_id_plus_one || !n->observation_line)return;
    sts_observation_manifest*m=w->cg->observation_manifest;
    uint32_t file=n->observation_file_id_plus_one-1;
    if(file>=m->file_count)return;
    uint32_t end=(uint32_t)n->observation_end_line;
    if(end>m->files[file].physical_line_count)end=m->files[file].physical_line_count;
    for(uint32_t line=(uint32_t)n->observation_line;line<=end;++line)
        m->files[file].reason_masks[line]|=1u<<reason;
}
static void obs_anchor(obs_walk*w,node_t*n) {
    if(!n)return;
    if(n->observation_file_id_plus_one!=w->file_id+1){obs_error(n,"anchor origin differs from declaration owner file");w->cg->observation_inventory_error=True;return;}
    cg_t*cg=w->cg;sts_observation_manifest*m=cg->observation_manifest;
    if(m->anchor_count>=STS_OBS_ANCHOR_LIMIT){obs_error(n,"source anchor capacity");cg->observation_inventory_error=True;return;}
    uint32_t id=m->anchor_count++;
    m->anchors[id]=(sts_observation_anchor){.anchor_id=id,.decl_id=w->decl_id,.file_id=w->file_id,
        .line=(uint32_t)n->observation_line,.col=n->observation_col,
        .end_line=(uint32_t)n->observation_end_line,.end_col=n->observation_end_col,
        .kind=w->anchor_kind?w->anchor_kind:STS_OBS_ANCHOR_STATEMENT,.exclusion_reason=w->reason};
    cg->observation_anchor_nodes[id]=n;
    if(w->file_id<m->file_count && n->observation_line<=m->files[w->file_id].physical_line_count)
        m->files[w->file_id].reason_masks[n->observation_line]|=1u<<w->reason;
}
static boolean_t obs_site(obs_walk*w,node_t*n,uint32_t kind) {
    sts_observation_manifest*m=w->cg->observation_manifest;
    if(w->reason!=STS_OBS_ELIGIBLE && w->reason!=STS_OBS_TEST)return True;
    if(m->site_count>=STASHA_OBSERVATION_MAX_SITES)return obs_error(n,"allocation-site capacity");
    uint32_t id=m->site_count++;
    m->sites[id]=(sts_observation_site){.line=(uint32_t)n->observation_line,.kind=kind,.owner_test=w->owner_test,
        .owner=strdup(w->owner?w->owner:"global"),.file_id=w->file_id,.decl_id=w->decl_id,
        .col=n->observation_col,.end_line=(uint32_t)n->observation_end_line,.end_col=n->observation_end_col};
    if(!m->sites[id].owner)return obs_error(n,"allocation-site owner storage");
    n->observation_site_id=id+1;return True;
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
    if(n->observation_file_id_plus_one!=w->file_id+1)
        return obs_error(n,"node origin differs from declaration owner file");
    if (n->headers) return obs_error(n,"statement/expression header attribute");
    if (statement && n->kind != NodeVarDecl && n->kind != NodeBlock && n->kind != NodeDeferStmt &&
        n->kind != NodeComptimeIf && n->kind != NodeComptimeAssert) obs_anchor(w,n);
#define ON(x) do { if (!obs_node(w,(x),False)) return False; } while (0)
#define OS(x) do { if (!obs_node(w,(x),True)) return False; } while (0)
#define OA(x) do { uint32_t old_kind=w->anchor_kind; w->anchor_kind=STS_OBS_ANCHOR_CONTROL; obs_anchor(w,(x)); w->anchor_kind=old_kind; ON(x); } while (0)
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
        case NodeDeferStmt: {uint32_t old=w->anchor_kind;w->anchor_kind=STS_OBS_ANCHOR_DEFER;OS(n->as.defer_stmt.body);w->anchor_kind=old;return True;}
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
        case NodeCallExpr: case NodeMethodCall: {
            node_t*f=obs_selected_function(w,n);
            if(!f)return obs_error(n,"call must resolve unambiguously to an admitted direct source function with exported cross-module linkage");
            node_list_t args=n->kind==NodeCallExpr?n->as.call.args:n->as.method_call.args;
            if(args.count!=f->as.fn_decl.params.count)return obs_error(n,"source call primitive signature arity mismatch");
            n->observation_target_decl_id_plus_one=f->observation_decl_id_plus_one;
            if(n->kind==NodeMethodCall && !obs_origin(w,n->as.method_call.object))return False;
            for(usize_t i=0;i<args.count;++i)
                if(!obs_conversion(w,args.items[i],f->as.fn_decl.params.items[i]->as.var_decl.type))return False;
            return obs_list(w,args);
        }
        case NodeIdentExpr: {
            node_t*d=obs_visible_decl(w,n->as.ident.name);
            if(!obs_bound(w,n->as.ident.name) && (obs_reserved(n->as.ident.name) || (d&&d->kind==NodeFnDecl)))
                return obs_error(n,"native symbol or source function used as a value");
            return True;
        }
        case NodeIntLitExpr: case NodeFloatLitExpr: case NodeCharLitExpr:
        case NodeBoolLitExpr: case NodeNilExpr: return True;
        case NodeStrLitExpr:
            return n->as.str_lit.is_heap ? obs_error(n,"implicit heap string allocation") : True;
        case NodeIndexExpr: ON(n->as.index_expr.object); ON(n->as.index_expr.index); return True;
        case NodeLenExpr: case NodeCapExpr:
            if(!obs_array_query_rank(w,n->as.len_expr.operand))
                return obs_error(n,"fixed-array query requires an admitted array with a remaining dimension");
            /* The bounded shape classifier cannot replace the complete walk:
             * nested calls, arms and effects retain their ordinary inventory. */
            ON(n->as.len_expr.operand); return True;
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

static boolean_t obs_symbol_name(node_t*d,char out[512]) {
    const char*name=obs_decl_name(d);if(!name)return True;
    const char*module=d->module_name;size_t length=strlen(name);
    if(module && *module) {
        length+=2+strlen(module);
        for(const char*p=module;*p;++p)if(*p=='.')++length;
    }
    if(length>STS_OBS_SYMBOL_LIMIT)return obs_error(d,"emitted source symbol exceeds 511 bytes");
    mangle_fn(module,name,out,512);
    if(strlen(out)!=length)return obs_error(d,"emitted source symbol truncation");
    return True;
}
static boolean_t cg_observation_prepare(cg_t*cg) {
    if(!cg->observation_config)return True;
    const sts_observation_config*cfg=cg->observation_config;
    const sts_observation_graph*g=cfg->graph;
    obs_diagnostic_graph=g;
    sts_observation_manifest*m=cg->observation_manifest;
    if(!m || !cg->test_mode || !g || !g->frozen || g->failed || !g->file_count ||
       g->file_count>STS_OBS_FILE_LIMIT || strcmp(cfg->input_graph_id,g->input_graph_id) ||
       codegen_observation_check_providers(cg->ast,cfg)!=Ok)return False;
    m->file_count=g->file_count;
    m->files=calloc(m->file_count,sizeof(*m->files));
    m->lines=calloc(STASHA_OBSERVATION_MAX_LINES,sizeof(*m->lines));
    m->line_file_ids=calloc(STASHA_OBSERVATION_MAX_LINES,sizeof(*m->line_file_ids));
    m->sites=calloc(STASHA_OBSERVATION_MAX_SITES,sizeof(*m->sites));
    m->tests=calloc(STASHA_OBSERVATION_MAX_TESTS,sizeof(*m->tests));
    m->declarations=calloc(STS_OBS_DECL_LIMIT,sizeof(*m->declarations));
    m->anchors=calloc(STS_OBS_ANCHOR_LIMIT,sizeof(*m->anchors));
    cg->observation_anchor_nodes=calloc(STS_OBS_ANCHOR_LIMIT,sizeof(*cg->observation_anchor_nodes));
    cg->observation_emitted=calloc(STASHA_OBSERVATION_MAX_LINES,1);
    if(!m->files || !m->lines || !m->line_file_ids || !m->sites || !m->tests ||
       !m->declarations || !m->anchors || !cg->observation_anchor_nodes || !cg->observation_emitted)
        return obs_error(cg->ast,"source inventory allocation failure");
    for(uint32_t f=0;f<m->file_count;++f) {
        const sts_observation_source*src=&g->files[f];
        if(src->file_id!=f || !src->physical_line_count || src->physical_line_count>STS_OBS_SOURCE_LINES)
            return obs_error(cg->ast,"source file identity or line capacity");
        m->files[f].file_id=f;m->files[f].role=src->role;
        m->files[f].physical_line_count=src->physical_line_count;
        strcpy(m->files[f].content_id,src->content_id);
        m->files[f].reason_masks=calloc((size_t)src->physical_line_count+1,sizeof(uint32_t));
        if(!m->files[f].reason_masks)return obs_error(cg->ast,"per-file classification allocation failure");
        for(uint32_t l=1;l<=src->physical_line_count;++l)m->files[f].reason_masks[l]=0x80000000u;
    }
    obs_walk w={.cg=cg,.owner_test=UINT32_MAX,.reason=STS_OBS_ELIGIBLE};
    boolean_t ok=False,skip=False;
    if(!obs_origin(&w,cg->ast) || !obs_headers(cg,cg->ast,&skip) || skip)goto done;
    /* Identity pass precedes every type/body traversal, including forward calls.
     * Flattening order is all root declarations followed by each leaf in order. */
    uint32_t last_file=0;
    for(usize_t i=0;i<cg->ast->as.module.decls.count;++i) {
        node_t*d=cg->ast->as.module.decls.items[i];
        if(!obs_origin(&w,d) || !obs_headers(cg,d,&skip))goto done;
        uint32_t file=d->observation_file_id_plus_one-1;
        if(file<last_file || m->decl_count>=STS_OBS_DECL_LIMIT) {obs_error(d,"declaration order or capacity");goto done;}
        last_file=file;
        if((file && (!d->module_name || strcmp(d->module_name,g->files[file].declared_module))) ||
           (!file && d->module_name && *d->module_name)){obs_error(d,"declaration lexical module disagrees with graph owner");goto done;}
        const char*name=obs_decl_name(d);
        char emitted[512]={0};
        if(name) {
            if(obs_reserved(name) || (d->kind==NodeVarDecl && !file && !strcmp(name,"main")) || !obs_symbol_name(d,emitted)) {
                obs_error(d,"declaration requires a bounded nonreserved source symbol");goto done;
            }
            for(usize_t j=0;j<i;++j) {
                node_t*prev=cg->ast->as.module.decls.items[j];const char*other=obs_decl_name(prev);
                if(!other)continue;
                char previous[512];if(!obs_symbol_name(prev,previous))goto done;
                if((prev->observation_file_id_plus_one==d->observation_file_id_plus_one && !strcmp(name,other)) ||
                   !strcmp(emitted,previous)){obs_error(d,"duplicate scope declaration or emitted source symbol collision");goto done;}
            }
        }
        uint32_t id=m->decl_count++;d->observation_decl_id_plus_one=id+1;
        if(!name && d->kind==NodeTypeDecl) {
            name=d->as.type_decl.name;
            if(obs_reserved(name)){obs_error(d,"inactive type name must be a bounded nonreserved identifier");goto done;}
        }
        if(!name)name="";
        m->declarations[id]=(sts_observation_declaration){.decl_id=id,.file_id=file,
            .ordinal_in_file=d->observation_decl_ordinal,.kind=(uint32_t)d->kind,
            .line=(uint32_t)d->observation_line,.col=d->observation_col,
            .end_line=(uint32_t)d->observation_end_line,.end_col=d->observation_end_col,
            .module_name=strdup(g->files[file].declared_module),.name=strdup(name?name:""),
            .source_linkage=d->kind==NodeFnDecl?d->as.fn_decl.linkage:d->kind==NodeVarDecl?d->as.var_decl.linkage:LinkageNone,
            .selected=!skip};
        if(!m->declarations[id].module_name || !m->declarations[id].name){obs_error(d,"owned declaration metadata allocation failure");goto done;}
        if(skip)obs_exclude(&w,d,STS_OBS_INACTIVE);
    }
    /* Bind global values with lexical file IDs, never one merged bare namespace. */
    for(usize_t i=0;i<cg->ast->as.module.decls.count;++i) {
        node_t*d=cg->ast->as.module.decls.items[i];
        if(d->kind!=NodeVarDecl || !m->declarations[d->observation_decl_id_plus_one-1].selected)continue;
        w.file_id=d->observation_file_id_plus_one-1;w.decl_id=d->observation_decl_id_plus_one-1;
        w.reason=STS_OBS_NONEXEC;w.global_binding=True;obs_exclude(&w,d,STS_OBS_NONEXEC);
        if(!obs_variable(&w,d,False))goto done;
    }
    size_t globals=w.binding_count;w.global_binding=False;
    for(usize_t i=0;i<cg->ast->as.module.decls.count;++i) {
        node_t*d=cg->ast->as.module.decls.items[i];
        sts_observation_declaration*identity=&m->declarations[d->observation_decl_id_plus_one-1];
        if(!identity->selected || d->kind==NodeVarDecl)continue;
        w.file_id=d->observation_file_id_plus_one-1;w.decl_id=d->observation_decl_id_plus_one-1;
        w.binding_count=globals;w.owner_test=UINT32_MAX;w.reason=STS_OBS_ELIGIBLE;
        if(d->kind==NodeFnDecl) {
            if(d->as.fn_decl.is_method || d->as.fn_decl.is_variadic || d->as.fn_decl.is_typed_variadic ||
               d->as.fn_decl.is_async || d->as.fn_decl.type_param_count || d->as.fn_decl.attr_flags ||
               d->as.fn_decl.return_count!=1 || !d->as.fn_decl.body || !obs_type(d,d->as.fn_decl.return_types[0],True)) {
                obs_error(d,"function signature, generic, extern or lifecycle");goto done;
            }
            if(!w.file_id && !strcmp(d->as.fn_decl.name,"main")) {
                identity->selected=0;obs_exclude(&w,d,STS_OBS_ROOT_MAIN);continue;
            }
            w.owner=d->as.fn_decl.name;w.return_type=d->as.fn_decl.return_types[0];
            for(usize_t k=0;k<d->as.fn_decl.params.count;++k) {
                node_t*param=d->as.fn_decl.params.items[k];if(!obs_origin(&w,param) || param->observation_file_id_plus_one!=w.file_id+1) {obs_error(param,"parameter origin differs from declaration owner file");goto done;}
                if(param->headers || param->as.var_decl.init || param->as.var_decl.flags ||
                   param->as.var_decl.attr_flags || param->as.var_decl.bitfield_width || param->as.var_decl.array_ndim ||
                   param->as.var_decl.storage<StorageDefault || param->as.var_decl.storage>StorageHeap) {
                    obs_error(param,"unsupported parameter storage or metadata");goto done;
                }
                if(!obs_type(param,param->as.var_decl.type,False) || !obs_bind(&w,param))goto done;
            }
            if(!obs_node(&w,d->as.fn_decl.body,True))goto done;
        } else if(d->kind==NodeTestBlock) {
            if(m->test_count>=STASHA_OBSERVATION_MAX_TESTS){obs_error(d,"test inventory capacity");goto done;}
            w.owner_test=m->test_count;w.owner=d->as.test_block.name;w.reason=STS_OBS_TEST;w.return_type=NO_TYPE;
            m->tests[m->test_count++]=(sts_observation_test){.line=(uint32_t)d->observation_line,.name=strdup(w.owner),
                .file_id=w.file_id,.decl_id=w.decl_id,.col=d->observation_col};
            if(!m->tests[m->test_count-1].name){obs_error(d,"test-name allocation failure");goto done;}
            obs_exclude(&w,d,STS_OBS_TEST);if(!obs_node(&w,d->as.test_block.body,True))goto done;
        } else if(d->kind==NodeComptimeAssert) {
            w.reason=STS_OBS_NONEXEC;if(!obs_node(&w,d,False))goto done;
        } else if(d->kind==NodeImpDecl) {obs_exclude(&w,d,STS_OBS_NONEXEC);}
        else {obs_error(d,"unqualified top-level type or syntax");goto done;}
    }
    if(cg->observation_inventory_error)goto done;
    for(uint32_t f=0;f<m->file_count;++f)for(uint32_t line=1;line<=m->files[f].physical_line_count;++line) {
        uint32_t*mask=&m->files[f].reason_masks[line];
        *mask&=~0x80000000u;if(!*mask)*mask=1u<<STS_OBS_NONEXEC;
        if(*mask&(1u<<STS_OBS_ELIGIBLE)) {
            if(m->line_count>=STASHA_OBSERVATION_MAX_LINES){obs_error(cg->ast,"coverage line capacity");goto done;}
            m->line_file_ids[m->line_count]=f;m->lines[m->line_count++]=line;
        }
    }
    for(uint32_t i=0;i<m->anchor_count;++i) {
        sts_observation_anchor*a=&m->anchors[i];
        if(a->exclusion_reason!=STS_OBS_ELIGIBLE)continue;
        uint32_t lo=0,hi=m->line_count;
        while(lo<hi){uint32_t mid=lo+(hi-lo)/2;if(m->line_file_ids[mid]<a->file_id ||
            (m->line_file_ids[mid]==a->file_id && m->lines[mid]<a->line))lo=mid+1;else hi=mid;}
        if(lo>=m->line_count || m->line_file_ids[lo]!=a->file_id || m->lines[lo]!=a->line){obs_error(cg->ast,"anchor coverage identity");goto done;}
        a->has_coverage_id=1;a->coverage_id=lo;cg->observation_anchor_nodes[i]->observation_hit_id=lo+1;
    }
    if(!m->test_count){obs_error(cg->ast,"zero discovered tests");goto done;}
    ok=True;
done:
    (free)(w.bindings);return ok;
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
    const char *names[]={"__stasha_obs_init_v2","__stasha_obs_test_begin","__stasha_obs_test_end","__stasha_obs_hit",
        "__stasha_obs_new","__stasha_obs_mov","__stasha_obs_rem","__stasha_obs_finish"};
    for (unsigned i=0;i<ObsFunctionCount;++i) cg->observation_fns[i]=LLVMAddFunction(cg->module,names[i],types[i]);
    cg->observation_pass=LLVMAddGlobal(cg->module,z,"__stasha_obs_assert_pass");
    cg->observation_fail=LLVMAddGlobal(cg->module,z,"__stasha_obs_assert_fail");
    LLVMSetInitializer(cg->observation_pass,LLVMConstInt(z,0,0));
    LLVMSetInitializer(cg->observation_fail,LLVMConstInt(z,0,0));
    LLVMSetLinkage(cg->observation_pass,LLVMInternalLinkage);
    LLVMSetLinkage(cg->observation_fail,LLVMInternalLinkage);
}
static void cg_observation_hit(cg_t*cg,node_t*n) {
    if(!cg->observation_config || !n || !n->observation_hit_id)return;
    uint32_t id=n->observation_hit_id-1;sts_observation_manifest*m=cg->observation_manifest;
    if(id>=m->line_count || m->lines[id]!=n->observation_line ||
       m->line_file_ids[id]+1!=n->observation_file_id_plus_one){obs_error(n,"missing dense coverage manifest identity");return;}
    cg->observation_emitted[id]=1;
    if(!(cg->observation_config->enabled_mask&STASHA_OBSERVATION_COVERAGE))return;
    LLVMValueRef arg=LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),id,0);cg_observation_call(cg,ObsHit,&arg,1);
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

static boolean_t cg_observation_seal(cg_t*cg) {
    if(!cg->observation_config)return True;
    sts_observation_manifest*m=cg->observation_manifest;
    for(uint32_t i=0;i<m->line_count;++i)
        if(!cg->observation_emitted[i])return obs_error(cg->ast,"eligible source line was not lowered");
    if(sts_observation_manifest_digest(cg->observation_config,m)!=0)
        return obs_error(cg->ast,"canonical v2 manifest bounds or identity");
    return True;
}
static void cg_observation_init_main(cg_t *cg) {
    if(!cg->observation_config)return;
    const sts_observation_config*c=cg->observation_config;sts_observation_manifest*m=cg->observation_manifest;
    const char*ids[]={c->run_id,c->input_graph_id,m->map_id,c->config_id,c->compiler_id};LLVMValueRef args[9];
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

/* LLVM copies each exact byte slice into its context-owned DI metadata.
 * The source strings remain owned by the frozen graph through reporting.
 * Ordinary compilation retains its existing root-only debug behavior. */
static boolean_t cg_observation_debug_path_file(cg_t*cg,uint32_t file,
        const char**basename,const char**directory,size_t*length) {
    const sts_observation_graph*g=cg->observation_config?cg->observation_config->graph:Null;
    if(!g || !g->frozen || !g->file_count || g->file_count>STS_OBS_FILE_LIMIT || file>=g->file_count ||
       g->files[file].file_id!=file || !g->files[file].canonical_path ||
       strlen(g->files[file].canonical_path)>STS_OBS_PATH_LIMIT)
        return obs_error(cg->ast,"debug source file differs from frozen graph");
    const char*owned=g->files[file].canonical_path;
    const char*last=strrchr(owned,'/');if(!last)last=strrchr(owned,'\\');
    *basename=last?last+1:owned;*directory=last?owned:".";
    *length=last?(last==owned?1:(size_t)(last-owned)):1;return True;
}
static boolean_t cg_observation_debug_path(cg_t*cg,const char**basename,
                                           const char**directory,size_t*length) {
    if(!cg_observation_debug_path_file(cg,0,basename,directory,length))return False;
    if(!cg->source_file || strcmp(cg->source_file,cg->observation_config->graph->files[0].canonical_path))
        return obs_error(cg->ast,"debug root path differs from frozen graph");
    return True;
}
/* Pure owner selection is shared by globals, functions and generated tests.
 * A matching pathname alone never establishes a file identity. */
static boolean_t cg_observation_debug_owner_file(cg_t*cg,node_t*n,uint32_t*file) {
    const sts_observation_graph*g=cg->observation_config?cg->observation_config->graph:Null;
    if(!g || !g->frozen || !g->file_count || g->file_count>STS_OBS_FILE_LIMIT || !n ||
       !n->observation_file_id_plus_one || n->observation_file_id_plus_one>g->file_count)
        return obs_error(n,"debug declaration has no graph-issued owner");
    uint32_t id=n->observation_file_id_plus_one-1;
    if(g->files[id].file_id!=id || !g->files[id].canonical_path || !n->observation_file ||
       strcmp(n->observation_file,g->files[id].canonical_path) || !n->observation_line ||
       n->observation_line>g->files[id].physical_line_count)
        return obs_error(n,"debug declaration origin disagrees with graph owner");
    *file=id;return True;
}
static boolean_t cg_observation_debug_files(cg_t*cg) {
    if(!cg->observation_config || !cg->debug_mode)return True;
    if(!cg->di_builder || !cg->di_file)return obs_error(cg->ast,"missing observed debug builder");
    const sts_observation_graph*g=cg->observation_config->graph;
    if(!g || !g->file_count || g->file_count>STS_OBS_FILE_LIMIT)return obs_error(cg->ast,"observed debug file capacity");
    cg->observation_di_files[0]=cg->di_file;
    for(uint32_t file=1;file<g->file_count;++file) {
        const char*basename,*directory;size_t length;
        if(!cg_observation_debug_path_file(cg,file,&basename,&directory,&length))return False;
        cg->observation_di_files[file]=LLVMDIBuilderCreateFile(cg->di_builder,
            basename,strlen(basename),directory,length);
        if(!cg->observation_di_files[file])return obs_error(cg->ast,"observed debug file allocation");
    }
    return True;
}
static void cg_observation_debug_clear(cg_t*cg) {
    if(!cg->observation_config || !cg->debug_mode)return;
    LLVMSetCurrentDebugLocation2(cg->builder,Null);
    cg->di_scope=Null;
    cg->di_file=cg->observation_di_files[0];
}
static boolean_t cg_observation_debug_select(cg_t*cg,node_t*n) {
    if(!cg->observation_config || !cg->debug_mode)return True;
    uint32_t file;
    if(!cg_observation_debug_owner_file(cg,n,&file))return False;
    if(!cg->observation_di_files[file])return obs_error(n,"missing observed owner DIFile");
    LLVMSetCurrentDebugLocation2(cg->builder,Null);
    cg->di_scope=Null;
    cg->di_file=cg->observation_di_files[file];return True;
}
static boolean_t cg_observation_debug_test(cg_t*cg,node_t*n,int optimization_level) {
    if(!cg->observation_config || !cg->debug_mode)return True;
    if(!cg_observation_debug_select(cg,n))return False;
    LLVMMetadataRef result_type[]={Null};
    LLVMMetadataRef function_type=LLVMDIBuilderCreateSubroutineType(cg->di_builder,
        cg->di_file,result_type,1,LLVMDIFlagZero);
    size_t linkage_length=0;
    const char*linkage=LLVMGetValueName2(cg->current_fn,&linkage_length);
    const char*label=n->as.test_block.name;
    LLVMMetadataRef scope=LLVMDIBuilderCreateFunction(cg->di_builder,cg->di_compile_unit,
        label,strlen(label),linkage,linkage_length,cg->di_file,(unsigned)n->observation_line,
        function_type,1,1,(unsigned)n->observation_line,LLVMDIFlagZero,optimization_level!=0);
    LLVMSetSubprogram(cg->current_fn,scope);cg->di_scope=scope;
    di_set_location(cg,n->observation_line);return True;
}

static boolean_t obs_same_primitive(type_info_t a,type_info_t b) {
    if(a.base!=b.base || a.is_pointer!=b.is_pointer || a.nullable!=b.nullable ||
       a.written_nullable!=b.written_nullable || a.written_storage!=b.written_storage)return False;
    if(a.is_pointer) {
        if((a.ptr_depth?a.ptr_depth:1)!=(b.ptr_depth?b.ptr_depth:1) || a.ptr_perm!=b.ptr_perm)return False;
        for(int i=1;i<a.ptr_depth;++i)if(a.ptr_perms[i]!=b.ptr_perms[i])return False;
    }
    return True;
}
static boolean_t cg_observation_selected_guard(cg_t*cg,node_t*n,symbol_t*selected) {
    if(!cg->observation_config)return True;
    node_t*body=Null;
    for(usize_t i=0;i<cg->ast->as.module.decls.count;++i) {
        node_t*d=cg->ast->as.module.decls.items[i];
        if(d->observation_decl_id_plus_one==n->observation_target_decl_id_plus_one &&
           d->kind==NodeFnDecl){body=d;break;}
    }
    if(!n->observation_target_decl_id_plus_one || !body || !selected || !LLVMIsAFunction(selected->value) ||
       scalar_call_decl(cg,n)!=body || scalar_call_symbol(cg,n)!=cg_lookup_symbol(cg,selected->name) ||
       !selected->scalar_signature || selected->c_abi)
        return obs_error(n,"selected call escaped graph declaration/scalar identity");
    char exact[512];if(!obs_symbol_name(body,exact) || strcmp(exact,selected->name) ||
       LLVMGetNamedFunction(cg->module,exact)!=selected->value)
        return obs_error(n,"selected emitted function identity mismatch");
    fn_ptr_desc_t*sig=selected->scalar_signature;
    if(sig->param_count!=body->as.fn_decl.params.count || sig->is_variadic || sig->is_c_abi ||
       !obs_same_primitive(sig->ret_type,body->as.fn_decl.return_types[0]))
        return obs_error(n,"selected primitive result signature mismatch");
    for(usize_t i=0;i<sig->param_count;++i)
        if(!obs_same_primitive(sig->params[i].type,body->as.fn_decl.params.items[i]->as.var_decl.type) ||
           sig->params[i].storage!=body->as.fn_decl.params.items[i]->as.var_decl.storage)
            return obs_error(n,"selected primitive parameter signature mismatch");
    return True;
}
static boolean_t cg_observation_call_guard(cg_t*cg,node_t*n) {
    if(!cg->observation_config)return True;
    return cg_observation_selected_guard(cg,n,cg_lookup(cg,n->as.call.callee));
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
        "__crash_runtime_install","__crash_runtime_register_block","__stasha_obs_init_v2","__stasha_obs_test_begin","__stasha_obs_test_end",
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
