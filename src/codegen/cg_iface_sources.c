/* I1b.1: source actual contracts, not stored-value/return-body provenance.
 * Pure queries borrow immutable ASTs and copy facts; they never keep a symbol
 * table entry address, lower expressions, instantiate types, or choose a type
 * from an expected nominal/LLVM shape. Preparation and emission are separate. */
typedef enum { IfaceSourceUnknown, IfaceSourceKnown, IfaceSourceLiteral,
               IfaceSourceNull, IfaceSourceExcluded } iface_source_status_t;
typedef struct {
    iface_source_status_t status;
    iface_type_t contract;
    node_t *evidence, *owner;
    LLVMValueRef context; /* declaration scope identity, never a borrowed symbol */
    unsigned origin;
    int dimensions;
    storage_t slot_domain;
    ptr_perm_t access;
    boolean_t addressable, fresh_address;
    const char *reason;
} iface_source_fact_t;
struct iface_source_record {
    iface_source_record_t *next;
    heap_t heap;
    node_t *decl;
    LLVMValueRef context;
    iface_source_fact_t fact;
};
typedef enum { IfaceTransferReject, IfaceTransferExact, IfaceTransferPrimitive,
               IfaceTransferPointerWeaken, IfaceTransferFreshObjectAddress,
               IfaceTransferNullableNil, IfaceTransferDescriptorCopy,
               IfaceTransferConcreteBox } iface_transfer_t;

static boolean_t iface_local_fact(cg_t *, node_t *, iface_source_fact_t *);
static iface_source_fact_t iface_source_expr_at(cg_t *, node_t *, type_info_t, usize_t);
static symbol_t *iface_source_selected(cg_t *, node_t *, usize_t);
static iface_signature_t *iface_source_slot(cg_t *, node_t *, usize_t);
static iface_source_fact_t iface_source_unknown(const char *why) {
    return (iface_source_fact_t){.status=IfaceSourceUnknown,.reason=why};
}
static boolean_t iface_source_error(node_t *n, const char *why) {
    ast_source_loc_t loc=iface_node_loc(n); if(!loc.col)loc.col=1;if(!loc.len)loc.len=1;
    iface_error(loc,"interface source argument:",why); return False;
}
/* cg_lookup marks symbols used. This equivalent lookup is intentionally pure. */
static symbol_t *iface_source_lookup(cg_t *cg, const char *name) {
    if(!name)return Null;
    boolean_t planned=False;
    symbol_t *s=iface_local_lookup(cg,name,&planned);
    if(!planned)s=symtab_lookup(&cg->locals,name); char qualified[1024];
    if(!s && cg->current_module_prefix[0]) {
        int z=snprintf(qualified,sizeof(qualified),"%s__%s",cg->current_module_prefix,name);
        if(z>=0 && (size_t)z<sizeof(qualified))s=symtab_lookup(&cg->globals,qualified);
    }
    return s?s:symtab_lookup(&cg->globals,name);
}
static iface_native_t *iface_source_native(cg_t *cg, symbol_t *s) {
    for(iface_native_t *n=cg->iface_natives;s && n;n=n->next)if(n->function==s->value)return n;
    return Null;
}
static boolean_t iface_source_is_interface(iface_type_t t) {
    return t.nominal && t.nominal->as.type_decl.decl_kind==TypeDeclInterface;
}
static boolean_t iface_source_primitive(iface_source_fact_t f) {
    return (f.status==IfaceSourceKnown || f.status==IfaceSourceLiteral) &&
        !f.dimensions && (f.access&PtrRead) && !f.contract.nominal && scalar_numeric(f.contract.type);
}
static iface_source_fact_t iface_source_from_type(cg_t *cg,type_info_t raw,storage_t domain,node_t *evidence) {
    iface_source_fact_t f={.status=IfaceSourceKnown,.evidence=evidence,.context=cg->current_fn,.access=PtrRead};
    f.origin=iface_type_origin(cg,raw);
    if(f.origin){f.status=IfaceSourceExcluded;f.contract.type=raw;f.reason="generic/dependent source identity is unsupported";return f;}
    /* Slices are storage carriers, not admissible scalar actuals. Their element
     * contract is queried only after a source index removes this descriptor. */
    if(raw.base==TypeSlice && raw.elem_type && !raw.is_pointer)f.contract.type=raw;
    else { node_t *seen[IfaceMaxMetadataDepth];
        if(!iface_resolve_type(cg,raw,False,True,False,seen,0,&f.contract))
            return iface_source_unknown(f.contract.source_failure ? f.contract.source_failure : "unsupported or unresolved raw declared type");
    }
    if(domain!=StorageDefault&&f.contract.type.written_storage!=StorageDefault&&domain!=f.contract.type.written_storage)
        return iface_source_unknown("conflicting raw source domain annotations");
    f.contract.domain=f.contract.type.written_storage!=StorageDefault?f.contract.type.written_storage:domain;
    f.contract.raw_domain=domain;
    return f;
}
static iface_source_fact_t iface_source_declaration(cg_t *cg,node_t *d) {
    if(!d || d->kind!=NodeVarDecl)return iface_source_unknown("missing original variable declaration");
    iface_source_fact_t f=iface_source_from_type(cg,d->as.var_decl.type,d->as.var_decl.storage,d);
    f.dimensions=d->as.var_decl.array_ndim;
    if(!f.dimensions && (d->as.var_decl.flags&VdeclArray))f.dimensions=1;
    f.addressable=True;f.fresh_address=True;
    f.access=(d->as.var_decl.flags&(VdeclConst|VdeclFinal))?PtrRead:PtrReadWrite;
    /* A pointer declaration's domain describes its pointee; its ordinary local
     * slot remains stack. Global slots have static/unknown storage domain. */
    f.slot_domain=cg->current_fn?StorageStack:StorageDefault;
    if(!f.contract.type.is_pointer && d->as.var_decl.storage==StorageHeap)f.slot_domain=StorageHeap;
    return f;
}
static iface_source_record_t *iface_source_record_var(cg_t *cg,node_t *d) {
    heap_t h=allocate(1,sizeof(iface_source_record_t));iface_source_record_t *r=h.pointer;
    *r=(iface_source_record_t){.next=cg->iface_sources,.heap=h,.decl=d,.context=cg->current_fn};
    cg->iface_sources=r;
    if((d->as.var_decl.flags&VdeclLet) && d->as.var_decl.type.base==TypeVoid && !d->as.var_decl.type.is_pointer) {
        r->fact=iface_source_expr_at(cg,d->as.var_decl.init,NO_TYPE,0);
        r->fact.evidence=d;r->fact.context=cg->current_fn;r->fact.addressable=True;r->fact.fresh_address=True;
        r->fact.slot_domain=cg->current_fn?StorageStack:StorageDefault;
        r->fact.access=(d->as.var_decl.flags&(VdeclConst|VdeclFinal))?PtrRead:PtrReadWrite;
    } else {
        /* Unsupported unrelated declarations stay raw until an interface seam
         * consumes them, so metadata retention never broadens diagnostics. */
        r->fact=iface_source_unknown("raw declaration requires a source query");
    }
    return r;
}
static void iface_source_free(cg_t *cg) {
    while(cg->iface_sources){iface_source_record_t *r=cg->iface_sources;cg->iface_sources=r->next;deallocate(r->heap);}
}
static iface_source_fact_t iface_source_symbol(cg_t *cg,symbol_t *s) {
    if(!s || !s->iface_source)return iface_source_unknown("missing retained original declaration metadata");
    iface_source_record_t *r=s->iface_source;
    iface_source_fact_t f;
    if(r->decl->as.var_decl.flags&VdeclLet)f=r->fact;
    else {f=iface_source_declaration(cg,r->decl);f.context=r->context;
        f.slot_domain=r->context?StorageStack:StorageDefault;
        if(!f.contract.type.is_pointer && r->decl->as.var_decl.storage==StorageHeap)f.slot_domain=StorageHeap;
    }
    f.origin|=s->iface_excluded_origin;
    return f;
}
static symbol_t *iface_source_named_method(cg_t *cg,const char *owner,const char *method,boolean_t fallback_module) {
    if(!owner||!method)return Null;
    char name[1024];struct_reg_t *sr=find_struct(cg,owner);symbol_t *s=Null;
    const char *prefix=sr&&sr->mod_prefix&&sr->mod_prefix[0]?sr->mod_prefix:
        fallback_module&&cg->current_module_prefix[0]?cg->current_module_prefix:Null;
    if(prefix){snprintf(name,sizeof(name),"%s__%s__%s",prefix,owner,method);s=symtab_lookup(&cg->globals,name);}
    if(!s){snprintf(name,sizeof(name),"%s.%s",owner,method);s=symtab_lookup(&cg->globals,name);}
    return s;
}
/* Match the actual native dispatch's module/static/sibling precedence. This
 * consumes already registered callables; it does not instantiate a template. */
static symbol_t *iface_source_selected(cg_t *cg,node_t *n,usize_t depth) {
    if(!n||depth>IfaceMaxMetadataDepth)return Null;
    char name[1024];const char *owner=Null,*method=Null;
    if(n->kind==NodeCallExpr || n->kind==NodeErrPropCall || n->kind==NodeThreadCall || n->kind==NodeAsyncCall) {
        const char *callee=n->kind==NodeCallExpr?n->as.call.callee:n->kind==NodeErrPropCall?n->as.err_prop_call.callee:
            n->kind==NodeThreadCall?n->as.thread_call.callee:n->as.async_call.callee;
        symbol_t *s=iface_source_lookup(cg,callee);
        return s?s:iface_source_named_method(cg,cg->current_struct_name,callee,True);
    }
    if(n->kind==NodeColonCall){lib_entry_t *l=find_lib_entry(cg,n->as.colon_call.module_name);if(!l||!l->mod_prefix)return Null;
        if(n->as.colon_call.type_name)snprintf(name,sizeof(name),"%s__%s__%s",l->mod_prefix,n->as.colon_call.type_name,n->as.colon_call.method_name);
        else snprintf(name,sizeof(name),"%s__%s",l->mod_prefix,n->as.colon_call.method_name);
        return symtab_lookup(&cg->globals,name);
    }
    if(n->kind==NodeConstructorCall){owner=n->as.ctor_call.type_name;method="new";
        struct_reg_t *sr=find_struct(cg,owner); const char *pfx=sr&&sr->mod_prefix&&sr->mod_prefix[0]?sr->mod_prefix:
            cg->current_module_prefix[0]?cg->current_module_prefix:Null;
        symbol_t *s=Null;
        if(pfx){snprintf(name,sizeof(name),"%s__%s__new",pfx,owner);s=symtab_lookup(&cg->globals,name);}
        if(!s){snprintf(name,sizeof(name),"%s__new",owner);s=symtab_lookup(&cg->globals,name);}
        if(!s){snprintf(name,sizeof(name),"%s.new",owner);s=symtab_lookup(&cg->globals,name);}return s;
    }
    if(n->kind==NodeSelfMethodCall){owner=n->as.self_method_call.type_name?n->as.self_method_call.type_name:cg->current_struct_name;method=n->as.self_method_call.method;}
    if(n->kind==NodeMethodCall){node_t *o=n->as.method_call.object;method=n->as.method_call.method;
        if(o->kind==NodeIdentExpr){lib_entry_t *l=find_lib_entry(cg,o->as.ident.name);
            if(l){if(l->mod_prefix&&l->mod_prefix[0])snprintf(name,sizeof(name),"%s__%s",l->mod_prefix,method);else snprintf(name,sizeof(name),"%s",method);
                return symtab_lookup(&cg->globals,name);}
            symbol_t *s=iface_source_named_method(cg,o->as.ident.name,method,False);if(s)return s;
        }
        iface_source_fact_t f=iface_source_expr_at(cg,o,NO_TYPE,depth+1);
        owner=f.status==IfaceSourceKnown&&f.contract.nominal?f.contract.nominal->as.type_decl.name:Null;
    }
    return iface_source_named_method(cg,owner,method,n->kind==NodeSelfMethodCall);
}
static boolean_t iface_source_callable_agrees(node_t *call,iface_native_t *native) {
    if(!native||!native->decl)return False;
    generic_use_t *use=call->generic_use;
    if(use && (native->decl->kind!=NodeFnDecl || !native->decl->as.fn_decl.type_param_count ||
        strcmp(use->base_name,native->decl->as.fn_decl.name)))return False;
    return True;
}
static iface_source_fact_t iface_source_result(cg_t *cg,node_t *n,usize_t depth) {
    iface_signature_t *slot=iface_source_slot(cg,n,depth+1);
    if(slot){iface_source_fact_t f={.status=IfaceSourceKnown,.contract=slot->result,.evidence=slot->decl,.access=PtrRead};return f;}
    if(n->kind==NodeLambdaCall && n->as.lambda_call.lambda && n->as.lambda_call.lambda->kind==NodeLambda)
        return iface_source_from_type(cg,n->as.lambda_call.lambda->as.lambda_expr.ret_type,StorageDefault,n->as.lambda_call.lambda);
    symbol_t *s=iface_source_selected(cg,n,depth+1);iface_native_t *native=iface_source_native(cg,s);
    if(s && s->iface_source && s->iface_source->decl->as.var_decl.type.fn_ptr_desc) {
        type_info_t raw=s->iface_source->decl->as.var_decl.type;
        if(raw.base==TypeFnPtr && !raw.fn_ptr_desc->is_c_abi && !raw.fn_ptr_desc->is_variadic)
            return iface_source_from_type(cg,raw.fn_ptr_desc->ret_type,StorageDefault,s->iface_source->decl);
    }
    if(!iface_source_callable_agrees(n,native) || native->decl->kind!=NodeFnDecl || native->decl->as.fn_decl.return_count!=1)
        return iface_source_unknown("selected callable has no matching single original result contract");
    type_info_t raw=native->decl->as.fn_decl.return_types[0];
    if(raw.base==TypeUser && raw.user_name)for(usize_t i=0;i<native->decl->as.fn_decl.type_param_count;i++)
        if(!strcmp(raw.user_name,native->decl->as.fn_decl.type_params[i]))return iface_source_unknown("dependent selected result contract is unsupported");
    return iface_source_from_type(cg,raw,StorageDefault,native->decl);
}
static iface_source_fact_t iface_source_field(cg_t *cg,node_t *n,usize_t depth) {
    iface_source_fact_t base;const char *field;
    if(n->kind==NodeSelfMemberExpr){const char *owner=n->as.self_member.type_name?n->as.self_member.type_name:cg->current_struct_name;
        base=iface_source_from_type(cg,(type_info_t){.base=TypeUser,.user_name=(char*)owner},StorageDefault,n);
        base.addressable=True;base.fresh_address=False;base.access=PtrReadWrite;field=n->as.self_member.field;
    } else {node_t *o=n->as.member_expr.object;field=n->as.member_expr.field;
        if(o->kind==NodeIdentExpr && !iface_source_lookup(cg,o->as.ident.name)) {
            node_t *e=iface_quiet_type(cg,o->as.ident.name);
            if(e&&e->as.type_decl.decl_kind==TypeDeclEnum)for(usize_t i=0;i<e->as.type_decl.variants.count;i++)
                if(!strcmp(e->as.type_decl.variants.items[i]->as.enum_variant.name,field))
                    return iface_source_from_type(cg,(type_info_t){.base=TypeUser,.user_name=e->as.type_decl.name},StorageDefault,e);
        }
        base=iface_source_expr_at(cg,o,NO_TYPE,depth+1);
    }
    if(base.status!=IfaceSourceKnown || !base.contract.nominal || base.dimensions || base.contract.type.ptr_depth>1)
        return iface_source_unknown("field requires an ordinary raw owner declaration");
    node_t *owner=base.contract.nominal;
    if(owner->as.type_decl.type_param_count)return iface_source_unknown("generic field owner is unsupported");
    if(base.contract.type.is_pointer&&!(base.access&PtrRead))return iface_source_unknown("field owner pointer source is not readable");
    if(base.contract.type.is_pointer&&base.contract.type.nullable)return iface_source_unknown("nullable field-owner dereference requires an explicit reviewed source contract");
    if(base.contract.type.is_pointer){base.access=base.contract.type.ptr_perm;base.slot_domain=base.contract.domain;base.fresh_address=False;}
    for(usize_t i=0;i<owner->as.type_decl.fields.count;i++){node_t *d=owner->as.type_decl.fields.items[i];
        if(!strcmp(d->as.var_decl.name,field)){
            iface_source_fact_t f=iface_source_declaration(cg,d);f.owner=owner;f.context=base.context;
            f.addressable=base.addressable||base.contract.type.is_pointer;f.fresh_address=base.fresh_address;
            f.slot_domain=base.slot_domain;f.access&=base.access;return f;
        }
    }
    return iface_source_unknown("field has no unique raw declaration");
}
static boolean_t iface_source_pointer_compatible(iface_type_t from,iface_type_t to,const char **why) {
    if(!from.type.is_pointer || !to.type.is_pointer){*why="pointer-versus-value source category mismatch";return False;}
    if(from.type.base!=to.type.base || from.nominal!=to.nominal){*why="pointer pointee identity mismatch";return False;}
    if(from.type.ptr_depth!=to.type.ptr_depth){*why="pointer depth mismatch";return False;}
    for(int i=1;i<from.type.ptr_depth;i++)if(from.type.ptr_perms[i]!=to.type.ptr_perms[i]){*why="inner pointer layers must remain invariant at interface boundary";return False;}
    if(to.type.ptr_perm&~from.type.ptr_perm){*why="pointer permission/arithmetic capability widening is unsupported";return False;}
    if(from.type.nullable&&!to.type.nullable){*why="nullable pointer cannot satisfy nonnullable formal";return False;}
    if(to.domain!=StorageDefault && from.domain!=to.domain){*why="pointer pointee domain is unknown or does not match formal";return False;}
    return True;
}
static type_info_t iface_source_common(node_t *a,node_t *b,iface_source_fact_t af,iface_source_fact_t bf) {
    type_info_t at=af.contract.type,bt=bf.contract.type;
    boolean_t al=scalar_literal_tree(a),bl=scalar_literal_tree(b);
    if(al&&!bl&&scalar_integer(bt))return bt;if(bl&&!al&&scalar_integer(at))return at;
    if(scalar_integer(at)&&scalar_integer(bt)){
        if(scalar_unsigned(at)!=scalar_unsigned(bt)){
            uint64_t am,bm;boolean_t an,bn;
            if(al&&bl&&scalar_literal(a,&am,&an)&&scalar_literal(b,&bm,&bn)&&!an&&!bn)return scalar_type(TypeU64);
            return NO_TYPE;
        }
        return scalar_bits(at)>=scalar_bits(bt)?at:bt;
    }
    if(scalar_numeric(at)&&scalar_numeric(bt)&&(is_float_type(at)||is_float_type(bt)))return scalar_type(at.base==TypeF64||bt.base==TypeF64?TypeF64:TypeF32);
    return NO_TYPE;
}
static boolean_t iface_source_immediate_conversion(node_t *rhs,iface_source_fact_t a,iface_source_fact_t target) {
    if((a.status!=IfaceSourceKnown&&a.status!=IfaceSourceLiteral)||!(a.access&PtrRead)||a.dimensions)return False;
    if(a.contract.type.is_pointer||target.contract.type.is_pointer){const char *why=Null;return iface_source_pointer_compatible(a.contract,target.contract,&why);}
    if(a.contract.nominal||target.contract.nominal)return a.contract.nominal==target.contract.nominal;
    if(!iface_source_primitive(a)||!iface_source_primitive(target))return False;
    type_info_t from=a.contract.type,to=target.contract.type;uint64_t m;boolean_t neg;
    if(scalar_literal(rhs,&m,&neg)&&scalar_integer(to)&&to.base!=TypeBool)return scalar_literal_fits(rhs,to);
    if(scalar_integer(from)&&scalar_integer(to)&&to.base!=TypeBool)
        return scalar_bits(to)>=scalar_bits(from)&&(scalar_unsigned(from)==scalar_unsigned(to)||(scalar_unsigned(from)&&scalar_bits(to)>scalar_bits(from)));
    return True;
}
static iface_source_fact_t iface_source_expr_at(cg_t *cg,node_t *n,type_info_t context,usize_t depth) {
    if(!n||depth>IfaceMaxMetadataDepth)return iface_source_unknown("missing expression or source metadata depth exceeds 256");
    iface_source_fact_t cached; if(iface_local_fact(cg,n,&cached))return cached;
    iface_source_fact_t f={.status=IfaceSourceKnown,.access=PtrRead,.evidence=n};
    uint64_t magnitude;boolean_t negative;
    if(scalar_literal(n,&magnitude,&negative)){f.status=IfaceSourceLiteral;f.contract.type=scalar_literal_type(n,context);return f;}
#define IF_SOURCE(N,C) iface_source_expr_at(cg,(N),(C),depth+1)
    switch(n->kind){
    case NodeNilExpr:f.status=IfaceSourceNull;return f;
    case NodeBoolLitExpr:f.contract.type=scalar_type(TypeBool);return f;
    case NodeCharLitExpr:f.contract.type=scalar_integer(context)&&context.base!=TypeBool?context:scalar_type(TypeU8);return f;
    case NodeFloatLitExpr:f.contract.type=scalar_type(TypeF64);return f;
    case NodeSizeofExpr:f.contract.type=scalar_type(TypeU64);return f;
    case NodeStrLitExpr:f.contract.type=(type_info_t){.base=TypeI8,.is_pointer=True,.ptr_depth=1,.ptr_perm=PtrRead,.ptr_perms={PtrRead}};return f;
    case NodeIdentExpr:return iface_source_symbol(cg,iface_source_lookup(cg,n->as.ident.name));
    case NodeDesigInit:return iface_source_from_type(cg,(type_info_t){.base=TypeUser,.user_name=n->as.desig_init.type_name,.generic_use=n->generic_use},StorageDefault,n);
    case NodeMemberExpr:case NodeSelfMemberExpr:return iface_source_field(cg,n,depth+1);
    case NodeCallExpr:case NodeMethodCall:case NodeSelfMethodCall:case NodeColonCall:case NodeConstructorCall:case NodeLambdaCall:return iface_source_result(cg,n,depth+1);
    case NodeIndexExpr:case NodeFlaggedIndex:{
        node_t *o=n->kind==NodeIndexExpr?n->as.index_expr.object:n->as.flagged_index.object;
        node_t *idx=n->kind==NodeIndexExpr?n->as.index_expr.index:n->as.flagged_index.index;
        iface_source_fact_t ix=IF_SOURCE(idx,NO_TYPE);if(!iface_source_primitive(ix)||!scalar_integer(ix.contract.type))return iface_source_unknown("index requires a primitive integer source");
        f=IF_SOURCE(o,NO_TYPE);if(f.status!=IfaceSourceKnown)return f;
        if(f.dimensions){--f.dimensions;return f;}
        if(f.contract.type.base==TypeSlice&&!f.contract.type.is_pointer&&f.contract.type.elem_type){
            if(!(f.access&PtrRead))return iface_source_unknown("slice descriptor source is not readable");
            iface_source_fact_t e=iface_source_from_type(cg,*f.contract.type.elem_type,StorageDefault,f.evidence);
            e.addressable=True;e.access=PtrReadWrite;e.slot_domain=f.contract.domain;e.fresh_address=False;return e;
        }
        if(!f.contract.type.is_pointer)return iface_source_unknown("index has no original array/slice/pointer element contract");
        goto dereference;
    }
    case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr:{
        f=IF_SOURCE(n->as.unary.operand,n->as.unary.op==TokBang?NO_TYPE:context);
        if(n->as.unary.op==TokStar){if(f.status!=IfaceSourceKnown||!f.contract.type.is_pointer||f.dimensions)return iface_source_unknown("dereference requires a source pointer");goto dereference;}
        if(!iface_source_primitive(f))return iface_source_unknown("nonprimitive unary argument expression is unsupported");
        if(n->as.unary.op==TokPlusPlus||n->as.unary.op==TokMinusMinus)
            if(n->as.unary.operand->kind!=NodeIdentExpr||!(f.access&PtrWrite))return iface_source_unknown("increment actual requires a writable primitive identifier");
        f.addressable=False;if(n->as.unary.op==TokBang)f.contract.type=scalar_type(TypeBool);return f;
    }
    case NodeAddrOf:{
        f=IF_SOURCE(n->as.addr_of.operand,NO_TYPE);
        if(f.status!=IfaceSourceKnown||!f.addressable||f.dimensions)return iface_source_unknown("address actual requires supported selected object storage");
        type_info_t *t=&f.contract.type;if(t->ptr_depth>=8)return iface_source_unknown("address pointer depth exceeds 8");
        for(int i=t->ptr_depth;i>0;i--)t->ptr_perms[i]=t->ptr_perms[i-1];
        ++t->ptr_depth;t->is_pointer=True;t->nullable=False;t->written_nullable=False;
        t->ptr_perm=f.access|(f.fresh_address?PtrArith:0);t->ptr_perms[0]=t->ptr_perm;
        f.contract.domain=f.slot_domain;f.addressable=False;f.access=PtrRead;return f;
    }
    case NodeBinaryExpr:{
        node_t *a=n->as.binary.left,*b=n->as.binary.right;boolean_t literals=scalar_literal_tree(a)&&scalar_literal_tree(b);
        iface_source_fact_t af=IF_SOURCE(a,literals?context:NO_TYPE),bf=IF_SOURCE(b,literals?context:NO_TYPE);token_kind_t op=n->as.binary.op;
        if(af.status==IfaceSourceKnown&&bf.status==IfaceSourceKnown&&!af.contract.type.is_pointer&&!bf.contract.type.is_pointer&&
            (af.access&PtrRead)&&(bf.access&PtrRead)&&af.contract.nominal&&af.contract.nominal==bf.contract.nominal&&af.contract.nominal->as.type_decl.decl_kind==TypeDeclEnum&&(op==TokEqEq||op==TokBangEq)){
            f.contract.type=scalar_type(TypeBool);return f;}
        if(!iface_source_primitive(af)||!iface_source_primitive(bf))return iface_source_unknown("nonprimitive or mixed nominal binary argument expression is unsupported");
        f.contract.type=iface_source_common(a,b,af,bf);
        if(f.contract.type.base==TypeVoid)return iface_source_unknown("primitive expression has no permitted common source type");
        if(op==TokEqEq||op==TokBangEq||op==TokLt||op==TokGt||op==TokLtEq||op==TokGtEq||op==TokAmpAmp||op==TokPipePipe)f.contract.type=scalar_type(TypeBool);
        if(op==TokLtLt||op==TokGtGt)f.contract.type=af.contract.type;return f;
    }
    case NodeCastExpr:{
        iface_source_fact_t a=IF_SOURCE(n->as.cast_expr.expr,NO_TYPE);
        f=iface_source_from_type(cg,n->as.cast_expr.target,StorageDefault,n);
        if(iface_source_primitive(a)&&iface_source_primitive(f))return f;
        if(a.status!=IfaceSourceKnown||f.status!=IfaceSourceKnown||!(a.access&PtrRead))return iface_source_unknown("cast operand lacks an admitted original source contract");
        const char *why="cast cannot create nominal source identity";
        if(a.contract.type.is_pointer&&f.contract.type.is_pointer){f.contract.domain=a.contract.domain;
            if(iface_source_pointer_compatible(a.contract,f.contract,&why))return f;return iface_source_unknown(why);}
        if(a.contract.nominal==f.contract.nominal && a.contract.nominal && a.contract.nominal->as.type_decl.decl_kind!=TypeDeclEnum &&
            !a.contract.type.is_pointer&&!f.contract.type.is_pointer){f.origin=a.origin;return f;}
        return iface_source_unknown(why);
    }
    case NodeTernaryExpr:{
        iface_source_fact_t c=IF_SOURCE(n->as.ternary.cond,NO_TYPE);
        iface_source_fact_t a=IF_SOURCE(n->as.ternary.then_expr,context),b=IF_SOURCE(n->as.ternary.else_expr,context);
        if(!iface_source_primitive(c))return iface_source_unknown("conditional actual requires a primitive condition");
        if(iface_source_primitive(a)&&iface_source_primitive(b)){f.contract.type=iface_source_common(n->as.ternary.then_expr,n->as.ternary.else_expr,a,b);
            if(f.contract.type.base!=TypeVoid)return f;}
        if(a.status!=IfaceSourceKnown||b.status!=IfaceSourceKnown||a.dimensions||b.dimensions||!(a.access&PtrRead)||!(b.access&PtrRead))return iface_source_unknown("conditional arms require independently known source contracts");
        iface_type_t aj=a.contract,bj=b.contract;
        if(!aj.type.is_pointer&&!bj.type.is_pointer&&aj.nominal==bj.nominal&&aj.nominal&&
            (aj.nominal->as.type_decl.decl_kind==TypeDeclEnum||aj.nominal->as.type_decl.decl_kind==TypeDeclInterface)&&
            aj.domain!=StorageHeap&&bj.domain!=StorageHeap)aj.domain=bj.domain=StorageDefault;
        if(iface_types_equal(&aj,&bj)){a.contract=aj;a.addressable=False;a.access=PtrRead;a.origin|=b.origin;return a;}
        return iface_source_unknown("ambiguous nonnumeric conditional source join is unsupported");
    }
    case NodeAssignExpr:case NodeCompoundAssign:{
        node_t *target=n->kind==NodeAssignExpr?n->as.assign.target:n->as.compound_assign.target;
        node_t *rhs=n->kind==NodeAssignExpr?n->as.assign.value:n->as.compound_assign.value;
        if(!target||target->kind!=NodeIdentExpr)return iface_source_unknown("assignment actual requires a plain identifier target");
        f=IF_SOURCE(target,NO_TYPE);
        if(f.status!=IfaceSourceKnown||!f.addressable||!(f.access&PtrWrite)||f.dimensions)return iface_source_unknown("assignment actual target requires writable known source storage");
        iface_source_fact_t a=IF_SOURCE(rhs,iface_source_primitive(f)?f.contract.type:NO_TYPE);
        if(n->kind==NodeCompoundAssign&&(!iface_source_primitive(f)||!iface_source_primitive(a)))return iface_source_unknown("compound assignment actual requires primitive operands");
        if(!iface_source_immediate_conversion(rhs,a,f))return iface_source_unknown("assignment actual source contract does not match its original target");
        f.addressable=False;f.access=PtrRead;f.origin|=a.origin;return f;
    }
    default:return iface_source_unknown("expression carrier has no admitted source contract");
    }
 dereference:
    if(!(f.access&PtrRead))return iface_source_unknown("pointer source value is not readable");
    if(f.contract.type.nullable)return iface_source_unknown("nullable pointer dereference cannot establish a nonnullable source contract");
    f.access=f.contract.type.ptr_perm;f.slot_domain=f.contract.domain;f.fresh_address=False;f.addressable=True;
    f.contract.type=ti_deref_one(f.contract.type);f.contract.type.nullable=False;f.contract.type.written_nullable=False;
    f.contract.domain=StorageDefault; /* one outer domain cannot certify a stored inner pointer */
    return f;
#undef IF_SOURCE
}

static iface_signature_t *iface_source_slot(cg_t *cg,node_t *n,usize_t depth) {
    if(!n||n->kind!=NodeMethodCall||depth>IfaceMaxMetadataDepth)return Null;
    node_t *o=n->as.method_call.object;usize_t ii=(usize_t)-1;
    if(o->kind==NodeMemberExpr)ii=find_interface_index(cg,o->as.member_expr.field);
    if(ii==(usize_t)-1){iface_source_fact_t f=iface_source_expr_at(cg,o,NO_TYPE,depth+1);
        if(f.status==IfaceSourceKnown && iface_source_is_interface(f.contract))ii=find_interface_index(cg,f.contract.nominal->as.type_decl.name);}
    if(ii==(usize_t)-1)return Null;
    iface_method_group_t *g=iface_group(cg,ii,n->as.method_call.method);return g?g->signature:Null;
}
static boolean_t iface_source_mentions_type(cg_t *cg,type_info_t t,usize_t depth) {
    if(depth>IfaceMaxMetadataDepth)return True; /* fail closed for affected carriers */
    if(t.base==TypeUser&&t.user_name){node_t *d=iface_quiet_type(cg,t.user_name);
        if(d&&d->as.type_decl.decl_kind==TypeDeclInterface)return True;
        if(d&&d->as.type_decl.decl_kind==TypeDeclAlias)return iface_source_mentions_type(cg,d->as.type_decl.alias_type,depth+1);}
    if(t.elem_type&&iface_source_mentions_type(cg,*t.elem_type,depth+1))return True;
    if(t.fn_ptr_desc){fn_ptr_desc_t *d=t.fn_ptr_desc;
        if(iface_source_mentions_type(cg,d->ret_type,depth+1))return True;
        for(usize_t i=0;i<d->param_count;i++)if(iface_source_mentions_type(cg,d->params[i].type,depth+1))return True;}
    return False;
}
static boolean_t iface_source_mentions_expr(cg_t *cg,node_t *n,usize_t depth) {
    if(!n)return False;if(depth>IfaceMaxMetadataDepth)return True;
#define IF_MENTION(N) iface_source_mentions_expr(cg,(N),depth+1)
    switch(n->kind){
    case NodeIdentExpr:{symbol_t *s=iface_source_lookup(cg,n->as.ident.name);if(!s)return False;
        if(s->iface_source&&(s->iface_source->decl->as.var_decl.flags&VdeclLet))
            return iface_source_mentions_type(cg,s->iface_source->fact.contract.type,0)||iface_source_mentions_type(cg,s->stype,0);
        return iface_source_mentions_type(cg,s->iface_source?s->iface_source->decl->as.var_decl.type:s->stype,0);}
    case NodeCastExpr:return iface_source_mentions_type(cg,n->as.cast_expr.target,0)||IF_MENTION(n->as.cast_expr.expr);
    case NodeAddrOf:return IF_MENTION(n->as.addr_of.operand);
    case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr:return IF_MENTION(n->as.unary.operand);
    case NodeAssignExpr:return IF_MENTION(n->as.assign.target)||IF_MENTION(n->as.assign.value);
    case NodeTernaryExpr:return IF_MENTION(n->as.ternary.then_expr)||IF_MENTION(n->as.ternary.else_expr);
    case NodeIndexExpr:return IF_MENTION(n->as.index_expr.object);
    case NodeFlaggedIndex:return IF_MENTION(n->as.flagged_index.object);
    case NodeMemberExpr:case NodeSelfMemberExpr:{iface_source_fact_t f=iface_source_expr_at(cg,n,NO_TYPE,depth+1);return f.status==IfaceSourceKnown&&iface_source_is_interface(f.contract);}
    case NodeCallExpr:case NodeMethodCall:case NodeSelfMethodCall:case NodeColonCall:case NodeConstructorCall:{
        iface_signature_t *slot=iface_source_slot(cg,n,depth+1);if(slot)return iface_source_is_interface(slot->result);
        symbol_t *s=iface_source_selected(cg,n,depth+1);iface_native_t *d=iface_source_native(cg,s);
        if(d&&d->decl->kind==NodeFnDecl)for(usize_t i=0;i<d->decl->as.fn_decl.return_count;i++)if(iface_source_mentions_type(cg,d->decl->as.fn_decl.return_types[i],0))return True;
        return s&&s->stype.fn_ptr_desc&&iface_source_mentions_type(cg,s->stype.fn_ptr_desc->ret_type,0);}
    case NodeLambda:for(usize_t i=0;i<n->as.lambda_expr.params.count;i++)if(iface_source_mentions_type(cg,n->as.lambda_expr.params.items[i]->as.var_decl.type,0))return True;
        return iface_source_mentions_type(cg,n->as.lambda_expr.ret_type,0);
    default:return False;
    }
#undef IF_MENTION
}
/* Lowered actual types are conservative detection only. A requested future/va
 * extraction type, dependent substitution or erased carrier must never supply
 * positive source identity, but must not hide an interface from the gate. */
static boolean_t iface_source_actual_bears(cg_t *cg,node_t *n) {
    return iface_source_mentions_expr(cg,n,0)||iface_source_mentions_type(cg,scalar_expr_type(cg,n,NO_TYPE),0);
}
static node_list_t iface_source_args(node_t *n) {
    switch(n->kind){
    case NodeCallExpr:return n->as.call.args;
    case NodeMethodCall:return n->as.method_call.args;
    case NodeSelfMethodCall:return n->as.self_method_call.args;
    case NodeColonCall:return n->as.colon_call.args;
    case NodeConstructorCall:return n->as.ctor_call.args;
    case NodeLambdaCall:return n->as.lambda_call.args;
    case NodeThreadCall:return n->as.thread_call.args;
    case NodeAsyncCall:return n->as.async_call.args;
    case NodeErrPropCall:return n->as.err_prop_call.args;
    default:return (node_list_t){0};
    }
}
/* Record syntactic reads in preparation, including a rejected actual. This is
 * ordinary use accounting, separate from the immutable source-fact query. */
static void iface_source_note_use(cg_t *cg,node_t *n,usize_t depth) {
    if(!n||depth>IfaceMaxMetadataDepth)return;
    if(n->kind==NodeIdentExpr){symbol_t *s=iface_source_lookup(cg,n->as.ident.name);if(s)s->used=True;return;}
    node_list_t args=iface_source_args(n);for(usize_t i=0;i<args.count;i++)iface_source_note_use(cg,args.items[i],depth+1);
#define IF_READ(N) iface_source_note_use(cg,(N),depth+1)
    switch(n->kind){
    case NodeAddrOf:IF_READ(n->as.addr_of.operand);break;
    case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr:IF_READ(n->as.unary.operand);break;
    case NodeCastExpr:IF_READ(n->as.cast_expr.expr);break;
    case NodeMemberExpr:IF_READ(n->as.member_expr.object);break;
    case NodeMethodCall:IF_READ(n->as.method_call.object);break;
    case NodeIndexExpr:IF_READ(n->as.index_expr.object);IF_READ(n->as.index_expr.index);break;
    case NodeFlaggedIndex:IF_READ(n->as.flagged_index.object);IF_READ(n->as.flagged_index.index);break;
    case NodeBinaryExpr:IF_READ(n->as.binary.left);IF_READ(n->as.binary.right);break;
    case NodeTernaryExpr:IF_READ(n->as.ternary.cond);IF_READ(n->as.ternary.then_expr);IF_READ(n->as.ternary.else_expr);break;
    case NodeAssignExpr:IF_READ(n->as.assign.target);IF_READ(n->as.assign.value);break;
    case NodeCompoundAssign:IF_READ(n->as.compound_assign.target);IF_READ(n->as.compound_assign.value);break;
    default:break;
    }
#undef IF_READ
}
/* Existing lazy call selection is preparation, not part of the fact query.
 * It may emit a callable definition, never evaluate a source actual. */
static void iface_source_prepare(cg_t *cg,node_t *n,usize_t depth) {
    if(!n||depth>IfaceMaxMetadataDepth)return;
    node_list_t args=iface_source_args(n);
    for(usize_t i=0;i<args.count;i++)iface_source_prepare(cg,args.items[i],depth+1);
    switch(n->kind){
    case NodeCallExpr:case NodeMethodCall:case NodeSelfMethodCall:case NodeColonCall:case NodeConstructorCall:
        scalar_call_symbol(cg,n);break;
    case NodeAddrOf:iface_source_prepare(cg,n->as.addr_of.operand,depth+1);break;
    case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr:iface_source_prepare(cg,n->as.unary.operand,depth+1);break;
    case NodeCastExpr:iface_source_prepare(cg,n->as.cast_expr.expr,depth+1);break;
    case NodeTernaryExpr:iface_source_prepare(cg,n->as.ternary.cond,depth+1);iface_source_prepare(cg,n->as.ternary.then_expr,depth+1);iface_source_prepare(cg,n->as.ternary.else_expr,depth+1);break;
    case NodeBinaryExpr:iface_source_prepare(cg,n->as.binary.left,depth+1);iface_source_prepare(cg,n->as.binary.right,depth+1);break;
    case NodeMemberExpr:iface_source_prepare(cg,n->as.member_expr.object,depth+1);break;
    case NodeIndexExpr:iface_source_prepare(cg,n->as.index_expr.object,depth+1);iface_source_prepare(cg,n->as.index_expr.index,depth+1);break;
    case NodeFlaggedIndex:iface_source_prepare(cg,n->as.flagged_index.object,depth+1);iface_source_prepare(cg,n->as.flagged_index.index,depth+1);break;
    case NodeSliceExpr:iface_source_prepare(cg,n->as.slice_expr.object,depth+1);iface_source_prepare(cg,n->as.slice_expr.lo,depth+1);iface_source_prepare(cg,n->as.slice_expr.hi,depth+1);break;
    case NodeAssignExpr:iface_source_prepare(cg,n->as.assign.target,depth+1);iface_source_prepare(cg,n->as.assign.value,depth+1);break;
    case NodeCompoundAssign:iface_source_prepare(cg,n->as.compound_assign.target,depth+1);iface_source_prepare(cg,n->as.compound_assign.value,depth+1);break;
    default:break;
    }
}
/* Constant-conversion proof uses only a whitelisted primitive constant tree.
 * Runtime/selected-call paths never reach scalar's type-query machinery here. */
static boolean_t iface_source_constant_tree(cg_t *cg,node_t *n,usize_t depth) {
    if(!n||depth>IfaceMaxMetadataDepth)return False;
    switch(n->kind){
    case NodeIntLitExpr:case NodeCharLitExpr:case NodeBoolLitExpr:return True;
    case NodeIdentExpr:{symbol_t *s=iface_source_lookup(cg,n->as.ident.name);return s&&s->iface_source&&(s->flags&SymConst)&&LLVMIsAGlobalVariable(s->value)&&!s->iface_source->decl->as.var_decl.type.generic_use;}
    case NodeCastExpr:return iface_source_constant_tree(cg,n->as.cast_expr.expr,depth+1);
    case NodeUnaryPrefixExpr:return (n->as.unary.op==TokMinus||n->as.unary.op==TokPlus||n->as.unary.op==TokTilde||n->as.unary.op==TokBang)&&iface_source_constant_tree(cg,n->as.unary.operand,depth+1);
    case NodeBinaryExpr:return iface_source_constant_tree(cg,n->as.binary.left,depth+1)&&iface_source_constant_tree(cg,n->as.binary.right,depth+1);
    case NodeTernaryExpr:return iface_source_constant_tree(cg,n->as.ternary.cond,depth+1)&&iface_source_constant_tree(cg,n->as.ternary.then_expr,depth+1)&&iface_source_constant_tree(cg,n->as.ternary.else_expr,depth+1);
    default:return False;
    }
}
static iface_transfer_t iface_source_transfer(cg_t *cg,node_t *arg,node_t *original,iface_source_fact_t f,iface_type_t formal,boolean_t boxing) {
    if(f.status==IfaceSourceNull){if(!formal.type.is_pointer||!formal.type.nullable){iface_source_error(arg,"nil requires a nullable pointer formal");return IfaceTransferReject;}
        if(formal.domain!=StorageDefault){iface_source_error(arg,"nullable nil with a required pointee domain is unsupported");return IfaceTransferReject;}return IfaceTransferNullableNil;}
    if(f.status!=IfaceSourceKnown&&f.status!=IfaceSourceLiteral){iface_source_error(arg,f.reason?f.reason:"unknown source contract");return IfaceTransferReject;}
    if(f.dimensions){iface_source_error(arg,"array value requires an explicit supported element/address actual");return IfaceTransferReject;}
    if(!(f.access&PtrRead)){iface_source_error(arg,"actual source storage is not readable");return IfaceTransferReject;}
    iface_type_t a=f.contract;
    if(a.type.is_pointer||formal.type.is_pointer){const char *why=Null;
        if(!iface_source_pointer_compatible(a,formal,&why)){iface_source_error(arg,why);return IfaceTransferReject;}
        return f.fresh_address?IfaceTransferFreshObjectAddress:IfaceTransferPointerWeaken;}
    if(iface_source_is_interface(formal)){
        if(formal.domain==StorageHeap){iface_source_error(arg,"heap by-value descriptor formal is unsupported");return IfaceTransferReject;}
        if(a.nominal==formal.nominal)return IfaceTransferDescriptorCopy;
        if(a.nominal&&a.nominal->as.type_decl.decl_kind==TypeDeclStruct&&boxing&&arg->kind==NodeIdentExpr){
            symbol_t *s=iface_source_lookup(cg,arg->as.ident.name);
            if(!s||!iface_concrete_origin_allowed(cg,original,s))return IfaceTransferReject;
            usize_t ii=find_interface_index(cg,formal.nominal->as.type_decl.name);iface_binding_t *b=iface_find_binding(cg,a.nominal,ii);
            if(b&&(b->state==IfaceBindingTableReady||b->state==IfaceBindingNativeReady))return IfaceTransferConcreteBox;
            if(b&&b->state==IfaceBindingLocalUnavailable){
                iface_error(iface_node_loc(a.nominal),"imported internal interface implementation is unavailable for local table:",formal.nominal->as.type_decl.name);
                return IfaceTransferReject;
            }
            iface_source_error(arg,"concrete source has no declared/transitive ready exact interface binding");return IfaceTransferReject;
        }
        iface_source_error(arg,"descriptor requires exact interface identity; concrete boxing is limited to existing identifier routes");return IfaceTransferReject;
    }
    if(a.nominal||formal.nominal){if(a.nominal==formal.nominal&&a.nominal&&a.nominal->as.type_decl.decl_kind==TypeDeclEnum)return IfaceTransferExact;
        iface_source_error(arg,"nominal enum/interface argument identity mismatch");return IfaceTransferReject;}
    if(iface_source_primitive(f)&&scalar_numeric(formal.type)){
        uint64_t mag;boolean_t neg;
        if(scalar_literal(arg,&mag,&neg)&&scalar_integer(formal.type)&&formal.type.base!=TypeBool&&!scalar_literal_fits(arg,formal.type)){
            iface_source_error(arg,"integer literal is outside the formal range");return IfaceTransferReject;}
        type_info_t from=a.type,to=formal.type;
        if(scalar_integer(from)&&scalar_integer(to)&&to.base!=TypeBool &&
            (scalar_bits(to)<scalar_bits(from)||(scalar_unsigned(from)!=scalar_unsigned(to)&&!(scalar_unsigned(from)&&scalar_bits(to)>scalar_bits(from)))) &&
            !(iface_source_constant_tree(cg,arg,0)&&scalar_const_conversion_fits(cg,arg,from,to))){
            iface_source_error(arg,"implicit integer conversion may change value; use an explicit cast");return IfaceTransferReject;}
        return IfaceTransferPrimitive;
    }
    iface_source_error(arg,"unsupported source argument contract");return IfaceTransferReject;
}
static boolean_t iface_source_check_slot(cg_t *cg,node_t *node,iface_signature_t *s) {
    if(!s||!s->valid)return False;node_list_t args=iface_source_args(node);
    iface_source_note_use(cg,node,0);
    if(args.count!=s->count)return iface_source_error(node,"interface call argument count does not match declaration");
    usize_t before=get_error_count();
    for(usize_t i=0;i<args.count;i++)iface_source_prepare(cg,args.items[i],0);
    for(usize_t i=0;i<args.count;i++){
        type_info_t context=!s->params[i].nominal&&scalar_numeric(s->params[i].type)?s->params[i].type:NO_TYPE;
        iface_source_fact_t f=iface_source_expr_at(cg,args.items[i],context,0);
        node_t located=*args.items[i];if(!located.col){located.col=node->col;located.len=node->len;}
        if(iface_source_transfer(cg,&located,args.items[i],f,s->params[i],False)==IfaceTransferReject)return False;
    }
    return before==get_error_count();
}
static LLVMValueRef iface_source_emit_slot_arg(cg_t *cg,node_t *arg,iface_type_t formal) {
    if(arg->kind==NodeNilExpr)return LLVMConstNull(get_llvm_type(cg,formal.type));
    return !formal.nominal&&scalar_numeric(formal.type)?gen_scalar_as(cg,arg,formal.type):gen_expr(cg,arg);
}
static boolean_t iface_source_formal(cg_t *cg,symbol_t *sym,usize_t index,iface_type_t *out) {
    iface_native_t *native=iface_source_native(cg,sym);type_info_t raw;storage_t storage;
    if(native&&native->decl->kind==NodeFnDecl){if(index>=native->decl->as.fn_decl.params.count)return False;
        node_t *p=native->decl->as.fn_decl.params.items[index];raw=p->as.var_decl.type;storage=p->as.var_decl.storage;}
    else if(sym&&sym->iface_source&&sym->iface_source->decl->as.var_decl.type.fn_ptr_desc){fn_ptr_desc_t *d=sym->iface_source->decl->as.var_decl.type.fn_ptr_desc;
        if(index>=d->param_count)return False;raw=d->params[index].type;storage=d->params[index].storage;}
    else return False;
    if(!iface_source_mentions_type(cg,raw,0))return False;
    iface_source_fact_t f=iface_source_from_type(cg,raw,storage,native?native->decl:sym->iface_source->decl);
    if(f.status!=IfaceSourceKnown)return False;*out=f.contract;return True;
}
static boolean_t iface_source_native_bears(cg_t *cg,symbol_t *s) {
    iface_native_t *native=iface_source_native(cg,s);
    if(native&&native->decl->kind==NodeFnDecl){node_t *d=native->decl;
        for(usize_t i=0;i<d->as.fn_decl.params.count;i++)if(iface_source_mentions_type(cg,d->as.fn_decl.params.items[i]->as.var_decl.type,0))return True;
        for(usize_t i=0;i<d->as.fn_decl.return_count;i++)if(iface_source_mentions_type(cg,d->as.fn_decl.return_types[i],0))return True;}
    if(s&&iface_source_mentions_type(cg,s->iface_source?s->iface_source->decl->as.var_decl.type:s->stype,0))return True;
    /* Lowered metadata may expose an interface that raw dependent metadata did
     * not establish. It is only a rejection trigger, never positive evidence. */
    if(s&&s->scalar_signature){fn_ptr_desc_t *d=s->scalar_signature;
        if(iface_source_mentions_type(cg,d->ret_type,0))return True;
        for(usize_t i=0;i<d->param_count;i++)if(iface_source_mentions_type(cg,d->params[i].type,0))return True;}
    return False;
}
static boolean_t iface_source_call_is_boxing(cg_t *cg,node_t *n,symbol_t *s) {
    if(n->kind==NodeCallExpr)return True;
    if(n->kind!=NodeMethodCall||n->as.method_call.object->kind!=NodeIdentExpr||!s||!s->requires_receiver)return False;
    return iface_source_lookup(cg,n->as.method_call.object->as.ident.name)!=Null;
}
static boolean_t iface_source_preflight_selected(cg_t *,node_t *,symbol_t *);
static boolean_t iface_source_preflight_call_at(cg_t *,node_t *,usize_t);
static boolean_t iface_source_validate_nested(cg_t *cg,node_t *n,usize_t depth) {
    if(!n)return True;if(depth>IfaceMaxMetadataDepth)return iface_source_error(n,"nested actual metadata exceeds 256");
    switch(n->kind){
    case NodeCallExpr:case NodeMethodCall:case NodeSelfMethodCall:case NodeColonCall:case NodeConstructorCall:
    case NodeLambdaCall:case NodeThreadCall:case NodeAsyncCall:case NodeErrPropCall:return iface_source_preflight_call_at(cg,n,depth+1);
    case NodeAddrOf:return iface_source_validate_nested(cg,n->as.addr_of.operand,depth+1);
    case NodeUnaryPrefixExpr:case NodeUnaryPostfixExpr:return iface_source_validate_nested(cg,n->as.unary.operand,depth+1);
    case NodeCastExpr:return iface_source_validate_nested(cg,n->as.cast_expr.expr,depth+1);
    case NodeMemberExpr:return iface_source_validate_nested(cg,n->as.member_expr.object,depth+1);
    case NodeBinaryExpr:return iface_source_validate_nested(cg,n->as.binary.left,depth+1)&&iface_source_validate_nested(cg,n->as.binary.right,depth+1);
    case NodeSliceExpr:return iface_source_validate_nested(cg,n->as.slice_expr.object,depth+1)&&iface_source_validate_nested(cg,n->as.slice_expr.lo,depth+1)&&iface_source_validate_nested(cg,n->as.slice_expr.hi,depth+1);
    case NodeIndexExpr:return iface_source_validate_nested(cg,n->as.index_expr.object,depth+1)&&iface_source_validate_nested(cg,n->as.index_expr.index,depth+1);
    case NodeFlaggedIndex:return iface_source_validate_nested(cg,n->as.flagged_index.object,depth+1)&&iface_source_validate_nested(cg,n->as.flagged_index.index,depth+1);
    case NodeTernaryExpr:return iface_source_validate_nested(cg,n->as.ternary.cond,depth+1)&&iface_source_validate_nested(cg,n->as.ternary.then_expr,depth+1)&&iface_source_validate_nested(cg,n->as.ternary.else_expr,depth+1);
    case NodeAssignExpr:return iface_source_validate_nested(cg,n->as.assign.value,depth+1);
    case NodeCompoundAssign:return iface_source_validate_nested(cg,n->as.compound_assign.value,depth+1);
    default:return True;
    }
}

static boolean_t iface_source_preflight_call_at(cg_t *cg,node_t *n,usize_t depth) {
    if(!cg->iface_count)return True;
    if(depth>IfaceMaxMetadataDepth)return iface_source_error(n,"nested actual call depth exceeds 256");
    switch(n->kind){case NodeCallExpr:case NodeMethodCall:case NodeSelfMethodCall:case NodeColonCall:case NodeConstructorCall:
        case NodeLambdaCall:case NodeThreadCall:case NodeAsyncCall:case NodeErrPropCall:break;default:return True;}
    iface_source_note_use(cg,n,0);
    iface_source_prepare(cg,n,0);
    node_list_t args=iface_source_args(n);
    for(usize_t i=0;i<args.count;i++)if(!iface_source_validate_nested(cg,args.items[i],depth+1))return False;
    iface_signature_t *slot=iface_source_slot(cg,n,0);
    if(slot){
        node_t *o=n->as.method_call.object;
        boolean_t qualified=o->kind==NodeMemberExpr&&find_interface_index(cg,o->as.member_expr.field)!=(usize_t)-1;
        if(!qualified){iface_source_fact_t f=iface_source_expr_at(cg,o,NO_TYPE,0);
            if(f.status!=IfaceSourceKnown||!iface_source_is_interface(f.contract)||f.contract.type.is_pointer||f.dimensions||!(f.access&PtrRead))
                return iface_source_error(n,"pointer-to-interface descriptor receiver or unreadable/unknown descriptor source is unsupported");}
        return iface_source_check_slot(cg,n,slot);
    }
    if(n->kind==NodeMethodCall){
        type_info_t lowered=scalar_expr_type(cg,n->as.method_call.object,NO_TYPE);
        if(iface_source_mentions_type(cg,lowered,0))return iface_source_error(n,"interface-bearing receiver has no complete original source contract");
    }
    return iface_source_preflight_selected(cg,n,iface_source_selected(cg,n,0));
}
static boolean_t iface_source_preflight_call(cg_t *cg,node_t *n) {
    return iface_source_preflight_call_at(cg,n,0);
}
static boolean_t iface_source_preflight_selected(cg_t *cg,node_t *n,symbol_t *s) {
    if(!cg->iface_count)return True;
    /* Conservative lowered queries may instantiate nested generics. Never keep
     * a growable symbol-table entry across that phase, even for unknown syntax. */
    symbol_t selected; if(s){selected=*s;s=&selected;}
    node_list_t args=iface_source_args(n);iface_native_t *native=iface_source_native(cg,s);
    boolean_t bears=iface_source_native_bears(cg,s);
    for(usize_t i=0;i<args.count&&!bears;i++)bears=iface_source_actual_bears(cg,args.items[i]);
    if(n->kind==NodeLambdaCall&&iface_source_mentions_expr(cg,n->as.lambda_call.lambda,0))bears=True;
    if(!bears)return True;
    symbol_t *current=iface_source_selected(cg,n,0);
    if(s && (!current || current->value!=s->value || current->iface_source!=s->iface_source))
        return iface_source_error(n,"selected native callable differs from retained source selection");
    if(n->kind==NodeLambdaCall||n->kind==NodeThreadCall||n->kind==NodeAsyncCall||n->kind==NodeErrPropCall)
        return iface_source_error(n,"interface-bearing lambda/thread/async/error carrier is unsupported");
    if(!s)return iface_source_error(n,"interface-bearing call has no selected native source metadata");
    fn_ptr_desc_t *desc=s->iface_source?s->iface_source->decl->as.var_decl.type.fn_ptr_desc:Null;
    if(s->c_abi||s->stype.base==TypeClosure||(desc&&(desc->is_c_abi||desc->is_variadic))||
        (native&&native->decl->kind==NodeFnDecl&&(native->c_abi||native->decl->as.fn_decl.is_variadic||native->decl->as.fn_decl.is_async)))
        return iface_source_error(n,"interface-bearing C/variadic/closure/coroutine carrier is unsupported");
    if(native&&!iface_source_callable_agrees(n,native))return iface_source_error(n,"selected callable identity disagrees with retained generic application");
    if(s->scalar_signature&&iface_source_mentions_type(cg,s->scalar_signature->ret_type,0)&&native&&native->decl->kind==NodeFnDecl&&
        (native->decl->as.fn_decl.return_count!=1||!iface_source_mentions_type(cg,native->decl->as.fn_decl.return_types[0],0)))
        return iface_source_error(n,"interface-bearing result lacks a complete original source contract");
    usize_t count=native&&native->decl->kind==NodeFnDecl?native->decl->as.fn_decl.params.count:desc?desc->param_count:(usize_t)-1;
    if(count==(usize_t)-1)return iface_source_error(n,"interface-bearing callable lacks a complete original formal contract");
    if(count!=args.count)return iface_source_error(n,"interface-bearing call argument count does not match declaration");
    usize_t before=get_error_count();
    for(usize_t i=0;i<args.count;i++){
        iface_type_t formal;
        if(iface_source_formal(cg,s,i,&formal)){
            if(iface_source_is_interface(formal)&&!formal.type.is_pointer&&iface_source_call_is_boxing(cg,n,s)&&args.items[i]->kind==NodeIdentExpr){
                symbol_t *actual=iface_source_lookup(cg,args.items[i]->as.ident.name);
                if(actual&&actual->stype.base==TypeUser&&!actual->stype.is_pointer&&
                    find_interface_index(cg,actual->stype.user_name)==(usize_t)-1&&!iface_concrete_origin_allowed(cg,args.items[i],actual))return False;
            }
            iface_source_fact_t f=iface_source_expr_at(cg,args.items[i],NO_TYPE,0);
            node_t located=*args.items[i];if(!located.col){located.col=n->col;located.len=n->len;}
            if(iface_source_transfer(cg,&located,args.items[i],f,formal,iface_source_call_is_boxing(cg,n,s))==IfaceTransferReject)return False;
        }else if((s->scalar_signature&&i<s->scalar_signature->param_count&&iface_source_mentions_type(cg,s->scalar_signature->params[i].type,0)) ||
            iface_source_actual_bears(cg,args.items[i]))return iface_source_error(args.items[i],"interface actual requires a complete exact interface formal; dependent or unknown carriers are unsupported");
    }
    return before==get_error_count();
}
/* Declared interface actuals bypass scalar contextualization, even where native
 * types already match. This adapter does not add any concrete boxing route. */
static LLVMValueRef iface_source_gen_arg(cg_t *cg,node_t *call,node_t *arg,usize_t index,type_info_t fallback) {
    symbol_t *s=iface_source_selected(cg,call,0);iface_type_t formal;
    if(iface_source_formal(cg,s,index,&formal))return gen_expr(cg,arg);
    return gen_scalar_as(cg,arg,fallback);
}

/* Exact local concrete-value transfer. This does not grant stored-value proof,
 * descriptor validity, or a new native formal conversion. Queries stay quiet. */
typedef enum { IfaceLocalCopyBoundary, IfaceLocalCopyExact, IfaceLocalCopyMismatch } iface_local_copy_t;
static iface_local_copy_t iface_source_local_copy(iface_source_fact_t source, iface_source_fact_t target) {
    if (target.status != IfaceSourceKnown || !target.contract.nominal ||
        target.contract.nominal->as.type_decl.decl_kind != TypeDeclStruct ||
        target.contract.type.is_pointer || target.dimensions) return IfaceLocalCopyBoundary;
    if (source.status == IfaceSourceExcluded || (source.origin & IfaceOriginGeneric))
        return IfaceLocalCopyMismatch;
    if (source.status == IfaceSourceUnknown) return IfaceLocalCopyBoundary;
    if (source.status != IfaceSourceKnown || source.contract.nominal != target.contract.nominal ||
        source.contract.type.is_pointer || source.dimensions || !(source.access & PtrRead))
        return IfaceLocalCopyMismatch;
    return IfaceLocalCopyExact;
}

/* I1b.3a is a negative-only comparison of immediate declarations. It never
 * consults stored-history origin/exclusion bits or grants local eligibility. */
typedef enum { IfaceImmediateNotApplicable, IfaceImmediateDeclaredNominalEqual,
               IfaceImmediateDefiniteNominalMismatch, IfaceImmediateNoEvidence } iface_immediate_status_t;
typedef struct { node_t *decl; boolean_t generic; } iface_immediate_nominal_t;

/* Resolve only original nominal metadata. Generic application provenance takes
 * precedence over its legacy mangled spelling, including spelling collisions.
 * Unknown/dependent arguments, aliases and repeated applications stay unknown. */
static iface_immediate_nominal_t iface_immediate_type_at(cg_t *cg,type_info_t raw,
                                                         node_t **seen,usize_t depth) {
    iface_immediate_nominal_t none={0};
    if(depth>=IfaceMaxMetadataDepth||raw.base!=TypeUser||!raw.user_name||raw.is_pointer||
       raw.ptr_depth||raw.elem_type||raw.fn_ptr_desc||raw.nullable||raw.written_nullable)return none;
    for(usize_t i=0;i<cg->generic_n;i++)
        if(!strcmp(raw.user_name,cg->generic_params[i]))return none;
    seen[depth]=Null;
    if(raw.generic_use){
        generic_use_t *use=raw.generic_use;
        node_t *d=iface_quiet_type(cg,use->base_name);
        if(!d||use->previous||!d->as.type_decl.type_param_count||
           d->as.type_decl.type_param_count!=use->count)return none;
        for(usize_t i=0;i<use->count;i++){
            type_info_t arg=use->args[i];
            /* Primitive and explicitly pointer-qualified primitive arguments
             * are resolved without imposing interface-signature restrictions. */
            if(arg.base>=TypeVoid&&arg.base<=TypeF64&&!arg.generic_use&&!arg.elem_type&&!arg.fn_ptr_desc)continue;
            if(arg.base!=TypeUser)return none;
            arg.is_pointer=False;arg.ptr_depth=0;arg.nullable=False;arg.written_nullable=False;
            if(!iface_immediate_type_at(cg,arg,seen,depth+1).decl)return none;
        }
        return (iface_immediate_nominal_t){d,True};
    }
    node_t *d=iface_quiet_type(cg,raw.user_name);if(!d)return none;
    if(d->as.type_decl.decl_kind==TypeDeclAlias){
        for(usize_t i=0;i<depth;i++)if(seen[i]==d)return none;
        seen[depth]=d;
        return iface_immediate_type_at(cg,d->as.type_decl.alias_type,seen,depth+1);
    }
    return (iface_immediate_nominal_t){d,d->as.type_decl.type_param_count!=0};
}
static iface_immediate_nominal_t iface_immediate_type(cg_t *cg,type_info_t raw) {
    node_t *seen[IfaceMaxMetadataDepth]={0};return iface_immediate_type_at(cg,raw,seen,0);
}
static node_t *iface_immediate_destination(cg_t *cg,type_info_t raw) {
    iface_immediate_nominal_t t=iface_immediate_type(cg,raw);
    return t.decl&&!t.generic&&t.decl->as.type_decl.decl_kind==TypeDeclStruct&&
        t.decl->as.type_decl.impl_iface_count?t.decl:Null;
}
static boolean_t iface_immediate_error(node_t *at,node_t *fallback,const char *route,const char *reason) {
    ast_source_loc_t loc=iface_node_loc(at);
    if(fallback){ast_source_loc_t parent=iface_node_loc(fallback);
        if(!loc.col){loc.col=parent.col;loc.len=parent.len;}
        if(!loc.file)loc.file=parent.file;
    }
    if(!loc.col)loc.col=1;if(!loc.len)loc.len=1;
    const char *saved=diag_get_file();if(loc.file)diag_set_file(loc.file);
    diag_begin_error("interface immediate %s: %s",route,reason);
    diag_span(SRC_LOC(loc.line,loc.col,loc.len),True,"immediate contract here");
    diag_finish();diag_set_file(saved);return False;
}
static iface_native_t *iface_immediate_native(cg_t *cg,LLVMValueRef function,boolean_t *ambiguous) {
    iface_native_t *found=Null;*ambiguous=False;
    for(iface_native_t *n=cg->iface_natives;n;n=n->next)if(n->function==function){
        if(found&&found!=n){*ambiguous=True;return Null;}found=n;
    }
    return found;
}
static boolean_t iface_immediate_ordinary(iface_native_t *native) {
    node_t *d=native?native->decl:Null;
    return d&&d->kind==NodeFnDecl&&!native->owner&&!native->instance&&!native->c_abi&&
        !d->is_c_extern&&!d->as.fn_decl.type_param_count&&!d->as.fn_decl.is_async&&
        !d->as.fn_decl.is_variadic&&!d->as.fn_decl.is_typed_variadic;
}
static boolean_t iface_immediate_native_arity(iface_native_t *native) {
    return native&&LLVMIsAFunction(native->function)&&
        LLVMGetFunctionCallConv(native->function)==LLVMCCallConv&&
        !LLVMIsFunctionVarArg(LLVMGlobalGetValueType(native->function))&&
        LLVMCountParams(native->function)==native->decl->as.fn_decl.params.count;
}
static LLVMTypeRef iface_immediate_storage(cg_t *cg,node_t *decl) {
    struct_reg_t *s=decl?find_struct(cg,decl->as.type_decl.name):Null;
    return s&&s->layout_decl==decl&&!s->is_union&&!s->is_interface?s->llvm_type:Null;
}
/* Exactly four source forms. Inferred identifiers consume an already retained
 * declaration-time contract, never re-query their initializer or prior stores. */
static iface_immediate_nominal_t iface_immediate_source(cg_t *cg,node_t *n,const char **mapping) {
    iface_immediate_nominal_t none={0};if(!n)return none;
    switch(n->kind){
    case NodeIdentExpr:{
        symbol_t *s=iface_source_lookup(cg,n->as.ident.name);
        iface_source_record_t *r=s?s->iface_source:Null;
        node_t *d=r?r->decl:Null;if(!d||d->kind!=NodeVarDecl)return none;
        if(d->as.var_decl.array_ndim||(d->as.var_decl.flags&VdeclArray))return none;
        if(d->as.var_decl.flags&VdeclLet){
            if(r->fact.dimensions||(r->fact.status!=IfaceSourceKnown&&r->fact.status!=IfaceSourceExcluded))return none;
            return iface_immediate_type(cg,r->fact.contract.type);
        }
        return iface_immediate_type(cg,d->as.var_decl.type);
    }
    case NodeDesigInit:return iface_immediate_type(cg,(type_info_t){.base=TypeUser,
        .user_name=n->as.desig_init.type_name,.generic_use=n->generic_use});
    case NodeCallExpr:{
        if(n->generic_use)return none;
        symbol_t *s=iface_source_lookup(cg,n->as.call.callee);
        if(!s||s->c_abi||s->stype.base==TypeFnPtr||s->stype.base==TypeClosure)return none;
        boolean_t ambiguous;iface_native_t *native=iface_immediate_native(cg,s->value,&ambiguous);
        if(ambiguous){*mapping="source call has ambiguous native identity";return none;}
        if(!iface_immediate_ordinary(native)||native->decl->as.fn_decl.is_method)return none;
        if(native->decl->as.fn_decl.return_count!=1)return none;
        if(s->requires_receiver||!iface_immediate_native_arity(native)||
           n->as.call.args.count!=native->decl->as.fn_decl.params.count||
           !s->scalar_signature||s->scalar_signature->param_count!=native->decl->as.fn_decl.params.count){
            *mapping="source call declaration/native mapping disagrees";return none;
        }
        iface_immediate_nominal_t result=iface_immediate_type(cg,native->decl->as.fn_decl.return_types[0]);
        if(result.decl&&!result.generic&&result.decl->as.type_decl.decl_kind==TypeDeclStruct){
            iface_immediate_nominal_t signature=iface_immediate_type(cg,s->scalar_signature->ret_type);
            LLVMTypeRef physical=iface_immediate_storage(cg,result.decl);
            if(!physical||signature.generic||signature.decl!=result.decl||
               LLVMGetAttributeCountAtIndex(native->function,LLVMAttributeReturnIndex)||
               LLVMGetReturnType(LLVMGlobalGetValueType(native->function))!=physical){
                *mapping="source call result/native correspondence disagrees";return none;
            }
        }
        return result;
    }
    default:return none;
    }
}
static iface_immediate_status_t iface_immediate_compare(cg_t *cg,node_t *source,node_t *target,const char **mapping) {
    if(!target)return IfaceImmediateNotApplicable;
    iface_immediate_nominal_t from=iface_immediate_source(cg,source,mapping);
    if(!from.decl)return IfaceImmediateNoEvidence;
    return !from.generic&&from.decl==target?IfaceImmediateDeclaredNominalEqual:IfaceImmediateDefiniteNominalMismatch;
}
/* Called with the copied symbol actually selected by gen_call, before argument
 * inference/evaluation. It neither reselects a fitting callable nor emits IR. */
static boolean_t iface_immediate_arguments(cg_t *cg,node_t *call,symbol_t *selected,boolean_t sibling) {
    if(!cg->iface_count||call->kind!=NodeCallExpr||call->generic_use||sibling||!selected||selected->c_abi||
       selected->stype.base==TypeFnPtr||selected->stype.base==TypeClosure)return True;
    boolean_t ambiguous;iface_native_t *native=iface_immediate_native(cg,selected->value,&ambiguous);
    if(native&&(!iface_immediate_ordinary(native)||native->decl->as.fn_decl.is_method))return True;
    node_t *d=native?native->decl:Null;boolean_t affected=False;
    if(d)for(usize_t i=0;i<d->as.fn_decl.params.count;i++){
        node_t *p=d->as.fn_decl.params.items[i];
        if(p&&p->kind==NodeVarDecl&&!p->as.var_decl.array_ndim&&!(p->as.var_decl.flags&VdeclArray)&&
           iface_immediate_destination(cg,p->as.var_decl.type))affected=True;
    }
    /* Missing/ambiguous registration cannot hide an affected retained native
     * formal. This is only a rejection trigger, never nominal evidence. */
    if(!d&&selected->scalar_signature)for(usize_t i=0;i<selected->scalar_signature->param_count;i++)
        if(iface_immediate_destination(cg,selected->scalar_signature->params[i].type))affected=True;
    if(!affected)return True;
    node_t *owner=Null;int original=iface_immediate_original(cg,call,&owner);
    if(!original)return True;
    if(original<0)return iface_immediate_error(call,Null,"argument","original call/body correspondence is incomplete");
    if(ambiguous||!d||selected->requires_receiver||!iface_immediate_native_arity(native)||
       !selected->scalar_signature||selected->scalar_signature->param_count!=d->as.fn_decl.params.count||
       call->as.call.args.count!=d->as.fn_decl.params.count)
        return iface_immediate_error(call,Null,"argument","selected declaration/native arity or identity disagrees");
    for(usize_t i=0;i<d->as.fn_decl.params.count;i++){
        node_t *p=d->as.fn_decl.params.items[i];
        if(!p||p->kind!=NodeVarDecl||p->as.var_decl.array_ndim||(p->as.var_decl.flags&VdeclArray))continue;
        node_t *target=iface_immediate_destination(cg,p->as.var_decl.type);if(!target)continue;
        LLVMTypeRef physical=iface_immediate_storage(cg,target);
        if(!physical||LLVMGetAttributeCountAtIndex(native->function,(LLVMAttributeIndex)i+1)||
           LLVMTypeOf(LLVMGetParam(native->function,(unsigned)i))!=physical||
           iface_immediate_destination(cg,selected->scalar_signature->params[i].type)!=target)
            return iface_immediate_error(call,Null,"argument","selected formal/native ordinal disagrees");
        const char *mapping=Null;
        iface_immediate_status_t status=iface_immediate_compare(cg,call->as.call.args.items[i],target,&mapping);
        if(mapping)return iface_immediate_error(call->as.call.args.items[i],call,"argument",mapping);
        if(status==IfaceImmediateDefiniteNominalMismatch)
            return iface_immediate_error(call->as.call.args.items[i],call,"argument","definite nominal mismatch");
    }
    return True;
}
static boolean_t iface_immediate_return(cg_t *cg,node_t *ret) {
    if(!cg->iface_count||ret->kind!=NodeRetStmt||ret->as.ret_stmt.values.count!=1||cg->cur_coro.active)return True;
    node_t *owner=Null;int original=iface_immediate_original(cg,ret,&owner);
    if(!original)return True;
    boolean_t ambiguous;iface_native_t *native=iface_immediate_native(cg,cg->current_fn,&ambiguous);
    /* Generated/generic/instance/C bodies are expressly outside this route. */
    if(native&&!iface_immediate_ordinary(native))return True;
    node_t *decl=native?native->decl:owner;
    if(!decl||decl->kind!=NodeFnDecl||decl->as.fn_decl.type_param_count||decl->as.fn_decl.is_async||
       decl->as.fn_decl.is_variadic||decl->as.fn_decl.is_typed_variadic||decl->is_c_extern)return True;
    node_t *target=decl->as.fn_decl.return_count==1?
        iface_immediate_destination(cg,decl->as.fn_decl.return_types[0]):Null;
    /* A stale/missing owner cannot turn a registered affected body into a
     * supported exclusion. Neither candidate repairs the broken mapping. */
    if(!target&&owner&&owner->kind==NodeFnDecl&&owner->as.fn_decl.return_count==1)
        target=iface_immediate_destination(cg,owner->as.fn_decl.return_types[0]);
    if(!target)return True;
    /* Compound/return AST nodes may have only a line. Their exact body owner
     * supplies the source file, never the importing caller's diagnostic file. */
    node_t location=*ret;
    if(!location.source_file)location.source_file=decl->source_file?decl->source_file:
        decl->as.fn_decl.return_count?decl->as.fn_decl.return_types[0].source_loc.file:Null;
    if(original<0||ambiguous||!native||native->decl!=owner||!owner||owner->kind!=NodeFnDecl||
       owner->as.fn_decl.return_count!=1||!iface_immediate_native_arity(native)||
       cg->current_return_count!=1||cg->current_return_types!=owner->as.fn_decl.return_types||
       !iface_immediate_storage(cg,target)||
       LLVMGetAttributeCountAtIndex(native->function,LLVMAttributeReturnIndex)||
       LLVMGetReturnType(LLVMGlobalGetValueType(native->function))!=iface_immediate_storage(cg,target))
        return iface_immediate_error(&location,Null,"return","original body/result native correspondence disagrees");
    const char *mapping=Null;
    iface_immediate_status_t status=iface_immediate_compare(cg,ret->as.ret_stmt.values.items[0],target,&mapping);
    if(mapping)return iface_immediate_error(ret->as.ret_stmt.values.items[0],&location,"return",mapping);
    return status!=IfaceImmediateDefiniteNominalMismatch||
        iface_immediate_error(ret->as.ret_stmt.values.items[0],&location,"return","definite nominal mismatch");
}
