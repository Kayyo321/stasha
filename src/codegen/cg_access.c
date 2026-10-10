/* Shared hosted memory access lowering. These helpers preserve source element
 * types and access permissions; they are not a CFG lifetime/nullability proof. */

/* Storage shape is independent from source scalar type: an array of pointers
 * has pointer element type but ARRAY storage, and must not load a pointer slot
 * until an index has selected that slot. This query emits no instructions. */
static LLVMTypeRef access_storage_type(cg_t *cg, node_t *node) {
    if (!node) return Null;
    if (node->kind == NodeIdentExpr) {
        symbol_t *sym = scalar_symbol(cg, node->as.ident.name);
        return sym && !LLVMIsAFunction(sym->value) ? sym->type : Null;
    }
    if (node->kind == NodeMemberExpr || node->kind == NodeSelfMemberExpr) {
        type_info_t source = NO_TYPE;
        const char *field;
        if (node->kind == NodeMemberExpr) {
            source = scalar_resolve(cg, scalar_expr_type(cg, node->as.member_expr.object, NO_TYPE));
            field = node->as.member_expr.field;
        } else {
            source.base = TypeUser;
            source.user_name = node->as.self_member.type_name ? node->as.self_member.type_name : cg->current_struct_name;
            field = node->as.self_member.field;
        }
        const char *name = source.user_name ? cg_subst_name(cg, source.user_name) : Null;
        struct_reg_t *sr = name ? find_struct(cg, name) : Null;
        if (sr) for (usize_t i = 0; i < sr->field_count; ++i) {
            field_info_t *f = &sr->fields[i];
            if (!strcmp(f->name, field))
                return f->llvm_type ? f->llvm_type : get_llvm_type(cg, f->type);
        }
        return Null;
    }
    if (node->kind == NodeIndexExpr || node->kind == NodeFlaggedIndex) {
        node_t *object = node->kind == NodeIndexExpr ? node->as.index_expr.object : node->as.flagged_index.object;
        LLVMTypeRef storage = access_storage_type(cg, object);
        if (storage && LLVMGetTypeKind(storage) == LLVMArrayTypeKind)
            return LLVMGetElementType(storage);
    }
    type_info_t t = scalar_resolve(cg, scalar_expr_type(cg, node, NO_TYPE));
    return t.base != TypeVoid || t.is_pointer ? get_llvm_type(cg, t) : Null;
}

/* Permissions of an address expression describe the selected storage, not the
 * mutability of the local pointer slot holding its address. */
static ptr_perm_t access_address_permissions(cg_t *cg, node_t *node) {
    if (!node) return PtrNone;
    if (node->kind == NodeIdentExpr) {
        symbol_t *sym = scalar_symbol(cg, node->as.ident.name);
        return sym && (sym->flags & (SymConst | SymFinal)) ? PtrRead : PtrReadWrite;
    }
    node_t *object = Null;
    if (node->kind == NodeMemberExpr) object = node->as.member_expr.object;
    else if (node->kind == NodeIndexExpr) object = node->as.index_expr.object;
    else if (node->kind == NodeFlaggedIndex) object = node->as.flagged_index.object;
    else if (node->kind == NodeUnaryPrefixExpr && node->as.unary.op == TokStar)
        return scalar_resolve(cg, scalar_expr_type(cg, node->as.unary.operand, NO_TYPE)).ptr_perm;
    if (object) {
        LLVMTypeRef storage = access_storage_type(cg, object);
        type_info_t source = scalar_resolve(cg, scalar_expr_type(cg, object, NO_TYPE));
        /* Indexing a slice selects its backing buffer, not its descriptor.
         * Readonly access to a containing object does not freeze that buffer. */
        if ((node->kind == NodeIndexExpr || node->kind == NodeFlaggedIndex) &&
            source.base == TypeSlice && !source.is_pointer &&
            (!storage || LLVMGetTypeKind(storage) != LLVMArrayTypeKind))
            return PtrReadWrite;
        if (source.is_pointer && (!storage || LLVMGetTypeKind(storage) != LLVMArrayTypeKind))
            return source.ptr_perm;
        return access_address_permissions(cg, object);
    }
    return PtrReadWrite;
}

static boolean_t access_check_pointer(cg_t *cg, node_t *object, node_t *site,
                                      type_info_t pointer, ptr_perm_t access) {
    if (cg->in_unsafe > 0) return True;
    if ((access & PtrRead) && !(pointer.ptr_perm & PtrRead)) {
        scalar_error(site, "memory access requires a readable pointer"); return False;
    }
    if ((access & PtrWrite) && !(pointer.ptr_perm & PtrWrite)) {
        scalar_error(site, "memory assignment requires a writable pointer"); return False;
    }
    if (object && object->kind == NodeIdentExpr) {
        symbol_t *sym = scalar_symbol(cg, object->as.ident.name);
        if (sym && (sym->flags & SymNil)) {
            check_null_deref(cg, object->as.ident.name, site->line);
            return False;
        }
    }
    return True;
}

static LLVMValueRef access_deref_address(cg_t *cg, node_t *node,
                                         LLVMTypeRef *type, ptr_perm_t access) {
    node_t *object = node->as.unary.operand;
    type_info_t pointer = scalar_resolve(cg, scalar_expr_type(cg, object, NO_TYPE));
    if (!pointer.is_pointer) {
        scalar_error(node, "dereference requires a resolved pointer type"); return Null;
    }
    type_info_t element = scalar_resolve(cg, ti_deref_one(pointer));
    if (element.base == TypeVoid && !element.is_pointer) {
        scalar_error(node, "dereference of void pointer requires an explicit typed cast"); return Null;
    }
    if (!access_check_pointer(cg, object, node, pointer, access)) return Null;
    *type = get_llvm_type(cg, element);
    LLVMValueRef address = gen_expr(cg, object);
    if (!llvm_is_ptr(LLVMTypeOf(address))) {
        scalar_error(node, "dereference operand did not produce a pointer"); return Null;
    }
    return address;
}

static LLVMValueRef access_index_value(cg_t *cg, node_t *index_node) {
    type_info_t type = scalar_resolve(cg, scalar_expr_type(cg, index_node, NO_TYPE));
    if (!scalar_integer(type)) {
        scalar_error(index_node, "array index must have integer type"); return Null;
    }
    LLVMValueRef index = gen_expr(cg, index_node);
    if (!llvm_is_int(LLVMTypeOf(index))) {
        scalar_error(index_node, "array index did not produce an integer"); return Null;
    }
    return LLVMBuildIntCast2(cg->builder, index, LLVMInt64TypeInContext(cg->ctx),
                             !scalar_unsigned(type), "index.wide");
}

/* Terminal address formation is distinct from permissions. It permits only a
 * final fixed-array one-past address; recursive bases always select elements. */
typedef enum { AccessElement, AccessTerminalAddress } access_purpose_t;

/* Descriptor shape is mandatory even for explicitly unchecked indexing. This
 * validates numbers only, not backing allocation, initialization or lifetime. */
static void access_slice_shape(cg_t *cg, LLVMValueRef length, LLVMValueRef capacity) {
    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    LLVMValueRef negative = LLVMBuildICmp(cg->builder, LLVMIntSLT, length, zero, "slice.len.negative");
    LLVMValueRef short_cap = LLVMBuildICmp(cg->builder, LLVMIntSLT, capacity, length, "slice.cap.short");
    scalar_trap_if(cg, LLVMBuildOr(cg->builder, negative, short_cap, "slice.shape.invalid"), 6);
}

/* Source-signed widening plus unsigned comparison rejects negative signed
 * indices and large u64 values without truncating either index or extent. */
static void access_extent_bounds(cg_t *cg, LLVMValueRef index, LLVMValueRef extent,
                                  boolean_t unchecked, access_purpose_t purpose) {
    if (unchecked) return;
    LLVMValueRef bad = LLVMBuildICmp(cg->builder,
        purpose == AccessTerminalAddress ? LLVMIntUGT : LLVMIntUGE,
        index, extent, "index.out.of.bounds");
    scalar_trap_if(cg, bad, 6);
}

static LLVMValueRef access_index_address(cg_t *cg, node_t *node,
                                         LLVMTypeRef *type, symbol_t **root,
                                         ptr_perm_t access, access_purpose_t purpose) {
    boolean_t unchecked = node->kind == NodeFlaggedIndex;
    if (unchecked && cg->in_unsafe == 0) {
        scalar_error(node, "unchecked indexing requires an unsafe block"); return Null;
    }
    node_t *object = unchecked ? node->as.flagged_index.object : node->as.index_expr.object;
    node_t *index_node = unchecked ? node->as.flagged_index.index : node->as.index_expr.index;
    LLVMTypeRef storage = access_storage_type(cg, object);
    type_info_t source = scalar_resolve(cg, scalar_expr_type(cg, object, NO_TYPE));
    if (storage && LLVMGetTypeKind(storage) == LLVMArrayTypeKind) {
        LLVMTypeRef actual;
        LLVMValueRef base = scalar_member_address(cg, object, &actual, root, access);
        if (!base) return Null;
        LLVMValueRef index = access_index_value(cg, index_node);
        if (!index) return Null;
        if (!actual || LLVMGetTypeKind(actual) != LLVMArrayTypeKind) {
            scalar_error(node, "indexing requires actual array storage"); return Null;
        }
        LLVMValueRef extent = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), LLVMGetArrayLength2(actual), 0);
        access_extent_bounds(cg, index, extent, unchecked, purpose);
        *type = LLVMGetElementType(actual);
        LLVMValueRef indices[2] = {LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0), index};
        return LLVMBuildGEP2(cg->builder, actual, base, indices, 2, "array.address");
    }
    if (source.base == TypeSlice && !source.is_pointer) {
        if (!source.elem_type) { scalar_error(node, "slice access requires a resolved element type"); return Null; }
        /* Loading a slice-valued field reads its descriptor; it does not write
         * the containing object when its backing buffer is mutated. */
        LLVMValueRef slice = gen_expr(cg, object);
        if (LLVMGetTypeKind(LLVMTypeOf(slice)) != LLVMStructTypeKind) {
            scalar_error(node, "slice access requires a slice descriptor"); return Null;
        }
        LLVMValueRef index = access_index_value(cg, index_node);
        if (!index) return Null;
        LLVMValueRef data = LLVMBuildExtractValue(cg->builder, slice, 0, "slice.data");
        LLVMValueRef length = LLVMBuildExtractValue(cg->builder, slice, 1, "slice.len");
        LLVMValueRef capacity = LLVMBuildExtractValue(cg->builder, slice, 2, "slice.cap");
        access_slice_shape(cg, length, capacity);
        LLVMValueRef extent = LLVMBuildZExt(cg->builder, length, LLVMInt64TypeInContext(cg->ctx), "slice.length");
        /* Slice addresses always select an element, including terminal &s[i]. */
        access_extent_bounds(cg, index, extent, unchecked, AccessElement);
        *type = get_slice_elem_llvm_type(cg, source.elem_type[0]);
        return LLVMBuildGEP2(cg->builder, *type, data, &index, 1, "slice.address");
    }
    if (source.is_pointer) {
        if (!access_check_pointer(cg, object, node, source, access)) return Null;
        type_info_t element = scalar_resolve(cg, ti_deref_one(source));
        if (element.base == TypeVoid && !element.is_pointer) {
            scalar_error(node, "indexing void pointer requires an explicit typed cast"); return Null;
        }
        /* gen_expr loads a pointer-valued member with READ access to its slot.
         * Only the subsequent pointee access uses the requested final mask. */
        LLVMValueRef base = gen_expr(cg, object);
        if (!llvm_is_ptr(LLVMTypeOf(base))) { scalar_error(node, "index operand did not produce a pointer"); return Null; }
        LLVMValueRef index = access_index_value(cg, index_node);
        if (!index) return Null;
        *type = get_llvm_type(cg, element);
        if (root && object->kind == NodeIdentExpr) *root = scalar_symbol(cg, object->as.ident.name);
        return LLVMBuildGEP2(cg->builder, *type, base, &index, 1, "pointer.address");
    }
    scalar_error(node, "cannot index a non-array, non-slice or non-pointer type");
    return Null;
}
