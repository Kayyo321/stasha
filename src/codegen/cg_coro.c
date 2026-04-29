/* ── stream coroutine lowering ─────────────────────────────────────────────
 *
 * `async fn` declared with `stream.[T]` return type and using `yield` is
 * lowered as a real LLVM coroutine via `llvm.coro.*` intrinsics.  The fn
 * is "eager-start": calling it runs the body up to the first suspend point
 * (or to completion, if the body has none) and returns the coroutine handle
 * — a plain `ptr` that stasha sees as the `stream.[T]` value.
 *
 * The runtime side is intentionally minimal.  Consumer-side primitives —
 * `await.next(s)` / `stream.done(s)` / `stream.drop(s)` — drive the
 * coroutine synchronously on the calling thread by calling `llvm.coro.resume`
 * in a loop until the producer yields, hits eos, or finishes.  Multi-threaded
 * executors are a v2 concern.
 *
 * Promise layout — every stream coroutine stores its state in an alloca whose
 * address is registered with `llvm.coro.id`.  After `coro-split`, the alloca
 * lives in the coroutine frame and can be reached from the outside through
 * `llvm.coro.promise(handle, alignment, false)` (from_promise = false).  The header
 * is fixed-size (40 bytes on 64-bit), and the inline item slot follows it:
 *
 *     %__sts_coro_prom_hdr = type {
 *         i32, i32, i32, i32, i32, i32, ptr, ptr
 *     ;   complete, eos, item_ready, has_error, is_stream, padding,
 *     ;   continuation, error_msg
 *     }
 *     %__sts_stream_prom_<T> = type { %__sts_coro_prom_hdr, T }
 *
 * Field offsets used by emitted IR — keep in sync with coro_runtime.h:
 *     hdr.complete     = 0   hdr.has_error  = 12
 *     hdr.eos          = 4   hdr.is_stream  = 16
 *     hdr.item_ready   = 8   hdr.padding    = 20
 *     hdr.continuation = 24  hdr.error_msg  = 32
 *     item                       = 40 (T-aligned)
 *
 * ── lowering shape (eager-start) ────────────────────────────────────────────
 *
 *   entry:                      ; param allocas, promise alloca, init fields
 *     %prom = alloca %prom_T
 *     ; init promise fields (complete=0, eos=0, item_ready=0, is_stream=1, ...)
 *     %id = call token @llvm.coro.id(i32 8, ptr %prom, ptr null, ptr null)
 *     %need = call i1 @llvm.coro.alloc(token %id)
 *     br i1 %need, label %coro.alloc, label %coro.begin
 *   coro.alloc:
 *     %sz = call i64 @llvm.coro.size.i64()
 *     %frame = call ptr @malloc(i64 %sz)
 *     br label %coro.begin
 *   coro.begin:
 *     %fp = phi ptr [null, %entry], [%frame, %coro.alloc]
 *     %hdl = call ptr @llvm.coro.begin(token %id, ptr %fp)
 *     br label %coro.body
 *   coro.body:
 *     ; user body — yield/yield;/ret; lowered per-statement (see below)
 *
 *   coro.final_suspend:         ; reached on `ret;`
 *     store i32 1, ptr eos_ptr
 *     store i32 1, ptr complete_ptr
 *     %fs = call i8 @llvm.coro.suspend(token none, i1 true)
 *     switch i8 %fs, label %coro.suspend
 *       [i8 0, label %coro.unreach
 *        i8 1, label %coro.cleanup]
 *   coro.unreach:
 *     unreachable
 *   coro.cleanup:
 *     %mem = call ptr @llvm.coro.free(token %id, ptr %hdl)
 *     %hm  = icmp ne ptr %mem, null
 *     br i1 %hm, label %coro.dofree, label %coro.suspend
 *   coro.dofree:
 *     call void @free(ptr %mem)
 *     br label %coro.suspend
 *   coro.suspend:
 *     call void @llvm.coro.end(ptr %hdl, i1 false, token none)
 *     ret ptr %hdl
 *
 *   ; -- yield expr: store item, mark ready, suspend --
 *     store T %v, ptr item_ptr
 *     store i32 1, ptr item_ready_ptr
 *     %sp = call i8 @llvm.coro.suspend(token none, i1 false)
 *     switch i8 %sp, label %coro.suspend
 *       [i8 0, label %after_yield
 *        i8 1, label %coro.cleanup]
 *   after_yield:
 *
 *   ; -- yield;: bare cooperative reschedule, no item --
 *     %sp = call i8 @llvm.coro.suspend(token none, i1 false)
 *     switch i8 %sp, label %coro.suspend
 *       [i8 0, label %after_yield_now
 *        i8 1, label %coro.cleanup]
 * ── */

/* ── header constants — must match coro_runtime.h ── */
#define STS_PROM_OFF_COMPLETE      0u
#define STS_PROM_OFF_EOS           1u
#define STS_PROM_OFF_ITEM_READY    2u
#define STS_PROM_OFF_HAS_ERROR     3u
#define STS_PROM_OFF_IS_STREAM     4u
#define STS_PROM_OFF_PADDING       5u
#define STS_PROM_OFF_CONTINUATION  6u
#define STS_PROM_OFF_ERROR_MSG     7u
#define STS_PROM_HDR_NUM_FIELDS    8u
#define STS_PROM_ALIGN             8u

/* ── intrinsic getters ── */

static LLVMTypeRef sts_token_ty(cg_t *cg) {
    return LLVMTokenTypeInContext(cg->ctx);
}

static LLVMValueRef sts_get_or_decl_intrinsic(cg_t *cg, const char *name,
                                              LLVMTypeRef ret,
                                              LLVMTypeRef *params, unsigned npar) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
    if (fn) return fn;
    LLVMTypeRef ft = LLVMFunctionType(ret, params, npar, 0);
    return LLVMAddFunction(cg->module, name, ft);
}

static LLVMValueRef sts_intrinsic_coro_id(cg_t *cg) {
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef p[4] = { i32_t, ptr_t, ptr_t, ptr_t };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.id", sts_token_ty(cg), p, 4);
}

static LLVMValueRef sts_intrinsic_coro_alloc(cg_t *cg) {
    LLVMTypeRef p[1] = { sts_token_ty(cg) };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.alloc",
                                     LLVMInt1TypeInContext(cg->ctx), p, 1);
}

static LLVMValueRef sts_intrinsic_coro_size_i64(cg_t *cg) {
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.size.i64",
                                     LLVMInt64TypeInContext(cg->ctx), Null, 0);
}

static LLVMValueRef sts_intrinsic_coro_begin(cg_t *cg) {
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef p[2] = { sts_token_ty(cg), ptr_t };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.begin", ptr_t, p, 2);
}

static LLVMValueRef sts_intrinsic_coro_suspend(cg_t *cg) {
    LLVMTypeRef p[2] = { sts_token_ty(cg), LLVMInt1TypeInContext(cg->ctx) };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.suspend",
                                     LLVMInt8TypeInContext(cg->ctx), p, 2);
}

static LLVMValueRef sts_intrinsic_coro_free(cg_t *cg) {
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef p[2] = { sts_token_ty(cg), ptr_t };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.free", ptr_t, p, 2);
}

static LLVMValueRef sts_intrinsic_coro_end(cg_t *cg) {
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef p[3] = { ptr_t, LLVMInt1TypeInContext(cg->ctx), sts_token_ty(cg) };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.end",
                                     LLVMVoidTypeInContext(cg->ctx), p, 3);
}

static LLVMValueRef sts_intrinsic_coro_resume(cg_t *cg) {
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef p[1] = { ptr_t };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.resume",
                                     LLVMVoidTypeInContext(cg->ctx), p, 1);
}

static LLVMValueRef sts_intrinsic_coro_destroy(cg_t *cg) {
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef p[1] = { ptr_t };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.destroy",
                                     LLVMVoidTypeInContext(cg->ctx), p, 1);
}

static LLVMValueRef sts_intrinsic_coro_promise(cg_t *cg) {
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef p[3] = { ptr_t, LLVMInt32TypeInContext(cg->ctx),
                         LLVMInt1TypeInContext(cg->ctx) };
    return sts_get_or_decl_intrinsic(cg, "llvm.coro.promise", ptr_t, p, 3);
}

/* Look up (creating once) the standard `__sts_coro_prom_hdr` LLVM struct. */
static LLVMTypeRef sts_coro_hdr_type(cg_t *cg) {
    LLVMTypeRef cached = LLVMGetTypeByName2(cg->ctx, "__sts_coro_prom_hdr");
    if (cached) return cached;
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef fields[STS_PROM_HDR_NUM_FIELDS] = {
        i32_t, i32_t, i32_t, i32_t, i32_t, i32_t, ptr_t, ptr_t
    };
    LLVMTypeRef ty = LLVMStructCreateNamed(cg->ctx, "__sts_coro_prom_hdr");
    LLVMStructSetBody(ty, fields, STS_PROM_HDR_NUM_FIELDS, 0);
    return ty;
}

/* Build the per-T promise type `{ hdr, T }`.  Returns existing if already
 * registered.  T may be void → we substitute i8 placeholder. */
static LLVMTypeRef sts_stream_prom_type(cg_t *cg, type_info_t item_ti,
                                        LLVMTypeRef *out_item_lt) {
    LLVMTypeRef hdr = sts_coro_hdr_type(cg);
    LLVMTypeRef item_lt;
    if (item_ti.base == TypeVoid && !item_ti.is_pointer)
        item_lt = LLVMInt8TypeInContext(cg->ctx);
    else
        item_lt = get_llvm_type(cg, item_ti);
    if (out_item_lt) *out_item_lt = item_lt;

    /* Use an anonymous struct — distinct LLVM type instances are fine because
     * the layout is hashed by (hdr, item_lt), and LLVM dedupes structurally. */
    LLVMTypeRef fields[2] = { hdr, item_lt };
    return LLVMStructTypeInContext(cg->ctx, fields, 2, 0);
}

/* GEP into header field `fld` (0..7), returning a ptr suitable for
 * load/store of the field's i32/ptr type.  `prom_ty` must be the
 * outer `{hdr, T}` struct type. */
static LLVMValueRef sts_gep_hdr_field(cg_t *cg, LLVMTypeRef prom_ty,
                                      LLVMValueRef prom_ptr, unsigned fld) {
    LLVMValueRef idx[3] = {
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0),  /* prom_ty[0] = hdr */
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0),
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), fld, 0),
    };
    return LLVMBuildInBoundsGEP2(cg->builder, prom_ty, prom_ptr, idx, 3, "prom.hdr.fld");
}

/* Forward decl: defined below in the consumer-side helpers section. */
static LLVMValueRef sts_call_coro_promise(cg_t *cg, LLVMValueRef handle);

static LLVMValueRef sts_gep_item(cg_t *cg, LLVMTypeRef prom_ty,
                                 LLVMValueRef prom_ptr) {
    LLVMValueRef idx[2] = {
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0),
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0),  /* prom_ty[1] = item */
    };
    return LLVMBuildInBoundsGEP2(cg->builder, prom_ty, prom_ptr, idx, 2, "prom.item");
}

/* ── coroutine prologue / epilogue ── */

/* Emit the prologue (coro.id, optional alloc, coro.begin) at the current
 * insertion point.  Pre-creates the final_suspend / cleanup / suspend basic
 * blocks but leaves the builder positioned in a fresh "coro.body" block so
 * the caller can lower the user body afterwards.
 *
 * On return, *out_ctx is filled with all the state needed by yield/ret. */
static void sts_emit_coro_stream_prologue(cg_t *cg, type_info_t item_ti,
                                          sts_coro_ctx_t *out_ctx) {
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);

    /* CoroEarly requires the frontend to mark Switch-Resumed coroutines with
       the `presplitcoroutine` function attribute — without it, the pass
       skips the function entirely and CoroSplit never fires. */
    {
        unsigned attr_kind = LLVMGetEnumAttributeKindForName("presplitcoroutine",
                                                             strlen("presplitcoroutine"));
        if (attr_kind != 0) {
            LLVMAttributeRef attr = LLVMCreateEnumAttribute(cg->ctx, attr_kind, 0);
            LLVMAddAttributeAtIndex(cg->current_fn,
                                    LLVMAttributeFunctionIndex, attr);
        }
    }

    /* allocate promise alloca in entry block (alongside other allocas).
       Init stores are emitted AFTER `coro.begin` — the CoroEarly pass rewrites
       loads/stores through the promise alloca to `llvm.coro.promise(hdl, ...)`,
       which requires hdl to dominate the use.  Initializing earlier breaks
       domination after the pass runs. */
    LLVMTypeRef item_lt;
    LLVMTypeRef prom_ty = sts_stream_prom_type(cg, item_ti, &item_lt);
    LLVMTypeRef hdr_ty  = sts_coro_hdr_type(cg);
    LLVMValueRef prom   = alloc_in_entry(cg, prom_ty, "coro.prom");

    /* coro.id */
    LLVMValueRef id_args[4] = {
        LLVMConstInt(i32_t, STS_PROM_ALIGN, 0),
        prom,
        LLVMConstNull(ptr_t),
        LLVMConstNull(ptr_t),
    };
    LLVMValueRef id_token = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_id(cg)),
        sts_intrinsic_coro_id(cg), id_args, 4, "coro.id");

    /* coro.alloc */
    LLVMValueRef alloc_args[1] = { id_token };
    LLVMValueRef need_alloc = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_alloc(cg)),
        sts_intrinsic_coro_alloc(cg), alloc_args, 1, "coro.need_alloc");

    /* basic blocks */
    LLVMBasicBlockRef bb_alloc = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.alloc");
    LLVMBasicBlockRef bb_begin = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.begin");
    LLVMBasicBlockRef bb_body  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.body");
    LLVMBasicBlockRef bb_final = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.final_suspend");
    LLVMBasicBlockRef bb_unr   = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.unreach");
    LLVMBasicBlockRef bb_clean = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.cleanup");
    LLVMBasicBlockRef bb_dofr  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.dofree");
    LLVMBasicBlockRef bb_susp  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "coro.suspend");

    LLVMBasicBlockRef bb_pre_alloc = LLVMGetInsertBlock(cg->builder);
    LLVMBuildCondBr(cg->builder, need_alloc, bb_alloc, bb_begin);

    /* coro.alloc: malloc(coro.size) */
    LLVMPositionBuilderAtEnd(cg->builder, bb_alloc);
    LLVMValueRef sz = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_size_i64(cg)),
        sts_intrinsic_coro_size_i64(cg), Null, 0, "coro.size");
    LLVMValueRef m_args[1] = { sz };
    LLVMValueRef frame_mem = LLVMBuildCall2(cg->builder, cg->malloc_type,
        cg->malloc_fn, m_args, 1, "coro.frame_mem");
    LLVMBuildBr(cg->builder, bb_begin);

    /* coro.begin */
    LLVMPositionBuilderAtEnd(cg->builder, bb_begin);
    LLVMValueRef phi_fp = LLVMBuildPhi(cg->builder, ptr_t, "coro.fp");
    LLVMValueRef incoming_v[2] = { LLVMConstNull(ptr_t), frame_mem };
    LLVMBasicBlockRef incoming_b[2] = { bb_pre_alloc, bb_alloc };
    LLVMAddIncoming(phi_fp, incoming_v, incoming_b, 2);
    LLVMValueRef begin_args[2] = { id_token, phi_fp };
    LLVMValueRef hdl = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_begin(cg)),
        sts_intrinsic_coro_begin(cg), begin_args, 2, "coro.hdl");

    /* Resolve the promise pointer through `llvm.coro.promise(hdl)` rather
       than the alloca — both producer and consumer access the promise
       through this canonical intrinsic so CoroEarly lowers them to the
       same frame-relative GEP. */
    LLVMValueRef prom_via_hdl = sts_call_coro_promise(cg, hdl);

    /* zero-init header fields, then mark is_stream=1 */
    for (unsigned f = 0; f < STS_PROM_HDR_NUM_FIELDS; f++) {
        LLVMValueRef gep = sts_gep_hdr_field(cg, prom_ty, prom_via_hdl, f);
        if (f == STS_PROM_OFF_CONTINUATION || f == STS_PROM_OFF_ERROR_MSG)
            LLVMBuildStore(cg->builder, LLVMConstNull(ptr_t), gep);
        else
            LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, 0, 0), gep);
    }
    LLVMBuildStore(cg->builder,
                   LLVMConstInt(i32_t, 1, 0),
                   sts_gep_hdr_field(cg, prom_ty, prom_via_hdl, STS_PROM_OFF_IS_STREAM));

    LLVMBuildBr(cg->builder, bb_body);

    /* coro.final_suspend */
    LLVMPositionBuilderAtEnd(cg->builder, bb_final);
    LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, 1, 0),
                   sts_gep_hdr_field(cg, prom_ty, prom_via_hdl, STS_PROM_OFF_EOS));
    LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, 1, 0),
                   sts_gep_hdr_field(cg, prom_ty, prom_via_hdl, STS_PROM_OFF_COMPLETE));
    LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, 0, 0),
                   sts_gep_hdr_field(cg, prom_ty, prom_via_hdl, STS_PROM_OFF_ITEM_READY));
    LLVMValueRef fs_args[2] = {
        LLVMConstNull(sts_token_ty(cg)),
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0)
    };
    LLVMValueRef fs_v = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_suspend(cg)),
        sts_intrinsic_coro_suspend(cg), fs_args, 2, "coro.fs");
    LLVMValueRef fs_sw = LLVMBuildSwitch(cg->builder, fs_v, bb_susp, 2);
    LLVMAddCase(fs_sw, LLVMConstInt(LLVMInt8TypeInContext(cg->ctx), 0, 0), bb_unr);
    LLVMAddCase(fs_sw, LLVMConstInt(LLVMInt8TypeInContext(cg->ctx), 1, 0), bb_clean);

    /* coro.unreach */
    LLVMPositionBuilderAtEnd(cg->builder, bb_unr);
    LLVMBuildUnreachable(cg->builder);

    /* coro.cleanup */
    LLVMPositionBuilderAtEnd(cg->builder, bb_clean);
    LLVMValueRef free_args[2] = { id_token, hdl };
    LLVMValueRef mem = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_free(cg)),
        sts_intrinsic_coro_free(cg), free_args, 2, "coro.free");
    LLVMValueRef has_mem = LLVMBuildICmp(cg->builder, LLVMIntNE, mem,
        LLVMConstNull(ptr_t), "coro.has_mem");
    LLVMBuildCondBr(cg->builder, has_mem, bb_dofr, bb_susp);

    /* coro.dofree */
    LLVMPositionBuilderAtEnd(cg->builder, bb_dofr);
    LLVMValueRef free_arg[1] = { mem };
    LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, free_arg, 1, "");
    LLVMBuildBr(cg->builder, bb_susp);

    /* coro.suspend */
    LLVMPositionBuilderAtEnd(cg->builder, bb_susp);
    LLVMValueRef end_args[3] = {
        hdl,
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0),
        LLVMConstNull(sts_token_ty(cg))
    };
    LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_end(cg)),
        sts_intrinsic_coro_end(cg), end_args, 3, "");
    LLVMBuildRet(cg->builder, hdl);

    /* leave the builder ready to lower the body */
    LLVMPositionBuilderAtEnd(cg->builder, bb_body);

    out_ctx->handle           = hdl;
    out_ctx->id_token         = id_token;
    out_ctx->promise          = prom_via_hdl;  /* canonical promise ptr */
    out_ctx->promise_type     = prom_ty;
    out_ctx->hdr_type         = hdr_ty;
    out_ctx->item_type        = item_lt;
    out_ctx->final_suspend_bb = bb_final;
    out_ctx->cleanup_bb       = bb_clean;
    out_ctx->suspend_bb       = bb_susp;
    out_ctx->active           = 1;
    out_ctx->susp_counter     = 0;
}

/* Emit a `yield expr;` suspend point.  Stores the produced item, marks
 * item_ready=1, and suspends.  After the suspend, the producer's resume
 * lands at the new "coro.after_yield_<n>" block which becomes the current
 * insertion point. */
static void sts_emit_yield_value(cg_t *cg, sts_coro_ctx_t *cx,
                                 LLVMValueRef value, LLVMTypeRef value_ty) {
    if (!cx || !cx->active) return;
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i8_t  = LLVMInt8TypeInContext(cg->ctx);

    /* store item — coerce when needed */
    LLVMValueRef item_ptr = sts_gep_item(cg, cx->promise_type, cx->promise);
    LLVMValueRef stored   = coerce_int(cg, value, cx->item_type);
    (void)value_ty;
    LLVMBuildStore(cg->builder, stored, item_ptr);
    LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, 1, 0),
        sts_gep_hdr_field(cg, cx->promise_type, cx->promise, STS_PROM_OFF_ITEM_READY));

    /* suspend point */
    char nm[64];
    snprintf(nm, sizeof(nm), "coro.after_yield_%d", cx->susp_counter++);
    LLVMBasicBlockRef bb_after = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, nm);
    LLVMValueRef sp_args[2] = {
        LLVMConstNull(sts_token_ty(cg)),
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0)
    };
    LLVMValueRef sp = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_suspend(cg)),
        sts_intrinsic_coro_suspend(cg), sp_args, 2, "coro.sp");
    LLVMValueRef sw = LLVMBuildSwitch(cg->builder, sp, cx->suspend_bb, 2);
    LLVMAddCase(sw, LLVMConstInt(i8_t, 0, 0), bb_after);
    LLVMAddCase(sw, LLVMConstInt(i8_t, 1, 0), cx->cleanup_bb);

    /* resume edge: clear item_ready so the next yield round-trips cleanly */
    LLVMPositionBuilderAtEnd(cg->builder, bb_after);
    LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, 0, 0),
        sts_gep_hdr_field(cg, cx->promise_type, cx->promise, STS_PROM_OFF_ITEM_READY));
}

/* Emit a bare `yield;` cooperative reschedule (no item produced). */
static void sts_emit_yield_now(cg_t *cg, sts_coro_ctx_t *cx) {
    if (!cx || !cx->active) return;
    LLVMTypeRef i8_t = LLVMInt8TypeInContext(cg->ctx);
    char nm[64];
    snprintf(nm, sizeof(nm), "coro.after_ynow_%d", cx->susp_counter++);
    LLVMBasicBlockRef bb_after = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, nm);
    LLVMValueRef sp_args[2] = {
        LLVMConstNull(sts_token_ty(cg)),
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0)
    };
    LLVMValueRef sp = LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_suspend(cg)),
        sts_intrinsic_coro_suspend(cg), sp_args, 2, "coro.sp");
    LLVMValueRef sw = LLVMBuildSwitch(cg->builder, sp, cx->suspend_bb, 2);
    LLVMAddCase(sw, LLVMConstInt(i8_t, 0, 0), bb_after);
    LLVMAddCase(sw, LLVMConstInt(i8_t, 1, 0), cx->cleanup_bb);
    LLVMPositionBuilderAtEnd(cg->builder, bb_after);
}

/* `ret;` inside a stream coroutine — branch to the final suspend block. */
static void sts_emit_stream_ret(cg_t *cg, sts_coro_ctx_t *cx) {
    if (!cx || !cx->active) return;
    LLVMBuildBr(cg->builder, cx->final_suspend_bb);
}

/* If the body of a coroutine fell through without `ret;`, close it for the
 * user — branch to final suspend so the IR is well-formed. */
static void sts_finish_coro_body_if_open(cg_t *cg, sts_coro_ctx_t *cx) {
    if (!cx || !cx->active) return;
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
    if (!LLVMGetBasicBlockTerminator(cur))
        LLVMBuildBr(cg->builder, cx->final_suspend_bb);
}

/* ── consumer-side primitives ── */

/* Resolve the promise pointer for a stream handle by calling
 * `llvm.coro.promise(handle, alignment, false)` (from_promise = false). */
static LLVMValueRef sts_call_coro_promise(cg_t *cg, LLVMValueRef handle) {
    LLVMValueRef args[3] = {
        handle,
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), STS_PROM_ALIGN, 0),
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0),
    };
    return LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_promise(cg)),
        sts_intrinsic_coro_promise(cg), args, 3, "stream.prom");
}

/* await.next(stream) — drive `coro.resume` until the producer yields a value
 *                      or signals eos.  Returns the yielded T on the "got"
 *                      path; on EOS, returns a zero-initialised T (caller
 *                      checks `stream.done(s)` post-call). */
static LLVMValueRef sts_emit_await_next(cg_t *cg, LLVMValueRef stream_h,
                                        type_info_t item_ti) {
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef item_lt;
    LLVMTypeRef prom_ty = sts_stream_prom_type(cg, item_ti, &item_lt);

    LLVMValueRef prom = sts_call_coro_promise(cg, stream_h);

    /* Drive flow:
         loop_head: if item_ready -> got_item
                    if eos        -> eos
                    call coro.resume; goto loop_head
       The producer side clears item_ready via the post-yield resume edge
       and via final_suspend, so the consumer never has to.  Eager-start
       leaves item_ready=1 / item populated for the first-call fast path. */
    LLVMBasicBlockRef bb_loop  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "anext.loop");
    LLVMBasicBlockRef bb_after_chk = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "anext.after_chk");
    LLVMBasicBlockRef bb_resume = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "anext.resume");
    LLVMBasicBlockRef bb_got   = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "anext.got");
    LLVMBasicBlockRef bb_eos   = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "anext.eos");
    LLVMBasicBlockRef bb_done  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "anext.done");
    LLVMBuildBr(cg->builder, bb_loop);

    /* anext.loop: check item_ready first */
    LLVMPositionBuilderAtEnd(cg->builder, bb_loop);
    LLVMValueRef ir_p = sts_gep_hdr_field(cg, prom_ty, prom, STS_PROM_OFF_ITEM_READY);
    LLVMValueRef ir_v = LLVMBuildLoad2(cg->builder, i32_t, ir_p, "anext.ir");
    LLVMValueRef has_item = LLVMBuildICmp(cg->builder, LLVMIntNE, ir_v,
        LLVMConstInt(i32_t, 0, 0), "anext.has_item");
    LLVMBuildCondBr(cg->builder, has_item, bb_got, bb_after_chk);

    /* anext.after_chk: no item; check eos */
    LLVMPositionBuilderAtEnd(cg->builder, bb_after_chk);
    LLVMValueRef eos_p = sts_gep_hdr_field(cg, prom_ty, prom, STS_PROM_OFF_EOS);
    LLVMValueRef eos_v = LLVMBuildLoad2(cg->builder, i32_t, eos_p, "anext.eos");
    LLVMValueRef is_eos = LLVMBuildICmp(cg->builder, LLVMIntNE, eos_v,
        LLVMConstInt(i32_t, 0, 0), "anext.is_eos");
    LLVMBuildCondBr(cg->builder, is_eos, bb_eos, bb_resume);

    /* anext.resume: drive producer one step */
    LLVMPositionBuilderAtEnd(cg->builder, bb_resume);
    LLVMValueRef rargs[1] = { stream_h };
    LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_resume(cg)),
        sts_intrinsic_coro_resume(cg), rargs, 1, "");
    LLVMBuildBr(cg->builder, bb_loop);

    /* anext.got — consume the item, clear item_ready for the next round */
    LLVMPositionBuilderAtEnd(cg->builder, bb_got);
    LLVMValueRef item_p = sts_gep_item(cg, prom_ty, prom);
    LLVMValueRef item_v = LLVMBuildLoad2(cg->builder, item_lt, item_p, "anext.item");
    LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, 0, 0),
        sts_gep_hdr_field(cg, prom_ty, prom, STS_PROM_OFF_ITEM_READY));
    LLVMBuildBr(cg->builder, bb_done);

    /* anext.eos */
    LLVMPositionBuilderAtEnd(cg->builder, bb_eos);
    LLVMValueRef eos_zero = LLVMConstNull(item_lt);
    LLVMBuildBr(cg->builder, bb_done);

    /* anext.done */
    LLVMPositionBuilderAtEnd(cg->builder, bb_done);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, item_lt, "anext.r");
    LLVMValueRef incoming_v[2] = { item_v, eos_zero };
    LLVMBasicBlockRef incoming_b[2] = { bb_got, bb_eos };
    LLVMAddIncoming(phi, incoming_v, incoming_b, 2);
    return phi;
}

/* stream.done(s) — load the eos flag.  Returns i32 (1 == done). */
static LLVMValueRef sts_emit_stream_done(cg_t *cg, LLVMValueRef stream_h,
                                         type_info_t item_ti) {
    LLVMTypeRef i32_t = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef prom_ty = sts_stream_prom_type(cg, item_ti, Null);
    LLVMValueRef prom = sts_call_coro_promise(cg, stream_h);
    LLVMValueRef eos_p = sts_gep_hdr_field(cg, prom_ty, prom, STS_PROM_OFF_EOS);
    return LLVMBuildLoad2(cg->builder, i32_t, eos_p, "stream.done");
}

/* stream.drop(s) — destroy the stream coroutine, freeing its frame. */
static void sts_emit_stream_drop(cg_t *cg, LLVMValueRef stream_h) {
    LLVMValueRef args[1] = { stream_h };
    LLVMBuildCall2(cg->builder,
        LLVMGlobalGetValueType(sts_intrinsic_coro_destroy(cg)),
        sts_intrinsic_coro_destroy(cg), args, 1, "");
}
