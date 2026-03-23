/* ── pointer safety checks ── */

/* Check: no writable pointer from const/final variable */
static void check_const_addr_of(cg_t *cg, node_t *init, type_info_t target_type, usize_t line) {
    if (!init || init->kind != NodeAddrOf || !target_type.is_pointer) return;
    node_t *operand = init->as.addr_of.operand;
    if (operand->kind != NodeIdentExpr) return;
    symbol_t *src = cg_lookup(cg, operand->as.ident.name);
    if (!src) return;
    boolean_t src_const = (src->flags & SymConst) != 0;
    boolean_t src_final = (src->flags & SymFinal) != 0;
    if ((src_const || src_final) && (target_type.ptr_perm & (PtrWrite)))
        log_err("line %lu: cannot derive writable pointer from %s variable '%s'",
                line, src_const ? "const" : "final", src->name);
}

/* Check: permission widening forbidden (e.g. *r → *rw) */
static void check_permission_widening(cg_t *cg, node_t *init, type_info_t target_type, usize_t line) {
    if (!init || !target_type.is_pointer) return;
    if (init->kind == NodeIdentExpr) {
        symbol_t *src = cg_lookup(cg, init->as.ident.name);
        if (!src || !src->stype.is_pointer) return;
        ptr_perm_t sp = src->stype.ptr_perm;
        ptr_perm_t tp = target_type.ptr_perm;
        /* a permission bit present in tp but absent in sp is a widening — forbidden */
        if (tp & ~sp & (PtrRead | PtrWrite | PtrArith))
            log_err("line %lu: cannot widen pointer permissions (source: %s%s%s → target: %s%s%s)",
                    line,
                    (sp & PtrRead)  ? "r" : "", (sp & PtrWrite) ? "w" : "",
                    (sp & PtrArith) ? "+" : "",
                    (tp & PtrRead)  ? "r" : "", (tp & PtrWrite) ? "w" : "",
                    (tp & PtrArith) ? "+" : "");
    }
}

/* Check: no stack pointer escape via ret */
static void check_stack_escape(cg_t *cg, node_t *ret_val, usize_t line) {
    if (!ret_val) return;
    if (ret_val->kind == NodeAddrOf) {
        node_t *operand = ret_val->as.addr_of.operand;
        if (operand->kind == NodeIdentExpr) {
            symbol_t *src = cg_lookup(cg, operand->as.ident.name);
            /* locals are stack-allocated; returning their address is always wrong */
            if (src && src->storage == StorageStack
                && symtab_lookup(&cg->locals, operand->as.ident.name))
                log_err("line %lu: cannot return pointer to local stack variable '%s'",
                        line, operand->as.ident.name);
        }
    }
}

/* Check: no ext function returning pointer to int (private) global */
static void check_ext_returns_int_ptr(cg_t *cg, node_t *ret_val, linkage_t fn_linkage, usize_t line) {
    if (fn_linkage != LinkageExternal || !ret_val) return;
    if (ret_val->kind == NodeAddrOf) {
        node_t *operand = ret_val->as.addr_of.operand;
        if (operand->kind == NodeIdentExpr) {
            symbol_t *src = symtab_lookup(&cg->globals, operand->as.ident.name);
            if (src && src->linkage == LinkageInternal)
                log_err("line %lu: ext function cannot expose pointer to int global '%s'",
                        line, operand->as.ident.name);
        }
    }
}

/* Check: pointer lifetime — pointee must outlive the pointer */
static void check_pointer_lifetime(cg_t *cg, node_t *init, usize_t ptr_scope_depth, usize_t line) {
    if (!init || init->kind != NodeAddrOf) return;
    node_t *operand = init->as.addr_of.operand;
    if (operand->kind != NodeIdentExpr) return;
    symbol_t *src = cg_lookup(cg, operand->as.ident.name);
    if (!src) return;
    /* global variables live forever (scope_depth 0); no problem */
    if (src->scope_depth > ptr_scope_depth)
        log_err("line %lu: pointer outlives pointee '%s' (scope mismatch)",
                line, src->name);
}

/* Check: null dereference of a statically-nil pointer */
static void check_null_deref(cg_t *cg, const char *name, usize_t line) {
    symbol_t *sym = cg_lookup(cg, name);
    if (sym && (sym->flags & SymNil))
        log_err("line %lu: dereference of nil pointer '%s'", line, name);
}

/* Check: pointer arithmetic permission and known array bounds */
static void check_ptr_arith_bounds(cg_t *cg, node_t *node) {
    if (node->as.binary.op != TokPlus && node->as.binary.op != TokMinus) return;
    node_t *ptr_node = node->as.binary.left;
    node_t *idx_node = node->as.binary.right;
    if (ptr_node->kind != NodeIdentExpr) return;
    symbol_t *sym = cg_lookup(cg, ptr_node->as.ident.name);
    if (!sym) return;
    /* enforce + permission for pointer arithmetic */
    if (sym->stype.is_pointer && !(sym->stype.ptr_perm & PtrArith))
        log_err("line %lu: pointer arithmetic not permitted on '%s' (pointer lacks '+' permission)",
                node->line, sym->name);
    if (idx_node->kind != NodeIntLitExpr) return;
    if (sym->array_size < 0) return; /* unknown size */
    long offset = idx_node->as.int_lit.value;
    if (node->as.binary.op == TokMinus) offset = -offset;
    if (offset < 0 || offset >= sym->array_size)
        log_err("line %lu: pointer arithmetic index %ld out of bounds for '%s[%ld]'",
                node->line, offset, sym->name, sym->array_size);
}
