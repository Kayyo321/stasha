/* ── expressions ── */

/* convert any value to i1 for use as a boolean condition */
static LLVMValueRef llvm_to_bool(cg_t *cg, LLVMValueRef val) {
    LLVMTypeRef t = LLVMTypeOf(val);
    if (t == LLVMInt1TypeInContext(cg->ctx)) return val;
    if (LLVMGetTypeKind(t) == LLVMPointerTypeKind)
        return LLVMBuildIsNotNull(cg->builder, val, "tobool");
    if (LLVMGetTypeKind(t) == LLVMFloatTypeKind || LLVMGetTypeKind(t) == LLVMDoubleTypeKind)
        return LLVMBuildFCmp(cg->builder, LLVMRealONE, val, LLVMConstReal(t, 0.0), "tobool");
    return LLVMBuildICmp(cg->builder, LLVMIntNE, val, LLVMConstInt(t, 0, 0), "tobool");
}

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
    /* module-level globals are stored under the mangled name (e.g. "config__MathPI").
       When inside a function in the same module, try the prefixed form as fallback. */
    if (!sym && cg->current_module_prefix[0]) {
        char mangled[512];
        snprintf(mangled, sizeof(mangled), "%s__%s",
                 cg->current_module_prefix, node->as.ident.name);
        sym = cg_lookup(cg, mangled);
    }
    if (!sym) {
        diag_begin_error("undefined variable '%s'", node->as.ident.name);
        diag_span(DIAG_NODE(node), True, "not found in this scope");
        diag_note("variables must be declared before use");
        /* Levenshtein suggestion: scan symbol table for close name */
        usize_t best_dist = 3; /* max edit distance to suggest */
        const char *best = Null;
        for (usize_t i = 0; i < cg->locals.count; i++) {
            usize_t d = levenshtein(node->as.ident.name, cg->locals.entries[i].name);
            if (d < best_dist) { best_dist = d; best = cg->locals.entries[i].name; }
        }
        for (usize_t i = 0; i < cg->globals.count; i++) {
            usize_t d = levenshtein(node->as.ident.name, cg->globals.entries[i].name);
            if (d < best_dist) { best_dist = d; best = cg->globals.entries[i].name; }
        }
        if (best) diag_help("did you mean '%s'?", best);
        diag_finish();
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
        lhs = llvm_to_bool(cg, lhs);
        LLVMBuildCondBr(cg->builder, lhs, rhs_bb, merge_bb);

        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef rhs = gen_expr(cg, node->as.binary.right);
        rhs = llvm_to_bool(cg, rhs);
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
        lhs = llvm_to_bool(cg, lhs);
        LLVMBuildCondBr(cg->builder, lhs, merge_bb, rhs_bb);

        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef rhs = gen_expr(cg, node->as.binary.right);
        rhs = llvm_to_bool(cg, rhs);
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

    /* pointer arithmetic: ptr ± integer → GEP (i8 base = byte-level stride) */
    if ((node->as.binary.op == TokPlus || node->as.binary.op == TokMinus)
        && LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMPointerTypeKind) {
        LLVMTypeRef i8 = LLVMInt8TypeInContext(cg->ctx);
        right = coerce_int(cg, right, LLVMInt64TypeInContext(cg->ctx));
        if (node->as.binary.op == TokMinus)
            right = LLVMBuildNeg(cg->builder, right, "neg");
        return LLVMBuildGEP2(cg->builder, i8, left, &right, 1, "ptrarith");
    }

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
            diag_begin_error("unknown binary operator");
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

static LLVMValueRef gen_unary_prefix(cg_t *cg, node_t *node) {
    if (node->as.unary.op == TokPlusPlus || node->as.unary.op == TokMinusMinus) {
        node_t *operand = node->as.unary.operand;
        if (operand->kind == NodeSelfMemberExpr) {
            /* prefix ++/-- on Type.(field) */
            symbol_t *this_sym = cg_lookup(cg, "this");
            if (!this_sym) {
                diag_begin_error("self-member used outside of method");
                diag_note("'this' is only available inside struct method bodies");
                diag_finish();
                goto prefix_incdec_fail;
            }
            struct_reg_t *sr = find_struct(cg, operand->as.self_member.type_name);
            if (!sr) {
                diag_begin_error("unknown struct in prefix ++/--");
                diag_finish();
                goto prefix_incdec_fail;
            }
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef this_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
            for (usize_t i = 0; i < sr->field_count; i++) {
                if (strcmp(sr->fields[i].name, operand->as.self_member.field) != 0) continue;
                LLVMValueRef store_ptr = LLVMBuildStructGEP2(cg->builder, sr->llvm_type, this_ptr,
                                                              (unsigned)sr->fields[i].index, "smf");
                LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
                LLVMValueRef val = LLVMBuildLoad2(cg->builder, ft, store_ptr, "");
                LLVMValueRef one = LLVMConstInt(ft, 1, 0);
                LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
                    ? LLVMBuildAdd(cg->builder, val, one, "inc")
                    : LLVMBuildSub(cg->builder, val, one, "dec");
                LLVMBuildStore(cg->builder, result, store_ptr);
                return result;
            }
            prefix_incdec_fail:;
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        if (operand->kind != NodeIdentExpr) {
            diag_begin_error("prefix ++/-- requires an identifier");
            diag_span(DIAG_NODE(operand), True, "");
            diag_note("operand of ++/-- must be a variable name");
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
        if (!sym) {
            diag_begin_error("undefined variable '%s'", operand->as.ident.name);
            diag_span(DIAG_NODE(operand), True, "not found in this scope");
            diag_note("variables must be declared before use");
            diag_finish();
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
        LLVMValueRef result;
        if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMPointerTypeKind) {
            type_info_t elem_ti = sym->stype;
            elem_ti.is_pointer = False;
            LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
            long long delta = (node->as.unary.op == TokPlusPlus) ? 1 : -1;
            LLVMValueRef offset = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx),
                                               (unsigned long long)delta, 1);
            result = LLVMBuildGEP2(cg->builder, elem_type, val, &offset, 1, "ptrinc");
        } else {
            LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
            result = (node->as.unary.op == TokPlusPlus)
                ? LLVMBuildAdd(cg->builder, val, one, "inc")
                : LLVMBuildSub(cg->builder, val, one, "dec");
        }
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
        operand = llvm_to_bool(cg, operand);
        return LLVMBuildNot(cg->builder, operand, "not");
    }
    if (node->as.unary.op == TokTilde) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        return LLVMBuildNot(cg->builder, operand, "bnot");
    }
    /* pointer dereference: *ptr — load the value the pointer points to */
    if (node->as.unary.op == TokStar) {
        LLVMValueRef ptr = gen_expr(cg, node->as.unary.operand);
        /* determine pointee type from the source expression */
        LLVMTypeRef pointee_type = Null;
        node_t *inner = node->as.unary.operand;
        if (inner->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, inner->as.ident.name);
            if (sym && sym->stype.is_pointer) {
                type_info_t pt = sym->stype;
                pt.is_pointer = False;
                pointee_type = get_llvm_type(cg, pt);
            }
        }
        if (!pointee_type) pointee_type = LLVMInt8TypeInContext(cg->ctx);
        return LLVMBuildLoad2(cg->builder, pointee_type, ptr, "deref");
    }
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_unary_postfix(cg_t *cg, node_t *node) {
    node_t *operand = node->as.unary.operand;
    if (operand->kind == NodeSelfMemberExpr) {
        /* postfix ++/-- on Type.(field) — return old value */
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (!this_sym) {
            diag_begin_error("self-member used outside of method");
            diag_note("'this' is only available inside struct method bodies");
            diag_finish();
            goto postfix_incdec_fail;
        }
        struct_reg_t *sr = find_struct(cg, operand->as.self_member.type_name);
        if (!sr) {
            diag_begin_error("unknown struct in postfix ++/--");
            diag_finish();
            goto postfix_incdec_fail;
        }
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef this_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
        for (usize_t i = 0; i < sr->field_count; i++) {
            if (strcmp(sr->fields[i].name, operand->as.self_member.field) != 0) continue;
            LLVMValueRef store_ptr = LLVMBuildStructGEP2(cg->builder, sr->llvm_type, this_ptr,
                                                          (unsigned)sr->fields[i].index, "smf");
            LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
            LLVMValueRef val = LLVMBuildLoad2(cg->builder, ft, store_ptr, "");
            LLVMValueRef one = LLVMConstInt(ft, 1, 0);
            LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
                ? LLVMBuildAdd(cg->builder, val, one, "inc")
                : LLVMBuildSub(cg->builder, val, one, "dec");
            LLVMBuildStore(cg->builder, result, store_ptr);
            return val; /* postfix returns the original value */
        }
        postfix_incdec_fail:;
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    if (operand->kind != NodeIdentExpr) {
        diag_begin_error("postfix ++/-- requires an identifier");
        diag_span(DIAG_NODE(operand), True, "");
        diag_note("operand of ++/-- must be a variable name");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
    if (!sym) {
        diag_begin_error("undefined variable '%s'", operand->as.ident.name);
        diag_span(DIAG_NODE(operand), True, "not found in this scope");
        diag_note("variables must be declared before use");
        diag_finish();
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
    LLVMValueRef result;
    if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMPointerTypeKind) {
        type_info_t elem_ti = sym->stype;
        elem_ti.is_pointer = False;
        LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
        long long delta = (node->as.unary.op == TokPlusPlus) ? 1 : -1;
        LLVMValueRef offset = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx),
                                           (unsigned long long)delta, 1);
        result = LLVMBuildGEP2(cg->builder, elem_type, val, &offset, 1, "ptrinc");
    } else {
        LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
        result = (node->as.unary.op == TokPlusPlus)
            ? LLVMBuildAdd(cg->builder, val, one, "inc")
            : LLVMBuildSub(cg->builder, val, one, "dec");
    }
    LLVMBuildStore(cg->builder, result, store_ptr);
    return val;
}

static LLVMValueRef gen_call(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.call.callee);

    /* intra-module unqualified call: try module__callee when inside an
       imported module (current_module_prefix is non-empty) */
    if (!sym && cg->current_module_prefix[0]) {
        char mod_mangled[512];
        snprintf(mod_mangled, sizeof(mod_mangled), "%s__%s",
                 cg->current_module_prefix, node->as.call.callee);
        sym = cg_lookup(cg, mod_mangled);
    }

    /* sibling method call: inside a struct method, plain name resolves to
       the mangled method or legacy "Struct.method" form */
    boolean_t is_sibling_call = False;
    if (!sym && cg->current_struct_name) {
        char mangled[512];
        if (cg->current_module_prefix[0]) {
            snprintf(mangled, sizeof(mangled), "%s__%s__%s",
                     cg->current_module_prefix,
                     cg->current_struct_name, node->as.call.callee);
        } else {
            snprintf(mangled, sizeof(mangled), "%s.%s",
                     cg->current_struct_name, node->as.call.callee);
        }
        sym = cg_lookup(cg, mangled);
        if (sym) is_sibling_call = True;
    }
    if (!sym) {
        diag_begin_error("undefined function or function pointer '%s'", node->as.call.callee);
        diag_span(DIAG_NODE(node), True, "not defined in this module");
        /* Suggest close names */
        usize_t best_dist = 3;
        const char *best = Null;
        for (usize_t i = 0; i < cg->globals.count; i++) {
            usize_t d = levenshtein(node->as.call.callee, cg->globals.entries[i].name);
            if (d < best_dist) { best_dist = d; best = cg->globals.entries[i].name; }
        }
        if (best) diag_help("did you mean '%s'?", best);
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    usize_t user_argc = node->as.call.args.count;

    LLVMTypeRef fn_type;
    LLVMValueRef fn_val;

    if (sym->stype.base == TypeFnPtr && sym->stype.fn_ptr_desc) {
        /* ── indirect call through a domain-tagged function pointer variable ── */
        fn_ptr_desc_t *desc = sym->stype.fn_ptr_desc;

        /* domain check: actual argument storage must match declared parameter domain */
        for (usize_t i = 0; i < user_argc && i < desc->param_count; i++) {
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
                        diag_begin_error("argument %lu to function pointer '%s' has "
                                "wrong storage domain (expected %s, got %s)",
                                (unsigned long)(i + 1),
                                sym->name, exp_s, got_s);
                        diag_finish();
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

    /* for sibling calls, check if we need to prepend 'this' */
    unsigned n_params = LLVMCountParamTypes(fn_type);
    boolean_t prepend_this = is_sibling_call
        && ((usize_t)n_params == user_argc + 1)
        && (cg_lookup(cg, "this") != Null);

    usize_t argc = user_argc + (prepend_this ? 1 : 0);
    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (argc > 0) {
        args_heap = allocate(argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        usize_t offset = 0;
        if (prepend_this) {
            symbol_t *this_sym = cg_lookup(cg, "this");
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            args[0] = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
            offset = 1;
        }
        for (usize_t i = 0; i < user_argc; i++)
            args[offset + i] = gen_expr(cg, node->as.call.args.items[i]);
    }

    /* coerce arguments to the declared parameter types (e.g. i32 literal → f32) */
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

    /* check if object is a lib/module alias: alias.func(args) */
    if (obj->kind == NodeIdentExpr) {
        lib_entry_t *lib_ent = find_lib_entry(cg, obj->as.ident.name);
        if (lib_ent) {
            if (lib_ent->mod_prefix && lib_ent->mod_prefix[0]) {
                /* ── Stasha module alias: look up the mangled symbol ──────── */
                char mangled_sym[512];
                snprintf(mangled_sym, sizeof(mangled_sym), "%s__%s",
                         lib_ent->mod_prefix, method);
                symbol_t *fn_sym = cg_lookup(cg, mangled_sym);
                if (fn_sym) {
                    usize_t user_argc = node->as.method_call.args.count;
                    LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
                    unsigned n_params = LLVMCountParamTypes(fn_type);
                    usize_t argc = user_argc;
                    heap_t args_heap = NullHeap;
                    LLVMValueRef *args = Null;
                    if (argc > 0) {
                        args_heap = allocate(argc, sizeof(LLVMValueRef));
                        args = args_heap.pointer;
                        for (usize_t i = 0; i < user_argc; i++)
                            args[i] = gen_expr(cg, node->as.method_call.args.items[i]);
                    }
                    /* coerce args to declared param types */
                    if (argc > 0 && n_params > 0) {
                        heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
                        LLVMTypeRef *ptypes = pt_heap.pointer;
                        LLVMGetParamTypes(fn_type, ptypes);
                        for (usize_t i = 0; i < argc && i < (usize_t)n_params; i++)
                            args[i] = coerce_int(cg, args[i], ptypes[i]);
                        deallocate(pt_heap);
                    }
                    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type,
                                                       fn_sym->value, args,
                                                       (unsigned)argc, "");
                    if (argc > 0) deallocate(args_heap);
                    return ret;
                }
                /* fall through to error at the bottom */
            } else {
                /* ── C lib alias: auto-declare function with raw symbol name ─ */
                usize_t argc = node->as.method_call.args.count;
                heap_t args_heap = NullHeap;
                LLVMValueRef *args = Null;
                if (argc > 0) {
                    args_heap = allocate(argc, sizeof(LLVMValueRef));
                    args = args_heap.pointer;
                    for (usize_t i = 0; i < argc; i++)
                        args[i] = gen_expr(cg, node->as.method_call.args.items[i]);
                }

                symbol_t *fn_sym = cg_lookup(cg, method);
                if (!fn_sym) {
                    LLVMTypeRef ret_type;
                    LLVMTypeRef *param_types = Null;
                    heap_t ptypes_heap = NullHeap;
                    unsigned param_count = 0;
                    boolean_t is_varargs = False;

                    /* Use hint from LHS context if available (e.g. ptr return for *rw vars) */
                    ret_type = (cg->hint_ret_type)
                               ? cg->hint_ret_type
                               : LLVMInt32TypeInContext(cg->ctx);
                    if (argc > 0) {
                        /*
                         * Infer the C function signature from the call-site arg types.
                         *
                         * Strategy: find the last pointer argument.  If there are
                         * non-pointer args *after* it, the function is printf-style
                         * varargs and everything up to (and including) that last
                         * pointer is a fixed param.  Otherwise use an exact signature.
                         *
                         * Examples:
                         *   printf(ptr, i32)         → last ptr=0, non-ptr after → (ptr,...) varargs
                         *   snprintf(ptr, i64, ptr, i32) → last ptr=2 → (ptr,i64,ptr,...) varargs
                         *   puts(ptr)                → last ptr=0, no non-ptr after → (ptr) exact
                         *   memcpy(ptr, ptr, i64)    → last ptr=1, non-ptr after → (ptr,ptr,...) varargs
                         */
                        usize_t last_ptr_idx = 0;
                        boolean_t has_ptr = False;
                        boolean_t non_ptr_after = False;
                        for (usize_t i = 0; i < argc; i++) {
                            if (LLVMGetTypeKind(LLVMTypeOf(args[i])) == LLVMPointerTypeKind) {
                                last_ptr_idx = i;
                                has_ptr = True;
                                non_ptr_after = False;
                            } else if (has_ptr) {
                                non_ptr_after = True;
                            }
                        }

                        usize_t fixed_count = (has_ptr && non_ptr_after)
                                             ? last_ptr_idx + 1
                                             : argc;
                        is_varargs = (has_ptr && non_ptr_after);

                        ptypes_heap = allocate(fixed_count, sizeof(LLVMTypeRef));
                        param_types = ptypes_heap.pointer;
                        for (usize_t i = 0; i < fixed_count; i++)
                            param_types[i] = LLVMTypeOf(args[i]);
                        param_count = (unsigned)fixed_count;
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
        }

        /* check if it's a static method call: Type.method(args)
           First try the struct registry to get the module prefix for correct mangling,
           then fall back to the legacy "Type.method" symtab key for root-module types. */
        {
        char mangled[512];
        struct_reg_t *sr_static = find_struct(cg, obj->as.ident.name);
        if (sr_static && sr_static->mod_prefix && sr_static->mod_prefix[0]) {
            snprintf(mangled, sizeof(mangled), "%s__%s__%s",
                     sr_static->mod_prefix, obj->as.ident.name, method);
        } else {
            snprintf(mangled, sizeof(mangled), "%s.%s", obj->as.ident.name, method);
        }
        symbol_t *fn_sym = cg_lookup(cg, mangled);
        if (fn_sym) {
            usize_t user_argc = node->as.method_call.args.count;
            LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
            unsigned n_params = LLVMCountParamTypes(fn_type);
            /* if function is an instance method (this + user args), skip this */
            boolean_t has_this = ((usize_t)n_params == user_argc + 1);
            usize_t argc = has_this ? user_argc + 1 : user_argc;
            heap_t args_heap = NullHeap;
            LLVMValueRef *args = Null;
            if (argc > 0) {
                args_heap = allocate(argc, sizeof(LLVMValueRef));
                args = args_heap.pointer;
                usize_t offset = 0;
                if (has_this) {
                    /* pass null/undef for unused this pointer */
                    args[0] = LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
                    offset = 1;
                }
                for (usize_t i = 0; i < user_argc; i++)
                    args[offset + i] = gen_expr(cg, node->as.method_call.args.items[i]);
            }
            /* coerce args */
            if (argc > 0 && n_params > 0) {
                heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
                LLVMTypeRef *param_types = pt_heap.pointer;
                LLVMGetParamTypes(fn_type, param_types);
                for (usize_t i = has_this ? 1 : 0; i < argc && i < (usize_t)n_params; i++)
                    args[i] = coerce_int(cg, args[i], param_types[i]);
                deallocate(pt_heap);
            }
            LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                               args, (unsigned)argc, "");
            if (argc > 0) deallocate(args_heap);
            return ret;
        }
        } /* close extra scope block for static-method mangling vars */
    }

    /* instance method call: obj.method(args) — pass &obj as first arg.
       Use the struct registry's mod_prefix so that methods on imported types
       resolve to the correct mangled symbol (e.g. geom__Vec2__len). */
    if (obj->kind == NodeIdentExpr) {
        symbol_t *obj_sym = cg_lookup(cg, obj->as.ident.name);
        if (obj_sym && obj_sym->stype.base == TypeUser && obj_sym->stype.user_name) {
            char mangled[512];
            struct_reg_t *sr_inst = find_struct(cg, obj_sym->stype.user_name);
            if (sr_inst && sr_inst->mod_prefix && sr_inst->mod_prefix[0]) {
                snprintf(mangled, sizeof(mangled), "%s__%s__%s",
                         sr_inst->mod_prefix, obj_sym->stype.user_name, method);
            } else {
                snprintf(mangled, sizeof(mangled), "%s.%s",
                         obj_sym->stype.user_name, method);
            }
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

    diag_begin_error("cannot resolve method call '%s'", method);
    diag_span(DIAG_NODE(node), True, "");
    diag_note("check that the field/method exists in the struct definition");
    diag_finish();
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

/* ── thread dispatch code generation ────────────────────────────────────── */

/* Return the thr_wrapper_t for `fn_name`, creating it if not cached.
   `sym` must point to the LLVM function value; `fn_decl` is the AST node. */
static thr_wrapper_t *get_or_create_thread_wrapper(cg_t *cg,
        const char *fn_name, symbol_t *sym, node_t *fn_decl) {

    /* cache lookup */
    for (usize_t i = 0; i < cg->thr_wrap_count; i++)
        if (strcmp(cg->thr_wrappers[i].fn_name, fn_name) == 0)
            return &cg->thr_wrappers[i];

    LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);

    /* ── build args struct type { param0_type, param1_type, ... } ── */
    usize_t param_count = fn_decl->as.fn_decl.params.count;
    LLVMTypeRef args_struct_type = Null;

    if (param_count > 0) {
        heap_t pt_heap = allocate(param_count, sizeof(LLVMTypeRef));
        LLVMTypeRef *ptypes = pt_heap.pointer;
        for (usize_t i = 0; i < param_count; i++) {
            node_t *param = fn_decl->as.fn_decl.params.items[i];
            type_info_t pti = resolve_alias(cg, param->as.var_decl.type);
            ptypes[i] = get_llvm_type(cg, pti);
        }
        char sname[256];
        snprintf(sname, sizeof(sname), "__thr_args_%s_t", fn_name);
        args_struct_type = LLVMStructCreateNamed(cg->ctx, sname);
        LLVMStructSetBody(args_struct_type, ptypes, (unsigned)param_count, 0);
        deallocate(pt_heap);
    }

    /* ── determine return type size ── */
    boolean_t void_return = True;
    LLVMTypeRef ret_llvm_type = Null;
    usize_t result_size = 0;
    if (fn_decl->as.fn_decl.return_count > 0) {
        type_info_t rti = fn_decl->as.fn_decl.return_types[0];
        if (!(rti.base == TypeVoid && !rti.is_pointer)) {
            void_return = False;
            ret_llvm_type = get_llvm_type(cg, rti);
            if (cg->di_data_layout && ret_llvm_type)
                result_size = (usize_t)LLVMABISizeOfType(cg->di_data_layout, ret_llvm_type);
            else
                result_size = payload_type_size(rti);
        }
    }

    /* ── generate the wrapper function ──
       signature: void __thr_wrap_<name>(ptr %args, ptr %result)          */
    LLVMTypeRef wp_params[2] = { ptr_t, ptr_t };
    LLVMTypeRef wrapper_fn_type = LLVMFunctionType(
        LLVMVoidTypeInContext(cg->ctx), wp_params, 2, 0);

    char wrapper_name[256];
    snprintf(wrapper_name, sizeof(wrapper_name), "__thr_wrap_%s", fn_name);
    LLVMValueRef wrapper_fn = LLVMAddFunction(cg->module, wrapper_name, wrapper_fn_type);
    LLVMSetLinkage(wrapper_fn, LLVMInternalLinkage);

    /* save builder state */
    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef      saved_fn = cg->current_fn;

    /* build entry block */
    LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlockInContext(cg->ctx, wrapper_fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry_bb);
    cg->current_fn = wrapper_fn;

    LLVMValueRef args_param   = LLVMGetParam(wrapper_fn, 0);
    LLVMValueRef result_param = LLVMGetParam(wrapper_fn, 1);

    /* load each argument from the packed struct */
    LLVMValueRef *call_args = Null;
    heap_t        ca_heap   = NullHeap;
    if (param_count > 0) {
        ca_heap   = allocate(param_count, sizeof(LLVMValueRef));
        call_args = ca_heap.pointer;
        for (usize_t i = 0; i < param_count; i++) {
            node_t *param = fn_decl->as.fn_decl.params.items[i];
            type_info_t pti = resolve_alias(cg, param->as.var_decl.type);
            LLVMTypeRef ptype = get_llvm_type(cg, pti);
            LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, args_struct_type,
                                                    args_param, (unsigned)i, "ap");
            call_args[i] = LLVMBuildLoad2(cg->builder, ptype, gep, "av");
        }
    }

    /* free the args struct — all values have been loaded */
    {
        LLVMValueRef free_args[1] = { args_param };
        LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, free_args, 1, "");
    }

    /* call the original function */
    LLVMTypeRef  orig_fn_type = LLVMGlobalGetValueType(sym->value);
    if (void_return) {
        LLVMBuildCall2(cg->builder, orig_fn_type, sym->value,
                       call_args, (unsigned)param_count, "");
    } else {
        LLVMValueRef ret_val = LLVMBuildCall2(cg->builder, orig_fn_type, sym->value,
                                               call_args, (unsigned)param_count, "tret");
        /* store result into result buffer */
        LLVMBuildStore(cg->builder, ret_val, result_param);
    }
    LLVMBuildRetVoid(cg->builder);

    if (param_count > 0) deallocate(ca_heap);

    /* restore builder state */
    cg->current_fn = saved_fn;
    if (saved_bb) LLVMPositionBuilderAtEnd(cg->builder, saved_bb);

    /* ── store in cache ── */
    if (cg->thr_wrap_count >= cg->thr_wrap_cap) {
        usize_t new_cap = cg->thr_wrap_cap ? cg->thr_wrap_cap * 2 : 8;
        heap_t  new_h   = allocate(new_cap, sizeof(thr_wrapper_t));
        if (cg->thr_wrap_count > 0)
            memcpy(new_h.pointer, cg->thr_wrappers,
                   cg->thr_wrap_count * sizeof(thr_wrapper_t));
        if (cg->thr_wrap_cap > 0) deallocate(cg->thr_wrap_heap);
        cg->thr_wrappers  = new_h.pointer;
        cg->thr_wrap_cap  = new_cap;
        cg->thr_wrap_heap = new_h;
    }
    thr_wrapper_t *w    = &cg->thr_wrappers[cg->thr_wrap_count++];
    w->fn_name          = ast_strdup(fn_name, strlen(fn_name));
    w->wrapper_fn       = wrapper_fn;
    w->args_struct_type = args_struct_type;
    w->param_count      = param_count;
    w->result_size      = result_size;
    return w;
}

static LLVMValueRef gen_thread_call(cg_t *cg, node_t *node) {
    const char *callee = node->as.thread_call.callee;

    /* look up the function in the symbol table */
    symbol_t *sym = cg_lookup(cg, callee);
    if (!sym) {
        diag_begin_error("undefined function '%s'", callee);
        diag_span(DIAG_NODE(node), True, "not defined in this module");
        diag_note("thread dispatch requires the function to be visible in this module");
        diag_finish();
        return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }

    /* find AST declaration for param / return type info */
    node_t *fn_decl = find_fn_decl(cg, callee);
    if (!fn_decl) {
        diag_begin_error("cannot find declaration of '%s' for thread dispatch", callee);
        diag_span(DIAG_NODE(node), True, "dispatched here");
        diag_note("function must be declared in the same compilation unit");
        diag_finish();
        return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }

    /* get / lazily create the wrapper function */
    thr_wrapper_t *w = get_or_create_thread_wrapper(cg, callee, sym, fn_decl);

    /* ── pack arguments into a heap-allocated struct ── */
    LLVMValueRef args_ptr;
    usize_t argc = node->as.thread_call.args.count;

    if (argc > 0 && w->args_struct_type) {
        LLVMValueRef sz = LLVMSizeOf(w->args_struct_type);
        sz = coerce_int(cg, sz, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef malloc_args[1] = { sz };
        args_ptr = LLVMBuildCall2(cg->builder, cg->malloc_type,
                                   cg->malloc_fn, malloc_args, 1, "thr_args");
        for (usize_t i = 0; i < argc; i++) {
            LLVMValueRef val = gen_expr(cg, node->as.thread_call.args.items[i]);
            LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, w->args_struct_type,
                                                    args_ptr, (unsigned)i, "af");
            LLVMBuildStore(cg->builder, val, gep);
        }
    } else {
        args_ptr = LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }

    /* ── call __thread_dispatch(wrapper_fn, args_ptr, result_size) ── */
    LLVMValueRef result_sz = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx),
                                           (unsigned long long)w->result_size, 0);
    LLVMValueRef dispatch_args[3] = { w->wrapper_fn, args_ptr, result_sz };
    LLVMValueRef future = LLVMBuildCall2(cg->builder,
                                          cg->thread_dispatch_type,
                                          cg->thread_dispatch_fn,
                                          dispatch_args, 3, "future");
    return future;
}

static LLVMValueRef gen_future_op(cg_t *cg, node_t *node) {
    LLVMValueRef handle = gen_expr(cg, node->as.future_op.handle);
    LLVMValueRef call_args[1] = { handle };

    switch (node->as.future_op.op) {
        case FutureWait:
            LLVMBuildCall2(cg->builder, cg->future_wait_type,
                           cg->future_wait_fn, call_args, 1, "");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);

        case FutureReady:
            return LLVMBuildCall2(cg->builder, cg->future_ready_type,
                                   cg->future_ready_fn, call_args, 1, "fready");

        case FutureGetRaw:
            return LLVMBuildCall2(cg->builder, cg->future_get_type,
                                   cg->future_get_fn, call_args, 1, "fget");

        case FutureGet: {
            /* block and get the void* result pointer */
            LLVMValueRef raw = LLVMBuildCall2(cg->builder, cg->future_get_type,
                                               cg->future_get_fn, call_args, 1, "fget_raw");
            /* load the typed value from the result buffer */
            LLVMTypeRef llvm_ret_type = get_llvm_type(cg, node->as.future_op.get_type);
            return LLVMBuildLoad2(cg->builder, llvm_ret_type, raw, "fget_val");
        }

        case FutureDrop:
            LLVMBuildCall2(cg->builder, cg->future_drop_type,
                           cg->future_drop_fn, call_args, 1, "");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
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
            diag_begin_error("undefined variable '%s'", target->as.ident.name);
            diag_span(DIAG_NODE(target), True, "not found in this scope");
            diag_note("variables must be declared before use");
            diag_finish();
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
                diag_begin_error("undefined variable '%s'", obj->as.ident.name);
                diag_span(DIAG_NODE(obj), True, "not found in this scope");
                diag_note("variables must be declared before use");
                diag_finish();
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
    } else if (target->kind == NodeSelfMemberExpr) {
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (this_sym) {
            struct_reg_t *sr = find_struct(cg, target->as.self_member.type_name);
            if (sr) {
                LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                LLVMValueRef this_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
                for (usize_t i = 0; i < sr->field_count; i++) {
                    if (strcmp(sr->fields[i].name, target->as.self_member.field) != 0) continue;
                    store_ptr = LLVMBuildStructGEP2(cg->builder, sr->llvm_type, this_ptr,
                                                    (unsigned)sr->fields[i].index, "sgep");
                    store_type = get_llvm_type(cg, sr->fields[i].type);
                    break;
                }
            }
        }
        if (!store_ptr) {
            diag_begin_error("compound assignment target must be assignable");
            diag_span(DIAG_NODE(target), True, "");
            diag_note("only variables and fields can appear on the left side of =");
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
    } else {
        diag_begin_error("compound assignment target must be assignable");
        diag_span(DIAG_NODE(target), True, "");
        diag_note("only variables and fields can appear on the left side of =");
        diag_finish();
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
            diag_begin_error("undefined variable '%s'", target->as.ident.name);
            diag_span(DIAG_NODE(target), True, "not found in this scope");
            diag_note("variables must be declared before use");
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }

        /* cross-domain check when assigning address-of */
        if (node->as.assign.value->kind == NodeAddrOf && sym->stype.is_pointer) {
            node_t *addr_op = node->as.assign.value->as.addr_of.operand;
            if (addr_op->kind == NodeIdentExpr) {
                symbol_t *src = cg_lookup(cg, addr_op->as.ident.name);
                if (src) {
                    if (sym->storage == StorageHeap && src->storage == StorageStack) {
                        diag_begin_error("cannot assign stack address to heap pointer");
                        diag_span(DIAG_NODE(node), True, "");
                        diag_finish();
                    } else if (sym->storage == StorageStack && src->storage == StorageHeap) {
                        diag_begin_error("cannot assign heap address to stack pointer");
                        diag_span(DIAG_NODE(node), True, "");
                        diag_finish();
                    }
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
                diag_begin_error("undefined variable '%s'", obj->as.ident.name);
                diag_span(DIAG_NODE(obj), True, "not found in this scope");
                diag_note("variables must be declared before use");
                diag_finish();
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
                    if (base_sr->fields[fi].array_size > 0) {
                        /* inline array field: 2-index GEP into the array */
                        LLVMTypeRef elem_t = get_llvm_type(cg, base_sr->fields[fi].type);
                        LLVMTypeRef arr_t = LLVMArrayType2(elem_t, (unsigned long long)base_sr->fields[fi].array_size);
                        LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                        index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
                        LLVMValueRef indices[2] = { zero, index_val };
                        LLVMValueRef gep = LLVMBuildGEP2(cg->builder, arr_t, field_gep, indices, 2, "aidx");
                        rhs = coerce_int(cg, rhs, elem_t);
                        LLVMBuildStore(cg->builder, rhs, gep);
                        return rhs;
                    }
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

        if (obj->kind == NodeSelfMemberExpr) {
            /* Type.(field)[idx] / this.field[idx] = rhs — pointer field of current struct instance */
            symbol_t *this_sym = cg_lookup(cg, "this");
            if (this_sym) {
                char *smtn = obj->as.self_member.type_name;
                if (!smtn) smtn = cg->current_struct_name;
                struct_reg_t *sr = find_struct(cg, smtn);
                if (sr) {
                    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                    LLVMValueRef this_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
                    for (usize_t i = 0; i < sr->field_count; i++) {
                        if (strcmp(sr->fields[i].name, obj->as.self_member.field) != 0) continue;
                        LLVMValueRef field_gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type, this_ptr,
                                                                      (unsigned)sr->fields[i].index, "sfgep");
                        LLVMTypeRef ptr_type = get_llvm_type(cg, sr->fields[i].type);
                        LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, "sfptr");
                        type_info_t elem_ti = sr->fields[i].type;
                        elem_ti.is_pointer = False;
                        LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                        index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                        LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "sidx");
                        rhs = coerce_int(cg, rhs, elem_type);
                        LLVMBuildStore(cg->builder, rhs, gep);
                        return rhs;
                    }
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
        if (!type_name) type_name = cg->current_struct_name;
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

    diag_begin_error("invalid assignment target");
    diag_span(DIAG_NODE(target), True, "");
    diag_note("only variables and fields can appear on the left side of =");
    diag_finish();
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
            diag_begin_error("undefined variable '%s'", obj->as.ident.name);
            diag_span(DIAG_NODE(obj), True, "not found in this scope");
            diag_note("variables must be declared before use");
            diag_finish();
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
                if (base_sr->fields[fi].array_size > 0) {
                    /* inline array field: 2-index GEP into the array */
                    LLVMTypeRef elem_t = get_llvm_type(cg, base_sr->fields[fi].type);
                    LLVMTypeRef arr_t = LLVMArrayType2(elem_t, (unsigned long long)base_sr->fields[fi].array_size);
                    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                    index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
                    LLVMValueRef indices[2] = { zero, index_val };
                    LLVMValueRef gep = LLVMBuildGEP2(cg->builder, arr_t, field_gep, indices, 2, "aidx");
                    return LLVMBuildLoad2(cg->builder, elem_t, gep, "aelem");
                }
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

    diag_begin_error("cannot index a non-array or non-pointer type");
    diag_span(DIAG_NODE(node), True, "indexing applied here");
    diag_note("only array types and pointer types support indexing with []");
    diag_finish();
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
                        /* array field: return pointer to first element (array decay) */
                        if (sr->fields[i].array_size > 0) {
                            LLVMTypeRef elem_t = get_llvm_type(cg, sr->fields[i].type);
                            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                            LLVMTypeRef arr_t = LLVMArrayType2(elem_t, (unsigned long long)sr->fields[i].array_size);
                            return LLVMBuildGEP2(cg->builder, arr_t, gep, &zero, 1, field);
                        }
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

    diag_begin_error("cannot resolve member '%s'", field);
    diag_span(DIAG_NODE(node), True, "member not found");
    diag_finish();
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_self_method_call(cg_t *cg, node_t *node) {
    /* Type.(method)(args) / this.method(args) — call a method on the current struct instance */
    char *type_name = node->as.self_method_call.type_name;
    char *method    = node->as.self_method_call.method;
    /* NULL type_name means 'this' keyword was used — resolve from current struct context */
    if (!type_name) type_name = cg->current_struct_name;

    char mangled[256];
    symbol_t *fn_sym = Null;
    /* Try module-prefixed form first (when struct has mod_prefix or current module prefix) */
    {
        struct_reg_t *sr_self = find_struct(cg, type_name);
        const char *pfx = (sr_self && sr_self->mod_prefix && sr_self->mod_prefix[0])
                          ? sr_self->mod_prefix
                          : (cg->current_module_prefix[0] ? cg->current_module_prefix : Null);
        if (pfx) {
            snprintf(mangled, sizeof(mangled), "%s__%s__%s", pfx, type_name, method);
            fn_sym = cg_lookup(cg, mangled);
        }
    }
    if (!fn_sym) {
        snprintf(mangled, sizeof(mangled), "%s.%s", type_name, method);
        fn_sym = cg_lookup(cg, mangled);
    }
    if (!fn_sym) {
        diag_begin_error("undefined method '%s'", mangled);
        diag_span(DIAG_NODE(node), True, "method not found on '%s'", type_name);
        diag_help("define the method with: fn %s.%s(...): ret { ... }", type_name, method);
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    symbol_t *this_sym = cg_lookup(cg, "this");
    LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
    usize_t user_argc = node->as.self_method_call.args.count;
    unsigned n_params = LLVMCountParamTypes(fn_type);

    /* instance methods have an implicit this first param */
    boolean_t is_instance = (this_sym != Null) && ((usize_t)n_params == user_argc + 1);
    usize_t total_argc = user_argc + (is_instance ? 1 : 0);

    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (total_argc > 0) {
        args_heap = allocate(total_argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        usize_t offset = 0;
        if (is_instance) {
            /* load the this pointer from its alloca */
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            args[0] = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
            offset = 1;
        }
        for (usize_t i = 0; i < user_argc; i++)
            args[offset + i] = gen_expr(cg, node->as.self_method_call.args.items[i]);
    }

    /* coerce args to declared parameter types */
    if (total_argc > 0 && n_params > 0) {
        heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
        LLVMTypeRef *param_types = pt_heap.pointer;
        LLVMGetParamTypes(fn_type, param_types);
        for (usize_t i = 0; i < total_argc && i < (usize_t)n_params; i++)
            args[i] = coerce_int(cg, args[i], param_types[i]);
        deallocate(pt_heap);
    }

    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                       args, (unsigned)total_argc, "");
    if (total_argc > 0) deallocate(args_heap);
    return ret;
}

static LLVMValueRef gen_self_member(cg_t *cg, node_t *node) {
    /* Type.(field) / this.field — resolve to this->field */
    char *field = node->as.self_member.field;
    symbol_t *this_sym = cg_lookup(cg, "this");
    if (!this_sym) {
        diag_begin_error("self-member '%s' used outside of method", field);
        diag_span(DIAG_NODE(node), True, "");
        diag_note("'this' is only available inside struct method bodies");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    char *type_name = node->as.self_member.type_name;
    /* NULL type_name means 'this' keyword was used — resolve from current struct context */
    if (!type_name) type_name = cg->current_struct_name;
    struct_reg_t *sr = find_struct(cg, type_name);
    if (!sr) {
        diag_begin_error("unknown struct '%s'", type_name);
        diag_span(DIAG_NODE(node), True, "");
        diag_finish();
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
    diag_begin_error("unknown field '%s' in struct '%s'", field, type_name);
    diag_span(DIAG_NODE(node), True, "");
    diag_note("check that the field/method exists in the struct definition");
    diag_finish();
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

/* ── universal hash ── */

/* MurmurHash3 finalizer: avalanches all bits of a 64-bit integer */
static LLVMValueRef hash_mix_i64(cg_t *cg, LLVMValueRef v) {
    LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
    LLVMValueRef c33 = LLVMConstInt(i64, 33, 0);
    LLVMValueRef m1  = LLVMConstInt(i64, 0xff51afd7ed558ccdULL, 0);
    LLVMValueRef m2  = LLVMConstInt(i64, 0xc4ceb9fe1a85ec53ULL, 0);
    v = LLVMBuildXor(cg->builder, v, LLVMBuildLShr(cg->builder, v, c33, ""), "hm0");
    v = LLVMBuildMul(cg->builder, v, m1, "hm1");
    v = LLVMBuildXor(cg->builder, v, LLVMBuildLShr(cg->builder, v, c33, ""), "hm2");
    v = LLVMBuildMul(cg->builder, v, m2, "hm3");
    v = LLVMBuildXor(cg->builder, v, LLVMBuildLShr(cg->builder, v, c33, ""), "hm4");
    return v;
}

/* Combine two hash values (boost::hash_combine style) */
static LLVMValueRef hash_combine(cg_t *cg, LLVMValueRef seed, LLVMValueRef h) {
    LLVMTypeRef i64  = LLVMInt64TypeInContext(cg->ctx);
    LLVMValueRef phi = LLVMConstInt(i64, 0x9e3779b97f4a7c15ULL, 0);
    LLVMValueRef c6  = LLVMConstInt(i64, 6, 0);
    LLVMValueRef c2  = LLVMConstInt(i64, 2, 0);
    LLVMValueRef tmp = LLVMBuildAdd(cg->builder, h, phi, "");
    tmp = LLVMBuildAdd(cg->builder, tmp, LLVMBuildShl(cg->builder, seed, c6, ""), "");
    tmp = LLVMBuildAdd(cg->builder, tmp, LLVMBuildLShr(cg->builder, seed, c2, ""), "");
    return LLVMBuildXor(cg->builder, seed, tmp, "hcomb");
}

/* Hash a single primitive LLVM value to i64 */
static LLVMValueRef hash_primitive(cg_t *cg, LLVMValueRef val, LLVMTypeRef ty) {
    LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
    LLVMValueRef v64;
    if (LLVMGetTypeKind(ty) == LLVMPointerTypeKind) {
        v64 = LLVMBuildPtrToInt(cg->builder, val, i64, "htoi");
    } else if (ty == LLVMFloatTypeInContext(cg->ctx)) {
        LLVMValueRef i32v = LLVMBuildBitCast(cg->builder, val,
                                              LLVMInt32TypeInContext(cg->ctx), "fbc");
        v64 = LLVMBuildZExt(cg->builder, i32v, i64, "fext");
    } else if (ty == LLVMDoubleTypeInContext(cg->ctx)) {
        v64 = LLVMBuildBitCast(cg->builder, val, i64, "dbc");
    } else if (ty == i64) {
        v64 = val;
    } else {
        v64 = LLVMBuildZExt(cg->builder, val, i64, "iext");
    }
    return hash_mix_i64(cg, v64);
}

/* Forward declaration for recursion */
static LLVMValueRef hash_struct_default(cg_t *cg, LLVMValueRef val, struct_reg_t *sr);

static LLVMValueRef hash_value(cg_t *cg, LLVMValueRef val, LLVMTypeRef ty) {
    LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
        const char *sname = LLVMGetStructName(ty);
        if (sname) {
            char mname[256];
            snprintf(mname, sizeof(mname), "%s.hash", sname);
            LLVMValueRef hfn = LLVMGetNamedFunction(cg->module, mname);
            if (hfn) {
                LLVMValueRef tmp = alloc_in_entry(cg, ty, "hs_tmp");
                LLVMBuildStore(cg->builder, val, tmp);
                LLVMTypeRef fty = LLVMGlobalGetValueType(hfn);
                LLVMValueRef ca[1] = { tmp };
                LLVMValueRef r = LLVMBuildCall2(cg->builder, fty, hfn, ca, 1, "fh");
                return coerce_int(cg, r, i64);
            }
            struct_reg_t *fsr = find_struct(cg, sname);
            if (fsr) return hash_struct_default(cg, val, fsr);
        }
        return LLVMConstInt(i64, 0, 0);
    }
    return hash_primitive(cg, val, ty);
}

static LLVMValueRef hash_struct_default(cg_t *cg, LLVMValueRef val, struct_reg_t *sr) {
    LLVMTypeRef i64  = LLVMInt64TypeInContext(cg->ctx);
    LLVMValueRef seed = LLVMConstInt(i64, 0, 0);
    for (usize_t i = 0; i < sr->field_count; i++) {
        field_info_t *f = &sr->fields[i];
        LLVMValueRef fval = LLVMBuildExtractValue(cg->builder, val, (unsigned)f->index, "hfv");
        LLVMValueRef fh   = hash_value(cg, fval, LLVMTypeOf(fval));
        seed = hash_combine(cg, seed, fh);
    }
    return seed;
}

static LLVMValueRef gen_hash(cg_t *cg, node_t *node) {
    LLVMValueRef val = gen_expr(cg, node->as.hash_expr.operand);
    LLVMTypeRef  ty  = LLVMTypeOf(val);
    LLVMTypeRef  i64 = LLVMInt64TypeInContext(cg->ctx);

    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
        const char *sname = LLVMGetStructName(ty);
        if (sname) {
            char mname[256];
            snprintf(mname, sizeof(mname), "%s.hash", sname);
            LLVMValueRef hfn = LLVMGetNamedFunction(cg->module, mname);
            if (hfn) {
                LLVMValueRef tmp = alloc_in_entry(cg, ty, "hash_self");
                LLVMBuildStore(cg->builder, val, tmp);
                LLVMTypeRef fty = LLVMGlobalGetValueType(hfn);
                LLVMValueRef ca[1] = { tmp };
                LLVMValueRef r = LLVMBuildCall2(cg->builder, fty, hfn, ca, 1, "hash_r");
                return coerce_int(cg, r, i64);
            }
            struct_reg_t *sr = find_struct(cg, sname);
            if (sr) return hash_struct_default(cg, val, sr);
        }
        return LLVMConstInt(i64, 0, 0);
    }
    return hash_primitive(cg, val, ty);
}

static LLVMValueRef gen_sizeof(cg_t *cg, node_t *node) {
    type_info_t ti = node->as.sizeof_expr.type;
    /* If the "type" is a bare user name with no pointer qualifier, check whether
       it is actually a variable — if so, use the variable's LLVM type instead.
       This makes sizeof.(my_var) work alongside sizeof.(MyStruct). */
    if (ti.base == TypeUser && !ti.is_pointer && ti.user_name) {
        symbol_t *sym = cg_lookup(cg, ti.user_name);
        if (sym) return LLVMSizeOf(sym->type);
    }
    LLVMTypeRef ty = get_llvm_type(cg, ti);
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
            diag_begin_error("undefined variable '%s'", operand->as.ident.name);
            diag_span(DIAG_NODE(operand), True, "not found in this scope");
            diag_note("variables must be declared before use");
            diag_finish();
            return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
        }
        if (sym->flags & SymHeapVar) {
            /* heap var: address is the malloc'd pointer */
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            return LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        }
        return sym->value; /* alloca = address of the stack variable */
    }
    /* &arr[i] / &ptr[i] — address of an indexed element (GEP, no load) */
    if (operand->kind == NodeIndexExpr) {
        node_t *obj = operand->as.index_expr.object;
        LLVMValueRef index_val = gen_expr(cg, operand->as.index_expr.index);
        if (obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
            if (!sym) {
                diag_begin_error("undefined variable '%s'", obj->as.ident.name);
                diag_span(DIAG_NODE(obj), True, "not found in this scope");
                diag_finish();
                return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
            }
            if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind) {
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
                LLVMValueRef indices[2] = { zero, index_val };
                return LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "aidxptr");
            }
            if (llvm_is_ptr(sym->type)) {
                LLVMValueRef ptr = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "ptr");
                type_info_t elem_ti = sym->stype;
                elem_ti.is_pointer = False;
                LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                return LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidxptr");
            }
        }
    }
    diag_begin_error("address-of requires an lvalue");
    diag_span(DIAG_NODE(operand), True, "");
    diag_finish();
    return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
}

/* ── error expression ── */

static char error_decode_esc(char c) {
    switch (c) {
        case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
        case '\\': return '\\'; case '\'': return '\''; case '"': return '"';
        case 'a': return '\a'; case 'b': return '\b'; case '0': return '\0';
        default: return c;
    }
}

static LLVMValueRef gen_error_expr(cg_t *cg, node_t *node) {
    const char  *fmt  = node->as.error_expr.fmt;
    usize_t      flen = node->as.error_expr.fmt_len;
    node_list_t *args = &node->as.error_expr.args;

    LLVMValueRef msg_ptr;

    if (args->count == 0) {
        /* No format args: decode escape sequences and store as a static string. */
        heap_t bh = allocate(flen + 1, 1);
        char *buf = bh.pointer;
        usize_t blen = 0;
        for (usize_t i = 0; i < flen; ) {
            if (fmt[i] == '\\' && i + 1 < flen) {
                char d = error_decode_esc(fmt[i + 1]);
                if (d != '\0') buf[blen++] = d;
                i += 2;
            } else {
                buf[blen++] = fmt[i++];
            }
        }
        buf[blen] = '\0';
        msg_ptr = LLVMBuildGlobalStringPtr(cg->builder, buf, "errmsg");
        deallocate(bh);
    } else {
        /* Format args: build a C printf format string at compile time, then call
           malloc + snprintf at runtime to produce the formatted error message. */
        usize_t argc = args->count;

        /* Evaluate all argument expressions. */
        heap_t vh = allocate(argc, sizeof(LLVMValueRef));
        LLVMValueRef *vals = vh.pointer;
        for (usize_t i = 0; i < argc; i++)
            vals[i] = gen_expr(cg, args->items[i]);

        /* Build the C printf format string from the stasha format template.
           Each {} placeholder is translated to an appropriate printf specifier
           based on the LLVM type of the corresponding argument. */
        heap_t fh = allocate(flen * 4 + 64, 1);
        char *cfmt = fh.pointer;
        usize_t cfmt_len = 0;
        usize_t arg_idx = 0;

        for (usize_t i = 0; i < flen; ) {
            /* backslash escape */
            if (fmt[i] == '\\' && i + 1 < flen) {
                char ec = fmt[i + 1];
                if (ec == '{' || ec == '}') { cfmt[cfmt_len++] = ec; }
                else if (ec == '%')         { cfmt[cfmt_len++] = '%'; cfmt[cfmt_len++] = '%'; }
                else {
                    char d = error_decode_esc(ec);
                    if (d != '\0') cfmt[cfmt_len++] = d;
                }
                i += 2; continue;
            }
            /* literal '%' must be doubled for printf */
            if (fmt[i] == '%') { cfmt[cfmt_len++] = '%'; cfmt[cfmt_len++] = '%'; i++; continue; }
            /* {} placeholder */
            if (fmt[i] == '{') {
                usize_t j = i + 1;
                while (j < flen && fmt[j] != '}') j++;
                if (j < flen && arg_idx < argc) {
                    LLVMTypeRef ty = LLVMTypeOf(vals[arg_idx]);
                    const char *spec = fmt + i + 1;
                    usize_t slen = j - (i + 1);
                    if (slen > 0 && spec[0] == ':') { spec++; slen--; }
                    cfmt[cfmt_len++] = '%';
                    if (slen > 0) {
                        for (usize_t k = 0; k < slen; k++) cfmt[cfmt_len++] = spec[k];
                    } else if (llvm_is_ptr(ty)) {
                        cfmt[cfmt_len++] = 's';
                    } else if (llvm_is_float(ty)) {
                        cfmt[cfmt_len++] = 'g';
                        if (ty == LLVMFloatTypeInContext(cg->ctx))
                            vals[arg_idx] = LLVMBuildFPExt(cg->builder, vals[arg_idx],
                                                            LLVMDoubleTypeInContext(cg->ctx), "fpext");
                    } else if (ty == LLVMInt64TypeInContext(cg->ctx)) {
                        cfmt[cfmt_len++] = 'l'; cfmt[cfmt_len++] = 'l'; cfmt[cfmt_len++] = 'd';
                    } else if (ty == LLVMInt8TypeInContext(cg->ctx)) {
                        cfmt[cfmt_len++] = 'c';
                        vals[arg_idx] = LLVMBuildSExt(cg->builder, vals[arg_idx],
                                                       LLVMInt32TypeInContext(cg->ctx), "cext");
                    } else if (ty == LLVMInt1TypeInContext(cg->ctx)) {
                        cfmt[cfmt_len++] = 'd';
                        vals[arg_idx] = LLVMBuildZExt(cg->builder, vals[arg_idx],
                                                       LLVMInt32TypeInContext(cg->ctx), "bext");
                    } else {
                        cfmt[cfmt_len++] = 'd';
                        if (ty != LLVMInt32TypeInContext(cg->ctx))
                            vals[arg_idx] = LLVMBuildSExt(cg->builder, vals[arg_idx],
                                                           LLVMInt32TypeInContext(cg->ctx), "iext");
                    }
                    arg_idx++;
                    i = j + 1;
                    continue;
                }
                /* no closing brace or ran out of args: emit literal '{' */
                cfmt[cfmt_len++] = fmt[i++];
                continue;
            }
            cfmt[cfmt_len++] = fmt[i++];
        }
        cfmt[cfmt_len] = '\0';

        /* Declare snprintf if not already in the module. */
        LLVMValueRef snprintf_fn = LLVMGetNamedFunction(cg->module, "snprintf");
        if (!snprintf_fn) {
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
            /* snprintf(char *buf, size_t n, const char *fmt, ...) — fmt is fixed */
            LLVMTypeRef fixed[3] = { ptr_ty, i64_ty, ptr_ty };
            LLVMTypeRef snfty = LLVMFunctionType(
                LLVMInt32TypeInContext(cg->ctx), fixed, 3, /*varargs=*/True);
            snprintf_fn = LLVMAddFunction(cg->module, "snprintf", snfty);
        }
        LLVMTypeRef snprintf_type = LLVMGlobalGetValueType(snprintf_fn);

        /* Allocate a 512-byte heap buffer for the formatted message. */
        LLVMValueRef buf_size = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 512, 0);
        LLVMValueRef buf = LLVMBuildCall2(cg->builder, cg->malloc_type, cg->malloc_fn,
                                          &buf_size, 1, "errbuf");

        /* Call snprintf(buf, 512, cfmt_str, arg0, arg1, ...) */
        usize_t sn_argc = 3 + arg_idx;
        heap_t sah = allocate(sn_argc, sizeof(LLVMValueRef));
        LLVMValueRef *sn_args = sah.pointer;
        sn_args[0] = buf;
        sn_args[1] = buf_size;
        sn_args[2] = LLVMBuildGlobalStringPtr(cg->builder, cfmt, "errfmt");
        for (usize_t i = 0; i < arg_idx; i++)
            sn_args[3 + i] = vals[i];
        LLVMBuildCall2(cg->builder, snprintf_type, snprintf_fn,
                       sn_args, (unsigned)sn_argc, "");

        msg_ptr = buf;
        deallocate(sah);
        deallocate(fh);
        deallocate(vh);
    }

    LLVMValueRef err = LLVMGetUndef(cg->error_type);
    err = LLVMBuildInsertValue(cg->builder, err,
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0), 0, "err.has");
    err = LLVMBuildInsertValue(cg->builder, err, msg_ptr, 1, "err.msg");
    return err;
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
        case NodeThreadCall:       return gen_thread_call(cg, node);
        case NodeFutureOp:         return gen_future_op(cg, node);
        case NodeCompoundAssign:   return gen_compound_assign(cg, node);
        case NodeAssignExpr:       return gen_assign(cg, node);
        case NodeIndexExpr:        return gen_index(cg, node);
        case NodeMemberExpr:       return gen_member(cg, node);
        case NodeSelfMemberExpr:   return gen_self_member(cg, node);
        case NodeSelfMethodCall:   return gen_self_method_call(cg, node);
        case NodeTernaryExpr:      return gen_ternary(cg, node);
        case NodeCastExpr:         return gen_cast(cg, node);
        case NodeNewExpr:          return gen_new(cg, node);
        case NodeSizeofExpr:       return gen_sizeof(cg, node);
        case NodeHashExpr:         return gen_hash(cg, node);
        case NodeNilExpr:          return gen_nil(cg);
        case NodeMovExpr:          return gen_mov(cg, node);
        case NodeAddrOf:           return gen_addr_of(cg, node);
        case NodeErrorExpr:        return gen_error_expr(cg, node);
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
                diag_begin_error("unknown struct '%s' in designated initializer",
                        node->as.desig_init.type_name);
                diag_finish();
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
            diag_begin_error("unexpected node kind %d in expression", node->kind);
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}
