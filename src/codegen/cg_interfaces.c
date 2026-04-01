/* cg_interfaces.c ─────────────────────────────────────────────────────────
 * Interface system: registration, vtables, dynamic dispatch.
 * Included by codegen.c (unity build).
 * ────────────────────────────────────────────────────────────────────────── */

/* Forward declare iface_reg_t and vtable_entry_t using the anonymous structs
   from cg_t — we reference them via cg->interfaces[i] and cg->vtables[i] */

/* ── forward declarations ── */
static struct_reg_t *find_struct_by_llvm_type(cg_t *cg, LLVMTypeRef ty);

/* ── interface registry helpers ── */

/* Find interface by name */
static usize_t *find_interface_idx(cg_t *cg, const char *name) {
    /* Linear search — return index via static local */
    static usize_t idx_buf;
    for (usize_t i = 0; i < cg->iface_count; i++) {
        if (strcmp(cg->interfaces[i].name, name) == 0) {
            idx_buf = i;
            return &idx_buf;
        }
    }
    return Null;
}

/* Get all methods of an interface (own + inherited, depth-first, deduped)
   Returns count stored in out_names/out_rets (up to max_out). */
static usize_t collect_all_iface_methods(cg_t *cg, const char *iface_name,
                                          char **out_names, type_info_t *out_rets,
                                          usize_t max_out) {
    usize_t *idx = find_interface_idx(cg, iface_name);
    if (!idx) return 0;
    usize_t ii = *idx;
    usize_t count = 0;
    /* first collect parent methods (inherited first) */
    for (usize_t p = 0; p < cg->interfaces[ii].parent_count && count < max_out; p++) {
        count += collect_all_iface_methods(cg, cg->interfaces[ii].parent_names[p],
                                            out_names + count,
                                            out_rets + count,
                                            max_out - count);
    }
    /* then own methods (dedup) */
    for (usize_t m = 0; m < cg->interfaces[ii].method_count && count < max_out; m++) {
        boolean_t found = False;
        for (usize_t j = 0; j < count; j++) {
            if (strcmp(out_names[j], cg->interfaces[ii].method_names[m]) == 0) {
                found = True; break;
            }
        }
        if (!found) {
            out_names[count] = cg->interfaces[ii].method_names[m];
            out_rets[count]  = cg->interfaces[ii].method_ret_types[m];
            count++;
        }
    }
    return count;
}

/* Register an interface from its AST decl node */
static void register_interface(cg_t *cg, node_t *decl) {
    if (cg->iface_count >= cg->iface_cap) {
        usize_t new_cap = cg->iface_cap < 8 ? 8 : cg->iface_cap * 2;
        if (cg->ifaces_heap.pointer == Null)
            cg->ifaces_heap = allocate(new_cap, sizeof(cg->interfaces[0]));
        else
            cg->ifaces_heap = reallocate(cg->ifaces_heap,
                                          new_cap * sizeof(cg->interfaces[0]));
        cg->interfaces = cg->ifaces_heap.pointer;
        cg->iface_cap = new_cap;
    }
    usize_t ii = cg->iface_count++;
    memset(&cg->interfaces[ii], 0, sizeof(cg->interfaces[0]));
    cg->interfaces[ii].name = decl->as.type_decl.name;

    /* store parent names */
    for (usize_t i = 0; i < decl->as.type_decl.impl_iface_count && i < 8; i++)
        cg->interfaces[ii].parent_names[cg->interfaces[ii].parent_count++] =
            decl->as.type_decl.impl_ifaces[i];

    /* collect own methods */
    for (usize_t i = 0; i < decl->as.type_decl.methods.count && cg->interfaces[ii].method_count < 32; i++) {
        node_t *m = decl->as.type_decl.methods.items[i];
        cg->interfaces[ii].method_names[cg->interfaces[ii].method_count] = m->as.fn_decl.name;
        if (m->as.fn_decl.return_count > 0)
            cg->interfaces[ii].method_ret_types[cg->interfaces[ii].method_count] =
                m->as.fn_decl.return_types[0];
        cg->interfaces[ii].method_count++;
    }

    /* store fat pointer type (already created in pass 0) */
    struct_reg_t *sr = find_struct(cg, decl->as.type_decl.name);
    if (sr) {
        cg->interfaces[ii].fat_ptr_type = sr->llvm_type;
    }
}

/* Find interface idx — public version returns pointer to the struct */
static usize_t find_interface_index(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->iface_count; i++)
        if (strcmp(cg->interfaces[i].name, name) == 0) return i;
    return (usize_t)-1;
}

/* Find vtable for (struct_name, iface_name) pair */
static usize_t find_vtable_idx(cg_t *cg, const char *struct_name, const char *iface_name) {
    for (usize_t i = 0; i < cg->vtable_count; i++) {
        if (strcmp(cg->vtables[i].struct_name, struct_name) == 0 &&
            strcmp(cg->vtables[i].iface_name, iface_name) == 0)
            return i;
    }
    return (usize_t)-1;
}

/* Create/return vtable for (struct_name, iface_name) pair.
   Requires: all functions are forward-declared.
   Builds a global constant struct of function pointers. */
static usize_t ensure_vtable(cg_t *cg, const char *struct_name, const char *iface_name) {
    /* check if already exists */
    usize_t existing = find_vtable_idx(cg, struct_name, iface_name);
    if (existing != (usize_t)-1) return existing;

    /* find the interface */
    usize_t ii = find_interface_index(cg, iface_name);
    if (ii == (usize_t)-1) {
        /* unknown interface — skip gracefully */
        return (usize_t)-1;
    }

    /* collect all interface methods */
    char *method_names[32];
    type_info_t method_rets[32];
    usize_t method_count = collect_all_iface_methods(cg, iface_name,
                                                      method_names, method_rets, 32);
    if (method_count == 0) return (usize_t)-1;

    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);

    /* vtable type: struct of N ptr (one per method) */
    heap_t vt_fields_heap = allocate(method_count, sizeof(LLVMTypeRef));
    LLVMTypeRef *vt_fields = vt_fields_heap.pointer;
    for (usize_t i = 0; i < method_count; i++) vt_fields[i] = ptr_t;

    char vt_type_name[512];
    snprintf(vt_type_name, sizeof(vt_type_name), "__%s__%s__vtable_t",
             struct_name, iface_name);
    LLVMTypeRef vt_type = LLVMStructCreateNamed(cg->ctx, vt_type_name);
    LLVMStructSetBody(vt_type, vt_fields, (unsigned)method_count, 0);
    deallocate(vt_fields_heap);

    /* find function pointer for each method */
    struct_reg_t *sr = find_struct(cg, struct_name);
    heap_t fn_ptrs_heap = allocate(method_count, sizeof(LLVMValueRef));
    LLVMValueRef *fn_ptrs = fn_ptrs_heap.pointer;

    for (usize_t i = 0; i < method_count; i++) {
        char mangled[512];
        /* try iface-qualified name first: "struct.iface.method" */
        snprintf(mangled, sizeof(mangled), "%s.%s.%s",
                 struct_name, iface_name, method_names[i]);
        symbol_t *fn_sym = cg_lookup(cg, mangled);

        if (!fn_sym && sr && sr->mod_prefix && sr->mod_prefix[0]) {
            snprintf(mangled, sizeof(mangled), "%s__%s__%s__%s",
                     sr->mod_prefix, struct_name, iface_name, method_names[i]);
            fn_sym = cg_lookup(cg, mangled);
        }
        if (!fn_sym) {
            /* try plain struct.method */
            if (sr && sr->mod_prefix && sr->mod_prefix[0]) {
                snprintf(mangled, sizeof(mangled), "%s__%s__%s",
                         sr->mod_prefix, struct_name, method_names[i]);
            } else {
                snprintf(mangled, sizeof(mangled), "%s.%s",
                         struct_name, method_names[i]);
            }
            fn_sym = cg_lookup(cg, mangled);
        }
        fn_ptrs[i] = fn_sym ? fn_sym->value : LLVMConstNull(ptr_t);
    }

    /* create global vtable constant */
    char vt_global_name[512];
    snprintf(vt_global_name, sizeof(vt_global_name), "__%s__%s__vtable",
             struct_name, iface_name);
    LLVMValueRef vt_global = LLVMAddGlobal(cg->module, vt_type, vt_global_name);
    LLVMSetLinkage(vt_global, LLVMInternalLinkage);
    LLVMValueRef vt_init = LLVMConstNamedStruct(vt_type, fn_ptrs, (unsigned)method_count);
    LLVMSetInitializer(vt_global, vt_init);
    LLVMSetGlobalConstant(vt_global, 1);
    deallocate(fn_ptrs_heap);

    /* register in vtable registry */
    if (cg->vtable_count >= cg->vtable_cap) {
        usize_t new_cap = cg->vtable_cap < 8 ? 8 : cg->vtable_cap * 2;
        if (cg->vtables_heap.pointer == Null)
            cg->vtables_heap = allocate(new_cap, sizeof(cg->vtables[0]));
        else
            cg->vtables_heap = reallocate(cg->vtables_heap,
                                           new_cap * sizeof(cg->vtables[0]));
        cg->vtables = cg->vtables_heap.pointer;
        cg->vtable_cap = new_cap;
    }
    usize_t vi = cg->vtable_count++;
    cg->vtables[vi].struct_name = (char *)struct_name;
    cg->vtables[vi].iface_name  = (char *)iface_name;
    cg->vtables[vi].vtable_global = vt_global;
    cg->vtables[vi].vtable_type   = vt_type;
    return vi;
}

/* Construct a fat pointer { obj_ptr, vtable_ptr } from a struct pointer and vtable.
   Returns the fat pointer value (struct value, not pointer). */
static LLVMValueRef construct_fat_ptr(cg_t *cg, const char *struct_name,
                                        const char *iface_name,
                                        LLVMValueRef struct_ptr) {
    usize_t ii = find_interface_index(cg, iface_name);
    if (ii == (usize_t)-1) return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));

    usize_t vi = find_vtable_idx(cg, struct_name, iface_name);
    if (vi == (usize_t)-1) {
        vi = ensure_vtable(cg, struct_name, iface_name);
        if (vi == (usize_t)-1)
            return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }

    LLVMTypeRef fat_type = cg->interfaces[ii].fat_ptr_type;
    if (!fat_type) return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));

    LLVMValueRef fat_alloca = alloc_in_entry(cg, fat_type, "fat_ptr");
    /* field 0: obj_ptr */
    LLVMValueRef obj_gep = LLVMBuildStructGEP2(cg->builder, fat_type, fat_alloca, 0, "obj_f");
    LLVMBuildStore(cg->builder, struct_ptr, obj_gep);
    /* field 1: vtable_ptr */
    LLVMValueRef vt_gep = LLVMBuildStructGEP2(cg->builder, fat_type, fat_alloca, 1, "vt_f");
    LLVMBuildStore(cg->builder, cg->vtables[vi].vtable_global, vt_gep);
    return LLVMBuildLoad2(cg->builder, fat_type, fat_alloca, "fat_val");
}

/* Generate a method call via vtable dispatch (for interface-typed variables).
   obj_sym: the symbol holding the fat-pointer value (alloca of { ptr, ptr }). */
static LLVMValueRef gen_iface_method_call(cg_t *cg, node_t *node,
                                            symbol_t *obj_sym, usize_t iface_idx) {
    char *method = node->as.method_call.method;
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);

    /* collect all interface methods to find the method index */
    char *all_methods[32];
    type_info_t all_rets[32];
    usize_t total = collect_all_iface_methods(cg,
                        cg->interfaces[iface_idx].name,
                        all_methods, all_rets, 32);

    usize_t method_idx = (usize_t)-1;
    for (usize_t i = 0; i < total; i++) {
        if (strcmp(all_methods[i], method) == 0) { method_idx = i; break; }
    }
    if (method_idx == (usize_t)-1) {
        diag_begin_error("interface '%s' has no method '%s'",
                         cg->interfaces[iface_idx].name, method);
        diag_span(DIAG_NODE(node), True, "");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    /* load the fat pointer { obj_ptr, vtable_ptr } from the alloca */
    LLVMTypeRef fat_type = cg->interfaces[iface_idx].fat_ptr_type;
    LLVMValueRef fat_val = LLVMBuildLoad2(cg->builder, fat_type, obj_sym->value, "fat");

    /* extract obj_ptr (field 0) and vtable_ptr (field 1) */
    LLVMValueRef obj_ptr    = LLVMBuildExtractValue(cg->builder, fat_val, 0, "iface_obj");
    LLVMValueRef vtable_ptr = LLVMBuildExtractValue(cg->builder, fat_val, 1, "iface_vt");

    /* GEP into the vtable to get the function pointer slot */
    LLVMValueRef method_idx_val = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                                                (unsigned long long)method_idx, 0);
    LLVMValueRef fn_ptr_ptr = LLVMBuildGEP2(cg->builder, ptr_t, vtable_ptr,
                                              &method_idx_val, 1, "fn_slot");
    LLVMValueRef fn_ptr = LLVMBuildLoad2(cg->builder, ptr_t, fn_ptr_ptr, "fn_ptr");

    /* build args: obj_ptr + user args */
    usize_t user_argc = node->as.method_call.args.count;
    usize_t argc = user_argc + 1;
    heap_t args_heap = allocate(argc, sizeof(LLVMValueRef));
    LLVMValueRef *args = args_heap.pointer;
    args[0] = obj_ptr;
    for (usize_t i = 0; i < user_argc; i++)
        args[i + 1] = gen_expr(cg, node->as.method_call.args.items[i]);

    /* build function type using the interface method's return type */
    type_info_t ret_ti = (method_idx < total) ? all_rets[method_idx] : NO_TYPE;
    LLVMTypeRef ret_llvm = (ret_ti.base == TypeVoid && !ret_ti.is_pointer)
        ? LLVMVoidTypeInContext(cg->ctx)
        : get_llvm_type(cg, ret_ti);

    heap_t param_types_heap = allocate(argc, sizeof(LLVMTypeRef));
    LLVMTypeRef *param_types = param_types_heap.pointer;
    param_types[0] = ptr_t; /* this */
    for (usize_t i = 1; i < argc; i++)
        param_types[i] = LLVMTypeOf(args[i]);
    LLVMTypeRef fn_type = LLVMFunctionType(ret_llvm, param_types, (unsigned)argc, 0);
    deallocate(param_types_heap);

    LLVMValueRef result = LLVMBuildCall2(cg->builder, fn_type, fn_ptr,
                                          args, (unsigned)argc, "");
    deallocate(args_heap);
    return result;
}
