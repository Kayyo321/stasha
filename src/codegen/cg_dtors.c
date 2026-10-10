/* ── destructor scope helpers ── */

static void push_dtor_scope(cg_t *cg) {
    if (cg->dtor_depth >= cg->dtor_cap) {
        usize_t new_cap = cg->dtor_cap < 8 ? 8 : cg->dtor_cap * 2;
        if (cg->dtor_stack_heap.pointer == Null)
            cg->dtor_stack_heap = allocate(new_cap, sizeof(dtor_scope_t));
        else
            cg->dtor_stack_heap = reallocate(cg->dtor_stack_heap, new_cap * sizeof(dtor_scope_t));
        cg->dtor_stack = cg->dtor_stack_heap.pointer;
        cg->dtor_cap = new_cap;
    }
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth++];
    scope->vars = Null;
    scope->count = 0;
    scope->capacity = 0;
    scope->heap = NullHeap;
    scope->deferred = Null;
    scope->deferred_count = 0;
    scope->deferred_cap = 0;
    scope->deferred_heap = NullHeap;
}

static void add_deferred_stmt(cg_t *cg, node_t *stmt) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    if (scope->deferred_count >= scope->deferred_cap) {
        usize_t new_cap = scope->deferred_cap < 4 ? 4 : scope->deferred_cap * 2;
        if (scope->deferred_heap.pointer == Null)
            scope->deferred_heap = allocate(new_cap, sizeof(deferred_stmt_t));
        else
            scope->deferred_heap = reallocate(scope->deferred_heap, new_cap * sizeof(deferred_stmt_t));
        scope->deferred = scope->deferred_heap.pointer;
        scope->deferred_cap = new_cap;
    }
    scope->deferred[scope->deferred_count++] = (deferred_stmt_t){ stmt, cg->locals.count };
}

static void add_dtor_var(cg_t *cg, LLVMValueRef alloca_val, const char *struct_name) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    if (scope->count >= scope->capacity) {
        usize_t new_cap = scope->capacity < 4 ? 4 : scope->capacity * 2;
        if (scope->heap.pointer == Null)
            scope->heap = allocate(new_cap, sizeof(dtor_var_t));
        else
            scope->heap = reallocate(scope->heap, new_cap * sizeof(dtor_var_t));
        scope->vars = scope->heap.pointer;
        scope->capacity = new_cap;
    }
    scope->vars[scope->count].alloca_val  = alloca_val;
    scope->vars[scope->count].struct_name = (char *)struct_name;
    scope->vars[scope->count].is_zone = False;
    scope->count++;
}

static void add_zone_cleanup(cg_t *cg, LLVMValueRef alloca_val) {
    if (cg->dtor_depth == 0) return;
    add_dtor_var(cg, alloca_val, Null);
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    scope->vars[scope->count - 1].is_zone = True;
}

/* ── struct auto-cleanup helpers ── */

/* Recursively free heap pointer fields and call nested struct destructors.
   This runs AFTER the user-defined rem() (if any).  Heap pointer fields are
   freed with free() — safe if null (user should null after manual rem.()).
   Nested struct-typed fields are fully cleaned up recursively. */
static void emit_struct_field_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val) {
    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
    for (usize_t i = 0; i < sr->field_count; i++) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) return;
        field_info_t *f = &sr->fields[i];

        /* auto-free heap pointer fields (free(NULL) is a no-op) */
        if (f->storage == StorageHeap && f->type.is_pointer) {
            LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type,
                                                    alloca_val, (unsigned)f->index, "hfld.gep");
            LLVMValueRef hptr = LLVMBuildLoad2(cg->builder, ptr_ty, gep, "hfld.ptr");
            LLVMValueRef fargs[1] = { hptr };
            LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, fargs, 1, "");
        }

        /* recursively clean up nested struct-typed (non-pointer) fields */
        if (f->type.base == TypeUser && !f->type.is_pointer && f->type.user_name) {
            struct_reg_t *nested = find_struct(cg, f->type.user_name);
            if (nested) {
                LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type,
                                                        alloca_val, (unsigned)f->index, "sfld.gep");
                emit_struct_cleanup(cg, nested, gep);
            }
        }
    }
}

/* Call the user's rem() destructor (if any), then auto-clean all fields. */
static void emit_struct_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) return;
    if (sr->destructor) {
        LLVMTypeRef fn_type = LLVMGlobalGetValueType(sr->destructor);
        LLVMValueRef args[1] = { alloca_val };
        LLVMBuildCall2(cg->builder, fn_type, sr->destructor, args, 1, "");
    }
    emit_struct_field_cleanup(cg, sr, alloca_val);
}

static void pop_dtor_scope(cg_t *cg);

/* Emit a defer with declaration-point name lookup and its own temporary
   lexical scope. Copy the symbol table, rather than truncating it in place:
   deferred declarations must not overwrite live symbols on another CFG edge.
   Values themselves are still read at cleanup time, not captured by value. */
static void emit_deferred_stmt(cg_t *cg, deferred_stmt_t deferred) {
    symtab_t saved_locals = cg->locals;
    symtab_init(&cg->locals);
    if (deferred.locals_count) {
        cg->locals.heap = allocate(deferred.locals_count, sizeof(symbol_t));
        cg->locals.entries = cg->locals.heap.pointer;
        cg->locals.count = cg->locals.capacity = deferred.locals_count;
        memcpy(cg->locals.entries, saved_locals.entries,
               deferred.locals_count * sizeof(symbol_t));
    }
    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_continue = cg->continue_target;
    usize_t saved_break_depth = cg->break_cleanup_depth;
    usize_t saved_continue_depth = cg->continue_cleanup_depth;
    boolean_t saved_emitting_defer = cg->emitting_defer;
    cg->break_target = cg->continue_target = Null;
    cg->break_cleanup_depth = cg->continue_cleanup_depth = cg->dtor_depth;
    cg->emitting_defer = True;
    push_dtor_scope(cg);
    gen_stmt(cg, deferred.stmt);
    pop_dtor_scope(cg);
    cg->emitting_defer = saved_emitting_defer;
    cg->break_target = saved_break;
    cg->continue_target = saved_continue;
    cg->break_cleanup_depth = saved_break_depth;
    cg->continue_cleanup_depth = saved_continue_depth;
    for (usize_t i = 0; i < deferred.locals_count; i++)
        if (cg->locals.entries[i].used) saved_locals.entries[i].used = True;
    symtab_free(&cg->locals);
    cg->locals = saved_locals;
}

static void emit_dtor_calls_except(cg_t *cg, dtor_scope_t *scope,
                                   LLVMValueRef *moved, usize_t moved_count) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;
    /* Deferred blocks can grow/reallocate dtor_stack. Keep a value snapshot,
       never a pointer into that array across recursive statement generation. */
    dtor_scope_t snapshot = *scope;
    /* run deferred statements in LIFO order first */
    for (usize_t i = snapshot.deferred_count; i > 0; i--) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) break;
        emit_deferred_stmt(cg, snapshot.deferred[i - 1]);
    }
    /* then run struct destructors in LIFO order */
    for (usize_t i = snapshot.count; i > 0; i--) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) break;
        dtor_var_t *dv = &snapshot.vars[i - 1];
        boolean_t transferred = False;
        for (usize_t j = 0; j < moved_count; j++)
            if (dv->alloca_val == moved[j]) transferred = True;
        if (transferred) continue;
        if (dv->is_zone) {
            LLVMValueRef args[1] = { dv->alloca_val };
            LLVMBuildCall2(cg->builder, cg->zone_free_type, cg->zone_free_fn, args, 1, "");
            continue;
        }
        if (dv->struct_name) {
            struct_reg_t *sr = find_struct(cg, dv->struct_name);
            /* emit_struct_cleanup calls rem() then auto-frees heap fields
               and recursively destroys nested struct-typed fields */
            if (sr) emit_struct_cleanup(cg, sr, dv->alloca_val);
        }
    }
}

static void emit_dtor_calls(cg_t *cg, dtor_scope_t *scope) {
    emit_dtor_calls_except(cg, scope, Null, 0);
}

/* Unwind only scopes exited by this CFG edge. Registrations are immutable:
   a return in one branch must not erase a destructor in another branch. */
static void emit_cleanup_to_depth(cg_t *cg, usize_t retained_depth,
                                  LLVMValueRef *moved, usize_t moved_count) {
    for (usize_t i = cg->dtor_depth; i > retained_depth; i--)
        emit_dtor_calls_except(cg, &cg->dtor_stack[i - 1], moved, moved_count);
}

static void pop_dtor_scope(cg_t *cg) {
    if (cg->dtor_depth == 0) return;
    emit_dtor_calls(cg, &cg->dtor_stack[cg->dtor_depth - 1]);
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    if (scope->heap.pointer != Null) deallocate(scope->heap);
    if (scope->deferred_heap.pointer != Null) deallocate(scope->deferred_heap);
    cg->dtor_depth--;
}
