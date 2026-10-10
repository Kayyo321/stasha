/* Authoritative object layout. All sizes, alignments and member offsets come
 * from the selected LLVM DataLayout, never from compiler-host sizeof or guessed
 * source-type sizes. Aggregate calling conventions are a separate ABI concern. */

#include <limits.h>
#include "cg_layout_storage.c"

static type_info_t subst_type_info(cg_t *cg, type_info_t ti);
static boolean_t eval_comptime_expr(cg_t *cg, node_t *expr, long *out);
static boolean_t cg_layout_struct(cg_t *cg, const char *name);
static boolean_t cg_layout_enum(cg_t *cg, const char *name);

static void cg_layout_error(usize_t line, const char *message, const char *name) {
    diag_begin_error("%s '%s'", message, name ? name : "<unnamed>");
    diag_span(SRC_LOC(line, 1, 1), True, "unsupported or invalid object layout");
    diag_finish();
}

/* A union needs both maximum allocation size and maximum ABI alignment. Use a
 * real member as an alignment anchor plus trailing bytes; LLVM provides the
 * final tail padding. A byte array alone loses alignment. All variant pointers
 * still address byte zero of this storage (opaque LLVM pointers). */
static uint64_t cg_layout_max_size(cg_t *cg) {
    unsigned bits = LLVMPointerSize(cg->di_data_layout) * 8;
    return bits >= 64 ? INT64_MAX : (UINT64_C(1) << (bits - 1)) - 1;
}

/* Validate additions and tail padding before asking LLVM to size the body. */
static boolean_t cg_layout_fits(cg_t *cg, LLVMTypeRef *types, usize_t count,
                                boolean_t packed, usize_t line) {
    uint64_t size = 0, limit = cg_layout_max_size(cg);
    unsigned alignment = 1;
    for (usize_t i = 0; i < count; i++) {
        unsigned a = packed ? 1 : LLVMABIAlignmentOfType(cg->di_data_layout, types[i]);
        uint64_t n = LLVMABISizeOfType(cg->di_data_layout, types[i]);
        uint64_t pad = (a - size % a) % a;
        if (pad > limit - size || n > limit - size - pad) {
            cg_layout_error(line, "object exceeds target addressable size", "aggregate");
            return False;
        }
        size += pad + n;
        if (a > alignment) alignment = a;
    }
    if ((alignment - size % alignment) % alignment > limit - size) {
        cg_layout_error(line, "object tail padding exceeds target addressable size", "aggregate");
        return False;
    }
    return True;
}

static LLVMTypeRef cg_layout_storage(cg_t *cg, LLVMTypeRef *members,
                                    usize_t count, boolean_t packed) {
    if (!cg->di_data_layout) {
        cg_layout_error(0, "missing target DataLayout for", "union storage");
        return Null;
    }
    for (usize_t i = 0; i < count; i++) {
        if (!members[i] || !LLVMTypeIsSized(members[i])) {
            cg_layout_error(0, "incomplete payload type in", "union storage");
            return Null;
        }
    }
    uint64_t max_size = 1;
    unsigned align = 1;
    for (usize_t i = 0; i < count; i++) {
        uint64_t n = LLVMABISizeOfType(cg->di_data_layout, members[i]);
        unsigned a = LLVMABIAlignmentOfType(cg->di_data_layout, members[i]);
        if (n > max_size) max_size = n;
        if (!packed && a > align) align = a;
    }
    uint64_t limit = cg_layout_max_size(cg);
    if (max_size > limit || (align - max_size % align) % align > limit - max_size) {
        cg_layout_error(0, "union storage exceeds target addressable size", "payload");
        return Null;
    }
    return sts_layout_storage(cg->ctx, cg->di_data_layout, members, count, packed);
}

static type_info_t cg_layout_resolve_type(cg_t *cg, type_info_t ti) {
    ti = subst_type_info(cg, ti);
    /* resolve_alias historically follows only one link. Layout must not fall
       back to i32 for a chain, nor recurse without a bound for a cyclic alias. */
    for (usize_t depth = 0; depth <= cg->alias_count; depth++) {
        type_info_t next = resolve_alias(cg, ti);
        if (next.base == ti.base && next.user_name == ti.user_name
                && next.is_pointer == ti.is_pointer) return next;
        ti = subst_type_info(cg, next);
    }
    return ti; /* an unresolved alias is diagnosed by require_type below */
}

static boolean_t cg_layout_require_type(cg_t *cg, type_info_t ti, usize_t line) {
    ti = cg_layout_resolve_type(cg, ti);
    if (ti.is_pointer) return True; /* finite indirection, including self */
    if (ti.base == TypeUser && ti.user_name) {
        const char *name = ti.user_name;
        struct_reg_t *sr = find_struct(cg, name);
        enum_reg_t *er = find_enum(cg, name);
        if (sr) return cg_layout_struct(cg, name);
        if (er) return cg_layout_enum(cg, name);
        if (strstr(name, "_G_")) {
            /* Existing lazy generic/any creation registers its concrete body. */
            LLVMTypeRef type = get_llvm_type(cg, ti);
            sr = find_struct(cg, name);
            if (sr && type && LLVMTypeIsSized(type) && sr->layout_state != 3)
                return True;
        }
        cg_layout_error(line, "unknown or cyclic by-value type", name);
        return False;
    }
    LLVMTypeRef type = get_llvm_type(cg, ti);
    if (!type || !LLVMTypeIsSized(type)) {
        cg_layout_error(line, "unsized by-value type in", "field or payload");
        return False;
    }
    return True;
}

static LLVMTypeRef cg_layout_field_type(cg_t *cg, node_t *field, type_info_t *ti) {
    *ti = cg_layout_resolve_type(cg, field->as.var_decl.type);
    if (!cg_layout_require_type(cg, *ti, field->line)) return Null;
    LLVMTypeRef type = get_llvm_type(cg, *ti);
    if (field->as.var_decl.flags & VdeclArray) {
        int ndim = field->as.var_decl.array_ndim;
        if (ndim < 1 || ndim > 8) {
            cg_layout_error(field->line, "invalid array dimensions for", field->as.var_decl.name);
            return Null;
        }
        for (int d = ndim - 1; d >= 0; d--) {
            long n = field->as.var_decl.array_sizes[d];
            if (field->as.var_decl.array_size_names[d] || n <= 0) {
                cg_layout_error(field->line, "array field needs a positive literal extent", field->as.var_decl.name);
                return Null;
            }
            uint64_t elem_size = LLVMABISizeOfType(cg->di_data_layout, type);
            if (elem_size && (uint64_t)n > cg_layout_max_size(cg) / elem_size) {
                cg_layout_error(field->line, "array object size overflows for", field->as.var_decl.name);
                return Null;
            }
            type = LLVMArrayType2(type, (uint64_t)n);
        }
    }
    return type;
}

static boolean_t cg_layout_struct(cg_t *cg, const char *name) {
    struct_reg_t *sr = find_struct(cg, name);
    if (!sr) return False;
    if (sr->layout_state == 2) return True;
    if (sr->layout_state == 3) return False;
    node_t *decl = sr->layout_decl;
    if (sr->layout_state == 1 || !decl) {
        cg_layout_error(decl ? decl->line : 0, "recursive by-value or incomplete type", name);
        return False;
    }
    sr->layout_state = 1;
    sr->is_packed = (decl->as.type_decl.attr_flags & AttrPacked) != 0;
    boolean_t packed = sr->is_packed;
    boolean_t is_union = sr->is_union;
    LLVMTypeRef llvm_type = sr->llvm_type;
    usize_t fc = decl->as.type_decl.fields.count;
    heap_t types_heap = fc ? allocate(fc, sizeof(LLVMTypeRef)) : NullHeap;
    LLVMTypeRef *types = types_heap.pointer;
    usize_t count = 0;
    boolean_t ok = True;
    for (usize_t i = 0; i < fc; i++) {
        node_t *field = decl->as.type_decl.fields.items[i];
        if (field->as.var_decl.flags & VdeclComptimeField) {
            if (sr->ct_field_count < 16) {
                long value = 0;
                if (field->as.var_decl.init &&
                        !eval_comptime_expr(cg, field->as.var_decl.init, &value)) {
                    cg_layout_error(field->line, "compile-time field is not a supported constant in metadata range", field->as.var_decl.name);
                    ok = False;
                }
                /* Evaluating sizeof/generic constants may grow the registry. */
                sr = find_struct(cg, name);
                sr->ct_fields[sr->ct_field_count++] = (comptime_field_t){field->as.var_decl.name, value};
            }
            continue;
        }
        if (packed && (field->as.var_decl.flags & VdeclAtomic)) {
            cg_layout_error(field->line, "packed atomic fields are unsupported for", field->as.var_decl.name);
            ok = False;
            continue;
        }
        if (field->as.var_decl.bitfield_width > 0) {
            cg_layout_error(field->line, "bitfield access/layout is not qualified; use explicit integer masks for", field->as.var_decl.name);
            ok = False;
            continue;
        }
        type_info_t ti;
        LLVMTypeRef ft = cg_layout_field_type(cg, field, &ti);
        /* Completing a generic dependency can reallocate the registry. */
        sr = find_struct(cg, name);
        if (!ft) { ok = False; continue; }
        struct_add_field(sr, field->as.var_decl.name, ti, is_union ? 0 : count,
                         field->as.var_decl.linkage, field->as.var_decl.storage);
        field_info_t *fi = &sr->fields[sr->field_count - 1];
        fi->llvm_type = ft;
        if (field->as.var_decl.flags & VdeclArray)
            fi->array_size = field->as.var_decl.array_sizes[0];
        types[count++] = ft;
    }
    if (ok) {
        if (is_union) {
            LLVMTypeRef storage = cg_layout_storage(cg, types, count, packed);
            if (storage) LLVMStructSetBody(llvm_type, &storage, 1, packed);
            else ok = False;
        } else if (cg_layout_fits(cg, types, count, packed, decl->line))
            LLVMStructSetBody(llvm_type, types, (unsigned)count, packed);
        else ok = False;
    }
    sr = find_struct(cg, name);
    if (ok) {
        sr->size_bytes = LLVMABISizeOfType(cg->di_data_layout, llvm_type);
        sr->abi_alignment = LLVMABIAlignmentOfType(cg->di_data_layout, llvm_type);
        unsigned requested = decl->as.type_decl.align_value;
        if (requested && (requested & (requested - 1))) {
            cg_layout_error(decl->line, "alignment must be a power of two for", name);
            ok = False;
        } else if (requested > sr->abi_alignment) {
            cg_layout_error(decl->line, "type over-alignment is not supported (array stride and ABI must agree) for", name);
            ok = False;
        }
        /* Smaller/equal requests are minimum alignment assertions, already met
           by the natural representation. @packed + @align(>1) is rejected. */
    }
    if (types_heap.pointer) deallocate(types_heap);
    sr->layout_state = ok ? 2 : 3;
    /* Keep failed types sized for diagnostic recovery in lazy instantiation.
       The error count still prevents emitting any object. */
    if (!ok && LLVMIsOpaqueStruct(llvm_type)) {
        LLVMTypeRef byte = LLVMInt8TypeInContext(cg->ctx);
        LLVMStructSetBody(llvm_type, &byte, 1, 0);
    }
    return ok;
}

static boolean_t cg_layout_enum(cg_t *cg, const char *name) {
    enum_reg_t *er = find_enum(cg, name);
    if (!er) return False;
    if (er->layout_state == 2) return True;
    if (er->layout_state == 3) return False;
    node_t *decl = er->layout_decl;
    if (er->layout_state == 1) {
        cg_layout_error(decl ? decl->line : 0, "recursive by-value tagged enum", name);
        return False;
    }
    er->layout_state = 1;
    LLVMTypeRef llvm_type = er->llvm_type;
    usize_t vc = er->variant_count;
    heap_t types_heap = vc ? allocate(vc, sizeof(LLVMTypeRef)) : NullHeap;
    LLVMTypeRef *types = types_heap.pointer;
    usize_t count = 0;
    boolean_t ok = True;
    for (usize_t i = 0; i < vc; i++) {
        if (!er->variants[i].has_payload) continue;
        type_info_t ti = cg_layout_resolve_type(cg, er->variants[i].payload_type);
        if (!cg_layout_require_type(cg, ti, decl ? decl->line : 0)) { ok = False; continue; }
        er = find_enum(cg, name);
        er->variants[i].payload_type = ti;
        types[count++] = get_llvm_type(cg, ti);
    }
    if (ok) {
        LLVMTypeRef storage = cg_layout_storage(cg, types, count, False);
        if (!storage) ok = False;
        else {
            LLVMTypeRef body[2] = { LLVMInt32TypeInContext(cg->ctx), storage };
            if (!cg_layout_fits(cg, body, 2, False, decl ? decl->line : 0)) {
                ok = False;
            } else LLVMStructSetBody(llvm_type, body, 2, 0);
            er = find_enum(cg, name);
            er->payload_size = LLVMABISizeOfType(cg->di_data_layout, storage);
            er->payload_alignment = LLVMABIAlignmentOfType(cg->di_data_layout, storage);
        }
    }
    if (types_heap.pointer) deallocate(types_heap);
    er = find_enum(cg, name);
    er->layout_state = ok ? 2 : 3;
    if (!ok && LLVMIsOpaqueStruct(llvm_type)) {
        LLVMTypeRef byte = LLVMInt8TypeInContext(cg->ctx);
        LLVMStructSetBody(llvm_type, &byte, 1, 0);
    }
    return ok;
}

/* Source pointers carry no verified alignment provenance yet. LLVM's builder
 * inserts natural alignment even for a raw pointer or a packed member, which
 * is an unjustified promise. Lower every ordinary access to the guaranteed
 * byte alignment before optimization; LLVM may rediscover stronger alignment
 * from real allocas/globals/GEPs. Atomic operations retain their own contract. */
static void cg_layout_normalize_memory_alignment(cg_t *cg) {
    for (LLVMValueRef fn = LLVMGetFirstFunction(cg->module); fn;
            fn = LLVMGetNextFunction(fn))
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
                bb = LLVMGetNextBasicBlock(bb))
            for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst;
                    inst = LLVMGetNextInstruction(inst)) {
                LLVMOpcode op = LLVMGetInstructionOpcode(inst);
                if ((op == LLVMLoad || op == LLVMStore)
                        && LLVMGetOrdering(inst) == LLVMAtomicOrderingNotAtomic)
                    LLVMSetAlignment(inst, 1);
            }
}

/* Shared size/alignment/offset query surface for constant evaluation and future
 * alignof/offsetof syntax. A field name selects a direct member offset; union
 * members always begin at zero. Size includes tail padding and equals stride. */
static boolean_t cg_layout_query(cg_t *cg, type_info_t ti, const char *field,
                                 usize_t line, uint64_t *size,
                                 unsigned *alignment, uint64_t *offset) {
    ti = cg_layout_resolve_type(cg, ti);
    if (!cg_layout_require_type(cg, ti, line)) return False;
    LLVMTypeRef type = get_llvm_type(cg, ti);
    if (!LLVMTypeIsSized(type)) return False;
    if (size) *size = LLVMABISizeOfType(cg->di_data_layout, type);
    if (alignment) *alignment = LLVMABIAlignmentOfType(cg->di_data_layout, type);
    if (offset) *offset = 0;
    if (!field) return True;
    struct_reg_t *sr = !ti.is_pointer && ti.base == TypeUser && ti.user_name
        ? find_struct(cg, ti.user_name) : Null;
    if (sr) for (usize_t i = 0; i < sr->field_count; i++) {
        field_info_t *fi = &sr->fields[i];
        if (strcmp(fi->name, field) == 0) {
            if (offset && !sr->is_union)
                *offset = LLVMOffsetOfElement(cg->di_data_layout, type, (unsigned)fi->index);
            return True;
        }
    }
    cg_layout_error(line, "unknown member in layout query", field);
    return False;
}
