/* ── pointer safety checks ── */

/* Check: no writable pointer from const/final variable */
static void check_const_addr_of(cg_t *cg, node_t *init, type_info_t target_type, usize_t line) {
    if (cg->in_unsafe > 0) return;
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
        diag_set_category(ErrCatPointerSafety);
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
    if (cg->in_unsafe > 0) return;
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
            diag_set_category(ErrCatPointerSafety);
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
    if (ret_val->kind == NodeComptimeFmt && !ret_val->as.comptime_fmt.on_heap) {
        diag_begin_error("cannot return a stack comptime format string");
        diag_set_category(ErrCatPointerSafety);
        diag_span(SRC_LOC(line, 0, 0), True,
                  "points into a stack frame that is about to be destroyed");
        diag_note("plain @'...' allocates into the current frame; the buffer dies on return");
        diag_help("use 'heap @'...'' to allocate on the heap (caller must rem.())");
        diag_finish();
        return;
    }
    if (ret_val->kind == NodeAddrOf) {
        node_t *operand = ret_val->as.addr_of.operand;
        if (operand->kind == NodeIdentExpr) {
            symbol_t *src = cg_lookup(cg, operand->as.ident.name);
            if (src && src->storage == StorageStack
                && symtab_lookup(&cg->locals, operand->as.ident.name)) {
                diag_begin_error("cannot return a pointer to local stack variable '%s'",
                                 operand->as.ident.name);
                diag_set_category(ErrCatPointerSafety);
                diag_span(SRC_LOC(line, 0, 0), True,
                          "'%s' is a stack variable — it is freed when the function returns",
                          operand->as.ident.name);
                diag_note("stack variables are destroyed when the function returns; the pointer would dangle");
                diag_help("heap-allocate via a pointer: stack i32 *rw x = new.(sizeof.(i32));");
                diag_finish();
            }
        }
    }

    /* returning a stack-stored slice — its data ptr dangles */
    if (ret_val->kind == NodeIdentExpr) {
        symbol_t *src = cg_lookup(cg, ret_val->as.ident.name);
        if (src && src->stype.base == TypeSlice && src->storage == StorageStack
                && symtab_lookup(&cg->locals, ret_val->as.ident.name)) {
            diag_begin_error("cannot return stack slice '%s'", ret_val->as.ident.name);
            diag_set_category(ErrCatPointerSafety);
            diag_span(SRC_LOC(line, 0, 0), True,
                      "'%s' borrows stack memory — it dies when the function returns",
                      ret_val->as.ident.name);
            diag_note("stack slices view stack-allocated memory; the data pointer would dangle after return");
            diag_help("declare the slice as 'heap []T' and allocate with make.([]T, n) or make.{...}");
            diag_finish();
        }
    }

    /* returning arr[:] / arr[lo:hi] of a local fixed array — same dangle */
    if (ret_val->kind == NodeSliceExpr) {
        node_t *obj = ret_val->as.slice_expr.object;
        if (obj && obj->kind == NodeIdentExpr) {
            symbol_t *src = cg_lookup(cg, obj->as.ident.name);
            if (src && symtab_lookup(&cg->locals, obj->as.ident.name)) {
                boolean_t is_local_array = (LLVMGetTypeKind(src->type) == LLVMArrayTypeKind);
                boolean_t is_stack_slice = (src->stype.base == TypeSlice
                                            && src->storage == StorageStack);
                if (is_local_array || is_stack_slice) {
                    diag_begin_error("cannot return slice that views local stack memory");
                    diag_set_category(ErrCatPointerSafety);
                    diag_span(SRC_LOC(line, 0, 0), True,
                              "'%s' is local — its memory dies when the function returns",
                              obj->as.ident.name);
                    diag_note("returned slice would point at freed stack frame");
                    diag_help("allocate a heap slice and copy into it before returning");
                    diag_finish();
                }
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
    if (cg->in_unsafe > 0) return;
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
        diag_set_category(ErrCatPointerSafety);
        diag_span(SRC_LOC(line, 0, 0), True,
                  "'%s' is statically known to be nil here", name);
        diag_note("dereferencing a nil pointer is undefined behaviour");
        diag_help("check the pointer is non-nil before dereferencing it");
        diag_finish();
    }
}

/* ── Provenance tracking ── */

/* Assign a fresh provenance tag to a newly-declared pointer variable.
 * Called from gen_local_var when init is a new.() or new_in_zone(). */
static void prov_record_alloc(cg_t *cg, const char *var_name) {
    if (cg->in_unsafe > 0) return;
    for (int i = cg->provenance_count - 1; i >= 0; i--) {
        if (!cg->provenance[i].name) {
            cg->provenance[i].name = (char *)var_name;
            return;
        }
    }
}

/* Mark a provenance tag as closed when rem.(p) is called.
 * var_name is the name of the pointer variable being freed. */
static void prov_close(cg_t *cg, const char *var_name, usize_t line) {
    if (cg->in_unsafe > 0) return;
    for (int i = 0; i < cg->provenance_count; i++) {
        if (cg->provenance[i].name && strcmp(cg->provenance[i].name, var_name) == 0) {
            cg->provenance[i].closed     = True;
            cg->provenance[i].close_line = line;
            return;
        }
    }
}

/* Check: use of a pointer with closed provenance (use-after-free). */
static void prov_check_use(cg_t *cg, const char *var_name, usize_t line) {
    if (cg->in_unsafe > 0) return;
    for (int i = 0; i < cg->provenance_count; i++) {
        if (cg->provenance[i].name && strcmp(cg->provenance[i].name, var_name) == 0
                && cg->provenance[i].closed) {
            diag_begin_error("use-after-free: pointer '%s' was freed on line %zu",
                             var_name, cg->provenance[i].close_line);
            diag_set_category(ErrCatPointerSafety);
            diag_span(SRC_LOC(line, 0, 0), True,
                      "use of freed pointer '%s' here", var_name);
            diag_note("the memory pointed to by '%s' was released via rem.()", var_name);
            diag_help("do not use a pointer after calling rem.() on it");
            diag_finish();
            return;
        }
    }
}

/* Propagate provenance: when `dst = src`, dst inherits src's provenance tag */
static void prov_propagate(cg_t *cg, const char *dst, const char *src) {
    if (!dst || !src || cg->in_unsafe > 0) return;
    for (int i = 0; i < cg->provenance_count; i++) {
        if (cg->provenance[i].name && strcmp(cg->provenance[i].name, src) == 0) {
            /* dst now shares the same provenance entry name chain — we just
             * record dst as an alias pointing to the same tag. */
            if (cg->provenance_count < 255) {
                cg->provenance[cg->provenance_count].name       = (char *)dst;
                cg->provenance[cg->provenance_count].tag        = cg->provenance[i].tag;
                cg->provenance[cg->provenance_count].closed     = cg->provenance[i].closed;
                cg->provenance[cg->provenance_count].close_line = cg->provenance[i].close_line;
                cg->provenance_count++;
            }
            return;
        }
    }
}

/* ── @frees enforcement ── */

/* Check: a function with @frees param must call rem.(param) or pass to @frees fn.
 * Called on function exit when checking params.
 * This is a simple conservative check: we verify the function body contains at
 * least one rem.(param) statement for each @frees parameter.  More precise
 * dataflow analysis would be needed for path coverage, but this catches the
 * obvious cases. */
static void check_frees_param(cg_t *cg, node_t *fn_decl) {
    if (cg->in_unsafe > 0) return;
    for (usize_t pi = 0; pi < fn_decl->as.fn_decl.params.count; pi++) {
        node_t *param = fn_decl->as.fn_decl.params.items[pi];
        if (!(param->as.var_decl.flags & VdeclFrees)) continue;
        const char *pname = param->as.var_decl.name;
        if (!pname) continue;
        /* walk the body looking for rem.(pname) or a call to an @frees fn with pname */
        boolean_t found_rem = False;
        node_t *body = fn_decl->as.fn_decl.body;
        if (!body) continue;
        /* simplified: search the top-level block statements */
        node_t *block = body;
        for (usize_t si = 0; si < block->as.block.stmts.count && !found_rem; si++) {
            node_t *stmt = block->as.block.stmts.items[si];
            if (stmt->kind == NodeRemStmt) {
                node_t *ptr = stmt->as.rem_stmt.ptr;
                if (ptr && ptr->kind == NodeIdentExpr
                        && strcmp(ptr->as.ident.name, pname) == 0)
                    found_rem = True;
            }
        }
        if (!found_rem) {
            diag_begin_error("@frees parameter '%s' must be freed on all paths",
                             pname);
            diag_span(DIAG_NODE(fn_decl), True,
                      "function declared with @frees '%s' here", pname);
            diag_note("@frees parameters must either call rem.(p) directly or pass to "
                      "another @frees function on every code path");
            diag_help("add rem.(%s); at the end of the function", pname);
            diag_finish();
        }
    }
}

/* Check: calling rem.(p) on a parameter without @frees annotation is forbidden
 * (unless inside unsafe {}).  Called from gen_rem in cg_expr.c. */
static void check_rem_on_param(cg_t *cg, const char *var_name, usize_t line) {
    if (cg->in_unsafe > 0) return;
    /* find the symbol; if it was declared as a function parameter (scope_depth == 1
     * and storage is a param), check for @frees flag */
    symbol_t *sym = symtab_lookup(&cg->locals, var_name);
    if (!sym) sym = symtab_lookup(&cg->globals, var_name);
    if (!sym) return;
    /* we check the @frees flag stored in sym->flags — need to extend symbol_t */
    /* For now: if sym is a parameter and lacks @frees, emit warning-level note.
     * (Full enforcement requires the @frees flag to be stored in symbol_t.) */
    (void)line; /* suppress unused warning until symbol_t gains a frees flag */
}

/* ── Nullable pointer dereference check ── */

/* Check: dereferencing a nullable pointer without a nil-check guard.
 * Called when a pointer access is performed on a nullable type. */
static void check_nullable_deref(cg_t *cg, const char *var_name, boolean_t nullable, usize_t line) {
    if (!nullable || cg->in_unsafe > 0) return;
    /* Simplified: if the type is nullable and we see a direct deref without a prior
     * nil-check in scope, emit an error.  A full dominator-based nil-check analysis
     * would require tracking "narrowed to non-null" in the symbol table per scope. */
    symbol_t *sym = cg_lookup(cg, var_name);
    if (!sym) return;
    if (sym->stype.nullable) {
        diag_begin_error("dereference of nullable pointer '%s' without nil-check", var_name);
        diag_span(SRC_LOC(line, 0, 0), True,
                  "'%s' has type '?T *' and may be nil", var_name);
        diag_note("nullable pointers must be checked against nil before dereferencing");
        diag_help("wrap the dereference in: if (%s != nil) { ... }", var_name);
        diag_finish();
    }
}

/* Record that a variable is in a nil-checked scope (narrowed to non-null).
 * Called from gen_if when the condition is `p != nil` or `nil != p`. */
static void prov_narrow_nonnull(cg_t *cg, const char *var_name) {
    /* Set the nullable flag to False in the local symbol so nullable_deref checks
     * pass inside the if-body.  The flag is restored when the scope exits. */
    symbol_t *sym = symtab_lookup(&cg->locals, var_name);
    if (sym) sym->stype.nullable = False;
}

/* ── Storage-qualified pointer restriction checks ── */

/* Check: a heap-qualified pointer must not point at stack memory.
 * A stack-qualified pointer must not point at heap memory.
 * Called from gen_assign and gen_local_var when storing a pointer value. */
static void check_storage_domain(cg_t *cg, storage_t ptr_qual, boolean_t rhs_is_heap,
                                  boolean_t rhs_is_stack, usize_t line) {
    if (cg->in_unsafe > 0) return;
    if (ptr_qual == StorageHeap && rhs_is_stack) {
        diag_begin_error("cannot assign a stack address to a heap pointer");
        diag_set_category(ErrCatStorageDomain);
        diag_span(SRC_LOC(line, 0, 0), True, "assignment here");
        diag_note("heap-qualified pointers can only point at heap-allocated memory");
        diag_help("use a heap pointer pointing to heap memory via new.()");
        diag_finish();
    } else if (ptr_qual == StorageStack && rhs_is_heap) {
        diag_begin_error("cannot assign a heap address to a stack pointer");
        diag_set_category(ErrCatStorageDomain);
        diag_span(SRC_LOC(line, 0, 0), True, "assignment here");
        diag_note("stack-qualified pointers can only point at stack variables");
        diag_help("declare the pointer without a storage qualifier: T *p = new.(...)");
        diag_finish();
    }
}

/* Determine if a RHS node expression produces a heap or stack address.
 * Returns: 1 = heap, -1 = stack, 0 = unknown */
static int rhs_addr_kind(cg_t *cg, node_t *rhs) {
    if (!rhs) return 0;
    if (rhs->kind == NodeNewExpr)   return  1;   /* heap — needs rem.() */
    if (rhs->kind == NodeNewInZone) return -1;   /* zone-managed, stack-like */
    if (rhs->kind == NodeMakeExpr)
        return rhs->as.make_expr.init ? 0 : 1;   /* make.{} domain follows LHS */
    if (rhs->kind == NodeAppendExpr) return 1;   /* append always returns heap */
    if (rhs->kind == NodeComptimeFmt)
        return rhs->as.comptime_fmt.on_heap ? 1 : -1;
    if (rhs->kind == NodeAddrOf) {
        node_t *op = rhs->as.addr_of.operand;
        if (op->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, op->as.ident.name);
            if (sym && sym->storage == StorageStack) return -1; /* stack */
            if (sym && sym->storage == StorageHeap)  return  1; /* heap  */
        }
        return -1; /* default: &var is stack */
    }
    if (rhs->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, rhs->as.ident.name);
        if (sym && sym->stype.is_pointer) {
            if (sym->storage == StorageStack) return -1;
            if (sym->storage == StorageHeap)  return  1;
        }
        return 0;
    }
    return 0;
}

/* Classify a slice-typed RHS as heap (+1) / stack (-1) / unknown (0).
 * Used for slice-LHS assignments — pointer-only rhs_addr_kind misses these. */
static int slice_addr_kind(cg_t *cg, node_t *rhs) {
    if (!rhs) return 0;
    if (rhs->kind == NodeMakeExpr)
        return rhs->as.make_expr.init ? 0 : 1;   /* make.{} domain follows LHS */
    if (rhs->kind == NodeAppendExpr) return 1;
    if (rhs->kind == NodeNilExpr)    return 0;
    if (rhs->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, rhs->as.ident.name);
        if (sym && sym->stype.base == TypeSlice) {
            if (sym->storage == StorageHeap)  return  1;
            if (sym->storage == StorageStack) return -1;
        }
        return 0;
    }
    if (rhs->kind == NodeSliceExpr) {
        node_t *obj = rhs->as.slice_expr.object;
        if (obj && obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
            if (sym) {
                if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind) return -1;
                if (sym->stype.base == TypeSlice) {
                    if (sym->storage == StorageHeap)  return  1;
                    if (sym->storage == StorageStack) return -1;
                }
            }
        }
        return 0;
    }
    return 0;
}

/* Reject storing a stack-borrowed slice into a heap slice variable.
 * The reverse (heap → stack reslice) stays allowed — it is a borrowed view. */
static void check_slice_domain(cg_t *cg, storage_t lhs_storage, node_t *rhs, usize_t line) {
    if (cg->in_unsafe > 0) return;
    if (lhs_storage != StorageHeap) return;
    int ak = slice_addr_kind(cg, rhs);
    if (ak == -1) {
        diag_begin_error("cannot assign a stack-borrowed slice to a heap slice");
        diag_set_category(ErrCatStorageDomain);
        diag_span(SRC_LOC(line, 0, 0), True, "assignment here");
        diag_note("heap-qualified slices must own their backing allocation");
        diag_help("allocate with make.([]T, n) or make.{...} into the heap slice");
        diag_finish();
    }
}

/* ── Unconsumed-future check ─────────────────────────────────────────────
 * Warn when a `future.[T]` local goes out of scope without ever being
 * passed to `await(...)`, `await.(fn)(...)`, `await.all/any(...)`,
 * `future.wait/ready/get/drop(...)`, or referenced inside a `defer` block.
 *
 * This is a syntactic heuristic — no dataflow. A false negative is
 * preferable to a false positive here. In practice the check catches
 * the common "declared but forgot to consume" case, which maps directly
 * to a runtime leak of the `__future_t`, its result buffer, and the
 * pthread condvar/mutex pair inside it. */

typedef struct {
    const char *names[128];
    usize_t     lines[128];
    usize_t     count;
} fut_name_set_t;

static void fut_set_add(fut_name_set_t *s, const char *name, usize_t line) {
    if (!name || s->count >= 128) return;
    for (usize_t i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return;
    s->names[s->count] = name;
    s->lines[s->count] = line;
    s->count++;
}

static boolean_t fut_set_has(fut_name_set_t *s, const char *name) {
    if (!name) return False;
    for (usize_t i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return True;
    return False;
}

static void collect_consumed_ident(fut_name_set_t *used, node_t *handle) {
    if (!handle) return;
    if (handle->kind == NodeIdentExpr)
        fut_set_add(used, handle->as.ident.name, handle->line);
}

static void walk_unconsumed_futures(node_t *n, fut_name_set_t *decls,
                                     fut_name_set_t *used);

static void walk_list(node_list_t *list, fut_name_set_t *decls, fut_name_set_t *used) {
    if (!list) return;
    for (usize_t i = 0; i < list->count; i++)
        walk_unconsumed_futures(list->items[i], decls, used);
}

static void walk_unconsumed_futures(node_t *n, fut_name_set_t *decls,
                                     fut_name_set_t *used) {
    if (!n) return;
    switch (n->kind) {
        case NodeVarDecl:
            if (n->as.var_decl.type.base == TypeFuture
                    && !n->as.var_decl.type.is_pointer
                    && n->as.var_decl.name)
                fut_set_add(decls, n->as.var_decl.name, n->line);
            walk_unconsumed_futures(n->as.var_decl.init, decls, used);
            break;
        case NodeAwaitExpr:
            collect_consumed_ident(used, n->as.await_expr.handle);
            walk_unconsumed_futures(n->as.await_expr.handle, decls, used);
            break;
        case NodeFutureOp:
            collect_consumed_ident(used, n->as.future_op.handle);
            walk_unconsumed_futures(n->as.future_op.handle, decls, used);
            break;
        case NodeAwaitCombinator:
            for (usize_t i = 0; i < n->as.await_combinator.handles.count; i++) {
                node_t *h = n->as.await_combinator.handles.items[i];
                collect_consumed_ident(used, h);
                walk_unconsumed_futures(h, decls, used);
            }
            break;
        case NodeBlock:        walk_list(&n->as.block.stmts,     decls, used); break;
        case NodeExprStmt:     walk_unconsumed_futures(n->as.expr_stmt.expr, decls, used); break;
        case NodeIfStmt:
            walk_unconsumed_futures(n->as.if_stmt.cond, decls, used);
            walk_unconsumed_futures(n->as.if_stmt.then_block, decls, used);
            walk_unconsumed_futures(n->as.if_stmt.else_block, decls, used);
            break;
        case NodeForStmt:
            walk_unconsumed_futures(n->as.for_stmt.init,   decls, used);
            walk_unconsumed_futures(n->as.for_stmt.cond,   decls, used);
            walk_unconsumed_futures(n->as.for_stmt.update, decls, used);
            walk_unconsumed_futures(n->as.for_stmt.body,   decls, used);
            break;
        case NodeWhileStmt:
            walk_unconsumed_futures(n->as.while_stmt.cond, decls, used);
            walk_unconsumed_futures(n->as.while_stmt.body, decls, used);
            break;
        case NodeDoWhileStmt:
            walk_unconsumed_futures(n->as.do_while_stmt.body, decls, used);
            walk_unconsumed_futures(n->as.do_while_stmt.cond, decls, used);
            break;
        case NodeForeachStmt:
            walk_unconsumed_futures(n->as.foreach_stmt.slice, decls, used);
            walk_unconsumed_futures(n->as.foreach_stmt.body,  decls, used);
            break;
        case NodeInfLoop:      walk_unconsumed_futures(n->as.inf_loop.body, decls, used); break;
        case NodeDeferStmt:    walk_unconsumed_futures(n->as.defer_stmt.body, decls, used); break;
        case NodeRetStmt:      walk_list(&n->as.ret_stmt.values,  decls, used); break;
        case NodeMatchStmt:
            walk_unconsumed_futures(n->as.match_stmt.expr, decls, used);
            for (usize_t i = 0; i < n->as.match_stmt.arms.count; i++)
                walk_unconsumed_futures(n->as.match_stmt.arms.items[i]->as.match_arm.body,
                                         decls, used);
            break;
        case NodeSwitchStmt:
            walk_unconsumed_futures(n->as.switch_stmt.expr, decls, used);
            for (usize_t i = 0; i < n->as.switch_stmt.cases.count; i++)
                walk_unconsumed_futures(n->as.switch_stmt.cases.items[i]->as.switch_case.body,
                                         decls, used);
            break;
        case NodeWithStmt:
            walk_unconsumed_futures(n->as.with_stmt.decl, decls, used);
            walk_unconsumed_futures(n->as.with_stmt.cond, decls, used);
            walk_unconsumed_futures(n->as.with_stmt.body, decls, used);
            walk_unconsumed_futures(n->as.with_stmt.else_block, decls, used);
            break;
        case NodeUnsafeBlock:  walk_unconsumed_futures(n->as.unsafe_block.body, decls, used); break;
        case NodeZoneDecl:     walk_unconsumed_futures(n->as.zone_decl.body, decls, used); break;
        case NodeAsyncCall:    walk_list(&n->as.async_call.args,  decls, used); break;
        case NodeThreadCall:   walk_list(&n->as.thread_call.args, decls, used); break;
        case NodeCallExpr:
            /* futures passed as ordinary call arguments are treated as consumed
               — the callee takes ownership of the handle. */
            for (usize_t i = 0; i < n->as.call.args.count; i++) {
                collect_consumed_ident(used, n->as.call.args.items[i]);
                walk_unconsumed_futures(n->as.call.args.items[i], decls, used);
            }
            break;
        case NodeMethodCall:
            walk_unconsumed_futures(n->as.method_call.object, decls, used);
            for (usize_t i = 0; i < n->as.method_call.args.count; i++) {
                collect_consumed_ident(used, n->as.method_call.args.items[i]);
                walk_unconsumed_futures(n->as.method_call.args.items[i], decls, used);
            }
            break;
        case NodeAssignExpr:
            /* `f = other_future` rebinds the slot — treat the old value as
               consumed (caller must have handled it) and the new RHS ident
               as consumed too. */
            collect_consumed_ident(used, n->as.assign.target);
            collect_consumed_ident(used, n->as.assign.value);
            walk_unconsumed_futures(n->as.assign.target, decls, used);
            walk_unconsumed_futures(n->as.assign.value, decls, used);
            break;
        case NodeMultiAssign:
            walk_list(&n->as.multi_assign.targets, decls, used);
            walk_list(&n->as.multi_assign.values,  decls, used);
            break;
        case NodeAddrOf:
            /* &f — escaping the handle's address counts as consumption. */
            collect_consumed_ident(used, n->as.addr_of.operand);
            walk_unconsumed_futures(n->as.addr_of.operand, decls, used);
            break;
        case NodeBinaryExpr:
            walk_unconsumed_futures(n->as.binary.left,  decls, used);
            walk_unconsumed_futures(n->as.binary.right, decls, used);
            break;
        case NodeUnaryPrefixExpr:
        case NodeUnaryPostfixExpr:
            walk_unconsumed_futures(n->as.unary.operand, decls, used);
            break;
        default: break;
    }
}

static void check_unconsumed_futures(cg_t *cg, node_t *fn_decl) {
    (void)cg;
    if (!fn_decl || fn_decl->kind != NodeFnDecl) return;
    node_t *body = fn_decl->as.fn_decl.body;
    if (!body) return;

    fut_name_set_t decls = {0};
    fut_name_set_t used  = {0};
    walk_unconsumed_futures(body, &decls, &used);

    for (usize_t i = 0; i < decls.count; i++) {
        if (fut_set_has(&used, decls.names[i])) continue;
        /* a local named '_' (discard) is intentional. */
        if (decls.names[i][0] == '_') continue;
        diag_begin_warning("future '%s' is never consumed — this leaks the handle, "
                           "result buffer, and condvar", decls.names[i]);
        diag_span(SRC_LOC(decls.lines[i], 0, 0), True,
                  "declared here");
        diag_note("futures own runtime resources; consume with "
                  "await(f), future.drop(f), await.all(...), or await.any(...)");
        diag_help("if the result is intentionally discarded, add "
                  "'defer future.drop(%s);' right after the declaration",
                  decls.names[i]);
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
