/* cg_interfaces.c ─────────────────────────────────────────────────────────
 * Interface system: registration, vtables, dynamic dispatch.
 * Included by codegen.c (unity build).
 * ────────────────────────────────────────────────────────────────────────── */

/* ── forward declarations ── */
static struct_reg_t *find_struct_by_llvm_type(cg_t *cg, LLVMTypeRef ty);

static usize_t find_interface_index(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->iface_count; i++)
        if (strcmp(cg->interfaces[i].name, name) == 0) return i;
    return (usize_t)-1;
}

static ast_source_loc_t iface_node_loc(node_t *node) {
    return (ast_source_loc_t){node->source_file, node->line, node->col, node->len};
}

/* Diagnostic file switches are restored; imported declarations must not borrow
 * the entry module's path. Metadata has its own arena-owned source file. */
static void iface_error(ast_source_loc_t loc, const char *message,
                         const char *name) {
    const char *saved = diag_get_file();
    if (loc.file) diag_set_file(loc.file);
    diag_begin_error("interface declaration: %s '%s'", message, name ? name : "");
    diag_span(SRC_LOC(loc.line, loc.col, loc.len), True, "declared here");
    diag_finish();
    diag_set_file(saved);
}

static void free_interface_registry(cg_t *cg) {
    for (usize_t i = 0; i < cg->iface_count; i++)
        for (usize_t j = 0; j < cg->interfaces[i].group_count; j++)
            if (cg->interfaces[i].groups[j].origins_heap.pointer)
                deallocate(cg->interfaces[i].groups[j].origins_heap);
    if (cg->ifaces_heap.pointer) deallocate(cg->ifaces_heap);
    cg->interfaces = Null;
    cg->iface_count = cg->iface_cap = 0;
    cg->ifaces_heap = NullHeap;
}

static void register_interface(cg_t *cg, node_t *decl) {
    if (cg->iface_count == cg->iface_cap) {
        if (cg->iface_cap > SIZE_MAX / sizeof(iface_reg_t) / 2) {
            iface_error(iface_node_loc(decl), "registry allocation limit exceeded for", decl->as.type_decl.name);
            return;
        }
        usize_t cap = cg->iface_cap ? cg->iface_cap * 2 : 8;
        cg->ifaces_heap = cg->ifaces_heap.pointer
            ? reallocate(cg->ifaces_heap, cap * sizeof(iface_reg_t))
            : allocate(cap, sizeof(iface_reg_t));
        cg->interfaces = cg->ifaces_heap.pointer;
        cg->iface_cap = cap;
    }
    iface_reg_t *iface = &cg->interfaces[cg->iface_count++];
    memset(iface, 0, sizeof(*iface));
    iface->name = decl->as.type_decl.name;
    iface->decl = decl;
    iface->parent_count = decl->as.type_decl.impl_iface_count;
}

/* Same method spelling is a group, NOT signature equality. A diamond repeats
 * the same originating declaration only once. Distinct same-name declarations
 * remain present, even when they will conflict under I1's signature rules. */
static boolean_t iface_add_origin(iface_reg_t *iface, node_t *method) {
    usize_t slot = 0;
    while (slot < iface->group_count &&
           strcmp(iface->groups[slot].name, method->as.fn_decl.name)) slot++;
    if (slot == iface->group_count) {
        if (slot == IfaceMaxMethods) {
            iface_error(iface_node_loc(iface->decl),
                "maximum of 32 unique flattened methods exceeded for", iface->name);
            return False;
        }
        iface->group_count++;
        iface->groups[slot].name = method->as.fn_decl.name;
        iface->groups[slot].signature_pending = True;
    }
    iface_method_group_t *group = &iface->groups[slot];
    for (usize_t i = 0; i < group->origin_count; i++)
        if (group->origins[i] == method) return True;
    if (group->origin_count == group->origin_cap) {
        if (group->origin_cap > SIZE_MAX / sizeof(node_t *) / 2) {
            iface_error(iface_node_loc(iface->decl), "method-origin allocation limit exceeded for", iface->name);
            return False;
        }
        usize_t cap = group->origin_cap ? group->origin_cap * 2 : 2;
        group->origins_heap = group->origins_heap.pointer
            ? reallocate(group->origins_heap, cap * sizeof(node_t *))
            : allocate(cap, sizeof(node_t *));
        group->origins = group->origins_heap.pointer;
        group->origin_cap = cap;
    }
    group->origins[group->origin_count++] = method;
    return True;
}

static boolean_t flatten_interface(cg_t *cg, usize_t index, usize_t depth) {
    iface_reg_t *iface = &cg->interfaces[index];
    if (iface->graph_state == IfaceGraphReady) return True;
    if (iface->graph_state == IfaceInvalid) return False;
    if (depth > IfaceMaxDepth) {
        iface_error(iface_node_loc(iface->decl),
            "maximum inheritance path of 64 interfaces exceeded at", iface->name);
        iface->graph_state = IfaceInvalid;
        return False;
    }
    iface->graph_state = IfaceVisiting;
    iface->path_depth = 1;
    for (usize_t p = 0; p < iface->parent_count; p++) {
        usize_t pi = iface->parent_indices[p];
        iface_reg_t *parent = &cg->interfaces[pi];
        if (parent->graph_state == IfaceVisiting) {
            iface_error(iface->decl->as.type_decl.impl_iface_locs[p],
                         "inheritance cycle through parent", parent->name);
            iface->graph_state = IfaceInvalid;
            return False;
        }
        if (!flatten_interface(cg, pi, depth + 1)) {
            iface->graph_state = IfaceInvalid;
            return False;
        }
        usize_t height = parent->path_depth + 1;
        if (height > IfaceMaxDepth) {
            iface_error(iface->decl->as.type_decl.impl_iface_locs[p],
                "maximum inheritance path of 64 interfaces exceeded through", parent->name);
            iface->graph_state = IfaceInvalid;
            return False;
        }
        if (height > iface->path_depth) iface->path_depth = height;
        for (usize_t g = 0; g < parent->group_count; g++)
            for (usize_t o = 0; o < parent->groups[g].origin_count; o++)
                if (!iface_add_origin(iface, parent->groups[g].origins[o])) {
                    iface->graph_state = IfaceInvalid;
                    return False;
                }
    }
    node_list_t *methods = &iface->decl->as.type_decl.methods;
    for (usize_t m = 0; m < methods->count; m++)
        if (!iface_add_origin(iface, methods->items[m])) {
            iface->graph_state = IfaceInvalid;
            return False;
        }
    iface->graph_state = IfaceGraphReady;
    return True;
}

static void iface_check_constraints(generic_param_meta_t *meta, usize_t count) {
    for (usize_t i = 0; i < count; i++)
        if (meta[i].constraints)
            iface_error(meta[i].constraints->source_loc,
                "generic interface constraints are unsupported for", meta[i].constraints->base_name);
}

/* Metadata inspection follows aliases without asserting canonical equality.
 * The bound is explicit because aliases can add nesting beyond parser nesting. */
enum { IfaceMaxMetadataDepth = 256 };
static void iface_check_return_metadata(cg_t *cg, type_info_t type,
                                        boolean_t is_return, node_t **aliases,
                                        usize_t alias_count, usize_t depth) {
    if (depth > IfaceMaxMetadataDepth) {
        iface_error(type.source_loc, "metadata nesting exceeds maximum of 256 at", "type");
        return;
    }
    if (is_return && type.written_storage != StorageDefault)
        iface_error(type.source_loc, "storage-qualified interface returns are unsupported:",
                    type.written_storage == StorageHeap ? "heap" : "stack");
    if (type.written_nullable && !type.is_pointer &&
        (type.base == TypeFnPtr || type.base == TypeClosure || type.base == TypeSlice))
        iface_error(type.source_loc, "nullable composite interface types are unsupported:", "?");
    if (type.elem_type)
        iface_check_return_metadata(cg, *type.elem_type, False, aliases, alias_count, depth + 1);
    if (type.fn_ptr_desc) {
        iface_check_return_metadata(cg, type.fn_ptr_desc->ret_type, True, aliases, alias_count, depth + 1);
        for (usize_t i = 0; i < type.fn_ptr_desc->param_count; i++)
            iface_check_return_metadata(cg, type.fn_ptr_desc->params[i].type, False, aliases, alias_count, depth + 1);
    }
    if (type.generic_use)
        for (usize_t i = 0; i < type.generic_use->count; i++)
            iface_check_return_metadata(cg, type.generic_use->args[i], False, aliases, alias_count, depth + 1);
    if (type.base != TypeUser || !type.user_name) return;
    node_t *alias = Null;
    node_list_t *decls = &cg->ast->as.module.decls;
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *decl = decls->items[i];
        if (decl->kind != NodeTypeDecl || decl->as.type_decl.decl_kind != TypeDeclAlias ||
            strcmp(type.user_name, decl->as.type_decl.name) ||
            cg_fh_skip_decl(cg, decl->headers, decl->line)) continue;
        if (alias) {
            iface_error(type.source_loc, "ambiguous alias in interface metadata:", type.user_name);
            return;
        }
        alias = decl;
    }
    if (!alias) return;
    for (usize_t i = 0; i < alias_count; i++)
        if (aliases[i] == alias) {
            iface_error(type.source_loc, "alias cycle in interface metadata:", type.user_name);
            return;
        }
    if (alias_count == IfaceMaxMetadataDepth) {
        iface_error(type.source_loc, "metadata nesting exceeds maximum of 256 at", type.user_name);
        return;
    }
    aliases[alias_count] = alias;
    iface_check_return_metadata(cg, alias->as.type_decl.alias_type, is_return,
                                 aliases, alias_count + 1, depth + 1);
}

static void iface_check_method_metadata(cg_t *cg, node_t *method) {
    node_t *aliases[IfaceMaxMetadataDepth];
    for (usize_t i = 0; i < method->as.fn_decl.return_count; i++)
        iface_check_return_metadata(cg, method->as.fn_decl.return_types[i], True, aliases, 0, 0);
    for (usize_t i = 0; i < method->as.fn_decl.params.count; i++)
        iface_check_return_metadata(cg, method->as.fn_decl.params.items[i]->as.var_decl.type, False, aliases, 0, 0);
}

/* Legacy interface lowerers use 512-byte formatted names. Account for the
 * longest vtable suffix and qualified method spelling before any lowering. */
static usize_t iface_module_prefix_length(const char *name) {
    usize_t n = 0;
    if (name) for (; *name; name++) n += *name == '.' ? 2 : 1;
    return n;
}

static void iface_check_name_lengths(cg_t *cg) {
    for (usize_t i = 0; i < cg->iface_count; i++) {
        iface_reg_t *iface = &cg->interfaces[i];
        usize_t prefix = iface_module_prefix_length(iface->decl->module_name);
        if (strlen(iface->name) + (prefix ? prefix + 2 : 0) > 511)
            iface_error(iface_node_loc(iface->decl),
                "formatted type name exceeds maximum of 511 bytes for", iface->name);
    }
    node_list_t *decls = &cg->ast->as.module.decls;
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *decl = decls->items[i];
        if (decl->kind != NodeTypeDecl || cg_fh_skip_decl(cg, decl->headers, decl->line)) continue;
        if (decl->as.type_decl.decl_kind != TypeDeclStruct) continue;
        for (usize_t p = 0; p < decl->as.type_decl.impl_iface_count; p++) {
            usize_t ii = find_interface_index(cg, decl->as.type_decl.impl_ifaces[p]);
            if (ii == (usize_t)-1) continue;
            iface_reg_t *iface = &cg->interfaces[ii];
            usize_t base = strlen(decl->as.type_decl.name) + strlen(iface->name);
            boolean_t too_long = base + strlen("____vtable_t") + 2 > 511;
            usize_t prefix = iface_module_prefix_length(decl->module_name);
            for (usize_t g = 0; g < iface->group_count; g++)
                if (base + strlen(iface->groups[g].name) +
                    (prefix ? prefix + 6 : 2) > 511) too_long = True;
            if (too_long)
                iface_error(iface_node_loc(decl),
                    "formatted interface name exceeds maximum of 511 bytes for", decl->as.type_decl.name);
        }
    }
}

static void validate_interface_declarations(cg_t *cg) {
    node_list_t *decls = &cg->ast->as.module.decls;
    usize_t before = get_error_count();
    /* Register declarations first, allowing forward parents and module order. */
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *decl = decls->items[i];
        if (decl->kind != NodeTypeDecl && decl->kind != NodeFnDecl) continue;
        if (cg_fh_skip_decl(cg, decl->headers, decl->line)) continue;
        if (decl->kind == NodeFnDecl) {
            iface_check_constraints(decl->as.fn_decl.type_param_meta, decl->as.fn_decl.type_param_count);
            continue;
        }
        if (decl->as.type_decl.impl_iface_count > IfaceMaxParents) {
            iface_error(iface_node_loc(decl), "maximum of 8 direct parents/implementations exceeded for", decl->as.type_decl.name);
            continue;
        }
        iface_check_constraints(decl->as.type_decl.type_param_meta, decl->as.type_decl.type_param_count);
        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];
            iface_check_constraints(method->as.fn_decl.type_param_meta, method->as.fn_decl.type_param_count);
        }
        if (decl->as.type_decl.decl_kind != TypeDeclInterface) continue;
        if (find_interface_index(cg, decl->as.type_decl.name) != (usize_t)-1) {
            iface_error(iface_node_loc(decl), "duplicate interface definition", decl->as.type_decl.name);
            continue;
        }
        register_interface(cg, decl);
        if (decl->as.type_decl.type_param_count)
            iface_error(iface_node_loc(decl), "generic interfaces are unsupported:", decl->as.type_decl.name);
        if (decl->as.type_decl.methods.count > IfaceMaxMethods)
            iface_error(iface_node_loc(decl->as.type_decl.methods.items[IfaceMaxMethods]),
                "maximum of 32 own methods exceeded for", decl->as.type_decl.name);
        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];
            iface_check_method_metadata(cg, method);
            for (usize_t j = 0; j < m; j++)
                if (!strcmp(method->as.fn_decl.name, decl->as.type_decl.methods.items[j]->as.fn_decl.name)) {
                    iface_error(iface_node_loc(method), "duplicate own method", method->as.fn_decl.name);
                    break;
                }
        }
    }
    /* Bare-name registries cannot safely resolve an interface/type collision. */
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *decl = decls->items[i];
        if (decl->kind != NodeTypeDecl || cg_fh_skip_decl(cg, decl->headers, decl->line)) continue;
        if (decl->as.type_decl.impl_iface_count > IfaceMaxParents) continue;
        usize_t ii = find_interface_index(cg, decl->as.type_decl.name);
        if (ii != (usize_t)-1 && cg->interfaces[ii].decl != decl &&
                decl->as.type_decl.decl_kind != TypeDeclInterface)
            iface_error(iface_node_loc(decl), "ambiguous interface/type name", decl->as.type_decl.name);
        for (usize_t p = 0; p < decl->as.type_decl.impl_iface_count; p++) {
            char *name = decl->as.type_decl.impl_ifaces[p];
            ast_source_loc_t loc = decl->as.type_decl.impl_iface_locs[p];
            for (usize_t j = 0; j < p; j++)
                if (!strcmp(name, decl->as.type_decl.impl_ifaces[j])) {
                    iface_error(loc, "duplicate parent/implementation", name);
                    break;
                }
            usize_t pi = find_interface_index(cg, name);
            if (pi == (usize_t)-1) {
                boolean_t known = False;
                for (usize_t j = 0; j < decls->count; j++) {
                    node_t *other = decls->items[j];
                    if (other->kind == NodeTypeDecl && !strcmp(other->as.type_decl.name, name) &&
                        !cg_fh_skip_decl(cg, other->headers, other->line)) { known = True; break; }
                }
                iface_error(loc, known ? "parent/implementation is not an interface:" :
                            "unknown parent/implementation", name);
            } else if (decl->as.type_decl.decl_kind == TypeDeclInterface && ii != (usize_t)-1) {
                cg->interfaces[ii].parent_indices[p] = pi;
            }
        }
    }
    if (get_error_count() != before) return;
    for (usize_t i = 0; i < cg->iface_count; i++) flatten_interface(cg, i, 1);
    if (get_error_count() == before) iface_check_name_lengths(cg);
}

/* Compatibility adapter for existing consumers. I0 supplies deterministic group
 * order, but intentionally does not certify pending source signatures. Returning
 * zero is safe only after an explicit diagnostic, never as truncation. */
static usize_t collect_all_iface_methods(cg_t *cg, const char *iface_name,
                                         char **out_names, type_info_t *out_rets,
                                         usize_t max_out) {
    usize_t index = find_interface_index(cg, iface_name);
    if (index == (usize_t)-1) {
        iface_error(iface_node_loc(cg->ast), "unknown interface requested by collector", iface_name);
        return 0;
    }
    iface_reg_t *iface = &cg->interfaces[index];
    if (iface->graph_state != IfaceGraphReady || max_out < iface->group_count) {
        iface_error(iface_node_loc(iface->decl),
            max_out < iface->group_count ? "collector capacity is smaller than complete graph for" :
                                          "collector requires a ready graph for", iface_name);
        return 0;
    }
    for (usize_t i = 0; i < iface->group_count; i++) {
        if (iface->groups[i].origin_count > 1) {
            iface_error(iface_node_loc(iface->decl),
                "multiple distinct origins for method require signature validation (unsupported in I0):",
                iface->groups[i].name);
            return 0;
        }
    }
    for (usize_t i = 0; i < iface->group_count; i++) {
        node_t *origin = iface->groups[i].origins[0];
        out_names[i] = iface->groups[i].name;
        out_rets[i] = origin->as.fn_decl.return_count
            ? origin->as.fn_decl.return_types[0] : NO_TYPE;
    }
    return iface->group_count;
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
    if (method_count == 0) {
        if (cg->interfaces[ii].group_count == 0)
            iface_error(iface_node_loc(cg->interfaces[ii].decl),
                "empty-interface table construction is unsupported in I0:", iface_name);
        return (usize_t)-1;
    }

    struct_reg_t *sr = find_struct(cg, struct_name);
    usize_t base = strlen(struct_name) + strlen(iface_name);
    boolean_t names_fit = base <= 511 - 14; /* __S__I__vtable_t */
    for (usize_t i = 0; i < method_count; i++) {
        usize_t length = base + strlen(method_names[i]);
        if (length + 2 > 511 ||
            (sr && sr->mod_prefix && sr->mod_prefix[0] &&
             length + strlen(sr->mod_prefix) + 6 > 511)) names_fit = False;
    }
    if (!names_fit) {
        iface_error(iface_node_loc(cg->interfaces[ii].decl),
            "formatted interface name exceeds maximum of 511 bytes for", struct_name);
        return (usize_t)-1;
    }
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
    heap_t fn_ptrs_heap = allocate(method_count, sizeof(LLVMValueRef));
    LLVMValueRef *fn_ptrs = fn_ptrs_heap.pointer;

    for (usize_t i = 0; i < method_count; i++) {
        char mangled[512];
        /* try iface-qualified name first: "struct.iface.method" */
        snprintf(mangled, sizeof(mangled), "%s.%s.%s",
                 struct_name, iface_name, method_names[i]);
        symbol_t *fn_sym = cg_lookup_symbol(cg, mangled);

        if (!fn_sym && sr && sr->mod_prefix && sr->mod_prefix[0]) {
            snprintf(mangled, sizeof(mangled), "%s__%s__%s__%s",
                     sr->mod_prefix, struct_name, iface_name, method_names[i]);
            fn_sym = cg_lookup_symbol(cg, mangled);
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
            fn_sym = cg_lookup_symbol(cg, mangled);
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
