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
    if ((src_const || src_final) && (target_type.ptr_perm & PtrWrite)) {
        diag_begin_error("cannot derive a writable pointer from %s variable '%s'",
                         src_const ? "const" : "final", src->name);
        diag_span(SRC_LOC(line, 0, 0), True,
                  "'%s' is declared %s here", src->name,
                  src_const ? "const" : "final");
        diag_note("%s variables cannot be mutated through a pointer",
                  src_const ? "const" : "final");
        diag_help("use a *r (read-only) pointer instead");
        diag_finish();
    }
}

/* Check: permission widening forbidden (e.g. *r → *rw) */
static void check_permission_widening(cg_t *cg, node_t *init, type_info_t target_type, usize_t line) {
    if (!init || !target_type.is_pointer) return;
    if (init->kind == NodeIdentExpr) {
        symbol_t *src = cg_lookup(cg, init->as.ident.name);
        if (!src || !src->stype.is_pointer) return;
        ptr_perm_t sp = src->stype.ptr_perm;
        ptr_perm_t tp = target_type.ptr_perm;
        if (tp & ~sp & (PtrRead | PtrWrite | PtrArith)) {
            char src_perm[8] = {0}, tgt_perm[8] = {0};
            usize_t si = 0, ti = 0;
            if (sp & PtrRead)  src_perm[si++] = 'r';
            if (sp & PtrWrite) src_perm[si++] = 'w';
            if (sp & PtrArith) src_perm[si++] = '+';
            if (tp & PtrRead)  tgt_perm[ti++] = 'r';
            if (tp & PtrWrite) tgt_perm[ti++] = 'w';
            if (tp & PtrArith) tgt_perm[ti++] = '+';
            diag_begin_error("cannot widen pointer permissions from *%s to *%s",
                             src_perm[0] ? src_perm : "none",
                             tgt_perm[0] ? tgt_perm : "none");
            diag_span(SRC_LOC(line, 0, 0), True, "permission widening here");
            diag_note("a pointer may only be used with the permissions it was declared with");
            diag_help("declare the source pointer with the required permissions");
            diag_finish();
        }
    }
}

/* Check: no stack pointer escape via ret */
static void check_stack_escape(cg_t *cg, node_t *ret_val, usize_t line) {
    if (!ret_val) return;
    if (ret_val->kind == NodeAddrOf) {
        node_t *operand = ret_val->as.addr_of.operand;
        if (operand->kind == NodeIdentExpr) {
            symbol_t *src = cg_lookup(cg, operand->as.ident.name);
            if (src && src->storage == StorageStack
                && symtab_lookup(&cg->locals, operand->as.ident.name)) {
                diag_begin_error("cannot return a pointer to local stack variable '%s'",
                                 operand->as.ident.name);
                diag_span(SRC_LOC(line, 0, 0), True,
                          "'%s' is a stack variable — it is freed when the function returns",
                          operand->as.ident.name);
                diag_note("stack variables are destroyed when the function returns; the pointer would dangle");
                diag_help("heap-allocate via a pointer: stack i32 *rw x = new.(sizeof.(i32));");
                diag_finish();
            }
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
            if (src && src->linkage == LinkageInternal) {
                diag_begin_error("ext function cannot expose a pointer to int (private) global '%s'",
                                 operand->as.ident.name);
                diag_span(SRC_LOC(line, 0, 0), True,
                          "returning address of private global here");
                diag_note("'int' globals are module-private; exposing their address breaks encapsulation");
                diag_help("declare the global as 'ext' if it is meant to be accessible from outside");
                diag_finish();
            }
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
    if (src->scope_depth > ptr_scope_depth) {
        diag_begin_error("pointer outlives pointee '%s' (scope mismatch)", src->name);
        diag_span(SRC_LOC(line, 0, 0), True,
                  "pointer is stored in an outer scope than '%s'", src->name);
        diag_note("'%s' will be freed before the pointer that holds its address", src->name);
        diag_help("move the variable to the same or outer scope as the pointer");
        diag_finish();
    }
}

/* Check: null dereference of a statically-nil pointer */
static void check_null_deref(cg_t *cg, const char *name, usize_t line) {
    symbol_t *sym = cg_lookup(cg, name);
    if (sym && (sym->flags & SymNil)) {
        diag_begin_error("dereference of nil pointer '%s'", name);
        diag_span(SRC_LOC(line, 0, 0), True,
                  "'%s' is statically known to be nil here", name);
        diag_note("dereferencing a nil pointer is undefined behaviour");
        diag_help("check the pointer is non-nil before dereferencing it");
        diag_finish();
    }
}

/* Check: pointer arithmetic permission and known array bounds */
static void check_ptr_arith_bounds(cg_t *cg, node_t *node) {
    if (node->as.binary.op != TokPlus && node->as.binary.op != TokMinus) return;
    node_t *ptr_node = node->as.binary.left;
    node_t *idx_node = node->as.binary.right;
    if (ptr_node->kind != NodeIdentExpr) return;
    symbol_t *sym = cg_lookup(cg, ptr_node->as.ident.name);
    if (!sym) return;
    if (sym->stype.is_pointer && !(sym->stype.ptr_perm & PtrArith)) {
        diag_begin_error("pointer arithmetic not permitted on '%s'", sym->name);
        diag_span(SRC_LOC(node->line, node->col, 0), True,
                  "pointer lacks '+' permission");
        diag_note("'%s' was declared without the '+' permission — pointer arithmetic is disallowed",
                  sym->name);
        diag_help("declare the pointer with '+' permission: e.g. *rw+ or *+");
        diag_finish();
    }
    if (idx_node->kind != NodeIntLitExpr) return;
    if (sym->array_size < 0) return;
    long offset = idx_node->as.int_lit.value;
    if (node->as.binary.op == TokMinus) offset = -offset;
    if (offset < 0 || offset >= sym->array_size) {
        diag_begin_error("pointer arithmetic index %ld is out of bounds for '%s[%ld]'",
                         offset, sym->name, sym->array_size);
        diag_span(SRC_LOC(node->line, node->col, 0), True,
                  "index %ld is outside [0, %ld)", offset, sym->array_size);
        diag_note("accessing outside array bounds is undefined behaviour");
        diag_finish();
    }
}
