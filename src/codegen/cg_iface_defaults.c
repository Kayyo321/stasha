/* Ordinary compiler-owned default interface leaves. This is a physical write
 * plan, not source-history, definite-assignment, ownership or lifetime proof.
 * No type creation or registry mutation is allowed during discovery/emission.
 * Effect-qualified edges stop before memo lookup: all memoized nodes describe
 * only ordinary storage. Plans live for one declaration and are fully disposed. */
enum { IfaceDefaultMaxNodes = 4096, IfaceDefaultMaxDepth = 128,
       IfaceDefaultMaxEmission = 4096 };
typedef enum { IfaceDefaultNone, IfaceDefaultZero, IfaceDefaultStruct,
               IfaceDefaultArray } iface_default_kind_t;
typedef struct iface_default_node iface_default_node_t;
typedef struct {
    unsigned index;
    iface_default_node_t *child;
} iface_default_edge_t;
struct iface_default_node {
    LLVMTypeRef type;
    iface_default_kind_t kind;
    boolean_t complete;
    uint64_t extent;
    iface_default_node_t *element, *next;
    iface_default_edge_t *edges;
    usize_t count;
    usize_t emission_work;
    heap_t self_heap, edges_heap;
};
typedef struct {
    cg_t *cg;
    node_t *declaration;
    iface_default_node_t *nodes;
    iface_default_node_t empty;
    usize_t count;
    boolean_t failed;
} iface_default_plan_t;

static void iface_default_error(iface_default_plan_t *plan, const char *message) {
    if (plan->failed) return;
    plan->failed = True;
    const char *saved_file = diag_get_file();
    if (plan->declaration->source_file) diag_set_file(plan->declaration->source_file);
    diag_begin_error("cannot establish interface default layout: %s", message);
    diag_span(DIAG_NODE(plan->declaration), True, "default storage declared here");
    diag_finish();
    diag_set_file(saved_file);
}

/* Instruction-free overapproximation before qualifier/layout planning. A wide
 * or deep descriptor-free object must not inherit interface planning limits
 * just because the module declares an unrelated I. Walk distinct physical
 * types iteratively, stopping at explicit carriers and all pointer leaves.
 * This negative precheck grows with the actual finite representation graph;
 * selected-plan quotas must not reject a descriptor-free source type.
 * A positive answer is not admission: the checked ordinary plan below is still
 * required. Exhaustion is unknown/error, never a cached negative answer. */
static boolean_t iface_default_possible(iface_default_plan_t *plan, LLVMTypeRef root) {
    usize_t capacity = 32;
    heap_t h = allocate(capacity, sizeof(LLVMTypeRef));
    LLVMTypeRef *types = h.pointer;
    usize_t count = 1;
    types[0] = root;
    boolean_t found = False;
    for (usize_t at = 0; at < count && !plan->failed && !found; ++at) {
        LLVMTypeRef type = types[at];
        for (usize_t i = 0; i < plan->cg->iface_count; ++i)
            if (plan->cg->interfaces[i].fat_ptr_type == type) { found = True; break; }
        if (found) break;
        LLVMTypeKind kind = LLVMGetTypeKind(type);
        unsigned children = 0;
        if (kind == LLVMArrayTypeKind) children = LLVMGetArrayLength2(type) ? 1 : 0;
        else if (kind == LLVMStructTypeKind) {
            struct_reg_t *sr = Null;
            for (usize_t i = 0; i < plan->cg->struct_count; ++i)
                if (plan->cg->structs[i].llvm_type == type) { sr = &plan->cg->structs[i]; break; }
            if (!sr || sr->is_union || sr->is_any_type) continue;
            if (LLVMIsOpaqueStruct(type)) {
                iface_default_error(plan, "incomplete source storage"); break;
            }
            children = LLVMCountStructElementTypes(type);
        }
        for (unsigned i = 0; i < children; ++i) {
            LLVMTypeRef child = kind == LLVMArrayTypeKind ? LLVMGetElementType(type) : LLVMStructGetTypeAtIndex(type, i);
            usize_t seen = 0;
            while (seen < count && types[seen] != child) ++seen;
            if (seen < count) continue;
            if (count == capacity) {
                if (capacity > SIZE_MAX / (2 * sizeof(LLVMTypeRef))) {
                    iface_default_error(plan, "type reachability size overflow"); break;
                }
                capacity *= 2;
                h = reallocate(h, capacity * sizeof(LLVMTypeRef));
                types = h.pointer;
            }
            types[count++] = child;
        }
    }
    deallocate(h);
    return found;
}

static iface_default_node_t *iface_default_discover(iface_default_plan_t *plan,
                                                   LLVMTypeRef type, unsigned depth) {
    if (plan->failed) return Null;
    for (iface_default_node_t *p = plan->nodes; p; p = p->next) {
        if (p->type != type) continue;
        if (!p->complete) iface_default_error(plan, "cyclic by-value representation");
        return p;
    }
    if (!type) {
        iface_default_error(plan, "missing storage type"); return Null;
    }
    boolean_t possible = iface_default_possible(plan, type);
    if (plan->failed) return Null;
    if (!possible) {
        plan->empty.complete = True;
        return &plan->empty;
    }
    if (depth > IfaceDefaultMaxDepth || plan->count >= IfaceDefaultMaxNodes) {
        iface_default_error(plan, "incomplete type or traversal budget exhausted");
        return Null;
    }
    heap_t h = allocate(1, sizeof(iface_default_node_t));
    iface_default_node_t *p = h.pointer;
    memset(p, 0, sizeof(*p));
    p->self_heap = h; p->type = type; p->next = plan->nodes;
    plan->nodes = p; ++plan->count;
    cg_t *cg = plan->cg;
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    /* Exact nominal identity, never a name or two-pointer structural guess. */
    for (usize_t i = 0; i < cg->iface_count; ++i)
        if (cg->interfaces[i].fat_ptr_type == type) {
            p->kind = IfaceDefaultZero;
            goto complete;
        }
    if (kind == LLVMArrayTypeKind) {
        p->extent = LLVMGetArrayLength2(type);
        /* Empty arrays have no descriptor object, address or access. */
        if (!p->extent) goto complete;
        p->element = iface_default_discover(plan, LLVMGetElementType(type), depth + 1);
        if (plan->failed || !p->element) return p;
        if (p->element->kind == IfaceDefaultZero) p->kind = IfaceDefaultZero;
        else if (p->element->kind != IfaceDefaultNone) p->kind = IfaceDefaultArray;
    } else if (kind == LLVMStructTypeKind) {
        /* Tagged payload identity stops before its physical alignment anchor. */
        for (usize_t i = 0; i < cg->enum_count; ++i)
            if (cg->enums[i].llvm_type == type) goto complete;
        struct_reg_t *sr = Null;
        for (usize_t i = 0; i < cg->struct_count; ++i)
            if (cg->structs[i].llvm_type == type) { sr = &cg->structs[i]; break; }
        /* Unregistered structures are opaque compiler carriers (slice, error,
         * closure, etc.), not admitted ordinary source structs. Never traverse
         * their backing pointer, payload or environment representation. */
        if (!sr || sr->is_union || sr->is_any_type) goto complete;
        node_t *decl = sr->layout_decl;
        if (sr->is_interface || sr->layout_state != 2 || !decl ||
                decl->kind != NodeTypeDecl || decl->as.type_decl.decl_kind != TypeDeclStruct ||
                LLVMIsOpaqueStruct(type) || LLVMCountStructElementTypes(type) != sr->field_count) {
            iface_default_error(plan, "missing completed ordinary source layout");
            return p;
        }
        if (sr->field_count) {
            p->edges_heap = allocate(sr->field_count, sizeof(iface_default_edge_t));
            p->edges = p->edges_heap.pointer;
        }
        usize_t runtime = 0;
        for (usize_t i = 0; i < decl->as.type_decl.fields.count; ++i) {
            node_t *field = decl->as.type_decl.fields.items[i];
            if (!field || field->kind != NodeVarDecl) {
                iface_default_error(plan, "invalid source field metadata"); return p;
            }
            if (field->as.var_decl.flags & VdeclComptimeField) continue;
            if (runtime >= sr->field_count) {
                iface_default_error(plan, "source/runtime field count mismatch"); return p;
            }
            field_info_t *fi = &sr->fields[runtime];
            if (!field->as.var_decl.name || !fi->name ||
                    strcmp(field->as.var_decl.name, fi->name) || fi->index != runtime ||
                    !fi->llvm_type || fi->llvm_type != LLVMStructGetTypeAtIndex(type, (unsigned)runtime) ||
                    fi->storage != field->as.var_decl.storage || fi->bit_width ||
                    field->as.var_decl.bitfield_width) {
                iface_default_error(plan, "source/runtime field correspondence mismatch"); return p;
            }
            ++runtime;
            /* Template fields preserve qualifiers for instantiated ordinary
             * structs; the completed fi type already contains substitutions
             * and every array rank. Never re-resolve raw generic T here. */
            if (field->as.var_decl.flags & (VdeclAtomic | VdeclVolatile | VdeclTls)) continue;
            iface_default_node_t *child = iface_default_discover(plan, fi->llvm_type, depth + 1);
            if (plan->failed || !child) return p;
            if (child->kind != IfaceDefaultNone) {
                p->edges[p->count++] = (iface_default_edge_t){(unsigned)fi->index, child};
                p->kind = IfaceDefaultStruct;
            }
        }
        if (runtime != sr->field_count) {
            iface_default_error(plan, "source/runtime field count mismatch"); return p;
        }
    }
complete:
    if (p->kind != IfaceDefaultNone) {
        /* Memoization bounds discovery, not expansion through a shared DAG.
         * Count actual recursive emitter work before any CFG/store is emitted.
         * Arrays add a constant loop cost, never multiply by their extent. */
        p->emission_work = p->kind == IfaceDefaultArray ? 8 : 1;
        if (p->kind == IfaceDefaultArray) {
            if (p->element->emission_work > IfaceDefaultMaxEmission - p->emission_work) {
                iface_default_error(plan, "expanded emission budget exhausted"); return p;
            }
            p->emission_work += p->element->emission_work;
        } else if (p->kind == IfaceDefaultStruct) {
            for (usize_t i = 0; i < p->count; ++i) {
                usize_t work = p->edges[i].child->emission_work;
                if (work >= IfaceDefaultMaxEmission - p->emission_work) {
                    iface_default_error(plan, "expanded emission budget exhausted"); return p;
                }
                p->emission_work += work + 1;
            }
        }
        if (!LLVMTypeIsSized(type) || !cg->di_data_layout) {
            iface_default_error(plan, "unsized selected storage"); return p;
        }
        uint64_t limit = cg_layout_max_size(cg);
        if (kind == LLVMArrayTypeKind) {
            uint64_t element_size = LLVMABISizeOfType(cg->di_data_layout, LLVMGetElementType(type));
            if (element_size && p->extent > limit / element_size) {
                iface_default_error(plan, "selected array exceeds target addressable size"); return p;
            }
        }
        if (LLVMABISizeOfType(cg->di_data_layout, type) > limit) {
            iface_default_error(plan, "selected storage exceeds target addressable size"); return p;
        }
    }
    p->complete = True;
    return p;
}

static void iface_default_emit(cg_t *cg, iface_default_node_t *p, LLVMValueRef address) {
    if (p->kind == IfaceDefaultZero) {
        LLVMValueRef store = LLVMBuildStore(cg->builder, LLVMConstNull(p->type), address);
        LLVMSetAlignment(store, 1);
    } else if (p->kind == IfaceDefaultStruct) {
        LLVMTypeRef index_type = LLVMInt32TypeInContext(cg->ctx);
        for (usize_t i = 0; i < p->count; ++i) {
            LLVMValueRef indices[2] = {LLVMConstInt(index_type, 0, 0),
                                      LLVMConstInt(index_type, p->edges[i].index, 0)};
            LLVMValueRef field = LLVMBuildGEP2(cg->builder, p->type, address, indices, 2, "iface.default.field");
            iface_default_emit(cg, p->edges[i].child, field);
        }
    } else if (p->kind == IfaceDefaultArray) {
        /* A strict unsigned i64 comparison represents every LLVM array extent.
         * The increment runs only for i < extent, so even UINT64_MAX cannot
         * wrap. Target-size validation above proves the typed object fits.
         * No inbounds or stronger alias/alignment/lifetime facts are asserted. */
        LLVMTypeRef index_type = LLVMInt64TypeInContext(cg->ctx);
        LLVMValueRef zero = LLVMConstInt(index_type, 0, 0);
        LLVMBasicBlockRef pre = LLVMGetInsertBlock(cg->builder);
        LLVMBasicBlockRef test = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "iface.default.test");
        LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "iface.default.body");
        LLVMBasicBlockRef end = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "iface.default.end");
        LLVMBuildBr(cg->builder, test);
        LLVMPositionBuilderAtEnd(cg->builder, test);
        LLVMValueRef index = LLVMBuildPhi(cg->builder, index_type, "iface.default.index");
        LLVMAddIncoming(index, &zero, &pre, 1);
        LLVMValueRef valid = LLVMBuildICmp(cg->builder, LLVMIntULT, index,
            LLVMConstInt(index_type, p->extent, 0), "iface.default.in.range");
        LLVMBuildCondBr(cg->builder, valid, body, end);
        LLVMPositionBuilderAtEnd(cg->builder, body);
        LLVMValueRef indices[2] = {zero, index};
        LLVMValueRef element = LLVMBuildGEP2(cg->builder, p->type, address, indices, 2, "iface.default.element");
        iface_default_emit(cg, p->element, element);
        LLVMValueRef next = LLVMBuildAdd(cg->builder, index, LLVMConstInt(index_type, 1, 0), "iface.default.next");
        LLVMBasicBlockRef back = LLVMGetInsertBlock(cg->builder);
        LLVMBuildBr(cg->builder, test);
        LLVMAddIncoming(index, &next, &back, 1);
        LLVMPositionBuilderAtEnd(cg->builder, end);
    }
}

static void iface_default_dispose(iface_default_plan_t *plan) {
    for (iface_default_node_t *p = plan->nodes, *next; p; p = next) {
        next = p->next;
        if (p->edges_heap.pointer) deallocate(p->edges_heap);
        deallocate(p->self_heap);
    }
    plan->nodes = Null;
}

static void iface_default_local(cg_t *cg, node_t *node, LLVMTypeRef type, LLVMValueRef address) {
    if (!cg->iface_count || node->as.var_decl.init ||
            (node->as.var_decl.flags & (VdeclAtomic | VdeclVolatile | VdeclTls))) return;
    iface_default_plan_t plan = {.cg = cg, .declaration = node};
    iface_default_node_t *root = iface_default_discover(&plan, type, 0);
    /* Discovery either succeeds entirely or emits nothing. No failed result
     * enters a long-lived cache or supplies stronger source-history facts. */
    if (!plan.failed && root) iface_default_emit(cg, root, address);
    iface_default_dispose(&plan);
}
