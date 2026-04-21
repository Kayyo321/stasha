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
