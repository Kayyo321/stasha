/* Source-only canonical contracts for the bounded ordinary interface ABI.
 * Included by cg_interfaces.c after the I0 graph helpers. Never scalar-erases
 * source identity, mutates AST metadata, or consults generated symbol spelling.
 * Each linked record has a stable address and belongs to exactly one cg_t. */
typedef struct {
    type_info_t type;             /* fully expanded, owned value; nominal leaf */
    node_t *nominal;              /* declaration identity, never layout identity */
    storage_t domain, raw_domain;
    int raw_flags;
    const char *source_failure;   /* quiet query failure, reported only at admission */
} iface_type_t;
struct iface_signature {
    iface_signature_t *next;
    heap_t self_heap, params_heap;
    node_t *decl, *owner;
    iface_type_t result, *params;
    usize_t count;
    boolean_t valid;
    LLVMTypeRef native_type;      /* cached only after type registration */
};
typedef enum { IfaceBindingInvalid, IfaceBindingSourceReady, IfaceBindingNativeReady,
               IfaceBindingTableReady, IfaceBindingLocalUnavailable } iface_binding_state_t;
struct iface_binding {
    iface_binding_t *next;
    heap_t self_heap;
    node_t *owner;
    usize_t iface_index;
    iface_binding_state_t state;
    iface_signature_t *methods[IfaceMaxMethods];
    LLVMValueRef functions[IfaceMaxMethods];
    usize_t table_index;
};
struct iface_native {
    iface_native_t *next;
    heap_t self_heap;
    node_t *decl, *owner;
    LLVMValueRef function;
    boolean_t instance, c_abi;
    cg_abi_fn_t *abi_plan;
};

static boolean_t iface_active(cg_t *cg, node_t *n) {
    return !cg_fh_skip_decl(cg, n->headers, n->line);
}
static node_t *iface_named_type(cg_t *cg, const char *name, ast_source_loc_t loc) {
    node_t *found = Null;
    node_list_t ds = cg->ast->as.module.decls;
    for (usize_t i = 0; i < ds.count; ++i) {
        node_t *d = ds.items[i];
        if (d->kind != NodeTypeDecl || !name || strcmp(d->as.type_decl.name, name) || !iface_active(cg, d)) continue;
        if (found && found != d) {
            iface_error(loc, "ambiguous nominal declaration in signature:", name);
            return Null;
        }
        found = d;
    }
    if (!found) iface_error(loc, "unresolved nominal declaration in signature:", name);
    return found;
}
/* A screening reason is deliberately NOT a source-flow safety certificate.
 * It retains known generic/dependent origins until a concrete interface seam
 * consumes the value. Later assignments and arbitrary transfers are I1b. */
enum { IfaceOriginOrdinary = 0, IfaceOriginGeneric = 1, IfaceOriginUnresolved = 2 };
static node_t *iface_quiet_type(cg_t *cg, const char *name) {
    node_t *found = Null;
    for (usize_t i = 0; name && i < cg->ast->as.module.decls.count; ++i) {
        node_t *d = cg->ast->as.module.decls.items[i];
        if (d->kind != NodeTypeDecl || strcmp(d->as.type_decl.name, name) || !iface_active(cg, d)) continue;
        if (found && found != d) return Null;
        found = d;
    }
    return found;
}
static unsigned iface_type_origin(cg_t *cg, type_info_t type) {
    node_t *seen[IfaceMaxMetadataDepth]; usize_t count = 0;
    for (;;) {
        if (type.generic_use) return IfaceOriginGeneric;
        if (type.base != TypeUser) return IfaceOriginOrdinary;
        if (!type.user_name) return IfaceOriginUnresolved;
        /* A type parameter has no qualified specialization identity at this
         * seam. This does not exclude independent concrete declarations in the
         * same generic function, nor existing interface descriptor transfers. */
        for (usize_t i = 0; i < cg->generic_n; ++i)
            if (!strcmp(type.user_name, cg->generic_params[i])) return IfaceOriginUnresolved;
        node_t *d = iface_quiet_type(cg, type.user_name);
        if (!d) return IfaceOriginUnresolved;
        if (d->as.type_decl.type_param_count) return IfaceOriginGeneric;
        if (d->as.type_decl.decl_kind != TypeDeclAlias) return IfaceOriginOrdinary;
        if (count == IfaceMaxMetadataDepth) return IfaceOriginUnresolved;
        for (usize_t i = 0; i < count; ++i) if (seen[i] == d) return IfaceOriginUnresolved;
        seen[count++] = d; type = d->as.type_decl.alias_type;
    }
}
static boolean_t iface_concrete_origin_allowed(cg_t *cg, node_t *use, symbol_t *symbol) {
    unsigned origin = symbol->iface_excluded_origin | iface_type_origin(cg, symbol->stype);
    if (!origin) return True;
    ast_source_loc_t loc = iface_node_loc(use);
    const char *saved = diag_get_file();
    if (loc.file) diag_set_file(loc.file);
    diag_begin_error("interface declaration: %s '%s'", origin & IfaceOriginGeneric
        ? "generic-origin concrete interface use is unsupported for"
        : "unresolved/dependent concrete interface origin is unsupported for", symbol->name);
    diag_span(SRC_LOC(loc.line,loc.col,loc.len), True, "used here");
    diag_note("use a directly declared ordinary concrete value or a call with a declared ordinary result at this interface boundary");
    if (!(origin & IfaceOriginGeneric))
        diag_note("field/index-derived and other unresolved initialization origins are not yet qualified for concrete interface use");
    diag_finish();
    diag_set_file(saved);
    return False;
}

static boolean_t iface_type_fail(type_info_t t, const char *why) {
    iface_error(t.source_loc, why, t.user_name ? t.user_name : "type");
    return False;
}
/* The source query is also used conservatively on ordinary calls. Unsupported
 * metadata must stay silent there; only an affected interface admission reports
 * the copied reason. Signature construction retains its existing diagnostics. */
static boolean_t iface_resolve_type_fail(type_info_t t, const char *why,
                                         boolean_t source, iface_type_t *out) {
    if (source) { out->source_failure = why; return False; }
    return iface_type_fail(t, why);
}
/* Nominal leaves stop recursion. Alias expansion checks each syntax layer
 * before combining it, so an inner nullable/domain annotation cannot disappear. */
static boolean_t iface_resolve_type(cg_t *cg, type_info_t t, boolean_t result,
                                      boolean_t source, boolean_t alias_layer, node_t **seen,
                                      usize_t depth, iface_type_t *out) {
    memset(out, 0, sizeof(*out));
    if (depth > IfaceMaxMetadataDepth)
        return iface_resolve_type_fail(t, "metadata nesting exceeds maximum of 256 at", source, out);
    if (!source && result && t.written_storage != StorageDefault)
        return iface_resolve_type_fail(t, "storage-qualified interface returns are unsupported:", source, out);
    if (!source && alias_layer && t.written_storage != StorageDefault)
        return iface_resolve_type_fail(t, "storage-qualified aliases in interface signatures are unsupported:", source, out);
    if (t.generic_use) return iface_resolve_type_fail(t, "generic signature types are unsupported:", source, out);
    if (t.fn_ptr_desc || t.base == TypeFnPtr || t.base == TypeClosure)
        return iface_resolve_type_fail(t, "callback/closure interface signatures are unsupported:", source, out);
    if (t.elem_type || t.base == TypeSlice)
        return iface_resolve_type_fail(t, "slice interface signatures are unsupported:", source, out);
    if (t.base == TypeError || t.base == TypeFuture || t.base == TypeStream ||
        t.base == TypeVa || t.base == TypeZone || t.base == TypeInfer)
        return iface_resolve_type_fail(t, "error/handle/inferred interface signatures are unsupported:", source, out);
    int n = t.is_pointer ? (t.ptr_depth ? t.ptr_depth : 1) : 0;
    if (t.ptr_depth < 0 || t.ptr_depth > 8 || (!t.is_pointer && t.ptr_depth))
        return iface_resolve_type_fail(t, "invalid pointer depth in interface signature:", source, out);
    if (n && !t.ptr_depth) t.ptr_perms[0] = t.ptr_perm;
    for (int i = 0; i < n; ++i)
        if (t.ptr_perms[i] & ~(PtrRead | PtrWrite | PtrArith))
            return iface_resolve_type_fail(t, "invalid pointer permission bits in interface signature:", source, out);
    if (n && t.ptr_perm != t.ptr_perms[0])
        return iface_resolve_type_fail(t, "conflicting outer pointer permissions in interface signature:", source, out);
    t.ptr_depth = n;
    t.nullable = t.nullable || t.written_nullable;
    out->type = t;
    if (t.base == TypeUser) {
        node_t *d = source ? iface_quiet_type(cg, t.user_name) : iface_named_type(cg, t.user_name, t.source_loc);
        if (!d) {
            if (source) out->source_failure = "unresolved or ambiguous raw nominal declaration";
            return False;
        }
        if (d->as.type_decl.type_param_count)
            return iface_resolve_type_fail(t, "generic nominal interface signatures are unsupported:", source, out);
        if (d->as.type_decl.decl_kind == TypeDeclAlias) {
            for (usize_t i = 0; i < depth; ++i)
                if (seen[i] == d) return iface_resolve_type_fail(t, "alias cycle in interface signature:", source, out);
            if (depth == IfaceMaxMetadataDepth)
                return iface_resolve_type_fail(t, "metadata nesting exceeds maximum of 256 at", source, out);
            seen[depth] = d;
            iface_type_t inner;
            if (!iface_resolve_type(cg, d->as.type_decl.alias_type, result, source, True, seen, depth + 1, &inner)) {
                out->source_failure = inner.source_failure;
                return False;
            }
            if (source && n && inner.type.written_storage != StorageDefault)
                return iface_resolve_type_fail(t, "inner-domain alias composition is unsupported:", source, out);
            if (n && inner.type.nullable)
                return iface_resolve_type_fail(t, "inner-nullable alias composition is unsupported:", source, out);
            if (n + inner.type.ptr_depth > 8)
                return iface_resolve_type_fail(t, "composed pointer depth exceeds maximum of 8:", source, out);
            *out = inner;
            type_info_t *r = &out->type;
            for (int i = r->ptr_depth - 1; i >= 0; --i) r->ptr_perms[i + n] = r->ptr_perms[i];
            for (int i = 0; i < n; ++i) r->ptr_perms[i] = t.ptr_perms[i];
            r->ptr_depth += n;
            r->is_pointer = r->ptr_depth != 0;
            r->ptr_perm = r->is_pointer ? r->ptr_perms[0] : PtrNone;
            r->nullable = t.nullable || r->nullable;
            r->written_nullable = r->nullable;
            r->written_storage = source && t.written_storage == StorageDefault ? r->written_storage : t.written_storage;
            r->source_loc = t.source_loc;
        } else {
            out->nominal = d;
            if (!source && !alias_layer && !n && d->as.type_decl.decl_kind != TypeDeclEnum &&
                      d->as.type_decl.decl_kind != TypeDeclInterface)
                return iface_resolve_type_fail(t, "by-value struct/union interface signatures are unsupported:", source, out);
            if (!alias_layer && !n && d->as.type_decl.decl_kind == TypeDeclEnum) {
                for (usize_t i = 0; i < d->as.type_decl.variants.count; ++i)
                    if (d->as.type_decl.variants.items[i]->as.enum_variant.has_payload)
                        return iface_resolve_type_fail(t, "payload-enum interface signatures are unsupported:", source, out);
            }
        }
    } else if (t.base < TypeVoid || t.base > TypeF64) {
        return iface_resolve_type_fail(t, "unsupported interface signature type:", source, out);
    }
    if (!alias_layer && !out->type.is_pointer) {
        if (out->type.nullable) return iface_resolve_type_fail(t, "nullable nonpointer interface signature is unsupported:", source, out);
        if (!result && out->type.base == TypeVoid) return iface_resolve_type_fail(t, "void value parameter in interface signature:", source, out);
        if (out->nominal) {
            int kind = out->nominal->as.type_decl.decl_kind;
            if (!source && kind != TypeDeclEnum && kind != TypeDeclInterface)
                return iface_resolve_type_fail(t, "by-value struct/union interface signatures are unsupported:", source, out);
            if (kind == TypeDeclEnum)
                for (usize_t i = 0; i < out->nominal->as.type_decl.variants.count; ++i)
                    if (out->nominal->as.type_decl.variants.items[i]->as.enum_variant.has_payload)
                        return iface_resolve_type_fail(t, "payload-enum interface signatures are unsupported:", source, out);
        }
    }
    return True;
}
/* Signature admissibility remains unchanged; source facts may identify concrete
 * values without admitting aggregates into an interface method signature. */
static boolean_t iface_canonical_type(cg_t *cg, type_info_t t, boolean_t result,
                                      boolean_t alias_layer, node_t **seen,
                                      usize_t depth, iface_type_t *out) {
    return iface_resolve_type(cg,t,result,False,alias_layer,seen,depth,out);
}
static boolean_t iface_headers_supported(node_t *n) {
    if (!n->headers) return True;
    for (usize_t i = 0; i < n->headers->count; ++i) {
        const char *key = n->headers->items[i].key;
        if (!strcmp(key, "if") || !strcmp(key, "require") || !strcmp(key, "weak") ||
            !strcmp(key, "hidden") || !strcmp(key, "section") || !strcmp(key, "align") ||
            !strcmp(key, "export_name") || !strcmp(key, "doc") || !strcmp(key, "params") ||
            !strcmp(key, "returns") || !strcmp(key, "diagnostic")) continue;
        iface_error(iface_node_loc(n), !strcmp(key, "abi") ?
            "C/ABI interface signature headers are unsupported:" :
            "interface signature effect header is unsupported:", key);
        return False;
    }
    return True;
}
static iface_signature_t *iface_signature(cg_t *cg, node_t *method, node_t *owner) {
    for (iface_signature_t *s = cg->iface_signatures; s; s = s->next)
        if (s->decl == method && s->owner == owner) return s;
    heap_t h = allocate(1, sizeof(iface_signature_t));
    iface_signature_t *s = h.pointer;
    memset(s, 0, sizeof(*s)); s->self_heap = h; s->decl = method; s->owner = owner;
    s->next = cg->iface_signatures; cg->iface_signatures = s;
    usize_t before = get_error_count();
    if (method->is_c_extern) iface_error(iface_node_loc(method), "C-ABI interface methods are unsupported:", method->as.fn_decl.name);
    if (method->as.fn_decl.is_async || method->as.fn_decl.coro_flavor != CoroNone ||
        method->as.fn_decl.has_await || method->as.fn_decl.has_yield_value || method->as.fn_decl.has_yield_now)
        iface_error(iface_node_loc(method), "coroutine interface methods are unsupported:", method->as.fn_decl.name);
    if (method->as.fn_decl.is_variadic || method->as.fn_decl.is_typed_variadic)
        iface_error(iface_node_loc(method), "variadic interface methods are unsupported:", method->as.fn_decl.name);
    if (method->as.fn_decl.type_param_count || (owner && owner->as.type_decl.type_param_count))
        iface_error(iface_node_loc(method), "generic interface methods are unsupported:", method->as.fn_decl.name);
    if (method->as.fn_decl.attr_flags & ~(AttrWeak | AttrHidden))
        iface_error(iface_node_loc(method), "interface method attributes are unsupported:", method->as.fn_decl.name);
    iface_headers_supported(method);
    node_t *seen[IfaceMaxMetadataDepth];
    if (method->as.fn_decl.return_count != 1)
        iface_error(iface_node_loc(method), "interface methods require exactly one result:", method->as.fn_decl.name);
    else iface_canonical_type(cg, method->as.fn_decl.return_types[0], True, False, seen, 0, &s->result);
    s->count = method->as.fn_decl.params.count;
    if (s->count > (UINT_MAX - 1) || s->count > SIZE_MAX / sizeof(iface_type_t)) {
        iface_error(iface_node_loc(method), "interface parameter allocation limit exceeded:", method->as.fn_decl.name);
        return s;
    }
    if (s->count) { s->params_heap = allocate(s->count, sizeof(iface_type_t)); s->params = s->params_heap.pointer; memset(s->params, 0, s->count * sizeof(*s->params)); }
    for (usize_t i = 0; i < s->count; ++i) {
        node_t *p = method->as.fn_decl.params.items[i];
        if (p->as.var_decl.array_ndim || (p->as.var_decl.flags & VdeclArray))
            iface_error(iface_node_loc(p), "array interface parameters are unsupported:", p->as.var_decl.name);
        if (p->as.var_decl.flags & ~(VdeclConst | VdeclFinal))
            iface_error(iface_node_loc(p), "interface parameter effects are unsupported:", p->as.var_decl.name);
        iface_headers_supported(p);
        if (!iface_canonical_type(cg, p->as.var_decl.type, False, False, seen, 0, &s->params[i])) continue;
        iface_type_t *t = &s->params[i];
        t->raw_domain = p->as.var_decl.storage; t->raw_flags = p->as.var_decl.flags;
        storage_t written = p->as.var_decl.type.written_storage;
        if (written != StorageDefault && t->raw_domain != StorageDefault && written != t->raw_domain)
            iface_error(iface_node_loc(p), "conflicting interface parameter storage annotations:", p->as.var_decl.name);
        t->domain = written != StorageDefault ? written : t->raw_domain;
        if (!t->type.is_pointer && t->domain == StorageHeap)
            iface_error(iface_node_loc(p), "heap nonpointer interface parameters are unsupported:", p->as.var_decl.name);
        if (!t->type.is_pointer && (!t->nominal || t->nominal->as.type_decl.decl_kind == TypeDeclEnum))
            if (t->domain == StorageStack) t->domain = StorageDefault;
    }
    s->valid = get_error_count() == before;
    return s;
}
static boolean_t iface_types_equal(const iface_type_t *a, const iface_type_t *b) {
    if (a->type.base != b->type.base || a->nominal != b->nominal || a->domain != b->domain ||
        a->type.ptr_depth != b->type.ptr_depth || a->type.nullable != b->type.nullable) return False;
    for (int i = 0; i < a->type.ptr_depth; ++i) if (a->type.ptr_perms[i] != b->type.ptr_perms[i]) return False;
    return True;
}
static boolean_t iface_signatures_equal(iface_signature_t *a, iface_signature_t *b) {
    if (!a || !b || !a->valid || !b->valid || a->count != b->count || !iface_types_equal(&a->result, &b->result)) return False;
    for (usize_t i = 0; i < a->count; ++i) if (!iface_types_equal(&a->params[i], &b->params[i])) return False;
    return True;
}
static void iface_conflict(node_t *a, node_t *b, const char *reason) {
    iface_error(iface_node_loc(a), reason, a->as.fn_decl.name);
    iface_error(iface_node_loc(b), "conflicting declaration is here:", b->as.fn_decl.name);
}
static boolean_t iface_descends(cg_t *cg, usize_t child, usize_t parent) {
    if (child == parent) return True;
    if (cg->iface_count > SIZE_MAX / sizeof(usize_t)) return False;
    heap_t vh = allocate(cg->iface_count, sizeof(boolean_t)); boolean_t *visited = vh.pointer;
    heap_t sh = allocate(cg->iface_count, sizeof(usize_t)); usize_t *stack = sh.pointer;
    memset(visited, 0, cg->iface_count * sizeof(*visited));
    usize_t count = 0; stack[count++] = child; visited[child] = True; boolean_t found = False;
    while (count && !found) {
        iface_reg_t *r = &cg->interfaces[stack[--count]];
        for (usize_t i = 0; i < r->parent_count; ++i) {
            usize_t p = r->parent_indices[i];
            if (p == parent) { found = True; break; }
            if (!visited[p]) { visited[p] = True; stack[count++] = p; }
        }
    }
    deallocate(sh); deallocate(vh); return found;
}
static boolean_t iface_declares(cg_t *cg, node_t *owner, usize_t ii) {
    for (usize_t i = 0; i < owner->as.type_decl.impl_iface_count; ++i) {
        usize_t direct = find_interface_index(cg, owner->as.type_decl.impl_ifaces[i]);
        if (direct != (usize_t)-1 && iface_descends(cg, direct, ii)) return True;
    }
    return False;
}
static const char *iface_qualifier(node_t *owner, node_t *method) {
    const char *q = method->as.fn_decl.iface_qualifier;
    return q && strcmp(q, owner->as.type_decl.name) ? q : Null;
}
static iface_method_group_t *iface_group(cg_t *cg, usize_t ii, const char *name) {
    for (usize_t i = 0; i < cg->interfaces[ii].group_count; ++i)
        if (!strcmp(cg->interfaces[ii].groups[i].name, name)) return &cg->interfaces[ii].groups[i];
    return Null;
}
static boolean_t iface_origins_intersect(iface_method_group_t *a, iface_method_group_t *b) {
    if (!a || !b) return False;
    for (usize_t i = 0; i < a->origin_count; ++i)
        for (usize_t j = 0; j < b->origin_count; ++j) if (a->origins[i] == b->origins[j]) return True;
    return False;
}
static node_t *iface_select(cg_t *cg, node_t *owner, usize_t ii, iface_method_group_t *g) {
    for (unsigned priority = 0; priority < 3; ++priority) {
        node_t *found = Null;
        for (usize_t i = 0; i < owner->as.type_decl.methods.count; ++i) {
            node_t *m = owner->as.type_decl.methods.items[i];
            if (!iface_active(cg, m) || strcmp(m->as.fn_decl.name, g->name)) continue;
            const char *q = iface_qualifier(owner, m);
            boolean_t match = False;
            if (priority == 0) match = q && !strcmp(q, cg->interfaces[ii].name);
            else if (priority == 1) match = !q;
            else if (q) {
                usize_t qi = find_interface_index(cg, q);
                match = qi != (usize_t)-1 && iface_declares(cg, owner, qi) &&
                    (iface_descends(cg, qi, ii) || iface_descends(cg, ii, qi)) &&
                    iface_origins_intersect(g, iface_group(cg, qi, g->name));
            }
            if (!match || found == m) continue;
            if (found) { iface_conflict(m, found, "ambiguous selected instance implementation:"); return Null; }
            found = m;
        }
        if (found) return found;
    }
    iface_error(iface_node_loc(owner), "missing instance implementation:", g->name);
    return Null;
}
static iface_binding_t *iface_find_binding(cg_t *cg, node_t *owner, usize_t ii) {
    for (iface_binding_t *b = cg->iface_bindings; b; b = b->next)
        if (b->owner == owner && b->iface_index == ii) return b;
    return Null;
}
static void iface_preflight(cg_t *cg) {
    for (usize_t ii = 0; ii < cg->iface_count; ++ii) {
        iface_reg_t *r = &cg->interfaces[ii]; usize_t before = get_error_count();
        for (usize_t i = 0; i < r->group_count; ++i) {
            iface_method_group_t *g = &r->groups[i];
            g->signature = iface_signature(cg, g->origins[0], Null);
            for (usize_t j = 1; j < g->origin_count; ++j) {
                iface_signature_t *s = iface_signature(cg, g->origins[j], Null);
                if (s->valid && g->signature->valid && !iface_signatures_equal(s, g->signature))
                    iface_conflict(s->decl, g->signature->decl, "distinct-origin interface signature conflict:");
            }
            g->signature_pending = !g->signature->valid || get_error_count() != before;
        }
        r->signatures_ready = get_error_count() == before;
    }
    if (get_error_count()) return;
    node_list_t ds = cg->ast->as.module.decls;
    for (usize_t i = 0; i < ds.count; ++i) {
        node_t *owner = ds.items[i];
        if (owner->kind != NodeTypeDecl || !iface_active(cg, owner) ||
            (owner->as.type_decl.decl_kind != TypeDeclStruct && owner->as.type_decl.decl_kind != TypeDeclUnion)) continue;
        if (owner->as.type_decl.type_param_count) continue; /* separately qualified generic lifecycle */
        if (owner->as.type_decl.impl_iface_count)
            if (!iface_named_type(cg, owner->as.type_decl.name, iface_node_loc(owner))) continue;
        for (usize_t j = 0; j < owner->as.type_decl.methods.count; ++j) {
            node_t *m = owner->as.type_decl.methods.items[j];
            if (!iface_active(cg, m)) continue;
            const char *q = iface_qualifier(owner, m);
            if (q) {
                usize_t qi = find_interface_index(cg, q);
                if (qi == (usize_t)-1) iface_error(iface_node_loc(m), "unknown interface qualifier:", q);
                else if (!iface_declares(cg, owner, qi)) iface_error(iface_node_loc(m), "unrelated interface qualifier:", q);
                else if (!iface_group(cg, qi, m->as.fn_decl.name)) iface_error(iface_node_loc(m), "unknown qualified interface method:", m->as.fn_decl.name);
                iface_signature(cg, m, owner);
            }
            for (usize_t k = 0; (q || owner->as.type_decl.impl_iface_count) && k < j; ++k) {
                node_t *other = owner->as.type_decl.methods.items[k]; const char *oq = iface_qualifier(owner, other);
                if (iface_active(cg, other) && !strcmp(m->as.fn_decl.name, other->as.fn_decl.name) &&
                    ((!q && !oq) || (q && oq && !strcmp(q, oq))))
                    iface_conflict(m, other, "duplicate concrete method declaration:");
            }
        }
        for (usize_t ii = 0; ii < cg->iface_count; ++ii) {
            if (!iface_declares(cg, owner, ii)) continue;
            heap_t h = allocate(1, sizeof(iface_binding_t)); iface_binding_t *b = h.pointer;
            memset(b, 0, sizeof(*b)); b->self_heap = h; b->owner = owner; b->iface_index = ii;
            b->next = cg->iface_bindings; cg->iface_bindings = b; b->table_index = (usize_t)-1;
            usize_t before = get_error_count(); iface_reg_t *r = &cg->interfaces[ii];
            for (usize_t j = 0; j < r->group_count; ++j) {
                node_t *m = iface_select(cg, owner, ii, &r->groups[j]);
                if (!m) continue;
                iface_signature_t *s = iface_signature(cg, m, owner); b->methods[j] = s;
                if (!s->valid) { iface_error(iface_node_loc(m), "selected interface implementation is unsupported:", m->as.fn_decl.name); continue; }
                if (!iface_signatures_equal(s, r->groups[j].signature))
                    iface_conflict(m, r->groups[j].signature->decl, "selected interface implementation signature mismatch:");
            }
            if (get_error_count() == before) b->state = IfaceBindingSourceReady;
        }
    }
}
static iface_signature_t *iface_cached_signature(cg_t *cg, node_t *decl, node_t *owner) {
    for (iface_signature_t *s = cg->iface_signatures; s; s = s->next)
        if (s->decl == decl && s->owner == owner && s->valid) return s;
    return Null;
}
static void iface_register_native(cg_t *cg, node_t *decl, node_t *owner,
                                   LLVMValueRef function, boolean_t instance, cg_abi_fn_t *abi) {
    heap_t h = allocate(1, sizeof(iface_native_t)); iface_native_t *n = h.pointer;
    *n = (iface_native_t){.next=cg->iface_natives,.self_heap=h,.decl=decl,.owner=owner,
        .function=function,.instance=instance,.c_abi=abi != Null,.abi_plan=abi}; cg->iface_natives = n;
}
static iface_native_t *iface_native(cg_t *cg, node_t *decl, node_t *owner) {
    for (iface_native_t *n = cg->iface_natives; n; n = n->next)
        if (n->decl == decl && n->owner == owner) return n;
    return Null;
}
static LLVMTypeRef iface_native_type(cg_t *cg, iface_signature_t *s) {
    if (!s || !s->valid) return Null;
    if (s->native_type) return s->native_type;
    heap_t h = allocate(s->count + 1, sizeof(LLVMTypeRef)); LLVMTypeRef *p = h.pointer;
    p[0] = LLVMPointerTypeInContext(cg->ctx, 0);
    for (usize_t i = 0; i < s->count; ++i) p[i + 1] = get_llvm_type(cg, s->params[i].type);
    s->native_type = LLVMFunctionType(get_llvm_type(cg, s->result.type), p, (unsigned)s->count + 1, 0);
    deallocate(h); return s->native_type;
}
static boolean_t iface_native_matches(cg_t *cg, iface_signature_t *contract, iface_native_t *n) {
    if (!n || !n->instance || n->c_abi || !n->owner || !LLVMIsAFunction(n->function)) return False;
    if (LLVMGlobalGetValueType(n->function) != iface_native_type(cg, contract) ||
        LLVMGetFunctionCallConv(n->function) != LLVMCCallConv) return False;
    /* This native plan has no parameter/result attributes. Compare the exact
     * set, including LLVM type attributes (byref/byval/sret), rather than a
     * name denylist that silently misses new ABI or behavioral promises.
     * Function-level target/debug/optimization attributes are not this set. */
    for (unsigned p = 0; p <= contract->count + 1; ++p)
        if (LLVMGetAttributeCountAtIndex(n->function, p)) return False;
    return True;
}
static void iface_free_contracts(cg_t *cg) {
    while (cg->iface_signatures) { iface_signature_t *s = cg->iface_signatures; cg->iface_signatures = s->next; if (s->params_heap.pointer) deallocate(s->params_heap); deallocate(s->self_heap); }
    while (cg->iface_bindings) { iface_binding_t *b = cg->iface_bindings; cg->iface_bindings = b->next; deallocate(b->self_heap); }
    while (cg->iface_natives) { iface_native_t *n = cg->iface_natives; cg->iface_natives = n->next; deallocate(n->self_heap); }
}
