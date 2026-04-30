/* ── statements ── */

/* Returns True if type_info is a primitive (non-struct, non-enum) that can be heap-allocated */
static boolean_t is_primitive_type(type_info_t ti) {
    if (ti.is_pointer || ti.base == TypeVoid) return False;
    if (ti.base == TypeUser)   return False;
    if (ti.base == TypeFnPtr)  return False; /* function pointers are not heap primitives */
    if (ti.base == TypeFuture) return False; /* future is an opaque pointer — never heap-wrap */
    if (ti.base == TypeStream) return False; /* stream is an opaque pointer — never heap-wrap */
    return True;
}

static void gen_local_var(cg_t *cg, node_t *node) {
    /* blank identifier: silently skip — the value will be discarded */
    if (node->as.var_decl.name && strcmp(node->as.var_decl.name, "_") == 0)
        return;

    /* single-variable let binding: stack let name = expr;
       Type is inferred from the initializer at codegen time. */
    if ((node->as.var_decl.flags & VdeclLet)
            && node->as.var_decl.type.base == TypeVoid
            && !node->as.var_decl.type.is_pointer
            && node->as.var_decl.init) {
        LLVMValueRef init_val = gen_expr(cg, node->as.var_decl.init);
        LLVMTypeRef  inferred = LLVMTypeOf(init_val);
        type_info_t  ti       = llvm_type_to_ti(inferred);
        /* Infer future.[T] / stream.[T] when the RHS is a thread/async
           dispatch, so later lowering can recover the carried element type. */
        node_t *init_n = node->as.var_decl.init;
        if (init_n && init_n->kind == NodeAsyncCall) {
            const char *callee = init_n->as.async_call.callee;
            node_t *fn = find_fn_decl(cg, callee);
            ti.base       = TypeFuture;
            ti.is_pointer = False;
            ti.ptr_perm   = PtrNone;
            ti.ptr_depth  = 0;
            if (fn && fn->as.fn_decl.coro_flavor == CoroStream) ti.base = TypeStream;
            if (fn && fn->as.fn_decl.return_count > 0) {
                type_info_t rt = fn->as.fn_decl.return_types[0];
                if (ti.base == TypeStream && rt.base == TypeStream && rt.elem_type) {
                    ti.elem_type = alloc_type_array(1);
                    ti.elem_type[0] = rt.elem_type[0];
                } else {
                    ti.elem_type = alloc_type_array(1);
                    ti.elem_type[0] = rt;
                }
            }
        }
        LLVMValueRef alloca_val = alloc_in_entry(cg, inferred, node->as.var_decl.name);
        LLVMBuildStore(cg->builder, init_val, alloca_val);
        {
            int sym_flags = 0;
            if (node->as.var_decl.flags & VdeclAtomic)  sym_flags |= SymAtomic;
            if (node->as.var_decl.flags & VdeclVolatile) sym_flags |= SymVolatile;
            /* zone-allocated: rem.() must be a no-op — zone owns the memory */
            if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeNewInZone)
                sym_flags |= SymZoneAlloc;
            check_shadow(cg, node->as.var_decl.name, node->line);
            symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, inferred, ti, sym_flags);
        }
        symtab_set_last_storage(&cg->locals, node->as.var_decl.storage, False);
        symtab_set_last_extra(&cg->locals, node->as.var_decl.flags & VdeclConst,
                              node->as.var_decl.flags & VdeclFinal,
                              node->as.var_decl.linkage, cg->dtor_depth, -1);
        symtab_set_last_line(&cg->locals, node->line);
        if (ti.base == TypeUser && ti.user_name && !ti.is_pointer) {
            struct_reg_t *sr = find_struct(cg, ti.user_name);
            if (sr && sr->destructor)
                add_dtor_var(cg, alloca_val, ti.user_name);
        }
        return;
    }

    type_info_t ti = resolve_alias(cg, node->as.var_decl.type);

    /* Substitute generic type parameters (T → i32, K → u64, etc.) when
     * declaring a local variable inside a generic method instantiation. */
    if (ti.base == TypeUser && ti.user_name && cg->generic_n > 0) {
        for (usize_t gi = 0; gi < cg->generic_n; gi++) {
            if (strcmp(ti.user_name, cg->generic_params[gi]) == 0) {
                type_info_t concrete = cg->generic_concs[gi];
                concrete.is_pointer = ti.is_pointer;
                concrete.ptr_perm   = ti.ptr_perm;
                concrete.ptr_depth  = ti.ptr_depth;
                memcpy(concrete.ptr_perms, ti.ptr_perms, sizeof(ti.ptr_perms));
                ti = concrete;
                break;
            }
        }
        if (ti.base == TypeUser && ti.user_name) {
            const char *subst = cg_subst_name(cg, ti.user_name);
            if (subst != ti.user_name) ti.user_name = (char *)subst;
        }
    }

    /* Catch unknown user-defined types before generating any code.
     * For generic instantiations (name contains _G_), trigger lazy instantiation first. */
    if (ti.base == TypeUser && ti.user_name && !ti.is_pointer) {
        if (!find_struct(cg, ti.user_name) && !find_enum(cg, ti.user_name)) {
            if (strncmp(ti.user_name, "any_G_", 6) == 0)
                try_instantiate_any(cg, ti.user_name);
            else if (strstr(ti.user_name, "_G_"))
                try_instantiate_generic(cg, ti.user_name);
            if (!find_struct(cg, ti.user_name) && !find_enum(cg, ti.user_name)) {
                char dedup_key[600];
                snprintf(dedup_key, sizeof(dedup_key), "undef_type:%s", ti.user_name);
                if (!cg_error_already_reported(cg, dedup_key)) {
                    diag_begin_error("unknown type '%s'", ti.user_name);
                    diag_set_category(ErrCatUndefined);
                    diag_span(DIAG_NODE(node), True, "type used here");
                    diag_note("did you forget to define or import the type?");
                    /* Levenshtein suggestion: scan registered structs and enums */
                    usize_t best_dist = 3;
                    const char *best = Null;
                    for (usize_t i = 0; i < cg->struct_count; i++) {
                        usize_t d = levenshtein(ti.user_name, cg->structs[i].name);
                        if (d < best_dist) { best_dist = d; best = cg->structs[i].name; }
                    }
                    for (usize_t i = 0; i < cg->enum_count; i++) {
                        usize_t d = levenshtein(ti.user_name, cg->enums[i].name);
                        if (d < best_dist) { best_dist = d; best = cg->enums[i].name; }
                    }
                    if (best) diag_help("did you mean '%s'?", best);
                    diag_finish();
                }
            }
        }
    }

    LLVMTypeRef type;

    if (node->as.var_decl.flags & VdeclArray) {
        LLVMTypeRef elem = get_llvm_type(cg, ti);
        int ndim = node->as.var_decl.array_ndim > 0 ? node->as.var_decl.array_ndim : 1;

        /* Resolve each dimension's size (named const or literal). */
        unsigned long long sizes[8] = {0,0,0,0,0,0,0,0};
        for (int _d = 0; _d < ndim; _d++) {
            sizes[_d] = (unsigned long long)node->as.var_decl.array_sizes[_d];
            if (node->as.var_decl.array_size_names[_d]) {
                symbol_t *sym = cg_lookup(cg, node->as.var_decl.array_size_names[_d]);
                if (sym) {
                    if (sym->const_int_val >= 0) {
                        /* local const integer: value recorded at declaration time */
                        sizes[_d] = (unsigned long long)sym->const_int_val;
                    } else {
                        /* global const: may have an LLVM initializer */
                        LLVMValueRef cinit = LLVMGetInitializer(sym->value);
                        if (cinit)
                            sizes[_d] = LLVMConstIntGetZExtValue(cinit);
                    }
                } else {
                    diag_begin_error("undefined constant '%s' used as array size",
                                     node->as.var_decl.array_size_names[_d]);
                    diag_span(DIAG_NODE(node), True, "array declared here");
                    diag_note("array size must be a compile-time constant declared before this point");
                    diag_finish();
                }
            }
        }

        /* Infer outermost dimension from compound initializer when 0. */
        if (sizes[0] == 0 && node->as.var_decl.init
                && node->as.var_decl.init->kind == NodeCompoundInit) {
            boolean_t needs_trailing_nul = False;
            boolean_t inferred_ok = True;
            long inferred_len = count_compound_init_values(cg, node->as.var_decl.init,
                                                           &needs_trailing_nul, &inferred_ok);
            if (inferred_ok) {
                if (ndim == 1 && elem == LLVMInt8TypeInContext(cg->ctx) && needs_trailing_nul)
                    inferred_len += 1;
                if (inferred_len > 0) {
                    sizes[0] = (unsigned long long)inferred_len;
                    node->as.var_decl.array_sizes[0] = inferred_len;
                }
            }
        }

        /* Build nested LLVM array type from innermost dimension outward:
           i32 arr[4][8] → [4 x [8 x i32]]  (sizes[0]=4, sizes[1]=8) */
        type = elem;
        for (int _d = ndim - 1; _d >= 0; _d--)
            type = LLVMArrayType2(type, sizes[_d]);
    } else {
        type = get_llvm_type(cg, ti);
    }

    /* ── New allocation rule:
       - Non-pointer variables are always stack-allocated (StorageDefault).
         Using 'stack' or 'heap' on a non-pointer is a semantic error.
       - Pointer variables must specify 'stack' or 'heap'.
         Omitting the qualifier on a pointer is a semantic error.
       Both checks are skipped for 'let' bindings whose types are inferred
       at code-gen time (the inferred type is checked after inference).
    ── */
    boolean_t is_let_binding = (node->as.var_decl.flags & VdeclLet) != 0
                             || ti.base == TypeVoid; /* multi-assign target: no type yet */
    if (!is_let_binding) {
        if (node->as.var_decl.storage == StorageHeap && !ti.is_pointer
                && ti.base != TypeFnPtr && ti.base != TypeSlice) {
            const char *qual = "heap";
            diag_begin_error("'%s' qualifier is not allowed on non-pointer variable '%s'",
                             qual, node->as.var_decl.name ? node->as.var_decl.name : "?");
            diag_span(DIAG_NODE(node), True, "declared here");
            diag_note("non-pointer variables may be declared without 'stack', but cannot use 'heap'");
            diag_help("remove '%s': write '%s %s;' instead",
                      qual,
                      node->as.var_decl.type.user_name ? node->as.var_decl.type.user_name : "T",
                      node->as.var_decl.name ? node->as.var_decl.name : "x");
            diag_finish();
        }
        if (node->as.var_decl.storage == StorageDefault && ti.is_pointer) {
            diag_begin_error("pointer variable '%s' must specify 'stack' or 'heap'",
                             node->as.var_decl.name ? node->as.var_decl.name : "?");
            diag_span(DIAG_NODE(node), True, "pointer declared here");
            diag_note("pointer variables require an explicit allocation domain");
            diag_help("write 'stack T* name;' (pointer on stack) or 'heap T* name;' (pointer on heap)");
            diag_finish();
        }
    }

    /* emit alloca in the entry block so loop variables get a fixed slot */
    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBasicBlockRef entry_bb = LLVMGetEntryBasicBlock(cg->current_fn);
    LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_bb);
    if (first_instr)
        LLVMPositionBuilderBefore(cg->builder, first_instr);
    else
        LLVMPositionBuilderAtEnd(cg->builder, entry_bb);
    LLVMValueRef alloca_val = LLVMBuildAlloca(cg->builder, type, node->as.var_decl.name);
    LLVMPositionBuilderAtEnd(cg->builder, saved_bb);

    /* cross-domain check: heap/stack pointer domain vs initialiser provenance */
    if (node->as.var_decl.init && ti.is_pointer
            && (node->as.var_decl.storage == StorageStack
                || node->as.var_decl.storage == StorageHeap)) {
        int ak = rhs_addr_kind(cg, node->as.var_decl.init);
        if (ak != 0)
            check_storage_domain(cg, node->as.var_decl.storage, ak == 1, ak == -1, node->line);
    }

    /* slice-LHS domain check at declaration */
    if (node->as.var_decl.init && ti.base == TypeSlice)
        check_slice_domain(cg, node->as.var_decl.storage, node->as.var_decl.init, node->line);

    if (node->as.var_decl.init) {
        LLVMValueRef init;
        /* nil → error type produces a nil error struct */
        if (node->as.var_decl.init->kind == NodeNilExpr && ti.base == TypeError) {
            init = make_nil_error(cg);
        } else if (node->as.var_decl.init->kind == NodeNilExpr && ti.base == TypeSlice) {
            /* nil → zero-initialised slice { null, 0, 0 } */
            init = LLVMConstNull(type);
        } else {
            type_info_t saved_slice_elem = cg->hint_slice_elem;
            storage_t   saved_storage    = cg->hint_storage;
            int         saved_var_flags  = cg->hint_var_flags;
            if (ti.base == TypeSlice && ti.elem_type) {
                cg->hint_slice_elem = ti.elem_type[0];
                cg->hint_storage    = node->as.var_decl.storage;
                cg->hint_var_flags  = node->as.var_decl.flags;
            }
            cg->hint_ret_type = type;
            init = gen_expr(cg, node->as.var_decl.init);
            cg->hint_ret_type   = Null;
            cg->hint_slice_elem = saved_slice_elem;
            cg->hint_storage    = saved_storage;
            cg->hint_var_flags  = saved_var_flags;
            if (!(node->as.var_decl.flags & VdeclArray))
                init = coerce_int(cg, init, type);
        }
        LLVMBuildStore(cg->builder, init, alloca_val);
    }

    {
        int sym_flags = 0;
        if (node->as.var_decl.flags & VdeclAtomic)   sym_flags |= SymAtomic;
        if (node->as.var_decl.flags & VdeclVolatile)  sym_flags |= SymVolatile;
        /* zone-allocated: rem.() must be a no-op — zone owns the memory */
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeNewInZone)
            sym_flags |= SymZoneAlloc;
        check_shadow(cg, node->as.var_decl.name, node->line);
        symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, type, ti, sym_flags);
    }
    symtab_set_last_storage(&cg->locals, node->as.var_decl.storage, False);
    {
        long arr_sz = (node->as.var_decl.flags & VdeclArray) ? node->as.var_decl.array_sizes[0] : -1;
        symtab_set_last_extra(&cg->locals, node->as.var_decl.flags & VdeclConst,
                              node->as.var_decl.flags & VdeclFinal, node->as.var_decl.linkage,
                              cg->dtor_depth, arr_sz);
        symtab_set_last_line(&cg->locals, node->line);
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeNilExpr)
            symtab_set_last_nil(&cg->locals, True);
        /* Record compile-time value for const integer locals so array dimension
           lookup does not need to inspect LLVM IR use-chains. */
        if ((node->as.var_decl.flags & VdeclConst) && node->as.var_decl.init
                && node->as.var_decl.init->kind == NodeIntLitExpr
                && cg->locals.count > 0)
            cg->locals.entries[cg->locals.count - 1].const_int_val =
                (long long)node->as.var_decl.init->as.int_lit.value;
    }

    /* pointer safety checks on initializer */
    if (node->as.var_decl.init) {
        check_const_addr_of(cg, node->as.var_decl.init, ti, node->line);
        check_permission_widening(cg, node->as.var_decl.init, ti, node->line);
        check_pointer_lifetime(cg, node->as.var_decl.init, cg->dtor_depth, node->line);
    }

    /* ── DI: local variable declaration ── */
    if (cg->debug_mode && cg->di_builder && cg->di_scope && node->line > 0) {
        LLVMMetadataRef di_vty = get_di_type(cg, ti);
        LLVMMetadataRef di_var = LLVMDIBuilderCreateAutoVariable(
            cg->di_builder, cg->di_scope,
            node->as.var_decl.name, strlen(node->as.var_decl.name),
            cg->di_file, (unsigned)node->line,
            di_vty, /* alwaysPreserve= */ 0, LLVMDIFlagZero, /* alignInBits= */ 0);
        LLVMMetadataRef di_expr =
            LLVMDIBuilderCreateExpression(cg->di_builder, Null, 0);
        LLVMMetadataRef di_loc = di_make_location(cg, node->line);
        LLVMDIBuilderInsertDeclareRecordAtEnd(
            cg->di_builder, alloca_val, di_var, di_expr, di_loc,
            LLVMGetInsertBlock(cg->builder));
    }

    /* track struct variables for destructor auto-call */
    if (ti.base == TypeUser && ti.user_name && !ti.is_pointer) {
        struct_reg_t *sr = find_struct(cg, ti.user_name);
        if (sr && sr->destructor)
            add_dtor_var(cg, alloca_val, ti.user_name);
    }

}

static void gen_for(cg_t *cg, node_t *node) {
    push_dtor_scope(cg);

    if (node->as.for_stmt.init) {
        if (node->as.for_stmt.init->kind == NodeVarDecl)
            gen_local_var(cg, node->as.for_stmt.init);
        else
            gen_expr(cg, node->as.for_stmt.init);
    }

    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.body");
    LLVMBasicBlockRef inc_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.inc");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = inc_bb;

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = gen_expr(cg, node->as.for_stmt.cond);
    cond = llvm_to_bool(cg, cond);
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.for_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, inc_bb);

    LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
    gen_expr(cg, node->as.for_stmt.update);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;

    /* don't emit destructors here — pop without emitting */
    if (cg->dtor_depth > 0) {
        dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
        if (scope->heap.pointer != Null) deallocate(scope->heap);
        if (scope->deferred_heap.pointer != Null) deallocate(scope->deferred_heap);
        cg->dtor_depth--;
    }
}

static void gen_foreach(cg_t *cg, node_t *node) {
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);

    /* Determine element LLVM type and stype from the slice expression. */
    LLVMTypeRef elem_ty = LLVMInt8TypeInContext(cg->ctx);
    type_info_t iter_ti = NO_TYPE;
    if (node->as.foreach_stmt.slice->kind == NodeIdentExpr) {
        symbol_t *sl_sym = cg_lookup(cg, node->as.foreach_stmt.slice->as.ident.name);
        if (sl_sym && sl_sym->stype.base == TypeSlice && sl_sym->stype.elem_type) {
            iter_ti  = sl_sym->stype.elem_type[0];
            elem_ty  = get_slice_elem_llvm_type(cg, iter_ti);
        }
    }

    /* Evaluate the slice expression once before the loop. */
    LLVMValueRef slice_val = gen_expr(cg, node->as.foreach_stmt.slice);
    if (LLVMGetTypeKind(LLVMTypeOf(slice_val)) != LLVMStructTypeKind) {
        diag_begin_error("'foreach' requires a slice operand");
        diag_span(DIAG_NODE(node), True, "expression is not a slice type");
        diag_note("foreach only works on []T slice values");
        diag_finish();
        return;
    }

    LLVMValueRef sl_ptr = LLVMBuildExtractValue(cg->builder, slice_val, 0, "fe.ptr");
    LLVMValueRef sl_len = LLVMBuildExtractValue(cg->builder, slice_val, 1, "fe.len");
    sl_len = coerce_int(cg, sl_len, i64_ty);

    /* Alloca for the loop index. */
    LLVMValueRef idx_alloca = alloc_in_entry(cg, i64_ty, "fe.idx");
    LLVMBuildStore(cg->builder, LLVMConstInt(i64_ty, 0, 0), idx_alloca);

    /* Alloca for the iteration variable — visible inside the body. */
    usize_t locals_before = cg->locals.count;
    LLVMValueRef iter_alloca = alloc_in_entry(cg, elem_ty, node->as.foreach_stmt.iter_name);
    if (iter_ti.base == TypeVoid && !iter_ti.is_pointer)
        iter_ti = llvm_type_to_ti(elem_ty);
    symtab_add(&cg->locals, node->as.foreach_stmt.iter_name,
               iter_alloca, elem_ty, iter_ti, 0);
    symtab_set_last_storage(&cg->locals, StorageStack, False);
    /* Mark used immediately so unused-variable warning is never emitted for
     * the implicit iterator — the user always reads it inside the body. */
    cg->locals.entries[cg->locals.count - 1].used = True;

    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "fe.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "fe.body");
    LLVMBasicBlockRef inc_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "fe.inc");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "fe.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target    = end_bb;
    cg->continue_target = inc_bb;

    LLVMBuildBr(cg->builder, cond_bb);

    /* Condition: idx < len */
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef idx = LLVMBuildLoad2(cg->builder, i64_ty, idx_alloca, "fe.i");
    LLVMValueRef cmp = LLVMBuildICmp(cg->builder, LLVMIntSLT, idx, sl_len, "fe.ok");
    LLVMBuildCondBr(cg->builder, cmp, body_bb, end_bb);

    /* Body: bind iter_name = slice[idx], then run block */
    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    LLVMValueRef gep  = LLVMBuildGEP2(cg->builder, elem_ty, sl_ptr, &idx, 1, "fe.gep");
    LLVMValueRef elem = LLVMBuildLoad2(cg->builder, elem_ty, gep, "fe.elem");
    LLVMBuildStore(cg->builder, elem, iter_alloca);
    gen_block(cg, node->as.foreach_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, inc_bb);

    /* Increment: idx++ */
    LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
    LLVMValueRef idx2 = LLVMBuildLoad2(cg->builder, i64_ty, idx_alloca, "fe.i2");
    LLVMValueRef nxt  = LLVMBuildAdd(cg->builder, idx2, LLVMConstInt(i64_ty, 1, 0), "fe.nxt");
    LLVMBuildStore(cg->builder, nxt, idx_alloca);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target    = saved_break;
    cg->continue_target = saved_cont;

    /* Remove the iteration variable from locals. */
    cg->locals.count = locals_before;
}

static void gen_while(cg_t *cg, node_t *node) {
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "while.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "while.body");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "while.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = cond_bb;

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = gen_expr(cg, node->as.while_stmt.cond);
    cond = llvm_to_bool(cg, cond);
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.while_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;
}

static void gen_do_while(cg_t *cg, node_t *node) {
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "do.body");
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "do.cond");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "do.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = cond_bb;

    LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.do_while_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = gen_expr(cg, node->as.do_while_stmt.cond);
    cond = llvm_to_bool(cg, cond);
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;
}

static void gen_inf_loop(cg_t *cg, node_t *node) {
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "inf.body");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "inf.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = body_bb;

    LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.inf_loop.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;
}

static void gen_if(cg_t *cg, node_t *node) {
    LLVMValueRef cond = gen_expr(cg, node->as.if_stmt.cond);
    cond = llvm_to_bool(cg, cond);

    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "if.then");
    LLVMBasicBlockRef else_bb = node->as.if_stmt.else_block
        ? LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "if.else") : Null;
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "if.end");

    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb ? else_bb : merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    if (node->as.if_stmt.then_block->kind == NodeBlock)
        gen_block(cg, node->as.if_stmt.then_block);
    else
        gen_stmt(cg, node->as.if_stmt.then_block);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, merge_bb);

    if (else_bb) {
        LLVMPositionBuilderAtEnd(cg->builder, else_bb);
        node_t *eb = node->as.if_stmt.else_block;
        if (eb->kind == NodeBlock)
            gen_block(cg, eb);
        else if (eb->kind == NodeIfStmt)
            gen_if(cg, eb);
        else
            gen_stmt(cg, eb);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, merge_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}

static void gen_ret(cg_t *cg, node_t *node) {
    /* Coroutine ret:
         - Stream coroutine: `ret;` only (analysis rejects `ret expr;`); jump to
           final-suspend, which sets eos/complete + runs coro.end.
         - Task coroutine: `ret expr;` stores the result into the promise's
           item slot, sets complete=1, then jumps to final-suspend.  `ret;`
           on a void-returning task just sets complete=1 and finalizes. */
    if (cg->cur_coro.active) {
        emit_all_dtor_calls(cg);
        if (cg->current_fn && cg->current_fn_is_async_task) {
            if (node->as.ret_stmt.values.count == 0) {
                sts_emit_task_void_ret(cg, &cg->cur_coro);
            } else {
                LLVMValueRef rv = gen_expr(cg, node->as.ret_stmt.values.items[0]);
                sts_emit_task_ret(cg, &cg->cur_coro, rv);
            }
        } else {
            sts_emit_stream_ret(cg, &cg->cur_coro);
        }
        return;
    }

    /* pointer safety: check each return value */
    for (usize_t i = 0; i < node->as.ret_stmt.values.count; i++) {
        node_t *rv = node->as.ret_stmt.values.items[i];
        check_stack_escape(cg, rv, node->line);
        check_ext_returns_int_ptr(cg, rv, cg->current_fn_linkage, node->line);
    }

    /* If returning a named struct variable, move it out of dtor scopes so
       the destructor isn't called — the caller takes ownership. */
    if (node->as.ret_stmt.values.count == 1) {
        node_t *rv = node->as.ret_stmt.values.items[0];
        if (rv->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, rv->as.ident.name);
            if (sym && sym->stype.base == TypeUser && !sym->stype.is_pointer)
                remove_from_dtor_scopes(cg, sym->value);
        }
    }

    /* call destructors for all active scopes before returning */
    emit_all_dtor_calls(cg);

    if (node->as.ret_stmt.values.count == 0) {
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        if (cg->current_fn_is_entry_main)
            LLVMBuildRet(cg->builder, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0));
        else if (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind)
            LLVMBuildRetVoid(cg->builder);
        else
            LLVMBuildRet(cg->builder, LLVMConstNull(ret_type));
    } else if (node->as.ret_stmt.values.count == 1) {
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        LLVMValueRef val;
        /* nil returning as error → {i1=0, ptr=null} */
        if (node->as.ret_stmt.values.items[0]->kind == NodeNilExpr
            && ret_type == cg->error_type) {
            val = make_nil_error(cg);
        } else {
            cg->hint_ret_type = ret_type;
            val = gen_expr(cg, node->as.ret_stmt.values.items[0]);
            cg->hint_ret_type = Null;
            val = coerce_int(cg, val, ret_type);
        }
        LLVMBuildRet(cg->builder, val);
    } else {
        /* multi-return: pack into struct */
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        LLVMValueRef agg = LLVMGetUndef(ret_type);
        for (usize_t i = 0; i < node->as.ret_stmt.values.count; i++) {
            LLVMTypeRef field_type = LLVMStructGetTypeAtIndex(ret_type, (unsigned)i);
            LLVMValueRef val;
            /* nil in an error slot → {i1=0, ptr=null} */
            if (node->as.ret_stmt.values.items[i]->kind == NodeNilExpr
                && field_type == cg->error_type) {
                val = make_nil_error(cg);
            } else {
                cg->hint_ret_type = field_type;
                val = gen_expr(cg, node->as.ret_stmt.values.items[i]);
                cg->hint_ret_type = Null;
                val = coerce_int(cg, val, field_type);
            }
            agg = LLVMBuildInsertValue(cg->builder, agg, val, (unsigned)i, "");
        }
        LLVMBuildRet(cg->builder, agg);
    }
}

/* ── print.(fmt, args...) helpers ── */

/* Lazily emit (or return the existing) __sts_print_bin(i64) → void.
   The function writes the minimal binary representation of its argument
   to a local stack buffer, then calls printf("%s", buf).

   IR sketch:
     entry:  alloca [65 x i8] buf
             msb = 63 - ctlz(val | 1)      ; at least bit 0
             goto loop(msb)
     loop(bit):
             digit = ((val >> bit) & 1) + '0'
             buf[msb - bit] = digit
             if bit == 0 → exit
             goto loop(bit - 1)
     exit:   buf[msb + 1] = '\0'
             printf("%s", buf)
             ret void                                                     */
static LLVMValueRef ensure_print_binary_fn(cg_t *cg, boolean_t to_stderr) {
    const char *fname = to_stderr ? "__sts_eprint_bin" : "__sts_print_bin";
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, fname);
    if (fn) return fn;

    LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef i32_t  = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i8_t   = LLVMInt8TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);

    LLVMTypeRef fn_type = LLVMFunctionType(void_t, &i64_t, 1, 0);
    fn = LLVMAddFunction(cg->module, fname, fn_type);
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    LLVMBuilderRef b = LLVMCreateBuilderInContext(cg->ctx);

    LLVMBasicBlockRef bb_entry = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
    LLVMBasicBlockRef bb_loop  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "loop");
    LLVMBasicBlockRef bb_exit  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "exit");

    LLVMValueRef val = LLVMGetParam(fn, 0);

    /* ── entry ── */
    LLVMPositionBuilderAtEnd(b, bb_entry);

    /* buf[65] on the stack */
    LLVMTypeRef buf_ty = LLVMArrayType(i8_t, 65);
    LLVMValueRef buf   = LLVMBuildAlloca(b, buf_ty, "binbuf");

    /* msb = 63 - ctlz(val | 1, is_zero_undef=false) via llvm.ctlz.i64 */
    LLVMTypeRef ctlz_param_types[2] = { i64_t, LLVMInt1TypeInContext(cg->ctx) };
    LLVMTypeRef ctlz_fn_type = LLVMFunctionType(i64_t, ctlz_param_types, 2, 0);
    LLVMValueRef ctlz_fn = LLVMGetNamedFunction(cg->module, "llvm.ctlz.i64");
    if (!ctlz_fn) ctlz_fn = LLVMAddFunction(cg->module, "llvm.ctlz.i64", ctlz_fn_type);

    LLVMValueRef val_or1 = LLVMBuildOr(b, val,
                               LLVMConstInt(i64_t, 1, 0), "or1");
    LLVMValueRef ctlz_args[2] = { val_or1,
                                   LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0) };
    LLVMValueRef lz   = LLVMBuildCall2(b, ctlz_fn_type, ctlz_fn, ctlz_args, 2, "lz");
    LLVMValueRef msb  = LLVMBuildSub(b, LLVMConstInt(i64_t, 63, 0), lz, "msb");

    LLVMBuildBr(b, bb_loop);

    /* ── loop(bit) ── */
    LLVMPositionBuilderAtEnd(b, bb_loop);
    LLVMValueRef bit_phi = LLVMBuildPhi(b, i64_t, "bit");
    LLVMAddIncoming(bit_phi, &msb, &bb_entry, 1);

    /* digit = ((val >> bit) & 1) + '0' */
    LLVMValueRef shifted = LLVMBuildLShr(b, val, bit_phi, "sh");
    LLVMValueRef masked  = LLVMBuildAnd(b, shifted, LLVMConstInt(i64_t, 1, 0), "bit1");
    LLVMValueRef digit64 = LLVMBuildAdd(b, masked,
                               LLVMConstInt(i64_t, (unsigned long long)'0', 0), "dig64");
    LLVMValueRef digit   = LLVMBuildTrunc(b, digit64, i8_t, "dig");

    /* buf[msb - bit] = digit */
    LLVMValueRef idx64 = LLVMBuildSub(b, msb, bit_phi, "idx64");
    LLVMValueRef idx32 = LLVMBuildTrunc(b, idx64, i32_t, "idx32");
    LLVMValueRef zero32 = LLVMConstInt(i32_t, 0, 0);
    LLVMValueRef gep_idx[2] = { zero32, idx32 };
    LLVMValueRef slot = LLVMBuildGEP2(b, buf_ty, buf, gep_idx, 2, "slot");
    LLVMBuildStore(b, digit, slot);

    /* if (bit == 0) goto exit; else goto loop(bit - 1) */
    LLVMValueRef is_zero = LLVMBuildICmp(b, LLVMIntEQ, bit_phi,
                                          LLVMConstInt(i64_t, 0, 0), "done");
    LLVMValueRef bit_dec = LLVMBuildSub(b, bit_phi, LLVMConstInt(i64_t, 1, 0), "dec");
    LLVMAddIncoming(bit_phi, &bit_dec, &bb_loop, 1);
    LLVMBuildCondBr(b, is_zero, bb_exit, bb_loop);

    /* ── exit ── */
    LLVMPositionBuilderAtEnd(b, bb_exit);

    /* buf[msb + 1] = '\0' */
    LLVMValueRef null_idx64 = LLVMBuildAdd(b, msb, LLVMConstInt(i64_t, 1, 0), "nidx64");
    LLVMValueRef null_idx32 = LLVMBuildTrunc(b, null_idx64, i32_t, "nidx32");
    LLVMValueRef null_gep_idx[2] = { zero32, null_idx32 };
    LLVMValueRef null_slot = LLVMBuildGEP2(b, buf_ty, buf, null_gep_idx, 2, "nslot");
    LLVMBuildStore(b, LLVMConstInt(i8_t, 0, 0), null_slot);

    /* printf/fprintf("%s", buf_ptr) */
    LLVMValueRef first_idx[2] = { zero32, zero32 };
    LLVMValueRef buf_ptr = LLVMBuildGEP2(b, buf_ty, buf, first_idx, 2, "bufptr");
    LLVMValueRef pfmt = LLVMBuildGlobalStringPtr(b, "%s", "binfmt");
    if (to_stderr) {
        LLVMTypeRef ptr_t2 = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef stderr_val = LLVMBuildLoad2(b, ptr_t2, cg->stderr_var, "stderr");
        LLVMValueRef pargs[3] = { stderr_val, pfmt, buf_ptr };
        LLVMBuildCall2(b, cg->fprintf_type, cg->fprintf_fn, pargs, 3, "");
    } else {
        LLVMValueRef pargs[2] = { pfmt, buf_ptr };
        LLVMBuildCall2(b, cg->printf_type, cg->printf_fn, pargs, 2, "");
    }

    LLVMBuildRetVoid(b);

    LLVMDisposeBuilder(b);
    return fn;
}

static void print_emit_cstr(cg_t *cg, const char *s, boolean_t to_stderr) {
    LLVMValueRef str = LLVMBuildGlobalStringPtr(cg->builder, s, "plit");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, "%s", "pfmt_s");
    if (to_stderr) {
        LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef stderr_val = LLVMBuildLoad2(cg->builder, ptr_t, cg->stderr_var, "stderr");
        LLVMValueRef pargs[3] = { stderr_val, fmt, str };
        LLVMBuildCall2(cg->builder, cg->fprintf_type, cg->fprintf_fn, pargs, 3, "");
    } else {
        LLVMValueRef pargs[2] = { fmt, str };
        LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, pargs, 2, "");
    }
}

static void print_emit_scalar(cg_t *cg, LLVMValueRef val, LLVMTypeRef ty, boolean_t to_stderr) {
    const char *fmt_str;
    LLVMValueRef print_val = val;

    if (llvm_is_ptr(ty)) {
        fmt_str = "%s";
    } else if (llvm_is_float(ty)) {
        if (ty == LLVMFloatTypeInContext(cg->ctx))
            print_val = LLVMBuildFPExt(cg->builder, val,
                                       LLVMDoubleTypeInContext(cg->ctx), "fpext");
        fmt_str = "%g";
    } else if (ty == LLVMInt64TypeInContext(cg->ctx)) {
        fmt_str = "%lld";
    } else if (ty == LLVMInt8TypeInContext(cg->ctx)) {
        fmt_str = "%c";
        print_val = LLVMBuildSExt(cg->builder, val,
                                   LLVMInt32TypeInContext(cg->ctx), "cext");
    } else if (ty == LLVMInt1TypeInContext(cg->ctx)) {
        fmt_str = "%d";
        print_val = LLVMBuildZExt(cg->builder, val,
                                   LLVMInt32TypeInContext(cg->ctx), "bext");
    } else {
        fmt_str = "%d";
        if (ty != LLVMInt32TypeInContext(cg->ctx))
            print_val = LLVMBuildSExt(cg->builder, val,
                                       LLVMInt32TypeInContext(cg->ctx), "iext");
    }

    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "pfmt");
    if (to_stderr) {
        LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef stderr_val = LLVMBuildLoad2(cg->builder, ptr_t, cg->stderr_var, "stderr");
        LLVMValueRef pargs[3] = { stderr_val, fmt, print_val };
        LLVMBuildCall2(cg->builder, cg->fprintf_type, cg->fprintf_fn, pargs, 3, "");
    } else {
        LLVMValueRef pargs[2] = { fmt, print_val };
        LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, pargs, 2, "");
    }
}

/* emit a value with a format spec (the part after ':' inside {}).
   Supported specs:
     x / X       lowercase / uppercase hex
     b           binary (C23 / clang %b extension)
     o           octal
     .N          float with N decimal places  (e.g. .2)
     <N         left-align in field of width N     (e.g. <8)
     +          force sign prefix for positive nums
     #x / #X   hex with 0x / 0X prefix
     #b         binary with 0b prefix
     #o         octal with 0 prefix (via printf %#llo)
     N          minimum field width, right-aligned  (e.g. 8)
     0N         zero-padded minimum width            (e.g. 08)
     .N         float with N decimal places          (e.g. .2)
   Flags and modifiers compose: {:+#08x}, {:<+8}, etc.
   Anything unrecognised falls back to the auto format. */

/* Parsed representation of a format spec (the part after ':'). */
typedef struct {
    boolean_t left_align;   /* '<' flag                          */
    boolean_t force_sign;   /* '+' flag                          */
    boolean_t alt_form;     /* '#' flag (0x/0X/0b/0 prefixes)   */
    boolean_t zero_pad;     /* '0' flag                          */
    usize_t   width;        /* field width; 0 = unset            */
    usize_t   prec;         /* float precision; (usize_t)-1 = unset */
    char      base;         /* 'x','X','b','o','d'; 0 = auto     */
} print_spec_t;

static print_spec_t parse_print_spec(const char *s, usize_t len) {
    print_spec_t sp = { False, False, False, False, 0, (usize_t)-1, 0 };
    usize_t i = 0;

    /* flags: any of + < # 0 in any order before width/precision/base.
       '0' is always the zero-pad flag here; width digits come after. */
    while (i < len) {
        if      (s[i] == '<') { sp.left_align = True; i++; }
        else if (s[i] == '+') { sp.force_sign = True; i++; }
        else if (s[i] == '#') { sp.alt_form   = True; i++; }
        else if (s[i] == '0') { sp.zero_pad   = True; i++; }
        else break;
    }

    /* width */
    while (i < len && s[i] >= '0' && s[i] <= '9')
        sp.width = sp.width * 10 + (usize_t)(s[i++] - '0');

    /* precision */
    if (i < len && s[i] == '.') {
        i++;
        sp.prec = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9')
            sp.prec = sp.prec * 10 + (usize_t)(s[i++] - '0');
    }

    /* base specifier */
    if (i < len && (s[i]=='x'||s[i]=='X'||s[i]=='b'||s[i]=='o'||s[i]=='d'))
        sp.base = s[i];

    return sp;
}

static void print_emit_scalar_spec(cg_t *cg, LLVMValueRef val, LLVMTypeRef ty,
                                   const char *spec, usize_t slen, boolean_t to_stderr) {
    if (slen == 0) { print_emit_scalar(cg, val, ty, to_stderr); return; }

    print_spec_t sp = parse_print_spec(spec, slen);

    /* ── binary: custom helper (printf has no portable %b) ── */
    if (sp.base == 'b') {
        if (sp.alt_form) print_emit_cstr(cg, "0b", to_stderr);
        LLVMValueRef ival = val;
        if (llvm_is_int(ty) && ty != LLVMInt64TypeInContext(cg->ctx))
            ival = LLVMBuildSExt(cg->builder, val,
                                  LLVMInt64TypeInContext(cg->ctx), "i64e");
        LLVMValueRef bfn = ensure_print_binary_fn(cg, to_stderr);
        LLVMTypeRef bfty = LLVMGlobalGetValueType(bfn);
        LLVMValueRef a[1] = { ival };
        LLVMBuildCall2(cg->builder, bfty, bfn, a, 1, "");
        return;
    }

    /* helper: emit a printf/fprintf call with one variadic argument */
#define EMIT_FMT1(pfmt_str, arg_val) do { \
    LLVMValueRef _f = LLVMBuildGlobalStringPtr(cg->builder, pfmt_str, "pfmt_v"); \
    if (to_stderr) { \
        LLVMTypeRef _pt = LLVMPointerTypeInContext(cg->ctx, 0); \
        LLVMValueRef _se = LLVMBuildLoad2(cg->builder, _pt, cg->stderr_var, "stderr"); \
        LLVMValueRef _a[3] = { _se, _f, (arg_val) }; \
        LLVMBuildCall2(cg->builder, cg->fprintf_type, cg->fprintf_fn, _a, 3, ""); \
    } else { \
        LLVMValueRef _a[2] = { _f, (arg_val) }; \
        LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, _a, 2, ""); \
    } \
} while (0)

    /* ── float ── */
    if (llvm_is_float(ty) || (sp.base == 0 && sp.prec != (usize_t)-1)) {
        LLVMValueRef dval = val;
        if (llvm_is_float(ty) && ty == LLVMFloatTypeInContext(cg->ctx))
            dval = LLVMBuildFPExt(cg->builder, val,
                                   LLVMDoubleTypeInContext(cg->ctx), "fpext");
        else if (llvm_is_int(ty))
            dval = LLVMBuildSIToFP(cg->builder, val,
                                    LLVMDoubleTypeInContext(cg->ctx), "itof");

        /* build: %[flags][width][.prec]g */
        char pfmt[32];
        usize_t n = 0;
        pfmt[n++] = '%';
        if (sp.left_align) pfmt[n++] = '-';
        if (sp.force_sign) pfmt[n++] = '+';
        if (sp.zero_pad && !sp.left_align) pfmt[n++] = '0';
        if (sp.width > 0)
            n += (usize_t)snprintf(pfmt + n, sizeof(pfmt) - n, "%zu", sp.width);
        if (sp.prec != (usize_t)-1)
            n += (usize_t)snprintf(pfmt + n, sizeof(pfmt) - n, ".%zu", sp.prec);
        pfmt[n++] = (sp.prec != (usize_t)-1) ? 'f' : 'g';
        pfmt[n] = '\0';

        EMIT_FMT1(pfmt, dval);
#undef EMIT_FMT1
        return;
    }

    /* ── integer (decimal, hex, octal) ── */
    {
        /* sign-extend to the right width for printf */
        boolean_t use64 = (ty == LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef ival = val;
        if (llvm_is_int(ty) && !use64) {
            ival = LLVMBuildSExt(cg->builder, val,
                                  LLVMInt32TypeInContext(cg->ctx), "iext");
        }

        /* build: %[flags][width]ll?[base] */
        char pfmt[32];
        usize_t n = 0;
        pfmt[n++] = '%';
        if (sp.left_align) pfmt[n++] = '-';
        if (sp.force_sign) pfmt[n++] = '+';
        if (sp.alt_form && sp.base != 'b') pfmt[n++] = '#';
        if (sp.zero_pad && !sp.left_align) pfmt[n++] = '0';
        if (sp.width > 0)
            n += (usize_t)snprintf(pfmt + n, sizeof(pfmt) - n, "%zu", sp.width);
        if (use64) { pfmt[n++] = 'l'; pfmt[n++] = 'l'; }

        char base_ch = (sp.base != 0) ? sp.base : 'd';
        pfmt[n++] = base_ch;
        pfmt[n] = '\0';

        LLVMValueRef f = LLVMBuildGlobalStringPtr(cg->builder, pfmt, "pfmt_i");
        if (to_stderr) {
            LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef stderr_val = LLVMBuildLoad2(cg->builder, ptr_t, cg->stderr_var, "stderr");
            LLVMValueRef a[3] = { stderr_val, f, ival };
            LLVMBuildCall2(cg->builder, cg->fprintf_type, cg->fprintf_fn, a, 3, "");
        } else {
            LLVMValueRef a[2] = { f, ival };
            LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, a, 2, "");
        }
    }
}

static void print_emit_struct_default(cg_t *cg, LLVMValueRef val, struct_reg_t *sr, boolean_t to_stderr) {
    char open[256];
    snprintf(open, sizeof(open), "%s{", sr->name);
    print_emit_cstr(cg, open, to_stderr);

    boolean_t first = True;
    for (usize_t i = 0; i < sr->field_count; i++) {
        field_info_t *f = &sr->fields[i];
        if (f->linkage != LinkageExternal) continue;

        if (!first) print_emit_cstr(cg, ", ", to_stderr);
        first = False;

        char label[256];
        snprintf(label, sizeof(label), ".%s = ", f->name);
        print_emit_cstr(cg, label, to_stderr);

        LLVMValueRef fval = LLVMBuildExtractValue(cg->builder, val,
                                                   (unsigned)f->index, "");
        print_emit_scalar(cg, fval, LLVMTypeOf(fval), to_stderr);
    }

    print_emit_cstr(cg, "}", to_stderr);
}

/* Emit one value argument with an optional format spec string.
   Called after the { ... } has been parsed from the format string. */
static void print_emit_arg(cg_t *cg, node_t *arg, const char *spec, usize_t slen, boolean_t to_stderr) {
    LLVMValueRef val = gen_expr(cg, arg);
    LLVMTypeRef ty   = LLVMTypeOf(val);

    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind && ty == cg->error_type) {
        /* error type: print the message string, or "(nil error)" if nil */
        LLVMValueRef has_err = LLVMBuildExtractValue(cg->builder, val, 0, "err_flag");
        LLVMValueRef msg_ptr = LLVMBuildExtractValue(cg->builder, val, 1, "err_msg");
        LLVMValueRef nil_str = LLVMBuildGlobalStringPtr(cg->builder, "(nil error)", "nil_err_str");
        LLVMValueRef selected = LLVMBuildSelect(cg->builder, has_err, msg_ptr, nil_str, "err_sel");
        LLVMValueRef efmt = LLVMBuildGlobalStringPtr(cg->builder, "%s", "err_pfmt");
        if (to_stderr) {
            LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef stderr_val = LLVMBuildLoad2(cg->builder, ptr_t, cg->stderr_var, "stderr");
            LLVMValueRef eargs[3] = { stderr_val, efmt, selected };
            LLVMBuildCall2(cg->builder, cg->fprintf_type, cg->fprintf_fn, eargs, 3, "");
        } else {
            LLVMValueRef eargs[2] = { efmt, selected };
            LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, eargs, 2, "");
        }
    } else if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
        /* struct: format spec is ignored — use print method or default layout */
        const char *sname = LLVMGetStructName(ty);
        if (sname) {
            char mname[256];
            snprintf(mname, sizeof(mname), "%s.print", sname);
            LLVMValueRef pfn = LLVMGetNamedFunction(cg->module, mname);
            if (pfn) {
                LLVMValueRef tmp = alloc_in_entry(cg, ty, "print_self");
                LLVMBuildStore(cg->builder, val, tmp);
                LLVMTypeRef fty = LLVMGlobalGetValueType(pfn);
                LLVMValueRef ca[1] = { tmp };
                LLVMBuildCall2(cg->builder, fty, pfn, ca, 1, "");
            } else {
                struct_reg_t *sr = find_struct(cg, sname);
                if (sr) print_emit_struct_default(cg, val, sr, to_stderr);
            }
        }
    } else {
        print_emit_scalar_spec(cg, val, ty, spec, slen, to_stderr);
    }
}

/* Decode a standard backslash escape char (e.g. 'n' -> '\n').
   Returns the raw char if unrecognised. */
static char print_decode_esc(char c) {
    switch (c) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        case '0':  return '\0';
        default:   return c;
    }
}

static void gen_print(cg_t *cg, node_t *node) {
    const char *fmt      = node->as.print_stmt.fmt;  /* raw: escape seqs not yet decoded */
    usize_t     flen     = node->as.print_stmt.fmt_len;
    node_list_t *args    = &node->as.print_stmt.args;
    boolean_t   to_stderr = node->as.print_stmt.to_stderr;
    usize_t arg_idx      = 0;

    /* working buffer for the current literal segment (decoded escapes) */
    heap_t seg_heap = allocate(flen + 1, 1);
    char  *seg      = seg_heap.pointer;
    usize_t seg_len = 0;

#define FLUSH_SEG() do {                                          \
    if (seg_len > 0) {                                            \
        seg[seg_len] = '\0';                                      \
        print_emit_cstr(cg, seg, to_stderr);                      \
        seg_len = 0;                                              \
    }                                                             \
} while (0)

    for (usize_t i = 0; i < flen; ) {
        /* ── backslash escape ── */
        if (fmt[i] == '\\' && i + 1 < flen) {
            char ec = fmt[i + 1];
            if (ec == '{') {
                seg[seg_len++] = '{'; /* \{ → literal { */
            } else if (ec == '}') {
                seg[seg_len++] = '}'; /* \} → literal } */
            } else {
                char decoded = print_decode_esc(ec);
                /* NUL would cut off printf; flush before it */
                if (decoded == '\0') { FLUSH_SEG(); }
                else seg[seg_len++] = decoded;
            }
            i += 2;
            continue;
        }

        /* ── placeholder { ... } ── */
        if (fmt[i] == '{') {
            usize_t j = i + 1;
            while (j < flen && fmt[j] != '}') j++;

            if (j >= flen) {
                /* no closing brace — emit as literal */
                seg[seg_len++] = fmt[i++];
                continue;
            }

            /* flush literal segment built so far */
            FLUSH_SEG();

            /* parse spec: skip optional leading ':' */
            const char *spec = fmt + i + 1;
            usize_t slen = j - (i + 1);
            if (slen > 0 && spec[0] == ':') { spec++; slen--; }

            /* emit the argument */
            if (arg_idx < args->count)
                print_emit_arg(cg, args->items[arg_idx++], spec, slen, to_stderr);

            i = j + 1;
            continue;
        }

        seg[seg_len++] = fmt[i++];
    }

    FLUSH_SEG();
#undef FLUSH_SEG

    deallocate(seg_heap);
}

static void gen_multi_assign(cg_t *cg, node_t *node) {
    node_list_t *targets = &node->as.multi_assign.targets;
    node_list_t *values = &node->as.multi_assign.values;

    /* let binding: infer types from the callee's return type list */
    boolean_t is_let = (targets->count > 0)
        && (targets->items[0]->as.var_decl.flags & VdeclLet);

    if (is_let && values->count == 1) {
        node_t *rhs = values->items[0];

        /* await.all(f1, ..., fN) destructuring: each target takes the
           corresponding future's element type (same T across all in v1). */
        if (rhs->kind == NodeAwaitCombinator
                && !rhs->as.await_combinator.is_any) {
            node_list_t *hs = &rhs->as.await_combinator.handles;
            for (usize_t i = 0; i < targets->count; i++) {
                node_t *tgt = targets->items[i];
                if (tgt->as.var_decl.name && strcmp(tgt->as.var_decl.name, "_") == 0)
                    continue;
                if (i < hs->count)
                    tgt->as.var_decl.type = resolve_future_elem_type(cg, hs->items[i]);
                else
                    tgt->as.var_decl.type = NO_TYPE;
            }
        } else {
            /* Extract the callee name from the single RHS expression.
               Supports plain calls, instance method calls, and self-method calls. */
            const char *callee = Null;
            if (rhs->kind == NodeCallExpr)
                callee = rhs->as.call.callee;
            else if (rhs->kind == NodeMethodCall)
                callee = rhs->as.method_call.method;
            else if (rhs->kind == NodeSelfMethodCall)
                callee = rhs->as.self_method_call.method;

            node_t *fn_decl = callee ? find_fn_decl(cg, callee) : Null;

            if (!fn_decl || fn_decl->as.fn_decl.return_count < 1) {
                diag_begin_error("'let' binding requires a multi-return function call");
                diag_span(DIAG_NODE(node), True, "not a multi-return call");
                diag_note("'let' binds the results of a function that returns multiple values");
                diag_help("example: stack i32 [lo, hi] = min_max(a, b);");
                diag_finish();
                return;
            }

            /* Assign inferred types to each non-blank target */
            for (usize_t i = 0; i < targets->count; i++) {
                node_t *tgt = targets->items[i];
                if (tgt->as.var_decl.name && strcmp(tgt->as.var_decl.name, "_") == 0)
                    continue;
                if (i < fn_decl->as.fn_decl.return_count)
                    tgt->as.var_decl.type = fn_decl->as.fn_decl.return_types[i];
                else
                    tgt->as.var_decl.type = NO_TYPE;
            }
        }
    }

    /* first, declare all target variables */
    for (usize_t i = 0; i < targets->count; i++)
        gen_local_var(cg, targets->items[i]);

    if (values->count == 1 && targets->count > 1) {
        /* single call returning multi-value: unpack struct */
        LLVMValueRef ret_val = gen_expr(cg, values->items[0]);
        for (usize_t i = 0; i < targets->count; i++) {
            LLVMValueRef elem = LLVMBuildExtractValue(cg->builder, ret_val, (unsigned)i, "");
            symbol_t *sym = cg_lookup(cg, targets->items[i]->as.var_decl.name);
            if (sym) {
                elem = coerce_int(cg, elem, sym->type);
                LLVMBuildStore(cg->builder, elem, sym->value);
            }
        }
    } else {
        /* multiple values: assign 1:1 */
        usize_t count = targets->count < values->count ? targets->count : values->count;
        for (usize_t i = 0; i < count; i++) {
            LLVMValueRef val = gen_expr(cg, values->items[i]);
            symbol_t *sym = cg_lookup(cg, targets->items[i]->as.var_decl.name);
            if (sym) {
                val = coerce_int(cg, val, sym->type);
                LLVMBuildStore(cg->builder, val, sym->value);
            }
        }
    }
}

static void gen_match(cg_t *cg, node_t *node) {
    node_t *subject_node = node->as.match_stmt.expr;
    usize_t arm_count = node->as.match_stmt.arms.count;
    if (arm_count == 0) return;

    /* Detect any-type match: subject is NodeAnyTypeExpr */
    boolean_t is_any_match = (subject_node->kind == NodeAnyTypeExpr);

    /* find enum registry from the first non-wildcard arm */
    enum_reg_t *er = Null;
    if (!is_any_match) {
        for (usize_t i = 0; i < arm_count; i++) {
            node_t *arm = node->as.match_stmt.arms.items[i];
            if (!arm->as.match_arm.is_wildcard && arm->as.match_arm.enum_name) {
                er = find_enum(cg, arm->as.match_arm.enum_name);
                break;
            }
        }
    }

    /* get subject alloca pointer (for payload extraction in tagged enums / any types) */
    LLVMValueRef subject_alloca = Null;
    LLVMValueRef discriminant;

    if (is_any_match) {
        /* subject is any.(val) — get the inner value (the any struct) */
        node_t *inner = subject_node->as.any_type_expr.operand;
        struct_reg_t *any_sr = Null;
        /* Try to get the any struct type from the symbol */
        if (inner->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, inner->as.ident.name);
            if (sym) {
                subject_alloca = sym->value;
                any_sr = find_struct_by_llvm_type(cg, sym->type);
            }
        }
        if (!subject_alloca) {
            LLVMValueRef val = gen_expr(cg, inner);
            /* need an alloca to GEP into */
            LLVMTypeRef vt = LLVMTypeOf(val);
            subject_alloca = alloc_in_entry(cg, vt, "any_match_subj");
            LLVMBuildStore(cg->builder, val, subject_alloca);
            any_sr = find_struct_by_llvm_type(cg, vt);
        }
        (void)any_sr; /* used below per-arm */
        /* discriminant = field 0 (tag i32) of the any struct */
        /* We need the LLVM struct type — get from the symbol alloca's allocated type */
        /* Use any_sr if found, otherwise read the tag directly */
        if (subject_alloca) {
            /* Determine the struct type from any_sr or by looking at what was stored */
            /* Find any_sr by scanning struct registry for is_any_type */
            struct_reg_t *found_any_sr = Null;
            if (inner->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, inner->as.ident.name);
                if (sym) found_any_sr = find_struct_by_llvm_type(cg, sym->type);
            }
            if (found_any_sr && found_any_sr->is_any_type) {
                LLVMValueRef tag_ptr = LLVMBuildStructGEP2(
                    cg->builder, found_any_sr->llvm_type, subject_alloca, 0, "any_tag_ptr");
                discriminant = LLVMBuildLoad2(
                    cg->builder, LLVMInt32TypeInContext(cg->ctx), tag_ptr, "any_tag");
            } else {
                /* fallback: extract value 0 from a loaded struct */
                LLVMValueRef val = gen_expr(cg, inner);
                discriminant = LLVMBuildExtractValue(cg->builder, val, 0, "any_tag");
            }
        } else {
            LLVMValueRef val = gen_expr(cg, inner);
            discriminant = LLVMBuildExtractValue(cg->builder, val, 0, "any_tag");
        }
    } else if (er && er->is_tagged) {
        if (subject_node->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, subject_node->as.ident.name);
            if (sym) subject_alloca = sym->value;
        }
        if (!subject_alloca) {
            /* expression result: store into temp alloca for GEP access */
            LLVMValueRef val = gen_expr(cg, subject_node);
            subject_alloca = alloc_in_entry(cg, er->llvm_type, "match_subj");
            LLVMBuildStore(cg->builder, val, subject_alloca);
        }
        LLVMValueRef disc_ptr = LLVMBuildStructGEP2(
            cg->builder, er->llvm_type, subject_alloca, 0, "disc_ptr");
        discriminant = LLVMBuildLoad2(
            cg->builder, LLVMInt32TypeInContext(cg->ctx), disc_ptr, "disc");
    } else {
        /* C-style enum or unknown: subject is already an integer discriminant */
        discriminant = gen_expr(cg, subject_node);
        discriminant = coerce_int(cg, discriminant, LLVMInt32TypeInContext(cg->ctx));
    }

    /* collect wildcard arm and count cases */
    node_t *wildcard_arm = Null;
    unsigned case_count = 0;
    for (usize_t i = 0; i < arm_count; i++) {
        node_t *arm = node->as.match_stmt.arms.items[i];
        if (arm->as.match_arm.is_wildcard)
            wildcard_arm = arm;
        else
            case_count++;
    }

    /* if any wildcard arm wants to bind the subject, store discriminant now
       (must happen before switch so alloc_in_entry goes in the entry block) */
    LLVMValueRef disc_alloca = Null;
    if (wildcard_arm && wildcard_arm->as.match_arm.bind_name) {
        LLVMTypeRef i32ty = LLVMInt32TypeInContext(cg->ctx);
        disc_alloca = alloc_in_entry(cg, i32ty, wildcard_arm->as.match_arm.bind_name);
        LLVMBuildStore(cg->builder, discriminant, disc_alloca);
    }

    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "match.end");
    LLVMBasicBlockRef default_bb = end_bb;

    if (wildcard_arm) {
        default_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->current_fn, "match.default");
    }

    LLVMValueRef sw = LLVMBuildSwitch(cg->builder, discriminant, default_bb, case_count);

    /* generate each non-wildcard arm */
    for (usize_t i = 0; i < arm_count; i++) {
        node_t *arm = node->as.match_stmt.arms.items[i];
        if (arm->as.match_arm.is_wildcard) continue;

        long disc_val = -1;

        if (arm->is_any_arm) {
            /* any-type arm: discriminant is the variant index inside the any struct */
            /* Find which index this type name corresponds to in the any struct */
            struct_reg_t *any_sr = Null;
            node_t *inner = subject_node->as.any_type_expr.operand;
            if (inner->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, inner->as.ident.name);
                if (sym) any_sr = find_struct_by_llvm_type(cg, sym->type);
            }
            if (any_sr && any_sr->is_any_type) {
                for (usize_t vi = 0; vi < any_sr->any_variant_count; vi++) {
                    type_info_t vt = any_sr->any_variants[vi];
                    const char *vname = (vt.base == TypeUser && vt.user_name)
                                        ? vt.user_name
                                        : type_name_for_base(vt.base);
                    if (vname && strcmp(vname, arm->any_type_name) == 0) {
                        disc_val = (long)vi;
                        break;
                    }
                }
            }
            if (disc_val < 0) {
                diag_begin_error("unknown type '%s' in any-match arm", arm->any_type_name);
                diag_span(DIAG_NODE(arm), True, "");
                diag_finish();
                continue;
            }

            LLVMBasicBlockRef arm_bb =
                LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "match.any.arm");
            LLVMAddCase(sw,
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                             (unsigned long long)disc_val, 0),
                arm_bb);
            LLVMPositionBuilderAtEnd(cg->builder, arm_bb);

            /* guard clause for any-arm */
            if (arm->as.match_arm.guard_expr) {
                LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
                    cg->ctx, cg->current_fn, "match.any.body");
                LLVMValueRef guard_val = gen_expr(cg, arm->as.match_arm.guard_expr);
                LLVMBuildCondBr(cg->builder, guard_val, body_bb, default_bb);
                LLVMPositionBuilderAtEnd(cg->builder, body_bb);
            }

            usize_t saved_locals = cg->locals.count;

            /* bind the concrete typed value if requested */
            if (arm->any_bind_name && any_sr && subject_alloca) {
                LLVMTypeRef data_arr_ty = LLVMArrayType2(
                    LLVMInt8TypeInContext(cg->ctx),
                    LLVMStoreSizeOfType(LLVMGetModuleDataLayout(cg->module), any_sr->llvm_type) - 4);
                (void)data_arr_ty;
                /* GEP into field 1 (data) of the any struct */
                LLVMValueRef data_ptr = LLVMBuildStructGEP2(
                    cg->builder, any_sr->llvm_type, subject_alloca, 1, "any_data");
                LLVMTypeRef bind_lltype = get_llvm_type(cg, arm->any_bind_ti);
                LLVMValueRef bind_val = LLVMBuildLoad2(
                    cg->builder, bind_lltype, data_ptr, arm->any_bind_name);
                LLVMValueRef bind_alloca = alloc_in_entry(cg, bind_lltype, arm->any_bind_name);
                LLVMBuildStore(cg->builder, bind_val, bind_alloca);
                symtab_add(&cg->locals, arm->any_bind_name,
                           bind_alloca, bind_lltype, arm->any_bind_ti, False);
            }

            push_dtor_scope(cg);
            for (usize_t s = 0; s < arm->as.match_arm.body->as.block.stmts.count; s++)
                gen_stmt(cg, arm->as.match_arm.body->as.block.stmts.items[s]);
            pop_dtor_scope(cg);

            cg->locals.count = saved_locals;

            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
                LLVMBuildBr(cg->builder, end_bb);
            continue;
        }

        /* integer literal arm: use the stored literal value directly */
        if (arm->as.match_arm.is_literal) {
            disc_val = arm->as.match_arm.literal_value;
        } else {
            /* find variant discriminant value */
            variant_info_t *vi_tmp = Null;
            enum_reg_t *arm_er_tmp = er ? er : find_enum(cg, arm->as.match_arm.enum_name);
            if (arm_er_tmp) {
                for (usize_t j = 0; j < arm_er_tmp->variant_count; j++) {
                    if (arm->as.match_arm.variant_name &&
                        strcmp(arm_er_tmp->variants[j].name,
                               arm->as.match_arm.variant_name) == 0) {
                        disc_val = arm_er_tmp->variants[j].value;
                        vi_tmp = &arm_er_tmp->variants[j];
                        break;
                    }
                }
            }
            if (disc_val < 0) {
                diag_begin_error("unknown variant '%s'",
                    arm->as.match_arm.variant_name
                        ? arm->as.match_arm.variant_name : "(null)");
                diag_span(DIAG_NODE(arm), True, "");
                diag_note("check the enum definition for valid variants");
                diag_finish();
                continue;
            }

            /* declare vi/arm_er in outer scope for payload binding below */
            variant_info_t *vi = vi_tmp;
            enum_reg_t *arm_er = arm_er_tmp ? arm_er_tmp : (er ? er : Null);

            LLVMBasicBlockRef arm_bb =
                LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "match.arm");
            LLVMAddCase(sw,
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                             (unsigned long long)disc_val, 0),
                arm_bb);
            LLVMPositionBuilderAtEnd(cg->builder, arm_bb);

            /* scope: save locals count so binding is invisible after this arm */
            usize_t saved_locals = cg->locals.count;

            /* bind payload BEFORE evaluating guard (guard may reference binding) */
            if (arm->as.match_arm.bind_name && vi && vi->has_payload && subject_alloca) {
                LLVMValueRef payload_ptr = LLVMBuildStructGEP2(
                    cg->builder, arm_er->llvm_type, subject_alloca, 1, "payload_ptr");
                LLVMTypeRef payload_lltype = get_llvm_type(cg, vi->payload_type);
                LLVMValueRef payload_val = LLVMBuildLoad2(
                    cg->builder, payload_lltype, payload_ptr, arm->as.match_arm.bind_name);
                LLVMValueRef bind_alloca =
                    alloc_in_entry(cg, payload_lltype, arm->as.match_arm.bind_name);
                LLVMBuildStore(cg->builder, payload_val, bind_alloca);
                symtab_add(&cg->locals, arm->as.match_arm.bind_name,
                           bind_alloca, payload_lltype, vi->payload_type, False);
            }

            /* guard clause: if arm has guard, branch on it; fail → default_bb */
            if (arm->as.match_arm.guard_expr) {
                LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
                    cg->ctx, cg->current_fn, "match.arm.body");
                LLVMValueRef guard_val = gen_expr(cg, arm->as.match_arm.guard_expr);
                LLVMBuildCondBr(cg->builder, guard_val, body_bb, default_bb);
                LLVMPositionBuilderAtEnd(cg->builder, body_bb);
            }

            push_dtor_scope(cg);
            for (usize_t s = 0; s < arm->as.match_arm.body->as.block.stmts.count; s++)
                gen_stmt(cg, arm->as.match_arm.body->as.block.stmts.items[s]);
            pop_dtor_scope(cg);

            cg->locals.count = saved_locals;

            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
                LLVMBuildBr(cg->builder, end_bb);
            continue;
        }

        /* literal arm code path */
        {
            LLVMBasicBlockRef arm_bb =
                LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "match.lit.arm");
            LLVMAddCase(sw,
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                             (unsigned long long)(long long)disc_val, 0),
                arm_bb);
            LLVMPositionBuilderAtEnd(cg->builder, arm_bb);

            /* guard clause for literal arm */
            if (arm->as.match_arm.guard_expr) {
                LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
                    cg->ctx, cg->current_fn, "match.lit.body");
                LLVMValueRef guard_val = gen_expr(cg, arm->as.match_arm.guard_expr);
                LLVMBuildCondBr(cg->builder, guard_val, body_bb, default_bb);
                LLVMPositionBuilderAtEnd(cg->builder, body_bb);
            }

            usize_t saved_locals = cg->locals.count;
            push_dtor_scope(cg);
            for (usize_t s = 0; s < arm->as.match_arm.body->as.block.stmts.count; s++)
                gen_stmt(cg, arm->as.match_arm.body->as.block.stmts.items[s]);
            pop_dtor_scope(cg);
            cg->locals.count = saved_locals;

            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
                LLVMBuildBr(cg->builder, end_bb);
        }
    }

    /* generate wildcard / default arm */
    if (wildcard_arm) {
        LLVMPositionBuilderAtEnd(cg->builder, default_bb);
        usize_t saved_locals = cg->locals.count;

        /* wildcard binding: bind the matched subject value to the given name */
        if (wildcard_arm->as.match_arm.bind_name && disc_alloca) {
            type_info_t bind_ti;
            memset(&bind_ti, 0, sizeof(bind_ti));
            bind_ti.base = TypeI32;
            LLVMTypeRef i32ty = LLVMInt32TypeInContext(cg->ctx);
            symtab_add(&cg->locals, wildcard_arm->as.match_arm.bind_name,
                       disc_alloca, i32ty, bind_ti, False);
        }

        push_dtor_scope(cg);
        for (usize_t s = 0; s < wildcard_arm->as.match_arm.body->as.block.stmts.count; s++)
            gen_stmt(cg, wildcard_arm->as.match_arm.body->as.block.stmts.items[s]);
        pop_dtor_scope(cg);

        cg->locals.count = saved_locals;

        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, end_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
}

static void gen_switch(cg_t *cg, node_t *node) {
    LLVMValueRef subject = gen_expr(cg, node->as.switch_stmt.expr);
    subject = coerce_int(cg, subject, LLVMInt32TypeInContext(cg->ctx));

    usize_t case_count = node->as.switch_stmt.cases.count;
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "sw.end");

    /* find default case */
    LLVMBasicBlockRef default_bb = end_bb;
    for (usize_t i = 0; i < case_count; i++) {
        node_t *c = node->as.switch_stmt.cases.items[i];
        if (c->as.switch_case.is_default) {
            default_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "sw.default");
            break;
        }
    }

    /* count non-default cases */
    unsigned num_cases = 0;
    for (usize_t i = 0; i < case_count; i++) {
        node_t *c = node->as.switch_stmt.cases.items[i];
        if (!c->as.switch_case.is_default)
            num_cases += (unsigned)c->as.switch_case.values.count;
    }

    LLVMValueRef sw = LLVMBuildSwitch(cg->builder, subject, default_bb, num_cases);

    LLVMBasicBlockRef saved_break = cg->break_target;
    cg->break_target = end_bb;

    for (usize_t i = 0; i < case_count; i++) {
        node_t *c = node->as.switch_stmt.cases.items[i];

        if (c->as.switch_case.is_default) {
            LLVMPositionBuilderAtEnd(cg->builder, default_bb);
            push_dtor_scope(cg);
            if (c->as.switch_case.body)
                gen_block(cg, c->as.switch_case.body);
            pop_dtor_scope(cg);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
                LLVMBuildBr(cg->builder, end_bb);
            continue;
        }

        LLVMBasicBlockRef case_bb =
            LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "sw.case");

        /* add each case value to the switch */
        for (usize_t v = 0; v < c->as.switch_case.values.count; v++) {
            node_t *val_node = c->as.switch_case.values.items[v];
            LLVMValueRef case_val;
            if (val_node->kind == NodeIntLitExpr)
                case_val = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                    (unsigned long long)val_node->as.int_lit.value, 1);
            else
                case_val = coerce_int(cg, gen_expr(cg, val_node),
                    LLVMInt32TypeInContext(cg->ctx));
            LLVMAddCase(sw, case_val, case_bb);
        }

        LLVMPositionBuilderAtEnd(cg->builder, case_bb);
        push_dtor_scope(cg);
        if (c->as.switch_case.body)
            gen_block(cg, c->as.switch_case.body);
        pop_dtor_scope(cg);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, end_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
}

static void gen_asm_stmt(cg_t *cg, node_t *node) {
    char *code = node->as.asm_stmt.code;
    char *constraints = node->as.asm_stmt.constraints;
    if (!constraints) constraints = "";

    usize_t argc = node->as.asm_stmt.operands.count;
    LLVMTypeRef *arg_types = Null;
    LLVMValueRef *args = Null;
    heap_t at_heap = NullHeap, a_heap = NullHeap;
    if (argc > 0) {
        at_heap = allocate(argc, sizeof(LLVMTypeRef));
        a_heap  = allocate(argc, sizeof(LLVMValueRef));
        arg_types = at_heap.pointer;
        args      = a_heap.pointer;
        for (usize_t i = 0; i < argc; i++) {
            args[i] = gen_expr(cg, node->as.asm_stmt.operands.items[i]);
            arg_types[i] = LLVMTypeOf(args[i]);
        }
    }

    LLVMTypeRef fn_type = LLVMFunctionType(
        LLVMVoidTypeInContext(cg->ctx), arg_types, (unsigned)argc, 0);
    LLVMValueRef asm_val = LLVMGetInlineAsm(
        fn_type, code, strlen(code),
        constraints, strlen(constraints),
        1 /* has side effects */, 0 /* not align stack */,
        LLVMInlineAsmDialectATT, 0 /* can throw */);
    LLVMBuildCall2(cg->builder, fn_type, asm_val, args, (unsigned)argc, "");

    if (argc > 0) { deallocate(at_heap); deallocate(a_heap); }
}

static void gen_comptime_if(cg_t *cg, node_t *node) {
    char *key = node->as.comptime_if.key;
    char *value = node->as.comptime_if.value;
    boolean_t match = False;

    if (strcmp(key, "platform") == 0 || strcmp(key, "os") == 0) {
#if defined(__APPLE__)
        match = (strcmp(value, "macos") == 0 || strcmp(value, "darwin") == 0);
#elif defined(__linux__)
        match = (strcmp(value, "linux") == 0);
#elif defined(_WIN32)
        match = (strcmp(value, "windows") == 0);
#endif
    } else if (strcmp(key, "arch") == 0) {
#if defined(__x86_64__) || defined(_M_X64)
        match = (strcmp(value, "x86_64") == 0 || strcmp(value, "amd64") == 0);
#elif defined(__aarch64__) || defined(_M_ARM64)
        match = (strcmp(value, "aarch64") == 0 || strcmp(value, "arm64") == 0);
#elif defined(__i386__) || defined(_M_IX86)
        match = (strcmp(value, "x86") == 0 || strcmp(value, "i386") == 0);
#endif
    }

    if (match) {
        if (node->as.comptime_if.body)
            gen_block(cg, node->as.comptime_if.body);
    } else {
        if (node->as.comptime_if.else_body)
            gen_block(cg, node->as.comptime_if.else_body);
    }
}

/* ── comptime expression evaluator ── */

static boolean_t eval_comptime_expr(cg_t *cg, node_t *expr, long *out);

static boolean_t lookup_comptime_field(cg_t *cg, const char *struct_name,
                                       const char *field, long *out) {
    /* strip generic suffix: "table_t_G_i32_G_f32" → "table_t" */
    const char *g = strstr(struct_name, "_G_");
    char template_name[256];
    const char *lookup_name = struct_name;
    if (g) {
        usize_t prefix_len = (usize_t)(g - struct_name);
        if (prefix_len < sizeof(template_name)) {
            memcpy(template_name, struct_name, prefix_len);
            template_name[prefix_len] = '\0';
            lookup_name = template_name;
        }
    }
    /* search registered struct comptime fields */
    struct_reg_t *sr = find_struct(cg, lookup_name);
    if (!sr && lookup_name != struct_name) sr = find_struct(cg, struct_name);
    if (sr) {
        for (usize_t i = 0; i < sr->ct_field_count; i++) {
            if (strcmp(sr->ct_fields[i].name, field) == 0) {
                *out = sr->ct_fields[i].value;
                return True;
            }
        }
    }
    /* search AST directly for @comptime: field with matching name */
    if (cg->root_ast) {
        node_t *root = cg->root_ast;
        for (usize_t di = 0; di < root->as.module.decls.count; di++) {
            node_t *d = root->as.module.decls.items[di];
            if (d->kind != NodeTypeDecl) continue;
            const char *dname = d->as.type_decl.name;
            if (strcmp(dname, lookup_name) != 0 && strcmp(dname, struct_name) != 0) continue;
            for (usize_t fi = 0; fi < d->as.type_decl.fields.count; fi++) {
                node_t *f = d->as.type_decl.fields.items[fi];
                if (f->kind != NodeVarDecl) continue;
                if (!(f->as.var_decl.flags & VdeclComptimeField)) continue;
                if (strcmp(f->as.var_decl.name, field) != 0) continue;
                if (f->as.var_decl.init && f->as.var_decl.init->kind == NodeIntLitExpr) {
                    *out = f->as.var_decl.init->as.int_lit.value;
                    return True;
                }
                if (f->as.var_decl.init && f->as.var_decl.init->kind == NodeBoolLitExpr) {
                    *out = f->as.var_decl.init->as.bool_lit.value ? 1 : 0;
                    return True;
                }
            }
        }
    }
    return False;
}

static boolean_t eval_comptime_expr(cg_t *cg, node_t *expr, long *out) {
    if (!expr) return False;
    switch (expr->kind) {
        case NodeIntLitExpr:
            *out = expr->as.int_lit.value;
            return True;
        case NodeBoolLitExpr:
            *out = expr->as.bool_lit.value ? 1 : 0;
            return True;
        case NodeSizeofExpr: {
            LLVMTypeRef ty = get_llvm_type(cg, expr->as.sizeof_expr.type);
            *out = (long)LLVMABISizeOfType(LLVMGetModuleDataLayout(cg->module), ty);
            return True;
        }
        case NodeMemberExpr: {
            node_t *obj = expr->as.member_expr.object;
            if (obj && obj->kind == NodeIdentExpr) {
                return lookup_comptime_field(cg, obj->as.ident.name,
                                            expr->as.member_expr.field, out);
            }
            return False;
        }
        case NodeUnaryPrefixExpr:
            if (expr->as.unary.op == TokMinus) {
                long v;
                if (!eval_comptime_expr(cg, expr->as.unary.operand, &v)) return False;
                *out = -v;
                return True;
            }
            if (expr->as.unary.op == TokBang) {
                long v;
                if (!eval_comptime_expr(cg, expr->as.unary.operand, &v)) return False;
                *out = !v;
                return True;
            }
            return False;
        case NodeBinaryExpr: {
            long l, r;
            if (!eval_comptime_expr(cg, expr->as.binary.left, &l)) return False;
            if (!eval_comptime_expr(cg, expr->as.binary.right, &r)) return False;
            switch (expr->as.binary.op) {
                case TokPlus:    *out = l + r;  return True;
                case TokMinus:   *out = l - r;  return True;
                case TokStar:    *out = l * r;  return True;
                case TokSlash:   if (!r) return False; *out = l / r; return True;
                case TokPercent: if (!r) return False; *out = l % r; return True;
                case TokEqEq:    *out = l == r; return True;
                case TokBangEq:  *out = l != r; return True;
                case TokLt:      *out = l < r;  return True;
                case TokLtEq:    *out = l <= r; return True;
                case TokGt:      *out = l > r;  return True;
                case TokGtEq:    *out = l >= r; return True;
                case TokAmpAmp:  *out = l && r; return True;
                case TokPipePipe:*out = l || r; return True;
                case TokAmp:     *out = l & r;  return True;
                case TokPipe:    *out = l | r;  return True;
                case TokCaret:   *out = l ^ r;  return True;
                case TokLtLt:    *out = l << r; return True;
                case TokGtGt:    *out = l >> r; return True;
                default: return False;
            }
        }
        default:
            return False;
    }
}

static void comptime_assert_fail(cg_t *cg, node_t *node,
                                  const char *message, const char *detail) {
    (void)cg;
    if (message && detail)
        diag_begin_error("comptime_assert failed: %s (%s)", message, detail);
    else if (message)
        diag_begin_error("comptime_assert failed: %s", message);
    else if (detail)
        diag_begin_error("comptime_assert failed (%s)", detail);
    else
        diag_begin_error("comptime_assert failed");
    diag_span(DIAG_NODE(node), True, "assertion failed here");
    diag_note("the assertion evaluated at compile time and did not hold");
    diag_finish();
}

static void gen_comptime_assert(cg_t *cg, node_t *node) {
    node_t *expr = node->as.comptime_assert.expr;
    char *message = node->as.comptime_assert.message;

    long val;
    if (eval_comptime_expr(cg, expr, &val)) {
        if (!val) {
            /* build a detail string for binary comparisons */
            char detail[128] = {0};
            if (expr->kind == NodeBinaryExpr) {
                long l, r;
                if (eval_comptime_expr(cg, expr->as.binary.left, &l) &&
                    eval_comptime_expr(cg, expr->as.binary.right, &r))
                    snprintf(detail, sizeof(detail), "%ld vs %ld", l, r);
            }
            comptime_assert_fail(cg, node, message, detail[0] ? detail : Null);
        }
        return;
    }
    diag_begin_error("comptime_assert: expression too complex to evaluate at compile time");
    diag_span(DIAG_NODE(node), True, "unsupported expression");
    diag_note("only constants, sizeof, struct @comptime fields, and arithmetic are supported");
    diag_finish();
}

static void gen_with_stmt(cg_t *cg, node_t *node) {
    usize_t saved_locals = cg->locals.count;
    push_dtor_scope(cg);
    /* generate the binding declaration */
    gen_stmt(cg, node->as.with_stmt.decl);
    /* evaluate condition */
    LLVMValueRef cond = gen_expr(cg, node->as.with_stmt.cond);
    cond = llvm_to_bool(cg, cond);
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->current_fn, "with.body");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->current_fn, "with.end");
    node_t *else_block = node->as.with_stmt.else_block;
    LLVMBasicBlockRef else_bb = else_block
        ? LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "with.else")
        : end_bb;
    LLVMBuildCondBr(cg->builder, cond, body_bb, else_bb);
    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.with_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, end_bb);
    if (else_block) {
        LLVMPositionBuilderAtEnd(cg->builder, else_bb);
        gen_block(cg, else_block);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, end_bb);
    }
    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    pop_dtor_scope(cg);
    cg->locals.count = saved_locals;
}

static void gen_stmt(cg_t *cg, node_t *node) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;

    /* Attach source-line location to every instruction emitted for this
       statement.  Skipped for declarations handled by gen_local_var (which
       sets its own location) and for pure compile-time nodes that emit no IR. */
    if (cg->debug_mode && node->line > 0
        && node->kind != NodeVarDecl
        && node->kind != NodeComptimeIf
        && node->kind != NodeComptimeAssert)
        di_set_location(cg, node->line);

    switch (node->kind) {
        case NodeVarDecl:      gen_local_var(cg, node); break;
        case NodeMultiAssign:  gen_multi_assign(cg, node); break;
        case NodeForStmt:      gen_for(cg, node); break;
        case NodeForeachStmt:  gen_foreach(cg, node); break;
        case NodeWhileStmt:    gen_while(cg, node); break;
        case NodeDoWhileStmt:  gen_do_while(cg, node); break;
        case NodeInfLoop:      gen_inf_loop(cg, node); break;
        case NodeIfStmt:       gen_if(cg, node); break;
        case NodeRetStmt:      gen_ret(cg, node); break;
        case NodePrintStmt:    gen_print(cg, node); break;
        case NodeExprStmt:     gen_expr(cg, node->as.expr_stmt.expr); break;
        case NodeRemStmt:      gen_expr(cg, node); break;
        case NodeMatchStmt:    gen_match(cg, node); break;
        case NodeSwitchStmt:   gen_switch(cg, node); break;
        case NodeAsmStmt:      gen_asm_stmt(cg, node); break;
        case NodeComptimeIf:   gen_comptime_if(cg, node); break;
        case NodeComptimeAssert: gen_comptime_assert(cg, node); break;
        case NodeWithStmt:     gen_with_stmt(cg, node); break;
        case NodeYieldExpr: {
            if (!cg->cur_coro.active) {
                diag_begin_error("'yield' is only legal inside a stream coroutine");
                diag_span(DIAG_NODE(node), True, "yield used in non-stream context");
                diag_finish();
                break;
            }
            LLVMValueRef yv = gen_expr(cg, node->as.yield_expr.value);
            sts_emit_yield_value(cg, &cg->cur_coro, yv, LLVMTypeOf(yv));
            break;
        }
        case NodeYieldNowExpr:
            if (!cg->cur_coro.active) {
                diag_begin_error("'yield;' is only legal inside a stream coroutine");
                diag_span(DIAG_NODE(node), True, "scheduler-yield used outside coroutine");
                diag_finish();
                break;
            }
            sts_emit_yield_now(cg, &cg->cur_coro);
            break;
        case NodeBreakStmt:
            if (cg->break_target)
                LLVMBuildBr(cg->builder, cg->break_target);
            else {
                diag_begin_error("'break' used outside of a loop, switch, or watch handler");
                diag_span(DIAG_NODE(node), True, "break here");
                diag_note("'break' can only appear inside for, while, do-while, inf, switch, or watch handlers");
                diag_finish();
            }
            break;
        case NodeWatchStmt:    gen_watch_stmt(cg, node); break;
        case NodeSendStmt:     gen_send_stmt(cg, node); break;
        case NodeQuitStmt:     gen_quit_stmt(cg, node); break;
        case NodeContinueStmt:
            if (cg->continue_target)
                LLVMBuildBr(cg->builder, cg->continue_target);
            else {
                diag_begin_error("'continue' used outside of a loop");
                diag_span(DIAG_NODE(node), True, "continue here");
                diag_note("'continue' can only appear inside for, while, do-while, or inf loops");
                diag_finish();
            }
            break;
        case NodeDeferStmt:
            add_deferred_stmt(cg, node->as.defer_stmt.body);
            break;
        case NodeBlock:
            gen_block(cg, node);
            break;

        /* ── unsafe { body } — suppress safety checks within block ── */
        case NodeUnsafeBlock:
            cg->in_unsafe++;
            gen_block(cg, node->as.unsafe_block.body);
            cg->in_unsafe--;
            break;

        /* ── zone name; — manual zone variable declaration ── */
        case NodeZoneStmt: {
            const char *zname = node->as.zone_stmt.name;
            /* allocate a zone struct on the stack (opaque pointer placeholder).
             * The zone pointer is initialised to null; the runtime allocates
             * the zone block lazily on first __zone_alloc call. */
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef alloca_z = alloc_in_entry(cg, ptr_ty, zname);
            LLVMBuildStore(cg->builder, LLVMConstNull(ptr_ty), alloca_z);
            type_info_t zone_ti = NO_TYPE;
            zone_ti.base = TypeI64; /* opaque; used as ptr */
            zone_ti.is_pointer = True;
            zone_ti.ptr_perm = PtrReadWrite;
            symtab_add(&cg->locals, zname, alloca_z, ptr_ty, zone_ti, SymZone);
            symtab_set_last_storage(&cg->locals, StorageStack, False);
            symtab_set_last_extra(&cg->locals, False, False, LinkageNone,
                                  cg->dtor_depth, -1);
            symtab_set_last_line(&cg->locals, node->line);
            break;
        }

        /* ── zone name { body } — lexical zone, freed at closing brace ── */
        case NodeZoneDecl: {
            const char *zname = node->as.zone_decl.name;
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef alloca_z = alloc_in_entry(cg, ptr_ty, zname);
            LLVMBuildStore(cg->builder, LLVMConstNull(ptr_ty), alloca_z);
            type_info_t zone_ti = NO_TYPE;
            zone_ti.base = TypeI64;
            zone_ti.is_pointer = True;
            zone_ti.ptr_perm = PtrReadWrite;
            symtab_add(&cg->locals, zname, alloca_z, ptr_ty, zone_ti, SymZone);
            symtab_set_last_storage(&cg->locals, StorageStack, False);
            symtab_set_last_extra(&cg->locals, False, False, LinkageNone,
                                  cg->dtor_depth, -1);
            symtab_set_last_line(&cg->locals, node->line);
            /* generate the zone body */
            gen_block(cg, node->as.zone_decl.body);
            /* emit __zone_free(&zone_ptr) at lexical scope exit.
             * __zone_free takes void** so it can set *zone_ptr = NULL.
             * It is a no-op when *zone_ptr is already NULL. */
            {
                LLVMValueRef fa[1] = { alloca_z }; /* void** */
                LLVMBuildCall2(cg->builder, cg->zone_free_type, cg->zone_free_fn, fa, 1, "");
            }
            break;
        }

        /* ── zone.free(name) as statement ── */
        case NodeZoneFreeStmt: {
            symbol_t *zsym = cg_lookup(cg, node->as.zone_free.name);
            if (zsym) {
                /* Pass void** (the alloca) so __zone_free can clear *zone_ptr */
                LLVMValueRef fa[1] = { zsym->value };
                LLVMBuildCall2(cg->builder, cg->zone_free_type, cg->zone_free_fn, fa, 1, "");
            } else {
                diag_begin_error("undefined zone '%s'", node->as.zone_free.name);
                diag_span(DIAG_NODE(node), True, "zone not found in scope");
                diag_finish();
            }
            break;
        }

        /* zone.move as stmt (expression-like, result discarded) */
        case NodeZoneMoveExpr:
            gen_expr(cg, node);
            break;

        default:
            diag_begin_error("unexpected statement kind %d", node->kind);
            diag_finish();
            break;
    }
}

static void gen_block(cg_t *cg, node_t *node) {
    usize_t old_count = cg->locals.count;
    push_dtor_scope(cg);
    boolean_t block_terminated = False;
    for (usize_t i = 0; i < node->as.block.stmts.count; i++) {
        if (block_terminated) {
            /* Warn on first unreachable statement only */
            node_t *unreachable = node->as.block.stmts.items[i];
            diag_begin_optional_warning(WarnUnreachableCode,
                "unreachable code after return/break/continue");
            diag_set_category(ErrCatOther);
            diag_span(DIAG_NODE(unreachable), True, "this code will never execute");
            diag_finish();
            break;  /* stop generating unreachable stmts */
        }
        gen_stmt(cg, node->as.block.stmts.items[i]);
        /* Check if the current basic block now has a terminator */
        LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
        if (cur && LLVMGetBasicBlockTerminator(cur))
            block_terminated = True;
    }
    /* emit unused variable warnings for locals added in this block */
    for (usize_t i = old_count; i < cg->locals.count; i++) {
        symbol_t *entry = &cg->locals.entries[i];
        if (!entry->used && entry->name && entry->name[0] != '_') {
            diag_begin_optional_warning(WarnUnusedVar, "unused variable '%s'", entry->name);
            if (entry->line > 0)
                diag_span(SRC_LOC(entry->line, 0, strlen(entry->name)), True,
                          "variable declared here");
            diag_note("prefix the name with '_' to suppress this warning");
            diag_finish();
        }
    }
    pop_dtor_scope(cg);
    cg->locals.count = old_count;
}
