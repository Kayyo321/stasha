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
    "observation declaration schema must match reviewed AST enum");

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

/* Private identities are never symbol-table slots or serialized addresses.
 * The source records freeze after admission. Only alloca/type is write-once
 * emission state, and complete symbol copies retain the same record pointer. */
enum { ObsSliceRetained=1, ObsSliceDirect, ObsSliceDefer };
enum { ObsSliceQuery=1, ObsSliceRead, ObsSliceWrite, ObsSliceRelease };
enum { ObsSliceHashSlots=131072 }; /* twice either 65,536 identity cap */
typedef struct obs_slice_reference obs_slice_reference;
typedef struct {
    node_t *node;
    uint32_t ordinal,file_id,line,col,end_line,end_col,token_begin,token_end,kind;
} obs_slice_item;
typedef struct {
    node_t *node;
    size_t type_binding_count;
    uint32_t type_file_id;
    uint16_t types_ready;
    unsigned char types[TypeF64+1],literal,shape;
} obs_item_semantic;
struct obs_slice_reference {
    node_t *ident, *operation, *parent, *block, *deferred;
    uint32_t block_id, child, owner_child, use;
    obs_slice_record *binding;
    obs_slice_reference *next;
};
struct obs_slice_record {
    uint32_t id, file_id, decl_id, block_id, child, element, schedule;
    uint32_t constructor_kind,item_count,owner_test;
    char *owner;
    node_t *declaration, *make, *block, *release, *compound;
    obs_slice_item *items;
    uint32_t make_line,make_col,make_end_line,make_end_col;
    uint32_t make_token_begin,make_token_end;
    obs_slice_reference *references;
    obs_slice_record *next;
    LLVMValueRef alloca;
    LLVMTypeRef descriptor_type;
};
typedef struct {
    node_t *node;
    uint32_t file_id, decl_id, parent_id, parent_child;
} obs_slice_block;
struct obs_slice_state {
    obs_slice_record *records;
    obs_slice_record **declarations;
    obs_slice_reference **references;
    obs_slice_block *blocks;
    uint32_t binding_count, reference_count, block_count, block_capacity, positional_item_count;
    node_t **item_identities;
    obs_item_semantic *semantics;
    size_t semantic_count,semantic_capacity;
    size_t *line_offsets[STS_OBS_FILE_LIMIT];
    boolean_t frozen;
};
static void *obs_slice_calloc(size_t count,size_t size) {
    if(!size || count>SIZE_MAX/size)return Null;
    return calloc(count,size);
}
static obs_slice_state *obs_slice_state_get(cg_t*cg,node_t*n) {
    if(!cg->observation_slices) {
        cg->observation_slices=obs_slice_calloc(1,sizeof(*cg->observation_slices));
        if(!cg->observation_slices)obs_error(n,"slice identity allocation failure");
    }
    return cg->observation_slices;
}
static size_t obs_slice_hash(node_t*n) {
    uintptr_t x=(uintptr_t)n;
    x=(x>>4)^(x>>17);return (size_t)(x*UINT64_C(11400714819323198485))&(ObsSliceHashSlots-1);
}
static obs_slice_record *obs_slice_declaration_find(cg_t*cg,node_t*n) {
    obs_slice_state*s=cg->observation_slices;
    if(!s || !s->declarations)return Null;
    size_t slot=obs_slice_hash(n);
    for(size_t i=0;i<ObsSliceHashSlots;++i,slot=(slot+1)&(ObsSliceHashSlots-1)) {
        obs_slice_record*r=s->declarations[slot];
        if(!r || r->declaration==n)return r;
    }
    return Null;
}
static obs_slice_reference *obs_slice_reference_find(cg_t*cg,node_t*n) {
    obs_slice_state*s=cg->observation_slices;
    if(!s || !s->references)return Null;
    size_t slot=obs_slice_hash(n);
    for(size_t i=0;i<ObsSliceHashSlots;++i,slot=(slot+1)&(ObsSliceHashSlots-1)) {
        obs_slice_reference*r=s->references[slot];
        if(!r || r->ident==n)return r;
    }
    return Null;
}
static void cg_observation_slice_dispose(cg_t*cg) {
    obs_slice_state*s=cg->observation_slices;if(!s)return;
    for(obs_slice_record*r=s->records;r;) {
        obs_slice_record*next=r->next;
        for(obs_slice_reference*u=r->references;u;) {obs_slice_reference*v=u->next;(free)(u);u=v;}
        (free)(r->items);(free)(r->owner);(free)(r);r=next;
    }
    for(uint32_t i=0;i<STS_OBS_FILE_LIMIT;++i)(free)(s->line_offsets[i]);
    (free)(s->declarations);(free)(s->references);(free)(s->blocks);(free)(s->item_identities);(free)(s->semantics);(free)(s);
    cg->observation_slices=Null;cg->observation_slice_pending=Null;
}
static uint32_t obs_slice_element(type_info_t t) {
    if(t.is_pointer || t.ptr_depth || t.ptr_perm || t.user_name || t.fn_ptr_desc ||
       t.elem_type || t.nullable || t.written_nullable || t.written_storage || t.generic_use)return 0;
    for(size_t i=0;i<sizeof(t.ptr_perms)/sizeof(t.ptr_perms[0]);++i)if(t.ptr_perms[i])return 0;
    switch(t.base) {
        case TypeI8:return STS_OBS_SLICE_I8;case TypeU8:return STS_OBS_SLICE_U8;
        case TypeI16:return STS_OBS_SLICE_I16;case TypeU16:return STS_OBS_SLICE_U16;
        case TypeI32:return STS_OBS_SLICE_I32;case TypeU32:return STS_OBS_SLICE_U32;
        case TypeI64:return STS_OBS_SLICE_I64;case TypeU64:return STS_OBS_SLICE_U64;
        case TypeBool:return STS_OBS_SLICE_BOOL;
        case TypeF32:return STS_OBS_SLICE_F32;case TypeF64:return STS_OBS_SLICE_F64;
        default:return 0;
    }
}
static uint32_t obs_slice_type_element(type_info_t t) {
    if(t.base!=TypeSlice || !t.elem_type)return 0;
    type_info_t root=t;root.base=TypeBool;root.elem_type=Null;
    return obs_slice_element(root)?obs_slice_element(t.elem_type[0]):0;
}
/* Inclusive parser spans must refer to actual braces in graph-owned bytes.
 * Cache bounded line offsets, so nested scopes never rescan a whole file. */
static int obs_slice_source_char(cg_t*cg,node_t*n,uint32_t line,uint32_t col) {
    obs_slice_state*s=obs_slice_state_get(cg,n);
    const sts_observation_graph*g=cg->observation_config->graph;
    if(!s || !n->observation_file_id_plus_one || n->observation_file_id_plus_one>g->file_count || !col)return -1;
    uint32_t file=n->observation_file_id_plus_one-1;
    const sts_observation_source*f=&g->files[file];
    if(!line || line>f->physical_line_count || !f->bytes)return -1;
    if(!s->line_offsets[file]) {
        size_t count=(size_t)f->physical_line_count+1;
        if(count<=f->physical_line_count)return -1;
        size_t*offsets=obs_slice_calloc(count,sizeof(*offsets));if(!offsets)return -1;
        uint32_t next=1;offsets[0]=0;
        for(size_t i=0;i<f->byte_length;++i)if(f->bytes[i]=='\n') {
            if(next>=f->physical_line_count){(free)(offsets);return -1;}
            offsets[next++]=i+1;
        }
        if(next!=f->physical_line_count){(free)(offsets);return -1;}
        offsets[next]=f->byte_length;s->line_offsets[file]=offsets;
    }
    size_t start=s->line_offsets[file][line-1],end=s->line_offsets[file][line];
    if((size_t)col-1>=end-start)return -1;
    return (unsigned char)f->bytes[start+col-1];
}
static boolean_t obs_slice_braces(cg_t*cg,node_t*n) {
    return n && n->kind==NodeBlock &&
        obs_slice_source_char(cg,n,(uint32_t)n->observation_line,n->observation_col)=='{' &&
        obs_slice_source_char(cg,n,(uint32_t)n->observation_end_line,n->observation_end_col)=='}';
}

typedef struct {const char*name;type_info_t type;unsigned array_rank;uint32_t file_id;boolean_t global;obs_slice_record*slice;node_t*constant_source;} obs_binding;
typedef struct {
    cg_t *cg;
    const char *owner;
    uint32_t owner_test, file_id, decl_id, anchor_kind;
    unsigned reason, depth;
    boolean_t global_binding;
    type_info_t return_type,match_type;
    obs_binding *bindings;
    size_t binding_count,binding_capacity;
    node_t *path[STS_OBS_SLICE_DEPTH_LIMIT], *deferred, *pending_slice;
    struct {node_t*node;uint32_t id,child;} blocks[STS_OBS_SLICE_DEPTH_LIMIT];
    uint32_t block_depth;
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
        (n->as.var_decl.flags & VdeclArray) ? (unsigned)(n->as.var_decl.array_ndim > 0 ? n->as.var_decl.array_ndim : 1) : 0, w->file_id,w->global_binding,Null,w->global_binding?n:Null};
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
            t=obs_expr_type(w,n->as.index_expr.object,depth+1);
            if(t.base==TypeSlice && !t.is_pointer && t.elem_type)return t.elem_type[0];
            if(t.is_pointer){t.ptr_depth=t.ptr_depth>0?t.ptr_depth-1:0;t.is_pointer=t.ptr_depth>0;}return t;
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
    if(w->reason!=STS_OBS_ELIGIBLE && w->reason!=STS_OBS_TEST &&
       !((kind==STS_OBS_SLICE_MAKE || kind==STS_OBS_SLICE_REM || kind==STS_OBS_SLICE_MAKE_POSITIONAL) && w->reason==STS_OBS_UNREACHABLE))return True;
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

/* Count-only source typing mirrors the existing scalar result rules while
 * resolving identifiers through the frozen lexical source walk. It rejects
 * floating and unknown counts even when ordinary lowering skips dead source.
 * Pure scalar literal/width helpers are shared; no LLVM storage is fabricated. */
static type_info_t obs_slice_count_type(obs_walk*,node_t*,unsigned);
static type_info_t obs_slice_count_common(obs_walk*w,node_t*a,node_t*b,unsigned depth) {
    boolean_t al=scalar_literal_tree(a),bl=scalar_literal_tree(b);
    type_info_t at=obs_slice_count_type(w,a,depth+1),bt=obs_slice_count_type(w,b,depth+1);
    if(al && !bl && scalar_integer(bt))return bt;
    if(bl && !al && scalar_integer(at))return at;
    if(scalar_integer(at) && scalar_integer(bt)) {
        if(scalar_unsigned(at)!=scalar_unsigned(bt)) {
            uint64_t am,bm;boolean_t an,bn;
            if(al && bl && scalar_literal(a,&am,&an) && scalar_literal(b,&bm,&bn) && !an && !bn)
                return scalar_type(TypeU64);
            return NO_TYPE;
        }
        return scalar_bits(at)>=scalar_bits(bt)?at:bt;
    }
    if(scalar_numeric(at) && scalar_numeric(bt) && (is_float_type(at) || is_float_type(bt)))
        return scalar_type(at.base==TypeF64 || bt.base==TypeF64?TypeF64:TypeF32);
    return at;
}
static type_info_t obs_slice_count_type(obs_walk*w,node_t*n,unsigned depth) {
    if(!n || depth>=STS_OBS_SLICE_DEPTH_LIMIT)return NO_TYPE;
    uint64_t magnitude;boolean_t negative;
    if(scalar_literal(n,&magnitude,&negative))return scalar_literal_type(n,NO_TYPE);
    switch(n->kind) {
        case NodeCharLitExpr:return scalar_type(TypeU8);
        case NodeSizeofExpr:return scalar_type(TypeU64);
        case NodeBinaryExpr:
            switch(n->as.binary.op) {
                case TokLt:case TokGt:case TokLtEq:case TokGtEq:case TokEqEq:case TokBangEq:
                case TokAmpAmp:case TokPipePipe:return scalar_type(TypeBool);
                case TokLtLt:case TokGtGt:return obs_slice_count_type(w,n->as.binary.left,depth+1);
                default:return obs_slice_count_common(w,n->as.binary.left,n->as.binary.right,depth);
            }
        case NodeTernaryExpr:
            return obs_slice_count_common(w,n->as.ternary.then_expr,n->as.ternary.else_expr,depth);
        case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr: {
            if(n->as.unary.op==TokBang)return scalar_type(TypeBool);
            type_info_t t=obs_slice_count_type(w,n->as.unary.operand,depth+1);
            return n->as.unary.op==TokStar?ti_deref_one(t):t;
        }
        case NodeAssignExpr:return obs_slice_count_type(w,n->as.assign.target,depth+1);
        case NodeCompoundAssign:return obs_slice_count_type(w,n->as.compound_assign.target,depth+1);
        default:return obs_expr_type(w,n,depth);
    }
}

/* Source-only positional item semantics. These run before LLVM exists and
 * never borrow the lowerer's signless types or manufacture symbol storage. */
/* Every original expression consumes source tokens. This cache has at most
 * one entry per graph-owned source byte, and twelve primitive/NO_TYPE contexts
 * per entry. It avoids repeatedly rescanning a left-associated literal tree.
 * Cache keys are original AST identities, never mutable symbol-table slots. */
static size_t obs_item_hash(node_t*n,size_t capacity) {
    uintptr_t x=(uintptr_t)n;x=(x>>4)^(x>>17);
    return (size_t)(x*UINT64_C(11400714819323198485))&(capacity-1);
}
static obs_item_semantic *obs_item_entry(obs_walk*w,node_t*n) {
    obs_slice_state*s=obs_slice_state_get(w->cg,n);
    if(!s || !n)return Null;
    if(s->semantic_capacity) {
        size_t slot=obs_item_hash(n,s->semantic_capacity);
        while(s->semantics[slot].node && s->semantics[slot].node!=n)slot=(slot+1)&(s->semantic_capacity-1);
        if(s->semantics[slot].node)return &s->semantics[slot];
    }
    const sts_observation_graph*g=w->cg->observation_config->graph;
    if(s->frozen || !g || s->semantic_count>=g->byte_length) {
        obs_error(n,"positional semantic identity exceeds original source capacity");w->cg->observation_inventory_error=True;return Null;
    }
    if(!s->semantic_capacity || s->semantic_count>=s->semantic_capacity/2) {
        size_t capacity=s->semantic_capacity?s->semantic_capacity*2:256;
        if(capacity<s->semantic_capacity || capacity>SIZE_MAX/sizeof(*s->semantics)) {
            obs_error(n,"positional semantic cache allocation capacity");w->cg->observation_inventory_error=True;return Null;
        }
        obs_item_semantic*entries=obs_slice_calloc(capacity,sizeof(*entries));
        if(!entries){obs_error(n,"positional semantic cache allocation failure");w->cg->observation_inventory_error=True;return Null;}
        for(size_t i=0;i<s->semantic_capacity;++i)if(s->semantics[i].node) {
            size_t slot=obs_item_hash(s->semantics[i].node,capacity);
            while(entries[slot].node)slot=(slot+1)&(capacity-1);
            entries[slot]=s->semantics[i];
        }
        (free)(s->semantics);s->semantics=entries;s->semantic_capacity=capacity;
    }
    size_t slot=obs_item_hash(n,s->semantic_capacity);
    while(s->semantics[slot].node)slot=(slot+1)&(s->semantic_capacity-1);
    s->semantics[slot].node=n;++s->semantic_count;return &s->semantics[slot];
}
static boolean_t obs_item_literal_tree(obs_walk*w,node_t*n,unsigned depth) {
    if(!n || depth>=STS_OBS_SLICE_DEPTH_LIMIT)return False;
    obs_item_semantic*entry=obs_item_entry(w,n);if(!entry)return False;
    if(entry->literal)return entry->literal==2;
    boolean_t literal=False;
    switch(n->kind) {
        case NodeIntLitExpr:case NodeCharLitExpr:literal=True;break;
        case NodeUnaryPrefixExpr:
            literal=(n->as.unary.op==TokMinus || n->as.unary.op==TokTilde || n->as.unary.op==TokPlus) &&
                obs_item_literal_tree(w,n->as.unary.operand,depth+1);break;
        case NodeBinaryExpr:
            switch(n->as.binary.op) {
                case TokPlus:case TokMinus:case TokStar:case TokSlash:case TokPercent:
                case TokPlusPercent:case TokMinusPercent:case TokStarPercent:
                case TokPlusBang:case TokMinusBang:case TokStarBang:
                case TokAmp:case TokPipe:case TokCaret:case TokLtLt:case TokGtGt:
                    literal=obs_item_literal_tree(w,n->as.binary.left,depth+1) &&
                        obs_item_literal_tree(w,n->as.binary.right,depth+1);break;
                default:break;
            }
            break;
        default:break;
    }
    /* Recursive insertion can resize the table; reacquire the original key. */
    entry=obs_item_entry(w,n);if(entry)entry->literal=literal?2:1;
    return literal;
}
static boolean_t obs_item_shape(obs_walk*,node_t*,unsigned);
/* Establish the complete bounded tree before scalar_literal_tree or any
 * contextual inference traverses it. This pass has no inventory side effects. */
static boolean_t obs_item_shape_inner(obs_walk*w,node_t*n,unsigned depth) {
    if(!n || depth>STS_OBS_SLICE_DEPTH_LIMIT)return obs_error(n,"positional item source nesting limit or missing expression");
    if(!obs_origin(w,n) || n->observation_file_id_plus_one!=w->file_id+1 || n->headers)
        return obs_error(n,"positional item source owner or attribute");
#define OIS(N) do {if(!obs_item_shape(w,(N),depth+1))return False;} while(0)
    switch(n->kind) {
        case NodeIntLitExpr:case NodeFloatLitExpr:case NodeCharLitExpr:case NodeBoolLitExpr:
        case NodeIdentExpr:case NodeSizeofExpr:return True;
        case NodeUnaryPrefixExpr:
            if(n->as.unary.op!=TokMinus && n->as.unary.op!=TokBang && n->as.unary.op!=TokTilde)
                return obs_error(n,"positional item unary operator");
            OIS(n->as.unary.operand);return True;
        case NodeBinaryExpr:
            switch(n->as.binary.op) {
                case TokPlus:case TokMinus:case TokStar:case TokSlash:case TokPercent:
                case TokAmp:case TokPipe:case TokCaret:case TokLtLt:case TokGtGt:
                case TokPlusPercent:case TokMinusPercent:case TokStarPercent:
                case TokPlusBang:case TokMinusBang:case TokStarBang:
                case TokEqEq:case TokBangEq:case TokLt:case TokGt:case TokLtEq:case TokGtEq:
                case TokAmpAmp:case TokPipePipe:break;
                default:return obs_error(n,"positional item binary operator");
            }
            OIS(n->as.binary.left);OIS(n->as.binary.right);return True;
        case NodeCastExpr:OIS(n->as.cast_expr.expr);return True;
        case NodeTernaryExpr:
            OIS(n->as.ternary.cond);OIS(n->as.ternary.then_expr);OIS(n->as.ternary.else_expr);return True;
        case NodeCallExpr:case NodeMethodCall: {
            node_list_t args=n->kind==NodeCallExpr?n->as.call.args:n->as.method_call.args;
            if(args.count>args.capacity || (args.count && !args.items))return obs_error(n,"positional call argument list shape");
            if(n->kind==NodeMethodCall) {
                node_t*alias=n->as.method_call.object;
                if(!alias || alias->kind!=NodeIdentExpr)return obs_error(n,"positional call requires a source module alias");
                OIS(alias);
            }
            for(usize_t i=0;i<args.count;++i)OIS(args.items[i]);
            return True;
        }
        default:return obs_error(n,"positional item is outside the recursive primitive whitelist");
    }
#undef OIS
}
static boolean_t obs_item_shape(obs_walk*w,node_t*n,unsigned depth) {
    if(!n || depth>STS_OBS_SLICE_DEPTH_LIMIT)return obs_error(n,"positional item source nesting limit or missing expression");
    obs_item_semantic*entry=obs_item_entry(w,n);if(!entry)return False;
    if(entry->shape)return obs_error(n,"repeated or cyclic positional expression identity");
    entry->shape=1;
    if(!obs_item_shape_inner(w,n,depth))return False;
    entry=obs_item_entry(w,n);if(!entry)return False;
    entry->shape=2;return True;
}
static type_info_t obs_item_type(obs_walk*,node_t*,type_info_t,unsigned);
static type_info_t obs_item_common(obs_walk*w,node_t*a,node_t*b,type_info_t expected,unsigned depth) {
    if(expected.base==TypeBool)expected=NO_TYPE;
    boolean_t al=obs_item_literal_tree(w,a,0),bl=obs_item_literal_tree(w,b,0);
    type_info_t at=obs_item_type(w,a,al&&bl?expected:NO_TYPE,depth+1);
    type_info_t bt=obs_item_type(w,b,al&&bl?expected:NO_TYPE,depth+1);
    if(!obs_slice_element(at) || !obs_slice_element(bt))return NO_TYPE;
    if(al && !bl && scalar_integer(bt))return bt;
    if(bl && !al && scalar_integer(at))return at;
    if(scalar_integer(at) && scalar_integer(bt)) {
        if(scalar_unsigned(at)!=scalar_unsigned(bt)) {
            uint64_t am,bm;boolean_t an,bn;
            if(al && bl && scalar_literal(a,&am,&an) && scalar_literal(b,&bm,&bn) && !an && !bn)
                return scalar_type(TypeU64);
            return NO_TYPE;
        }
        return scalar_bits(at)>=scalar_bits(bt)?at:bt;
    }
    if(scalar_numeric(at) && scalar_numeric(bt))
        return scalar_type(at.base==TypeF64 || bt.base==TypeF64?TypeF64:TypeF32);
    return NO_TYPE;
}
static type_info_t obs_item_type_inner(obs_walk*w,node_t*n,type_info_t expected,unsigned depth) {
    if(!n || depth>=STS_OBS_SLICE_DEPTH_LIMIT)return NO_TYPE;
    uint64_t magnitude;boolean_t negative;
    if(scalar_literal(n,&magnitude,&negative))return scalar_literal_type(n,expected);
    switch(n->kind) {
        case NodeBoolLitExpr:return scalar_type(TypeBool);
        case NodeCharLitExpr:return scalar_integer(expected) && expected.base!=TypeBool?expected:scalar_type(TypeU8);
        case NodeFloatLitExpr:return scalar_type(TypeF64);
        case NodeIdentExpr: {
            obs_binding*b=obs_binding_lookup(w,n->as.ident.name);
            return b && !b->array_rank && !b->slice && obs_slice_element(b->type)?b->type:NO_TYPE;
        }
        case NodeCastExpr:return obs_slice_element(n->as.cast_expr.target)?n->as.cast_expr.target:NO_TYPE;
        case NodeSizeofExpr:return obs_slice_element(n->as.sizeof_expr.type)?scalar_type(TypeU64):NO_TYPE;
        case NodeUnaryPrefixExpr:
            if(n->as.unary.op==TokBang)return scalar_type(TypeBool);
            if(n->as.unary.op==TokMinus || n->as.unary.op==TokTilde)
                return obs_item_type(w,n->as.unary.operand,expected,depth+1);
            return NO_TYPE;
        case NodeBinaryExpr:
            switch(n->as.binary.op) {
                case TokEqEq:case TokBangEq:case TokLt:case TokGt:case TokLtEq:case TokGtEq:
                case TokAmpAmp:case TokPipePipe:return scalar_type(TypeBool);
                case TokLtLt:case TokGtGt:return obs_item_type(w,n->as.binary.left,expected,depth+1);
                default:return obs_item_common(w,n->as.binary.left,n->as.binary.right,expected,depth);
            }
        case NodeTernaryExpr:return obs_item_common(w,n->as.ternary.then_expr,n->as.ternary.else_expr,expected,depth);
        case NodeCallExpr:case NodeMethodCall: {
            node_t*f=obs_selected_function(w,n);
            return f && f->as.fn_decl.return_count==1 && obs_slice_element(f->as.fn_decl.return_types[0])?
                f->as.fn_decl.return_types[0]:NO_TYPE;
        }
        default:return NO_TYPE;
    }
}
static type_info_t obs_item_type(obs_walk*w,node_t*n,type_info_t expected,unsigned depth) {
    if(!n || depth>=STS_OBS_SLICE_DEPTH_LIMIT)return NO_TYPE;
    unsigned context=obs_slice_element(expected)?(unsigned)expected.base:(unsigned)TypeVoid;
    obs_item_semantic*entry=obs_item_entry(w,n);if(!entry)return NO_TYPE;
    if(entry->types_ready && (entry->type_file_id!=w->file_id || entry->type_binding_count!=w->binding_count)) {
        obs_error(n,"positional semantic cache escaped its frozen lexical environment");
        w->cg->observation_inventory_error=True;return NO_TYPE;
    }
    if(entry->types_ready&(1u<<context))return scalar_type((type_kind_t)entry->types[context]);
    type_info_t type=obs_item_type_inner(w,n,expected,depth);
    entry=obs_item_entry(w,n);if(!entry)return NO_TYPE;
    entry->type_file_id=w->file_id;entry->type_binding_count=w->binding_count;
    entry->types[context]=(unsigned char)type.base;entry->types_ready|=(uint16_t)(1u<<context);
    return type;
}
/* Constant proof adapter for ordinary implicit integer conversion. The pure
 * bit-pattern engine is shared; lexical bindings and primitive sizeof remain
 * source facts. Runtime locals, including const locals, are never fit proofs. */
static boolean_t obs_item_constant(obs_walk*w,node_t*n,type_info_t expected,
                                   scalar_constant_t*out,unsigned depth,unsigned*budget,boolean_t exact_bindings) {
    if(!n || depth>256 || !*budget)return False;
    --*budget;
    type_info_t t=obs_item_type(w,n,expected,0);
    if(!scalar_integer(t))return False;
    uint64_t m;boolean_t negative;
    if(scalar_literal(n,&m,&negative)) {
        if(!scalar_literal_fits(n,t))return False;
        *out=(scalar_constant_t){(negative?UINT64_C(0)-m:m)&scalar_mask(scalar_bits(t)),t};return True;
    }
    scalar_constant_t a,b;
#define OIC(N,T,O) obs_item_constant(w,(N),(T),(O),depth+1,budget,exact_bindings)
    switch(n->kind) {
        case NodeBoolLitExpr:*out=(scalar_constant_t){n->as.bool_lit.value!=0,scalar_type(TypeBool)};return True;
        case NodeCharLitExpr: {
            node_t literal=*n;literal.kind=NodeIntLitExpr;literal.as.int_lit.value=(unsigned char)n->as.char_lit.value;
            if(!scalar_literal_fits(&literal,t))return False;
            *out=(scalar_constant_t){literal.as.int_lit.value,t};return True;
        }
        case NodeSizeofExpr: {
            uint32_t element=obs_slice_element(n->as.sizeof_expr.type);
            if(!element)return False;
            *out=(scalar_constant_t){sts_observation_slice_stride(element),scalar_type(TypeU64)};return True;
        }
        case NodeIdentExpr: {
            obs_binding*binding=obs_binding_lookup(w,n->as.ident.name);
            node_t*d=binding?binding->constant_source:Null;
            /* Instrumentation internalizes globals; that must not broaden the
             * ordinary source's proof for an externally visible constant. */
            if(!binding || !binding->global || !d || !(d->as.var_decl.flags&VdeclConst) ||
               (exact_bindings && d->as.var_decl.linkage!=LinkageInternal) ||
               d->as.var_decl.attr_flags || binding->array_rank)return False;
            obs_walk globals=*w;globals.file_id=binding->file_id;
            globals.binding_count=0;
            while(globals.binding_count<w->binding_count && w->bindings[globals.binding_count].global)++globals.binding_count;
            if(!d->as.var_decl.init) {*out=(scalar_constant_t){0,t};return True;}
            if(!obs_item_constant(&globals,d->as.var_decl.init,t,out,depth+1,budget,False))return False;
            return scalar_const_convert(out,t,False);
        }
        case NodeCastExpr:
            if(!OIC(n->as.cast_expr.expr,NO_TYPE,&a) || !scalar_const_convert(&a,t,True))return False;
            *out=a;return True;
        case NodeTernaryExpr:
            if(!OIC(n->as.ternary.cond,NO_TYPE,&a) ||
               !OIC(a.bits?n->as.ternary.then_expr:n->as.ternary.else_expr,t,out))return False;
            return scalar_const_convert(out,t,False);
        case NodeUnaryPrefixExpr:
            if(!OIC(n->as.unary.operand,n->as.unary.op==TokBang?NO_TYPE:t,&a))return False;
            if(n->as.unary.op==TokBang){*out=(scalar_constant_t){!a.bits,scalar_type(TypeBool)};return True;}
            if(!scalar_const_convert(&a,t,False))return False;
            if(n->as.unary.op==TokMinus)a.bits=(UINT64_C(0)-a.bits)&scalar_mask(scalar_bits(t));
            else if(n->as.unary.op==TokTilde)a.bits=(~a.bits)&scalar_mask(scalar_bits(t));
            else return False;
            *out=a;return True;
        case NodeBinaryExpr: {
            token_kind_t op=n->as.binary.op;
            if(op==TokAmpAmp || op==TokPipePipe) {
                if(!OIC(n->as.binary.left,NO_TYPE,&a))return False;
                if((op==TokAmpAmp && !a.bits) || (op==TokPipePipe && a.bits)) {
                    *out=(scalar_constant_t){a.bits!=0,scalar_type(TypeBool)};return True;
                }
                if(!OIC(n->as.binary.right,NO_TYPE,&b))return False;
                *out=(scalar_constant_t){b.bits!=0,scalar_type(TypeBool)};return True;
            }
            boolean_t shift=op==TokLtLt || op==TokGtGt;
            type_info_t common=shift?obs_item_type(w,n->as.binary.left,expected,0):
                obs_item_common(w,n->as.binary.left,n->as.binary.right,expected,0);
            if(!scalar_integer(common) || !OIC(n->as.binary.left,common,&a) ||
               !scalar_const_convert(&a,common,False) || !OIC(n->as.binary.right,shift?NO_TYPE:common,&b) ||
               (!shift && !scalar_const_convert(&b,common,False)))return False;
            uint64_t value;boolean_t boolean_result;unsigned reason;
            if(!sts_scalar_fold_integer(op,a.bits,b.bits,scalar_bits(common),scalar_unsigned(common),
                 scalar_bits(b.type),scalar_unsigned(b.type),&value,&boolean_result,&reason))return False;
            *out=(scalar_constant_t){value,boolean_result?scalar_type(TypeBool):t};return True;
        }
        default:return False;
    }
#undef OIC
}
static boolean_t obs_item_conversion(obs_walk*w,node_t*n,type_info_t from,type_info_t to) {
    if(!obs_slice_element(from) || !obs_slice_element(to))return obs_error(n,"positional item conversion requires known primitive scalars");
    if(scalar_integer(from) && scalar_integer(to) && to.base!=TypeBool &&
       (scalar_bits(to)<scalar_bits(from) || (scalar_unsigned(from)!=scalar_unsigned(to) &&
        !(scalar_unsigned(from) && scalar_bits(to)>scalar_bits(from))))) {
        scalar_constant_t value;unsigned budget=10000;
        if(!obs_item_constant(w,n,from,&value,0,&budget,True) ||
           scalar_bits(value.type)!=scalar_bits(from) || scalar_unsigned(value.type)!=scalar_unsigned(from) ||
           !scalar_const_value_fits(value,to))
            return obs_error(n,"positional item implicit integer conversion may change value; use an explicit cast");
    }
    return True;
}
static boolean_t obs_item_validate(obs_walk*,node_t*,type_info_t,unsigned,type_info_t*);
static boolean_t obs_item_as(obs_walk*w,node_t*n,type_info_t to,unsigned depth) {
    type_info_t from;
    return obs_item_validate(w,n,to,depth,&from) && obs_item_conversion(w,n,from,to);
}
static boolean_t obs_item_validate(obs_walk*w,node_t*n,type_info_t expected,unsigned depth,type_info_t*out) {
    if(!n || depth>=STS_OBS_SLICE_DEPTH_LIMIT)return obs_error(n,"positional item nesting limit or missing expression");
    if(!obs_origin(w,n) || n->observation_file_id_plus_one!=w->file_id+1 || n->headers)
        return obs_error(n,"positional item source owner or attribute");
    type_info_t t=obs_item_type(w,n,expected,depth),child;
    uint64_t magnitude;boolean_t negative;
    if(scalar_literal(n,&magnitude,&negative)) {
        if(!scalar_literal_fits(n,t))return obs_error(n,"positional integer literal is outside its contextual type");
        /* Unary-minus integer literals are typed as one signed literal. */
        if(n->kind==NodeUnaryPrefixExpr && (!obs_origin(w,n->as.unary.operand) ||
           n->as.unary.operand->observation_file_id_plus_one!=w->file_id+1 || n->as.unary.operand->headers))return False;
        *out=t;return True;
    }
#define OIV(N,T) obs_item_validate(w,(N),(T),depth+1,&child)
#define OIA(N,T) obs_item_as(w,(N),(T),depth+1)
    switch(n->kind) {
        case NodeCharLitExpr: {
            node_t literal=*n;literal.kind=NodeIntLitExpr;literal.as.int_lit.value=(unsigned char)n->as.char_lit.value;
            if(!scalar_literal_fits(&literal,t))return obs_error(n,"positional character literal is outside its contextual type");
            break;
        }
        case NodeBoolLitExpr:case NodeFloatLitExpr:case NodeIdentExpr:break;
        case NodeSizeofExpr:
            if(!obs_slice_element(n->as.sizeof_expr.type))return obs_error(n,"positional sizeof requires a direct nonpointer primitive type");
            break;
        case NodeUnaryPrefixExpr:
            if(n->as.unary.op==TokBang) {if(!OIV(n->as.unary.operand,NO_TYPE))return False;}
            else if(n->as.unary.op==TokMinus || n->as.unary.op==TokTilde) {
                if(!obs_slice_element(t) || (n->as.unary.op==TokTilde && !scalar_integer(t)))
                    return obs_error(n,"positional unary operator requires compatible primitive operands");
                if(!OIA(n->as.unary.operand,t))return False;
            } else return obs_error(n,"positional item unary operator");
            break;
        case NodeBinaryExpr: {
            token_kind_t op=n->as.binary.op;
            if(op==TokAmpAmp || op==TokPipePipe) {
                if(!OIV(n->as.binary.left,NO_TYPE) || !OIV(n->as.binary.right,NO_TYPE))return False;
                break;
            }
            boolean_t integer_only=False,shift=False;
            switch(op) {
                case TokPlus:case TokMinus:case TokStar:case TokSlash:
                case TokEqEq:case TokBangEq:case TokLt:case TokGt:case TokLtEq:case TokGtEq:break;
                case TokLtLt:case TokGtGt:shift=True; /* fall through */
                case TokPercent:case TokAmp:case TokPipe:case TokCaret:
                case TokPlusPercent:case TokMinusPercent:case TokStarPercent:
                case TokPlusBang:case TokMinusBang:case TokStarBang:integer_only=True;break;
                default:return obs_error(n,"positional item binary operator");
            }
            type_info_t left=obs_item_type(w,n->as.binary.left,NO_TYPE,depth+1);
            type_info_t right=obs_item_type(w,n->as.binary.right,NO_TYPE,depth+1);
            type_info_t common=shift?obs_item_type(w,n->as.binary.left,expected,depth+1):
                obs_item_common(w,n->as.binary.left,n->as.binary.right,expected,depth);
            if(!obs_slice_element(left) || !obs_slice_element(right) || !obs_slice_element(common) ||
               (integer_only && (!scalar_integer(left) || !scalar_integer(right) || !scalar_integer(common))))
                return obs_error(n,"positional binary operands have incompatible primitive types");
            if(!OIA(n->as.binary.left,common))return False;
            if(shift) {if(!OIV(n->as.binary.right,NO_TYPE))return False;}
            else if(!OIA(n->as.binary.right,common))return False;
            break;
        }
        case NodeCastExpr:
            if(!obs_slice_element(t) || !OIV(n->as.cast_expr.expr,NO_TYPE))return obs_error(n,"positional cast requires primitive scalar source and destination");
            break;
        case NodeTernaryExpr:
            if(!obs_slice_element(t))return obs_error(n,"positional conditional arms have incompatible primitive types");
            if(!OIV(n->as.ternary.cond,NO_TYPE) || !OIA(n->as.ternary.then_expr,t) || !OIA(n->as.ternary.else_expr,t))return False;
            break;
        case NodeCallExpr:case NodeMethodCall: {
            node_t*f=obs_selected_function(w,n);
            node_list_t args=n->kind==NodeCallExpr?n->as.call.args:n->as.method_call.args;
            if(!f || f->as.fn_decl.return_count!=1 || !obs_slice_element(t) || f->as.fn_decl.is_variadic ||
               f->as.fn_decl.is_typed_variadic || args.count!=f->as.fn_decl.params.count)
                return obs_error(n,"positional call requires an exact admitted primitive source signature");
            for(usize_t i=0;i<args.count;++i) {
                type_info_t parameter=f->as.fn_decl.params.items[i]->as.var_decl.type;
                if(!obs_slice_element(parameter) || !OIA(args.items[i],parameter))
                    return obs_error(args.items[i],"positional call argument requires a primitive source parameter");
            }
            break;
        }
        default:return obs_error(n,"positional item is outside the recursive primitive whitelist");
    }
#undef OIV
#undef OIA
    if(!obs_slice_element(t))return obs_error(n,"positional item requires a known nonpointer primitive scalar value");
    *out=t;return True;
}
static uint32_t obs_slice_constructor(node_t*make,uint32_t element) {
    if(!make || make->kind!=NodeMakeExpr || !element)return 0;
    if(!make->as.make_expr.init)
        return make->as.make_expr.len && obs_slice_element(make->as.make_expr.elem_type)==element?STS_OBS_SLICE_MAKE:0;
    type_info_t context=make->as.make_expr.elem_type;context.base=TypeBool;
    return make->as.make_expr.init->kind==NodeCompoundInit && !make->as.make_expr.len && !make->as.make_expr.cap &&
        make->as.make_expr.elem_type.base==TypeVoid && obs_slice_element(context)?STS_OBS_SLICE_MAKE_POSITIONAL:0;
}

static boolean_t obs_slice_block_enter(obs_walk*w,node_t*n) {
    obs_slice_state*s=obs_slice_state_get(w->cg,n);if(!s)return False;
    if(s->block_count>=STS_OBS_SLICE_BLOCK_LIMIT || w->block_depth>=STS_OBS_SLICE_DEPTH_LIMIT)
        return obs_error(n,"slice block identity or depth capacity");
    if(s->block_count==s->block_capacity) {
        size_t capacity=s->block_capacity?(size_t)s->block_capacity*2:64;
        if(capacity>STS_OBS_SLICE_BLOCK_LIMIT || capacity>SIZE_MAX/sizeof(*s->blocks))
            return obs_error(n,"slice block identity allocation capacity");
        obs_slice_block*p=(realloc)(s->blocks,capacity*sizeof(*p));
        if(!p)return obs_error(n,"slice block identity allocation failure");
        s->blocks=p;s->block_capacity=(uint32_t)capacity;
    }
    uint32_t id=s->block_count++;
    s->blocks[id]=(obs_slice_block){.node=n,.file_id=w->file_id,.decl_id=w->decl_id,
        .parent_id=w->block_depth?w->blocks[w->block_depth-1].id:UINT32_MAX,
        .parent_child=w->block_depth?w->blocks[w->block_depth-1].child:UINT32_MAX};
    w->blocks[w->block_depth].node=n;w->blocks[w->block_depth].id=id;
    w->blocks[w->block_depth++].child=0;return True;
}
static boolean_t obs_slice_collect_reference(obs_walk*w,node_t*n,obs_slice_record*binding) {
    obs_slice_state*s=w->cg->observation_slices;
    node_t*operation=w->depth>=2?w->path[w->depth-2]:Null;
    node_t*parent=w->depth>=3?w->path[w->depth-3]:Null;
    uint32_t use=0;
    if(operation && (operation->kind==NodeLenExpr || operation->kind==NodeCapExpr) &&
       operation->as.len_expr.operand==n)use=ObsSliceQuery;
    else if(operation && operation->kind==NodeIndexExpr && operation->as.index_expr.object==n) {
        if(parent && ((parent->kind==NodeAddrOf && parent->as.addr_of.operand==operation) ||
           (parent->kind==NodeCompoundAssign && parent->as.compound_assign.target==operation) ||
           ((parent->kind==NodeUnaryPrefixExpr || parent->kind==NodeUnaryPostfixExpr) &&
            (parent->as.unary.op==TokPlusPlus || parent->as.unary.op==TokMinusMinus))))
            return obs_error(n,"slice element address or compound/postfix mutation");
        use=parent && parent->kind==NodeAssignExpr && parent->as.assign.target==operation?ObsSliceWrite:ObsSliceRead;
    } else if(operation && operation->kind==NodeRemStmt && operation->as.rem_stmt.ptr==n &&
              parent && parent->kind==NodeExprStmt && parent->as.expr_stmt.expr==operation)use=ObsSliceRelease;
    if(!use)return obs_error(n,"slice descriptor reference outside the closed direct-use whitelist");
    if(!s || s->frozen || s->reference_count>=STS_OBS_SLICE_REFERENCE_LIMIT || !w->block_depth ||
       binding->file_id!=w->file_id || binding->decl_id!=w->decl_id || obs_slice_reference_find(w->cg,n))
        return obs_error(n,"slice reference capacity or original declaration identity");
    uint32_t owner_child=UINT32_MAX;
    for(uint32_t i=0;i<w->block_depth;++i)
        if(w->blocks[i].id==binding->block_id && w->blocks[i].node==binding->block)
            owner_child=w->blocks[i].child;
    if(owner_child==UINT32_MAX)return obs_error(n,"slice reference escaped its original block");
    if(!s->references) {
        s->references=obs_slice_calloc(ObsSliceHashSlots,sizeof(*s->references));
        if(!s->references)return obs_error(n,"slice reference table allocation failure");
    }
    obs_slice_reference*r=obs_slice_calloc(1,sizeof(*r));
    if(!r)return obs_error(n,"slice reference allocation failure");
    *r=(obs_slice_reference){.ident=n,.operation=operation,.parent=parent,
        .block=w->blocks[w->block_depth-1].node,.block_id=w->blocks[w->block_depth-1].id,
        .child=w->blocks[w->block_depth-1].child,.owner_child=owner_child,.deferred=w->deferred,
        .use=use,.binding=binding,.next=binding->references};
    size_t slot=obs_slice_hash(n);
    while(s->references[slot])slot=(slot+1)&(ObsSliceHashSlots-1);
    s->references[slot]=r;binding->references=r;++s->reference_count;return True;
}
static boolean_t obs_slice_item_span(node_t*make,node_t*item,node_t*previous,uint32_t file) {
    if(!item || item->observation_file_id_plus_one!=file+1 ||
       item->observation_token_begin<make->observation_token_begin ||
       item->observation_token_end>make->observation_token_end ||
       item->observation_token_end<item->observation_token_begin ||
       item->observation_line<make->observation_line ||
       (item->observation_line==make->observation_line && item->observation_col<make->observation_col) ||
       item->observation_end_line>make->observation_end_line ||
       (item->observation_end_line==make->observation_end_line && item->observation_end_col>make->observation_end_col) ||
       (previous && (item->observation_token_begin<=previous->observation_token_end ||
        item->observation_line<previous->observation_end_line ||
        (item->observation_line==previous->observation_end_line && item->observation_col<=previous->observation_end_col))))return False;
    return True;
}
static boolean_t obs_slice_snapshot(obs_walk*w,obs_slice_record*r) {
    obs_slice_state*s=w->cg->observation_slices;
    node_t*make=r->make;
    r->make_line=(uint32_t)make->observation_line;r->make_col=make->observation_col;
    r->make_end_line=(uint32_t)make->observation_end_line;r->make_end_col=make->observation_end_col;
    r->make_token_begin=make->observation_token_begin;r->make_token_end=make->observation_token_end;
    r->owner_test=w->owner_test;r->owner=strdup(w->owner?w->owner:"global");
    if(!r->owner)return obs_error(make,"slice owner snapshot allocation failure");
    if(r->constructor_kind!=STS_OBS_SLICE_MAKE_POSITIONAL)return True;
    node_t*init=make->as.make_expr.init;
    usize_t count=init->as.compound_init.items.count;
    if(count>STS_OBS_POSITIONAL_ITEMS_PER_CONSTRUCTOR_LIMIT ||
       s->positional_item_count>STS_OBS_POSITIONAL_ITEMS_AGGREGATE_LIMIT ||
       count>STS_OBS_POSITIONAL_ITEMS_AGGREGATE_LIMIT-s->positional_item_count ||
       count>SIZE_MAX/sizeof(*r->items) || count>init->as.compound_init.items.capacity ||
       (count && !init->as.compound_init.items.items))
        return obs_error(make,"positional item snapshot count or aggregate capacity");
    r->compound=init;r->item_count=(uint32_t)count;
    if(!count)return True;
    r->items=obs_slice_calloc(count,sizeof(*r->items));
    if(!r->items)return obs_error(make,"positional item snapshot allocation failure");
    if(!s->item_identities)s->item_identities=obs_slice_calloc(ObsSliceHashSlots,sizeof(*s->item_identities));
    if(!s->item_identities)return obs_error(make,"positional item identity allocation failure");
    node_t*previous=Null;
    for(uint32_t i=0;i<r->item_count;++i) {
        node_t*item=init->as.compound_init.items.items[i];
        if(!obs_slice_item_span(make,item,previous,r->file_id) || !obs_origin(w,item))
            return obs_error(item,"positional item original order or source containment");
        size_t slot=obs_slice_hash(item);
        while(s->item_identities[slot] && s->item_identities[slot]!=item)slot=(slot+1)&(ObsSliceHashSlots-1);
        if(s->item_identities[slot])return obs_error(item,"duplicate positional item original identity");
        s->item_identities[slot]=item;
        r->items[i]=(obs_slice_item){item,i,r->file_id,(uint32_t)item->observation_line,item->observation_col,
            (uint32_t)item->observation_end_line,item->observation_end_col,item->observation_token_begin,item->observation_token_end,(uint32_t)item->kind};
        previous=item;
    }
    s->positional_item_count+=r->item_count;return True;
}
static boolean_t obs_slice_variable(obs_walk*w,node_t*n,boolean_t runtime) {
    uint32_t element=obs_slice_type_element(n->as.var_decl.type);
    node_t*make=n->as.var_decl.init;
    uint32_t constructor=obs_slice_constructor(make,element);
    node_t*parent=w->depth>=2?w->path[w->depth-2]:Null;
    if(!runtime || w->global_binding || w->deferred || !w->block_depth ||
       parent!=w->blocks[w->block_depth-1].node || !obs_slice_braces(w->cg,parent) ||
       n->as.var_decl.storage!=StorageHeap || n->as.var_decl.flags || n->as.var_decl.attr_flags ||
       n->as.var_decl.bitfield_width || n->as.var_decl.array_ndim || n->as.var_decl.linkage!=LinkageNone ||
       obs_reserved(n->as.var_decl.name) || !strcmp(n->as.var_decl.name,"_") || !element ||
       !constructor)
        return obs_error(n,"slice constructor requires a direct unqualified heap builtin declaration in a source brace block");
    obs_slice_state*s=obs_slice_state_get(w->cg,n);
    if(!s || s->frozen || s->binding_count>=STS_OBS_SLICE_BINDING_LIMIT || obs_slice_declaration_find(w->cg,n))
        return obs_error(n,"slice declaration capacity or duplicate original identity");
    obs_anchor(w,n);
    /* Counts and positional items resolve before this name enters lexical scope. */
    node_t*pending=w->pending_slice;w->pending_slice=n;
    boolean_t ok=obs_node(w,make,False);w->pending_slice=pending;if(!ok)return False;
    if(!s->declarations) {
        s->declarations=obs_slice_calloc(ObsSliceHashSlots,sizeof(*s->declarations));
        if(!s->declarations)return obs_error(n,"slice declaration table allocation failure");
    }
    obs_slice_record*r=obs_slice_calloc(1,sizeof(*r));
    if(!r)return obs_error(n,"slice declaration allocation failure");
    *r=(obs_slice_record){.id=s->binding_count++,.file_id=w->file_id,.decl_id=w->decl_id,
        .block_id=w->blocks[w->block_depth-1].id,.child=w->blocks[w->block_depth-1].child,
        .element=element,.constructor_kind=constructor,.declaration=n,.make=make,.block=parent,.next=s->records};
    size_t slot=obs_slice_hash(n);while(s->declarations[slot])slot=(slot+1)&(ObsSliceHashSlots-1);
    s->declarations[slot]=r;s->records=r;
    if(!obs_slice_snapshot(w,r) || !obs_bind(w,n))return False;
    w->bindings[w->binding_count-1].slice=r;return True;
}
static boolean_t obs_slice_source_guard(cg_t*,obs_slice_record*);
static boolean_t obs_slice_site_guard(cg_t*,node_t*,obs_slice_record*,uint32_t);
static boolean_t obs_slice_classify(cg_t*cg) {
    obs_slice_state*s=cg->observation_slices;if(!s)return True;
    for(obs_slice_record*r=s->records;r;r=r->next) {
        uint32_t releases=0,deferred=0;obs_slice_reference*release=Null;
        for(obs_slice_reference*u=r->references;u;u=u->next) {
            if(u->use==ObsSliceRelease){++releases;release=u;}
            if(u->deferred)++deferred;
        }
        if(!releases && !deferred){r->schedule=ObsSliceRetained;continue;}
        if(releases!=1)return obs_error(r->declaration,"slice schedule requires zero releases or exactly one designated release");
        if(!deferred && release->block==r->block && release->block_id==r->block_id &&
           release->child>r->child && release->child<r->block->as.block.stmts.count &&
           r->block->as.block.stmts.items[release->child]==release->parent) {
            for(obs_slice_reference*u=r->references;u;u=u->next)
                if(u->owner_child>release->child)
                    return obs_error(u->ident,"slice reference occurs after its direct release");
            r->schedule=ObsSliceDirect;r->release=release->operation;continue;
        }
        node_t*d=release->deferred;
        node_t*b=d && d->kind==NodeDeferStmt?d->as.defer_stmt.body:Null;
        if(deferred==1 && d && (size_t)r->child+1<r->block->as.block.stmts.count &&
           r->block->as.block.stmts.items[r->child+1]==d && release->owner_child==r->child+1 &&
           obs_slice_braces(cg,b) && b->as.block.stmts.count==1 && b->as.block.stmts.items[0]==release->parent &&
           release->block==b && release->child==0) {
            r->schedule=ObsSliceDefer;r->release=release->operation;continue;
        }
        return obs_error(release->operation,"slice release must be direct later same-block or exact adjacent defer { rem.(s); }");
    }
    s->frozen=True;
    uint32_t total=0;
    for(obs_slice_record*r=s->records;r;r=r->next) {
        if(!obs_slice_source_guard(cg,r) || !obs_slice_site_guard(cg,r->make,r,r->constructor_kind))return False;
        if(r->item_count>STS_OBS_POSITIONAL_ITEMS_AGGREGATE_LIMIT-total)
            return obs_error(r->make,"positional frozen aggregate capacity");
        total+=r->item_count;
    }
    if(total!=s->positional_item_count)return obs_error(cg->ast,"positional frozen aggregate identity");
    return True;
}

static boolean_t obs_variable(obs_walk *w, node_t *n, boolean_t runtime) {
    if(n->as.var_decl.type.base==TypeSlice)return obs_slice_variable(w,n,runtime);
    if (!obs_type(n,n->as.var_decl.type,False) || n->as.var_decl.attr_flags ||
        n->as.var_decl.bitfield_width ||
        (n->as.var_decl.flags & ~(VdeclConst|VdeclFinal|VdeclArray)) ||
        ((n->as.var_decl.flags & VdeclArray) && n->as.var_decl.type.is_pointer) ||
        (n->as.var_decl.storage == StorageHeap && !n->as.var_decl.type.is_pointer))
        return obs_error(n, "unsupported storage, declaration attribute or implicit allocation");
    if (n->as.var_decl.array_ndim < 0 || n->as.var_decl.array_ndim > 8)
        return obs_error(n,"array rank");
    for (int i=0;i<n->as.var_decl.array_ndim;++i) {
        if (n->as.var_decl.array_sizes[i] < 0)
            return obs_error(n,"non-fixed array extent");
        const char*name=n->as.var_decl.array_size_names[i];
        obs_binding*b=name?obs_binding_lookup(w,name):Null;
        if(b && b->slice)return obs_error(n,"slice descriptor cannot be a named array extent");
    }
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
            if(!obs_slice_block_enter(w,n))return False;
            for (usize_t i=0;i<n->as.block.stmts.count;++i) {
                node_t *s=n->as.block.stmts.items[i];
                if(i>UINT32_MAX)return obs_error(s,"block child identity capacity");
                w->blocks[w->block_depth-1].child=(uint32_t)i;
                if (w->reason == STS_OBS_UNREACHABLE) obs_exclude(w,s,STS_OBS_UNREACHABLE);
                OS(s);
                if (obs_terminates(w->cg,s)) w->reason=STS_OBS_UNREACHABLE;
            }
            w->binding_count=saved; w->reason=reason; --w->block_depth; return True;
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
        case NodeDeferStmt: {
            uint32_t old=w->anchor_kind;node_t*deferred=w->deferred;
            w->anchor_kind=STS_OBS_ANCHOR_DEFER;w->deferred=n;
            OS(n->as.defer_stmt.body);w->anchor_kind=old;w->deferred=deferred;return True;
        }
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
            obs_binding*b=obs_binding_lookup(w,n->as.ident.name);
            if(b && b->slice)return obs_slice_collect_reference(w,n,b->slice);
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
        case NodeLenExpr: case NodeCapExpr: {
            node_t*operand=n->as.len_expr.operand;
            obs_binding*b=operand && operand->kind==NodeIdentExpr?obs_binding_lookup(w,operand->as.ident.name):Null;
            if(b && b->slice) {ON(operand);return True;}
            if(!obs_array_query_rank(w,operand))
                return obs_error(n,"fixed-array query requires an admitted array with a remaining dimension");
            /* The bounded shape classifier cannot replace the complete walk:
             * nested calls, arms and effects retain their ordinary inventory. */
            ON(n->as.len_expr.operand); return True;
        }
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
        case NodeMakeExpr: {
            node_t*d=w->pending_slice;
            uint32_t element=d?obs_slice_type_element(d->as.var_decl.type):0;
            uint32_t constructor=obs_slice_constructor(n,element);
            if(!d || d->as.var_decl.init!=n || !constructor)
                return obs_error(n,"make must be the exact admitted direct slice initializer");
            if(constructor==STS_OBS_SLICE_MAKE_POSITIONAL) {
                node_t*init=n->as.make_expr.init;
                usize_t count=init->as.compound_init.items.count;
                obs_slice_state*s=obs_slice_state_get(w->cg,n);
                if(!s || count>STS_OBS_POSITIONAL_ITEMS_PER_CONSTRUCTOR_LIMIT ||
                   s->positional_item_count>STS_OBS_POSITIONAL_ITEMS_AGGREGATE_LIMIT ||
                   count>STS_OBS_POSITIONAL_ITEMS_AGGREGATE_LIMIT-s->positional_item_count ||
                   count>SIZE_MAX/sizeof(obs_slice_item) || count>init->as.compound_init.items.capacity ||
                   (count && !init->as.compound_init.items.items))
                    return obs_error(n,"positional item count or aggregate capacity");
                if(!obs_origin(w,init) || init->observation_file_id_plus_one!=w->file_id+1 || init->headers)
                    return obs_error(init,"positional compound wrapper source owner or shape");
                for(usize_t i=0;i<count;++i)
                    if(!obs_item_shape(w,init->as.compound_init.items.items[i],w->depth+2))return False;
                for(usize_t i=0;i<count;++i)
                    if(!obs_item_as(w,init->as.compound_init.items.items[i],d->as.var_decl.type.elem_type[0],0))return False;
                if(!obs_site(w,n,constructor))return False;
                if(n->observation_site_id) {
                    sts_observation_site*site=&w->cg->observation_manifest->sites[n->observation_site_id-1];
                    site->slice_element_kind=element;site->slice_abi_stride=sts_observation_slice_stride(element);
                    site->slice_count_arity=0;site->slice_initializer_item_count=(uint32_t)count;
                }
                /* One ordinary expression inventory walk retains conditional/
                 * logical anchors; plain item lines are not statement anchors. */
                ON(init);return True;
            }
            if(!obs_site(w,n,constructor))return False;
            if(n->observation_site_id) {
                sts_observation_site*site=&w->cg->observation_manifest->sites[n->observation_site_id-1];
                site->slice_element_kind=obs_slice_element(n->as.make_expr.elem_type);
                site->slice_abi_stride=sts_observation_slice_stride(site->slice_element_kind);
                site->slice_count_arity=n->as.make_expr.cap?2:1;
            }
            ON(n->as.make_expr.len);ON(n->as.make_expr.cap);
            node_t*counts[]={n->as.make_expr.len,n->as.make_expr.cap};
            for(unsigned i=0;i<2;++i)if(counts[i]) {
                type_info_t t=obs_slice_count_type(w,counts[i],0);
                if(t.is_pointer || t.base<TypeBool || t.base>TypeU64)
                    return obs_error(counts[i],"slice count must have ordinary primitive integer or bool type");
            }
            return True;
        }
        case NodeRemStmt: {
            node_t*ptr=n->as.rem_stmt.ptr;
            obs_binding*b=ptr && ptr->kind==NodeIdentExpr?obs_binding_lookup(w,ptr->as.ident.name):Null;
            if(!obs_site(w,n,b && b->slice?STS_OBS_SLICE_REM:STS_OBS_REM))return False;
            ON(ptr);return True;
        }
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
    if (++w->depth > STS_OBS_SLICE_DEPTH_LIMIT) { --w->depth; return obs_error(n,"source inventory nesting limit"); }
    w->path[w->depth-1]=n;
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
    if(cg->observation_inventory_error || !obs_slice_classify(cg))goto done;
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
        (op==ObsNew||op==ObsMov||op==ObsFinish||op==ObsSliceNew)?"obs.result":"");
}
static void cg_observation_declare(cg_t *cg) {
    if (!cg->observation_config) return;
    LLVMTypeRef p=LLVMPointerTypeInContext(cg->ctx,0),u=LLVMInt32TypeInContext(cg->ctx),z=LLVMInt64TypeInContext(cg->ctx),v=LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef init[]={p,p,p,p,p,u,u,u,u},one[]={u},newargs[]={z,u},movargs[]={p,z,u},remargs[]={p,u},finish[]={z,z};
    LLVMTypeRef types[]={LLVMFunctionType(v,init,9,0),LLVMFunctionType(v,one,1,0),LLVMFunctionType(v,one,1,0),
        LLVMFunctionType(v,one,1,0),LLVMFunctionType(p,newargs,2,0),LLVMFunctionType(p,movargs,3,0),
        LLVMFunctionType(v,remargs,2,0),LLVMFunctionType(u,finish,2,0),
        LLVMFunctionType(p,newargs,2,0),LLVMFunctionType(v,remargs,2,0)};
    const char *names[]={"__stasha_obs_init_v4","__stasha_obs_test_begin","__stasha_obs_test_end","__stasha_obs_hit",
        "__stasha_obs_new","__stasha_obs_mov","__stasha_obs_rem","__stasha_obs_finish",
        "__stasha_obs_slice_new_v4","__stasha_obs_slice_rem_v4"};
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
static boolean_t obs_slice_source_guard(cg_t*cg,obs_slice_record*r) {
    obs_slice_state*s=cg->observation_slices;
    const sts_observation_graph*g=cg->observation_config->graph;
    if(!r || !s || !s->frozen || !g || !g->frozen || g->failed || r->id>=s->binding_count ||
       r->file_id>=g->file_count || r->decl_id>=cg->observation_manifest->decl_count ||
       cg->observation_manifest->declarations[r->decl_id].file_id!=r->file_id ||
       !cg->observation_manifest->declarations[r->decl_id].selected ||
       r->block_id>=s->block_count || s->blocks[r->block_id].node!=r->block ||
       s->blocks[r->block_id].file_id!=r->file_id || s->blocks[r->block_id].decl_id!=r->decl_id ||
       !r->block || r->block->kind!=NodeBlock || r->child>=r->block->as.block.stmts.count ||
       r->block->as.block.stmts.items[r->child]!=r->declaration ||
       obs_slice_declaration_find(cg,r->declaration)!=r ||
       r->declaration->kind!=NodeVarDecl || r->declaration->as.var_decl.init!=r->make ||
       r->declaration->as.var_decl.storage!=StorageHeap || r->declaration->as.var_decl.flags ||
       r->declaration->as.var_decl.attr_flags || r->declaration->as.var_decl.array_ndim ||
       obs_slice_type_element(r->declaration->as.var_decl.type)!=r->element ||
       !r->make || obs_slice_constructor(r->make,r->element)!=r->constructor_kind ||
       r->make->observation_line!=r->make_line || r->make->observation_col!=r->make_col ||
       r->make->observation_end_line!=r->make_end_line || r->make->observation_end_col!=r->make_end_col ||
       r->make->observation_token_begin!=r->make_token_begin || r->make->observation_token_end!=r->make_token_end ||
       r->declaration->observation_file_id_plus_one!=r->file_id+1 ||
       r->make->observation_file_id_plus_one!=r->file_id+1 ||
       !r->declaration->observation_file || strcmp(r->declaration->observation_file,g->files[r->file_id].canonical_path) ||
       !r->make->observation_file || strcmp(r->make->observation_file,g->files[r->file_id].canonical_path) ||
       r->schedule<ObsSliceRetained || r->schedule>ObsSliceDefer)
        return obs_error(r?r->declaration:cg->ast,"slice escaped its frozen declaration/source/block identity");
    if(r->constructor_kind==STS_OBS_SLICE_MAKE_POSITIONAL) {
        node_t*init=r->make->as.make_expr.init;
        if(init!=r->compound || !init || init->kind!=NodeCompoundInit || init->headers ||
           init->observation_file_id_plus_one!=r->file_id+1 || init->observation_expanded || init->from_lib ||
           !init->observation_file || strcmp(init->observation_file,g->files[r->file_id].canonical_path) ||
           init->as.compound_init.items.count!=r->item_count ||
           r->item_count>init->as.compound_init.items.capacity ||
           r->item_count>STS_OBS_POSITIONAL_ITEMS_PER_CONSTRUCTOR_LIMIT ||
           (r->item_count && (!r->items || !init->as.compound_init.items.items || !s->item_identities)))
            return obs_error(r->make,"positional constructor escaped its frozen wrapper/count identity");
        node_t*previous=Null;
        for(uint32_t i=0;i<r->item_count;++i) {
            const obs_slice_item*saved=&r->items[i];node_t*item=init->as.compound_init.items.items[i];
            if(item!=saved->node || saved->ordinal!=i || saved->file_id!=r->file_id ||
               !item || (uint32_t)item->kind!=saved->kind ||
               !obs_slice_item_span(r->make,item,previous,r->file_id) || item->headers ||
               item->observation_expanded || item->from_lib || item->generic_use || item->is_c_extern ||
               !item->observation_file || strcmp(item->observation_file,g->files[r->file_id].canonical_path) ||
               item->observation_line!=saved->line || item->observation_col!=saved->col ||
               item->observation_end_line!=saved->end_line || item->observation_end_col!=saved->end_col ||
               item->observation_token_begin!=saved->token_begin || item->observation_token_end!=saved->token_end)
                return obs_error(item,"positional item escaped its frozen original identity/order/source span");
            size_t slot=obs_slice_hash(item);
            while(s->item_identities[slot] && s->item_identities[slot]!=item)slot=(slot+1)&(ObsSliceHashSlots-1);
            if(s->item_identities[slot]!=item)return obs_error(item,"missing frozen positional item identity");
            previous=item;
        }
    } else if(r->compound || r->items || r->item_count)
        return obs_error(r->make,"count make carries positional source metadata");
    return True;
}
static boolean_t obs_slice_site_guard(cg_t*cg,node_t*n,obs_slice_record*r,uint32_t kind) {
    sts_observation_manifest*m=cg->observation_manifest;
    if(!n->observation_site_id || n->observation_site_id>m->site_count)
        return obs_error(n,"slice operation has no original static site");
    sts_observation_site*t=&m->sites[n->observation_site_id-1];
    if(t->kind!=kind || t->file_id!=r->file_id || t->decl_id!=r->decl_id ||
       t->owner_test!=r->owner_test || !t->owner || !r->owner || strcmp(t->owner,r->owner) ||
       t->line!=n->observation_line || t->col!=n->observation_col ||
       t->end_line!=n->observation_end_line || t->end_col!=n->observation_end_col ||
       (kind==STS_OBS_SLICE_MAKE && (t->slice_element_kind!=r->element ||
        t->slice_abi_stride!=sts_observation_slice_stride(r->element) ||
        t->slice_count_arity!=(n->as.make_expr.cap?2u:1u) || t->slice_initializer_item_count)) ||
       (kind==STS_OBS_SLICE_MAKE_POSITIONAL && (t->slice_element_kind!=r->element ||
        t->slice_abi_stride!=sts_observation_slice_stride(r->element) || t->slice_count_arity ||
        t->slice_initializer_item_count!=r->item_count)) ||
       (kind==STS_OBS_SLICE_REM && (t->slice_element_kind || t->slice_abi_stride || t->slice_count_arity ||
        t->slice_initializer_item_count)))
        return obs_error(n,"slice operation static site metadata mismatch");
    return True;
}
static obs_slice_record *cg_observation_slice_declaration(cg_t*cg,node_t*n,LLVMTypeRef type) {
    obs_slice_record*r=obs_slice_declaration_find(cg,n);
    if(!obs_slice_source_guard(cg,r))return Null;
    if(cg->observation_block!=r->block || cg->observation_child!=r->child || type!=slice_struct_type(cg)) {
        obs_error(n,"slice declaration lowering differs from its original block or descriptor type");return Null;
    }
    return r;
}
static boolean_t cg_observation_slice_bind(cg_t*cg,node_t*n,obs_slice_record*r,symbol_t*symbol) {
    if(!r)return !cg->observation_config || n->as.var_decl.type.base!=TypeSlice;
    if(!obs_slice_source_guard(cg,r))return False;
    if(n!=r->declaration || !symbol || symbol->storage!=StorageHeap ||
       obs_slice_type_element(symbol->stype)!=r->element || symbol->type!=slice_struct_type(cg) ||
       !symbol->value || !LLVMIsAAllocaInst(symbol->value) ||
       LLVMGetAllocatedType(symbol->value)!=symbol->type ||
       LLVMGetBasicBlockParent(LLVMGetInstructionParent(symbol->value))!=cg->current_fn ||
       (symbol->observation_slice && symbol->observation_slice!=r) ||
       (r->alloca && (r->alloca!=symbol->value || r->descriptor_type!=symbol->type)))
        return obs_error(n,"slice declaration storage association mismatch");
    r->alloca=symbol->value;r->descriptor_type=symbol->type;symbol->observation_slice=r;return True;
}
static boolean_t cg_observation_slice_make_guard(cg_t*cg,node_t*n,type_info_t element,uint64_t stride) {
    if(!cg->observation_config)return True;
    obs_slice_record*r=cg->observation_slice_pending;
    if(!obs_slice_source_guard(cg,r))return False;
    if(r->make!=n || cg->observation_block!=r->block || cg->observation_child!=r->child ||
       obs_slice_element(element)!=r->element || !cg->di_data_layout ||
       LLVMPointerSize(cg->di_data_layout)!=8 || !stride ||
       LLVMABISizeOfType(cg->di_data_layout,get_llvm_type(cg,element))!=stride ||
       stride!=sts_observation_slice_stride(r->element))
        return obs_error(n,"slice make original initializer, element or target ABI stride mismatch");
    if(r->constructor_kind==STS_OBS_SLICE_MAKE_POSITIONAL &&
       ((uint64_t)r->item_count>cg_layout_max_size(cg)/stride ||
        (uint64_t)r->item_count*stride>UINT64_C(524288)))
        return obs_error(n,"positional slice byte count exceeds its frozen request bound");
    return obs_slice_site_guard(cg,n,r,r->constructor_kind);
}
static symbol_t *obs_slice_use_guard(cg_t*cg,node_t*operation,node_t*ident,uint32_t use) {
    obs_slice_reference*u=obs_slice_reference_find(cg,ident);
    obs_slice_record*r=u?u->binding:Null;
    if(!u || !r || u->ident!=ident || u->operation!=operation || u->use!=use ||
       !ident || ident->kind!=NodeIdentExpr || !operation ||
       ident->observation_file_id_plus_one!=r->file_id+1 ||
       operation->observation_file_id_plus_one!=r->file_id+1 || !obs_slice_source_guard(cg,r)) {
        obs_error(operation,"slice operation lacks its permitted original reference identity");return Null;
    }
    boolean_t shape=use==ObsSliceQuery?
        ((operation->kind==NodeLenExpr || operation->kind==NodeCapExpr) && operation->as.len_expr.operand==ident):
        use==ObsSliceRelease?(operation->kind==NodeRemStmt && operation->as.rem_stmt.ptr==ident && r->release==operation &&
            u->parent && u->parent->kind==NodeExprStmt && u->parent->as.expr_stmt.expr==operation):
        (operation->kind==NodeIndexExpr && operation->as.index_expr.object==ident &&
            (use!=ObsSliceWrite || (u->parent && u->parent->kind==NodeAssignExpr && u->parent->as.assign.target==operation)));
    symbol_t*symbol=cg_lookup(cg,ident->as.ident.name);
    if(!shape || !symbol || symbol->observation_slice!=r || symbol->storage!=StorageHeap || symbol->flags ||
       obs_slice_type_element(symbol->stype)!=r->element || !r->alloca ||
       symbol->value!=r->alloca || symbol->type!=r->descriptor_type || symbol->type!=slice_struct_type(cg) ||
       !LLVMIsAAllocaInst(r->alloca) || LLVMGetAllocatedType(r->alloca)!=r->descriptor_type ||
       LLVMGetBasicBlockParent(LLVMGetInstructionParent(r->alloca))!=cg->current_fn) {
        obs_error(operation,"slice reference resolved to different declaration/storage/element");return Null;
    }
    if(use==ObsSliceRelease && !obs_slice_site_guard(cg,operation,r,STS_OBS_SLICE_REM))return Null;
    return symbol;
}
static boolean_t cg_observation_slice_index_guard(cg_t*cg,node_t*n,boolean_t write) {
    if(!cg->observation_config || !n || n->kind!=NodeIndexExpr)return True;
    node_t*ident=n->as.index_expr.object;
    if(!ident || ident->kind!=NodeIdentExpr)return True;
    symbol_t*symbol=cg_lookup(cg,ident->as.ident.name);
    if(!obs_slice_reference_find(cg,ident) && (!symbol || symbol->stype.base!=TypeSlice))return True;
    return obs_slice_use_guard(cg,n,ident,write?ObsSliceWrite:ObsSliceRead)!=Null;
}
static boolean_t cg_observation_slice_rem_guard(cg_t*cg,node_t*n) {
    if(!cg->observation_config)return True;
    node_t*ident=n->as.rem_stmt.ptr;
    if(!ident || ident->kind!=NodeIdentExpr)return True;
    symbol_t*symbol=cg_lookup(cg,ident->as.ident.name);
    if(!obs_slice_reference_find(cg,ident) && (!symbol || symbol->stype.base!=TypeSlice))return True;
    return obs_slice_use_guard(cg,n,ident,ObsSliceRelease)!=Null;
}
/* Zero means use the separately proven array path, never struct fallback. */
static int cg_observation_slice_query_guard(cg_t*cg,node_t*n,symbol_t**out) {
    *out=Null;if(!cg->observation_config)return 0;
    node_t*ident=n->as.len_expr.operand;
    if(!ident || ident->kind!=NodeIdentExpr)return 0;
    symbol_t*symbol=cg_lookup(cg,ident->as.ident.name);
    if(!obs_slice_reference_find(cg,ident) && (!symbol || symbol->stype.base!=TypeSlice))return 0;
    *out=obs_slice_use_guard(cg,n,ident,ObsSliceQuery);return *out?1:-1;
}

static boolean_t cg_observation_expr_guard(cg_t *cg,node_t *n) {
    if(!cg->observation_config)return True;
    if(n->kind==NodeIndexExpr && !cg_observation_slice_index_guard(cg,n,False))return False;
    if(n->kind==NodeAssignExpr && !cg_observation_slice_index_guard(cg,n->as.assign.target,True))return False;
    if(n->kind==NodeRemStmt && !cg_observation_slice_rem_guard(cg,n))return False;
    if(n->kind==NodeMakeExpr && (!cg->observation_slice_pending || cg->observation_slice_pending->make!=n))
        return obs_error(n,"make escaped its pending original slice declaration");
    if(n->kind==NodeIdentExpr) {
        symbol_t*s=cg_lookup(cg,n->as.ident.name);
        obs_slice_reference*u=obs_slice_reference_find(cg,n);
        if(u || (s && s->stype.base==TypeSlice))
            return u && obs_slice_use_guard(cg,u->operation,n,u->use)?True:obs_error(n,"unclassified slice descriptor lowering");
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
        "__crash_runtime_install","__crash_runtime_register_block","__stasha_obs_init_v4","__stasha_obs_test_begin","__stasha_obs_test_end",
        "__stasha_obs_hit","__stasha_obs_new","__stasha_obs_mov","__stasha_obs_rem","__stasha_obs_finish",
        "__stasha_obs_slice_new_v4","__stasha_obs_slice_rem_v4"};
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
