/* ── cg_generics.c ──────────────────────────────────────────────────────────
 * Lazy monomorphization for @comptime[T] generic structs and functions.
 * Included by codegen.c (unity build).
 * ────────────────────────────────────────────────────────────────────────── */

/* Apply active generic substitutions to a type_info_t.
 * If ti is a TypeUser matching a generic param, return the concrete type_info_t.
 * Preserves is_pointer / ptr_perm from the original. */
static type_info_t subst_type_info(cg_t *cg, type_info_t ti) {
    if (ti.base != TypeUser || !ti.user_name || cg->generic_n == 0) return ti;
    for (usize_t i = 0; i < cg->generic_n; i++) {
        if (strcmp(ti.user_name, cg->generic_params[i]) == 0) {
            type_info_t c = cg->generic_concs[i];
            /* keep pointer wrapping from original */
            c.is_pointer = ti.is_pointer;
            c.ptr_perm   = ti.ptr_perm;
            return c;
        }
    }
    /* if user_name is a mangled generic (e.g. "pair_t_G_K_G_V"), substitute */
    const char *subst = cg_subst_name(cg, ti.user_name);
    if (subst != ti.user_name) {
        ti.user_name = (char *)subst;
    }
    return ti;
}

/* Convert a short type name (from comptime_type_name / name mangling)
 * back to a type_info_t for generic substitution.
 * User types (struct names, other generics) become TypeUser. */
static type_info_t type_name_to_ti(const char *name) {
    type_info_t ti;
    memset(&ti, 0, sizeof(ti));
    if (!name) return ti;
    if (strcmp(name, "i8")   == 0) { ti.base = TypeI8;   return ti; }
    if (strcmp(name, "u8")   == 0) { ti.base = TypeU8;   return ti; }
    if (strcmp(name, "i16")  == 0) { ti.base = TypeI16;  return ti; }
    if (strcmp(name, "u16")  == 0) { ti.base = TypeU16;  return ti; }
    if (strcmp(name, "i32")  == 0) { ti.base = TypeI32;  return ti; }
    if (strcmp(name, "u32")  == 0) { ti.base = TypeU32;  return ti; }
    if (strcmp(name, "i64")  == 0) { ti.base = TypeI64;  return ti; }
    if (strcmp(name, "u64")  == 0) { ti.base = TypeU64;  return ti; }
    if (strcmp(name, "f32")  == 0) { ti.base = TypeF32;  return ti; }
    if (strcmp(name, "f64")  == 0) { ti.base = TypeF64;  return ti; }
    if (strcmp(name, "bool") == 0) { ti.base = TypeBool; return ti; }
    if (strcmp(name, "void") == 0) { ti.base = TypeVoid; return ti; }
    /* user / compound type — treat as TypeUser */
    ti.base = TypeUser;
    ti.user_name = (char *)name;
    return ti;
}

/* Parse a mangled generic name into template name + concrete type-arg strings.
 *   "arr_t_G_i32"         → tmpl="arr_t",   args=["i32"]
 *   "pair_t_G_i32_G_f32"  → tmpl="pair_t",  args=["i32","f32"]
 *
 * Returns True on success. template_out must be >= 256 bytes; each args_out[i]
 * must be >= 256 bytes (to hold compound names like "arr_t_G_i32"). */
static boolean_t parse_mangled_generic(const char *mangled,
                                        char *template_out, usize_t tmpl_buf,
                                        char args_out[8][256], usize_t *n_args_out) {
    const char *g = strstr(mangled, "_G_");
    if (!g) return False;

    usize_t tlen = (usize_t)(g - mangled);
    if (tlen == 0 || tlen >= tmpl_buf) return False;
    memcpy(template_out, mangled, tlen);
    template_out[tlen] = '\0';

    *n_args_out = 0;
    const char *p = g + 3; /* skip first "_G_" */
    while (p && *p && *n_args_out < 8) {
        const char *next = strstr(p, "_G_");
        usize_t alen = next ? (usize_t)(next - p) : strlen(p);
        if (alen == 0 || alen >= 256) break;
        memcpy(args_out[*n_args_out], p, alen);
        args_out[*n_args_out][alen] = '\0';
        (*n_args_out)++;
        p = next ? next + 3 : Null;
    }
    return *n_args_out > 0;
}

/* Find the template AST decl node by name. */
static node_t *find_generic_template_decl(cg_t *cg, const char *tmpl_name) {
    for (usize_t i = 0; i < cg->generic_template_count; i++) {
        if (strcmp(cg->generic_templates[i], tmpl_name) == 0)
            return cg->generic_template_decls[i];
    }
    return Null;
}

/* Forward-declare + generate body for one method in an instantiation.
 * Substitution context (generic_params/concs/n/tmpl_name/inst_name) must
 * already be set in cg before calling. */
static void instantiate_method(cg_t *cg, node_t *method_decl,
                                const char *inst_struct_name,
                                const char *module_name) {
    const char *mname = method_decl->as.fn_decl.name;
    char fn_name[512];
    mangle_method(module_name, inst_struct_name, mname, fn_name, sizeof(fn_name));

    /* skip if already declared (idempotent) */
    if (cg_lookup(cg, fn_name)) return;

    /* "new" is a static constructor — no implicit this */
    boolean_t is_new      = strcmp(mname, "new") == 0;
    boolean_t is_instance = !is_new;
    usize_t   pc          = method_decl->as.fn_decl.params.count;
    usize_t   total_params = pc + (is_instance ? 1 : 0);

    /* build parameter types with substitution */
    heap_t ptypes_heap = NullHeap;
    LLVMTypeRef *ptypes = Null;
    if (total_params > 0) {
        ptypes_heap = allocate(total_params, sizeof(LLVMTypeRef));
        ptypes = ptypes_heap.pointer;
        if (is_instance) {
            ptypes[0] = LLVMPointerTypeInContext(cg->ctx, 0);
        }
        for (usize_t j = 0; j < pc; j++) {
            type_info_t pti = resolve_alias(cg,
                method_decl->as.fn_decl.params.items[j]->as.var_decl.type);
            ptypes[j + (is_instance ? 1 : 0)] = get_llvm_type(cg, pti);
        }
    }

    /* return type — may reference T; handle multi-return with aggregate struct */
    usize_t ret_count = method_decl->as.fn_decl.return_count;
    type_info_t rti = resolve_alias(cg, method_decl->as.fn_decl.return_types[0]);
    LLVMTypeRef ret_type;
    if (ret_count > 1) {
        heap_t rt_heap = allocate(ret_count, sizeof(LLVMTypeRef));
        LLVMTypeRef *rt_fields = rt_heap.pointer;
        for (usize_t j = 0; j < ret_count; j++) {
            type_info_t rtj = resolve_alias(cg, method_decl->as.fn_decl.return_types[j]);
            rt_fields[j] = get_llvm_type(cg, rtj);
        }
        ret_type = LLVMStructTypeInContext(cg->ctx, rt_fields, (unsigned)ret_count, 0);
        deallocate(rt_heap);
    } else {
        ret_type = get_llvm_type(cg, rti);
    }

    LLVMTypeRef fn_type = LLVMFunctionType(ret_type, ptypes,
                                            (unsigned)total_params, 0);
    LLVMValueRef fn = LLVMAddFunction(cg->module, fn_name, fn_type);
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    type_info_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    symtab_add(&cg->globals,
               ast_strdup(fn_name, strlen(fn_name)), fn, Null, dummy, False);
    if (total_params > 0) deallocate(ptypes_heap);

    /* register as destructor if rem */
    if (strcmp(mname, "rem") == 0) {
        struct_reg_t *sr2 = find_struct(cg, inst_struct_name);
        if (sr2) sr2->destructor = fn;
    }

    /* generate body (skip if no body — extern / abstract) */
    if (!method_decl->as.fn_decl.body) return;

    /* save and set module prefix so intra-module globals resolve correctly
     * (e.g. MAP_EMPTY → map__MAP_EMPTY inside a map module method body) */
    char saved_module_prefix[512];
    memcpy(saved_module_prefix, cg->current_module_prefix, sizeof(saved_module_prefix));
    mangle_module_prefix(module_name ? module_name : "", cg->current_module_prefix,
                         sizeof(cg->current_module_prefix));

    cg->current_fn           = fn;
    cg->current_struct_name  = (char *)inst_struct_name;
    cg->current_fn_linkage   = LinkageInternal;
    cg->locals.count         = 0;
    cg->dtor_depth           = 0;

    LLVMBasicBlockRef entry =
        LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    if (is_instance) {
        LLVMTypeRef this_type = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef this_alloca = LLVMBuildAlloca(cg->builder, this_type, "this");
        LLVMBuildStore(cg->builder, LLVMGetParam(fn, 0), this_alloca);
        type_info_t this_ti;
        memset(&this_ti, 0, sizeof(this_ti));
        this_ti.base       = TypeUser;
        this_ti.user_name  = (char *)inst_struct_name;
        this_ti.is_pointer = True;
        this_ti.ptr_perm   = PtrReadWrite;
        symtab_add(&cg->locals, "this", this_alloca, this_type, this_ti, False);
    }

    for (usize_t j = 0; j < pc; j++) {
        node_t *param  = method_decl->as.fn_decl.params.items[j];
        type_info_t pti = resolve_alias(cg, param->as.var_decl.type);
        LLVMTypeRef ptype = get_llvm_type(cg, pti);
        LLVMValueRef alloca_val =
            LLVMBuildAlloca(cg->builder, ptype, param->as.var_decl.name);
        LLVMBuildStore(cg->builder,
                       LLVMGetParam(fn, (unsigned)(j + (is_instance ? 1 : 0))),
                       alloca_val);
        symtab_add(&cg->locals, param->as.var_decl.name,
                   alloca_val, ptype, pti, False);
    }

    gen_block(cg, method_decl->as.fn_decl.body);

    LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
    if (!LLVMGetBasicBlockTerminator(cur_bb)) {
        if (ret_count <= 1 && rti.base == TypeVoid && !rti.is_pointer)
            LLVMBuildRetVoid(cg->builder);
        else
            LLVMBuildRet(cg->builder, LLVMConstNull(ret_type));
    }

    cg->current_fn          = Null;
    cg->current_struct_name = Null;
    memcpy(cg->current_module_prefix, saved_module_prefix, sizeof(cg->current_module_prefix));
}

/* Core generic instantiation.  Called lazily from get_llvm_base_type when an
 * unknown TypeUser with "_G_" in the name is needed.
 * Idempotent: returns immediately if the struct is already registered. */
static void try_instantiate_generic(cg_t *cg, const char *mangled_name) {
    /* already instantiated? */
    if (find_struct(cg, mangled_name)) return;

    /* parse: "arr_t_G_i32" → tmpl="arr_t", args=["i32"] */
    char tmpl_name[256];
    char arg_names[8][256];
    usize_t n_args = 0;
    if (!parse_mangled_generic(mangled_name, tmpl_name, sizeof(tmpl_name),
                               arg_names, &n_args))
        return;

    /* find template AST node */
    node_t *tmpl = find_generic_template_decl(cg, tmpl_name);
    if (!tmpl) return;
    if (n_args != tmpl->as.type_decl.type_param_count) return; /* arity mismatch */

    /* ── save & set substitution context ── */
    char      *saved_params[8];
    type_info_t saved_concs[8];
    usize_t    saved_n    = cg->generic_n;
    char      *saved_tmpl = cg->generic_tmpl_name;
    char      *saved_inst = cg->generic_inst_name;
    memcpy(saved_params, cg->generic_params, sizeof(saved_params));
    memcpy(saved_concs,  cg->generic_concs,  sizeof(saved_concs));

    /* ── save outer function-generation context (we may be called lazily
     *     from inside an active gen_block / gen_local_var etc.) ── */
    LLVMValueRef      saved_fn          = cg->current_fn;
    char             *saved_struct_name = cg->current_struct_name;
    linkage_t         saved_fn_linkage  = cg->current_fn_linkage;
    usize_t           saved_dtor_depth  = cg->dtor_depth;
    dtor_scope_t     *saved_dtor_stack  = cg->dtor_stack;
    heap_t            saved_dtor_heap   = cg->dtor_stack_heap;
    usize_t           saved_dtor_cap    = cg->dtor_cap;
    symtab_t          saved_locals      = cg->locals;
    LLVMBasicBlockRef saved_bb          = cg->builder
                                         ? LLVMGetInsertBlock(cg->builder) : Null;
    /* give the instantiation a fresh local-var scope and dtor stack so it
     * doesn't clobber the outer function's state */
    memset(&cg->locals, 0, sizeof(cg->locals));
    cg->dtor_stack      = Null;
    cg->dtor_stack_heap = NullHeap;
    cg->dtor_cap        = 0;
    cg->dtor_depth      = 0;

    /* intern the mangled name so the pointer stays valid after this stack frame */
    char *inst_name_intern = ast_strdup(mangled_name, strlen(mangled_name));

    cg->generic_n         = n_args;
    cg->generic_tmpl_name = tmpl_name; /* stack-local but valid during this call */
    cg->generic_inst_name = inst_name_intern;
    for (usize_t i = 0; i < n_args; i++) {
        cg->generic_params[i] = tmpl->as.type_decl.type_params[i];
        cg->generic_concs[i]  = type_name_to_ti(arg_names[i]);
        /* if this arg is itself a mangled generic, recurse first */
        if (cg->generic_concs[i].base == TypeUser && cg->generic_concs[i].user_name
                && strstr(cg->generic_concs[i].user_name, "_G_")) {
            try_instantiate_generic(cg, cg->generic_concs[i].user_name);
        }
    }

    /* ── register LLVM named struct ── */
    LLVMTypeRef stype = LLVMStructCreateNamed(cg->ctx, inst_name_intern);
    register_struct(cg, inst_name_intern, stype, False);
    struct_reg_t *sr = find_struct(cg, inst_name_intern);
    if (!sr) goto restore;

    /* set mod_prefix so gen_method_call can build the correct mangled name */
    if (tmpl->module_name && tmpl->module_name[0]) {
        char pfx[512];
        mangle_module_prefix(tmpl->module_name, pfx, sizeof(pfx));
        sr->mod_prefix = ast_strdup(pfx, strlen(pfx));
    }

    /* ── copy @comptime: fields from template (values are constants) ── */
    {
        usize_t fc = tmpl->as.type_decl.fields.count;
        for (usize_t j = 0; j < fc; j++) {
            node_t *field = tmpl->as.type_decl.fields.items[j];
            if (!(field->as.var_decl.flags & VdeclComptimeField)) continue;
            if (sr->ct_field_count >= 16) break;
            long val = 0;
            if (field->as.var_decl.init
                    && field->as.var_decl.init->kind == NodeIntLitExpr)
                val = field->as.var_decl.init->as.int_lit.value;
            sr->ct_fields[sr->ct_field_count].name  = field->as.var_decl.name;
            sr->ct_fields[sr->ct_field_count].value = val;
            sr->ct_field_count++;
        }
    }

    /* ── lay out LLVM struct body ── */
    {
        usize_t fc = tmpl->as.type_decl.fields.count;
        /* count non-comptime runtime fields */
        usize_t llvm_fc = 0;
        for (usize_t j = 0; j < fc; j++) {
            node_t *field = tmpl->as.type_decl.fields.items[j];
            if (field->as.var_decl.flags & VdeclComptimeField) continue;
            llvm_fc++;
        }

        heap_t ft_heap = NullHeap;
        LLVMTypeRef *field_types = Null;
        if (llvm_fc > 0) {
            ft_heap = allocate(llvm_fc, sizeof(LLVMTypeRef));
            field_types = ft_heap.pointer;
        }

        usize_t fi = 0;
        for (usize_t j = 0; j < fc; j++) {
            node_t *field = tmpl->as.type_decl.fields.items[j];
            if (field->as.var_decl.flags & VdeclComptimeField) continue;
            type_info_t fti = resolve_alias(cg, field->as.var_decl.type);
            /* apply generic substitution so concrete types are stored in the registry */
            fti = subst_type_info(cg, fti);
            field_types[fi] = get_llvm_type(cg, fti);
            struct_add_field(sr, field->as.var_decl.name, fti, fi,
                             field->as.var_decl.linkage,
                             field->as.var_decl.storage);
            fi++;
        }
        LLVMStructSetBody(stype, field_types, (unsigned)llvm_fc, 0);
        if (llvm_fc > 0) deallocate(ft_heap);
    }

    /* ── instantiate inline methods (defined inside the struct body) ── */
    for (usize_t m = 0; m < tmpl->as.type_decl.methods.count; m++) {
        node_t *method = tmpl->as.type_decl.methods.items[m];
        instantiate_method(cg, method, inst_name_intern, tmpl->module_name);
    }

    /* ── instantiate standalone methods (declared outside the struct) ──
     * These are top-level NodeFnDecl with struct_name == tmpl_name,
     * including both `fn @comptime[T] tmpl.method(...)` and
     * plain `fn tmpl.method(void)` that belong to the template. */
    if (cg->root_ast) {
        node_t *root = cg->root_ast;
        for (usize_t i = 0; i < root->as.module.decls.count; i++) {
            node_t *d = root->as.module.decls.items[i];
            if (d->kind != NodeFnDecl) continue;
            if (!d->as.fn_decl.is_method) continue;
            if (!d->as.fn_decl.struct_name) continue;
            if (strcmp(d->as.fn_decl.struct_name, tmpl_name) != 0) continue;
            /* methods whose struct belongs to the template — instantiate */
            instantiate_method(cg, d, inst_name_intern, d->module_name);
        }
    }

restore:
    /* ── restore substitution context ── */
    cg->generic_n         = saved_n;
    cg->generic_tmpl_name = saved_tmpl;
    cg->generic_inst_name = saved_inst;
    memcpy(cg->generic_params, saved_params, sizeof(saved_params));
    memcpy(cg->generic_concs,  saved_concs,  sizeof(saved_concs));

    /* ── restore outer function-generation context ── */
    symtab_free(&cg->locals);           /* free any entries the instantiation used */
    cg->locals              = saved_locals;
    /* free any dtor_stack_heap the instantiation allocated */
    if (cg->dtor_stack_heap.pointer) deallocate(cg->dtor_stack_heap);
    cg->dtor_stack          = saved_dtor_stack;
    cg->dtor_stack_heap     = saved_dtor_heap;
    cg->dtor_cap            = saved_dtor_cap;
    cg->dtor_depth          = saved_dtor_depth;
    cg->current_fn          = saved_fn;
    cg->current_struct_name = saved_struct_name;
    cg->current_fn_linkage  = saved_fn_linkage;
    if (saved_bb && cg->builder)
        LLVMPositionBuilderAtEnd(cg->builder, saved_bb);
}
