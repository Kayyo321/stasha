/* ── statements ── */

/* Returns True if type_info is a primitive (non-struct, non-enum) that can be heap-allocated */
static boolean_t is_primitive_type(type_info_t ti) {
    if (ti.is_pointer || ti.base == TypeVoid) return False;
    if (ti.base == TypeUser)   return False;
    if (ti.base == TypeFnPtr)  return False; /* function pointers are not heap primitives */
    return True;
}

static void gen_local_var(cg_t *cg, node_t *node) {
    /* blank identifier: silently skip — the value will be discarded */
    if (node->as.var_decl.name && strcmp(node->as.var_decl.name, "_") == 0)
        return;

    type_info_t ti = resolve_alias(cg, node->as.var_decl.type);
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
                log_err("line %lu: undefined constant '%s' used as array size",
                        node->line, node->as.var_decl.array_size_name);
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
                if (src && src->storage == StorageStack)
                    log_err("line %lu: cannot assign stack address to heap pointer",
                            node->line);
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
                if (ptr_domain == StorageHeap && src->storage == StorageStack)
                    log_err("line %lu: cannot assign stack address to heap pointer",
                            node->line);
                else if (ptr_domain == StorageStack && src->storage == StorageHeap)
                    log_err("line %lu: cannot assign heap address to stack pointer",
                            node->line);
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

    if (node->as.for_stmt.init->kind == NodeVarDecl)
        gen_local_var(cg, node->as.for_stmt.init);
    else
        gen_expr(cg, node->as.for_stmt.init);

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
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx)) {
        if (llvm_is_float(LLVMTypeOf(cond)))
            cond = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond,
                                 LLVMConstReal(LLVMTypeOf(cond), 0.0), "tobool");
        else
            cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                                 LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
    }
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
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx))
        cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                             LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
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
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx))
        cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                             LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
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
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx)) {
        if (llvm_is_float(LLVMTypeOf(cond)))
            cond = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond,
                                 LLVMConstReal(LLVMTypeOf(cond), 0.0), "tobool");
        else
            cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                                 LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
    }

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

static void gen_debug(cg_t *cg, node_t *node) {
    LLVMValueRef value = gen_expr(cg, node->as.debug_stmt.value);
    LLVMTypeRef vtype = LLVMTypeOf(value);

    const char *fmt;
    if (llvm_is_ptr(vtype))
        fmt = "%s\n";
    else if (llvm_is_float(vtype))
        fmt = "%f\n";
    else if (vtype == LLVMInt64TypeInContext(cg->ctx))
        fmt = "%lld\n";
    else if (vtype == LLVMInt8TypeInContext(cg->ctx))
        fmt = "%c\n";
    else if (vtype == LLVMInt1TypeInContext(cg->ctx))
        fmt = "%d\n";
    else
        fmt = "%d\n";

    /* promote small ints for printf */
    if (llvm_is_int(vtype) && vtype != LLVMInt32TypeInContext(cg->ctx)
        && vtype != LLVMInt64TypeInContext(cg->ctx)
        && vtype != LLVMInt8TypeInContext(cg->ctx)) {
        value = LLVMBuildSExt(cg->builder, value, LLVMInt32TypeInContext(cg->ctx), "dbgext");
    }
    if (llvm_is_float(vtype) && vtype != LLVMDoubleTypeInContext(cg->ctx)) {
        value = LLVMBuildFPExt(cg->builder, value, LLVMDoubleTypeInContext(cg->ctx), "dbgfext");
    }

    LLVMValueRef fmt_str = LLVMBuildGlobalStringPtr(cg->builder, fmt, "dbg_fmt");
    LLVMValueRef args[2] = { fmt_str, value };
    LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 2, "");
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
            log_err("line %lu: 'let' binding requires a multi-return function call",
                    node->line);
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
            log_err("line %lu: unknown variant '%s'",
                    arm->line, arm->as.match_arm.variant_name);
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
                    log_err("comptime_assert failed: %s (sizeof = %llu, expected %ld)",
                            message, sz, r->as.int_lit.value);
                else
                    log_err("comptime_assert failed: sizeof = %llu, expected %ld",
                            sz, r->as.int_lit.value);
            }
            return;
        }
        /* int == int */
        if (l->kind == NodeIntLitExpr && r->kind == NodeIntLitExpr) {
            if (l->as.int_lit.value != r->as.int_lit.value) {
                if (message)
                    log_err("comptime_assert failed: %s (%ld != %ld)",
                            message, l->as.int_lit.value, r->as.int_lit.value);
                else
                    log_err("comptime_assert failed: %ld != %ld",
                            l->as.int_lit.value, r->as.int_lit.value);
            }
            return;
        }
    }
    /* boolean literal */
    if (expr->kind == NodeBoolLitExpr) {
        if (!expr->as.bool_lit.value) {
            if (message)
                log_err("comptime_assert failed: %s", message);
            else
                log_err("comptime_assert failed");
        }
        return;
    }
    log_err("comptime_assert: expression too complex to evaluate at compile time");
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
        case NodeDebugStmt:    gen_debug(cg, node); break;
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
            else
                log_err("break outside of loop");
            break;
        case NodeContinueStmt:
            if (cg->continue_target)
                LLVMBuildBr(cg->builder, cg->continue_target);
            else
                log_err("continue outside of loop");
            break;
        case NodeDeferStmt:
            add_deferred_stmt(cg, node->as.defer_stmt.body);
            break;
        case NodeBlock:
            gen_block(cg, node);
            break;
        default:
            log_err("unexpected statement kind %d", node->kind);
            break;
    }
}

static void gen_block(cg_t *cg, node_t *node) {
    push_dtor_scope(cg);
    for (usize_t i = 0; i < node->as.block.stmts.count; i++)
        gen_stmt(cg, node->as.block.stmts.items[i]);
    pop_dtor_scope(cg);
}
