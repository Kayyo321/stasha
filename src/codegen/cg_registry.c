/* ── registry helpers ── */

static void register_struct(cg_t *cg, const char *name, LLVMTypeRef llvm_type,
                             boolean_t is_union) {
    if (cg->struct_count >= cg->struct_cap) {
        usize_t new_cap = cg->struct_cap < 8 ? 8 : cg->struct_cap * 2;
        if (cg->structs_heap.pointer == Null)
            cg->structs_heap = allocate(new_cap, sizeof(struct_reg_t));
        else
            cg->structs_heap = reallocate(cg->structs_heap, new_cap * sizeof(struct_reg_t));
        cg->structs = cg->structs_heap.pointer;
        cg->struct_cap = new_cap;
    }
    struct_reg_t *sr = &cg->structs[cg->struct_count++];
    sr->name = (char *)name;
    sr->mod_prefix = Null;
    sr->llvm_type = llvm_type;
    sr->fields = Null;
    sr->field_count = 0;
    sr->field_capacity = 0;
    sr->fields_heap = NullHeap;
    sr->destructor = Null;
    sr->is_union = is_union;
    sr->ct_field_count = 0;
}

static void struct_add_field_ex(struct_reg_t *sr, const char *name, type_info_t type,
                               usize_t index, linkage_t linkage, storage_t storage,
                               int bit_offset, int bit_width) {
    if (sr->field_count >= sr->field_capacity) {
        usize_t new_cap = sr->field_capacity < 8 ? 8 : sr->field_capacity * 2;
        if (sr->fields_heap.pointer == Null)
            sr->fields_heap = allocate(new_cap, sizeof(field_info_t));
        else
            sr->fields_heap = reallocate(sr->fields_heap, new_cap * sizeof(field_info_t));
        sr->fields = sr->fields_heap.pointer;
        sr->field_capacity = new_cap;
    }
    sr->fields[sr->field_count].name    = (char *)name;
    sr->fields[sr->field_count].type    = type;
    sr->fields[sr->field_count].storage = storage;
    sr->fields[sr->field_count].index = index;
    sr->fields[sr->field_count].linkage = linkage;
    sr->fields[sr->field_count].bit_offset  = bit_offset;
    sr->fields[sr->field_count].bit_width   = bit_width;
    sr->fields[sr->field_count].array_size  = 0;
    sr->field_count++;
}

static void struct_add_field(struct_reg_t *sr, const char *name, type_info_t type,
                             usize_t index, linkage_t linkage, storage_t storage) {
    struct_add_field_ex(sr, name, type, index, linkage, storage, 0, 0);
}

static void register_enum(cg_t *cg, const char *name, node_list_t *variants) {
    if (cg->enum_count >= cg->enum_cap) {
        usize_t new_cap = cg->enum_cap < 8 ? 8 : cg->enum_cap * 2;
        if (cg->enums_heap.pointer == Null)
            cg->enums_heap = allocate(new_cap, sizeof(enum_reg_t));
        else
            cg->enums_heap = reallocate(cg->enums_heap, new_cap * sizeof(enum_reg_t));
        cg->enums = cg->enums_heap.pointer;
        cg->enum_cap = new_cap;
    }
    enum_reg_t *er = &cg->enums[cg->enum_count++];
    er->name = (char *)name;
    er->is_tagged = False;
    er->variant_count = variants->count;

    if (variants->count > 0) {
        er->variants_heap = allocate(variants->count, sizeof(variant_info_t));
        er->variants = er->variants_heap.pointer;

        /* determine if any variant has a payload (tagged enum) */
        usize_t max_payload = 0;
        for (usize_t i = 0; i < variants->count; i++) {
            node_t *v = variants->items[i];
            er->variants[i].name = v->as.enum_variant.name;
            er->variants[i].value = (long)i;
            er->variants[i].has_payload = v->as.enum_variant.has_payload;
            er->variants[i].payload_type = v->as.enum_variant.payload_type;
            if (v->as.enum_variant.has_payload) {
                er->is_tagged = True;
                usize_t sz = payload_type_size(v->as.enum_variant.payload_type);
                if (sz > max_payload) max_payload = sz;
            }
        }

        if (er->is_tagged) {
            /* tagged union: { i32, [max_payload x i8] } */
            LLVMTypeRef byte_arr = LLVMArrayType2(
                LLVMInt8TypeInContext(cg->ctx), (unsigned long long)max_payload);
            LLVMTypeRef fields[2] = { LLVMInt32TypeInContext(cg->ctx), byte_arr };
            er->llvm_type = LLVMStructTypeInContext(cg->ctx, fields, 2, 0);
        } else {
            er->llvm_type = LLVMInt32TypeInContext(cg->ctx);
        }
    } else {
        er->variants_heap = NullHeap;
        er->variants = Null;
        er->llvm_type = LLVMInt32TypeInContext(cg->ctx);
    }
}

static void register_alias(cg_t *cg, const char *name, type_info_t actual) {
    if (cg->alias_count >= cg->alias_cap) {
        usize_t new_cap = cg->alias_cap < 8 ? 8 : cg->alias_cap * 2;
        if (cg->aliases_heap.pointer == Null)
            cg->aliases_heap = allocate(new_cap, sizeof(type_alias_t));
        else
            cg->aliases_heap = reallocate(cg->aliases_heap, new_cap * sizeof(type_alias_t));
        cg->aliases = cg->aliases_heap.pointer;
        cg->alias_cap = new_cap;
    }
    cg->aliases[cg->alias_count].name = (char *)name;
    cg->aliases[cg->alias_count].actual = actual;
    cg->alias_count++;
}

static void register_lib(cg_t *cg, const char *name, const char *alias,
                          const char *path) {
    if (cg->lib_count >= cg->lib_cap) {
        usize_t new_cap = cg->lib_cap < 8 ? 8 : cg->lib_cap * 2;
        if (cg->libs_heap.pointer == Null)
            cg->libs_heap = allocate(new_cap, sizeof(lib_entry_t));
        else
            cg->libs_heap = reallocate(cg->libs_heap, new_cap * sizeof(lib_entry_t));
        cg->libs = cg->libs_heap.pointer;
        cg->lib_cap = new_cap;
    }
    cg->libs[cg->lib_count].name  = (char *)name;
    cg->libs[cg->lib_count].alias = (char *)alias;
    cg->libs[cg->lib_count].path  = (char *)path;
    cg->lib_count++;
}
