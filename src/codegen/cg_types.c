/* ── LLVM type helpers ── */

static boolean_t is_float_type(type_info_t ti) {
    return ti.base == TypeF32 || ti.base == TypeF64;
}

static boolean_t is_unsigned_type(type_info_t ti) {
    return ti.base == TypeU8 || ti.base == TypeU16
        || ti.base == TypeU32 || ti.base == TypeU64;
}

static boolean_t is_integer_type(type_info_t ti) {
    return ti.base >= TypeI8 && ti.base <= TypeU64;
}

/* ── generic name substitution helper ──
 * Apply active generic substitutions to a (possibly mangled) type name.
 * Handles both simple params ("T" → "i32") and embedded params ("arr_t_G_T" → "arr_t_G_i32").
 * Returns the input pointer if no substitution needed, or a new ast_strdup'd string.
 */
static const char *cg_subst_name(cg_t *cg, const char *name) {
    if (!name || cg->generic_n == 0) return name;
    /* direct match: name is a bare type param */
    for (usize_t i = 0; i < cg->generic_n; i++) {
        if (strcmp(name, cg->generic_params[i]) == 0) {
            /* return concrete type name */
            type_info_t c = cg->generic_concs[i];
            if (c.base == TypeUser && c.user_name) return c.user_name;
            /* use the comptime_type_name encoding */
            switch (c.base) {
                case TypeI8:  return "i8";   case TypeU8:  return "u8";
                case TypeI16: return "i16";  case TypeU16: return "u16";
                case TypeI32: return "i32";  case TypeU32: return "u32";
                case TypeI64: return "i64";  case TypeU64: return "u64";
                case TypeF32: return "f32";  case TypeF64: return "f64";
                case TypeBool: return "bool"; case TypeVoid: return "void";
                default: return "type";
            }
        }
    }
    /* check if name contains _G_<param> patterns — substitute each */
    boolean_t changed = False;
    char result[512];
    usize_t rlen = 0;
    const char *p = name;
    usize_t nlen = strlen(name);
    while (rlen < sizeof(result) - 1 && (usize_t)(p - name) < nlen) {
        /* look for _G_ separator */
        const char *g = strstr(p, "_G_");
        if (!g) {
            /* copy remainder */
            usize_t rem = nlen - (usize_t)(p - name);
            if (rlen + rem >= sizeof(result)) rem = sizeof(result) - 1 - rlen;
            memcpy(result + rlen, p, rem); rlen += rem; p += rem;
            break;
        }
        /* copy up to and including "_G_" */
        usize_t prefix_len = (usize_t)(g - p) + 3;
        if (rlen + prefix_len >= sizeof(result)) break;
        memcpy(result + rlen, p, prefix_len); rlen += prefix_len;
        p = g + 3;
        /* p now points at the component after _G_ — check if it's a param */
        boolean_t subst = False;
        for (usize_t i = 0; i < cg->generic_n; i++) {
            usize_t plen = strlen(cg->generic_params[i]);
            /* component ends at next _G_ or end of string */
            const char *next_g = strstr(p, "_G_");
            usize_t comp_len = next_g ? (usize_t)(next_g - p) : strlen(p);
            if (comp_len == plen && memcmp(p, cg->generic_params[i], plen) == 0) {
                /* substitute with concrete type name */
                const char *rep = cg_subst_name(cg, cg->generic_params[i]);
                usize_t rlen2 = strlen(rep);
                if (rlen + rlen2 < sizeof(result)) {
                    memcpy(result + rlen, rep, rlen2); rlen += rlen2;
                }
                p += comp_len;
                changed = True;
                subst = True;
                break;
            }
        }
        if (!subst) {
            /* not a param — copy the component as-is up to next _G_ or end */
            const char *next_g = strstr(p, "_G_");
            usize_t comp_len = next_g ? (usize_t)(next_g - p) : strlen(p);
            if (rlen + comp_len >= sizeof(result)) break;
            memcpy(result + rlen, p, comp_len); rlen += comp_len;
            p += comp_len;
        }
    }
    if (!changed) return name;
    result[rlen] = '\0';
    return ast_strdup(result, rlen);
}

/* forward declaration — defined in cg_generics.c */
static void try_instantiate_generic(cg_t *cg, const char *mangled_name);

static LLVMTypeRef get_llvm_base_type(cg_t *cg, type_info_t ti) {
    ti = resolve_alias(cg, ti);
    if (ti.is_pointer)
        return LLVMPointerTypeInContext(cg->ctx, 0);
    switch (ti.base) {
        case TypeVoid: return LLVMVoidTypeInContext(cg->ctx);
        case TypeBool: return LLVMInt1TypeInContext(cg->ctx);
        case TypeI8:  case TypeU8:  return LLVMInt8TypeInContext(cg->ctx);
        case TypeI16: case TypeU16: return LLVMInt16TypeInContext(cg->ctx);
        case TypeI32: case TypeU32: return LLVMInt32TypeInContext(cg->ctx);
        case TypeI64: case TypeU64: return LLVMInt64TypeInContext(cg->ctx);
        case TypeF32: return LLVMFloatTypeInContext(cg->ctx);
        case TypeF64: return LLVMDoubleTypeInContext(cg->ctx);
        case TypeUser: {
            /* apply generic param substitution if active */
            const char *uname = ti.user_name;
            if (cg->generic_n > 0 && uname) {
                /* direct type param substitution: T → i32 */
                for (usize_t i = 0; i < cg->generic_n; i++) {
                    if (strcmp(uname, cg->generic_params[i]) == 0) {
                        type_info_t sub = cg->generic_concs[i];
                        if (ti.is_pointer) { sub.is_pointer = True; sub.ptr_perm = ti.ptr_perm; }
                        return get_llvm_base_type(cg, sub);
                    }
                }
                /* embedded substitution: arr_t_G_T → arr_t_G_i32 */
                const char *subst = cg_subst_name(cg, uname);
                if (subst != uname) uname = subst;
            }
            struct_reg_t *sr = uname ? find_struct(cg, uname) : Null;
            if (!sr && uname && strstr(uname, "_G_")) {
                /* lazy generic instantiation */
                try_instantiate_generic(cg, uname);
                sr = find_struct(cg, uname);
            }
            if (sr) return sr->llvm_type;
            enum_reg_t *er = uname ? find_enum(cg, uname) : Null;
            if (er) return er->llvm_type;
            return LLVMInt32TypeInContext(cg->ctx);
        }
        case TypeError:
            return cg->error_type;
        case TypeFnPtr:
            /* function pointers are opaque pointers in LLVM's opaque pointer model */
            return LLVMPointerTypeInContext(cg->ctx, 0);
        case TypeFuture:
            /* future is an opaque pointer to __future_t in the thread runtime */
            return LLVMPointerTypeInContext(cg->ctx, 0);
    }
    return LLVMVoidTypeInContext(cg->ctx);
}

static LLVMTypeRef get_llvm_type(cg_t *cg, type_info_t ti) {
    return get_llvm_base_type(cg, ti);
}

/* Build the LLVM function type for a TypeFnPtr descriptor. */
static LLVMTypeRef build_fn_ptr_llvm_type(cg_t *cg, fn_ptr_desc_t *desc) {
    LLVMTypeRef ret_ty = get_llvm_type(cg, desc->ret_type);
    LLVMTypeRef *param_types = Null;
    heap_t pt_heap = NullHeap;
    if (desc->param_count > 0) {
        pt_heap = allocate(desc->param_count, sizeof(LLVMTypeRef));
        param_types = pt_heap.pointer;
        for (usize_t i = 0; i < desc->param_count; i++)
            param_types[i] = get_llvm_type(cg, desc->params[i].type);
    }
    LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, param_types,
                                          (unsigned)desc->param_count, 0);
    if (desc->param_count > 0) deallocate(pt_heap);
    return fn_ty;
}

static LLVMValueRef coerce_int(cg_t *cg, LLVMValueRef val, LLVMTypeRef target) {
    LLVMTypeRef src = LLVMTypeOf(val);
    if (src == target) return val;
    LLVMTypeKind sk = LLVMGetTypeKind(src);
    LLVMTypeKind tk = LLVMGetTypeKind(target);
    if (sk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned tw = LLVMGetIntTypeWidth(target);
        unsigned sw = LLVMGetIntTypeWidth(src);
        if (tw > sw) return LLVMBuildSExt(cg->builder, val, target, "sext");
        if (tw < sw) return LLVMBuildTrunc(cg->builder, val, target, "trunc");
    }
    /* int -> float */
    if (sk == LLVMIntegerTypeKind && (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind))
        return LLVMBuildSIToFP(cg->builder, val, target, "itof");
    /* float -> int */
    if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) && tk == LLVMIntegerTypeKind)
        return LLVMBuildFPToSI(cg->builder, val, target, "ftoi");
    /* float -> float */
    if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
        (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
        if (sk == LLVMFloatTypeKind && tk == LLVMDoubleTypeKind)
            return LLVMBuildFPExt(cg->builder, val, target, "fpext");
        if (sk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
            return LLVMBuildFPTrunc(cg->builder, val, target, "fptrunc");
    }
    /* int -> ptr (e.g. lib call inferred as i32 but used where ptr expected) */
    if (sk == LLVMIntegerTypeKind && tk == LLVMPointerTypeKind)
        return LLVMBuildIntToPtr(cg->builder, val, target, "itoptr");
    /* ptr -> int */
    if (sk == LLVMPointerTypeKind && tk == LLVMIntegerTypeKind)
        return LLVMBuildPtrToInt(cg->builder, val, target, "ptrtoi");
    return val;
}

/* Create a nil error value: {i1 false, ptr null} */
static LLVMValueRef make_nil_error(cg_t *cg) {
    LLVMValueRef err = LLVMGetUndef(cg->error_type);
    err = LLVMBuildInsertValue(cg->builder, err,
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0), 0, "nil_err.has");
    err = LLVMBuildInsertValue(cg->builder, err,
        LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0)), 1, "nil_err.msg");
    return err;
}

static boolean_t llvm_is_float(LLVMTypeRef t) {
    LLVMTypeKind k = LLVMGetTypeKind(t);
    return k == LLVMFloatTypeKind || k == LLVMDoubleTypeKind;
}

static boolean_t llvm_is_int(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMIntegerTypeKind;
}

static boolean_t llvm_is_ptr(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMPointerTypeKind;
}

/* ── payload size helper ── */

static usize_t payload_type_size(type_info_t ti) {
    if (ti.is_pointer) return 8;
    switch (ti.base) {
        case TypeBool: case TypeI8:  case TypeU8:  return 1;
        case TypeI16:  case TypeU16:               return 2;
        case TypeI32:  case TypeU32: case TypeF32: return 4;
        case TypeI64:  case TypeU64: case TypeF64: return 8;
        default: return 8; /* conservative default for user types */
    }
}

/* ── infer type_info_t from an LLVM type (for let bindings) ── */

static type_info_t llvm_type_to_ti(LLVMTypeRef ty) {
    type_info_t ti = NO_TYPE;
    if (!ty) return ti;
    switch (LLVMGetTypeKind(ty)) {
        case LLVMIntegerTypeKind: {
            unsigned bits = LLVMGetIntTypeWidth(ty);
            if (bits == 1)       ti.base = TypeBool;
            else if (bits == 8)  ti.base = TypeI8;
            else if (bits == 16) ti.base = TypeI16;
            else if (bits == 32) ti.base = TypeI32;
            else                 ti.base = TypeI64;
            break;
        }
        case LLVMFloatTypeKind:   ti.base = TypeF32; break;
        case LLVMDoubleTypeKind:  ti.base = TypeF64; break;
        case LLVMPointerTypeKind:
            ti.base = TypeI8; ti.is_pointer = True;
            ti.ptr_perm = PtrRead | PtrWrite;
            break;
        case LLVMStructTypeKind: {
            const char *sname = LLVMGetStructName(ty);
            if (sname) {
                ti.base = TypeUser;
                ti.user_name = ast_strdup(sname, strlen(sname));
            }
            break;
        }
        default: break;
    }
    return ti;
}

/* ── alloca in entry block helper ── */

static LLVMValueRef alloc_in_entry(cg_t *cg, LLVMTypeRef type, const char *name) {
    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBasicBlockRef entry_bb = LLVMGetEntryBasicBlock(cg->current_fn);
    LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_bb);
    if (first_instr)
        LLVMPositionBuilderBefore(cg->builder, first_instr);
    else
        LLVMPositionBuilderAtEnd(cg->builder, entry_bb);
    LLVMValueRef a = LLVMBuildAlloca(cg->builder, type, name);
    LLVMPositionBuilderAtEnd(cg->builder, saved_bb);
    return a;
}
