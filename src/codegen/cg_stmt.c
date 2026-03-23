/* ── statements ── */

/* Returns True if type_info is a primitive (non-struct, non-enum) that can be heap-allocated */
static boolean_t is_primitive_type(type_info_t ti) {
    if (ti.is_pointer || ti.base == TypeVoid) return False;
    if (ti.base == TypeUser)   return False;
    if (ti.base == TypeFnPtr)  return False; /* function pointers are not heap primitives */
    if (ti.base == TypeFuture) return False; /* future is an opaque pointer — never heap-wrap */
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
        LLVMValueRef alloca_val = alloc_in_entry(cg, inferred, node->as.var_decl.name);
        LLVMBuildStore(cg->builder, init_val, alloca_val);
        {
            int sym_flags = 0;
            if (node->as.var_decl.flags & VdeclAtomic)  sym_flags |= SymAtomic;
            if (node->as.var_decl.flags & VdeclVolatile) sym_flags |= SymVolatile;
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

    /* Catch unknown user-defined types before generating any code. */
    if (ti.base == TypeUser && ti.user_name && !ti.is_pointer) {
        if (!find_struct(cg, ti.user_name) && !find_enum(cg, ti.user_name)) {
            diag_begin_error("unknown type '%s'", ti.user_name);
            diag_span(DIAG_NODE(node), True, "type used here");
            diag_note("did you forget to define or import the type?");
            diag_finish();
        }
    }

    LLVMTypeRef type;

    if (node->as.var_decl.flags & VdeclArray) {
        LLVMTypeRef elem = get_llvm_type(cg, ti);
        unsigned long long array_len = (unsigned long long)node->as.var_decl.array_size;
        if (node->as.var_decl.array_size_name) {
            symbol_t *sym = cg_lookup(cg, node->as.var_decl.array_size_name);
            if (sym) {
                LLVMValueRef init = LLVMGetInitializer(sym->value);
                if (init) array_len = LLVMConstIntGetZExtValue(init);
            } else {
                diag_begin_error("undefined constant '%s' used as array size",
                                 node->as.var_decl.array_size_name);
                diag_span(DIAG_NODE(node), True, "array declared here");
                diag_note("array size must be a compile-time constant declared before this point");
                diag_finish();
            }
        }
        type = LLVMArrayType2(elem, array_len);
    } else {
        type = get_llvm_type(cg, ti);
    }

    /* heap primitive: allocate via malloc, register for auto-free */
    boolean_t is_heap = (node->as.var_decl.storage == StorageHeap)
                        && is_primitive_type(ti)
                        && !(node->as.var_decl.flags & VdeclArray);

    if (is_heap) {
        /* alloca holds the heap pointer (alloca of ptr type, in entry block) */
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef alloca_val = alloc_in_entry(cg, ptr_ty, node->as.var_decl.name);

        /* check cross-domain: if init is addr-of a stack variable, error */
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeAddrOf) {
            node_t *addr_op = node->as.var_decl.init->as.addr_of.operand;
            if (addr_op->kind == NodeIdentExpr) {
                symbol_t *src = cg_lookup(cg, addr_op->as.ident.name);
                if (src && src->storage == StorageStack) {
                    diag_begin_error("cannot assign a stack address to a heap pointer");
                    diag_span(DIAG_NODE(node), True, "heap pointer assigned here");
                    diag_note("stack variables are freed when their scope ends; the heap pointer would dangle");
                    diag_help("allocate the source variable on the heap: heap i32 x = 0;");
                    diag_finish();
                }
            }
        }

        /* call malloc(sizeof(type)) */
        LLVMValueRef sz = LLVMSizeOf(type);
        sz = coerce_int(cg, sz, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef args[1] = { sz };
        LLVMValueRef heap_ptr = LLVMBuildCall2(cg->builder, cg->malloc_type,
                                                cg->malloc_fn, args, 1, "hmalloc");
        LLVMBuildStore(cg->builder, heap_ptr, alloca_val);

        if (node->as.var_decl.init) {
            LLVMValueRef init = gen_expr(cg, node->as.var_decl.init);
            init = coerce_int(cg, init, type);
            LLVMBuildStore(cg->builder, init, heap_ptr);
        }

        symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, type,
                   ti, (node->as.var_decl.flags & VdeclAtomic) ? SymAtomic : 0);
        symtab_set_last_storage(&cg->locals, StorageHeap, True);
        symtab_set_last_extra(&cg->locals, node->as.var_decl.flags & VdeclConst,
                              node->as.var_decl.flags & VdeclFinal, node->as.var_decl.linkage,
                              cg->dtor_depth, -1);
        symtab_set_last_line(&cg->locals, node->line);
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeNilExpr)
            symtab_set_last_nil(&cg->locals, True);

        /* DI: heap primitive — declare the alloca holding the heap ptr. */
        if (cg->debug_mode && cg->di_builder && cg->di_scope && node->line > 0) {
            LLVMMetadataRef di_pty = get_di_type(cg, ti);
            LLVMMetadataRef di_var = LLVMDIBuilderCreateAutoVariable(
                cg->di_builder, cg->di_scope,
                node->as.var_decl.name, strlen(node->as.var_decl.name),
                cg->di_file, (unsigned)node->line,
                di_pty, /* alwaysPreserve= */ 0, LLVMDIFlagZero, /* alignInBits= */ 0);
            LLVMMetadataRef di_expr =
                LLVMDIBuilderCreateExpression(cg->di_builder, Null, 0);
            LLVMMetadataRef di_loc = di_make_location(cg, node->line);
            LLVMDIBuilderInsertDeclareRecordAtEnd(
                cg->di_builder, alloca_val, di_var, di_expr, di_loc,
                LLVMGetInsertBlock(cg->builder));
        }

        add_heap_var(cg, alloca_val);
        return;
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

    /* cross-domain check for stack pointer assigned a heap address (and vice-versa) */
    if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeAddrOf && ti.is_pointer) {
        node_t *addr_op = node->as.var_decl.init->as.addr_of.operand;
        if (addr_op->kind == NodeIdentExpr) {
            symbol_t *src = cg_lookup(cg, addr_op->as.ident.name);
            if (src) {
                storage_t ptr_domain = node->as.var_decl.storage;
                if (ptr_domain == StorageHeap && src->storage == StorageStack) {
                    diag_begin_error("cannot assign a stack address to a heap pointer");
                    diag_span(DIAG_NODE(node), True, "assigned here");
                    diag_note("stack addresses become invalid after their scope ends");
                    diag_help("use 'heap' storage for the source variable");
                    diag_finish();
                } else if (ptr_domain == StorageStack && src->storage == StorageHeap) {
                    diag_begin_error("cannot assign a heap address to a stack pointer");
                    diag_span(DIAG_NODE(node), True, "assigned here");
                    diag_note("mixing stack and heap pointer domains is not allowed");
                    diag_finish();
                }
            }
        }
    }

    if (node->as.var_decl.init) {
        LLVMValueRef init;
        /* nil → error type produces a nil error struct */
        if (node->as.var_decl.init->kind == NodeNilExpr && ti.base == TypeError) {
            init = make_nil_error(cg);
        } else {
            init = gen_expr(cg, node->as.var_decl.init);
            if (!(node->as.var_decl.flags & VdeclArray))
                init = coerce_int(cg, init, type);
        }
        LLVMBuildStore(cg->builder, init, alloca_val);
    }

    {
        int sym_flags = 0;
        if (node->as.var_decl.flags & VdeclAtomic)   sym_flags |= SymAtomic;
        if (node->as.var_decl.flags & VdeclVolatile)  sym_flags |= SymVolatile;
        symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, type, ti, sym_flags);
    }
    symtab_set_last_storage(&cg->locals, node->as.var_decl.storage, False);
    {
        long arr_sz = (node->as.var_decl.flags & VdeclArray) ? node->as.var_decl.array_size : -1;
        symtab_set_last_extra(&cg->locals, node->as.var_decl.flags & VdeclConst,
                              node->as.var_decl.flags & VdeclFinal, node->as.var_decl.linkage,
                              cg->dtor_depth, arr_sz);
        symtab_set_last_line(&cg->locals, node->line);
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeNilExpr)
            symtab_set_last_nil(&cg->locals, True);
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
        LLVMBuildRetVoid(cg->builder);
    } else if (node->as.ret_stmt.values.count == 1) {
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        LLVMValueRef val;
        /* nil returning as error → {i1=0, ptr=null} */
        if (node->as.ret_stmt.values.items[0]->kind == NodeNilExpr
            && ret_type == cg->error_type) {
            val = make_nil_error(cg);
        } else {
            val = gen_expr(cg, node->as.ret_stmt.values.items[0]);
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
                val = gen_expr(cg, node->as.ret_stmt.values.items[i]);
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
static LLVMValueRef ensure_print_binary_fn(cg_t *cg) {
    const char *fname = "__sts_print_bin";
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, fname);
    if (fn) return fn;

    LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef i32_t  = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i8_t   = LLVMInt8TypeInContext(cg->ctx);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(cg->ctx);
    LLVMTypeRef ptr_t  = LLVMPointerTypeInContext(cg->ctx, 0);

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

    /* printf("%s", buf_ptr) */
    LLVMValueRef first_idx[2] = { zero32, zero32 };
    LLVMValueRef buf_ptr = LLVMBuildGEP2(b, buf_ty, buf, first_idx, 2, "bufptr");
    LLVMValueRef pfmt = LLVMBuildGlobalStringPtr(b, "%s", "binfmt");
    LLVMValueRef pargs[2] = { pfmt, buf_ptr };
    LLVMBuildCall2(b, cg->printf_type, cg->printf_fn, pargs, 2, "");

    LLVMBuildRetVoid(b);

    LLVMDisposeBuilder(b);
    return fn;
}

static void print_emit_cstr(cg_t *cg, const char *s) {
    LLVMValueRef str = LLVMBuildGlobalStringPtr(cg->builder, s, "plit");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, "%s", "pfmt_s");
    LLVMValueRef pargs[2] = { fmt, str };
    LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, pargs, 2, "");
}

static void print_emit_scalar(cg_t *cg, LLVMValueRef val, LLVMTypeRef ty) {
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
    LLVMValueRef pargs[2] = { fmt, print_val };
    LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, pargs, 2, "");
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
                                   const char *spec, usize_t slen) {
    if (slen == 0) { print_emit_scalar(cg, val, ty); return; }

    print_spec_t sp = parse_print_spec(spec, slen);

    /* ── binary: custom helper (printf has no portable %b) ── */
    if (sp.base == 'b') {
        if (sp.alt_form) print_emit_cstr(cg, "0b");
        LLVMValueRef ival = val;
        if (llvm_is_int(ty) && ty != LLVMInt64TypeInContext(cg->ctx))
            ival = LLVMBuildSExt(cg->builder, val,
                                  LLVMInt64TypeInContext(cg->ctx), "i64e");
        LLVMValueRef bfn = ensure_print_binary_fn(cg);
        LLVMTypeRef bfty = LLVMGlobalGetValueType(bfn);
        LLVMValueRef a[1] = { ival };
        LLVMBuildCall2(cg->builder, bfty, bfn, a, 1, "");
        return;
    }

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

        LLVMValueRef f = LLVMBuildGlobalStringPtr(cg->builder, pfmt, "pfmt_f");
        LLVMValueRef a[2] = { f, dval };
        LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, a, 2, "");
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
        LLVMValueRef a[2] = { f, ival };
        LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, a, 2, "");
    }
}

static void print_emit_struct_default(cg_t *cg, LLVMValueRef val, struct_reg_t *sr) {
    char open[256];
    snprintf(open, sizeof(open), "%s{", sr->name);
    print_emit_cstr(cg, open);

    boolean_t first = True;
    for (usize_t i = 0; i < sr->field_count; i++) {
        field_info_t *f = &sr->fields[i];
        if (f->linkage != LinkageExternal) continue;

        if (!first) print_emit_cstr(cg, ", ");
        first = False;

        char label[256];
        snprintf(label, sizeof(label), ".%s = ", f->name);
        print_emit_cstr(cg, label);

        LLVMValueRef fval = LLVMBuildExtractValue(cg->builder, val,
                                                   (unsigned)f->index, "");
        print_emit_scalar(cg, fval, LLVMTypeOf(fval));
    }

    print_emit_cstr(cg, "}");
}

/* Emit one value argument with an optional format spec string.
   Called after the { ... } has been parsed from the format string. */
static void print_emit_arg(cg_t *cg, node_t *arg, const char *spec, usize_t slen) {
    LLVMValueRef val = gen_expr(cg, arg);
    LLVMTypeRef ty   = LLVMTypeOf(val);

    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
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
                if (sr) print_emit_struct_default(cg, val, sr);
            }
        }
    } else {
        print_emit_scalar_spec(cg, val, ty, spec, slen);
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
    const char *fmt   = node->as.print_stmt.fmt;  /* raw: escape seqs not yet decoded */
    usize_t     flen  = node->as.print_stmt.fmt_len;
    node_list_t *args = &node->as.print_stmt.args;
    usize_t arg_idx   = 0;

    /* working buffer for the current literal segment (decoded escapes) */
    heap_t seg_heap = allocate(flen + 1, 1);
    char  *seg      = seg_heap.pointer;
    usize_t seg_len = 0;

#define FLUSH_SEG() do {                                    \
    if (seg_len > 0) {                                      \
        seg[seg_len] = '\0';                                \
        print_emit_cstr(cg, seg);                           \
        seg_len = 0;                                        \
    }                                                       \
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
                print_emit_arg(cg, args->items[arg_idx++], spec, slen);

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
        /* Extract the callee name from the single RHS expression */
        node_t *rhs = values->items[0];
        const char *callee = Null;
        if (rhs->kind == NodeCallExpr)
            callee = rhs->as.call.callee;

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

    /* find enum registry from the first non-wildcard arm */
    enum_reg_t *er = Null;
    for (usize_t i = 0; i < arm_count; i++) {
        node_t *arm = node->as.match_stmt.arms.items[i];
        if (!arm->as.match_arm.is_wildcard && arm->as.match_arm.enum_name) {
            er = find_enum(cg, arm->as.match_arm.enum_name);
            break;
        }
    }

    /* get subject alloca pointer (for payload extraction in tagged enums) */
    LLVMValueRef subject_alloca = Null;
    LLVMValueRef discriminant;

    if (er && er->is_tagged) {
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

        /* find variant discriminant value */
        long disc_val = -1;
        variant_info_t *vi = Null;
        enum_reg_t *arm_er = er ? er : find_enum(cg, arm->as.match_arm.enum_name);
        if (arm_er) {
            for (usize_t j = 0; j < arm_er->variant_count; j++) {
                if (strcmp(arm_er->variants[j].name, arm->as.match_arm.variant_name) == 0) {
                    disc_val = arm_er->variants[j].value;
                    vi = &arm_er->variants[j];
                    break;
                }
            }
        }
        if (disc_val < 0) {
            diag_begin_error("unknown variant '%s'", arm->as.match_arm.variant_name);
            diag_span(DIAG_NODE(arm), True, "");
            diag_note("check the enum definition for valid variants");
            diag_finish();
            continue;
        }

        LLVMBasicBlockRef arm_bb =
            LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "match.arm");
        LLVMAddCase(sw,
            LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                         (unsigned long long)disc_val, 0),
            arm_bb);
        LLVMPositionBuilderAtEnd(cg->builder, arm_bb);

        /* scope: save locals count so binding is invisible after this arm */
        usize_t saved_locals = cg->locals.count;

        /* bind payload value if requested */
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

        push_dtor_scope(cg);
        for (usize_t s = 0; s < arm->as.match_arm.body->as.block.stmts.count; s++)
            gen_stmt(cg, arm->as.match_arm.body->as.block.stmts.items[s]);
        pop_dtor_scope(cg);

        cg->locals.count = saved_locals;

        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, end_bb);
    }

    /* generate wildcard / default arm */
    if (wildcard_arm) {
        LLVMPositionBuilderAtEnd(cg->builder, default_bb);
        push_dtor_scope(cg);
        for (usize_t s = 0; s < wildcard_arm->as.match_arm.body->as.block.stmts.count; s++)
            gen_stmt(cg, wildcard_arm->as.match_arm.body->as.block.stmts.items[s]);
        pop_dtor_scope(cg);
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

static void gen_comptime_assert(cg_t *cg, node_t *node) {
    /* compile-time assertion: evaluate constant expression */
    node_t *expr = node->as.comptime_assert.expr;
    char *message = node->as.comptime_assert.message;

    /* try to evaluate simple constant comparisons */
    if (expr->kind == NodeBinaryExpr && expr->as.binary.op == TokEqEq) {
        node_t *l = expr->as.binary.left;
        node_t *r = expr->as.binary.right;
        if (l->kind == NodeSizeofExpr && r->kind == NodeIntLitExpr) {
            LLVMTypeRef ty = get_llvm_type(cg, l->as.sizeof_expr.type);
            unsigned long long sz = LLVMABISizeOfType(
                LLVMGetModuleDataLayout(cg->module), ty);
            if (sz != (unsigned long long)r->as.int_lit.value) {
                if (message)
                    diag_begin_error("comptime_assert failed: %s (sizeof = %llu, expected %ld)",
                            message, sz, r->as.int_lit.value);
                else
                    diag_begin_error("comptime_assert failed: sizeof = %llu, expected %ld",
                            sz, r->as.int_lit.value);
                diag_span(DIAG_NODE(node), True, "assertion failed here");
                diag_note("the assertion evaluated at compile time and did not hold");
                diag_finish();
            }
            return;
        }
        /* int == int */
        if (l->kind == NodeIntLitExpr && r->kind == NodeIntLitExpr) {
            if (l->as.int_lit.value != r->as.int_lit.value) {
                if (message)
                    diag_begin_error("comptime_assert failed: %s (%ld != %ld)",
                            message, l->as.int_lit.value, r->as.int_lit.value);
                else
                    diag_begin_error("comptime_assert failed: %ld != %ld",
                            l->as.int_lit.value, r->as.int_lit.value);
                diag_span(DIAG_NODE(node), True, "assertion failed here");
                diag_note("the assertion evaluated at compile time and did not hold");
                diag_finish();
            }
            return;
        }
    }
    /* boolean literal */
    if (expr->kind == NodeBoolLitExpr) {
        if (!expr->as.bool_lit.value) {
            if (message)
                diag_begin_error("comptime_assert failed: %s", message);
            else
                diag_begin_error("comptime_assert failed");
            diag_span(DIAG_NODE(node), True, "assertion failed here");
            diag_note("the assertion evaluated at compile time and did not hold");
            diag_finish();
        }
        return;
    }
    diag_begin_error("comptime_assert: expression too complex to evaluate at compile time");
    diag_span(DIAG_NODE(node), True, "unsupported expression");
    diag_note("only constants, sizeof, and arithmetic are supported in comptime_assert");
    diag_finish();
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
        case NodeBreakStmt:
            if (cg->break_target)
                LLVMBuildBr(cg->builder, cg->break_target);
            else {
                diag_begin_error("'break' used outside of a loop or switch");
                diag_span(DIAG_NODE(node), True, "break here");
                diag_note("'break' can only appear inside for, while, do-while, inf, or switch");
                diag_finish();
            }
            break;
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
        default:
            diag_begin_error("unexpected statement kind %d", node->kind);
            diag_finish();
            break;
    }
}

static void gen_block(cg_t *cg, node_t *node) {
    usize_t old_count = cg->locals.count;
    push_dtor_scope(cg);
    for (usize_t i = 0; i < node->as.block.stmts.count; i++)
        gen_stmt(cg, node->as.block.stmts.items[i]);
    /* emit unused variable warnings for locals added in this block */
    for (usize_t i = old_count; i < cg->locals.count; i++) {
        symbol_t *entry = &cg->locals.entries[i];
        if (!entry->used && entry->name && entry->name[0] != '_') {
            diag_begin_warning("unused variable '%s'", entry->name);
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
