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
        if (!cg_fh_skip_decl(cg, methods->items[m]->headers, methods->items[m]->line) && !iface_add_origin(iface, methods->items[m])) {
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
            if (cg_fh_skip_decl(cg, method->headers, method->line)) continue;
            iface_check_method_metadata(cg, method);
            for (usize_t j = 0; j < m; j++)
                if (!cg_fh_skip_decl(cg, decl->as.type_decl.methods.items[j]->headers, decl->as.type_decl.methods.items[j]->line) &&
                    !strcmp(method->as.fn_decl.name, decl->as.type_decl.methods.items[j]->as.fn_decl.name)) {
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

#include "cg_signatures.c"

/* I0 order is retained; every origin has passed full source equality. */
static usize_t collect_all_iface_methods(cg_t *cg, const char *name,
                                         char **names, type_info_t *returns, usize_t capacity) {
    usize_t ii = find_interface_index(cg, name);
    if (ii == (usize_t)-1 || !cg->interfaces[ii].signatures_ready ||
        capacity < cg->interfaces[ii].group_count) {
        iface_error(iface_node_loc(cg->ast), "collector requires complete signature-ready interface:", name);
        return 0;
    }
    iface_reg_t *r = &cg->interfaces[ii];
    for (usize_t i = 0; i < r->group_count; ++i) {
        names[i] = r->groups[i].name; returns[i] = r->groups[i].signature->result.type;
    }
    return r->group_count;
}

typedef struct { boolean_t success; usize_t table; iface_binding_t *binding; } iface_table_result_t;
static iface_table_result_t iface_table_failure(void) {
    return (iface_table_result_t){False, (usize_t)-1, Null};
}
static iface_table_result_t ensure_vtable(cg_t *cg, const char *owner_name, const char *iface_name) {
    usize_t ii = find_interface_index(cg, iface_name);
    node_t *owner = iface_named_type(cg, owner_name, iface_node_loc(cg->ast));
    if (!owner || ii == (usize_t)-1) return iface_table_failure();
    iface_binding_t *b = iface_find_binding(cg, owner, ii);
    if (!b) {
        iface_error(iface_node_loc(owner), "no declared/transitive interface implementation relation for", iface_name);
        return iface_table_failure();
    }
    if (b->state == IfaceBindingTableReady) return (iface_table_result_t){True, b->table_index, b};
    if (b->state == IfaceBindingLocalUnavailable) {
        iface_error(iface_node_loc(owner), "imported internal interface implementation is unavailable for local table:", iface_name);
        return iface_table_failure();
    }
    if (b->state != IfaceBindingNativeReady) {
        iface_error(iface_node_loc(owner), "interface binding is not native-ready for", iface_name);
        return iface_table_failure();
    }
    iface_reg_t *r = &cg->interfaces[ii];
    if (!r->group_count) {
        iface_error(iface_node_loc(owner), "empty-interface table construction is unsupported in I1a:", iface_name);
        return iface_table_failure();
    }
    char type_name[512], global_name[512];
    int tn = snprintf(type_name, sizeof(type_name), "__%s__%s__vtable_t", owner_name, iface_name);
    int gn = snprintf(global_name, sizeof(global_name), "__%s__%s__vtable", owner_name, iface_name);
    if (tn < 0 || gn < 0 || (usize_t)tn >= sizeof(type_name) || (usize_t)gn >= sizeof(global_name)) {
        iface_error(iface_node_loc(owner), "formatted interface name exceeds maximum of 511 bytes for", owner_name);
        return iface_table_failure();
    }
    /* Check the entire temporary slot array before creating any LLVM global. */
    LLVMTypeRef fields[IfaceMaxMethods]; LLVMValueRef slots[IfaceMaxMethods];
    for (usize_t i = 0; i < r->group_count; ++i) {
        if (!b->functions[i]) {
            iface_error(iface_node_loc(owner), "incomplete native binding for", r->groups[i].name);
            return iface_table_failure();
        }
        fields[i] = LLVMPointerTypeInContext(cg->ctx, 0); slots[i] = b->functions[i];
    }
    if (cg->vtable_count == cg->vtable_cap) {
        if (cg->vtable_cap > SIZE_MAX / sizeof(cg->vtables[0]) / 2) {
            iface_error(iface_node_loc(owner), "vtable allocation limit exceeded for", owner_name);
            return iface_table_failure();
        }
        usize_t cap = cg->vtable_cap ? cg->vtable_cap * 2 : 8;
        cg->vtables_heap = cg->vtables_heap.pointer ? reallocate(cg->vtables_heap, cap * sizeof(cg->vtables[0])) : allocate(cap, sizeof(cg->vtables[0]));
        cg->vtables = cg->vtables_heap.pointer; cg->vtable_cap = cap;
    }
    LLVMTypeRef vt = LLVMStructCreateNamed(cg->ctx, type_name);
    LLVMStructSetBody(vt, fields, (unsigned)r->group_count, 0);
    LLVMValueRef global = LLVMAddGlobal(cg->module, vt, global_name);
    LLVMSetLinkage(global, LLVMInternalLinkage);
    LLVMSetInitializer(global, LLVMConstNamedStruct(vt, slots, (unsigned)r->group_count));
    LLVMSetGlobalConstant(global, 1);
    usize_t vi = cg->vtable_count++;
    cg->vtables[vi].struct_name = (char *)owner_name; cg->vtables[vi].iface_name = (char *)iface_name;
    cg->vtables[vi].vtable_global = global; cg->vtables[vi].vtable_type = vt;
    b->table_index = vi; b->state = IfaceBindingTableReady;
    return (iface_table_result_t){True, vi, b};
}
static void iface_finish_bindings(cg_t *cg) {
    usize_t before = get_error_count();
    for (usize_t ii = 0; ii < cg->iface_count; ++ii)
        for (usize_t i = 0; i < cg->interfaces[ii].group_count; ++i)
            iface_native_type(cg, cg->interfaces[ii].groups[i].signature);
    for (iface_binding_t *b = cg->iface_bindings; b; b = b->next) {
        if (b->state != IfaceBindingSourceReady) continue;
        iface_reg_t *r = &cg->interfaces[b->iface_index]; boolean_t good = True, unavailable = False;
        for (usize_t i = 0; i < r->group_count; ++i) {
            iface_signature_t *s = b->methods[i];
            iface_native_t *n = s ? iface_native(cg, s->decl, b->owner) : Null;
            if (!n && s && b->owner->from_lib && s->decl->as.fn_decl.linkage == LinkageInternal) {
                unavailable = True; continue; /* provider owns its already-validated table */
            }
            if (!iface_native_matches(cg, r->groups[i].signature, n)) {
                iface_error(iface_node_loc(s ? s->decl : b->owner), "selected implementation native ABI/addressability mismatch:", r->groups[i].name);
                good = False;
            } else b->functions[i] = n->function;
        }
        b->state = !good ? IfaceBindingInvalid : unavailable ? IfaceBindingLocalUnavailable : IfaceBindingNativeReady;
    }
    if (get_error_count() != before) return;
    for (iface_binding_t *b = cg->iface_bindings; b; b = b->next)
        if (b->state == IfaceBindingNativeReady && cg->interfaces[b->iface_index].group_count)
            ensure_vtable(cg, b->owner->as.type_decl.name, cg->interfaces[b->iface_index].name);
}
/* Construction consumes a ready relation; there is no null-value recovery that
 * can be mistaken for a successful descriptor. */
static LLVMValueRef construct_fat_ptr(cg_t *cg, iface_table_result_t table, LLVMValueRef object) {
    if (!table.success || !table.binding || !object ||
        table.binding->state != IfaceBindingTableReady || LLVMGetTypeKind(LLVMTypeOf(object)) != LLVMPointerTypeKind) return Null;
    LLVMTypeRef fat = cg->interfaces[table.binding->iface_index].fat_ptr_type;
    LLVMValueRef value = LLVMGetUndef(fat);
    value = LLVMBuildInsertValue(cg->builder, value, object, 0, "iface.object");
    return LLVMBuildInsertValue(cg->builder, value, cg->vtables[table.table].vtable_global, 1, "iface.table");
}
static type_info_t scalar_expr_type(cg_t *, node_t *, type_info_t);
static boolean_t scalar_numeric(type_info_t);
static LLVMValueRef gen_scalar_as(cg_t *, node_t *, type_info_t);
static boolean_t iface_source_check_slot(cg_t *, node_t *, iface_signature_t *);
static LLVMValueRef iface_source_emit_slot_arg(cg_t *, node_t *, iface_type_t);
static LLVMValueRef iface_call_recovery(cg_t *cg, iface_signature_t *s) {
    LLVMTypeRef t = s && s->valid ? get_llvm_type(cg, s->result.type) : LLVMInt32TypeInContext(cg->ctx);
    return LLVMGetTypeKind(t) == LLVMVoidTypeKind ? LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0) : LLVMGetUndef(t);
}
/* Only reliable primitive conversions use scalar contextualization. Everything
 * else must already have the formal representation; source flow is I1b. */
static LLVMValueRef iface_emit_call(cg_t *cg, node_t *node, iface_signature_t *s,
                                    LLVMValueRef fn, LLVMValueRef object) {
    if (!s || !s->valid || !s->native_type || !fn || !object) return iface_call_recovery(cg, s);
    if (!iface_source_check_slot(cg, node, s)) return iface_call_recovery(cg, s);
    if (node->as.method_call.args.count != s->count) {
        iface_error(iface_node_loc(node), "interface call argument count does not match declaration:", node->as.method_call.method);
        return iface_call_recovery(cg, s);
    }
    if (LLVMTypeOf(object) != LLVMPointerTypeInContext(cg->ctx, 0)) {
        iface_error(iface_node_loc(node), "interface receiver representation mismatch:", node->as.method_call.method);
        return iface_call_recovery(cg, s);
    }
    heap_t h = allocate(s->count + 1, sizeof(LLVMValueRef)); LLVMValueRef *args = h.pointer; args[0] = object;
    usize_t before = get_error_count();
    for (usize_t i = 0; i < s->count; ++i) {
        node_t *arg = node->as.method_call.args.items[i]; type_info_t formal = s->params[i].type;
        args[i + 1] = iface_source_emit_slot_arg(cg, arg, s->params[i]);
        if (!args[i + 1] || LLVMTypeOf(args[i + 1]) != get_llvm_type(cg, formal))
        {
            ast_source_loc_t loc = iface_node_loc(arg);
            if (!loc.col) loc = iface_node_loc(node);
            if (!loc.col) loc.col = 1;
            if (!loc.len) loc.len = 1;
            iface_error(loc, "interface argument representation requires unsupported conversion:", node->as.method_call.method);
        }
    }
    LLVMValueRef result;
    if (get_error_count() != before) result = iface_call_recovery(cg, s);
    else { result = LLVMBuildCall2(cg->builder, s->native_type, fn, args, (unsigned)s->count + 1, ""); LLVMSetInstructionCallConv(result, LLVMCCallConv); }
    deallocate(h); return result;
}
static boolean_t iface_receiver_value(cg_t *cg, node_t *node, type_info_t t) {
    node_t *seen[IfaceMaxMetadataDepth]; iface_type_t canonical;
    if (!iface_canonical_type(cg, t, False, False, seen, 0, &canonical)) return False;
    if (canonical.type.is_pointer) {
        iface_error(iface_node_loc(node), "pointer-to-interface descriptor receiver is unsupported:", node->as.method_call.method);
        return False;
    }
    return True;
}
static LLVMValueRef gen_iface_method_call(cg_t *cg, node_t *node, symbol_t *object, usize_t ii) {
    iface_reg_t *r = &cg->interfaces[ii]; iface_method_group_t *g = iface_group(cg, ii, node->as.method_call.method);
    if (!g || !r->signatures_ready) {
        iface_error(iface_node_loc(node), "unknown/unready interface method:", node->as.method_call.method);
        return iface_call_recovery(cg, Null);
    }
    if (!iface_receiver_value(cg, node, object->stype)) return iface_call_recovery(cg, g->signature);
    if (node->as.method_call.args.count != g->signature->count) {
        iface_error(iface_node_loc(node), "interface call argument count does not match declaration:", g->name);
        return iface_call_recovery(cg, g->signature);
    }
    LLVMTypeRef ptr = LLVMPointerTypeInContext(cg->ctx, 0);
    if (!object->value || LLVMTypeOf(object->value) != ptr || (object->type && object->type != r->fat_ptr_type)) {
        iface_error(iface_node_loc(node), "interface receiver storage representation mismatch:", g->name);
        return iface_call_recovery(cg, g->signature);
    }
    LLVMValueRef fat = LLVMBuildLoad2(cg->builder, r->fat_ptr_type, object->value, "fat");
    LLVMValueRef obj = LLVMBuildExtractValue(cg->builder, fat, 0, "iface_obj");
    LLVMValueRef table = LLVMBuildExtractValue(cg->builder, fat, 1, "iface_vt");
    usize_t slot = (usize_t)(g - r->groups);
    LLVMValueRef index = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), slot, 0);
    LLVMValueRef address = LLVMBuildGEP2(cg->builder, ptr, table, &index, 1, "fn_slot");
    LLVMValueRef fn = LLVMBuildLoad2(cg->builder, ptr, address, "fn_ptr");
    return iface_emit_call(cg, node, g->signature, fn, obj);
}
/* The inference seam consults canonical slot metadata before any bare-name
 * method fallback. This is a result query, not source-actual validation. */
static iface_signature_t *iface_call_signature(cg_t *cg, node_t *node) {
    if (!node || node->kind != NodeMethodCall) return Null;
    node_t *obj = node->as.method_call.object; usize_t ii = (usize_t)-1;
    if (obj->kind == NodeMemberExpr) ii = find_interface_index(cg, obj->as.member_expr.field);
    if (ii == (usize_t)-1) {
        type_info_t t = scalar_expr_type(cg, obj, NO_TYPE);
        if (t.base == TypeUser && t.user_name) ii = find_interface_index(cg, t.user_name);
    }
    if (ii == (usize_t)-1) return Null;
    iface_method_group_t *g = iface_group(cg, ii, node->as.method_call.method);
    return g ? g->signature : Null;
}
