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
            scope->deferred_heap = allocate(new_cap, sizeof(node_t *));
        else
            scope->deferred_heap = reallocate(scope->deferred_heap, new_cap * sizeof(node_t *));
        scope->deferred = scope->deferred_heap.pointer;
        scope->deferred_cap = new_cap;
    }
    scope->deferred[scope->deferred_count++] = stmt;
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
    scope->vars[scope->count].alloca_val = alloca_val;
    scope->vars[scope->count].struct_name = (char *)struct_name;
    scope->vars[scope->count].is_heap_alloc = False;
    scope->count++;
}

static void add_heap_var(cg_t *cg, LLVMValueRef alloca_val) {
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
    scope->vars[scope->count].alloca_val = alloca_val;
    scope->vars[scope->count].struct_name = Null;
    scope->vars[scope->count].is_heap_alloc = True;
    scope->count++;
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

static void emit_dtor_calls(cg_t *cg, dtor_scope_t *scope) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;
    /* run deferred statements in LIFO order first */
    for (usize_t i = scope->deferred_count; i > 0; i--) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) break;
        gen_stmt(cg, scope->deferred[i - 1]);
    }
    /* then run destructors / heap-var frees in LIFO order */
    for (usize_t i = scope->count; i > 0; i--) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) break;
        dtor_var_t *dv = &scope->vars[i - 1];
        if (dv->is_heap_alloc) {
            /* free heap-allocated primitive (free(null) is safe, no branch needed) */
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty,
                                                    dv->alloca_val, "hptr");
            LLVMValueRef args[1] = { heap_ptr };
            LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, args, 1, "");
        } else if (dv->struct_name) {
            struct_reg_t *sr = find_struct(cg, dv->struct_name);
            /* emit_struct_cleanup calls rem() then auto-frees heap fields
               and recursively destroys nested struct-typed fields */
            if (sr) emit_struct_cleanup(cg, sr, dv->alloca_val);
        }
    }
}

static void emit_all_dtor_calls(cg_t *cg) {
    for (usize_t i = cg->dtor_depth; i > 0; i--)
        emit_dtor_calls(cg, &cg->dtor_stack[i - 1]);
}

/* Remove a variable from all dtor scopes so its destructor isn't called.
   Used when a struct is returned by value (moved to caller). */
static void remove_from_dtor_scopes(cg_t *cg, LLVMValueRef alloca_val) {
    for (usize_t d = 0; d < cg->dtor_depth; d++) {
        dtor_scope_t *scope = &cg->dtor_stack[d];
        for (usize_t i = 0; i < scope->count; i++) {
            if (scope->vars[i].alloca_val == alloca_val) {
                for (usize_t j = i; j + 1 < scope->count; j++)
                    scope->vars[j] = scope->vars[j + 1];
                scope->count--;
                return;
            }
        }
    }
}

static void pop_dtor_scope(cg_t *cg) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    emit_dtor_calls(cg, scope);
    if (scope->heap.pointer != Null) deallocate(scope->heap);
    if (scope->deferred_heap.pointer != Null) deallocate(scope->deferred_heap);
    cg->dtor_depth--;
}
