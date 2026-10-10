/* ── DWARF debug info helpers ── */

/* Look up a cached DI type by name. Returns Null if not found. */
static LLVMMetadataRef di_cache_lookup(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->di_type_count; i++)
        if (strcmp(cg->di_types[i].name, name) == 0)
            return cg->di_types[i].di_type;
    return Null;
}

/* Insert or update a DI type in the cache. */
static void di_cache_set(cg_t *cg, const char *name, LLVMMetadataRef di_type) {
    for (usize_t i = 0; i < cg->di_type_count; i++) {
        if (strcmp(cg->di_types[i].name, name) == 0) {
            cg->di_types[i].di_type = di_type;
            return;
        }
    }
    if (cg->di_type_count >= cg->di_type_cap) {
        usize_t new_cap = cg->di_type_cap < 8 ? 8 : cg->di_type_cap * 2;
        if (cg->di_types_heap.pointer == Null)
            cg->di_types_heap = allocate(new_cap, sizeof(di_type_entry_t));
        else
            cg->di_types_heap = reallocate(cg->di_types_heap,
                                            new_cap * sizeof(di_type_entry_t));
        cg->di_types = cg->di_types_heap.pointer;
        cg->di_type_cap = new_cap;
    }
    cg->di_types[cg->di_type_count].name    = (char *)name;
    cg->di_types[cg->di_type_count].di_type = di_type;
    cg->di_type_count++;
}

/* Forward declaration so get_di_named_type can call get_di_type recursively. */
static LLVMMetadataRef get_di_type(cg_t *cg, type_info_t ti);

/*
 * Build (or retrieve from cache) the DWARF composite type for a named
 * user-defined type (struct, union, or enum).
 *
 * To break potential circular references (e.g. a struct with a pointer
 * field to itself), a sentinel unspecified-type is inserted into the cache
 * before members are processed; cycles get the sentinel rather than crashing.
 */
static LLVMMetadataRef get_di_named_type(cg_t *cg, const char *name) {
    if (!name) return Null;

    LLVMMetadataRef cached = di_cache_lookup(cg, name);
    if (cached) return cached;

    struct_reg_t *sr = find_struct(cg, name);
    enum_reg_t *er = find_enum(cg, name);
    boolean_t replaceable = sr || (er && er->is_tagged);
    /* A temporary composite is replaced once members are complete, preserving
       pointer-to-self debug types instead of permanently caching an unspecified
       sentinel for the recursive edge. DW_TAG_structure_type=0x13, union=0x17. */
    LLVMMetadataRef sentinel = replaceable
        ? LLVMDIBuilderCreateReplaceableCompositeType(cg->di_builder,
            sr && sr->is_union ? 0x17 : 0x13, name, strlen(name),
            cg->di_compile_unit, cg->di_file, 0, 0, 0, 0,
            LLVMDIFlagFwdDecl, "", 0)
        : LLVMDIBuilderCreateUnspecifiedType(cg->di_builder, name, strlen(name));
    di_cache_set(cg, name, sentinel);

    /* ── struct / union ── */
    if (sr) {
        uint64_t size_bits  = 0;
        uint32_t align_bits = 0;
        if (cg->di_data_layout) {
            size_bits  = LLVMABISizeOfType(cg->di_data_layout, sr->llvm_type) * 8;
            align_bits = (uint32_t)(LLVMABIAlignmentOfType(cg->di_data_layout,
                                                            sr->llvm_type) * 8);
        }

        usize_t member_count = sr->field_count;
        heap_t  members_heap = NullHeap;
        LLVMMetadataRef *members = Null;

        if (member_count > 0) {
            members_heap = allocate(member_count, sizeof(LLVMMetadataRef));
            members = members_heap.pointer;

            for (usize_t m = 0; m < member_count; m++) {
                field_info_t *f    = &sr->fields[m];
                LLVMMetadataRef mty = get_di_type(cg, f->type);
                LLVMTypeRef storage_type = f->llvm_type ? f->llvm_type : get_llvm_type(cg, f->type);
                if (LLVMGetTypeKind(storage_type) == LLVMArrayTypeKind) {
                    LLVMMetadataRef dims[8];
                    unsigned ndim = 0;
                    LLVMTypeRef elem = storage_type;
                    while (LLVMGetTypeKind(elem) == LLVMArrayTypeKind && ndim < 8) {
                        dims[ndim++] = LLVMDIBuilderGetOrCreateSubrange(cg->di_builder,
                            0, (int64_t)LLVMGetArrayLength2(elem));
                        elem = LLVMGetElementType(elem);
                    }
                    mty = LLVMDIBuilderCreateArrayType(cg->di_builder,
                        LLVMABISizeOfType(cg->di_data_layout, storage_type) * 8,
                        LLVMABIAlignmentOfType(cg->di_data_layout, storage_type) * 8,
                        mty, dims, ndim);
                }

                if (f->bit_width > 0) {
                    /* Bitfield: storage offset is the offset of the backing
                       integer field; bit offset is position within that integer. */
                    uint64_t storage_off_bits = 0;
                    if (cg->di_data_layout)
                        storage_off_bits =
                            LLVMOffsetOfElement(cg->di_data_layout,
                                                sr->llvm_type,
                                                (unsigned)f->index) * 8;
                    uint64_t field_off_bits =
                        storage_off_bits + (uint64_t)f->bit_offset;

                    members[m] = LLVMDIBuilderCreateBitFieldMemberType(
                        cg->di_builder, cg->di_compile_unit,
                        f->name, strlen(f->name),
                        cg->di_file, 0,
                        (uint64_t)f->bit_width,
                        field_off_bits,
                        storage_off_bits,
                        LLVMDIFlagZero, mty);
                } else {
                    /* Normal field. */
                    uint64_t offset_bits      = 0;
                    uint64_t member_size_bits = 0;
                    uint32_t member_align_bits = 0;
                    if (cg->di_data_layout) {
                        LLVMTypeRef mllvm = storage_type;
                        offset_bits      = LLVMOffsetOfElement(cg->di_data_layout,
                                                               sr->llvm_type,
                                                               (unsigned)f->index) * 8;
                        member_size_bits = LLVMABISizeOfType(cg->di_data_layout,
                                                             mllvm) * 8;
                        member_align_bits = (uint32_t)(
                            LLVMABIAlignmentOfType(cg->di_data_layout, mllvm) * 8);
                    }
                    if (sr->is_packed) member_align_bits = 8;
                    /* Unions: all fields start at offset 0. */
                    if (sr->is_union) offset_bits = 0;

                    members[m] = LLVMDIBuilderCreateMemberType(
                        cg->di_builder, cg->di_compile_unit,
                        f->name, strlen(f->name),
                        cg->di_file, 0,
                        member_size_bits, member_align_bits,
                        offset_bits,
                        LLVMDIFlagZero, mty);
                }
            }
        }

        LLVMMetadataRef di_composite;
        if (sr->is_union) {
            di_composite = LLVMDIBuilderCreateUnionType(
                cg->di_builder, cg->di_compile_unit,
                name, strlen(name),
                cg->di_file, 0,
                size_bits, align_bits,
                LLVMDIFlagZero,
                members, (unsigned)member_count,
                0, "", 0);
        } else {
            di_composite = LLVMDIBuilderCreateStructType(
                cg->di_builder, cg->di_compile_unit,
                name, strlen(name),
                cg->di_file, 0,
                size_bits, align_bits,
                LLVMDIFlagZero,
                Null,  /* no base struct */
                members, (unsigned)member_count,
                0, Null, /* no runtime lang, no vtable */
                "", 0);
        }

        if (member_count > 0) deallocate(members_heap);
        LLVMMetadataReplaceAllUsesWith(sentinel, di_composite);
        di_cache_set(cg, name, di_composite);
        return di_composite;
    }

    /* ── enum ── */
    if (er) {
        usize_t var_count = er->variant_count;
        heap_t  vars_heap = NullHeap;
        LLVMMetadataRef *vars = Null;

        if (var_count > 0) {
            vars_heap = allocate(var_count, sizeof(LLVMMetadataRef));
            vars = vars_heap.pointer;
            for (usize_t v = 0; v < var_count; v++) {
                variant_info_t *vi = &er->variants[v];
                vars[v] = LLVMDIBuilderCreateEnumerator(
                    cg->di_builder,
                    vi->name, strlen(vi->name),
                    vi->value, /* isUnsigned= */ 0);
            }
        }

        uint64_t size_bits = 32; /* the tag is always i32, not the whole union */

        /* Underlying integer type for the enum tag. */
        LLVMMetadataRef di_base = LLVMDIBuilderCreateBasicType(
            cg->di_builder, "i32", 3, 32,
            STS_DW_ATE_signed, LLVMDIFlagZero);

        LLVMMetadataRef di_enum = LLVMDIBuilderCreateEnumerationType(
            cg->di_builder, cg->di_compile_unit,
            name, strlen(name),
            cg->di_file, 0,
            size_bits, 32,
            vars, (unsigned)var_count,
            di_base);

        if (var_count > 0) deallocate(vars_heap);
        LLVMMetadataRef result = di_enum;
        if (er->is_tagged) {
            heap_t payload_heap = var_count ? allocate(var_count, sizeof(LLVMMetadataRef)) : NullHeap;
            LLVMMetadataRef *payloads = payload_heap.pointer;
            unsigned payload_count = 0;
            for (usize_t i = 0; i < var_count; i++) {
                variant_info_t *v = &er->variants[i];
                if (!v->has_payload) continue;
                LLVMTypeRef vt = get_llvm_type(cg, v->payload_type);
                payloads[payload_count++] = LLVMDIBuilderCreateMemberType(
                    cg->di_builder, cg->di_compile_unit, v->name, strlen(v->name),
                    cg->di_file, 0, LLVMABISizeOfType(cg->di_data_layout, vt) * 8,
                    LLVMABIAlignmentOfType(cg->di_data_layout, vt) * 8, 0,
                    LLVMDIFlagZero, get_di_type(cg, v->payload_type));
            }
            LLVMMetadataRef payload = LLVMDIBuilderCreateUnionType(cg->di_builder,
                cg->di_compile_unit, "payload", 7, cg->di_file, 0,
                er->payload_size * 8, er->payload_alignment * 8, LLVMDIFlagZero,
                payloads, payload_count, 0, "", 0);
            LLVMMetadataRef members[2] = {
                LLVMDIBuilderCreateMemberType(cg->di_builder, cg->di_compile_unit,
                    "tag", 3, cg->di_file, 0, 32, 32, 0, LLVMDIFlagZero, di_enum),
                LLVMDIBuilderCreateMemberType(cg->di_builder, cg->di_compile_unit,
                    "payload", 7, cg->di_file, 0, er->payload_size * 8,
                    er->payload_alignment * 8,
                    LLVMOffsetOfElement(cg->di_data_layout, er->llvm_type, 1) * 8,
                    LLVMDIFlagZero, payload)
            };
            result = LLVMDIBuilderCreateStructType(cg->di_builder,
                cg->di_compile_unit, name, strlen(name), cg->di_file, 0,
                LLVMABISizeOfType(cg->di_data_layout, er->llvm_type) * 8,
                LLVMABIAlignmentOfType(cg->di_data_layout, er->llvm_type) * 8,
                LLVMDIFlagZero, Null, members, 2, 0, Null, "", 0);
            if (payload_heap.pointer) deallocate(payload_heap);
            LLVMMetadataReplaceAllUsesWith(sentinel, result);
        }
        di_cache_set(cg, name, result);
        return result;
    }

    /* Unknown user type: keep the sentinel unspecified type. */
    return sentinel;
}

/*
 * Convert a Stasha type_info_t to an LLVMMetadataRef DWARF type.
 * Returns Null for void (which is how DWARF represents void).
 */
static LLVMMetadataRef get_di_type(cg_t *cg, type_info_t ti) {
    if (!cg->debug_mode) return Null;

    type_info_t resolved = cg_layout_resolve_type(cg, ti);

    if (resolved.is_pointer) {
        /* Remove one pointer level; recurse for multi-level pointers. */
        type_info_t pointee   = ti_deref_one(resolved);
        LLVMMetadataRef inner = get_di_type(cg, pointee);
        LLVMTypeRef ptr_type = LLVMPointerTypeInContext(cg->ctx, 0);
        return LLVMDIBuilderCreatePointerType(
            cg->di_builder, inner,
            LLVMABISizeOfType(cg->di_data_layout, ptr_type) * 8,
            LLVMABIAlignmentOfType(cg->di_data_layout, ptr_type) * 8, 0, "", 0);
    }

    switch (resolved.base) {
        case TypeVoid:
            return Null;
        case TypeBool:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "bool", 4, 8,
                STS_DW_ATE_boolean, LLVMDIFlagZero);
        case TypeI8:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i8",  2,  8,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeI16:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i16", 3, 16,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeI32:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i32", 3, 32,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeI64:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i64", 3, 64,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeU8:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u8",  2,  8,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeU16:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u16", 3, 16,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeU32:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u32", 3, 32,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeU64:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u64", 3, 64,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeF32:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "f32", 3, 32,
                STS_DW_ATE_float, LLVMDIFlagZero);
        case TypeF64:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "f64", 3, 64,
                STS_DW_ATE_float, LLVMDIFlagZero);
        case TypeError:
            /* Built-in error: represent as an opaque struct. */
            return LLVMDIBuilderCreateUnspecifiedType(
                cg->di_builder, "error", 5);
        case TypeFnPtr:
        case TypeFuture:
        case TypeStream:
        case TypeZone:
            /* Function pointers are opaque pointers in LLVM's model. */
            return LLVMDIBuilderCreatePointerType(
                cg->di_builder, Null,
            LLVMABISizeOfType(cg->di_data_layout, LLVMPointerTypeInContext(cg->ctx, 0)) * 8,
            LLVMABIAlignmentOfType(cg->di_data_layout, LLVMPointerTypeInContext(cg->ctx, 0)) * 8, 0, "", 0);
        case TypeUser:
            return get_di_named_type(cg, resolved.user_name);
    }
    return Null;
}

/*
 * Build a DILocation metadata node for (line, col=0) in the current scope.
 * Returns Null when debug info is disabled or no scope is set.
 */
static LLVMMetadataRef di_make_location(cg_t *cg, usize_t line) {
    if (!cg->debug_mode || !cg->di_scope || line == 0) return Null;
    return LLVMDIBuilderCreateDebugLocation(
        cg->ctx, (unsigned)line, 0, cg->di_scope, Null);
}

/*
 * Attach a debug location to the IR builder so that all subsequently
 * emitted instructions carry source-line information.
 */
static void di_set_location(cg_t *cg, usize_t line) {
    if (!cg->debug_mode || !cg->di_scope || line == 0) return;
    LLVMMetadataRef loc = di_make_location(cg, line);
    LLVMSetCurrentDebugLocation2(cg->builder, loc);
}
