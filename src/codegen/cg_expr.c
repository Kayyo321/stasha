/* ── expressions ── */

static LLVMValueRef gen_int_lit(cg_t *cg, node_t *node) {
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                        (unsigned long long)node->as.int_lit.value, 1);
}

static LLVMValueRef gen_float_lit(cg_t *cg, node_t *node) {
    return LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), node->as.float_lit.value);
}

static LLVMValueRef gen_bool_lit(cg_t *cg, node_t *node) {
    return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), node->as.bool_lit.value, 0);
}

static LLVMValueRef gen_char_lit(cg_t *cg, node_t *node) {
    return LLVMConstInt(LLVMInt8TypeInContext(cg->ctx),
                        (unsigned char)node->as.char_lit.value, 0);
}

static LLVMValueRef gen_str_lit(cg_t *cg, node_t *node) {
    return LLVMBuildGlobalStringPtr(cg->builder, node->as.str_lit.value, "str");
}

static LLVMValueRef gen_ident(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.ident.name);
    if (!sym) {
        log_err("undefined variable '%s'", node->as.ident.name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    if (sym->flags & SymHeapVar) {
        /* heap primitive: alloca holds the malloc ptr; do double-load */
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        return LLVMBuildLoad2(cg->builder, sym->type, heap_ptr, node->as.ident.name);
    }
    /* arrays decay to a pointer (C semantics): return the alloca directly */
    if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind)
        return sym->value;
    LLVMValueRef load = LLVMBuildLoad2(cg->builder, sym->type, sym->value,
                                        node->as.ident.name);
    if (sym->flags & SymAtomic)
        LLVMSetOrdering(load, LLVMAtomicOrderingSequentiallyConsistent);
    if (sym->flags & SymVolatile)
        LLVMSetVolatile(load, 1);
    return load;
}

static LLVMValueRef gen_binary(cg_t *cg, node_t *node) {
    /* short-circuit logical AND */
    if (node->as.binary.op == TokAmpAmp) {
        LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "and.rhs");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "and.merge");

        LLVMValueRef lhs = gen_expr(cg, node->as.binary.left);
        LLVMBasicBlockRef lhs_bb = LLVMGetInsertBlock(cg->builder);
        if (LLVMTypeOf(lhs) != LLVMInt1TypeInContext(cg->ctx))
            lhs = LLVMBuildICmp(cg->builder, LLVMIntNE, lhs,
                                LLVMConstInt(LLVMTypeOf(lhs), 0, 0), "tobool");
        LLVMBuildCondBr(cg->builder, lhs, rhs_bb, merge_bb);

        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef rhs = gen_expr(cg, node->as.binary.right);
        if (LLVMTypeOf(rhs) != LLVMInt1TypeInContext(cg->ctx))
            rhs = LLVMBuildICmp(cg->builder, LLVMIntNE, rhs,
                                LLVMConstInt(LLVMTypeOf(rhs), 0, 0), "tobool");
        LLVMBuildBr(cg->builder, merge_bb);
        rhs_bb = LLVMGetInsertBlock(cg->builder);

        LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->ctx), "and");
        LLVMValueRef false_val = LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0);
        LLVMValueRef vals[2] = { false_val, rhs };
        LLVMBasicBlockRef bbs[2] = { lhs_bb, rhs_bb };
        LLVMAddIncoming(phi, vals, bbs, 2);
        return phi;
    }

    /* short-circuit logical OR */
    if (node->as.binary.op == TokPipePipe) {
        LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "or.rhs");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "or.merge");

        LLVMValueRef lhs = gen_expr(cg, node->as.binary.left);
        LLVMBasicBlockRef lhs_bb = LLVMGetInsertBlock(cg->builder);
        if (LLVMTypeOf(lhs) != LLVMInt1TypeInContext(cg->ctx))
            lhs = LLVMBuildICmp(cg->builder, LLVMIntNE, lhs,
                                LLVMConstInt(LLVMTypeOf(lhs), 0, 0), "tobool");
        LLVMBuildCondBr(cg->builder, lhs, merge_bb, rhs_bb);

        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef rhs = gen_expr(cg, node->as.binary.right);
        if (LLVMTypeOf(rhs) != LLVMInt1TypeInContext(cg->ctx))
            rhs = LLVMBuildICmp(cg->builder, LLVMIntNE, rhs,
                                LLVMConstInt(LLVMTypeOf(rhs), 0, 0), "tobool");
        LLVMBuildBr(cg->builder, merge_bb);
        rhs_bb = LLVMGetInsertBlock(cg->builder);

        LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->ctx), "or");
        LLVMValueRef true_val = LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0);
        LLVMValueRef vals[2] = { true_val, rhs };
        LLVMBasicBlockRef bbs[2] = { lhs_bb, rhs_bb };
        LLVMAddIncoming(phi, vals, bbs, 2);
        return phi;
    }

    /* pointer arithmetic bounds check */
    check_ptr_arith_bounds(cg, node);

    /* error == nil / error != nil: extract has_error flag */
    if ((node->as.binary.op == TokEqEq || node->as.binary.op == TokBangEq)
        && (node->as.binary.right->kind == NodeNilExpr
            || node->as.binary.left->kind == NodeNilExpr)) {
        /* figure out which side is the error value */
        node_t *err_node = node->as.binary.left;
        if (err_node->kind == NodeNilExpr) err_node = node->as.binary.right;
        if (err_node->kind == NodeIdentExpr) {
            symbol_t *esym = cg_lookup(cg, err_node->as.ident.name);
            if (esym && esym->stype.base == TypeError) {
                LLVMValueRef err_val = gen_expr(cg, err_node);
                LLVMValueRef has_err = LLVMBuildExtractValue(cg->builder, err_val, 0, "has_err");
                if (node->as.binary.op == TokEqEq)
                    return LLVMBuildNot(cg->builder, has_err, "is_nil");
                else
                    return has_err;
            }
        }
    }

    LLVMValueRef left = gen_expr(cg, node->as.binary.left);
    LLVMValueRef right = gen_expr(cg, node->as.binary.right);

    LLVMTypeRef lt = LLVMTypeOf(left);
    LLVMTypeRef rt = LLVMTypeOf(right);

    /* promote to matching types */
    if (lt != rt) {
        if (llvm_is_float(lt) || llvm_is_float(rt)) {
            LLVMTypeRef wider = LLVMDoubleTypeInContext(cg->ctx);
            left = coerce_int(cg, left, wider);
            right = coerce_int(cg, right, wider);
        } else if (llvm_is_int(lt) && llvm_is_int(rt)) {
            unsigned lw = LLVMGetIntTypeWidth(lt);
            unsigned rw = LLVMGetIntTypeWidth(rt);
            LLVMTypeRef wider = lw >= rw ? lt : rt;
            left = coerce_int(cg, left, wider);
            right = coerce_int(cg, right, wider);
        }
        lt = LLVMTypeOf(left);
    }

    boolean_t is_fp = llvm_is_float(lt);

    switch (node->as.binary.op) {
        case TokPlus:
            return is_fp ? LLVMBuildFAdd(cg->builder, left, right, "fadd")
                         : LLVMBuildAdd(cg->builder, left, right, "add");
        case TokMinus:
            return is_fp ? LLVMBuildFSub(cg->builder, left, right, "fsub")
                         : LLVMBuildSub(cg->builder, left, right, "sub");
        case TokStar:
            return is_fp ? LLVMBuildFMul(cg->builder, left, right, "fmul")
                         : LLVMBuildMul(cg->builder, left, right, "mul");
        case TokSlash:
            if (is_fp) return LLVMBuildFDiv(cg->builder, left, right, "fdiv");
            return LLVMBuildSDiv(cg->builder, left, right, "div");
        case TokPercent:
            if (is_fp) return LLVMBuildFRem(cg->builder, left, right, "fmod");
            return LLVMBuildSRem(cg->builder, left, right, "mod");

        case TokLt:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOLT, left, right, "flt")
                         : LLVMBuildICmp(cg->builder, LLVMIntSLT, left, right, "lt");
        case TokGt:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOGT, left, right, "fgt")
                         : LLVMBuildICmp(cg->builder, LLVMIntSGT, left, right, "gt");
        case TokLtEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOLE, left, right, "fle")
                         : LLVMBuildICmp(cg->builder, LLVMIntSLE, left, right, "le");
        case TokGtEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOGE, left, right, "fge")
                         : LLVMBuildICmp(cg->builder, LLVMIntSGE, left, right, "ge");
        case TokEqEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOEQ, left, right, "feq")
                         : LLVMBuildICmp(cg->builder, LLVMIntEQ, left, right, "eq");
        case TokBangEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealONE, left, right, "fne")
                         : LLVMBuildICmp(cg->builder, LLVMIntNE, left, right, "ne");

        /* wrapping arithmetic (+% -% *%) — LLVM integer add/sub/mul already wraps */
        case TokPlusPercent:
            return LLVMBuildAdd(cg->builder, left, right, "wadd");
        case TokMinusPercent:
            return LLVMBuildSub(cg->builder, left, right, "wsub");
        case TokStarPercent:
            return LLVMBuildMul(cg->builder, left, right, "wmul");

        /* trapping arithmetic (+! -! *!) — overflow intrinsics + trap */
        case TokPlusBang: case TokMinusBang: case TokStarBang: {
            const char *intrinsic;
            if (node->as.binary.op == TokPlusBang)      intrinsic = "llvm.sadd.with.overflow.i32";
            else if (node->as.binary.op == TokMinusBang) intrinsic = "llvm.ssub.with.overflow.i32";
            else                                          intrinsic = "llvm.smul.with.overflow.i32";

            LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);
            left  = coerce_int(cg, left,  i32);
            right = coerce_int(cg, right, i32);

            /* build { i32, i1 } return type */
            LLVMTypeRef ret_fields[2] = { i32, LLVMInt1TypeInContext(cg->ctx) };
            LLVMTypeRef ret_type = LLVMStructTypeInContext(cg->ctx, ret_fields, 2, 0);
            LLVMTypeRef param_types[2] = { i32, i32 };
            LLVMTypeRef fn_type = LLVMFunctionType(ret_type, param_types, 2, 0);

            LLVMValueRef fn = LLVMGetNamedFunction(cg->module, intrinsic);
            if (!fn) fn = LLVMAddFunction(cg->module, intrinsic, fn_type);

            LLVMValueRef args[2] = { left, right };
            LLVMValueRef result = LLVMBuildCall2(cg->builder, fn_type, fn, args, 2, "ov");
            LLVMValueRef val = LLVMBuildExtractValue(cg->builder, result, 0, "ov.val");
            LLVMValueRef overflowed = LLVMBuildExtractValue(cg->builder, result, 1, "ov.flag");

            /* branch: if overflowed, call llvm.trap; else continue */
            LLVMBasicBlockRef trap_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "ov.trap");
            LLVMBasicBlockRef ok_bb   = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "ov.ok");
            LLVMBuildCondBr(cg->builder, overflowed, trap_bb, ok_bb);

            LLVMPositionBuilderAtEnd(cg->builder, trap_bb);
            {
                LLVMTypeRef trap_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), Null, 0, 0);
                LLVMValueRef trap_fn = LLVMGetNamedFunction(cg->module, "llvm.trap");
                if (!trap_fn) trap_fn = LLVMAddFunction(cg->module, "llvm.trap", trap_type);
                LLVMBuildCall2(cg->builder, trap_type, trap_fn, Null, 0, "");
                LLVMBuildUnreachable(cg->builder);
            }
            LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
            return val;
        }

        /* bitwise */
        case TokAmp:   return LLVMBuildAnd(cg->builder, left, right, "and");
        case TokPipe:  return LLVMBuildOr(cg->builder, left, right, "or");
        case TokCaret: return LLVMBuildXor(cg->builder, left, right, "xor");
        case TokLtLt:  return LLVMBuildShl(cg->builder, left, right, "shl");
        case TokGtGt:  return LLVMBuildAShr(cg->builder, left, right, "shr");

        default:
            log_err("unknown binary operator");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

static LLVMValueRef gen_unary_prefix(cg_t *cg, node_t *node) {
    if (node->as.unary.op == TokPlusPlus || node->as.unary.op == TokMinusMinus) {
        node_t *operand = node->as.unary.operand;
        if (operand->kind != NodeIdentExpr) {
            log_err("prefix ++/-- requires an identifier");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", operand->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        LLVMValueRef store_ptr;
        LLVMValueRef val;
        if (sym->flags & SymHeapVar) {
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            store_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
            val = LLVMBuildLoad2(cg->builder, sym->type, store_ptr, "");
        } else {
            store_ptr = sym->value;
            val = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "");
        }
        LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
        LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
            ? LLVMBuildAdd(cg->builder, val, one, "inc")
            : LLVMBuildSub(cg->builder, val, one, "dec");
        LLVMBuildStore(cg->builder, result, store_ptr);
        return result;
    }
    if (node->as.unary.op == TokMinus) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        if (llvm_is_float(LLVMTypeOf(operand)))
            return LLVMBuildFNeg(cg->builder, operand, "fneg");
        return LLVMBuildNeg(cg->builder, operand, "neg");
    }
    if (node->as.unary.op == TokBang) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        if (LLVMTypeOf(operand) != LLVMInt1TypeInContext(cg->ctx))
            operand = LLVMBuildICmp(cg->builder, LLVMIntNE, operand,
                                    LLVMConstInt(LLVMTypeOf(operand), 0, 0), "tobool");
        return LLVMBuildNot(cg->builder, operand, "not");
    }
    if (node->as.unary.op == TokTilde) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        return LLVMBuildNot(cg->builder, operand, "bnot");
    }
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_unary_postfix(cg_t *cg, node_t *node) {
    node_t *operand = node->as.unary.operand;
    if (operand->kind != NodeIdentExpr) {
        log_err("postfix ++/-- requires an identifier");
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
    if (!sym) {
        log_err("undefined variable '%s'", operand->as.ident.name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    LLVMValueRef store_ptr;
    LLVMValueRef val;
    if (sym->flags & SymHeapVar) {
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        store_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        val = LLVMBuildLoad2(cg->builder, sym->type, store_ptr, "");
    } else {
        store_ptr = sym->value;
        val = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "");
    }
    LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
    LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
        ? LLVMBuildAdd(cg->builder, val, one, "inc")
        : LLVMBuildSub(cg->builder, val, one, "dec");
    LLVMBuildStore(cg->builder, result, store_ptr);
    return val;
}

static LLVMValueRef gen_call(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.call.callee);
    if (!sym) {
        log_err("undefined function or function pointer '%s'", node->as.call.callee);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    usize_t argc = node->as.call.args.count;
    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (argc > 0) {
        args_heap = allocate(argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        for (usize_t i = 0; i < argc; i++)
            args[i] = gen_expr(cg, node->as.call.args.items[i]);
    }

    LLVMTypeRef fn_type;
    LLVMValueRef fn_val;

    if (sym->stype.base == TypeFnPtr && sym->stype.fn_ptr_desc) {
        /* ── indirect call through a domain-tagged function pointer variable ── */
        fn_ptr_desc_t *desc = sym->stype.fn_ptr_desc;

        /* domain check: actual argument storage must match declared parameter domain */
        for (usize_t i = 0; i < argc && i < desc->param_count; i++) {
            node_t *arg_node = node->as.call.args.items[i];
            if (arg_node->kind == NodeIdentExpr) {
                symbol_t *asym = cg_lookup(cg, arg_node->as.ident.name);
                if (asym) {
                    storage_t declared = desc->params[i].storage;
                    storage_t actual   = asym->storage;
                    if (declared != StorageDefault && actual != StorageDefault
                            && declared != actual) {
                        const char *exp_s = (declared == StorageStack) ? "stack" : "heap";
                        const char *got_s = (actual   == StorageStack) ? "stack" : "heap";
                        log_err("line %lu: argument %lu to function pointer '%s' has "
                                "wrong storage domain (expected %s, got %s)",
                                node->line, (unsigned long)(i + 1),
                                sym->name, exp_s, got_s);
                    }
                }
            }
        }

        /* build the LLVM function type from the descriptor */
        fn_type = build_fn_ptr_llvm_type(cg, desc);

        /* load the actual function pointer value from the variable's alloca */
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        fn_val = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "fnptr");
    } else {
        /* ── direct call to a named function ── */
        fn_type = LLVMGlobalGetValueType(sym->value);
        fn_val  = sym->value;
    }

    /* coerce arguments to the declared parameter types (e.g. i32 literal → f32) */
    unsigned n_params = LLVMCountParamTypes(fn_type);
    if (argc > 0 && n_params > 0) {
        heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
        LLVMTypeRef *param_types = pt_heap.pointer;
        LLVMGetParamTypes(fn_type, param_types);
        for (usize_t i = 0; i < argc && i < (usize_t)n_params; i++)
            args[i] = coerce_int(cg, args[i], param_types[i]);
        deallocate(pt_heap);
    }

    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_val,
                                       args, (unsigned)argc, "");
    if (argc > 0) deallocate(args_heap);
    return ret;
}

static LLVMValueRef gen_method_call(cg_t *cg, node_t *node) {
    node_t *obj = node->as.method_call.object;
    char *method = node->as.method_call.method;

    /* tagged enum variant construction: Enum.Variant(payload) */
    if (obj->kind == NodeIdentExpr) {
        enum_reg_t *er = find_enum(cg, obj->as.ident.name);
        if (er && er->is_tagged) {
            for (usize_t i = 0; i < er->variant_count; i++) {
                if (strcmp(er->variants[i].name, method) == 0) {
                    LLVMValueRef tmp = alloc_in_entry(cg, er->llvm_type, "enum_tmp");

                    /* store discriminant */
                    LLVMValueRef disc_ptr = LLVMBuildStructGEP2(
                        cg->builder, er->llvm_type, tmp, 0, "disc_ptr");
                    LLVMBuildStore(cg->builder,
                        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned long long)i, 0),
                        disc_ptr);

                    /* store payload if variant carries one */
                    if (er->variants[i].has_payload &&
                            node->as.method_call.args.count > 0) {
                        LLVMValueRef payload_val =
                            gen_expr(cg, node->as.method_call.args.items[0]);
                        LLVMTypeRef payload_lltype =
                            get_llvm_type(cg, er->variants[i].payload_type);
                        payload_val = coerce_int(cg, payload_val, payload_lltype);
                        LLVMValueRef payload_ptr = LLVMBuildStructGEP2(
                            cg->builder, er->llvm_type, tmp, 1, "payload_ptr");
                        LLVMBuildStore(cg->builder, payload_val, payload_ptr);
                    }

                    return LLVMBuildLoad2(cg->builder, er->llvm_type, tmp, "enum_val");
                }
            }
        }
    }

    /* check if object is a lib alias: alias.func(args) */
    if (obj->kind == NodeIdentExpr) {
        const char *header = find_lib_alias(cg, obj->as.ident.name);
        if (header) {
            /* generate args first so we can inspect their types */
            usize_t argc = node->as.method_call.args.count;
            heap_t args_heap = NullHeap;
            LLVMValueRef *args = Null;
            if (argc > 0) {
                args_heap = allocate(argc, sizeof(LLVMValueRef));
                args = args_heap.pointer;
                for (usize_t i = 0; i < argc; i++)
                    args[i] = gen_expr(cg, node->as.method_call.args.items[i]);
            }

            /* auto-declare C function from actual arg types */
            symbol_t *fn_sym = cg_lookup(cg, method);
            if (!fn_sym) {
                LLVMTypeRef ret_type;
                LLVMTypeRef *param_types = Null;
                heap_t ptypes_heap = NullHeap;
                unsigned param_count = 0;
                boolean_t is_varargs = False;

                if (argc > 0 &&
                    LLVMGetTypeKind(LLVMTypeOf(args[0])) == LLVMPointerTypeKind) {
                    /* first arg is a pointer → printf-like varargs */
                    LLVMTypeRef ptr_type = LLVMPointerTypeInContext(cg->ctx, 0);
                    param_types = &ptr_type;
                    param_count = 1;
                    ret_type = LLVMInt32TypeInContext(cg->ctx);
                    is_varargs = True;
                } else if (argc > 0) {
                    /* build exact signature from arg types */
                    ptypes_heap = allocate(argc, sizeof(LLVMTypeRef));
                    param_types = ptypes_heap.pointer;
                    for (usize_t i = 0; i < argc; i++)
                        param_types[i] = LLVMTypeOf(args[i]);
                    param_count = (unsigned)argc;
                    ret_type = LLVMTypeOf(args[0]);
                } else {
                    ret_type = LLVMInt32TypeInContext(cg->ctx);
                }

                LLVMTypeRef ftype = LLVMFunctionType(ret_type, param_types,
                                                      param_count, is_varargs);
                LLVMValueRef fn = LLVMAddFunction(cg->module, method, ftype);
                type_info_t dummy = {TypeI32, Null, False, PtrNone, Null};
                symtab_add(&cg->globals, method, fn, Null, dummy, False);
                fn_sym = cg_lookup(cg, method);
                if (ptypes_heap.pointer) deallocate(ptypes_heap);
            }

            LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
            LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                               args, (unsigned)argc, "");
            if (argc > 0) deallocate(args_heap);
            return ret;
        }

        /* check if it's a static method call: Type.method(args) */
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s.%s", obj->as.ident.name, method);
        symbol_t *fn_sym = cg_lookup(cg, mangled);
        if (fn_sym) {
            usize_t argc = node->as.method_call.args.count;
            heap_t args_heap = NullHeap;
            LLVMValueRef *args = Null;
            if (argc > 0) {
                args_heap = allocate(argc, sizeof(LLVMValueRef));
                args = args_heap.pointer;
                for (usize_t i = 0; i < argc; i++)
                    args[i] = gen_expr(cg, node->as.method_call.args.items[i]);
            }
            LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
            LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                               args, (unsigned)argc, "");
            if (argc > 0) deallocate(args_heap);
            return ret;
        }
    }

    /* instance method call: obj.method(args) — pass &obj as first arg */
    if (obj->kind == NodeIdentExpr) {
        symbol_t *obj_sym = cg_lookup(cg, obj->as.ident.name);
        if (obj_sym && obj_sym->stype.base == TypeUser && obj_sym->stype.user_name) {
            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s.%s", obj_sym->stype.user_name, method);
            symbol_t *fn_sym = cg_lookup(cg, mangled);
            if (fn_sym) {
                usize_t argc = node->as.method_call.args.count + 1;
                heap_t args_heap = allocate(argc, sizeof(LLVMValueRef));
                LLVMValueRef *args = args_heap.pointer;
                args[0] = obj_sym->value; /* pass pointer to struct */
                for (usize_t i = 0; i < node->as.method_call.args.count; i++)
                    args[i + 1] = gen_expr(cg, node->as.method_call.args.items[i]);
                LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
                LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                                   args, (unsigned)argc, "");
                deallocate(args_heap);
                return ret;
            }
        }
    }

    log_err("cannot resolve method call '%s'", method);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_parallel_call(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.parallel_call.callee);
    if (!sym) {
        log_err("undefined function '%s'", node->as.parallel_call.callee);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    usize_t argc = node->as.parallel_call.args.count;
    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (argc > 0) {
        args_heap = allocate(argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        for (usize_t i = 0; i < argc; i++)
            args[i] = gen_expr(cg, node->as.parallel_call.args.items[i]);
    }
    LLVMTypeRef fn_type = LLVMGlobalGetValueType(sym->value);
    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, sym->value,
                                       args, (unsigned)argc, "");
    if (argc > 0) deallocate(args_heap);
    return ret;
}

static LLVMValueRef gen_compound_assign(cg_t *cg, node_t *node) {
    node_t *target = node->as.compound_assign.target;

    /* get the storage location */
    symbol_t *sym = Null;
    LLVMValueRef store_ptr = Null;
    LLVMTypeRef store_type = Null;
    boolean_t atomic = False;

    if (target->kind == NodeIdentExpr) {
        sym = cg_lookup(cg, target->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", target->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        if (sym->flags & SymHeapVar) {
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            store_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        } else {
            store_ptr = sym->value;
        }
        store_type = sym->type;
        atomic = (sym->flags & SymAtomic) != 0;
    } else if (target->kind == NodeIndexExpr) {
        /* arr[i] compound assign */
        node_t *obj = target->as.index_expr.object;
        node_t *idx = target->as.index_expr.index;
        if (obj->kind == NodeIdentExpr) {
            sym = cg_lookup(cg, obj->as.ident.name);
            if (!sym) {
                log_err("undefined variable '%s'", obj->as.ident.name);
                return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
            LLVMValueRef index_val = gen_expr(cg, idx);
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
            LLVMValueRef indices[2] = { zero, index_val };
            LLVMTypeRef elem_type = LLVMGetElementType(sym->type);
            store_ptr = LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "idx");
            store_type = elem_type;
        }
    } else {
        log_err("compound assignment target must be assignable");
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    if (!store_ptr) return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);

    LLVMValueRef rhs = gen_expr(cg, node->as.compound_assign.value);
    rhs = coerce_int(cg, rhs, store_type);

    if (atomic && (node->as.compound_assign.op == TokPlusEq
                || node->as.compound_assign.op == TokMinusEq)) {
        LLVMAtomicRMWBinOp op = (node->as.compound_assign.op == TokPlusEq)
            ? LLVMAtomicRMWBinOpAdd : LLVMAtomicRMWBinOpSub;
        return LLVMBuildAtomicRMW(cg->builder, op, store_ptr, rhs,
                                   LLVMAtomicOrderingSequentiallyConsistent, 0);
    }

    LLVMValueRef lhs = LLVMBuildLoad2(cg->builder, store_type, store_ptr, "");
    boolean_t is_fp = llvm_is_float(store_type);
    LLVMValueRef result;

    switch (node->as.compound_assign.op) {
        case TokPlusEq:
            result = is_fp ? LLVMBuildFAdd(cg->builder, lhs, rhs, "fadd")
                           : LLVMBuildAdd(cg->builder, lhs, rhs, "add"); break;
        case TokMinusEq:
            result = is_fp ? LLVMBuildFSub(cg->builder, lhs, rhs, "fsub")
                           : LLVMBuildSub(cg->builder, lhs, rhs, "sub"); break;
        case TokStarEq:
            result = is_fp ? LLVMBuildFMul(cg->builder, lhs, rhs, "fmul")
                           : LLVMBuildMul(cg->builder, lhs, rhs, "mul"); break;
        case TokSlashEq:
            result = is_fp ? LLVMBuildFDiv(cg->builder, lhs, rhs, "fdiv")
                           : LLVMBuildSDiv(cg->builder, lhs, rhs, "div"); break;
        case TokPercentEq:  result = LLVMBuildSRem(cg->builder, lhs, rhs, "mod"); break;
        case TokAmpEq:      result = LLVMBuildAnd(cg->builder, lhs, rhs, "and"); break;
        case TokPipeEq:     result = LLVMBuildOr(cg->builder, lhs, rhs, "or"); break;
        case TokCaretEq:    result = LLVMBuildXor(cg->builder, lhs, rhs, "xor"); break;
        case TokLtLtEq:     result = LLVMBuildShl(cg->builder, lhs, rhs, "shl"); break;
        case TokGtGtEq:     result = LLVMBuildAShr(cg->builder, lhs, rhs, "shr"); break;
        default: result = lhs; break;
    }
    LLVMBuildStore(cg->builder, result, store_ptr);
    return result;
}

static LLVMValueRef gen_assign(cg_t *cg, node_t *node) {
    node_t *target = node->as.assign.target;

    if (target->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, target->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", target->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }

        /* cross-domain check when assigning address-of */
        if (node->as.assign.value->kind == NodeAddrOf && sym->stype.is_pointer) {
            node_t *addr_op = node->as.assign.value->as.addr_of.operand;
            if (addr_op->kind == NodeIdentExpr) {
                symbol_t *src = cg_lookup(cg, addr_op->as.ident.name);
                if (src) {
                    if (sym->storage == StorageHeap && src->storage == StorageStack)
                        log_err("line %lu: cannot assign stack address to heap pointer",
                                node->line);
                    else if (sym->storage == StorageStack && src->storage == StorageHeap)
                        log_err("line %lu: cannot assign heap address to stack pointer",
                                node->line);
                }
            }
        }

        /* pointer safety checks on assignment */
        check_const_addr_of(cg, node->as.assign.value, sym->stype, node->line);
        check_permission_widening(cg, node->as.assign.value, sym->stype, node->line);
        check_pointer_lifetime(cg, node->as.assign.value, sym->scope_depth, node->line);

        /* track nil state */
        if (node->as.assign.value->kind == NodeNilExpr)
            sym->flags |= SymNil;
        else
            sym->flags &= ~SymNil;

        LLVMValueRef rhs = gen_expr(cg, node->as.assign.value);
        rhs = coerce_int(cg, rhs, sym->type);
        if (sym->flags & SymHeapVar) {
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty,
                                                    sym->value, "hptr");
            LLVMBuildStore(cg->builder, rhs, heap_ptr);
        } else {
            LLVMBuildStore(cg->builder, rhs, sym->value);
        }
        return rhs;
    }

    LLVMValueRef rhs = gen_expr(cg, node->as.assign.value);

    if (target->kind == NodeIndexExpr) {
        node_t *obj = target->as.index_expr.object;
        node_t *idx = target->as.index_expr.index;
        LLVMValueRef index_val = gen_expr(cg, idx);

        if (obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
            if (!sym) {
                log_err("undefined variable '%s'", obj->as.ident.name);
                return rhs;
            }
            /* check if it's an array or pointer */
            if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind) {
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
                LLVMValueRef indices[2] = { zero, index_val };
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "idx");
                LLVMTypeRef elem_type = LLVMGetElementType(sym->type);
                rhs = coerce_int(cg, rhs, elem_type);
                LLVMBuildStore(cg->builder, rhs, gep);
            } else if (llvm_is_ptr(sym->type)) {
                /* pointer indexing: load pointer, then GEP with element type */
                LLVMValueRef ptr = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "ptr");
                type_info_t elem_ti = sym->stype;
                elem_ti.is_pointer = False;
                LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidx");
                rhs = coerce_int(cg, rhs, elem_ty);
                LLVMBuildStore(cg->builder, rhs, gep);
            }
            return rhs;
        }

        if (obj->kind == NodeMemberExpr) {
            /* obj.field[idx] = rhs: e.g. b.data[i] = val
               Also handles nested: obj.field1.field2[idx] = rhs */
            node_t *mobj = obj->as.member_expr.object;
            char   *mfield = obj->as.member_expr.field;

            /* resolve the base alloca and struct for the member chain */
            LLVMValueRef base_alloca = Null;
            struct_reg_t *base_sr = Null;

            if (mobj->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, mobj->as.ident.name);
                if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                    base_sr = find_struct(cg, sym->stype.user_name);
                    base_alloca = sym->value;
                }
            } else if (mobj->kind == NodeMemberExpr) {
                /* one extra level: obj.field1.field2[idx] */
                node_t *outer_obj = mobj->as.member_expr.object;
                char   *outer_field = mobj->as.member_expr.field;
                if (outer_obj->kind == NodeIdentExpr) {
                    symbol_t *sym = cg_lookup(cg, outer_obj->as.ident.name);
                    if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                        struct_reg_t *outer_sr = find_struct(cg, sym->stype.user_name);
                        if (outer_sr) {
                            for (usize_t fi = 0; fi < outer_sr->field_count; fi++) {
                                if (strcmp(outer_sr->fields[fi].name, outer_field) != 0) continue;
                                base_alloca = LLVMBuildStructGEP2(cg->builder, outer_sr->llvm_type,
                                    sym->value, (unsigned)outer_sr->fields[fi].index, outer_field);
                                if (outer_sr->fields[fi].type.base == TypeUser
                                    && outer_sr->fields[fi].type.user_name) {
                                    base_sr = find_struct(cg, outer_sr->fields[fi].type.user_name);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            if (base_alloca && base_sr) {
                for (usize_t fi = 0; fi < base_sr->field_count; fi++) {
                    if (strcmp(base_sr->fields[fi].name, mfield) != 0) continue;
                    LLVMValueRef field_gep = LLVMBuildStructGEP2(
                        cg->builder, base_sr->llvm_type, base_alloca,
                        (unsigned)base_sr->fields[fi].index, mfield);
                    LLVMTypeRef ptr_type = get_llvm_type(cg, base_sr->fields[fi].type);
                    LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, mfield);
                    type_info_t elem_ti = base_sr->fields[fi].type;
                    elem_ti.is_pointer = False;
                    LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                    index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                    LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "midx");
                    rhs = coerce_int(cg, rhs, elem_type);
                    LLVMBuildStore(cg->builder, rhs, gep);
                    return rhs;
                }
            }
        }
    }

    if (target->kind == NodeMemberExpr) {
        node_t *obj = target->as.member_expr.object;
        char *field = target->as.member_expr.field;
        if (obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
            if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
                if (sr) {
                    for (usize_t i = 0; i < sr->field_count; i++) {
                        if (strcmp(sr->fields[i].name, field) == 0) {
                            LLVMValueRef gep = LLVMBuildStructGEP2(
                                cg->builder, sr->llvm_type, sym->value,
                                (unsigned)sr->fields[i].index, field);
                            LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
                            rhs = coerce_int(cg, rhs, ft);
                            /* bitfield: read-modify-write */
                            if (sr->fields[i].bit_width > 0) {
                                int boff = sr->fields[i].bit_offset;
                                int bw   = sr->fields[i].bit_width;
                                unsigned long long mask_val = ((unsigned long long)1 << bw) - 1;
                                /* mask the new value to bit_width bits */
                                LLVMValueRef masked_new = LLVMBuildAnd(cg->builder, rhs,
                                    LLVMConstInt(ft, mask_val, 0), "bf_new_mask");
                                /* shift to position */
                                LLVMValueRef shifted_new = masked_new;
                                if (boff > 0)
                                    shifted_new = LLVMBuildShl(cg->builder, masked_new,
                                        LLVMConstInt(ft, (unsigned long long)boff, 0), "bf_shl");
                                /* load old value, clear the target bits, OR in new */
                                LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, ft, gep, "bf_old");
                                unsigned long long clear_mask = ~(mask_val << boff);
                                LLVMValueRef cleared = LLVMBuildAnd(cg->builder, old_val,
                                    LLVMConstInt(ft, clear_mask, 0), "bf_clear");
                                LLVMValueRef result = LLVMBuildOr(cg->builder, cleared,
                                    shifted_new, "bf_insert");
                                LLVMBuildStore(cg->builder, result, gep);
                                return rhs;
                            }
                            LLVMBuildStore(cg->builder, rhs, gep);
                            return rhs;
                        }
                    }
                }
            }
        }
    }

    if (target->kind == NodeSelfMemberExpr) {
        char *field = target->as.self_member.field;
        char *type_name = target->as.self_member.type_name;
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (this_sym) {
            struct_reg_t *sr = find_struct(cg, type_name);
            if (sr) {
                LLVMValueRef this_ptr = this_sym->value;
                if (LLVMGetTypeKind(this_sym->type) == LLVMPointerTypeKind)
                    this_ptr = LLVMBuildLoad2(cg->builder, this_sym->type, this_sym->value, "this");
                for (usize_t i = 0; i < sr->field_count; i++) {
                    if (strcmp(sr->fields[i].name, field) == 0) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(
                            cg->builder, sr->llvm_type, this_ptr,
                            (unsigned)sr->fields[i].index, field);
                        LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
                        rhs = coerce_int(cg, rhs, ft);
                        LLVMBuildStore(cg->builder, rhs, gep);
                        return rhs;
                    }
                }
            }
        }
    }

    log_err("invalid assignment target");
    return rhs;
}

static LLVMValueRef gen_index(cg_t *cg, node_t *node) {
    node_t *obj = node->as.index_expr.object;
    LLVMValueRef index_val = gen_expr(cg, node->as.index_expr.index);

    /* null dereference check */
    if (obj->kind == NodeIdentExpr)
        check_null_deref(cg, obj->as.ident.name, node->line);

    if (obj->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", obj->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind) {
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
            LLVMValueRef indices[2] = { zero, index_val };
            LLVMValueRef gep = LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "idx");
            LLVMTypeRef elem_type = LLVMGetElementType(sym->type);
            return LLVMBuildLoad2(cg->builder, elem_type, gep, "elem");
        }
        if (llvm_is_ptr(sym->type)) {
            LLVMValueRef ptr = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "ptr");
            /* use the declared element type for correct stride */
            type_info_t elem_ti = sym->stype;
            elem_ti.is_pointer = False;
            LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
            index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
            LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidx");
            return LLVMBuildLoad2(cg->builder, elem_ty, gep, "pelem");
        }
    }

    /* member pointer indexing: obj.field[idx], e.g. b.data[i]
       Also handles nested: obj.field1.field2[idx], e.g. c.inner.data[i] */
    if (obj->kind == NodeMemberExpr) {
        node_t *mobj = obj->as.member_expr.object;
        char   *mfield = obj->as.member_expr.field;

        LLVMValueRef base_alloca = Null;
        struct_reg_t *base_sr = Null;

        if (mobj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, mobj->as.ident.name);
            if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                base_sr = find_struct(cg, sym->stype.user_name);
                base_alloca = sym->value;
            }
        } else if (mobj->kind == NodeMemberExpr) {
            node_t *outer_obj = mobj->as.member_expr.object;
            char   *outer_field = mobj->as.member_expr.field;
            if (outer_obj->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, outer_obj->as.ident.name);
                if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                    struct_reg_t *outer_sr = find_struct(cg, sym->stype.user_name);
                    if (outer_sr) {
                        for (usize_t fi = 0; fi < outer_sr->field_count; fi++) {
                            if (strcmp(outer_sr->fields[fi].name, outer_field) != 0) continue;
                            base_alloca = LLVMBuildStructGEP2(cg->builder, outer_sr->llvm_type,
                                sym->value, (unsigned)outer_sr->fields[fi].index, outer_field);
                            if (outer_sr->fields[fi].type.base == TypeUser
                                && outer_sr->fields[fi].type.user_name) {
                                base_sr = find_struct(cg, outer_sr->fields[fi].type.user_name);
                            }
                            break;
                        }
                    }
                }
            }
        }

        if (base_alloca && base_sr) {
            for (usize_t fi = 0; fi < base_sr->field_count; fi++) {
                if (strcmp(base_sr->fields[fi].name, mfield) != 0) continue;
                LLVMValueRef field_gep = LLVMBuildStructGEP2(
                    cg->builder, base_sr->llvm_type, base_alloca,
                    (unsigned)base_sr->fields[fi].index, mfield);
                LLVMTypeRef ptr_type = get_llvm_type(cg, base_sr->fields[fi].type);
                LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, mfield);
                type_info_t elem_ti = base_sr->fields[fi].type;
                elem_ti.is_pointer = False;
                LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "midx");
                return LLVMBuildLoad2(cg->builder, elem_type, gep, "melem");
            }
        }
    }

    /* string literal indexing: expr[idx] where expr might be a str_lit */
    LLVMValueRef obj_val = gen_expr(cg, obj);
    if (llvm_is_ptr(LLVMTypeOf(obj_val))) {
        LLVMTypeRef i8ty = LLVMInt8TypeInContext(cg->ctx);
        index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef gep = LLVMBuildGEP2(cg->builder, i8ty, obj_val, &index_val, 1, "stridx");
        return LLVMBuildLoad2(cg->builder, i8ty, gep, "ch");
    }

    log_err("cannot index non-array/pointer type");
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_member(cg_t *cg, node_t *node) {
    node_t *obj = node->as.member_expr.object;
    char *field = node->as.member_expr.field;

    if (obj->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
        if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
            struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
            if (sr) {
                for (usize_t i = 0; i < sr->field_count; i++) {
                    if (strcmp(sr->fields[i].name, field) == 0) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(
                            cg->builder, sr->llvm_type, sym->value,
                            (unsigned)sr->fields[i].index, field);
                        LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
                        LLVMValueRef val = LLVMBuildLoad2(cg->builder, ft, gep, field);
                        /* bitfield: extract bits via shift+mask */
                        if (sr->fields[i].bit_width > 0) {
                            int boff = sr->fields[i].bit_offset;
                            int bw   = sr->fields[i].bit_width;
                            if (boff > 0)
                                val = LLVMBuildLShr(cg->builder, val,
                                    LLVMConstInt(ft, (unsigned long long)boff, 0), "bf_shr");
                            unsigned long long mask = ((unsigned long long)1 << bw) - 1;
                            val = LLVMBuildAnd(cg->builder, val,
                                LLVMConstInt(ft, mask, 0), "bf_mask");
                        }
                        return val;
                    }
                }
            }
        }
    }

    /* nested member: obj.field1.field2, e.g. c.inner.data */
    if (obj->kind == NodeMemberExpr) {
        node_t *outer_obj = obj->as.member_expr.object;
        char   *outer_field = obj->as.member_expr.field;
        if (outer_obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, outer_obj->as.ident.name);
            if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                struct_reg_t *outer_sr = find_struct(cg, sym->stype.user_name);
                if (outer_sr) {
                    for (usize_t i = 0; i < outer_sr->field_count; i++) {
                        if (strcmp(outer_sr->fields[i].name, outer_field) != 0) continue;
                        LLVMValueRef outer_gep = LLVMBuildStructGEP2(
                            cg->builder, outer_sr->llvm_type, sym->value,
                            (unsigned)outer_sr->fields[i].index, outer_field);
                        /* outer field must be an embedded struct (non-pointer) */
                        if (outer_sr->fields[i].type.base != TypeUser
                            || outer_sr->fields[i].type.is_pointer
                            || !outer_sr->fields[i].type.user_name) break;
                        struct_reg_t *inner_sr = find_struct(cg, outer_sr->fields[i].type.user_name);
                        if (!inner_sr) break;
                        for (usize_t j = 0; j < inner_sr->field_count; j++) {
                            if (strcmp(inner_sr->fields[j].name, field) != 0) continue;
                            LLVMValueRef inner_gep = LLVMBuildStructGEP2(
                                cg->builder, inner_sr->llvm_type, outer_gep,
                                (unsigned)inner_sr->fields[j].index, field);
                            LLVMTypeRef ft = get_llvm_type(cg, inner_sr->fields[j].type);
                            return LLVMBuildLoad2(cg->builder, ft, inner_gep, field);
                        }
                        break;
                    }
                }
            }
        }
    }

    if (obj->kind == NodeIdentExpr) {
        /* might be an enum value: EnumName.Variant */
        enum_reg_t *er = find_enum(cg, obj->as.ident.name);
        if (er) {
            for (usize_t i = 0; i < er->variant_count; i++) {
                if (strcmp(er->variants[i].name, field) == 0) {
                    if (!er->is_tagged) {
                        /* C-style: return i32 constant */
                        return LLVMConstInt(er->llvm_type,
                                           (unsigned long long)er->variants[i].value, 0);
                    } else {
                        /* tagged enum: build { i32, [N x i8] } with discriminant set */
                        LLVMValueRef disc = LLVMConstInt(
                            LLVMInt32TypeInContext(cg->ctx), (unsigned long long)i, 0);
                        LLVMValueRef val = LLVMGetUndef(er->llvm_type);
                        return LLVMBuildInsertValue(cg->builder, val, disc, 0, "enum_disc");
                    }
                }
            }
        }
    }

    log_err("cannot resolve member '%s'", field);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_self_member(cg_t *cg, node_t *node) {
    /* Type.(field) — resolve to this->field */
    char *field = node->as.self_member.field;
    symbol_t *this_sym = cg_lookup(cg, "this");
    if (!this_sym) {
        log_err("self-member '%s' used outside of method", field);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    char *type_name = node->as.self_member.type_name;
    struct_reg_t *sr = find_struct(cg, type_name);
    if (!sr) {
        log_err("unknown struct '%s'", type_name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    /* this is a pointer to struct — load it first if needed */
    LLVMValueRef this_ptr = this_sym->value;
    if (LLVMGetTypeKind(this_sym->type) == LLVMPointerTypeKind)
        this_ptr = LLVMBuildLoad2(cg->builder, this_sym->type, this_sym->value, "this");

    for (usize_t i = 0; i < sr->field_count; i++) {
        if (strcmp(sr->fields[i].name, field) == 0) {
            LLVMValueRef gep = LLVMBuildStructGEP2(
                cg->builder, sr->llvm_type, this_ptr,
                (unsigned)sr->fields[i].index, field);
            LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
            return LLVMBuildLoad2(cg->builder, ft, gep, field);
        }
    }
    log_err("unknown field '%s' in struct '%s'", field, type_name);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_ternary(cg_t *cg, node_t *node) {
    LLVMValueRef cond = gen_expr(cg, node->as.ternary.cond);
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx))
        cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                             LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");

    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "tern.then");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "tern.else");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "tern.merge");
    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    LLVMValueRef then_val = gen_expr(cg, node->as.ternary.then_expr);
    LLVMBuildBr(cg->builder, merge_bb);
    then_bb = LLVMGetInsertBlock(cg->builder);

    LLVMPositionBuilderAtEnd(cg->builder, else_bb);
    LLVMValueRef else_val = gen_expr(cg, node->as.ternary.else_expr);
    LLVMBuildBr(cg->builder, merge_bb);
    else_bb = LLVMGetInsertBlock(cg->builder);

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    LLVMTypeRef result_type = LLVMTypeOf(then_val);
    else_val = coerce_int(cg, else_val, result_type);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, result_type, "ternary");
    LLVMValueRef vals[2] = { then_val, else_val };
    LLVMBasicBlockRef bbs[2] = { then_bb, else_bb };
    LLVMAddIncoming(phi, vals, bbs, 2);
    return phi;
}

static LLVMValueRef gen_cast(cg_t *cg, node_t *node) {
    LLVMValueRef val = gen_expr(cg, node->as.cast_expr.expr);
    LLVMTypeRef target = get_llvm_type(cg, node->as.cast_expr.target);
    LLVMTypeRef src = LLVMTypeOf(val);

    if (src == target) return val;

    /* int <-> ptr */
    if (llvm_is_int(src) && llvm_is_ptr(target))
        return LLVMBuildIntToPtr(cg->builder, val, target, "itoptr");
    if (llvm_is_ptr(src) && llvm_is_int(target))
        return LLVMBuildPtrToInt(cg->builder, val, target, "ptrtoi");
    /* ptr <-> ptr */
    if (llvm_is_ptr(src) && llvm_is_ptr(target))
        return val; /* opaque pointers, no-op */

    return coerce_int(cg, val, target);
}

static LLVMValueRef gen_new(cg_t *cg, node_t *node) {
    LLVMValueRef size = gen_expr(cg, node->as.new_expr.size);
    size = coerce_int(cg, size, LLVMInt64TypeInContext(cg->ctx));
    LLVMValueRef args[1] = { size };
    return LLVMBuildCall2(cg->builder, cg->malloc_type, cg->malloc_fn, args, 1, "alloc");
}

static LLVMValueRef gen_sizeof(cg_t *cg, node_t *node) {
    LLVMTypeRef ty = get_llvm_type(cg, node->as.sizeof_expr.type);
    return LLVMSizeOf(ty);
}

static LLVMValueRef gen_nil(cg_t *cg) {
    return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
}

static LLVMValueRef gen_mov(cg_t *cg, node_t *node) {
    LLVMValueRef ptr = gen_expr(cg, node->as.mov_expr.ptr);
    LLVMValueRef sz  = gen_expr(cg, node->as.mov_expr.size);
    sz = coerce_int(cg, sz, LLVMInt64TypeInContext(cg->ctx));
    LLVMValueRef args[2] = { ptr, sz };
    return LLVMBuildCall2(cg->builder, cg->realloc_type, cg->realloc_fn, args, 2, "realloc");
}

static LLVMValueRef gen_addr_of(cg_t *cg, node_t *node) {
    node_t *operand = node->as.addr_of.operand;
    if (operand->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", operand->as.ident.name);
            return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
        }
        if (sym->flags & SymHeapVar) {
            /* heap var: address is the malloc'd pointer */
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            return LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        }
        return sym->value; /* alloca = address of the stack variable */
    }
    log_err("address-of requires an lvalue");
    return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
}

static LLVMValueRef gen_expr(cg_t *cg, node_t *node) {
    switch (node->kind) {
        case NodeIntLitExpr:       return gen_int_lit(cg, node);
        case NodeFloatLitExpr:     return gen_float_lit(cg, node);
        case NodeBoolLitExpr:      return gen_bool_lit(cg, node);
        case NodeCharLitExpr:      return gen_char_lit(cg, node);
        case NodeStrLitExpr:       return gen_str_lit(cg, node);
        case NodeIdentExpr:        return gen_ident(cg, node);
        case NodeBinaryExpr:       return gen_binary(cg, node);
        case NodeUnaryPrefixExpr:  return gen_unary_prefix(cg, node);
        case NodeUnaryPostfixExpr: return gen_unary_postfix(cg, node);
        case NodeCallExpr:         return gen_call(cg, node);
        case NodeMethodCall:       return gen_method_call(cg, node);
        case NodeParallelCall:     return gen_parallel_call(cg, node);
        case NodeCompoundAssign:   return gen_compound_assign(cg, node);
        case NodeAssignExpr:       return gen_assign(cg, node);
        case NodeIndexExpr:        return gen_index(cg, node);
        case NodeMemberExpr:       return gen_member(cg, node);
        case NodeSelfMemberExpr:   return gen_self_member(cg, node);
        case NodeTernaryExpr:      return gen_ternary(cg, node);
        case NodeCastExpr:         return gen_cast(cg, node);
        case NodeNewExpr:          return gen_new(cg, node);
        case NodeSizeofExpr:       return gen_sizeof(cg, node);
        case NodeNilExpr:          return gen_nil(cg);
        case NodeMovExpr:          return gen_mov(cg, node);
        case NodeAddrOf:           return gen_addr_of(cg, node);
        case NodeErrorExpr: {
            /* error.('message') → {i1 true, ptr message} */
            LLVMValueRef msg = gen_expr(cg, node->as.error_expr.message);
            LLVMValueRef err = LLVMGetUndef(cg->error_type);
            err = LLVMBuildInsertValue(cg->builder, err,
                LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0), 0, "err.has");
            err = LLVMBuildInsertValue(cg->builder, err, msg, 1, "err.msg");
            return err;
        }
        case NodeExpectExpr: {
            /* expect.(expr) — if !expr, print failure and increment fail count */
            LLVMValueRef val = gen_expr(cg, node->as.expect_expr.expr);
            if (LLVMTypeOf(val) != LLVMInt1TypeInContext(cg->ctx))
                val = LLVMBuildICmp(cg->builder, LLVMIntNE, val,
                    LLVMConstInt(LLVMTypeOf(val), 0, 0), "tobool");
            LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expect.pass");
            LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expect.fail");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expect.end");
            LLVMBuildCondBr(cg->builder, val, pass_bb, fail_bb);
            LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "  FAIL: expect at line %lu\n", node->line);
                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, msg, "efmt");
                LLVMValueRef args[1] = { fmt };
                LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 1, "");
                if (cg->test_fail_count) {
                    LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                    cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                    LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
                }
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, pass_bb);
            if (cg->test_pass_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_pass_count, "pc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "pi");
                LLVMBuildStore(cg->builder, cnt, cg->test_pass_count);
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeExpectEqExpr: {
            LLVMValueRef left = gen_expr(cg, node->as.expect_eq.left);
            LLVMValueRef right = gen_expr(cg, node->as.expect_eq.right);
            right = coerce_int(cg, right, LLVMTypeOf(left));
            LLVMValueRef eq = llvm_is_float(LLVMTypeOf(left))
                ? LLVMBuildFCmp(cg->builder, LLVMRealOEQ, left, right, "feq")
                : LLVMBuildICmp(cg->builder, LLVMIntEQ, left, right, "eq");
            LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expeq.pass");
            LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expeq.fail");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expeq.end");
            LLVMBuildCondBr(cg->builder, eq, pass_bb, fail_bb);
            LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "  FAIL: expect_eq at line %lu\n", node->line);
                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, msg, "eqfmt");
                LLVMValueRef args[1] = { fmt };
                LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 1, "");
                if (cg->test_fail_count) {
                    LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                    cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                    LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
                }
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, pass_bb);
            if (cg->test_pass_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_pass_count, "pc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "pi");
                LLVMBuildStore(cg->builder, cnt, cg->test_pass_count);
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeExpectNeqExpr: {
            LLVMValueRef left = gen_expr(cg, node->as.expect_neq.left);
            LLVMValueRef right = gen_expr(cg, node->as.expect_neq.right);
            right = coerce_int(cg, right, LLVMTypeOf(left));
            LLVMValueRef neq = llvm_is_float(LLVMTypeOf(left))
                ? LLVMBuildFCmp(cg->builder, LLVMRealONE, left, right, "fne")
                : LLVMBuildICmp(cg->builder, LLVMIntNE, left, right, "ne");
            LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expneq.pass");
            LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expneq.fail");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expneq.end");
            LLVMBuildCondBr(cg->builder, neq, pass_bb, fail_bb);
            LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "  FAIL: expect_neq at line %lu\n", node->line);
                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, msg, "neqfmt");
                LLVMValueRef args[1] = { fmt };
                LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 1, "");
                if (cg->test_fail_count) {
                    LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                    cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                    LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
                }
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, pass_bb);
            if (cg->test_pass_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_pass_count, "pc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "pi");
                LLVMBuildStore(cg->builder, cnt, cg->test_pass_count);
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeTestFailExpr: {
            LLVMValueRef msg = gen_expr(cg, node->as.test_fail.message);
            LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, "  FAIL: %s\n", "tffmt");
            LLVMValueRef args[2] = { fmt, msg };
            LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 2, "");
            if (cg->test_fail_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
            }
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeDesigInit: {
            /* Type { .x = 1, .y = 2 } — designated initializer */
            struct_reg_t *sr = find_struct(cg, node->as.desig_init.type_name);
            if (!sr) {
                log_err("line %lu: unknown struct '%s' in designated initializer",
                        node->line, node->as.desig_init.type_name);
                return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
            LLVMValueRef tmp = alloc_in_entry(cg, sr->llvm_type, "desig_tmp");
            /* zero-initialize first */
            LLVMValueRef zero = LLVMConstNull(sr->llvm_type);
            LLVMBuildStore(cg->builder, zero, tmp);
            /* set each named field */
            for (usize_t di = 0; di < node->as.desig_init.fields.count; di++) {
                node_t *fname_node = node->as.desig_init.fields.items[di];
                node_t *fval_node  = node->as.desig_init.values.items[di];
                char *fname = fname_node->as.ident.name;
                for (usize_t fi = 0; fi < sr->field_count; fi++) {
                    if (strcmp(sr->fields[fi].name, fname) == 0) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(
                            cg->builder, sr->llvm_type, tmp,
                            (unsigned)sr->fields[fi].index, fname);
                        LLVMTypeRef ft = get_llvm_type(cg, sr->fields[fi].type);
                        LLVMValueRef val = gen_expr(cg, fval_node);
                        val = coerce_int(cg, val, ft);
                        LLVMBuildStore(cg->builder, val, gep);
                        break;
                    }
                }
            }
            return LLVMBuildLoad2(cg->builder, sr->llvm_type, tmp, "desig_val");
        }
        case NodeRemStmt: {
            node_t *ptr_node = node->as.rem_stmt.ptr;
            /* for heap primitive vars: free the heap ptr and null the alloca */
            if (ptr_node->kind == NodeIdentExpr) {
                symbol_t *hsym = cg_lookup(cg, ptr_node->as.ident.name);
                if (hsym && (hsym->flags & SymHeapVar)) {
                    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                    LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty,
                                                            hsym->value, "hptr");
                    LLVMValueRef fargs[1] = { heap_ptr };
                    LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, fargs, 1, "");
                    LLVMBuildStore(cg->builder, LLVMConstNull(ptr_ty), hsym->value);
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                }
            }
            LLVMValueRef ptr = gen_expr(cg, ptr_node);
            LLVMValueRef args[1] = { ptr };
            LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, args, 1, "");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        default:
            log_err("unexpected node kind %d in expression", node->kind);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}
