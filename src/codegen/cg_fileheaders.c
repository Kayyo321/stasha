/* ── fileheader (@[[...]]) codegen support ── */

/* Parsed target descriptor.  Populated once from cg.target_triple. */
typedef struct {
    char arch[32];          /* "x86_64", "aarch64", "i386", "arm", "riscv64"… */
    char os[32];            /* "linux", "macos", "windows", "freebsd"…      */
    int  pointer_width;     /* 64 or 32 */
} cg_target_desc_t;

static void cg_parse_target_desc(const char *triple, cg_target_desc_t *out) {
    memset(out, 0, sizeof(*out));
    if (!triple || !triple[0]) triple = "unknown-unknown-unknown";

    /* arch = first '-' segment */
    const char *p = triple;
    const char *dash = strchr(p, '-');
    usize_t alen = dash ? (usize_t)(dash - p) : strlen(p);
    if (alen >= sizeof(out->arch)) alen = sizeof(out->arch) - 1;
    memcpy(out->arch, p, alen);
    out->arch[alen] = '\0';

    /* Map vendor-specific arch spellings */
    if (strcmp(out->arch, "arm64") == 0) strcpy(out->arch, "aarch64");
    if (strcmp(out->arch, "amd64") == 0) strcpy(out->arch, "x86_64");

    /* os = third segment.  Skip vendor. */
    const char *os_start = Null;
    if (dash) {
        const char *vendor_end = strchr(dash + 1, '-');
        if (vendor_end) os_start = vendor_end + 1;
    }
    if (os_start) {
        const char *os_end = strchr(os_start, '-');
        if (!os_end) os_end = os_start + strlen(os_start);
        usize_t olen = (usize_t)(os_end - os_start);
        if (olen >= sizeof(out->os)) olen = sizeof(out->os) - 1;
        memcpy(out->os, os_start, olen);
        out->os[olen] = '\0';
    }
    /* Canonical forms: darwin → macos (for our condition tests) */
    if (strcmp(out->os, "darwin") == 0 || strstr(out->os, "darwin") == out->os)
        strcpy(out->os, "macos");

    /* pointer_width: crude arch-based inference */
    if (strcmp(out->arch, "x86_64") == 0
        || strcmp(out->arch, "aarch64") == 0
        || strcmp(out->arch, "riscv64") == 0
        || strcmp(out->arch, "powerpc64") == 0
        || strcmp(out->arch, "powerpc64le") == 0
        || strcmp(out->arch, "mips64") == 0
        || strcmp(out->arch, "mips64el") == 0
        || strcmp(out->arch, "loongarch64") == 0)
        out->pointer_width = 64;
    else if (strcmp(out->arch, "i386") == 0
        || strcmp(out->arch, "i686") == 0
        || strcmp(out->arch, "x86") == 0
        || strcmp(out->arch, "arm") == 0
        || strcmp(out->arch, "armv7") == 0
        || strcmp(out->arch, "riscv32") == 0
        || strcmp(out->arch, "mips") == 0
        || strcmp(out->arch, "mipsel") == 0
        || strcmp(out->arch, "wasm32") == 0)
        out->pointer_width = 32;
    else
        out->pointer_width = 64; /* default */
}

static boolean_t cg_arch_matches(const cg_target_desc_t *t, const char *s) {
    if (strcmp(t->arch, s) == 0) return True;
    /* aliases */
    if (strcmp(s, "x64") == 0 && strcmp(t->arch, "x86_64") == 0) return True;
    if (strcmp(s, "arm64") == 0 && strcmp(t->arch, "aarch64") == 0) return True;
    return False;
}

static boolean_t cg_os_matches(const cg_target_desc_t *t, const char *s) {
    if (strcmp(t->os, s) == 0) return True;
    if (strcmp(s, "darwin") == 0 && strcmp(t->os, "macos") == 0) return True;
    return False;
}

static boolean_t cg_eval_fh_cond(const cg_target_desc_t *t, const fh_cond_t *c) {
    switch (c->op) {
        case FhCondOsEq:       return c->str_rhs && cg_os_matches(t, c->str_rhs);
        case FhCondArchEq:     return c->str_rhs && cg_arch_matches(t, c->str_rhs);
        case FhCondPtrWidthEq: return t->pointer_width == c->int_rhs;
        case FhCondAlwaysFalse: return False;
        case FhCondNone:       return True;
    }
    return False;
}

/* Returns True if decl should be skipped this compile (for @[[if: ...]]).
   Emits errors for failing @[[require: ...]] entries. */
static boolean_t cg_fh_skip_decl(cg_t *cg, fileheader_t *fh, usize_t line) {
    if (!fh) return False;
    cg_target_desc_t t; cg_parse_target_desc(cg->target_triple, &t);
    for (usize_t i = 0; i < fh->count; i++) {
        fh_entry_t *e = &fh->items[i];
        if (e->vkind != FhCond) continue;
        boolean_t ok = cg_eval_fh_cond(&t, &e->cond);
        if (ok) continue;
        if (strcmp(e->key, "require") == 0) {
            diag_begin_error("@[[require: ...]] condition not satisfied for target '%s'",
                             cg->target_triple ? cg->target_triple : "(default)");
            diag_span(SRC_LOC(line, 1, 0), True, "declaration disabled by require");
            diag_finish();
            return True;
        }
        if (strcmp(e->key, "if") == 0) return True;
    }
    return False;
}

/* Look up the final symbol name based on abi/export_name fileheader entries.
   Copies into `buf` and returns True on override, or False to keep original. */
static boolean_t cg_fh_override_symbol(fileheader_t *fh, const char *orig,
                                        char *buf, usize_t buf_size,
                                        boolean_t *out_abi_c) {
    if (out_abi_c) *out_abi_c = False;
    if (!fh) return False;

    const char *export_name = Null;
    boolean_t abi_c = False;
    for (usize_t i = 0; i < fh->count; i++) {
        fh_entry_t *e = &fh->items[i];
        if (strcmp(e->key, "export_name") == 0 && e->vkind == FhStr)
            export_name = e->str_val;
        else if (strcmp(e->key, "abi") == 0 && e->vkind == FhIdent
                 && e->str_val && strcmp(e->str_val, "c") == 0)
            abi_c = True;
    }
    if (out_abi_c) *out_abi_c = abi_c;

    if (export_name) {
        usize_t n = strlen(export_name);
        if (n >= buf_size) n = buf_size - 1;
        memcpy(buf, export_name, n);
        buf[n] = '\0';
        return True;
    }
    if (abi_c) {
        usize_t n = strlen(orig);
        if (n >= buf_size) n = buf_size - 1;
        memcpy(buf, orig, n);
        buf[n] = '\0';
        return True;
    }
    return False;
}

/* Apply fn-level fileheader entries after LLVMAddFunction. */
static void cg_fh_apply_to_fn(cg_t *cg, LLVMValueRef fn, fileheader_t *fh) {
    if (!fh) return;
    for (usize_t i = 0; i < fh->count; i++) {
        fh_entry_t *e = &fh->items[i];
        if (strcmp(e->key, "weak") == 0) {
            LLVMSetLinkage(fn, LLVMWeakAnyLinkage);
        } else if (strcmp(e->key, "hidden") == 0) {
            LLVMSetVisibility(fn, LLVMHiddenVisibility);
        } else if (strcmp(e->key, "section") == 0 && e->vkind == FhStr) {
            LLVMSetSection(fn, e->str_val);
        } else if (strcmp(e->key, "align") == 0 && e->vkind == FhInt) {
            LLVMSetAlignment(fn, (unsigned)e->int_val);
        } else if (strcmp(e->key, "target") == 0 && e->vkind == FhStr) {
            LLVMAddTargetDependentFunctionAttr(fn, "target-cpu", e->str_val);
        } else if (strcmp(e->key, "features") == 0 && e->vkind == FhStr) {
            LLVMAddTargetDependentFunctionAttr(fn, "target-features", e->str_val);
        }
    }
    (void)cg;
}

/* Apply global-variable fileheader entries after LLVMAddGlobal. */
static void cg_fh_apply_to_global(cg_t *cg, LLVMValueRef gv, fileheader_t *fh) {
    if (!fh) return;
    for (usize_t i = 0; i < fh->count; i++) {
        fh_entry_t *e = &fh->items[i];
        if (strcmp(e->key, "weak") == 0) {
            LLVMSetLinkage(gv, LLVMWeakAnyLinkage);
        } else if (strcmp(e->key, "hidden") == 0) {
            LLVMSetVisibility(gv, LLVMHiddenVisibility);
        } else if (strcmp(e->key, "section") == 0 && e->vkind == FhStr) {
            LLVMSetSection(gv, e->str_val);
        } else if (strcmp(e->key, "align") == 0 && e->vkind == FhInt) {
            LLVMSetAlignment(gv, (unsigned)e->int_val);
        }
    }
    (void)cg;
}

/* ── @[[init]] / @[[exit]] lifecycle block emission ──
 *
 * Each collected block is lowered to a private `void()` function, then the set
 * is topologically ordered using the block titles and any `before("name")` /
 * `after("name")` constraints.  The ordered list is emitted as an
 * `@llvm.global_ctors` / `@llvm.global_dtors` array using the standard
 * `{i32 priority, void()* fn, i8* data}` struct layout with appending linkage.
 * Blocks without ordering constraints retain their source order. */

static const char *cg_lifecycle_title_of(node_t *n) {
    return n->as.lifecycle_block.title ? n->as.lifecycle_block.title : "";
}

/* Find an index for the block whose title matches `name`, or -1. */
static int cg_lifecycle_find_by_title(node_t **blocks, usize_t count, const char *name) {
    if (!name) return -1;
    for (usize_t i = 0; i < count; i++) {
        const char *t = blocks[i]->as.lifecycle_block.title;
        if (t && strcmp(t, name) == 0) return (int)i;
    }
    return -1;
}

/* Kahn topological sort.  Edges are `must-come-before` relations.
   `order_out` receives an index permutation of length `count`. */
static void cg_lifecycle_topo_sort(node_t **blocks, usize_t count, usize_t *order_out) {
    /* adjacency matrix in packed bits; count is small (<=128) */
    heap_t edge_heap = allocate(count * count, sizeof(unsigned char));
    unsigned char *edges = edge_heap.pointer;
    heap_t indeg_heap = allocate(count, sizeof(int));
    int *indeg = indeg_heap.pointer;

    for (usize_t i = 0; i < count; i++) {
        node_t *b = blocks[i];
        /* before("X") → edge i -> idx(X) */
        if (b->as.lifecycle_block.before_name) {
            int j = cg_lifecycle_find_by_title(blocks, count,
                                               b->as.lifecycle_block.before_name);
            if (j >= 0 && (usize_t)j != i && !edges[i * count + (usize_t)j]) {
                edges[i * count + (usize_t)j] = 1;
                indeg[j]++;
            }
        }
        /* after("Y") → edge idx(Y) -> i */
        if (b->as.lifecycle_block.after_name) {
            int j = cg_lifecycle_find_by_title(blocks, count,
                                               b->as.lifecycle_block.after_name);
            if (j >= 0 && (usize_t)j != i && !edges[(usize_t)j * count + i]) {
                edges[(usize_t)j * count + i] = 1;
                indeg[i]++;
            }
        }
    }

    /* Kahn: pick nodes with indeg == 0 in their original source order */
    usize_t out_n = 0;
    heap_t done_heap = allocate(count, sizeof(unsigned char));
    unsigned char *done = done_heap.pointer;
    while (out_n < count) {
        int picked = -1;
        for (usize_t i = 0; i < count; i++) {
            if (!done[i] && indeg[i] == 0) { picked = (int)i; break; }
        }
        if (picked < 0) {
            /* cycle — fall back to source order for the remaining */
            for (usize_t i = 0; i < count; i++)
                if (!done[i]) { order_out[out_n++] = i; done[i] = 1; }
            break;
        }
        order_out[out_n++] = (usize_t)picked;
        done[picked] = 1;
        for (usize_t k = 0; k < count; k++) {
            if (edges[(usize_t)picked * count + k]) {
                edges[(usize_t)picked * count + k] = 0;
                indeg[k]--;
            }
        }
    }

    deallocate(edge_heap);
    deallocate(indeg_heap);
    deallocate(done_heap);
}

/* Emit one lifecycle block as a private `void()` function and return it. */
static LLVMValueRef cg_emit_lifecycle_body(cg_t *cg, node_t *blk,
                                            const char *kind, usize_t idx) {
    char fn_name[128];
    const char *title = cg_lifecycle_title_of(blk);
    if (title[0])
        snprintf(fn_name, sizeof(fn_name), "__fh_%s_%s_%lu", kind, title, (unsigned long)idx);
    else
        snprintf(fn_name, sizeof(fn_name), "__fh_%s_%lu", kind, (unsigned long)idx);

    LLVMTypeRef fn_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), Null, 0, 0);
    LLVMValueRef fn = LLVMAddFunction(cg->module, fn_name, fn_type);
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    LLVMValueRef prev_fn = cg->current_fn;
    usize_t prev_locals = cg->locals.count;
    int prev_dtor = cg->dtor_depth;

    cg->current_fn = fn;
    cg->locals.count = 0;
    cg->dtor_depth = 0;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);
    if (blk->as.lifecycle_block.body)
        gen_block(cg, blk->as.lifecycle_block.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildRetVoid(cg->builder);

    cg->current_fn = prev_fn;
    cg->locals.count = prev_locals;
    cg->dtor_depth = prev_dtor;
    return fn;
}

/* Emit a single dispatcher function that calls `fns[0..n]` in order, and
   install it as the sole entry in `@llvm.global_ctors` (or _dtors).  Using
   one dispatcher sidesteps LLVM's cross-platform divergence in how
   same-priority or array-ordered ctor tables are run (Mach-O's
   __mod_init_func walks vs. ELF's ascending-priority sort): the call order
   is *inside* the dispatcher, which both platforms execute forwards. */
static void cg_emit_lifecycle_array(cg_t *cg, LLVMValueRef *fns, usize_t n,
                                     const char *global_name) {
    if (n == 0) return;

    LLVMTypeRef i32ty = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef ptrty = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef voidty = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef fnty = LLVMFunctionType(voidty, Null, 0, 0);

    /* build dispatcher */
    boolean_t is_ctor = (strcmp(global_name, "llvm.global_ctors") == 0);
    const char *disp_name = is_ctor ? "__fh_ctor_dispatch" : "__fh_dtor_dispatch";
    LLVMValueRef disp = LLVMAddFunction(cg->module, disp_name, fnty);
    LLVMSetLinkage(disp, LLVMInternalLinkage);

    LLVMValueRef prev_fn = cg->current_fn;
    cg->current_fn = disp;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, disp, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);
    for (usize_t i = 0; i < n; i++)
        LLVMBuildCall2(cg->builder, fnty, fns[i], Null, 0, "");
    LLVMBuildRetVoid(cg->builder);
    cg->current_fn = prev_fn;

    /* single-entry global_ctors/global_dtors array */
    LLVMTypeRef field_types[3] = { i32ty, ptrty, ptrty };
    LLVMTypeRef entry_ty = LLVMStructTypeInContext(cg->ctx, field_types, 3, 0);
    LLVMValueRef null_data = LLVMConstNull(ptrty);
    LLVMValueRef fields[3];
    fields[0] = LLVMConstInt(i32ty, 65535, 0);
    fields[1] = disp;
    fields[2] = null_data;
    LLVMValueRef one_entry = LLVMConstNamedStruct(entry_ty, fields, 3);

    LLVMTypeRef arr_ty = LLVMArrayType2(entry_ty, 1);
    LLVMValueRef arr = LLVMConstArray2(entry_ty, &one_entry, 1);
    LLVMValueRef gv = LLVMAddGlobal(cg->module, arr_ty, global_name);
    LLVMSetLinkage(gv, LLVMAppendingLinkage);
    LLVMSetInitializer(gv, arr);
}

/* Entry point: emit all collected init and exit blocks. */
static void cg_emit_lifecycle_blocks(cg_t *cg,
                                      node_t **init_blocks, usize_t init_n,
                                      node_t **exit_blocks, usize_t exit_n) {
    if (init_n > 0) {
        heap_t ord_heap = allocate(init_n, sizeof(usize_t));
        usize_t *order = ord_heap.pointer;
        cg_lifecycle_topo_sort(init_blocks, init_n, order);

        heap_t fns_heap = allocate(init_n, sizeof(LLVMValueRef));
        LLVMValueRef *fns = fns_heap.pointer;
        for (usize_t i = 0; i < init_n; i++)
            fns[i] = cg_emit_lifecycle_body(cg, init_blocks[order[i]], "init", i);

        cg_emit_lifecycle_array(cg, fns, init_n, "llvm.global_ctors");
        deallocate(fns_heap);
        deallocate(ord_heap);
    }

    if (exit_n > 0) {
        heap_t ord_heap = allocate(exit_n, sizeof(usize_t));
        usize_t *order = ord_heap.pointer;
        cg_lifecycle_topo_sort(exit_blocks, exit_n, order);

        heap_t fns_heap = allocate(exit_n, sizeof(LLVMValueRef));
        LLVMValueRef *fns = fns_heap.pointer;
        for (usize_t i = 0; i < exit_n; i++)
            fns[i] = cg_emit_lifecycle_body(cg, exit_blocks[order[i]], "exit", i);

        cg_emit_lifecycle_array(cg, fns, exit_n, "llvm.global_dtors");
        deallocate(fns_heap);
        deallocate(ord_heap);
    }
}
