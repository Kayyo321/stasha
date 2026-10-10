/* C call ABI, deliberately separate from cg_layout.c object representation.
 * Qualified profile: x86-64 GNU/Linux LP64, SysV AMD64 psABI 3.2.3.
 * Primary implementation reference: pinned llvm/clang/lib/CodeGen/Targets/X86.cpp
 * X86_64ABIInfo::{classify,classifyArgumentType,classifyReturnType,computeInfo}.
 * INTEGER/SSE classes cover supported C integers, pointers, float/double, arrays,
 * structs and unions. x87, vectors, C++ records and alternate ABIs are excluded.
 */
typedef enum { CAbiDirect, CAbiCoerce, CAbiMemory, CAbiIgnore } cg_abi_kind_t;
typedef struct {
    LLVMTypeRef source;
    type_info_t stype;
    cg_abi_kind_t kind;
    LLVMTypeRef pieces[2];
    unsigned offsets[2], count, first, alignment, integers, sses;
} cg_abi_value_t;
struct cg_abi_fn {
    cg_abi_value_t result, *params;
    unsigned count, llvm_count, free_int, free_sse;
    LLVMTypeRef llvm_type;
    boolean_t variadic, sysv;
};

static boolean_t cg_abi_is_aggregate(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMStructTypeKind || LLVMGetTypeKind(t) == LLVMArrayTypeKind;
}
static boolean_t cg_abi_sysv(cg_t *cg) {
    /* Do not infer an environment from an architecture, pointer width or host.
       Musl, BSD, Darwin, x32 and bare metal require their own qualification. */
    const sts_target_spec *t = &cg->target_spec;
    const char *os = strchr(t->triple, '-');
    os = os ? strchr(os + 1, '-') : Null;
    return !strcmp(t->arch, "x86_64") && t->pointer_bits == 64 &&
        t->format == StsFormatELF && os &&
        (!strcmp(os + 1, "linux-gnu") || !strcmp(os + 1, "linux-gnu-elf"));
}
static boolean_t cg_abi_decl_is_c(node_t *decl) {
    if (decl->is_c_extern) return True;
    if (decl->headers) for (usize_t i = 0; i < decl->headers->count; ++i) {
        fh_entry_t *e = &decl->headers->items[i];
        if (!strcmp(e->key, "abi") && e->vkind == FhIdent && e->str_val && !strcmp(e->str_val, "c"))
            return True;
    }
    return False;
}
static void cg_abi_check_fnptr_conversion(cg_t *cg, type_info_t from, type_info_t to, node_t *n) {
    if (from.base != TypeFnPtr || to.base != TypeFnPtr || !from.fn_ptr_desc || !to.fn_ptr_desc) return;
    fn_ptr_desc_t *a = from.fn_ptr_desc, *b = to.fn_ptr_desc;
    if (!a->is_c_abi && !b->is_c_abi) return;
    boolean_t mismatch = a->param_count != b->param_count || a->is_variadic != b->is_variadic;
    boolean_t lowered = a->is_variadic || b->is_variadic;
    usize_t common = a->param_count < b->param_count ? a->param_count : b->param_count;
    for (usize_t i = 0; i <= common; ++i) {
        type_info_t x = scalar_resolve(cg, i == common ? a->ret_type : a->params[i].type);
        type_info_t y = scalar_resolve(cg, i == common ? b->ret_type : b->params[i].type);
        LLVMTypeRef xt = get_llvm_type(cg, x), yt = get_llvm_type(cg, y);
        mismatch |= xt != yt;
        lowered |= cg_abi_is_aggregate(xt) || cg_abi_is_aggregate(yt)
            || (scalar_integer(x) && scalar_bits(x) < 32) || (scalar_integer(y) && scalar_bits(y) < 32);
        if (scalar_integer(x) && scalar_integer(y) && scalar_bits(x) < 32)
            mismatch |= scalar_unsigned(x) != scalar_unsigned(y);
    }
    const char *message = Null;
    if (lowered && a->is_c_abi != b->is_c_abi)
        message = "function pointer conversion would erase the calling convention; use an inferred pointer to a declared C function";
    else if (mismatch) message = "function pointer conversion does not match the declared C signature";
    if (message) {
        diag_begin_error("C ABI: %s", message);
        if (n) diag_span(DIAG_NODE(n), True, "incompatible function signatures");
        diag_finish();
    }
}
static void *cg_abi_alloc(usize_t count, usize_t size) {
    heap_t h = allocate(count, size); ast_arena_track(h);
    memset(h.pointer, 0, count * size); return h.pointer;
}
static void cg_abi_error(node_t *n, const char *message) {
    diag_begin_error("C ABI: %s", message);
    if (n) diag_span(DIAG_NODE(n), True, "C signature here");
    diag_finish();
}
/* An eightbyte class merge: INTEGER dominates SSE; zero means NO_CLASS.
   f32 byte masks let us choose float versus <2 x float> without guessing a
   union's classes from its arbitrary LLVM alignment-anchor storage member. */
typedef struct { unsigned cls[2], f32[2], f64[2]; boolean_t memory, invalid; } cg_abi_classes_t;
static void cg_abi_walk(cg_t *cg, LLVMTypeRef t, uint64_t off, cg_abi_classes_t *c, unsigned depth) {
    if (c->memory || c->invalid) return;
    if (depth > 64 || !LLVMTypeIsSized(t)) { c->invalid = True; return; }
    uint64_t size = LLVMABISizeOfType(cg->di_data_layout, t);
    if (!size) return;
    if (off + size > 16) { c->memory = True; return; }
    unsigned align = LLVMABIAlignmentOfType(cg->di_data_layout, t);
    if (off % align) { c->memory = True; return; }
    LLVMTypeKind k = LLVMGetTypeKind(t);
    if (k == LLVMArrayTypeKind) {
        LLVMTypeRef e = LLVMGetElementType(t);
        uint64_t stride = LLVMABISizeOfType(cg->di_data_layout, e);
        for (uint64_t i = 0; i < LLVMGetArrayLength2(t); ++i)
            cg_abi_walk(cg, e, off + i * stride, c, depth + 1);
        return;
    }
    if (k == LLVMStructTypeKind) {
        struct_reg_t *sr = Null;
        for (usize_t i = 0; i < cg->struct_count; ++i)
            if (cg->structs[i].llvm_type == t) { sr = &cg->structs[i]; break; }
        if (sr) {
            if (sr->is_interface || sr->is_any_type) { c->invalid = True; return; }
            for (usize_t i = 0; i < sr->field_count; ++i) {
                field_info_t *f = &sr->fields[i];
                uint64_t offset = sr->is_union ? 0 : LLVMOffsetOfElement(cg->di_data_layout, t, (unsigned)f->index);
                /* Bitfields have their own C layout rules, not the language's
                   backing-field layout; do not silently certify them. */
                if (f->bit_width) { c->invalid = True; return; }
                cg_abi_walk(cg, f->llvm_type ? f->llvm_type : get_llvm_type(cg, f->type), off + offset, c, depth + 1);
            }
        } else {
            /* Unregistered aggregates (tagged enum/closure/internal tuples)
               have no established C source representation. */
            c->invalid = True;
        }
        return;
    }
    unsigned cls;
    if (k == LLVMPointerTypeKind || (k == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(t) <= 64)) cls = 2;
    else if (k == LLVMFloatTypeKind || k == LLVMDoubleTypeKind) cls = 1;
    else { c->invalid = True; return; }
    for (unsigned word = (unsigned)(off / 8); word <= (off + size - 1) / 8; ++word)
        if (cls > c->cls[word]) c->cls[word] = cls;
    if (k == LLVMFloatTypeKind) c->f32[off / 8] |= 1u << (unsigned)(off % 8);
    if (k == LLVMDoubleTypeKind) c->f64[off / 8] = 1;
}
static boolean_t cg_abi_validate(cg_t *cg, LLVMTypeRef t, unsigned depth) {
    if (depth > 64 || !LLVMTypeIsSized(t)) return False;
    switch (LLVMGetTypeKind(t)) {
    case LLVMArrayTypeKind: return LLVMGetArrayLength2(t) > 0 && cg_abi_validate(cg, LLVMGetElementType(t), depth + 1);
    case LLVMStructTypeKind:
        for (usize_t i = 0; i < cg->struct_count; ++i) {
            struct_reg_t *sr = &cg->structs[i];
            if (sr->llvm_type != t) continue;
            if (!sr->field_count || sr->is_interface || sr->is_any_type) return False;
            for (usize_t j = 0; j < sr->field_count; ++j) {
                field_info_t *f = &sr->fields[j];
                if (f->bit_width || !cg_abi_validate(cg, f->llvm_type ? f->llvm_type : get_llvm_type(cg, f->type), depth + 1)) return False;
            }
            return True;
        }
        return False;
    case LLVMIntegerTypeKind: return LLVMGetIntTypeWidth(t) <= 64;
    case LLVMPointerTypeKind: case LLVMFloatTypeKind: case LLVMDoubleTypeKind: return True;
    default: return False;
    }
}
static cg_abi_value_t cg_abi_classify(cg_t *cg, LLVMTypeRef t, type_info_t st, node_t *n) {
    cg_abi_value_t a = { .source = t, .stype = st, .kind = CAbiDirect, .count = 1 };
    a.pieces[0] = t;
    if (LLVMGetTypeKind(t) == LLVMVoidTypeKind) { a.kind = CAbiIgnore; a.count = 0; return a; }
    if (!LLVMTypeIsSized(t)) { cg_abi_error(n, "incomplete C signature type"); return a; }
    a.alignment = LLVMABIAlignmentOfType(cg->di_data_layout, t);
    if (!cg_abi_is_aggregate(t)) {
        if (llvm_is_float(t)) a.sses = 1; else a.integers = 1;
        return a;
    }
    if (!cg_abi_sysv(cg)) {
        cg_abi_error(n, "aggregate arguments/returns are qualified only for x86_64 GNU/Linux LP64");
        return a;
    }
    if (!cg_abi_validate(cg, t, 0)) { cg_abi_error(n, "unsupported aggregate member representation"); return a; }
    uint64_t size = LLVMABISizeOfType(cg->di_data_layout, t);
    cg_abi_classes_t c = {0};
    if (size > 16) c.memory = True;
    else cg_abi_walk(cg, t, 0, &c, 0);
    if (c.invalid) { cg_abi_error(n, "unsupported aggregate member representation"); return a; }
    if (c.memory) { a.kind = CAbiMemory; a.count = 1; return a; }
    a.kind = CAbiCoerce; a.count = 0;
    for (unsigned i = 0; i < 2; ++i) if (c.cls[i]) {
        unsigned j = a.count++;
        a.offsets[j] = i * 8;
        if (c.cls[i] == 2) {
            unsigned bytes = size - i * 8 < 8 ? (unsigned)(size - i * 8) : 8;
            a.pieces[j] = LLVMIntTypeInContext(cg->ctx, bytes * 8); ++a.integers;
        } else {
            if (!c.f64[i] && c.f32[i] == 1) a.pieces[j] = LLVMFloatTypeInContext(cg->ctx);
            else if (!c.f64[i] && c.f32[i] == 17) a.pieces[j] = LLVMVectorType(LLVMFloatTypeInContext(cg->ctx), 2);
            else a.pieces[j] = LLVMDoubleTypeInContext(cg->ctx);
            ++a.sses;
        }
    }
    if (!a.count) a.kind = CAbiIgnore;
    return a;
}
static cg_abi_fn_t *cg_abi_plan(cg_t *cg, LLVMTypeRef ret, LLVMTypeRef *params,
                              fn_ptr_desc_t *desc, node_t *n) {
    cg_abi_fn_t *f = cg_abi_alloc(1, sizeof(*f));
    f->count = (unsigned)desc->param_count;
    f->params = cg_abi_alloc(f->count ? f->count : 1, sizeof(*f->params));
    f->variadic = desc->is_variadic; f->sysv = cg_abi_sysv(cg);
    f->result = cg_abi_classify(cg, ret, desc->ret_type, n);
    unsigned capacity = f->count * 2 + 1;
    LLVMTypeRef *pts = cg_abi_alloc(capacity, sizeof(*pts));
    f->free_int = 6; f->free_sse = 8;
    LLVMTypeRef rt = ret;
    if (f->result.kind == CAbiMemory) {
        pts[f->llvm_count++] = LLVMPointerTypeInContext(cg->ctx, 0);
        --f->free_int; rt = LLVMVoidTypeInContext(cg->ctx);
    } else if (f->result.kind == CAbiIgnore) rt = LLVMVoidTypeInContext(cg->ctx);
    else if (f->result.kind == CAbiCoerce)
        rt = f->result.count == 1 ? f->result.pieces[0] : LLVMStructTypeInContext(cg->ctx, f->result.pieces, 2, 0);
    for (unsigned i = 0; i < f->count; ++i) {
        cg_abi_value_t *a = &f->params[i];
        *a = cg_abi_classify(cg, params[i], desc->params[i].type, n);
        a->first = f->llvm_count;
        if (a->kind == CAbiCoerce && (a->integers > f->free_int || a->sses > f->free_sse)) {
            /* Roll back ALL register assignments for this aggregate. */
            a->kind = CAbiMemory; a->count = 1;
        }
        if (a->kind == CAbiMemory) {
            if (a->alignment < 8) a->alignment = 8;
            pts[f->llvm_count++] = LLVMPointerTypeInContext(cg->ctx, 0);
        } else {
            if (a->integers <= f->free_int) f->free_int -= a->integers; else f->free_int = 0;
            if (a->sses <= f->free_sse) f->free_sse -= a->sses; else f->free_sse = 0;
            for (unsigned j = 0; j < a->count; ++j) pts[f->llvm_count++] = a->pieces[j];
        }
    }
    f->llvm_type = LLVMFunctionType(rt, pts, f->llvm_count, f->variadic);
    return f;
}
static cg_abi_fn_t *cg_abi_plan_desc(cg_t *cg, fn_ptr_desc_t *desc) {
    LLVMTypeRef *types = cg_abi_alloc(desc->param_count ? desc->param_count : 1, sizeof(*types));
    for (usize_t i = 0; i < desc->param_count; ++i) types[i] = get_llvm_type(cg, desc->params[i].type);
    return cg_abi_plan(cg, get_llvm_type(cg, desc->ret_type), types, desc, Null);
}
static LLVMTypeRef cg_abi_fn_ptr_type(cg_t *cg, fn_ptr_desc_t *desc) {
    return cg_abi_plan_desc(cg, desc)->llvm_type;
}
static void cg_abi_attr(cg_t *cg, LLVMValueRef v, boolean_t call, unsigned index,
                        const char *name, uint64_t value, LLVMTypeRef type) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    LLVMAttributeRef attr = type ? LLVMCreateTypeAttribute(cg->ctx, kind, type)
                                : LLVMCreateEnumAttribute(cg->ctx, kind, value);
    if (call) LLVMAddCallSiteAttribute(v, index, attr); else LLVMAddAttributeAtIndex(v, index, attr);
}
static void cg_abi_extend_attr(cg_t *cg, LLVMValueRef v, boolean_t call, unsigned index, cg_abi_value_t *a) {
    type_info_t t = scalar_resolve(cg, a->stype);
    if (a->kind == CAbiDirect && scalar_integer(t) && scalar_bits(t) < 32)
        cg_abi_attr(cg, v, call, index, scalar_unsigned(t) ? "zeroext" : "signext", 0, Null);
}
static void cg_abi_attributes(cg_t *cg, LLVMValueRef v, boolean_t call, cg_abi_fn_t *f) {
    if (!f->sysv) return;
    if (f->result.kind == CAbiMemory) {
        cg_abi_attr(cg, v, call, 1, "sret", 0, f->result.source);
        cg_abi_attr(cg, v, call, 1, "align", f->result.alignment, Null);
        cg_abi_attr(cg, v, call, 1, "noalias", 0, Null);
    } else cg_abi_extend_attr(cg, v, call, LLVMAttributeReturnIndex, &f->result);
    for (unsigned i = 0; i < f->count; ++i) {
        cg_abi_value_t *a = &f->params[i];
        if (a->kind == CAbiMemory) {
            cg_abi_attr(cg, v, call, a->first + 1, "byval", 0, a->source);
            cg_abi_attr(cg, v, call, a->first + 1, "align", a->alignment, Null);
        } else cg_abi_extend_attr(cg, v, call, a->first + 1, a);
    }
}
static LLVMValueRef cg_abi_byte_ptr(cg_t *cg, LLVMValueRef p, unsigned offset) {
    LLVMValueRef n = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), offset, 0);
    return LLVMBuildGEP2(cg->builder, LLVMInt8TypeInContext(cg->ctx), p, &n, 1, "abi.byte");
}
static LLVMValueRef cg_abi_piece_load(cg_t *cg, LLVMValueRef p, cg_abi_value_t *a, unsigned j) {
    LLVMValueRef v = LLVMBuildLoad2(cg->builder, a->pieces[j], cg_abi_byte_ptr(cg, p, a->offsets[j]), "abi.pack");
    LLVMSetAlignment(v, 1); return v;
}
static void cg_abi_piece_store(cg_t *cg, LLVMValueRef p, cg_abi_value_t *a, unsigned j, LLVMValueRef v) {
    LLVMValueRef s = LLVMBuildStore(cg->builder, v, cg_abi_byte_ptr(cg, p, a->offsets[j]));
    LLVMSetAlignment(s, 1);
}
static LLVMValueRef cg_abi_temp(cg_t *cg, cg_abi_value_t *a, LLVMValueRef value) {
    LLVMValueRef p = alloc_in_entry(cg, a->source, "abi.value");
    LLVMSetAlignment(p, a->alignment);
    LLVMBuildStore(cg->builder, value ? value : LLVMConstNull(a->source), p);
    return p;
}
static LLVMValueRef cg_abi_call(cg_t *cg, cg_abi_fn_t *f, LLVMValueRef callee,
                              node_list_t nodes, node_t *site) {
    if (nodes.count < f->count || (!f->variadic && nodes.count != f->count)) {
        cg_abi_error(site, "argument count does not match C signature");
        return LLVMGetTypeKind(f->result.source) == LLVMVoidTypeKind ? Null : LLVMConstNull(f->result.source);
    }
    LLVMValueRef *args = cg_abi_alloc(f->llvm_count + nodes.count + 1, sizeof(*args));
    LLVMValueRef output = Null;
    if (f->result.kind == CAbiMemory) args[0] = output = cg_abi_temp(cg, &f->result, Null);
    unsigned argc = f->llvm_count;
    for (usize_t i = 0; i < nodes.count; ++i) {
        if (i >= f->count) {
            type_info_t t = scalar_expr_type(cg, nodes.items[i], NO_TYPE);
            if (scalar_integer(t) && scalar_bits(t) < 32) t = scalar_type(TypeI32);
            else if (t.base == TypeF32 && !t.is_pointer) t = scalar_type(TypeF64);
            LLVMValueRef v = gen_scalar_as(cg, nodes.items[i], t);
            if (cg_abi_is_aggregate(LLVMTypeOf(v))) {
                cg_abi_error(nodes.items[i], "aggregate variadic arguments are not yet supported");
                return LLVMGetTypeKind(f->result.source) == LLVMVoidTypeKind ? Null : LLVMConstNull(f->result.source);
            }
            args[argc++] = v; continue;
        }
        cg_abi_value_t *a = &f->params[i];
        LLVMValueRef v = gen_scalar_as(cg, nodes.items[i], a->stype);
        if (LLVMTypeOf(v) != a->source) v = coerce_int(cg, v, a->source);
        if (LLVMTypeOf(v) != a->source) {
            cg_abi_error(nodes.items[i], "argument representation does not match C signature");
            v = LLVMConstNull(a->source);
        }
        if (a->kind == CAbiDirect) args[a->first] = v;
        else if (a->kind != CAbiIgnore) {
            LLVMValueRef p = cg_abi_temp(cg, a, v);
            if (a->kind == CAbiMemory) args[a->first] = p;
            else for (unsigned j = 0; j < a->count; ++j) args[a->first + j] = cg_abi_piece_load(cg, p, a, j);
        }
    }
    LLVMValueRef v = LLVMBuildCall2(cg->builder, f->llvm_type, callee, args, argc, "");
    cg_abi_attributes(cg, v, True, f);
    cg_abi_value_t *r = &f->result;
    if (r->kind == CAbiMemory) return LLVMBuildLoad2(cg->builder, r->source, output, "abi.result");
    if (r->kind == CAbiIgnore) return cg_abi_is_aggregate(r->source) ? LLVMConstNull(r->source) : v;
    if (r->kind == CAbiDirect) return v;
    output = cg_abi_temp(cg, r, Null);
    for (unsigned j = 0; j < r->count; ++j)
        cg_abi_piece_store(cg, output, r, j, r->count == 1 ? v : LLVMBuildExtractValue(cg->builder, v, j, "abi.result.part"));
    return LLVMBuildLoad2(cg->builder, r->source, output, "abi.result");
}
static void cg_abi_bind_param(cg_t *cg, cg_abi_fn_t *f, unsigned i, LLVMValueRef local) {
    cg_abi_value_t *a = &f->params[i];
    if (a->kind == CAbiDirect) LLVMBuildStore(cg->builder, LLVMGetParam(cg->current_fn, a->first), local);
    else if (a->kind == CAbiMemory) {
        LLVMValueRef v = LLVMBuildLoad2(cg->builder, a->source, LLVMGetParam(cg->current_fn, a->first), "abi.param");
        LLVMSetAlignment(v, a->alignment);
        LLVMBuildStore(cg->builder, v, local);
    } else {
        LLVMBuildStore(cg->builder, LLVMConstNull(a->source), local);
        for (unsigned j = 0; j < a->count; ++j)
            cg_abi_piece_store(cg, local, a, j, LLVMGetParam(cg->current_fn, a->first + j));
    }
}
static cg_abi_fn_t *cg_abi_current(cg_t *cg) {
    for (usize_t i = 0; i < cg->globals.count; ++i)
        if (cg->globals.entries[i].value == cg->current_fn) return cg->globals.entries[i].c_abi;
    return Null;
}
static void cg_abi_return(cg_t *cg, cg_abi_fn_t *f, LLVMValueRef value) {
    cg_abi_value_t *r = &f->result;
    if (r->kind == CAbiMemory) {
        LLVMBuildStore(cg->builder, value, LLVMGetParam(cg->current_fn, 0));
        LLVMBuildRetVoid(cg->builder);
    } else if (r->kind == CAbiIgnore) LLVMBuildRetVoid(cg->builder);
    else if (r->kind == CAbiDirect) LLVMBuildRet(cg->builder, value);
    else {
        LLVMValueRef p = cg_abi_temp(cg, r, value);
        LLVMValueRef v = r->count == 1 ? cg_abi_piece_load(cg, p, r, 0) : LLVMGetUndef(LLVMGetReturnType(f->llvm_type));
        if (r->count > 1) for (unsigned j = 0; j < r->count; ++j)
            v = LLVMBuildInsertValue(cg->builder, v, cg_abi_piece_load(cg, p, r, j), j, "abi.return");
        LLVMBuildRet(cg->builder, v);
    }
}
