/* cg_signals.c — type-routed signal dispatch (watch/send/quit).
 *
 * Per-type globals are emitted linkonce_odr so multiple TUs share storage.
 * Shared helpers (register/dereg/dispatch/quit) are also linkonce_odr.
 * Handler fn signature: void(ptr arg, ptr data_gv, ptr lock_gv, i64 index).
 * A handler `break` lowers to a call into __stasha_watch_dereg + ret.
 */

/* ── signal type key derivation ──
 * Produces a unique string per LLVM type so storage symbol names collide
 * exactly when the static types match.  Primitive types use their name
 * ("i32"/"f64"/"bool"); user struct types use their mangled LLVM struct
 * name (e.g. "ex_signals__sig_t"). */
static char *signal_key_from_llvmtype(cg_t *cg, LLVMTypeRef ty) {
    (void)cg;
    char buf[256];
    switch (LLVMGetTypeKind(ty)) {
    case LLVMIntegerTypeKind: {
        unsigned bits = LLVMGetIntTypeWidth(ty);
        if (bits == 1) { snprintf(buf, sizeof(buf), "bool"); break; }
        snprintf(buf, sizeof(buf), "i%u", bits);
        break;
    }
    case LLVMFloatTypeKind:   snprintf(buf, sizeof(buf), "f32");   break;
    case LLVMDoubleTypeKind:  snprintf(buf, sizeof(buf), "f64");   break;
    case LLVMStructTypeKind: {
        const char *n = LLVMGetStructName(ty);
        snprintf(buf, sizeof(buf), "%s", n ? n : "__anon");
        break;
    }
    case LLVMPointerTypeKind: snprintf(buf, sizeof(buf), "ptr");   break;
    default:                  snprintf(buf, sizeof(buf), "other"); break;
    }
    return ast_strdup(buf, strlen(buf));
}

/* ── per-type storage lookup / creation ── */

static signal_storage_t *find_signal_storage(cg_t *cg, const char *mt) {
    for (usize_t i = 0; i < cg->signal_storage_count; i++)
        if (strcmp(cg->signal_storages[i].mt, mt) == 0)
            return &cg->signal_storages[i];
    return Null;
}

static signal_storage_t *ensure_signal_storage(cg_t *cg, const char *mt) {
    signal_storage_t *found = find_signal_storage(cg, mt);
    if (found) return found;

    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef i64_t = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);

    signal_storage_t entry = {0};
    entry.mt = ast_strdup(mt, strlen(mt));

    char name[512];
    snprintf(name, sizeof(name), "__stasha_watch_data_%s", mt);
    entry.data_gv = LLVMAddGlobal(cg->module, ptr_t, name);
    LLVMSetLinkage(entry.data_gv, LLVMLinkOnceODRLinkage);
    LLVMSetInitializer(entry.data_gv, LLVMConstNull(ptr_t));

    snprintf(name, sizeof(name), "__stasha_watch_len_%s", mt);
    entry.len_gv = LLVMAddGlobal(cg->module, i64_t, name);
    LLVMSetLinkage(entry.len_gv, LLVMLinkOnceODRLinkage);
    LLVMSetInitializer(entry.len_gv, LLVMConstInt(i64_t, 0, 0));

    snprintf(name, sizeof(name), "__stasha_watch_cap_%s", mt);
    entry.cap_gv = LLVMAddGlobal(cg->module, i64_t, name);
    LLVMSetLinkage(entry.cap_gv, LLVMLinkOnceODRLinkage);
    LLVMSetInitializer(entry.cap_gv, LLVMConstInt(i64_t, 0, 0));

    snprintf(name, sizeof(name), "__stasha_watch_lock_%s", mt);
    entry.lock_gv = LLVMAddGlobal(cg->module, i32_t, name);
    LLVMSetLinkage(entry.lock_gv, LLVMLinkOnceODRLinkage);
    LLVMSetInitializer(entry.lock_gv, LLVMConstInt(i32_t, 0, 0));

    if (cg->signal_storage_count >= cg->signal_storage_cap) {
        usize_t new_cap = cg->signal_storage_cap ? cg->signal_storage_cap * 2 : 8;
        heap_t  nh = allocate(new_cap, sizeof(signal_storage_t));
        if (cg->signal_storage_count > 0)
            memcpy(nh.pointer, cg->signal_storages,
                   cg->signal_storage_count * sizeof(signal_storage_t));
        if (cg->signal_storage_cap > 0) deallocate(cg->signal_storage_heap);
        cg->signal_storages     = nh.pointer;
        cg->signal_storage_cap  = new_cap;
        cg->signal_storage_heap = nh;
    }
    cg->signal_storages[cg->signal_storage_count++] = entry;
    return &cg->signal_storages[cg->signal_storage_count - 1];
}

/* ── inline spinlock ── */

static void emit_spin_acquire(cg_t *cg, LLVMValueRef lock_ptr) {
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMBasicBlockRef loop_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "spin.acq");
    LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "spin.done");
    LLVMBuildBr(cg->builder, loop_bb);
    LLVMPositionBuilderAtEnd(cg->builder, loop_bb);
    LLVMValueRef old = LLVMBuildAtomicRMW(cg->builder, LLVMAtomicRMWBinOpXchg,
        lock_ptr, LLVMConstInt(i32_t, 1, 0),
        LLVMAtomicOrderingSequentiallyConsistent, 0);
    LLVMValueRef acquired = LLVMBuildICmp(cg->builder, LLVMIntEQ, old,
        LLVMConstInt(i32_t, 0, 0), "acq");
    LLVMBuildCondBr(cg->builder, acquired, done_bb, loop_bb);
    LLVMPositionBuilderAtEnd(cg->builder, done_bb);
}

static void emit_spin_release(cg_t *cg, LLVMValueRef lock_ptr) {
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMBuildAtomicRMW(cg->builder, LLVMAtomicRMWBinOpXchg,
        lock_ptr, LLVMConstInt(i32_t, 0, 0),
        LLVMAtomicOrderingSequentiallyConsistent, 0);
}

/* ── libc exit / _Exit declarations (lazy) ── */

static LLVMValueRef get_exit_fn(cg_t *cg) {
    if (cg->exit_fn) return cg->exit_fn;
    LLVMTypeRef i32_t  = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef params[1] = { i32_t };
    cg->exit_type = LLVMFunctionType(void_t, params, 1, 0);
    cg->exit_fn   = LLVMAddFunction(cg->module, "exit", cg->exit_type);
    return cg->exit_fn;
}

static LLVMValueRef get_underscore_exit_fn(cg_t *cg) {
    if (cg->underscore_exit_fn) return cg->underscore_exit_fn;
    LLVMTypeRef i32_t  = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef params[1] = { i32_t };
    cg->underscore_exit_type = LLVMFunctionType(void_t, params, 1, 0);
    cg->underscore_exit_fn   = LLVMAddFunction(cg->module, "_Exit", cg->underscore_exit_type);
    return cg->underscore_exit_fn;
}

/* fflush(NULL) flushes all open output streams.  Called before _Exit so
   buffered stdout (e.g. print.() output from an @[[exit]] block) isn't
   lost when the re-entry guard demotes to _Exit. */
static LLVMValueRef get_fflush_fn(cg_t *cg) {
    if (cg->fflush_fn) return cg->fflush_fn;
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef params[1] = { ptr_t };
    cg->fflush_type = LLVMFunctionType(i32_t, params, 1, 0);
    cg->fflush_fn   = LLVMAddFunction(cg->module, "fflush", cg->fflush_type);
    return cg->fflush_fn;
}

/* ── shared helper: __stasha_watch_register ──
 * void __stasha_watch_register(ptr data_gv, ptr len_gv, ptr cap_gv,
 *                              ptr lock_gv, ptr fn)
 * Grows the fn-pointer array (realloc-doubling) and appends fn under lock. */
static LLVMValueRef ensure_watch_register_fn(cg_t *cg) {
    if (cg->watch_register_fn) return cg->watch_register_fn;

    LLVMTypeRef ptr_t  = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef params[5] = { ptr_t, ptr_t, ptr_t, ptr_t, ptr_t };

    cg->watch_register_type = LLVMFunctionType(void_t, params, 5, 0);
    cg->watch_register_fn   = LLVMAddFunction(cg->module,
        "__stasha_watch_register", cg->watch_register_type);
    LLVMSetLinkage(cg->watch_register_fn, LLVMLinkOnceODRLinkage);

    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef      saved_fn = cg->current_fn;
    cg->current_fn = cg->watch_register_fn;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_register_fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    LLVMValueRef data_gv = LLVMGetParam(cg->watch_register_fn, 0);
    LLVMValueRef len_gv  = LLVMGetParam(cg->watch_register_fn, 1);
    LLVMValueRef cap_gv  = LLVMGetParam(cg->watch_register_fn, 2);
    LLVMValueRef lock_gv = LLVMGetParam(cg->watch_register_fn, 3);
    LLVMValueRef fn_ptr  = LLVMGetParam(cg->watch_register_fn, 4);

    emit_spin_acquire(cg, lock_gv);

    LLVMValueRef len = LLVMBuildLoad2(cg->builder, i64_t, len_gv, "len");
    LLVMValueRef cap = LLVMBuildLoad2(cg->builder, i64_t, cap_gv, "cap");

    LLVMValueRef need_grow = LLVMBuildICmp(cg->builder, LLVMIntEQ, len, cap, "need_grow");
    LLVMBasicBlockRef grow_bb  = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_register_fn, "grow");
    LLVMBasicBlockRef store_bb = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_register_fn, "store");
    LLVMBuildCondBr(cg->builder, need_grow, grow_bb, store_bb);

    LLVMPositionBuilderAtEnd(cg->builder, grow_bb);
    LLVMValueRef cap_is_zero = LLVMBuildICmp(cg->builder, LLVMIntEQ, cap,
        LLVMConstInt(i64_t, 0, 0), "capz");
    LLVMValueRef cap_doubled = LLVMBuildMul(cg->builder, cap,
        LLVMConstInt(i64_t, 2, 0), "capx2");
    LLVMValueRef new_cap = LLVMBuildSelect(cg->builder, cap_is_zero,
        LLVMConstInt(i64_t, 4, 0), cap_doubled, "new_cap");
    LLVMValueRef bytes = LLVMBuildMul(cg->builder, new_cap,
        LLVMConstInt(i64_t, 8, 0), "bytes");
    LLVMValueRef old_data = LLVMBuildLoad2(cg->builder, ptr_t, data_gv, "old_data");
    LLVMValueRef realloc_args[2] = { old_data, bytes };
    LLVMValueRef new_data = LLVMBuildCall2(cg->builder, cg->realloc_type,
        cg->realloc_fn, realloc_args, 2, "new_data");
    LLVMBuildStore(cg->builder, new_data, data_gv);
    LLVMBuildStore(cg->builder, new_cap, cap_gv);
    LLVMBuildBr(cg->builder, store_bb);

    LLVMPositionBuilderAtEnd(cg->builder, store_bb);
    LLVMValueRef data = LLVMBuildLoad2(cg->builder, ptr_t, data_gv, "data");
    LLVMValueRef len2 = LLVMBuildLoad2(cg->builder, i64_t, len_gv, "len2");
    LLVMValueRef slot_ptr = LLVMBuildGEP2(cg->builder, ptr_t, data, &len2, 1, "slot_ptr");
    LLVMBuildStore(cg->builder, fn_ptr, slot_ptr);
    LLVMValueRef new_len = LLVMBuildAdd(cg->builder, len2,
        LLVMConstInt(i64_t, 1, 0), "new_len");
    LLVMBuildStore(cg->builder, new_len, len_gv);

    emit_spin_release(cg, lock_gv);
    LLVMBuildRetVoid(cg->builder);

    cg->current_fn = saved_fn;
    if (saved_bb) LLVMPositionBuilderAtEnd(cg->builder, saved_bb);
    return cg->watch_register_fn;
}

/* ── shared helper: __stasha_watch_dereg ──
 * void __stasha_watch_dereg(ptr data_gv, ptr lock_gv, i64 index)
 * Sets slots[index] = null under the lock (survives realloc). */
static LLVMValueRef ensure_watch_dereg_fn(cg_t *cg) {
    if (cg->watch_dereg_fn) return cg->watch_dereg_fn;

    LLVMTypeRef ptr_t  = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef params[3] = { ptr_t, ptr_t, i64_t };

    cg->watch_dereg_type = LLVMFunctionType(void_t, params, 3, 0);
    cg->watch_dereg_fn   = LLVMAddFunction(cg->module,
        "__stasha_watch_dereg", cg->watch_dereg_type);
    LLVMSetLinkage(cg->watch_dereg_fn, LLVMLinkOnceODRLinkage);

    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef      saved_fn = cg->current_fn;
    cg->current_fn = cg->watch_dereg_fn;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_dereg_fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    LLVMValueRef data_gv = LLVMGetParam(cg->watch_dereg_fn, 0);
    LLVMValueRef lock_gv = LLVMGetParam(cg->watch_dereg_fn, 1);
    LLVMValueRef index   = LLVMGetParam(cg->watch_dereg_fn, 2);

    emit_spin_acquire(cg, lock_gv);
    LLVMValueRef data = LLVMBuildLoad2(cg->builder, ptr_t, data_gv, "data");
    LLVMValueRef slot_ptr = LLVMBuildGEP2(cg->builder, ptr_t, data, &index, 1, "slot_ptr");
    LLVMBuildStore(cg->builder, LLVMConstNull(ptr_t), slot_ptr);
    emit_spin_release(cg, lock_gv);
    LLVMBuildRetVoid(cg->builder);

    cg->current_fn = saved_fn;
    if (saved_bb) LLVMPositionBuilderAtEnd(cg->builder, saved_bb);
    return cg->watch_dereg_fn;
}

/* ── shared helper: __stasha_watch_dispatch ──
 * void __stasha_watch_dispatch(ptr data_gv, ptr len_gv, ptr lock_gv, ptr arg)
 * Snapshot-then-iterate; each iter re-reads slot under lock, calls fn
 * outside lock (handler may reentrantly register/send/break). */
static LLVMValueRef ensure_watch_dispatch_fn(cg_t *cg) {
    if (cg->watch_dispatch_fn) return cg->watch_dispatch_fn;

    LLVMTypeRef ptr_t  = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef params[4] = { ptr_t, ptr_t, ptr_t, ptr_t };

    cg->watch_dispatch_type = LLVMFunctionType(void_t, params, 4, 0);
    cg->watch_dispatch_fn   = LLVMAddFunction(cg->module,
        "__stasha_watch_dispatch", cg->watch_dispatch_type);
    LLVMSetLinkage(cg->watch_dispatch_fn, LLVMLinkOnceODRLinkage);

    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef      saved_fn = cg->current_fn;
    cg->current_fn = cg->watch_dispatch_fn;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_dispatch_fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    LLVMValueRef data_gv = LLVMGetParam(cg->watch_dispatch_fn, 0);
    LLVMValueRef len_gv  = LLVMGetParam(cg->watch_dispatch_fn, 1);
    LLVMValueRef lock_gv = LLVMGetParam(cg->watch_dispatch_fn, 2);
    LLVMValueRef arg     = LLVMGetParam(cg->watch_dispatch_fn, 3);

    /* snapshot len under lock */
    emit_spin_acquire(cg, lock_gv);
    LLVMValueRef snap_len = LLVMBuildLoad2(cg->builder, i64_t, len_gv, "snap_len");
    emit_spin_release(cg, lock_gv);

    LLVMValueRef i_alloca = LLVMBuildAlloca(cg->builder, i64_t, "i");
    LLVMBuildStore(cg->builder, LLVMConstInt(i64_t, 0, 0), i_alloca);

    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_dispatch_fn, "loop.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_dispatch_fn, "loop.body");
    LLVMBasicBlockRef call_bb = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_dispatch_fn, "loop.call");
    LLVMBasicBlockRef step_bb = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_dispatch_fn, "loop.step");
    LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->watch_dispatch_fn, "loop.exit");
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef i_val = LLVMBuildLoad2(cg->builder, i64_t, i_alloca, "i.v");
    LLVMValueRef cond = LLVMBuildICmp(cg->builder, LLVMIntULT, i_val, snap_len, "cond");
    LLVMBuildCondBr(cg->builder, cond, body_bb, exit_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    emit_spin_acquire(cg, lock_gv);
    LLVMValueRef data = LLVMBuildLoad2(cg->builder, ptr_t, data_gv, "data");
    LLVMValueRef i_val2 = LLVMBuildLoad2(cg->builder, i64_t, i_alloca, "i.v2");
    LLVMValueRef slot_ptr = LLVMBuildGEP2(cg->builder, ptr_t, data, &i_val2, 1, "slot_ptr");
    LLVMValueRef fn_ptr = LLVMBuildLoad2(cg->builder, ptr_t, slot_ptr, "fn_ptr");
    emit_spin_release(cg, lock_gv);

    LLVMValueRef is_null = LLVMBuildICmp(cg->builder, LLVMIntEQ, fn_ptr,
        LLVMConstNull(ptr_t), "is_null");
    LLVMBuildCondBr(cg->builder, is_null, step_bb, call_bb);

    LLVMPositionBuilderAtEnd(cg->builder, call_bb);
    LLVMTypeRef handler_params[4] = { ptr_t, ptr_t, ptr_t, i64_t };
    LLVMTypeRef handler_type = LLVMFunctionType(void_t, handler_params, 4, 0);
    LLVMValueRef call_args[4] = { arg, data_gv, lock_gv, i_val2 };
    LLVMBuildCall2(cg->builder, handler_type, fn_ptr, call_args, 4, "");
    LLVMBuildBr(cg->builder, step_bb);

    LLVMPositionBuilderAtEnd(cg->builder, step_bb);
    LLVMValueRef i_cur = LLVMBuildLoad2(cg->builder, i64_t, i_alloca, "i.cur");
    LLVMValueRef i_next = LLVMBuildAdd(cg->builder, i_cur,
        LLVMConstInt(i64_t, 1, 0), "i.next");
    LLVMBuildStore(cg->builder, i_next, i_alloca);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, exit_bb);
    LLVMBuildRetVoid(cg->builder);

    cg->current_fn = saved_fn;
    if (saved_bb) LLVMPositionBuilderAtEnd(cg->builder, saved_bb);
    return cg->watch_dispatch_fn;
}

/* ── shared helper: __stasha_quit ──
 * void __stasha_quit(i32 code)
 * Sets a re-entry guard atomic flag: first call → exit(code) (runs
 * @llvm.global_dtors and thus @[[exit]] blocks); nested call from inside
 * an @[[exit]] block → _Exit(code) (skip further dtors, avoid recursion). */
static LLVMValueRef ensure_quit_fn(cg_t *cg) {
    if (cg->quit_fn) return cg->quit_fn;

    LLVMTypeRef i32_t  = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);

    cg->quitting_gv = LLVMAddGlobal(cg->module, i32_t, "__stasha_quitting");
    LLVMSetLinkage(cg->quitting_gv, LLVMLinkOnceODRLinkage);
    LLVMSetInitializer(cg->quitting_gv, LLVMConstInt(i32_t, 0, 0));

    LLVMTypeRef params[1] = { i32_t };
    cg->quit_type = LLVMFunctionType(void_t, params, 1, 0);
    cg->quit_fn   = LLVMAddFunction(cg->module, "__stasha_quit", cg->quit_type);
    LLVMSetLinkage(cg->quit_fn, LLVMLinkOnceODRLinkage);

    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef      saved_fn = cg->current_fn;
    cg->current_fn = cg->quit_fn;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->quit_fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    LLVMValueRef code = LLVMGetParam(cg->quit_fn, 0);
    LLVMValueRef prev = LLVMBuildAtomicRMW(cg->builder, LLVMAtomicRMWBinOpXchg,
        cg->quitting_gv, LLVMConstInt(i32_t, 1, 0),
        LLVMAtomicOrderingSequentiallyConsistent, 0);
    LLVMValueRef first = LLVMBuildICmp(cg->builder, LLVMIntEQ, prev,
        LLVMConstInt(i32_t, 0, 0), "first");

    LLVMBasicBlockRef first_bb   = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->quit_fn, "first");
    LLVMBasicBlockRef reentry_bb = LLVMAppendBasicBlockInContext(cg->ctx,
        cg->quit_fn, "reentry");
    LLVMBuildCondBr(cg->builder, first, first_bb, reentry_bb);

    LLVMPositionBuilderAtEnd(cg->builder, first_bb);
    LLVMValueRef exit_fn = get_exit_fn(cg);
    LLVMValueRef args1[1] = { code };
    LLVMBuildCall2(cg->builder, cg->exit_type, exit_fn, args1, 1, "");
    LLVMBuildUnreachable(cg->builder);

    LLVMPositionBuilderAtEnd(cg->builder, reentry_bb);
    LLVMValueRef fflush_fn = get_fflush_fn(cg);
    LLVMTypeRef  ptr_t     = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMValueRef null_file[1] = { LLVMConstNull(ptr_t) };
    LLVMBuildCall2(cg->builder, cg->fflush_type, fflush_fn, null_file, 1, "");
    LLVMValueRef _exit_fn = get_underscore_exit_fn(cg);
    LLVMValueRef args2[1] = { code };
    LLVMBuildCall2(cg->builder, cg->underscore_exit_type, _exit_fn, args2, 1, "");
    LLVMBuildUnreachable(cg->builder);

    cg->current_fn = saved_fn;
    if (saved_bb) LLVMPositionBuilderAtEnd(cg->builder, saved_bb);
    return cg->quit_fn;
}

/* ── gen_watch_stmt ──
 * Synthesizes a top-level handler function with signature
 *   void(ptr arg, ptr data_gv, ptr lock_gv, i64 index)
 * whose body is the user's block.  `break` inside the body lowers to
 * a jump to a synthesized dereg-and-return block.  Emits a call to the
 * shared register helper at the watch site. */
static void gen_watch_stmt(cg_t *cg, node_t *node) {
    type_info_t ti = node->as.watch_stmt.type;
    ti = resolve_alias(cg, ti);
    LLVMTypeRef t_llvm = get_llvm_type(cg, ti);
    if (!t_llvm) {
        diag_begin_error("watch: unknown type in handler");
        diag_span(DIAG_NODE(node), True, "in watch handler type");
        diag_finish();
        return;
    }

    char *mt = signal_key_from_llvmtype(cg, t_llvm);
    signal_storage_t *st = ensure_signal_storage(cg, mt);
    LLVMValueRef reg_fn = ensure_watch_register_fn(cg);
    ensure_watch_dereg_fn(cg);  /* used from break inside handler */

    LLVMTypeRef ptr_t  = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef hparams[4] = { ptr_t, ptr_t, ptr_t, i64_t };
    LLVMTypeRef htype = LLVMFunctionType(void_t, hparams, 4, 0);

    char hname[256];
    snprintf(hname, sizeof(hname), "__stasha_handler_%s_%lu",
             mt, (unsigned long)(cg->signal_watcher_counter++));
    LLVMValueRef hfn = LLVMAddFunction(cg->module, hname, htype);
    LLVMSetLinkage(hfn, LLVMInternalLinkage);

    /* save outer state */
    LLVMBasicBlockRef saved_bb       = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef      saved_fn       = cg->current_fn;
    LLVMBasicBlockRef saved_break    = cg->break_target;
    LLVMBasicBlockRef saved_continue = cg->continue_target;
    usize_t           saved_locals   = cg->locals.count;
    usize_t           saved_dtor     = cg->dtor_depth;

    cg->current_fn   = hfn;
    cg->locals.count = 0;
    cg->dtor_depth   = 0;

    LLVMBasicBlockRef entry    = LLVMAppendBasicBlockInContext(cg->ctx, hfn, "entry");
    LLVMBasicBlockRef dereg_bb = LLVMAppendBasicBlockInContext(cg->ctx, hfn, "watch.dereg");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    LLVMValueRef arg     = LLVMGetParam(hfn, 0);
    LLVMValueRef data_gv = LLVMGetParam(hfn, 1);
    LLVMValueRef lock_gv = LLVMGetParam(hfn, 2);
    LLVMValueRef index   = LLVMGetParam(hfn, 3);

    /* stack-copy the param so user reads behave as if it were a local value */
    LLVMValueRef param_alloca = LLVMBuildAlloca(cg->builder, t_llvm,
        node->as.watch_stmt.param_name);
    LLVMValueRef loaded = LLVMBuildLoad2(cg->builder, t_llvm, arg, "arg.v");
    LLVMBuildStore(cg->builder, loaded, param_alloca);

    symtab_add(&cg->locals, node->as.watch_stmt.param_name, param_alloca,
               t_llvm, ti, 0);
    symtab_set_last_storage(&cg->locals, StorageStack, False);
    symtab_set_last_extra(&cg->locals, False, False, LinkageNone,
                          cg->dtor_depth, -1);
    symtab_set_last_line(&cg->locals, node->line);

    cg->break_target    = dereg_bb;
    cg->continue_target = Null;

    gen_block(cg, node->as.watch_stmt.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildRetVoid(cg->builder);

    /* dereg_bb: call __stasha_watch_dereg(data_gv, lock_gv, index); ret */
    LLVMPositionBuilderAtEnd(cg->builder, dereg_bb);
    LLVMValueRef dereg_args[3] = { data_gv, lock_gv, index };
    LLVMBuildCall2(cg->builder, cg->watch_dereg_type, cg->watch_dereg_fn,
                   dereg_args, 3, "");
    LLVMBuildRetVoid(cg->builder);

    /* restore outer state */
    cg->current_fn      = saved_fn;
    cg->break_target    = saved_break;
    cg->continue_target = saved_continue;
    cg->locals.count    = saved_locals;
    cg->dtor_depth      = saved_dtor;
    if (saved_bb) LLVMPositionBuilderAtEnd(cg->builder, saved_bb);

    /* register the handler at the watch site */
    LLVMValueRef reg_args[5] = {
        st->data_gv, st->len_gv, st->cap_gv, st->lock_gv, hfn
    };
    LLVMBuildCall2(cg->builder, cg->watch_register_type, reg_fn, reg_args, 5, "");
}

/* ── gen_send_stmt ──
 * Evaluates the expression, stores into a stack temp, calls the shared
 * dispatch helper with the per-type storage globals. */
static void gen_send_stmt(cg_t *cg, node_t *node) {
    LLVMValueRef val = gen_expr(cg, node->as.send_stmt.value);
    if (!val) return;
    LLVMTypeRef t_llvm = LLVMTypeOf(val);
    char *mt = signal_key_from_llvmtype(cg, t_llvm);
    signal_storage_t *st = ensure_signal_storage(cg, mt);
    LLVMValueRef disp_fn = ensure_watch_dispatch_fn(cg);

    LLVMValueRef tmp = alloc_in_entry(cg, t_llvm, "send.tmp");
    LLVMBuildStore(cg->builder, val, tmp);

    LLVMValueRef args[4] = { st->data_gv, st->len_gv, st->lock_gv, tmp };
    LLVMBuildCall2(cg->builder, cg->watch_dispatch_type, disp_fn, args, 4, "");
}

/* ── gen_quit_stmt ──
 * Lowers to __stasha_quit(code) followed by unreachable. */
static void gen_quit_stmt(cg_t *cg, node_t *node) {
    LLVMValueRef code_val = gen_expr(cg, node->as.quit_stmt.code);
    if (!code_val) return;
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    code_val = coerce_int(cg, code_val, i32_t);
    LLVMValueRef quit_fn = ensure_quit_fn(cg);
    LLVMValueRef args[1] = { code_val };
    LLVMBuildCall2(cg->builder, cg->quit_type, quit_fn, args, 1, "");
    LLVMBuildUnreachable(cg->builder);
}
