/* Static initialization must produce the requested constant or a diagnostic.
 * Opaque LLVM pointers are not evidence of source-type compatibility. */
typedef struct {
    LLVMValueRef value;
    LLVMTypeRef pointee;
    type_info_t source_type; /* array indexing retains the declared element type */
    symbol_t *root;
    boolean_t readonly;
    boolean_t one_past;
} cg_global_address_t;

static unsigned cg_global_pointer_depth(type_info_t ti) {
    return ti.is_pointer ? (unsigned)(ti.ptr_depth > 0 ? ti.ptr_depth : 1) : 0;
}

/* Nested pointer permissions are invariant: accepting **rw as **r would let a
 * store through the outer pointer replace a differently qualified pointer. */
static boolean_t cg_global_same_type(cg_t *cg, type_info_t a, type_info_t b, unsigned depth) {
    if (depth > 64) return False;
    a = cg_layout_resolve_type(cg, a); b = cg_layout_resolve_type(cg, b);
    unsigned ap = cg_global_pointer_depth(a), bp = cg_global_pointer_depth(b);
    if (a.base != b.base || ap != bp || ap > 8 || a.nullable != b.nullable) return False;
    for (unsigned i = 0; i < ap; ++i) {
        ptr_perm_t pa = i ? a.ptr_perms[i] : a.ptr_perm;
        ptr_perm_t pb = i ? b.ptr_perms[i] : b.ptr_perm;
        if (pa != pb) return False;
    }
    if (a.base == TypeUser)
        return a.user_name && b.user_name && !strcmp(a.user_name, b.user_name);
    if (a.base == TypeSlice)
        return a.elem_type && b.elem_type && cg_global_same_type(cg, *a.elem_type, *b.elem_type, depth + 1);
    if (a.base == TypeFnPtr) {
        fn_ptr_desc_t *af = a.fn_ptr_desc, *bf = b.fn_ptr_desc;
        if (!af || !bf || af->param_count != bf->param_count ||
            !cg_global_same_type(cg, af->ret_type, bf->ret_type, depth + 1)) return False;
        for (usize_t i = 0; i < af->param_count; ++i)
            if (af->params[i].storage != bf->params[i].storage ||
                !cg_global_same_type(cg, af->params[i].type, bf->params[i].type, depth + 1)) return False;
    }
    return True;
}

/* Only outer permission narrowing is allowed when assigning an already typed
 * pointer value. In particular an explicit cast cannot silently change type a
 * second time when assigned to a different declared destination. */
static boolean_t cg_global_pointer_assignable(cg_t *cg, type_info_t from, type_info_t to) {
    from = cg_layout_resolve_type(cg, from); to = cg_layout_resolve_type(cg, to);
    if (!from.is_pointer || !to.is_pointer ||
        (to.ptr_perm & ~from.ptr_perm & (PtrRead | PtrWrite | PtrArith))) return False;
    from.ptr_perm = to.ptr_perm;
    from.ptr_perms[0] = to.ptr_perms[0];
    return cg_global_same_type(cg, from, to, 0);
}

static boolean_t cg_global_index(cg_t *cg, node_t *n, uint64_t *index) {
    scalar_constant_t c;
    if (!scalar_const_eval(cg, n, NO_TYPE, &c) || !scalar_integer(c.type)) return False;
    if (!scalar_unsigned(c.type) && (c.bits & (UINT64_C(1) << (scalar_bits(c.type) - 1)))) return False;
    *index = c.bits;
    return True;
}

static boolean_t cg_global_address(cg_t *cg, node_t *n, cg_global_address_t *out, unsigned depth) {
    if (!n || depth > 256) return False;
    if (n->kind == NodeIdentExpr) {
        symbol_t *sym = scalar_symbol(cg, n->as.ident.name);
        if (!sym || (!LLVMIsAGlobalVariable(sym->value) && !LLVMIsAFunction(sym->value))) return False;
        /* A TLS address is not a static relocation constant. */
        if (LLVMIsAGlobalVariable(sym->value) && LLVMIsThreadLocal(sym->value)) return False;
        out->value = sym->value;
        out->pointee = LLVMGlobalGetValueType(sym->value);
        out->source_type = cg_layout_resolve_type(cg, sym->stype);
        out->root = sym;
        out->readonly = (sym->flags & (SymConst | SymFinal)) != 0;
        out->one_past = False;
        return True;
    }
    if (n->kind == NodeIndexExpr) {
        if (!cg_global_address(cg, n->as.index_expr.object, out, depth + 1) || out->one_past ||
            LLVMGetTypeKind(out->pointee) != LLVMArrayTypeKind) return False;
        uint64_t index;
        if (!cg_global_index(cg, n->as.index_expr.index, &index) ||
            index > LLVMGetArrayLength2(out->pointee)) return False;
        LLVMTypeRef index_type = LLVMIntTypeInContext(cg->ctx, cg->target_spec.pointer_bits);
        LLVMValueRef indices[] = {LLVMConstInt(index_type, 0, 0), LLVMConstInt(index_type, index, 0)};
        out->one_past = index == LLVMGetArrayLength2(out->pointee);
        out->value = LLVMConstGEP2(out->pointee, out->value, indices, 2);
        out->pointee = LLVMGetElementType(out->pointee);
        return True;
    }
    if (n->kind == NodeMemberExpr) {
        if (!cg_global_address(cg, n->as.member_expr.object, out, depth + 1) || out->one_past ||
            LLVMGetTypeKind(out->pointee) != LLVMStructTypeKind) return False;
        struct_reg_t *sr = find_struct_by_llvm_type(cg, out->pointee);
        if (!sr || sr->is_union) return False;
        for (usize_t i = 0; i < sr->field_count; ++i) {
            field_info_t *field = &sr->fields[i];
            if (!strcmp(field->name, n->as.member_expr.field)) {
                if (field->bit_width || field->index >= LLVMCountStructElementTypes(out->pointee)) return False;
                LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);
                LLVMValueRef indices[] = {LLVMConstInt(i32, 0, 0), LLVMConstInt(i32, field->index, 0)};
                out->value = LLVMConstGEP2(out->pointee, out->value, indices, 2);
                out->pointee = LLVMStructGetTypeAtIndex(out->pointee, (unsigned)field->index);
                out->source_type = cg_layout_resolve_type(cg, field->type);
                if (sr->layout_decl) {
                    node_list_t fields = sr->layout_decl->as.type_decl.fields;
                    for (usize_t j = 0; j < fields.count; ++j)
                        if (!strcmp(fields.items[j]->as.var_decl.name, field->name) &&
                            (fields.items[j]->as.var_decl.flags & (VdeclConst | VdeclFinal))) out->readonly = True;
                }
                return True;
            }
        }
    }
    return False;
}

static boolean_t cg_global_function_matches(cg_t *cg, type_info_t target, symbol_t *sym) {
    if (target.is_pointer || target.base != TypeFnPtr || !target.fn_ptr_desc) return False;
    node_t *decl = scalar_fn_named(cg, sym->name);
    if (!decl || decl->as.fn_decl.is_variadic || decl->as.fn_decl.return_count != 1 ||
        decl->as.fn_decl.params.count != target.fn_ptr_desc->param_count) return False;
    if (!cg_global_same_type(cg, target.fn_ptr_desc->ret_type, decl->as.fn_decl.return_types[0], 0)) return False;
    for (usize_t i = 0; i < decl->as.fn_decl.params.count; ++i) {
        node_t *param = decl->as.fn_decl.params.items[i];
        fn_ptr_param_t expected = target.fn_ptr_desc->params[i];
        if ((param->as.var_decl.flags & VdeclArray) || expected.storage != param->as.var_decl.storage ||
            !cg_global_same_type(cg, expected.type, param->as.var_decl.type, 0)) return False;
    }
    /* Also catches implicit receiver parameters or other ABI differences. */
    return LLVMGlobalGetValueType(sym->value) == build_fn_ptr_llvm_type(cg, target.fn_ptr_desc);
}

static LLVMValueRef cg_global_constant_inner(cg_t *cg, node_t *n, type_info_t ti,
                                             LLVMTypeRef type, storage_t storage,
                                             unsigned depth, uint64_t *budget) {
    if (!n) return LLVMConstNull(type);
    if (depth > 256) {
        scalar_error(n, "global initializer nesting limit exceeded");
        return LLVMConstNull(type);
    }
    ti = cg_layout_resolve_type(cg, ti);
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMIntegerTypeKind) {
        LLVMValueRef value = scalar_global_initializer(cg, n, scalar_resolve(cg, ti));
        if (LLVMTypeOf(value) == type) return value;
        goto incompatible;
    }
    if (kind == LLVMPointerTypeKind) {
        if (!ti.is_pointer && ti.base != TypeFnPtr) goto unsupported;
        if (n->kind == NodeNilExpr) return LLVMConstNull(type);
        if (n->kind == NodeCastExpr) {
            type_info_t cast_type = cg_layout_resolve_type(cg, n->as.cast_expr.target);
            if (!cg_global_pointer_assignable(cg, cast_type, ti)) goto incompatible;
            scalar_constant_t value;
            if (!scalar_const_eval(cg, n->as.cast_expr.expr, NO_TYPE, &value) || !scalar_integer(value.type)) goto unsupported;
            LLVMValueRef integer = LLVMConstInt(get_llvm_type(cg, value.type), value.bits, 0);
            return LLVMConstIntToPtr(integer, type);
        }
        if (n->kind == NodeStrLitExpr && !n->as.str_lit.is_heap) {
            if (!ti.is_pointer || cg_global_pointer_depth(ti) != 1 ||
                (ti.base != TypeI8 && ti.base != TypeU8)) goto incompatible;
            if (ti.ptr_perm & PtrWrite) {
                scalar_error(n, "static string literal requires a read-only byte pointer");
                return LLVMConstNull(type);
            }
            if (storage == StorageHeap) goto storage_mismatch;
            LLVMValueRef text = LLVMConstStringInContext2(cg->ctx, n->as.str_lit.value, n->as.str_lit.len, 0);
            LLVMTypeRef array_type = LLVMTypeOf(text);
            LLVMValueRef global = LLVMAddGlobal(cg->module, array_type, ".stasha.string");
            LLVMSetLinkage(global, LLVMPrivateLinkage);
            LLVMSetGlobalConstant(global, 1);
            LLVMSetInitializer(global, text);
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            LLVMValueRef indices[] = {zero, zero};
            return LLVMConstGEP2(array_type, global, indices, 2);
        }
        if (n->kind == NodeAddrOf) {
            cg_global_address_t address = {0};
            if (!cg_global_address(cg, n->as.addr_of.operand, &address, 0)) goto unsupported;
            if (LLVMIsAFunction(address.value)) {
                if (storage == StorageHeap) goto storage_mismatch;
                if (!cg_global_function_matches(cg, ti, address.root)) goto incompatible;
                return address.value;
            }
            type_info_t pointee_type = ti_deref_one(ti);
            /* The nullable flag qualifies the destination pointer itself. */
            pointee_type.nullable = False;
            if (!ti.is_pointer || !cg_global_same_type(cg, pointee_type, address.source_type, 0) ||
                get_llvm_type(cg, pointee_type) != address.pointee) goto incompatible;
            if (address.readonly && (ti.ptr_perm & PtrWrite)) {
                scalar_error(n, "cannot derive a writable static pointer from const or final storage");
                return LLVMConstNull(type);
            }
            if (storage == StorageHeap) goto storage_mismatch;
            return address.value;
        }
    }
    if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind) {
        if (n->kind == NodeFloatLitExpr) return LLVMConstReal(type, n->as.float_lit.value);
        if (n->kind == NodeIntLitExpr) return LLVMConstReal(type, (double)n->as.int_lit.value);
        if (n->kind == NodeUnaryPrefixExpr && n->as.unary.op == TokMinus &&
            n->as.unary.operand->kind == NodeFloatLitExpr)
            return LLVMConstReal(type, -n->as.unary.operand->as.float_lit.value);
    }
    if ((kind == LLVMArrayTypeKind || kind == LLVMStructTypeKind) && n->kind == NodeCompoundInit) {
        uint64_t count = kind == LLVMArrayTypeKind ? LLVMGetArrayLength2(type) : LLVMCountStructElementTypes(type);
        struct_reg_t *sr = kind == LLVMStructTypeKind ? find_struct_by_llvm_type(cg, type) : Null;
        if (kind == LLVMStructTypeKind && (!sr || sr->is_union)) goto unsupported;
        if (count > *budget) {
            scalar_error(n, "global aggregate initializer expansion limit exceeded");
            return LLVMConstNull(type);
        }
        *budget -= count;
        heap_t values_heap = allocate(count ? count : 1, sizeof(LLVMValueRef));
        LLVMValueRef *values = values_heap.pointer;
        for (uint64_t i = 0; i < count; ++i) {
            LLVMTypeRef element = kind == LLVMArrayTypeKind ? LLVMGetElementType(type) : LLVMStructGetTypeAtIndex(type, (unsigned)i);
            values[i] = LLVMConstNull(element);
        }
        uint64_t cursor = 0;
        for (usize_t item = 0; item < n->as.compound_init.items.count; ++item) {
            node_t *value = n->as.compound_init.items.items[item];
            uint64_t index = cursor;
            if (value->kind == NodeInitField) {
                if (!sr) { scalar_error(value, "field designator requires a struct static initializer"); continue; }
                index = count;
                for (usize_t i = 0; i < sr->field_count; ++i)
                    if (!strcmp(sr->fields[i].name, value->as.init_field.name)) index = sr->fields[i].index;
                value = value->as.init_field.value;
            } else if (value->kind == NodeInitIndex) {
                if (kind != LLVMArrayTypeKind || !cg_global_index(cg, value->as.init_index.index, &index)) {
                    scalar_error(value, "array static initializer designator requires a nonnegative constant index"); continue;
                }
                value = value->as.init_index.value;
            }
            if (index >= count) {
                scalar_error(value, "global aggregate initializer index or field is out of range");
                continue;
            }
            LLVMTypeRef element = kind == LLVMArrayTypeKind ? LLVMGetElementType(type) : LLVMStructGetTypeAtIndex(type, (unsigned)index);
            type_info_t element_ti = ti;
            storage_t element_storage = storage;
            if (sr) {
                element_ti = NO_TYPE;
                for (usize_t i = 0; i < sr->field_count; ++i)
                    if (sr->fields[i].index == index) {
                        element_ti = sr->fields[i].type;
                        element_storage = sr->fields[i].storage;
                        break;
                    }
            }
            values[index] = cg_global_constant_inner(cg, value, element_ti, element, element_storage, depth + 1, budget);
            if (LLVMTypeOf(values[index]) != element) {
                scalar_error(value, "static initializer does not match aggregate element storage type");
                values[index] = LLVMConstNull(element);
            }
            cursor = index + 1;
        }
        LLVMValueRef result = kind == LLVMArrayTypeKind
            ? LLVMConstArray2(LLVMGetElementType(type), values, count)
            : LLVMConstNamedStruct(type, values, (unsigned)count);
        deallocate(values_heap);
        return result;
    }
unsupported:
    scalar_error(n, "unsupported static initializer; use a representable constant or explicit runtime initialization");
    return LLVMConstNull(type);
incompatible:
    scalar_error(n, "static initializer source type is incompatible with its declared destination type");
    return LLVMConstNull(type);
storage_mismatch:
    scalar_error(n, "static storage address cannot initialize a heap-owned pointer");
    return LLVMConstNull(type);
}

static LLVMValueRef cg_global_constant(cg_t *cg, node_t *n, type_info_t ti,
                                      LLVMTypeRef type, storage_t storage, unsigned depth) {
    uint64_t budget = UINT64_C(1048576);
    return cg_global_constant_inner(cg, n, ti, type, storage, depth, &budget);
}
