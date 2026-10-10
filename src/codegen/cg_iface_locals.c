/* I1b.2 closed local concrete identity, deliberately separate from source type
 * facts and retained origin exclusions. Preflight binds the complete body before
 * emission. Only exact original AST uses in this LLVM body consume its immutable
 * path/write result. It emits no IR, instantiates no type, and mutates no AST. */
enum { IfaceLocalMaxNodes=16384, IfaceLocalBuckets=32768,
       IfaceLocalMaxBindings=2048, IfaceLocalMaxProofs=4096,
       IfaceLocalMaxEdges=16384, IfaceLocalMaxPath=32,
       IfaceLocalMaxCoverage=65536 };
typedef enum { LocalClosed, LocalDeclared, LocalUnsupported, LocalMismatch } iface_local_state_t;
typedef struct iface_local_binding iface_local_binding_t;
typedef struct iface_local_node iface_local_node_t;
typedef struct iface_local_proof iface_local_proof_t;
typedef struct iface_local_edge iface_local_edge_t;
typedef struct iface_factory_request iface_factory_request_t;
typedef struct iface_factory_summary iface_factory_summary_t;
typedef struct iface_local_check iface_local_check_t;
typedef struct {
    iface_local_binding_t *root;
    node_t *steps[IfaceLocalMaxPath]; /* exact field; NULL = uniform array step */
    unsigned count, arrays;
    boolean_t valid;
} iface_local_path_t;
struct iface_local_binding {
    iface_local_binding_t *next, *outer;
    node_t *decl, *block, *statement;
    symbol_t symbol;                 /* owned stable query view; never emitted */
    iface_source_record_t source;
    iface_source_fact_t fact;
    uint64_t extent;
    unsigned ordinal;
    boolean_t local, exposed, deferred, no_new_proof, demanded, duplicate_multi_target, legacy_demanded, storage_seen;
    const char *boundary;
    iface_local_node_t *frontier;
    unsigned frontier_count;
};
struct iface_local_node {
    iface_local_node_t *next, *hash_next;
    node_t *ast, *block, *statement, *parent;
    unsigned ordinal, loops, control, deferred, inline_carrier;
    iface_source_fact_t fact;
    iface_local_path_t path;
    boolean_t fact_ready, write, compound_write, sink, terminal, constructor_needed, transfer_needed, checked_write;
    iface_source_fact_t transfer_target, extracted_source;
    boolean_t extracted_write;
    node_t *extracted_decl;
    iface_source_fact_t constructor_target;
    uint64_t constructor_extent;
    node_t *rhs;
    iface_local_proof_t *proof, *upgrade;
    boolean_t legacy_required, constructor_seen, expression_seen, result_seen, consumed;
    const char *physical_failure;
    LLVMValueRef selected_function;
    node_t *selected_decl;
    boolean_t named_direct;
};
struct iface_local_edge {
    iface_local_edge_t *next;
    iface_local_proof_t *source;
    node_t *read;                    /* physical source read, not root-only edge */
    boolean_t initializer;
    iface_local_state_t gate;
};
struct iface_local_proof {
    iface_local_proof_t *next;
    iface_local_path_t path;
    iface_source_fact_t target;
    iface_local_edge_t *edges;
    iface_local_state_t intrinsic, state, seed;
    node_t *evidence;
    const char *reason;
    unsigned seed_visit, seal_visit;
    boolean_t built, upgrade, projected, result_boundary;
    iface_local_check_t *checks;
    iface_factory_request_t *requests;
};
enum { FactoryCheckConstructor=1, FactoryCheckExpression=2, FactoryCheckResult=4 };
struct iface_local_check {
    iface_local_check_t *next;
    iface_local_node_t *node;
    unsigned kind;
};
struct iface_local_plan {
    iface_local_plan_t *previous, *retained_next;
    cg_t *cg;
    LLVMValueRef context;
    node_t *owner, *body;
    iface_local_binding_t *bindings, *environment;
    iface_local_node_t *nodes, **buckets;
    iface_local_proof_t *proofs;
    usize_t node_count, binding_count, proof_count, edge_count, legacy_proof_count;
    node_t *block, *statement, *parent;
    unsigned ordinal, loops, control, deferred, inline_carrier;
    boolean_t building, frozen, incomplete, rejected;
    node_t *failure;
    const char *reason;
    iface_source_frame_t frame;
    boolean_t retained, emitting, overlay_building, overlay_incomplete, factory_building;
    boolean_t participating, factory, finalized, factory_classified, factory_eligible;
    usize_t check_count;
    node_t *overlay_failure, *return_node, *return_operand, *return_in_progress;
    const char *overlay_reason, *factory_failure;
    LLVMValueRef raw_return, native_return, returned_value, return_storage;
    unsigned returns_started, returns_seen, raw_returns_seen;
};
/* New proof budgets bound temporary inventories as well as retained state. */
enum { FactoryMaxVisits=1048576, FactoryMaxTransientNodes=524288,
       FactoryMaxPlans=256, FactoryMaxBodies=64, FactoryMaxPaths=256,
       FactoryMaxRequests=1024, FactoryMaxRetainedNodes=262144,
       FactoryMaxChecks=16384 };
/* Independent source/lifetime/correspondence review must release this gate. */
#define IFACE_FACTORY_RESULT_PROMOTION 1
typedef enum { FactoryClean, FactoryCleanupPresent, FactoryUnknown } iface_factory_clean_t;
struct iface_factory_summary {
    iface_factory_summary_t *next;
    iface_local_plan_t *plan;
    iface_local_path_t path;
    iface_local_proof_t *root_proof, *proof;
    iface_local_state_t state;
    const char *reason;
    node_t *evidence;
};
struct iface_factory_request {
    iface_factory_request_t *next, *proof_next;
    iface_local_plan_t *consumer;
    iface_factory_summary_t *summary;
    iface_local_proof_t *proof;
    node_t *call, *decl;
    LLVMValueRef function, emitted;
    LLVMTypeRef result;
    iface_source_fact_t source;
    const char *failure;
    unsigned selected_count, emitted_count;
    boolean_t consumed;
};
struct iface_factory_state {
    iface_local_plan_t *plans;
    iface_factory_summary_t *summaries;
    iface_factory_request_t *requests;
    usize_t visits, transient_nodes, retained_nodes, plans_count, bodies, paths, request_count;
    boolean_t discovering, exhausted;
};
static boolean_t iface_factory_value(iface_local_plan_t *,iface_local_proof_t *,node_t *,iface_source_fact_t,node_t **,unsigned);
static int iface_factory_consume(cg_t *,iface_local_plan_t *,iface_local_node_t *,symbol_t *);
static void iface_factory_raw_result(cg_t *,node_t *,LLVMValueRef);
static iface_local_plan_t *iface_factory_current(cg_t *);

static boolean_t iface_local_error(node_t *n,const char *why) {
    ast_source_loc_t loc=iface_node_loc(n);if(!loc.col)loc.col=1;if(!loc.len)loc.len=1;
    iface_error(loc,"interface local transfer:",why);return False;
}
static void iface_local_fail(iface_local_plan_t *p,node_t *n,const char *why) {
    if(p->overlay_building){p->overlay_incomplete=True;if(!p->overlay_reason){p->overlay_reason=why;p->overlay_failure=n;}return;}
    p->incomplete=True;if(!p->reason){p->reason=why;p->failure=n;}
}
typedef union { heap_t heap; long double alignment; } iface_local_allocation_t;
static void *iface_local_zero(size_t size) {
    heap_t h=allocate(1,sizeof(iface_local_allocation_t)+size);
    iface_local_allocation_t *a=h.pointer;a->heap=h;
    void *v=a+1;memset(v,0,size);return v;
}
static void iface_local_release(void *v) {
    if(v){iface_local_allocation_t *a=(iface_local_allocation_t*)v-1;deallocate(a->heap);}
}
static void *iface_local_alloc(iface_local_plan_t *p,size_t size) {
    (void)p;return iface_local_zero(size);
}
static iface_local_plan_t *iface_local_current(cg_t *cg) {
    iface_local_plan_t *p=cg->iface_local_plan;
    return p&&p->context==cg->current_fn?p:Null;
}
static iface_local_node_t *iface_local_node(iface_local_plan_t *p,node_t *n,boolean_t create) {
    if(!n||!p||!p->buckets)return Null;
    usize_t h=((uintptr_t)n>>4)&(IfaceLocalBuckets-1);
    for(iface_local_node_t *r=p->buckets[h];r;r=r->hash_next)if(r->ast==n)return r;
    if(!create)return Null;
    iface_factory_state_t *fs=p->cg->iface_factories;
    if(fs&&fs->discovering){
        if(fs->visits>=FactoryMaxVisits||fs->transient_nodes>=FactoryMaxTransientNodes){fs->exhausted=True;return Null;}
        ++fs->visits;++fs->transient_nodes;
    }
    if(p->node_count>=IfaceLocalMaxNodes){iface_local_fail(p,n,"body exceeds bounded local node inventory");return Null;}
    iface_local_node_t *r=iface_local_alloc(p,sizeof(*r));if(!r)return Null;
    r->ast=n;r->parent=p->parent;r->block=p->block;r->statement=p->statement;r->ordinal=p->ordinal;
    r->loops=p->loops;r->control=p->control;r->deferred=p->deferred;r->inline_carrier=p->inline_carrier;
    r->next=p->nodes;p->nodes=r;r->hash_next=p->buckets[h];p->buckets[h]=r;++p->node_count;
    return r;
}
/* Location repair is diagnostic-only. A copied location node never enters
 * inventory lookup or authorizes a transfer. */
static boolean_t iface_local_error_at(iface_local_plan_t *p,node_t *original,const char *why) {
    node_t located=original?*original:*p->owner;
    node_t *at=original;
    for(unsigned depth=0;(!located.col||!located.line)&&at&&depth<IfaceMaxMetadataDepth;depth++){
        iface_local_node_t *r=iface_local_node(p,at,False);at=r?r->parent:Null;
        if(at&&at->col){located.col=at->col;located.len=at->len;located.line=at->line;if(at->source_file)located.source_file=at->source_file;}
    }
    return iface_local_error(&located,why);
}
static symbol_t *iface_local_lookup(cg_t *cg,const char *name,boolean_t *active) {
    iface_local_plan_t *p=cg->iface_query_frame?cg->iface_query_frame->plan:iface_local_current(cg);*active=p&&p->building;
    if(!*active)return Null;
    for(iface_local_binding_t *b=p->environment;b;b=b->outer)
        if(b->symbol.name&&!strcmp(b->symbol.name,name))return &b->symbol;
    return Null;
}
static boolean_t iface_local_fact(cg_t *cg,node_t *n,iface_source_fact_t *out) {
    iface_local_plan_t *p=cg->iface_query_frame?cg->iface_query_frame->plan:iface_local_current(cg);
    /* The ordinary source query remains unchanged outside preflight. */
    if(!p||!p->building)return False;
    iface_local_node_t *r=iface_local_node(p,n,False);
    if(!r||!r->fact_ready)return False;*out=r->fact;return True;
}
static iface_local_binding_t *iface_local_binding(iface_local_plan_t *p,const char *name) {
    for(iface_local_binding_t *b=p->environment;name&&b;b=b->outer)
        if(b->symbol.name&&!strcmp(b->symbol.name,name))return b;
    return Null;
}
static boolean_t iface_local_struct(iface_source_fact_t f) {
    return f.status==IfaceSourceKnown&&f.contract.nominal&&
        f.contract.nominal->as.type_decl.decl_kind==TypeDeclStruct&&
        !f.contract.type.is_pointer&&!(f.origin&IfaceOriginGeneric);
}
static iface_source_fact_t iface_local_query(iface_local_plan_t *p,node_t *n) {
    iface_local_node_t *r=iface_local_node(p,n,False);
    return r&&r->fact_ready?r->fact:iface_source_unknown("missing exact preflight expression identity");
}
static boolean_t iface_local_append(iface_local_plan_t *p,iface_local_path_t *path,node_t *step,node_t *at) {
    (void)p;(void)at;
    if(!path->valid)return False;
    if(path->count>=IfaceLocalMaxPath||(!step&&path->arrays)){
        path->valid=False;return False;}
    path->steps[path->count++]=step;if(!step)++path->arrays;return True;
}
static void iface_local_expose(iface_local_node_t *r,const char *why) {
    if(r&&r->path.root){r->path.root->exposed=True;r->path.root->boundary=why;}
}
static void iface_local_visit(iface_local_plan_t *,node_t *,unsigned);
static void iface_local_list(iface_local_plan_t *p,node_list_t list,unsigned depth) {
    if(list.count>IfaceLocalMaxNodes){iface_local_fail(p,p->statement,"child list exceeds bounded local inventory");return;}
    for(usize_t i=0;i<list.count;i++)iface_local_visit(p,list.items[i],depth+1);
}
static iface_local_binding_t *iface_local_bind(iface_local_plan_t *p,node_t *d,const char *name,
                                               iface_source_fact_t fact,boolean_t local) {
    if(p->binding_count>=IfaceLocalMaxBindings){iface_local_fail(p,d,"body exceeds bounded lexical binding inventory");return Null;}
    iface_local_binding_t *b=iface_local_alloc(p,sizeof(*b));if(!b)return Null;
    b->decl=d;b->block=p->block;b->statement=p->statement;b->ordinal=p->ordinal;b->local=local;
    b->fact=fact;b->fact.context=p->context;b->fact.evidence=d;
    b->source.decl=d;b->source.context=p->context;b->source.fact=b->fact;
    b->symbol.name=(char*)name;b->symbol.stype=fact.contract.type;
    if(d&&d->kind==NodeVarDecl){
        b->symbol.iface_source=&b->source;
        if(d->as.var_decl.flags&VdeclConst)b->symbol.flags|=SymConst;
        if(d->as.var_decl.flags&VdeclFinal)b->symbol.flags|=SymFinal;
    }
    b->no_new_proof=p->inline_carrier!=0;
    b->next=p->bindings;p->bindings=b;b->outer=p->environment;p->environment=b;++p->binding_count;
    return b;
}
/* Positive local extents are source literals, earlier immutable literal names,
 * or a bounded flat count. No LLVM initializer access or constant evaluator. */
static uint64_t iface_local_extent(iface_local_plan_t *p,node_t *d) {
    if(!d||d->kind!=NodeVarDecl||d->as.var_decl.array_ndim>1)return 0;
    const char *name=d->as.var_decl.array_size_names[0];
    if(name){iface_local_binding_t *b=iface_local_binding(p,name);node_t *c=b?b->decl:Null;
        if(!c){symbol_t *s=symtab_lookup(&p->cg->globals,name);c=s&&s->iface_source?s->iface_source->decl:Null;}
        if(!c||c->kind!=NodeVarDecl||!(c->as.var_decl.flags&(VdeclConst|VdeclFinal))||
            c->as.var_decl.type.is_pointer||c->as.var_decl.array_ndim||!c->as.var_decl.init||
            c->as.var_decl.init->kind!=NodeIntLitExpr)return 0;
        iface_source_fact_t constant=b?b->fact:iface_source_declaration(p->cg,c);
        if(!scalar_integer(constant.contract.type)||constant.contract.type.base==TypeBool||
            !scalar_literal_fits(c->as.var_decl.init,constant.contract.type))return 0;
        uint64_t x=c->as.var_decl.init->as.int_lit.value;return x&&x<=IfaceLocalMaxCoverage?x:0;
    }
    long extent=d->as.var_decl.array_sizes[0];
    if(extent>0)return (uint64_t)extent<=IfaceLocalMaxCoverage?(uint64_t)extent:0;
    node_t *init=d->as.var_decl.init;if(!init||init->kind!=NodeCompoundInit)return 0;
    uint64_t cursor=0,count=0;
    for(usize_t i=0;i<init->as.compound_init.items.count;i++){
        node_t *n=init->as.compound_init.items.items[i];
        if(n->kind==NodeInitIndex){if(!n->as.init_index.index||n->as.init_index.index->kind!=NodeIntLitExpr)return 0;cursor=n->as.init_index.index->as.int_lit.value;}
        else if(n->kind==NodeSpreadExpr||n->kind==NodeRangeExpr||n->kind==NodeInitField)return 0;
        if(cursor>=IfaceLocalMaxCoverage)return 0;++cursor;if(cursor>count)count=cursor;
    }
    return count;
}
static void iface_local_declaration(iface_local_plan_t *p,node_t *n,boolean_t seed,unsigned depth) {
    if(!n||n->kind!=NodeVarDecl){iface_local_fail(p,n,"local declaration has no exact AST binding");return;}
    if(n->as.var_decl.name&&!strcmp(n->as.var_decl.name,"_"))return; /* lowering skips initializer too */
    if(seed)iface_local_visit(p,n->as.var_decl.init,depth+1);
    iface_source_fact_t f=iface_source_declaration(p->cg,n);
    if((n->as.var_decl.flags&VdeclLet)&&n->as.var_decl.type.base==TypeVoid&&!n->as.var_decl.type.is_pointer){
        iface_local_node_t *selected=iface_local_node(p,n,False);
        f=seed?iface_local_query(p,n->as.var_decl.init):selected&&selected->fact_ready?selected->fact:iface_source_unknown("multi-result inferred binding is a declaration boundary");
        f.addressable=True;f.access=(n->as.var_decl.flags&(VdeclConst|VdeclFinal))?PtrRead:PtrReadWrite;f.slot_domain=StorageStack;
    }
    /* Resolve names before installing the declaration, matching lowering. */
    uint64_t extent=f.dimensions?iface_local_extent(p,n):0;
    iface_local_binding_t *b=iface_local_bind(p,n,n->as.var_decl.name,f,True);
    if(!b)return;b->extent=extent;
    /* Query-only symbols carry original declared exclusions. Stored-value
     * closure belongs exclusively to the graph; legacy initializer screening
     * uses live locals and must run only during actual lowering. */
    b->symbol.iface_excluded_origin=f.origin;
    if(!seed)b->no_new_proof=True;
    if(n->as.var_decl.storage==StorageHeap||(n->as.var_decl.flags&(VdeclAtomic|VdeclVolatile|VdeclTls)))b->no_new_proof=True;
    if(iface_local_struct(f)){struct_reg_t *sr=find_struct(p->cg,f.contract.nominal->as.type_decl.name);
        if(sr&&sr->destructor){b->exposed=True;b->boundary="implicit destructor is outside closed local proof";}}
    iface_local_node_t *r=iface_local_node(p,n,True);if(r){r->path=(iface_local_path_t){.root=b,.valid=True};r->fact=f;r->fact_ready=True;}
}
static void iface_local_capture(iface_local_plan_t *p,capture_entry_t *cs,usize_t count,node_t *n) {
    if(count>IfaceLocalMaxBindings){iface_local_fail(p,n,"capture list exceeds bounded local inventory");return;}
    for(usize_t i=0;i<count;i++){iface_local_binding_t *b=iface_local_binding(p,cs[i].name);
        if(b){b->exposed=True;b->boundary="captured local storage is outside closed local proof";}}
}
static void iface_local_match(iface_local_plan_t *p,node_t *n,unsigned depth) {
    iface_local_visit(p,n->as.match_stmt.expr,depth+1);
    iface_source_fact_t subject=iface_local_query(p,n->as.match_stmt.expr);
    enum_reg_t *subject_enum=Null;
    for(usize_t i=0;i<n->as.match_stmt.arms.count;i++){
        node_t *a=n->as.match_stmt.arms.items[i];
        if(!a->as.match_arm.is_wildcard&&a->as.match_arm.enum_name){subject_enum=find_enum(p->cg,a->as.match_arm.enum_name);break;}
    }
    for(usize_t i=0;i<n->as.match_stmt.arms.count;i++){
        node_t *a=n->as.match_stmt.arms.items[i];if(!a||a->kind!=NodeMatchArm){iface_local_fail(p,n,"match arm is missing its original AST");continue;}
        iface_local_node(p,a,True);iface_local_binding_t *save=p->environment;++p->control;
        if(a->is_any_arm){
            iface_local_visit(p,a->as.match_arm.guard_expr,depth+1); /* actual guard-before-binding order */
            if(a->any_bind_name)iface_local_bind(p,a,a->any_bind_name,iface_source_from_type(p->cg,a->any_bind_ti,a->any_bind_storage,a),False);
        }else if(a->as.match_arm.is_wildcard){
            if(a->as.match_arm.guard_expr)iface_local_fail(p,a,"wildcard guards have no admitted lowering correspondence");
            if(a->as.match_arm.bind_name)iface_local_bind(p,a,a->as.match_arm.bind_name,subject,False);
        }else if(a->as.match_arm.is_literal){
            iface_local_visit(p,a->as.match_arm.literal_expr,depth+1);iface_local_visit(p,a->as.match_arm.guard_expr,depth+1);
        }else{
            if(a->as.match_arm.bind_name){
                node_t *e=iface_quiet_type(p->cg,subject_enum?subject_enum->name:a->as.match_arm.enum_name),*variant=Null;
                for(usize_t j=0;e&&j<e->as.type_decl.variants.count;j++){node_t *v=e->as.type_decl.variants.items[j];
                    if(a->as.match_arm.variant_name&&!strcmp(v->as.enum_variant.name,a->as.match_arm.variant_name))variant=v;}
                /* A payloadless pattern label is NOT an emitted local binding. */
                if(variant&&variant->as.enum_variant.has_payload&&subject_enum&&subject_enum->is_tagged)
                    iface_local_bind(p,a,a->as.match_arm.bind_name,iface_source_from_type(p->cg,variant->as.enum_variant.payload_type,variant->as.enum_variant.payload_storage,a),False);
                else if(!variant)iface_local_fail(p,a,"tagged arm has no selected original variant correspondence");
            }
            iface_local_visit(p,a->as.match_arm.guard_expr,depth+1);
        }
        iface_local_visit(p,a->as.match_arm.body,depth+1);--p->control;p->environment=save;
    }
}
static void iface_local_va(iface_local_plan_t *p,node_t *n,unsigned depth) {
    iface_local_visit(p,n->as.va_op.handle,depth+1);iface_local_visit(p,n->as.va_op.copy_src,depth+1);
    iface_local_visit(p,n->as.va_op.count_expr,depth+1);
    iface_local_expose(iface_local_node(p,n->as.va_op.handle,False),"vararg storage is outside closed local proof");
    iface_local_expose(iface_local_node(p,n->as.va_op.copy_src,False),"vararg storage copy is outside closed local proof");
    if(n->as.va_op.op==VaRead){
        for(usize_t i=0;i<n->as.va_op.type_list.count;i++)if(!n->as.va_op.type_list.items[i]||n->as.va_op.type_list.items[i]->kind!=NodeSizeofExpr)
            iface_local_fail(p,n,"vararg extraction type wrapper has no exact correspondence");
    }
    if(n->as.va_op.op!=VaForeach&&n->as.va_op.op!=VaForeachTyped)return;
    node_t *w=n->as.va_op.closure_body;
    if(!w||w->kind!=NodeLambda){iface_local_fail(p,n,"inline vararg loop has no admitted wrapper");return;}
    if(w->as.lambda_expr.capture_count)iface_local_fail(p,w,"inline vararg capture metadata is unsupported");
    iface_local_binding_t *save=p->environment;++p->loops;++p->inline_carrier;
    if(n->as.va_op.op==VaForeach){
        iface_local_bind(p,n,n->as.va_op.closure_param?n->as.va_op.closure_param:"__va_v",
            iface_source_from_type(p->cg,n->as.va_op.next_type,StorageDefault,n),False);
        iface_local_visit(p,w->as.lambda_expr.body,depth+1);
    }else{
        node_t *body=w->as.lambda_expr.body,*match=Null;
        if(body&&body->kind==NodeBlock)for(usize_t i=0;i<body->as.block.stmts.count;i++){
            node_t *s=body->as.block.stmts.items[i];if(s->kind==NodeExprStmt)s=s->as.expr_stmt.expr;
            if(s&&s->kind==NodeMatchStmt&&!match)match=s;else iface_local_fail(p,body,"extra inline typed-vararg wrapper statements are unsupported");
        }
        if(!match)iface_local_fail(p,n,"inline typed-vararg match correspondence is missing");
        else for(usize_t i=0;i<match->as.match_stmt.arms.count;i++){
            node_t *a=match->as.match_stmt.arms.items[i];p->environment=save;
            if(a->as.match_arm.guard_expr)iface_local_fail(p,a,"inline typed-vararg guard is not lowered");
            if(a->is_any_arm){if(a->any_bind_name)iface_local_bind(p,a,a->any_bind_name,iface_source_from_type(p->cg,a->any_bind_ti,a->any_bind_storage,a),False);}
            else if(!a->as.match_arm.is_wildcard){iface_local_fail(p,a,"inline typed-vararg arm has no lowering route");continue;}
            iface_local_visit(p,a->as.match_arm.body,depth+1);
        }
    }
    --p->inline_carrier;--p->loops;p->environment=save;
}
static void iface_local_call(iface_local_plan_t *p,iface_local_node_t *r) {
    node_t *n=r->ast;node_list_t args=iface_source_args(n);symbol_t *selected=iface_source_selected(p->cg,n,0);
    iface_native_t *native=iface_source_native(p->cg,selected);
    r->selected_function=selected?selected->value:Null;r->selected_decl=native?native->decl:Null;
    r->named_direct=n->kind==NodeCallExpr&&!n->generic_use&&selected&&!selected->c_abi&&
        selected->stype.base!=TypeFnPtr&&selected->stype.base!=TypeClosure&&!selected->requires_receiver;
    boolean_t ordinary=native&&native->decl&&native->decl->kind==NodeFnDecl&&!native->c_abi&&
        !native->decl->as.fn_decl.is_variadic&&!native->decl->as.fn_decl.is_async&&
        (n->kind==NodeCallExpr||n->kind==NodeMethodCall||n->kind==NodeSelfMethodCall||n->kind==NodeColonCall||n->kind==NodeConstructorCall);
    if(n->kind==NodeMethodCall){
        node_t *object=n->as.method_call.object;
        boolean_t qualified=object&&object->kind==NodeMemberExpr&&
            find_interface_index(p->cg,object->as.member_expr.field)!=(usize_t)-1;
        if(qualified){iface_local_node_t *receiver=iface_local_node(p,object->as.member_expr.object,False);
            if(receiver&&receiver->path.root){
                r->path=receiver->path;r->sink=True;
                iface_local_binding_t *b=r->path.root;
                boolean_t direct=r->statement&&((r->statement->kind==NodeExprStmt&&r->statement->as.expr_stmt.expr==n)||
                    (r->statement->kind==NodeRetStmt&&r->statement->as.ret_stmt.values.count==1&&r->statement->as.ret_stmt.values.items[0]==n));
                r->terminal=direct&&r->block==b->block&&!r->loops&&!r->control&&!r->deferred&&!r->inline_carrier&&
                    object->as.member_expr.object->kind==NodeIdentExpr;
                ++b->frontier_count;if(!b->frontier)b->frontier=r;
                if(!r->terminal){b->exposed=True;b->boundary="qualified receiver has no direct same-block terminal frontier";}
            }
        }else if(!selected||selected->requires_receiver){
            iface_local_expose(iface_local_node(p,object,False),"ordinary implicit method receiver exposes local storage");
        }
    }
    for(usize_t i=0;i<args.count;i++){
        iface_local_node_t *a=iface_local_node(p,args.items[i],False);if(!a||!a->path.root)continue;
        iface_source_fact_t f=a->fact;
        if(f.dimensions){iface_local_expose(a,"array decay through a call exposes containing local storage");continue;}
        if(!iface_local_struct(f))continue; /* primitives and pointer fields are value reads */
        iface_source_fact_t formal=iface_source_unknown("unknown original native formal");
        if(ordinary&&i<native->decl->as.fn_decl.params.count)formal=iface_source_declaration(p->cg,native->decl->as.fn_decl.params.items[i]);
        if(ordinary&&iface_source_is_interface(formal.contract)&&!formal.contract.type.is_pointer&&args.items[i]->kind==NodeIdentExpr&&
            iface_source_call_is_boxing(p->cg,n,selected)){a->sink=True;continue;}
        if(ordinary&&iface_source_local_copy(f,formal)==IfaceLocalCopyExact)continue; /* physical by-value copy-out */
        iface_local_expose(a,"unknown, indirect, foreign or packed aggregate call is a local proof boundary");
    }
}
/* Every current AST kind has an explicit lowering/binding row. Unsupported
 * producers still visit evaluated children; generated bodies are independent. */
static void iface_local_visit(iface_local_plan_t *p,node_t *n,unsigned depth) {
    if(!n)return;
    if(depth>IfaceMaxMetadataDepth){iface_local_fail(p,n,"local inventory nesting exceeds 256");return;}
    iface_local_node_t *r=iface_local_node(p,n,True);if(!r)return;
    if(r->fact_ready)return;
    node_t *saved_parent=p->parent;p->parent=n;
#define LV(x) iface_local_visit(p,(x),depth+1)
#define LL(x) iface_local_list(p,(x),depth+1)
#define LE(x,w) iface_local_expose(iface_local_node(p,(x),False),(w))
    switch(n->kind){
    case NodeModule:case NodeSubMod:case NodeFnDecl:case NodeTypeDecl:case NodeEnumVariant:
    case NodeLib:case NodeImpDecl:case NodeCHeader:case NodeLibImp:case NodeTestBlock:
    case NodeInitBlock:case NodeExitBlock:case NodeCrashBlock:
        iface_local_fail(p,n,"declaration or separate body appears inside local inventory");break;
    case NodeVarDecl:iface_local_declaration(p,n,True,depth+1);p->parent=saved_parent;return;
    case NodeBlock:{iface_local_binding_t *env=p->environment;node_t *block=p->block,*statement=p->statement;unsigned ordinal=p->ordinal;
        p->block=n;
        if(n->as.block.stmts.count>IfaceLocalMaxNodes){iface_local_fail(p,n,"block exceeds bounded statement inventory");break;}
        for(usize_t i=0;i<n->as.block.stmts.count;i++){p->statement=n->as.block.stmts.items[i];p->ordinal=(unsigned)i;LV(p->statement);}
        p->environment=env;p->block=block;p->statement=statement;p->ordinal=ordinal;break;}
    case NodeForStmt:{iface_local_binding_t *env=p->environment;++p->loops;
        LV(n->as.for_stmt.init);LV(n->as.for_stmt.cond);LV(n->as.for_stmt.body);LV(n->as.for_stmt.update);
        --p->loops;p->environment=env;break;}
    case NodeWhileStmt:++p->loops;LV(n->as.while_stmt.cond);LV(n->as.while_stmt.body);--p->loops;break;
    case NodeDoWhileStmt:++p->loops;LV(n->as.do_while_stmt.body);LV(n->as.do_while_stmt.cond);--p->loops;break;
    case NodeInfLoop:++p->loops;LV(n->as.inf_loop.body);--p->loops;break;
    case NodeForeachStmt:{LV(n->as.foreach_stmt.slice);LE(n->as.foreach_stmt.slice,"foreach view exposes local storage");
        iface_local_binding_t *env=p->environment;iface_source_fact_t f=iface_local_query(p,n->as.foreach_stmt.slice);
        f=f.contract.type.elem_type?iface_source_from_type(p->cg,*f.contract.type.elem_type,StorageDefault,n):iface_source_unknown("foreach iterator is an extracted boundary");
        iface_local_bind(p,n,n->as.foreach_stmt.iter_name,f,False);++p->loops;LV(n->as.foreach_stmt.body);--p->loops;p->environment=env;break;}
    case NodeIfStmt:LV(n->as.if_stmt.cond);++p->control;LV(n->as.if_stmt.then_block);LV(n->as.if_stmt.else_block);--p->control;break;
    case NodeWithStmt:{iface_local_binding_t *env=p->environment;LV(n->as.with_stmt.decl);LV(n->as.with_stmt.cond);
        ++p->control;LV(n->as.with_stmt.body);LV(n->as.with_stmt.else_block);--p->control;p->environment=env;break;}
    case NodeMatchStmt:iface_local_match(p,n,depth+1);break;
    case NodeMatchArm:iface_local_fail(p,n,"match arm lacks parent-specific binding context");break;
    case NodeSwitchStmt:LV(n->as.switch_stmt.expr);++p->control;LL(n->as.switch_stmt.cases);--p->control;break;
    case NodeSwitchCase:LL(n->as.switch_case.values);LV(n->as.switch_case.body);break;
    case NodeDeferStmt:{iface_local_binding_t *env=p->environment;++p->deferred;LV(n->as.defer_stmt.body);--p->deferred;p->environment=env;break;}
    case NodeBreakStmt:case NodeContinueStmt:break;
    case NodeRetStmt:LL(n->as.ret_stmt.values);
        for(usize_t i=0;i<n->as.ret_stmt.values.count;i++){iface_local_node_t *a=iface_local_node(p,n->as.ret_stmt.values.items[i],False);
            if(a&&a->fact.dimensions)iface_local_expose(a,"returned array decays and exposes containing storage");}break;
    case NodeExprStmt:LV(n->as.expr_stmt.expr);break;
    case NodePrintStmt:LL(n->as.print_stmt.args);break;
    case NodeComptimeFmt:LL(n->as.comptime_fmt.args);break;
    case NodeErrorExpr:LL(n->as.error_expr.args);break;
    case NodeRemStmt:LV(n->as.rem_stmt.ptr);LE(n->as.rem_stmt.ptr,"release uses local storage outside closed proof");break;
    case NodeAsmStmt:LL(n->as.asm_stmt.operands);iface_local_fail(p,n,"inline assembly has unbounded local memory effects");break;
    case NodeComptimeIf:{const char *key=n->as.comptime_if.key;boolean_t match=False;
        if(!strcmp(key,"platform")||!strcmp(key,"os"))match=sts_target_os_matches(&p->cg->target_spec,n->as.comptime_if.value);
        else if(!strcmp(key,"arch"))match=sts_target_arch_matches(&p->cg->target_spec,n->as.comptime_if.value);
        else{iface_local_fail(p,n,"compile-time target selector has no admitted correspondence");break;}
        LV(match?n->as.comptime_if.body:n->as.comptime_if.else_body);break;}
    case NodeComptimeAssert:LV(n->as.comptime_assert.expr);break;
    case NodeDesigInit:
        if(n->as.desig_init.fields.count!=n->as.desig_init.values.count)iface_local_fail(p,n,"named constructor field/value correspondence differs");
        LL(n->as.desig_init.values);break;
    case NodeCompoundInit:LL(n->as.compound_init.items);break;
    case NodeInitField:LV(n->as.init_field.value);break;
    case NodeInitIndex:LV(n->as.init_index.index);LV(n->as.init_index.value);break;
    case NodeSpreadExpr:LV(n->as.spread_expr.expr);break;
    case NodeRangeExpr:LV(n->as.range_expr.start);LV(n->as.range_expr.end);LV(n->as.range_expr.step);break;
    case NodeIdentExpr:{iface_local_binding_t *b=iface_local_binding(p,n->as.ident.name);
        if(b){r->fact=b->fact;r->fact_ready=True;r->path=(iface_local_path_t){.root=b,.valid=b->local&&!b->fact.contract.type.is_pointer};
            if(p->deferred){b->deferred=True;b->boundary="declaration-point deferred reference is outside closed local proof";}}
        break;}
    case NodeIntLitExpr:case NodeFloatLitExpr:case NodeStrLitExpr:case NodeCharLitExpr:case NodeBoolLitExpr:case NodeNilExpr:case NodeSizeofExpr:break;
    case NodeMemberExpr:{LV(n->as.member_expr.object);iface_local_node_t *o=iface_local_node(p,n->as.member_expr.object,False);
        if(o){r->path=o->path;
            iface_source_fact_t f=iface_source_field(p->cg,n,0);r->fact=f;r->fact_ready=True;
            if(!iface_local_struct(o->fact)||o->fact.dimensions||!f.owner||f.owner!=o->fact.contract.nominal||!f.evidence||f.evidence->kind!=NodeVarDecl)r->path.valid=False;
            else iface_local_append(p,&r->path,f.evidence,n);
        }break;}
    case NodeSelfMemberExpr:break;
    case NodeIndexExpr:case NodeFlaggedIndex:{node_t *object=n->kind==NodeIndexExpr?n->as.index_expr.object:n->as.flagged_index.object;
        node_t *index=n->kind==NodeIndexExpr?n->as.index_expr.index:n->as.flagged_index.index;LV(object);LV(index);
        iface_local_node_t *o=iface_local_node(p,object,False);if(o){r->path=o->path;
            if(o->fact.dimensions!=1||o->fact.contract.type.is_pointer)r->path.valid=False;else iface_local_append(p,&r->path,Null,n);}
        break;}
    case NodeAddrOf:LV(n->as.addr_of.operand);LE(n->as.addr_of.operand,"address of a local or descendant exposes the entire root");break;
    case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr:LV(n->as.unary.operand);
        if(n->as.unary.op==TokPlusPlus||n->as.unary.op==TokMinusMinus){iface_local_node_t *a=iface_local_node(p,n->as.unary.operand,False);if(a){r->path=a->path;r->write=True;r->compound_write=True;}}break;
    case NodeBinaryExpr:LV(n->as.binary.left);LV(n->as.binary.right);break;
    case NodeCmpChain:LV(n->as.cmp_chain.base_expr);
        if(n->as.cmp_chain.count>CMP_CHAIN_MAX)iface_local_fail(p,n,"comparison-chain child count exceeds representation");
        else for(usize_t i=0;i<n->as.cmp_chain.count;i++)LV(n->as.cmp_chain.rhs_nodes[i]);break;
    case NodeAssignExpr:{LV(n->as.assign.value);LV(n->as.assign.target); /* actual RHS-before-target order */
        iface_local_node_t *a=iface_local_node(p,n->as.assign.target,False);if(a){r->path=a->path;r->write=True;r->rhs=n->as.assign.value;
            if(a->path.root&&!a->path.valid)iface_local_expose(a,"unclassified local target write is outside closed proof");}
        iface_local_node_t *v=iface_local_node(p,n->as.assign.value,False);if(v&&v->fact.dimensions)iface_local_expose(v,"array assignment decays and exposes containing storage");break;}
    case NodeCompoundAssign:{LV(n->as.compound_assign.target);LV(n->as.compound_assign.value); /* target-before-RHS */
        iface_local_node_t *a=iface_local_node(p,n->as.compound_assign.target,False);if(a){r->path=a->path;r->write=True;r->compound_write=True;r->rhs=n->as.compound_assign.value;}break;}
    case NodeMultiAssign:{
        /* gen_multi_assign selects raw return metadata before installing all
         * target locals, then evaluates RHS expressions in that new scope. */
        node_list_t targets=n->as.multi_assign.targets,values=n->as.multi_assign.values;
        node_t *selected=Null;
        if(values.count==1){
            node_t *rhs=values.items[0];const char *callee=rhs->kind==NodeCallExpr?rhs->as.call.callee:
                rhs->kind==NodeMethodCall?rhs->as.method_call.method:rhs->kind==NodeSelfMethodCall?rhs->as.self_method_call.method:Null;
            selected=callee?find_fn_decl(p->cg,callee):Null;
        }
        if(targets.count&&(targets.items[0]->as.var_decl.flags&VdeclLet)&&values.count==1){
            for(usize_t i=0;i<targets.count;i++){
                iface_local_node_t *target=iface_local_node(p,targets.items[i],True);if(!target)continue;
                target->fact=selected&&i<selected->as.fn_decl.return_count?
                    iface_source_from_type(p->cg,selected->as.fn_decl.return_types[i],targets.items[i]->as.var_decl.storage,targets.items[i]):
                    iface_source_unknown("multi-result extraction lacks an original raw result contract");
                target->fact_ready=True;
            }
        }
        for(usize_t i=0;i<targets.count;i++){
            if(targets.items[i]->as.var_decl.init)iface_local_fail(p,targets.items[i],"multi-target initializer has no admitted lowering correspondence");
            iface_local_declaration(p,targets.items[i],False,depth+1);
        }
        for(usize_t i=0;i<targets.count;i++)for(usize_t j=0;j<i;j++){
            const char *name=targets.items[i]->as.var_decl.name;
            if(name&&strcmp(name,"_")&&targets.items[j]->as.var_decl.name&&!strcmp(name,targets.items[j]->as.var_decl.name)){
                iface_local_binding_t *actual=iface_local_binding(p,name);if(actual)actual->duplicate_multi_target=True;
            }
        }
        boolean_t extracts=values.count==1&&targets.count>1;
        usize_t evaluated=extracts?1:targets.count<values.count?targets.count:values.count;
        for(usize_t i=0;i<evaluated;i++)LV(values.items[i]);
        iface_native_t *actual=extracts?iface_source_native(p->cg,iface_source_selected(p->cg,values.items[0],0)):Null;
        node_t *result_decl=actual&&actual->decl&&actual->decl->kind==NodeFnDecl?actual->decl:Null;
        for(usize_t i=0;i<targets.count;i++){
            iface_local_node_t *target=iface_local_node(p,targets.items[i],False);if(!target||!target->path.root)continue;
            if(!extracts&&i<evaluated){target->write=True;target->rhs=values.items[i];}
            else if(extracts){target->write=True;target->rhs=values.items[0];target->extracted_write=True;target->extracted_decl=result_decl;
                target->extracted_source=result_decl&&i<result_decl->as.fn_decl.return_count?
                    iface_source_from_type(p->cg,result_decl->as.fn_decl.return_types[i],StorageDefault,result_decl):
                    iface_source_unknown("multi-result component is an unqualified extraction boundary");
            }
        }
        break;}
    case NodeCastExpr:LV(n->as.cast_expr.expr);break;
    case NodeTernaryExpr:LV(n->as.ternary.cond);LV(n->as.ternary.then_expr);LV(n->as.ternary.else_expr);break;
    case NodeCallExpr:case NodeSelfMethodCall:case NodeConstructorCall:case NodeColonCall:
    case NodeThreadCall:case NodeAsyncCall:case NodeErrPropCall:LL(iface_source_args(n));break;
    case NodeMethodCall:LV(n->as.method_call.object);LL(n->as.method_call.args);break;
    case NodeLambda:iface_local_capture(p,n->as.lambda_expr.captures,n->as.lambda_expr.capture_count,n);break;
    case NodeLambdaCall:LV(n->as.lambda_call.lambda);LL(n->as.lambda_call.args);break;
    case NodeErrProp:LV(n->as.err_prop.operand);LE(n->as.err_prop.operand,"error extraction is outside local proof");break;
    case NodeFutureOp:LV(n->as.future_op.handle);LE(n->as.future_op.handle,"future extraction is outside local proof");break;
    case NodeAwaitExpr:LV(n->as.await_expr.handle);LE(n->as.await_expr.handle,"await storage is outside local proof");iface_local_fail(p,n,"suspending body has no closed local plan");break;
    case NodeAwaitCombinator:LL(n->as.await_combinator.handles);iface_local_fail(p,n,"await combinator has no closed local plan");break;
    case NodeYieldExpr:LV(n->as.yield_expr.value);iface_local_fail(p,n,"yielding body has no closed local plan");break;
    case NodeYieldNowExpr:iface_local_fail(p,n,"yielding body has no closed local plan");break;
    case NodeNewExpr:LV(n->as.new_expr.size);break;
    case NodeNewInZone:LV(n->as.new_in_zone.size);LV(n->as.new_in_zone.zone_expr);LE(n->as.new_in_zone.zone_expr,"zone allocator takes the address of containing local storage");break;
    case NodeMovExpr:LV(n->as.mov_expr.ptr);LV(n->as.mov_expr.size);LE(n->as.mov_expr.ptr,"relocation exposes local storage");break;
    case NodeZoneMoveExpr:LV(n->as.zone_move.ptr);LE(n->as.zone_move.ptr,"zone relocation exposes local storage");break;
    case NodeZoneDecl:{iface_local_binding_t *env=p->environment;iface_local_bind(p,n,n->as.zone_decl.name,iface_source_unknown("zone handle boundary"),False);LV(n->as.zone_decl.body);p->environment=env;break;}
    case NodeZoneStmt:iface_local_bind(p,n,n->as.zone_stmt.name,iface_source_unknown("zone handle boundary"),False);break;
    case NodeZoneFreeStmt:break;
    case NodeSliceExpr:LV(n->as.slice_expr.object);LV(n->as.slice_expr.lo);LV(n->as.slice_expr.hi);LE(n->as.slice_expr.object,"slice view exposes containing local storage");break;
    case NodeMakeExpr:LV(n->as.make_expr.len);LV(n->as.make_expr.cap);LV(n->as.make_expr.init);break;
    case NodeAppendExpr:LV(n->as.append_expr.slice);LV(n->as.append_expr.val);LE(n->as.append_expr.slice,"opaque append writer exposes storage");LE(n->as.append_expr.val,"opaque append packing exposes storage");break;
    case NodeCopyExpr:LV(n->as.copy_expr.dst);LV(n->as.copy_expr.src);LE(n->as.copy_expr.dst,"opaque buffer copy exposes storage");LE(n->as.copy_expr.src,"opaque buffer copy exposes storage");break;
    case NodeLenExpr:case NodeCapExpr:LV(n->as.len_expr.operand);break;
    case NodeUnsafeBlock:LV(n->as.unsafe_block.body);break;
    case NodeHashExpr:LV(n->as.hash_expr.operand);LE(n->as.hash_expr.operand,"hash helper effects are outside closed local proof");break;
    case NodeEquExpr:LV(n->as.equ_expr.left);LV(n->as.equ_expr.right);LE(n->as.equ_expr.left,"equality helper effects are outside closed local proof");LE(n->as.equ_expr.right,"equality helper effects are outside closed local proof");break;
    case NodeAnyTypeExpr:LV(n->as.any_type_expr.operand);LE(n->as.any_type_expr.operand,"any carrier is outside closed local proof");break;
    case NodeExpectExpr:LV(n->as.expect_expr.expr);break;
    case NodeExpectEqExpr:LV(n->as.expect_eq.left);LV(n->as.expect_eq.right);break;
    case NodeExpectNeqExpr:LV(n->as.expect_neq.left);LV(n->as.expect_neq.right);break;
    case NodeTestFailExpr:LV(n->as.test_fail.message);break;
    case NodeWatchStmt:iface_local_capture(p,n->as.watch_stmt.captures,n->as.watch_stmt.capture_count,n);break;
    case NodeSendStmt:LV(n->as.send_stmt.value);LE(n->as.send_stmt.value,"signal packing is outside closed local proof");break;
    case NodeQuitStmt:LV(n->as.quit_stmt.code);break;
    case NodeVaOp:iface_local_va(p,n,depth+1);break;
    default:iface_local_fail(p,n,"unclassified AST node in local inventory");break;
    }
    if(!r->fact_ready){
        if(n->kind==NodeDesigInit)r->fact=iface_source_from_type(p->cg,(type_info_t){.base=TypeUser,.user_name=n->as.desig_init.type_name,.generic_use=n->generic_use},StorageDefault,n);
        else r->fact=iface_source_expr_at(p->cg,n,NO_TYPE,0);
        r->fact_ready=True;
    }
    switch(n->kind){
    case NodeCallExpr:case NodeMethodCall:case NodeSelfMethodCall:case NodeConstructorCall:case NodeColonCall:
    case NodeThreadCall:case NodeAsyncCall:case NodeErrPropCall:case NodeLambdaCall:iface_local_call(p,r);break;
    default:break;
    }
    /* Format lowering is not a trusted aggregate/array receiver contract. */
    node_list_t format={0};
    if(n->kind==NodePrintStmt)format=n->as.print_stmt.args;
    if(n->kind==NodeComptimeFmt)format=n->as.comptime_fmt.args;
    if(n->kind==NodeErrorExpr)format=n->as.error_expr.args;
    for(usize_t i=0;i<format.count;i++){iface_local_node_t *a=iface_local_node(p,format.items[i],False);
        if(a&&(a->fact.dimensions||iface_local_struct(a->fact)))iface_local_expose(a,"aggregate formatting route is outside closed local proof");}
p->parent=saved_parent;
#undef LV
#undef LL
#undef LE
}
static boolean_t iface_local_same_path(iface_local_path_t a,iface_local_path_t b) {
    return a.root==b.root&&a.count==b.count&&!memcmp(a.steps,b.steps,a.count*sizeof(*a.steps));
}
static boolean_t iface_local_prefix(iface_local_path_t a,iface_local_path_t b) {
    return a.root==b.root&&a.count<=b.count&&!memcmp(a.steps,b.steps,a.count*sizeof(*a.steps));
}
static void iface_local_raise(iface_local_proof_t *q,iface_local_state_t state,node_t *evidence,const char *reason) {
    if(state>q->intrinsic){q->intrinsic=state;q->evidence=evidence;q->reason=reason;}
}
static iface_source_fact_t iface_local_path_type(iface_local_plan_t *p,iface_local_path_t path,unsigned count,uint64_t *extent) {
    iface_source_fact_t f=path.root?path.root->fact:iface_source_unknown("path lacks lexical root");
    *extent=path.root?path.root->extent:0;
    if(!path.valid||count>path.count)return iface_source_unknown("unsupported local storage path");
    for(unsigned k=0;k<count;k++){
        node_t *d=path.steps[k];
        if(!d){if(f.dimensions!=1)return iface_source_unknown("path is not a flat fixed array");f.dimensions=0;*extent=0;continue;}
        if(!iface_local_struct(f)||f.dimensions)return iface_source_unknown("path owner is not an ordinary value struct");
        node_t *owner=f.contract.nominal;boolean_t found=False;
        for(usize_t i=0;i<owner->as.type_decl.fields.count;i++)if(owner->as.type_decl.fields.items[i]==d)found=True;
        if(!found||(d->as.var_decl.flags&VdeclComptimeField))return iface_source_unknown("selected field does not belong to runtime owner");
        f=iface_source_declaration(p->cg,d);f.owner=owner;
        *extent=f.dimensions==1&&!d->as.var_decl.array_size_names[0]&&d->as.var_decl.array_sizes[0]>0&&
            d->as.var_decl.array_sizes[0]<=IfaceLocalMaxCoverage?(uint64_t)d->as.var_decl.array_sizes[0]:0;
    }
    return f;
}
static boolean_t iface_local_layout(cg_t *cg,iface_source_fact_t f,uint64_t extent,LLVMTypeRef actual) {
    if(!iface_local_struct(f))return False;
    struct_reg_t *sr=find_struct(cg,f.contract.nominal->as.type_decl.name);
    if(!sr||sr->layout_decl!=f.contract.nominal||sr->layout_state!=2||sr->is_union)return False;
    if(f.dimensions){
        if(f.dimensions!=1||!extent)return False;
        if(actual){if(LLVMGetTypeKind(actual)!=LLVMArrayTypeKind||LLVMGetArrayLength2(actual)!=extent)return False;actual=LLVMGetElementType(actual);}
    }
    return !actual||actual==sr->llvm_type;
}
/* Cleanup of any by-value descendant acts on the root's storage. Pointer
 * pointees are never followed, but owned heap fields have implicit release. */
static boolean_t iface_local_cleanup(cg_t *cg,iface_source_fact_t f,unsigned depth) {
    if(depth>IfaceMaxMetadataDepth)return True;
    if(!iface_local_struct(f))return False;
    struct_reg_t *sr=find_struct(cg,f.contract.nominal->as.type_decl.name);
    if(!sr||sr->layout_decl!=f.contract.nominal||sr->layout_state!=2||sr->destructor)return True;
    node_list_t fields=f.contract.nominal->as.type_decl.fields;
    for(usize_t i=0;i<fields.count;i++){
        node_t *d=fields.items[i];if(d->as.var_decl.flags&VdeclComptimeField)continue;
        iface_source_fact_t child=iface_source_declaration(cg,d);
        if(child.contract.type.is_pointer){if(d->as.var_decl.storage==StorageHeap)return True;continue;}
        if(iface_local_cleanup(cg,child,depth+1))return True;
    }
    return False;
}
static iface_local_state_t iface_local_frontier(iface_local_binding_t *b,iface_local_node_t *read) {
    if(!b->frontier)return LocalClosed;
    if(b->frontier_count!=1||!b->frontier->terminal||!read)return LocalUnsupported;
    if(read==b->frontier)return LocalClosed;
    /* No expression-local ordering assumptions. A copy made in a preceding
     * direct owning-block statement has independent physical value storage. */
    if(read->block!=b->block||read->loops||read->control||read->deferred||read->inline_carrier||
        read->ordinal>=b->frontier->ordinal)return LocalUnsupported;
    return LocalClosed;
}
static iface_local_proof_t *iface_local_demand(iface_local_plan_t *,iface_local_path_t,unsigned);
static void iface_local_edge(iface_local_plan_t *p,iface_local_proof_t *q,iface_local_proof_t *source,
                               node_t *read,boolean_t initializer) {
    if(!source){iface_local_raise(q,LocalUnsupported,read,"source path has no complete local proof");return;}
    if(p->edge_count>=IfaceLocalMaxEdges){iface_local_fail(p,read,"local transfer dependency budget exceeded");return;}
    iface_local_edge_t *e=iface_local_alloc(p,sizeof(*e));if(!e)return;
    e->source=source;e->read=read;e->initializer=initializer;
    e->gate=iface_local_frontier(source->path.root,iface_local_node(p,read,False));
    e->next=q->edges;q->edges=e;++p->edge_count;
    if(e->gate!=LocalClosed)iface_local_raise(q,LocalUnsupported,read,"concrete source read occurs outside its exact before-call frontier");
}
/* Each constructor keeps an original-AST expected destination mapping. The
 * lowering hook checks the actual LLVM storage and synthetic named wrappers. */
static void iface_local_check(iface_local_plan_t *p,iface_local_proof_t *q,iface_local_node_t *r,unsigned kind) {
    if(!q->upgrade||!r)return;
    for(iface_local_check_t *c=q->checks;c;c=c->next)if(c->node==r){c->kind|=kind;return;}
    if(p->check_count>=FactoryMaxChecks){iface_local_fail(p,r->ast,"factory correspondence check budget exhausted");return;}
    iface_local_check_t *c=iface_local_alloc(p,sizeof(*c));c->node=r;c->kind=kind;c->next=q->checks;q->checks=c;++p->check_count;
}
static void iface_local_ctor_plan(iface_local_plan_t *p,iface_local_proof_t *q,node_t *n,
                                    iface_source_fact_t target,uint64_t extent) {
    iface_local_node_t *r=iface_local_node(p,n,False);
    if(!r){iface_local_raise(q,LocalUnsupported,n,"constructor is not an original inventoried AST node");return;}
    if(r->constructor_needed&&(r->constructor_target.contract.nominal!=target.contract.nominal||
        r->constructor_target.dimensions!=target.dimensions||r->constructor_extent!=extent)){
        iface_local_raise(q,LocalUnsupported,n,"constructor has conflicting planned destination identities");return;}
    iface_local_check(p,q,r,FactoryCheckConstructor);
    r->constructor_needed=True;r->constructor_target=target;r->constructor_extent=extent;
    if(!iface_local_layout(p->cg,target,extent,Null))iface_local_raise(q,LocalUnsupported,n,"constructor source owner lacks completed exact layout correspondence");
}
static void iface_local_value(iface_local_plan_t *p,iface_local_proof_t *q,node_t *n,
                                iface_source_fact_t storage,uint64_t extent,node_t **steps,unsigned count,
                                boolean_t initializer,unsigned depth) {
    if(depth>IfaceMaxMetadataDepth){iface_local_fail(p,n,"local constructor/copy metadata nesting exceeds 256");return;}
    if(!n){iface_local_raise(q,LocalUnsupported,q->path.root->decl,"selected concrete storage lacks an explicit initializer witness");return;}
    if(!iface_local_struct(storage)){iface_local_raise(q,LocalUnsupported,n,"selected path lacks an ordinary concrete storage contract");return;}
    iface_source_fact_t source=iface_local_query(p,n);
    boolean_t constructor=n->kind==NodeCompoundInit||n->kind==NodeDesigInit;
    if(n->kind==NodeDesigInit){
        iface_source_fact_t expected=storage;expected.dimensions=0;
        if(storage.dimensions||iface_source_local_copy(source,expected)!=IfaceLocalCopyExact){
            iface_local_raise(q,LocalMismatch,n,"named constructor identity does not match selected concrete destination");
            /* Still inspect original selected children when owner is known. */
        }
    }
    if(constructor){
        iface_local_ctor_plan(p,q,n,storage,extent);
        if(storage.dimensions){
            if(storage.dimensions!=1||!extent||extent>IfaceLocalMaxCoverage||n->kind!=NodeCompoundInit||!count||steps[0]){
                iface_local_raise(q,LocalUnsupported,n,"concrete array requires a bounded flat constructor and exact extent");return;}
            unsigned char *covered=iface_local_zero((size_t)extent);if(!covered){iface_local_fail(p,n,"array coverage allocation failed");return;}
            uint64_t cursor=0;iface_source_fact_t element=storage;element.dimensions=0;
            for(usize_t i=0;i<n->as.compound_init.items.count;i++){
                node_t *item=n->as.compound_init.items.items[i],*value=item;
                if(item->kind==NodeInitIndex){
                    if(!item->as.init_index.index||item->as.init_index.index->kind!=NodeIntLitExpr){
                        iface_local_raise(q,LocalUnsupported,item,"concrete array designator requires an original nonnegative integer literal");
                        iface_local_value(p,q,item->as.init_index.value,element,0,steps+1,count-1,initializer,depth+1);continue;}
                    cursor=item->as.init_index.index->as.int_lit.value;value=item->as.init_index.value;
                }else if(item->kind==NodeSpreadExpr||item->kind==NodeRangeExpr||item->kind==NodeInitField){
                    iface_local_raise(q,LocalUnsupported,item,"spread, range or field item is outside concrete-array construction proof");continue;
                }
                if(cursor>=extent){iface_local_raise(q,LocalMismatch,item,"concrete-array constructor element is outside its exact extent");}
                else covered[cursor]=1;
                iface_local_value(p,q,value,element,0,steps+1,count-1,initializer,depth+1);
                if(cursor==UINT64_MAX)iface_local_raise(q,LocalMismatch,item,"concrete-array constructor cursor overflows");else ++cursor;
            }
            for(uint64_t i=0;i<extent;i++)if(!covered[i]){iface_local_raise(q,LocalUnsupported,n,"every selected concrete-array element needs an explicit checked initializer");break;}
            iface_local_release(covered);return;
        }
        if(!count)return; /* zero construction creates this exact nominal object only */
        node_t *selected=steps[0],*owner=storage.contract.nominal;
        if(!selected){iface_local_raise(q,LocalUnsupported,n,"array selection disagrees with constructor storage");return;}
        node_t *runtime[IfaceLocalMaxPath*32];usize_t runtime_count=0;
        for(usize_t i=0;i<owner->as.type_decl.fields.count;i++){
            node_t *d=owner->as.type_decl.fields.items[i];if(d->as.var_decl.flags&VdeclComptimeField)continue;
            if(runtime_count==sizeof(runtime)/sizeof(runtime[0])){iface_local_fail(p,n,"constructor field count exceeds bounded layout inventory");return;}
            runtime[runtime_count++]=d;
        }
        iface_source_fact_t child=iface_source_declaration(p->cg,selected);child.owner=owner;
        uint64_t child_extent=child.dimensions==1&&!selected->as.var_decl.array_size_names[0]&&selected->as.var_decl.array_sizes[0]>0?
            (uint64_t)selected->as.var_decl.array_sizes[0]:0;
        boolean_t present=False;usize_t cursor=0;
        node_list_t items=n->kind==NodeCompoundInit?n->as.compound_init.items:n->as.desig_init.values;
        for(usize_t i=0;i<items.count;i++){
            node_t *item=items.items[i],*value=item,*field=Null;const char *name=Null;
            if(n->kind==NodeDesigInit){
                if(i>=n->as.desig_init.fields.count){iface_local_raise(q,LocalUnsupported,n,"named constructor field ordinal is missing");continue;}
                node_t *label=n->as.desig_init.fields.items[i];
                if(!label||label->kind!=NodeIdentExpr){iface_local_raise(q,LocalUnsupported,n,"named constructor label is not an original identifier");continue;}
                name=label->as.ident.name;
            }else if(item->kind==NodeInitField){name=item->as.init_field.name;value=item->as.init_field.value;}
            else if(item->kind==NodeSpreadExpr){
                iface_local_raise(q,LocalUnsupported,item,"whole-object spread is outside closed local construction proof");
                iface_local_value(p,q,item->as.spread_expr.expr,storage,extent,steps,count,initializer,depth+1);cursor=runtime_count;continue;
            }else if(cursor<runtime_count)field=runtime[cursor++];
            else{iface_local_raise(q,LocalMismatch,item,"constructor item exceeds the original runtime field layout");continue;}
            if(name)for(usize_t j=0;j<runtime_count;j++)if(!strcmp(runtime[j]->as.var_decl.name,name))field=runtime[j];
            if(!field){iface_local_raise(q,LocalUnsupported,item,"constructor field has no exact runtime declaration");continue;}
            if(field==selected){present=True;iface_local_value(p,q,value,child,child_extent,steps+1,count-1,initializer,depth+1);}
        }
        if(!present)iface_local_raise(q,LocalUnsupported,n,"selected nominal child lacks an explicit checked initializer");
        return;
    }
    if(n->kind==NodeTernaryExpr){
        iface_local_value(p,q,n->as.ternary.then_expr,storage,extent,steps,count,initializer,depth+1);
        iface_local_value(p,q,n->as.ternary.else_expr,storage,extent,steps,count,initializer,depth+1);return;
    }
    if(n->kind==NodeCastExpr){iface_source_fact_t cast=iface_source_from_type(p->cg,n->as.cast_expr.target,StorageDefault,n);
        iface_source_fact_t value=iface_local_query(p,n->as.cast_expr.expr),target=storage;target.dimensions=0;
        if(storage.dimensions||iface_source_local_copy(cast,target)!=IfaceLocalCopyExact||iface_source_local_copy(value,target)!=IfaceLocalCopyExact)
            iface_local_raise(q,LocalMismatch,n,"cast cannot manufacture exact concrete local source identity");
        iface_local_value(p,q,n->as.cast_expr.expr,storage,extent,steps,count,initializer,depth+1);return;
    }
    if(n->kind==NodeAssignExpr){
        iface_source_fact_t written=iface_local_query(p,n->as.assign.target);
        iface_source_fact_t expected=storage;expected.dimensions=0;
        if(storage.dimensions||iface_source_local_copy(written,expected)!=IfaceLocalCopyExact)
            iface_local_raise(q,LocalMismatch,n,"assignment result destination differs from its concrete value contract");
        if(!n->as.assign.target||n->as.assign.target->kind!=NodeIdentExpr)iface_local_raise(q,LocalUnsupported,n,"only a plain identifier assignment result has local copy proof");
        iface_local_value(p,q,n->as.assign.value,storage,extent,steps,count,initializer,depth+1);return;
    }
    iface_source_fact_t target=storage;
    if(target.dimensions){iface_local_raise(q,LocalUnsupported,n,"whole-array value copies are outside closed local proof");return;}
    iface_local_copy_t copy=iface_source_local_copy(source,target);
    if(copy==IfaceLocalCopyMismatch){iface_local_raise(q,LocalMismatch,n,"concrete source identity does not match the original local destination");return;}
    if(copy!=IfaceLocalCopyExact){iface_local_raise(q,LocalUnsupported,n,"concrete local transfer lacks an independent source contract");return;}
    iface_local_node_t *r=iface_local_node(p,n,False);
    if(r){
        iface_local_check(p,q,r,FactoryCheckExpression|FactoryCheckResult);
        if(r->transfer_needed&&r->transfer_target.contract.nominal!=target.contract.nominal)
            iface_local_raise(q,LocalUnsupported,n,"physical source read has conflicting planned destination contracts");
        r->transfer_needed=True;r->transfer_target=target;
    }
    if(r&&r->path.valid&&r->path.root&&r->path.root->local){
        iface_local_path_t path=r->path;
        for(unsigned i=0;i<count;i++)if(!iface_local_append(p,&path,steps[i],n))break;
        iface_local_proof_t *dependency=iface_local_demand(p,path,depth+1);
        iface_local_edge(p,q,dependency,n,initializer);return;
    }
    if(n->kind==NodeCallExpr){q->result_boundary=True;
        if(p->overlay_building&&!p->factory_building&&iface_factory_value(p,q,n,storage,steps,count))return;
    }
    iface_local_raise(q,LocalDeclared,n,"ordinary parameter/result/global or extracted value is a declared boundary, not a closed local producer");
}
static iface_local_proof_t *iface_local_demand(iface_local_plan_t *p,iface_local_path_t path,unsigned depth) {
    if(!path.root||!path.valid)return Null;
    for(iface_local_proof_t *q=p->proofs;q;q=q->next)if(q->upgrade==p->overlay_building&&iface_local_same_path(q->path,path))return q;
    if(depth>IfaceMaxMetadataDepth||p->proof_count>=IfaceLocalMaxProofs){iface_local_fail(p,path.root->decl,"selected local proof graph exceeds its bounded depth/count");return Null;}
    iface_local_proof_t *q=iface_local_alloc(p,sizeof(*q));if(!q)return Null;
    q->path=path;q->upgrade=p->overlay_building;q->projected=path.count&&!path.arrays;q->next=p->proofs;p->proofs=q;++p->proof_count;q->built=True;
    iface_local_binding_t *b=path.root;b->demanded=True;uint64_t selected_extent=0;
    if(b->duplicate_multi_target)iface_local_fail(p,b->decl,"duplicate multi-target bindings have no admitted concrete store correspondence");
    q->target=iface_local_path_type(p,path,path.count,&selected_extent);
    if(!iface_local_struct(q->target)||q->target.dimensions){iface_local_raise(q,LocalUnsupported,b->decl,"selected path is not an ordinary concrete value");}
    if(!b->local||!b->decl||b->decl->kind!=NodeVarDecl){iface_local_raise(q,LocalDeclared,b->decl,"declaration boundary cannot seed a closed local path");return q;}
    if(b->no_new_proof||b->exposed||b->deferred||b->frontier_count>1||iface_local_cleanup(p->cg,b->fact,0))
        iface_local_raise(q,LocalUnsupported,b->decl,b->boundary?b->boundary:"local storage or repeated receiver effects are outside closed proof");
    /* Complete the initializer and all overlapping writers despite a boundary.
     * Known mismatches dominate exposure, helpers, unsupported syntax and SCCs. */
    iface_local_value(p,q,b->decl->as.var_decl.init,b->fact,b->extent,path.steps,path.count,True,depth+1);
    for(iface_local_node_t *w=p->nodes;w;w=w->next){
        if(!w->write||!w->path.valid||w->path.root!=b||!iface_local_prefix(w->path,path))continue;
        uint64_t extent=0;iface_source_fact_t storage=iface_local_path_type(p,w->path,w->path.count,&extent);
        w->checked_write=True;iface_local_check(p,q,w,FactoryCheckExpression);
        if(w->extracted_write){
            if(w->extracted_source.status==IfaceSourceUnknown)
                iface_local_fail(p,w->rhs,"affected multi-result component lacks complete selected callable/source correspondence");
            if(iface_source_local_copy(w->extracted_source,storage)==IfaceLocalCopyMismatch)
                iface_local_raise(q,LocalMismatch,w->rhs,"extracted result identity does not match the original local destination");
            else iface_local_raise(q,LocalDeclared,w->rhs,"multi-result extraction remains an original declared boundary");
            continue;
        }
        if(w->compound_write){iface_local_raise(q,LocalUnsupported,w->ast,"aggregate read-modify-write has no exact concrete transfer");continue;}
        iface_local_value(p,q,w->rhs,storage,extent,path.steps+w->path.count,path.count-w->path.count,False,depth+1);
    }
    return q;
}
static iface_local_state_t iface_local_seed(iface_local_proof_t *q,unsigned depth) {
    if(q->seed_visit==2)return q->seed;
    if(q->seed_visit==1||depth>IfaceMaxMetadataDepth)return LocalUnsupported;
    q->seed_visit=1;q->seed=q->intrinsic;
    for(iface_local_edge_t *e=q->edges;e;e=e->next)if(e->initializer){
        iface_local_state_t s=iface_local_seed(e->source,depth+1);if(s>q->seed){q->seed=s;q->evidence=e->source->evidence;q->reason=e->source->reason;}if(e->gate>q->seed)q->seed=e->gate;}
    q->seed_visit=2;return q->seed;
}
static void iface_local_solve(iface_local_plan_t *p) {
    /* Array values lower as storage addresses. Classify each original occurrence
     * by its actual parent route, including pointer declaration and constructor
     * fields, without needing to guess its eventual destination from an LLVM type. */
    for(iface_local_node_t *r=p->nodes;r;r=r->next)if(r->path.root&&r->fact.dimensions){
        node_t *a=r->ast,*parent=r->parent;
        boolean_t internal=parent&&((parent->kind==NodeIndexExpr&&parent->as.index_expr.object==a)||
            (parent->kind==NodeFlaggedIndex&&parent->as.flagged_index.object==a)||
            ((parent->kind==NodeLenExpr||parent->kind==NodeCapExpr)&&parent->as.len_expr.operand==a));
        if(!internal&&a->kind!=NodeVarDecl)iface_local_expose(r,"array-valued occurrence exposes containing storage outside an internal element/extent access");
    }
    for(iface_local_node_t *r=p->nodes;r;r=r->next)if(!p->overlay_building&&r->sink&&r->path.root&&r->path.valid&&iface_local_struct(r->path.root->fact))
        r->proof=iface_local_demand(p,r->path,0);
    for(iface_local_proof_t *q=p->proofs;q;q=q->next)q->seed_visit=0;
    for(iface_local_proof_t *q=p->proofs;q;q=q->next)q->state=iface_local_seed(q,0);
    /* Monotone propagation admits reassignment SCCs only with independently
     * initialized members. No code-generation visit can improve an outcome. */
    boolean_t changed=True;usize_t rounds=0;
    while(changed&&rounds++<=p->proof_count){changed=False;
        for(iface_local_proof_t *q=p->proofs;q;q=q->next)for(iface_local_edge_t *e=q->edges;e;e=e->next){
            if(e->source->projected&&!q->projected){q->projected=True;changed=True;}
            if(e->source->result_boundary&&!q->result_boundary){q->result_boundary=True;changed=True;}
            iface_local_state_t s=e->source->state;if(e->gate>s)s=e->gate;
            if(s>q->state){q->state=s;changed=True;if(e->source->state==s){q->evidence=e->source->evidence;q->reason=e->source->reason;}}
        }
    }
    if(changed)iface_local_fail(p,p->body,"local dependency solver did not converge within graph bound");
}
/* Internal Gate A: inventory/mismatches/correspondence first. Gate B is enabled
 * only after the immutable inventory and exact-transfer tests are frozen. */
#define IFACE_LOCAL_PROJECTION_GATE 1
static void iface_local_diagnose(iface_local_plan_t *p) {
    if(p->incomplete&&p->legacy_proof_count){iface_local_error_at(p,p->failure?p->failure:p->owner,p->reason?p->reason:"incomplete local proof inventory");p->rejected=True;}
    for(iface_local_proof_t *q=p->proofs;q;q=q->next)if(!q->upgrade&&q->state==LocalMismatch){
        boolean_t duplicate=False;
        for(iface_local_proof_t *before=p->proofs;before!=q;before=before->next)
            if(!before->upgrade&&before->state==LocalMismatch&&before->evidence==q->evidence)duplicate=True;
        if(!duplicate)iface_local_error_at(p,q->evidence?q->evidence:q->path.root->decl,q->reason?q->reason:"concrete local source identity mismatch");
        p->rejected=True;
    }
}
static iface_local_plan_t *iface_local_create(cg_t *cg,node_t *owner,node_t *body,LLVMValueRef context,boolean_t original_formals) {
    iface_local_plan_t *p=iface_local_zero(sizeof(*p));if(!p)return Null;
    p->cg=cg;p->context=context;p->owner=owner;p->body=body;p->building=True;
    p->frame=(iface_source_frame_t){.plan=p,.owner=owner,.body=body,.context=context,
        .struct_name=owner->kind==NodeFnDecl&&owner->as.fn_decl.is_method?owner->as.fn_decl.struct_name:Null};
    mangle_module_prefix(owner->module_name?owner->module_name:"",p->frame.module,sizeof(p->frame.module));
    if(!original_formals){
        snprintf(p->frame.module,sizeof(p->frame.module),"%s",cg->current_module_prefix);
        p->frame.struct_name=cg->current_struct_name;
    }
    p->buckets=iface_local_zero(IfaceLocalBuckets*sizeof(*p->buckets));
    iface_source_frame_t *saved=cg->iface_query_frame;cg->iface_query_frame=&p->frame;
    if(!p->buckets)iface_local_fail(p,owner,"cannot allocate local AST identity index");
    if(original_formals){
        for(usize_t i=0;i<owner->as.fn_decl.params.count;i++){
            node_t *d=owner->as.fn_decl.params.items[i];
            iface_local_bind(p,d,d->as.var_decl.name,iface_source_declaration(cg,d),False);
        }
    }else for(usize_t i=0;i<cg->locals.count;i++){
        symbol_t *s=&cg->locals.entries[i];iface_source_fact_t f=iface_source_symbol(cg,s);
        iface_local_binding_t *b=iface_local_bind(p,s->iface_source?s->iface_source->decl:owner,s->name,f,False);
        if(b){b->symbol=*s;b->symbol.iface_source=&b->source;}
    }
    iface_local_visit(p,body,0);iface_local_solve(p);p->legacy_proof_count=p->proof_count;p->building=False;p->frozen=True;
    cg->iface_query_frame=saved;
    for(iface_local_node_t *r=p->nodes;r;r=r->next)
        r->legacy_required=r->constructor_needed||r->transfer_needed||r->checked_write||r->sink;
    for(iface_local_binding_t *b=p->bindings;b;b=b->next)b->legacy_demanded=b->demanded;
    return p;
}
static iface_local_plan_t *iface_local_begin(cg_t *cg,node_t *owner,node_t *body) {
    iface_local_plan_t *previous=cg->iface_local_plan;cg->iface_local_plan=Null;
    if(!cg->iface_count||!body||(owner->kind==NodeFnDecl&&owner->as.fn_decl.is_async)||cg->generic_n)return previous;
    iface_local_plan_t *p=Null;
    for(iface_local_plan_t *r=cg->iface_factories?cg->iface_factories->plans:Null;r;r=r->retained_next)
        if(r->owner==owner&&r->body==body&&r->context==cg->current_fn){p=r;break;}
    if(!p)p=iface_local_create(cg,owner,body,cg->current_fn,False);
    if(!p){iface_local_error(owner,"cannot allocate bounded local inventory");return previous;}
    p->previous=previous;p->emitting=True;cg->iface_local_plan=p;iface_local_diagnose(p);return previous;
}
static void iface_local_destroy(iface_local_plan_t *p) {
    if(!p)return;
    while(p->proofs){iface_local_proof_t *q=p->proofs;p->proofs=q->next;
        while(q->edges){iface_local_edge_t *e=q->edges;q->edges=e->next;iface_local_release(e);}
        while(q->checks){iface_local_check_t *c=q->checks;q->checks=c->next;iface_local_release(c);}
        iface_local_release(q);}
    while(p->nodes){iface_local_node_t *r=p->nodes;p->nodes=r->next;iface_local_release(r);}
    while(p->bindings){iface_local_binding_t *b=p->bindings;p->bindings=b->next;iface_local_release(b);}
    iface_local_release(p->buckets);iface_local_release(p);
}
static void iface_local_end(cg_t *cg,iface_local_plan_t *previous) {
    iface_local_plan_t *p=cg->iface_local_plan;
    if(p&&p!=previous){p->emitting=False;p->previous=Null;if(!p->retained)iface_local_destroy(p);}
    cg->iface_local_plan=previous;
}
static boolean_t iface_local_lowering(cg_t *cg,node_t *n) {
    iface_local_plan_t *p=iface_local_current(cg);
    if(!p)return True;
    if(!p->frozen||p->building)return iface_local_error(n,"local plan is not immutable before lowering");
    if(p->rejected)return False;
    return True;
}
static int iface_local_eligibility(cg_t *cg,node_t *use,symbol_t *symbol) {
    iface_local_plan_t *p=iface_local_current(cg);if(!p)return 0;
    if(!p->frozen||p->building){iface_local_error(use,"local use has no frozen exact-body plan");return -1;}
    if(p->rejected)return -1;
    if(p->incomplete){iface_local_error(use,p->reason?p->reason:"incomplete affected local inventory");return -1;}
    iface_local_node_t *r=iface_local_node(p,use,False);
    iface_local_binding_t *b=r?r->path.root:Null;
    if(!r||!r->sink||!b||!b->local)return 0;
    if(!r->path.valid||b->fact.status==IfaceSourceExcluded)return 0; /* intentional pointer/generic boundary: origin exclusions remain */
    if(!iface_local_struct(b->fact)){
        iface_local_error_at(p,use,"local binding has no complete independent preflight source correspondence");return -1;
    }
    if(!symbol->iface_source||symbol->iface_source->decl!=b->decl||symbol->iface_source->context!=p->context){
        iface_local_error(use,"emitted local binding differs from the original planned declaration");return -1;}
    if(p->incomplete||!r->proof){iface_local_error(use,"affected local use lacks a complete inventoried path proof");return -1;}
    if(r->proof->state==LocalMismatch){iface_local_error(use,r->proof->reason);return -1;}
    if(IFACE_FACTORY_RESULT_PROMOTION&&cg->iface_factory_exhausted&&r->proof->state!=LocalClosed&&
        r->proof->projected&&r->proof->result_boundary&&(symbol->iface_excluded_origin&IfaceOriginUnresolved)){
        iface_local_error_at(p,use,"factory result preparation budget exhausted");return -1;
    }
    if(r->proof->state!=LocalClosed&&r->upgrade){
        int admission=iface_factory_consume(cg,p,r,symbol);if(admission)return admission;
    }
    if(r->proof->state!=LocalClosed||iface_local_frontier(b,r)!=LocalClosed){
        if(symbol->iface_excluded_origin&IfaceOriginUnresolved){
            iface_local_error(use,r->proof->reason?r->proof->reason:"projected local source is outside its exact closed-use frontier");return -1;}
        return 0; /* existing declared-contract route, explicitly not new proof */
    }
    if(!iface_local_layout(cg,r->proof->target,0,symbol->type)){
        iface_local_error(use,"emitted concrete value differs from the exact source/layout plan");return -1;}
    return IFACE_LOCAL_PROJECTION_GATE?1:0;
}
static boolean_t iface_local_receipt_error(iface_local_plan_t *p,node_t *n,const char *why,boolean_t legacy) {
    iface_local_node_t *r=iface_local_node(p,n,False);if(r&&iface_factory_current(p->cg)==p&&!r->physical_failure)r->physical_failure=why;
    return legacy?iface_local_error_at(p,n,why):True;
}
static boolean_t iface_local_constructor(cg_t *cg,node_t *original,node_t *lowered,LLVMTypeRef actual) {
    iface_local_plan_t *p=iface_local_current(cg);if(!p)return True;
    if(!iface_local_lowering(cg,original))return False;
    iface_local_node_t *r=iface_local_node(p,original,False);
    if(!r||!r->constructor_needed)return True;
    boolean_t active=iface_factory_current(cg)==p;
    if(p->retained&&!r->legacy_required&&!active)return True;
    if(!iface_local_layout(cg,r->constructor_target,r->constructor_extent,actual))
        return iface_local_receipt_error(p,original,"constructor lowering differs from original owner or exact array extent",r->legacy_required);
    if(original!=lowered){
        if(original->kind!=NodeDesigInit||lowered->kind!=NodeCompoundInit||
            original->as.desig_init.values.count!=lowered->as.compound_init.items.count)
            return iface_local_receipt_error(p,original,"synthetic constructor lacks original child correspondence",r->legacy_required);
        for(usize_t i=0;i<original->as.desig_init.values.count;i++){
            node_t *item=lowered->as.compound_init.items.items[i],*label=original->as.desig_init.fields.items[i];
            if(!item||item->kind!=NodeInitField||item->as.init_field.value!=original->as.desig_init.values.items[i]||
                !label||label->kind!=NodeIdentExpr||strcmp(label->as.ident.name,item->as.init_field.name))
                return iface_local_receipt_error(p,original,"synthetic constructor child mapping differs from preflight",r->legacy_required);
        }
    }else if(original->kind!=NodeCompoundInit)return iface_local_receipt_error(p,original,"constructor lost its original AST lowering identity",r->legacy_required);
    if(active)r->constructor_seen=True;return True;
}
/* Cross-check physical local storage against the immutable original binding and
 * each original owner/field/layout step. No scalar fallback or lazy instantiation. */
static LLVMTypeRef iface_local_storage(iface_local_plan_t *p,iface_local_path_t path) {
    iface_local_binding_t *b=path.root;if(!b||!path.valid)return Null;
    symbol_t *s=iface_source_lookup(p->cg,b->symbol.name);
    if(!s||!s->iface_source||s->iface_source->decl!=b->decl||s->iface_source->context!=p->context)return Null;
    LLVMTypeRef actual=s->type;iface_source_fact_t f=b->fact;uint64_t extent=b->extent;
    for(unsigned k=0;k<path.count;k++){
        node_t *field=path.steps[k];
        if(!field){
            if(f.dimensions!=1||!extent||LLVMGetTypeKind(actual)!=LLVMArrayTypeKind||LLVMGetArrayLength2(actual)!=extent)return Null;
            actual=LLVMGetElementType(actual);f.dimensions=0;extent=0;continue;
        }
        if(!iface_local_layout(p->cg,f,extent,actual)||f.dimensions)return Null;
        struct_reg_t *sr=find_struct(p->cg,f.contract.nominal->as.type_decl.name);field_info_t *selected=Null;
        usize_t ordinal=0;boolean_t owns=False;
        for(usize_t i=0;i<f.contract.nominal->as.type_decl.fields.count;i++){
            node_t *d=f.contract.nominal->as.type_decl.fields.items[i];if(d->as.var_decl.flags&VdeclComptimeField)continue;
            if(d==field){owns=True;break;}++ordinal;
        }
        if(!owns||ordinal>=sr->field_count)return Null;
        selected=&sr->fields[ordinal];
        if(strcmp(selected->name,field->as.var_decl.name)||selected->index!=ordinal||
            LLVMStructGetTypeAtIndex(actual,(unsigned)ordinal)!=selected->llvm_type)return Null;
        actual=selected->llvm_type;f=iface_source_declaration(p->cg,field);
        extent=f.dimensions==1&&!field->as.var_decl.array_size_names[0]&&field->as.var_decl.array_sizes[0]>0?(uint64_t)field->as.var_decl.array_sizes[0]:0;
    }
    return actual;
}
static boolean_t iface_local_declaration_storage(cg_t *cg,node_t *n,LLVMTypeRef actual) {
    iface_local_plan_t *p=iface_local_current(cg);if(!p)return True;
    iface_local_node_t *r=iface_local_node(p,n,False);iface_local_binding_t *b=r?r->path.root:Null;
    if(!b)return True;
    boolean_t active=iface_factory_current(cg)==p;
    if(p->retained&&!b->legacy_demanded&&!active)return True;
    if(p->factory&&!b->demanded&&active){
        if(!actual||(iface_source_primitive(b->fact)?actual!=get_llvm_type(cg,b->fact.contract.type):
            !iface_local_layout(cg,b->fact,b->extent,actual)))
            return iface_local_receipt_error(p,n,"factory local storage has no exact source classification",False);
        if(active)b->storage_seen=True;return True;
    }
    if(!b->demanded)return True;
    if(!actual||!iface_local_layout(cg,b->fact,b->extent,actual))
        return iface_local_receipt_error(p,n,"local declaration lowering differs from its planned original storage identity/extent",b->legacy_demanded);
    if(active)b->storage_seen=True;return True;
}
static boolean_t iface_local_expression(cg_t *cg,node_t *n) {
    iface_local_plan_t *p=iface_local_current(cg);if(!p||!n)return True;
    if(!iface_local_lowering(cg,n))return False;
    iface_local_node_t *r=iface_local_node(p,n,False);if(!r)return True;
    boolean_t active=iface_factory_current(cg)==p;
    if(p->retained&&!r->legacy_required&&!active)return True;
    if((r->transfer_needed||r->checked_write||r->sink)&&r->path.valid&&r->path.root&&r->path.root->local){
        LLVMTypeRef storage=iface_local_storage(p,r->path);
        if(!storage)return iface_local_receipt_error(p,n,"emitted transfer storage differs from its original lexical binding or selected field path",r->legacy_required);
        if(r->transfer_needed&&!iface_local_layout(cg,r->transfer_target,0,storage))
            return iface_local_receipt_error(p,n,"emitted source storage differs from its exact planned concrete copy",r->legacy_required);
    }
    if(active)r->expression_seen=True;return True;
}
static LLVMValueRef iface_local_result(cg_t *cg,node_t *n,LLVMValueRef value) {
    iface_factory_raw_result(cg,n,value);
    iface_local_plan_t *p=iface_local_current(cg);if(!p||!n)return value;
    iface_local_node_t *r=iface_local_node(p,n,False);if(!r||!r->transfer_needed)return value;
    boolean_t active=iface_factory_current(cg)==p;
    if(p->retained&&!r->legacy_required&&!active)return value;
    if(!value||!iface_local_layout(cg,r->transfer_target,0,LLVMTypeOf(value)))
        iface_local_receipt_error(p,n,"emitted concrete copy value differs from the independently planned source identity",r->legacy_required);
    else if(active)r->result_seen=True;
    return value;
}

/* Only demanded ordinary local arrays consume these already-resolved immutable
 * extents. Unrelated declaration lowering is unchanged. */
static int iface_local_declaration_extent(cg_t *cg,node_t *n,uint64_t *extent) {
    iface_local_plan_t *p=iface_local_current(cg);if(!p)return 0;
    iface_local_node_t *r=iface_local_node(p,n,False);iface_local_binding_t *b=r?r->path.root:Null;
    if(!b||!b->demanded||!b->fact.dimensions)return 0;
    if(b->fact.dimensions!=1||!b->extent){iface_local_error_at(p,n,"selected concrete array requires a supported positive original extent");return -1;}
    *extent=b->extent;return 1;
}

static boolean_t iface_local_extracted(cg_t *cg,node_t *multi,node_t *target,usize_t index,LLVMValueRef value) {
    iface_local_plan_t *p=iface_local_current(cg);if(!p)return True;
    iface_local_node_t *r=iface_local_node(p,target,False);
    if(!r||!r->checked_write||!r->extracted_write)return True;
    if(!multi||multi->kind!=NodeMultiAssign||multi->as.multi_assign.values.count!=1||
        multi->as.multi_assign.values.items[0]!=r->rhs||index>=multi->as.multi_assign.targets.count||multi->as.multi_assign.targets.items[index]!=target)
        return iface_local_error_at(p,target,"multi-result lowering lost its original component/target mapping");
    iface_native_t *actual=iface_source_native(cg,iface_source_selected(cg,r->rhs,0));
    if(!actual||actual->decl!=r->extracted_decl||!value||
        !iface_local_layout(cg,r->extracted_source,0,LLVMTypeOf(value)))
        return iface_local_error_at(p,target,"multi-result lowering differs from the planned selected result identity");
    return True;
}

/* Locally seeded leaf results: owned source plans, path-specific obligations,
 * and emitted receipts are distinct. Nothing below changes source origin bits. */
static boolean_t iface_factory_ordinary(iface_native_t *n) {
    return iface_immediate_ordinary(n)&&!n->decl->as.fn_decl.is_method&&
        !n->decl->from_lib&&n->decl->as.fn_decl.body&&
        !n->decl->as.fn_decl.attr_flags&&!n->decl->headers;
}
static iface_native_t *iface_factory_identity(cg_t *cg,iface_local_plan_t *p) {
    boolean_t ambiguous=False;iface_native_t *n=iface_immediate_native(cg,p->context,&ambiguous);
    if(ambiguous||!iface_factory_ordinary(n)||n->decl!=p->owner||
        n->decl->as.fn_decl.body!=p->body||p->frame.owner!=p->owner||p->frame.body!=p->body||
        p->frame.context!=p->context)return Null;
    char module[512];mangle_module_prefix(p->owner->module_name?p->owner->module_name:"",module,sizeof(module));
    return !strcmp(module,p->frame.module)?n:Null;
}
static boolean_t iface_factory_signature(cg_t *cg,iface_native_t *n,boolean_t aggregate) {
    if(!iface_factory_ordinary(n)||!iface_immediate_native_arity(n)||n->decl->as.fn_decl.return_count!=1)return False;
    if(LLVMGetAttributeCountAtIndex(n->function,LLVMAttributeReturnIndex))return False;
    node_list_t args=n->decl->as.fn_decl.params;
    for(usize_t i=0;i<args.count;i++){
        node_t *d=args.items[i];iface_source_fact_t f=iface_source_declaration(cg,d);
        if(!d||d->kind!=NodeVarDecl||d->headers||d->as.var_decl.attr_flags||
           (d->as.var_decl.flags&~(VdeclConst|VdeclFinal))||d->as.var_decl.storage==StorageHeap||
           !iface_source_primitive(f)||LLVMGetAttributeCountAtIndex(n->function,(LLVMAttributeIndex)i+1)||
           LLVMTypeOf(LLVMGetParam(n->function,(unsigned)i))!=get_llvm_type(cg,f.contract.type))return False;
    }
    iface_source_fact_t result=iface_source_from_type(cg,n->decl->as.fn_decl.return_types[0],StorageDefault,n->decl);
    LLVMTypeRef type=LLVMGetReturnType(LLVMGlobalGetValueType(n->function));
    if(aggregate)return iface_local_layout(cg,result,0,type)&&LLVMGetLinkage(n->function)==LLVMInternalLinkage&&
        n->decl->as.fn_decl.linkage==LinkageInternal;
    return iface_source_primitive(result)&&type==get_llvm_type(cg,result.contract.type);
}
static boolean_t iface_factory_visit(cg_t *cg) {
    iface_factory_state_t *fs=cg->iface_factories;
    if(!fs)return False;
    if(fs->visits>=FactoryMaxVisits){fs->exhausted=True;return False;}
    ++fs->visits;return True;
}
static iface_factory_clean_t iface_factory_clean_type(cg_t *cg,iface_source_fact_t f,boolean_t pointer_field,unsigned depth) {
    if(!iface_factory_visit(cg)||depth>=IfaceMaxMetadataDepth||f.dimensions||f.status==IfaceSourceExcluded||f.origin||
       (f.status!=IfaceSourceKnown&&f.status!=IfaceSourceLiteral))return FactoryUnknown;
    if(f.contract.domain==StorageHeap)return FactoryCleanupPresent;
    if(f.contract.type.is_pointer)
        return pointer_field&&f.contract.type.base!=TypeFnPtr&&f.contract.type.base!=TypeClosure&&
            !f.contract.type.fn_ptr_desc&&!f.contract.type.elem_type?FactoryClean:FactoryUnknown;
    if(iface_source_primitive(f))return FactoryClean;
    if(!iface_local_struct(f))return FactoryUnknown;
    struct_reg_t *s=find_struct(cg,f.contract.nominal->as.type_decl.name);
    if(!s||s->layout_decl!=f.contract.nominal||s->layout_state!=2||s->is_union||s->is_interface||
       f.contract.nominal->as.type_decl.attr_flags)return FactoryUnknown;
    if(s->destructor)return FactoryCleanupPresent;
    iface_factory_clean_t answer=FactoryClean;
    node_list_t fields=f.contract.nominal->as.type_decl.fields;
    for(usize_t i=0;i<fields.count;i++){
        node_t *d=fields.items[i];if(!d||d->kind!=NodeVarDecl)return FactoryUnknown;
        if(d->as.var_decl.flags&VdeclComptimeField)continue;
        if(d->headers||d->as.var_decl.attr_flags||d->as.var_decl.bitfield_width||
            (d->as.var_decl.flags&~(VdeclConst|VdeclFinal)))return FactoryUnknown;
        iface_factory_clean_t child=iface_factory_clean_type(cg,iface_source_declaration(cg,d),True,depth+1);
        if(child>answer)answer=child;
    }
    return answer;
}
static iface_factory_clean_t iface_factory_clean_expr(iface_local_plan_t *p,node_t *n,iface_source_fact_t expected,unsigned depth) {
    if(!iface_factory_visit(p->cg)||!n||depth>=IfaceMaxMetadataDepth)return FactoryUnknown;
    iface_local_node_t *r=iface_local_node(p,n,False);if(!r)return FactoryUnknown;
    iface_source_fact_t f=r->fact;
    if(n->kind==NodeCompoundInit)f=expected;
    if(n->kind==NodeNilExpr&&expected.contract.type.is_pointer)return FactoryClean;
    iface_factory_clean_t answer=iface_factory_clean_type(p->cg,f,True,depth+1);
    if(answer!=FactoryClean&&n->kind!=NodeAssignExpr&&n->kind!=NodeCompoundAssign)return answer;
#define FC(x,t) do { iface_factory_clean_t c=iface_factory_clean_expr(p,(x),(t),depth+1); if(c>answer)answer=c; } while(0)
    switch(n->kind){
    case NodeIntLitExpr:case NodeFloatLitExpr:case NodeBoolLitExpr:case NodeCharLitExpr:case NodeIdentExpr:break;
    case NodeMemberExpr:FC(n->as.member_expr.object,iface_local_query(p,n->as.member_expr.object));break;
    case NodeBinaryExpr:FC(n->as.binary.left,iface_local_query(p,n->as.binary.left));FC(n->as.binary.right,iface_local_query(p,n->as.binary.right));break;
    case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr:
        if(!iface_source_primitive(f))return FactoryUnknown;
        FC(n->as.unary.operand,iface_local_query(p,n->as.unary.operand));break;
    case NodeCastExpr:FC(n->as.cast_expr.expr,iface_local_query(p,n->as.cast_expr.expr));break;
    case NodeAssignExpr:case NodeCompoundAssign:{
        node_t *target=n->kind==NodeAssignExpr?n->as.assign.target:n->as.compound_assign.target;
        node_t *value=n->kind==NodeAssignExpr?n->as.assign.value:n->as.compound_assign.value;
        iface_source_fact_t t=iface_local_query(p,target);
        answer=iface_factory_clean_type(p->cg,t,False,depth+1);FC(target,t);FC(value,t);break;}
    case NodeCallExpr:{
        boolean_t ambiguous=False;iface_native_t *native=iface_immediate_native(p->cg,r->selected_function,&ambiguous);
        if(!r->named_direct||ambiguous||!native||native->decl!=r->selected_decl||
            !iface_factory_signature(p->cg,native,False)||n->as.call.args.count!=native->decl->as.fn_decl.params.count)return FactoryUnknown;
        for(usize_t i=0;i<n->as.call.args.count;i++){
            iface_source_fact_t arg=iface_local_query(p,n->as.call.args.items[i]);
            if(!iface_source_primitive(arg))return FactoryUnknown;
            FC(n->as.call.args.items[i],arg);
        }break;}
    case NodeCompoundInit:case NodeDesigInit:{
        if(!iface_local_struct(f)||f.dimensions)return FactoryUnknown;
        node_list_t fields=f.contract.nominal->as.type_decl.fields;
        node_list_t items=n->kind==NodeCompoundInit?n->as.compound_init.items:n->as.desig_init.values;
        usize_t cursor=0;
        if(n->kind==NodeDesigInit&&items.count!=n->as.desig_init.fields.count)return FactoryUnknown;
        for(usize_t i=0;i<items.count;i++){
            node_t *value=items.items[i],*field=Null;const char *name=Null;
            if(n->kind==NodeDesigInit){node_t *label=n->as.desig_init.fields.items[i];if(!label||label->kind!=NodeIdentExpr)return FactoryUnknown;name=label->as.ident.name;}
            else if(value->kind==NodeInitField){name=value->as.init_field.name;value=value->as.init_field.value;}
            else if(value->kind==NodeSpreadExpr||value->kind==NodeInitIndex||value->kind==NodeRangeExpr)return FactoryUnknown;
            if(name){for(usize_t j=0;j<fields.count;j++)if(!strcmp(fields.items[j]->as.var_decl.name,name))field=fields.items[j];}
            else {while(cursor<fields.count&&(fields.items[cursor]->as.var_decl.flags&VdeclComptimeField))++cursor;if(cursor<fields.count)field=fields.items[cursor++];}
            if(!field||(field->as.var_decl.flags&VdeclComptimeField))return FactoryUnknown;
            FC(value,iface_source_declaration(p->cg,field));
        }break;}
    default:return FactoryUnknown;
    }
#undef FC
    return answer;
}
static iface_factory_clean_t iface_factory_clean_body(iface_local_plan_t *p) {
    if(!p->body||p->body->kind!=NodeBlock||!p->body->as.block.stmts.count)return FactoryUnknown;
    node_list_t stmts=p->body->as.block.stmts;
    iface_factory_clean_t answer=FactoryClean;
    for(usize_t i=0;i<stmts.count;i++){
        node_t *n=stmts.items[i];iface_factory_clean_t c=FactoryUnknown;
        if(n->kind==NodeVarDecl){
            iface_local_node_t *r=iface_local_node(p,n,False);iface_local_binding_t *b=r?r->path.root:Null;
            if(!b||!n->as.var_decl.init||n->headers||n->as.var_decl.attr_flags||n->as.var_decl.storage==StorageHeap||
                (n->as.var_decl.flags&~(VdeclConst|VdeclFinal|VdeclLet)))return FactoryUnknown;
            c=iface_factory_clean_type(p->cg,b->fact,False,0);
            iface_factory_clean_t init=iface_factory_clean_expr(p,n->as.var_decl.init,b->fact,0);if(init>c)c=init;
        }else if(n->kind==NodeExprStmt)c=iface_factory_clean_expr(p,n->as.expr_stmt.expr,iface_local_query(p,n->as.expr_stmt.expr),0);
        else if(n->kind==NodeRetStmt&&i+1==stmts.count&&n->as.ret_stmt.values.count==1&&
            n->as.ret_stmt.values.items[0]->kind==NodeIdentExpr)c=iface_factory_clean_expr(p,n->as.ret_stmt.values.items[0],iface_local_query(p,n->as.ret_stmt.values.items[0]),0);
        /* No target-elided branch or nested block is silently classified clean. */
        if(c>answer)answer=c;
    }
    return answer;
}
static iface_factory_summary_t *iface_factory_summary(iface_local_plan_t *p,node_t **steps,unsigned count) {
    cg_t *cg=p->cg;iface_factory_state_t *fs=cg->iface_factories;
    node_t *ret=p->body->kind==NodeBlock&&p->body->as.block.stmts.count?
        p->body->as.block.stmts.items[p->body->as.block.stmts.count-1]:Null;
    node_t *operand=ret&&ret->kind==NodeRetStmt&&ret->as.ret_stmt.values.count==1?ret->as.ret_stmt.values.items[0]:Null;
    iface_local_node_t *read=operand&&operand->kind==NodeIdentExpr?iface_local_node(p,operand,False):Null;
    iface_local_path_t path=read?read->path:(iface_local_path_t){0};
    for(unsigned i=0;i<count;i++)if(!steps[i]||!iface_local_append(p,&path,steps[i],operand))path.valid=False;
    for(iface_factory_summary_t *s=fs->summaries;s;s=s->next)
        if(s->plan==p&&iface_local_same_path(s->path,path))return s;
    if(fs->paths>=FactoryMaxPaths||(!p->factory&&fs->bodies>=FactoryMaxBodies)){fs->exhausted=True;return Null;}
    iface_factory_summary_t *s=iface_local_zero(sizeof(*s));s->plan=p;s->path=path;s->state=LocalUnsupported;
    s->reason="factory has no complete single initialized local-value result";s->evidence=ret?ret:p->owner;
    s->next=fs->summaries;fs->summaries=s;++fs->paths;
    if(!p->factory){p->factory=True;++fs->bodies;}p->participating=True;
    iface_native_t *native=iface_factory_identity(cg,p);
    if(!p->factory_classified){
        p->factory_classified=True;
        p->factory_eligible=native&&iface_factory_signature(cg,native,True)&&iface_factory_clean_body(p)==FactoryClean;
    }
    boolean_t eligible=p->factory_eligible;
    iface_local_binding_t *binding=read?read->path.root:Null;
    if(!binding||!binding->local||!binding->decl||binding->decl->kind!=NodeVarDecl||
        !binding->decl->as.var_decl.init||binding->block!=p->body||!path.valid||path.arrays||read->path.count)return s;
    p->return_node=ret;p->return_operand=operand;
    iface_source_frame_t *saved=cg->iface_query_frame;cg->iface_query_frame=&p->frame;
    boolean_t building=p->building,overlay=p->overlay_building,factory=p->factory_building;
    p->building=True;p->overlay_building=True;p->factory_building=True;
    iface_local_path_t root=read->path;
    s->root_proof=iface_local_demand(p,root,0);s->proof=iface_local_demand(p,path,0);
    iface_local_check(p,s->proof,read,FactoryCheckExpression|FactoryCheckResult);
    read->transfer_needed=True;read->transfer_target=binding->fact;
    iface_local_solve(p);p->building=building;p->overlay_building=overlay;p->factory_building=factory;cg->iface_query_frame=saved;
    iface_source_fact_t result=native&&native->decl->as.fn_decl.return_count==1?
        iface_source_from_type(cg,native->decl->as.fn_decl.return_types[0],StorageDefault,native->decl):iface_source_unknown("missing factory result");
    iface_local_copy_t root_match=iface_source_local_copy(binding->fact,result);
    if(s->proof&&s->root_proof){
        s->state=s->proof->state>s->root_proof->state?s->proof->state:s->root_proof->state;
        iface_local_proof_t *bad=s->proof->state>=s->root_proof->state?s->proof:s->root_proof;
        if(bad->reason){s->reason=bad->reason;s->evidence=bad->evidence;}
        if(root_match==IfaceLocalCopyMismatch){s->state=LocalMismatch;s->reason="factory returned local has a different original nominal root";s->evidence=operand;}
        else if(root_match!=IfaceLocalCopyExact||!eligible||p->incomplete||p->overlay_incomplete){
            if(s->state<LocalUnsupported)s->state=LocalUnsupported;
            if(bad->state!=LocalMismatch)s->reason="factory result requires a cleanup-free ordinary leaf with one final explicit local return";
        }
        for(iface_local_proof_t *q=p->proofs;q;q=q->next)if(q->upgrade&&q->path.root->frontier_count){
            if(s->state<LocalUnsupported)s->state=LocalUnsupported;
            if(s->state!=LocalMismatch)s->reason="factory result proof has a receiver frontier";
        }
    }
    return s;
}
static boolean_t iface_factory_value(iface_local_plan_t *p,iface_local_proof_t *q,node_t *n,
                                      iface_source_fact_t storage,node_t **steps,unsigned count) {
    iface_factory_state_t *fs=p->cg->iface_factories;if(!fs)return False;
    iface_local_node_t *r=iface_local_node(p,n,False);if(!r||!r->named_direct)return False;
    if(fs->request_count>=FactoryMaxRequests){fs->exhausted=True;iface_local_raise(q,LocalUnsupported,n,"factory result request budget exhausted");return True;}
    iface_factory_request_t *request=iface_local_zero(sizeof(*request));request->consumer=p;request->proof=q;
    request->call=n;request->function=r->selected_function;request->decl=r->selected_decl;request->source=storage;
    request->result=iface_immediate_storage(p->cg,storage.contract.nominal);
    request->next=fs->requests;fs->requests=request;request->proof_next=q->requests;q->requests=request;++fs->request_count;
    p->participating=True;
    iface_local_plan_t *factory=Null;
    for(iface_local_plan_t *other=fs->plans;other;other=other->retained_next)
        if(other->owner==r->selected_decl&&other->context==r->selected_function){factory=other;break;}
    if(factory)request->summary=iface_factory_summary(factory,steps,count);
    if(!request->summary){iface_local_raise(q,LocalDeclared,n,"selected result is not an eligible source-visible leaf factory");return True;}
    iface_factory_summary_t *s=request->summary;
    if(s->state!=LocalClosed)iface_local_raise(q,s->state,s->evidence,s->reason);
    return True;
}
static void iface_factory_dispose(cg_t *cg) {
    iface_factory_state_t *fs=cg->iface_factories;if(!fs)return;
    cg->iface_query_frame=Null;
    while(fs->requests){iface_factory_request_t *r=fs->requests;fs->requests=r->next;iface_local_release(r);}
    while(fs->summaries){iface_factory_summary_t *s=fs->summaries;fs->summaries=s->next;iface_local_release(s);}
    while(fs->plans){iface_local_plan_t *p=fs->plans;fs->plans=p->retained_next;
        if(cg->iface_local_plan==p)cg->iface_local_plan=Null;iface_local_destroy(p);}
    iface_local_release(fs);cg->iface_factories=Null;
}
static void iface_factory_prepare(cg_t *cg) {
    if(!cg->iface_count||!cg->ast||cg->ast->kind!=NodeModule)return;
    iface_factory_state_t *fs=iface_local_zero(sizeof(*fs));cg->iface_factories=fs;fs->discovering=True;
    for(usize_t i=0;i<cg->ast->as.module.decls.count&&!fs->exhausted;i++){
        node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=NodeFnDecl)continue;
        iface_native_t *native=iface_native(cg,d,Null);if(!iface_factory_ordinary(native))continue;
        if(cg->test_mode&&!d->module_name&&!strcmp(d->as.fn_decl.name,"main"))continue;
        boolean_t ambiguous=False;if(iface_immediate_native(cg,native->function,&ambiguous)!=native||ambiguous)continue;
        if(fs->plans_count>=FactoryMaxPlans){fs->exhausted=True;break;}
        iface_local_plan_t *p=iface_local_create(cg,d,d->as.fn_decl.body,native->function,True);
        p->retained_next=fs->plans;fs->plans=p;++fs->plans_count;
    }
    fs->discovering=False;
    if(!fs->exhausted)for(iface_local_plan_t *p=fs->plans;p;p=p->retained_next){
        iface_source_frame_t *saved=cg->iface_query_frame;cg->iface_query_frame=&p->frame;
        p->building=True;p->overlay_building=True;
        for(iface_local_node_t *r=p->nodes;r;r=r->next)if(r->sink&&r->proof&&r->proof->projected&&
            r->proof->result_boundary&&r->proof->state!=LocalClosed&&!r->path.arrays){
            r->upgrade=iface_local_demand(p,r->path,0);
        }
        iface_local_solve(p);p->building=False;p->overlay_building=False;cg->iface_query_frame=saved;
    }
    for(iface_local_plan_t *p=fs->plans;p;p=p->retained_next)if(p->participating)fs->retained_nodes+=p->node_count;
    if(fs->retained_nodes>FactoryMaxRetainedNodes)fs->exhausted=True;
    /* Exhaustion never narrows ordinary source acceptance. All partial proof
     * objects are discarded; the old per-body path remains authoritative. */
    if(fs->exhausted){iface_factory_dispose(cg);cg->iface_factory_exhausted=True;return;}
    iface_local_plan_t **at=&fs->plans;
    while(*at){iface_local_plan_t *p=*at;if(!p->participating){*at=p->retained_next;iface_local_destroy(p);}
        else{p->retained=True;at=&p->retained_next;}}
}
static iface_local_plan_t *iface_factory_current(cg_t *cg) {
    iface_local_plan_t *p=iface_local_current(cg);
    if(!p||!p->retained||!p->emitting||!p->frozen||p->building||cg->iface_query_frame||cg->generic_n||
        cg->cur_coro.active||cg->current_return_types!=p->owner->as.fn_decl.return_types||
        cg->current_return_count!=p->owner->as.fn_decl.return_count||strcmp(cg->current_module_prefix,p->frame.module)||
        !iface_factory_identity(cg,p))return Null;
    LLVMBasicBlockRef block=LLVMGetInsertBlock(cg->builder);
    return block&&LLVMGetBasicBlockParent(block)==p->context?p:Null;
}
static void iface_factory_call_selected(cg_t *cg,node_t *call,symbol_t *selected,boolean_t sibling) {
    iface_local_plan_t *p=iface_factory_current(cg);if(!p)return;
    iface_factory_state_t *fs=cg->iface_factories;
    for(iface_factory_request_t *r=fs->requests;r;r=r->next)if(r->consumer==p&&r->call==call){
        ++r->selected_count;boolean_t ambiguous=False;
        iface_native_t *native=selected?iface_immediate_native(cg,selected->value,&ambiguous):Null;
        if(sibling||!selected||selected->value!=r->function||ambiguous||!native||native->decl!=r->decl||
            !iface_factory_signature(cg,native,True)||selected->c_abi||selected->requires_receiver||
            selected->stype.base==TypeFnPtr||selected->stype.base==TypeClosure||call->kind!=NodeCallExpr||call->generic_use||
            call->as.call.args.count!=native->decl->as.fn_decl.params.count||!selected->scalar_signature||
            selected->scalar_signature->param_count!=native->decl->as.fn_decl.params.count||
            iface_immediate_type(cg,selected->scalar_signature->ret_type).decl!=r->source.contract.nominal)
            r->failure="selected named call differs from the planned factory native/source mapping";
        if(!r->failure)for(usize_t i=0;i<native->decl->as.fn_decl.params.count;i++){
            iface_source_fact_t formal=iface_source_declaration(cg,native->decl->as.fn_decl.params.items[i]);
            iface_source_fact_t signature=iface_source_from_type(cg,selected->scalar_signature->params[i].type,StorageDefault,call);
            if(!iface_source_primitive(signature)||signature.contract.type.base!=formal.contract.type.base)
                r->failure="selected factory primitive formal ordinal differs from original source";
        }
    }
}
static boolean_t iface_factory_call_agrees(cg_t *cg,iface_factory_request_t *r) {
    LLVMValueRef value=r->emitted;
    boolean_t ambiguous=False;iface_native_t *native=iface_immediate_native(cg,r->function,&ambiguous);
    if(ambiguous||!native||native->decl!=r->decl||!iface_factory_signature(cg,native,True)||
        !value||!LLVMIsACallInst(value)||!LLVMGetInstructionParent(value)||
        LLVMGetBasicBlockParent(LLVMGetInstructionParent(value))!=r->consumer->context||
        LLVMGetCalledValue(value)!=r->function||LLVMGetCalledFunctionType(value)!=LLVMGlobalGetValueType(r->function)||
        LLVMGetInstructionCallConv(value)!=LLVMCCallConv||LLVMTypeOf(value)!=r->result||
        LLVMGetNumArgOperands(value)!=r->call->as.call.args.count||
        LLVMGetCallSiteAttributeCount(value,LLVMAttributeReturnIndex))return False;
    for(unsigned i=0;i<LLVMGetNumArgOperands(value);i++)if(
        LLVMTypeOf(LLVMGetOperand(value,i))!=LLVMTypeOf(LLVMGetParam(r->function,i))||
        LLVMGetCallSiteAttributeCount(value,(LLVMAttributeIndex)i+1))return False;
    return True;
}
static void iface_factory_call_result(cg_t *cg,node_t *call,LLVMValueRef value) {
    iface_local_plan_t *p=iface_factory_current(cg);if(!p)return;
    for(iface_factory_request_t *r=cg->iface_factories->requests;r;r=r->next)if(r->consumer==p&&r->call==call){
        ++r->emitted_count;r->emitted=value;
        if(!iface_factory_call_agrees(cg,r))r->failure="emitted aggregate call differs from the exact planned factory result";
    }
}
static void iface_factory_return_begin(cg_t *cg,node_t *ret) {
    iface_local_plan_t *p=iface_factory_current(cg);if(!p||!p->factory)return;
    ++p->returns_started;p->return_in_progress=ret;
    if(ret!=p->return_node||ret->as.ret_stmt.values.count!=1||ret->as.ret_stmt.values.items[0]!=p->return_operand){
        p->factory_failure="factory return is not the one original explicit local return";return;
    }
    iface_local_node_t *r=iface_local_node(p,p->return_operand,False);iface_local_binding_t *b=r?r->path.root:Null;
    symbol_t *symbol=b?iface_source_lookup(cg,b->symbol.name):Null;
    if(!symbol||!symbol->iface_source||symbol->iface_source->decl!=b->decl||symbol->iface_source->context!=p->context||
        !iface_local_layout(cg,b->fact,0,symbol->type))p->factory_failure="factory return local lost its original root/storage identity";
    else p->return_storage=symbol->value;
}
static void iface_factory_raw_result(cg_t *cg,node_t *operand,LLVMValueRef raw) {
    iface_local_plan_t *p=iface_factory_current(cg);
    if(!p||!p->factory||!p->return_in_progress||operand!=p->return_operand)return;
    ++p->raw_returns_seen;p->raw_return=raw;
    iface_source_fact_t result=iface_source_from_type(cg,p->owner->as.fn_decl.return_types[0],StorageDefault,p->owner);
    if(!raw||!LLVMIsALoadInst(raw)||!iface_local_layout(cg,result,0,LLVMTypeOf(raw))||
        LLVMGetOperand(raw,0)!=p->return_storage)p->factory_failure="factory raw return is not the original local aggregate load";
}
static void iface_factory_return_end(cg_t *cg,node_t *ret,LLVMValueRef value,LLVMValueRef native_ret) {
    iface_local_plan_t *p=iface_factory_current(cg);if(!p||!p->factory)return;
    ++p->returns_seen;p->returned_value=value;p->native_return=native_ret;
    if(ret!=p->return_node||p->return_in_progress!=ret||value!=p->raw_return||!native_ret||
        !LLVMIsAReturnInst(native_ret)||LLVMGetNumOperands(native_ret)!=1||LLVMGetOperand(native_ret,0)!=p->raw_return)
        p->factory_failure="factory native return replaced the original raw result SSA value";
    p->return_in_progress=Null;
}
static void iface_factory_cleanup_scope(cg_t *cg) {
    iface_local_plan_t *p=iface_factory_current(cg);if(!p||!p->factory)return;
    for(usize_t i=0;i<cg->dtor_depth;i++)if(cg->dtor_stack[i].count||cg->dtor_stack[i].deferred_count)
        p->factory_failure="factory body registered an unclassified cleanup route";
}
static boolean_t iface_factory_return_sealed(cg_t *cg,iface_local_plan_t *p) {
    iface_native_t *native=iface_factory_identity(cg,p);
    if(!p->finalized||p->factory_failure||!native||!iface_factory_signature(cg,native,True)||
        !p->return_node||p->returns_started!=1||p->returns_seen!=1||p->raw_returns_seen!=1||
        !p->raw_return||p->raw_return!=p->returned_value||!p->native_return)return False;
    unsigned returns=0;
    for(LLVMBasicBlockRef b=LLVMGetFirstBasicBlock(p->context);b;b=LLVMGetNextBasicBlock(b)){
        LLVMValueRef t=LLVMGetBasicBlockTerminator(b);if(t&&LLVMIsAReturnInst(t)){
            ++returns;if(t!=p->native_return||LLVMGetNumOperands(t)!=1||LLVMGetOperand(t,0)!=p->raw_return)return False;
        }
    }
    for(iface_local_binding_t *b=p->bindings;b;b=b->next)if(b->local){
        iface_local_node_t *r=iface_local_node(p,b->decl,False);
        if(!b->storage_seen||!r||r->physical_failure)return False;
    }
    return returns==1;
}
static void iface_factory_finalize_body(cg_t *cg,node_t *decl) {
    iface_local_plan_t *p=iface_factory_current(cg);if(!p||p->owner!=decl)return;
    p->finalized=True;
    if(p->factory&&!iface_factory_return_sealed(cg,p)&&!p->factory_failure)
        p->factory_failure="factory finalization lacks one exact original raw-value native return";
}
static void iface_factory_reset_visits(cg_t *cg) {
    for(iface_local_plan_t *p=cg->iface_factories?cg->iface_factories->plans:Null;p;p=p->retained_next)
        for(iface_local_proof_t *q=p->proofs;q;q=q->next)q->seal_visit=0;
}
/* Source SCCs have already been closed using independently initialized local
 * producers. This walk records consumption/checks correspondence, never solves. */
static boolean_t iface_factory_proof_receipts(iface_local_plan_t *p,iface_local_proof_t *q,
                                               boolean_t final,boolean_t consume,unsigned depth,const char **why) {
    if(!q||depth>IfaceMaxMetadataDepth){*why="factory result receipt graph is incomplete";return False;}
    if(q->seal_visit)return True;q->seal_visit=1;
    if(q->state!=LocalClosed||p->incomplete||p->overlay_incomplete){*why=q->reason?q->reason:"factory result source plan is incomplete";return False;}
    iface_local_binding_t *b=q->path.root;iface_local_node_t *decl=iface_local_node(p,b->decl,False);
    if(!b->storage_seen||!decl||decl->physical_failure){*why="factory result local storage receipt is missing or failed";return False;}
    for(iface_local_check_t *c=q->checks;c;c=c->next){iface_local_node_t *r=c->node;
        if(r->physical_failure||((c->kind&FactoryCheckConstructor)&&!r->constructor_seen)||
            ((c->kind&FactoryCheckExpression)&&!r->expression_seen)||((c->kind&FactoryCheckResult)&&!r->result_seen)){
            *why=r->physical_failure?r->physical_failure:"factory result original transfer receipt is missing";return False;}
    }
    for(iface_factory_request_t *r=q->requests;r;r=r->proof_next){
        if(consume)r->consumed=True;
        if(r->failure||r->selected_count!=1||r->emitted_count!=1||!r->emitted||!r->summary||!iface_factory_call_agrees(p->cg,r)){
            *why=r->failure?r->failure:"factory result call receipt is missing or duplicated";return False;}
        iface_local_plan_t *factory=r->summary->plan;
        if(final){
            if(!iface_factory_return_sealed(p->cg,factory)){*why=factory->factory_failure?factory->factory_failure:"consumed factory result has an unsealed native return";return False;}
            if(!iface_factory_proof_receipts(factory,r->summary->root_proof,True,consume,depth+1,why)||
                !iface_factory_proof_receipts(factory,r->summary->proof,True,consume,depth+1,why))return False;
        }
    }
    for(iface_local_edge_t *e=q->edges;e;e=e->next)
        if(!iface_factory_proof_receipts(p,e->source,final,consume,depth+1,why))return False;
    return True;
}
static int iface_factory_require(cg_t *cg,iface_local_plan_t *p,iface_local_node_t *r,symbol_t *symbol) {
    if(!r||!r->upgrade||!r->sink||iface_factory_current(cg)!=p)return 0;
    /* Generic origin is never repaired, including a nominally ordinary slot. */
    if(symbol->iface_excluded_origin&IfaceOriginGeneric)return 0;
    r->consumed=True;
    if(r->upgrade->state!=LocalClosed){iface_local_error_at(p,r->upgrade->evidence?r->upgrade->evidence:r->ast,
        r->upgrade->reason?r->upgrade->reason:"factory result has no complete independent local producer");return -1;}
    if(iface_local_frontier(r->path.root,r)!=LocalClosed||!iface_local_layout(cg,r->upgrade->target,0,symbol->type)){
        iface_local_error_at(p,r->ast,"factory result sink differs from its exact original local use");return -1;}
    const char *why=Null;iface_factory_reset_visits(cg);
    if(!iface_factory_proof_receipts(p,r->upgrade,False,True,0,&why)){
        iface_local_error_at(p,r->ast,why);return -1;}
    return 1; /* Producer may still be pending; final artifact seal is mandatory. */
}
static int iface_factory_consume(cg_t *cg,iface_local_plan_t *p,iface_local_node_t *r,symbol_t *symbol) {
    return IFACE_FACTORY_RESULT_PROMOTION?iface_factory_require(cg,p,r,symbol):0;
}
static boolean_t iface_factory_seal(cg_t *cg) {
    iface_factory_state_t *fs=cg->iface_factories;if(!fs)return True;
    boolean_t okay=True;
    for(iface_local_plan_t *p=fs->plans;p;p=p->retained_next)for(iface_local_node_t *r=p->nodes;r;r=r->next)if(r->consumed){
        const char *why=Null;iface_factory_reset_visits(cg);
        if(!p->finalized||!iface_factory_identity(cg,p)||!iface_factory_proof_receipts(p,r->upgrade,True,False,0,&why)){
            iface_local_error_at(p,r->ast,why?why:"consumed factory result lost its original consumer/native identity");okay=False;
        }
    }
    return okay;
}
