/* ── struct / enum / alias lookup ── */

static struct_reg_t *find_struct(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->struct_count; i++)
        if (strcmp(cg->structs[i].name, name) == 0) return &cg->structs[i];
    return Null;
}

static enum_reg_t *find_enum(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->enum_count; i++)
        if (strcmp(cg->enums[i].name, name) == 0) return &cg->enums[i];
    return Null;
}

static type_info_t resolve_alias(cg_t *cg, type_info_t ti) {
    if (ti.base == TypeUser && ti.user_name) {
        for (usize_t i = 0; i < cg->alias_count; i++) {
            if (strcmp(cg->aliases[i].name, ti.user_name) == 0) {
                type_info_t actual = cg->aliases[i].actual;
                /* preserve pointer qualification from the original type */
                if (ti.is_pointer && !actual.is_pointer) {
                    actual.is_pointer = True;
                    actual.ptr_perm   = ti.ptr_perm;
                }
                return actual;
            }
        }
    }
    return ti;
}

/* Return the full lib_entry_t for an alias, or NULL if not found.
   Used by gen_method_call to distinguish Stasha module aliases
   (mod_prefix != NULL) from C lib aliases (mod_prefix == NULL). */
static lib_entry_t *find_lib_entry(cg_t *cg, const char *alias) {
    for (usize_t i = 0; i < cg->lib_count; i++) {
        if (cg->libs[i].alias && strcmp(cg->libs[i].alias, alias) == 0)
            return &cg->libs[i];
        if (!cg->libs[i].alias && strcmp(cg->libs[i].name, alias) == 0)
            return &cg->libs[i];
    }
    return Null;
}

/* Kept for call-sites that only need the library name string. */
static const char *find_lib_alias(cg_t *cg, const char *alias) {
    lib_entry_t *e = find_lib_entry(cg, alias);
    return e ? e->name : Null;
}

/* Walk the module's flat declaration list and return the NodeFnDecl whose
   bare name matches `name` (ignoring method qualifications).  Used by
   let-binding type inference. */
static node_t *find_fn_decl(cg_t *cg, const char *name) {
    if (!cg->ast) return Null;
    node_list_t *decls = &cg->ast->as.module.decls;
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *d = decls->items[i];
        if (d->kind == NodeFnDecl && d->as.fn_decl.name
                && strcmp(d->as.fn_decl.name, name) == 0)
            return d;
        /* also search inline struct methods */
        if (d->kind == NodeTypeDecl) {
            for (usize_t j = 0; j < d->as.type_decl.methods.count; j++) {
                node_t *m = d->as.type_decl.methods.items[j];
                if (m->kind == NodeFnDecl && m->as.fn_decl.name
                        && strcmp(m->as.fn_decl.name, name) == 0)
                    return m;
            }
        }
    }
    return Null;
}
