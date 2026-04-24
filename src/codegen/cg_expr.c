/* ── expressions ── */

/* forward declaration — defined in cg_generics.c */
static type_info_t subst_type_info(cg_t *cg, type_info_t ti);

/* forward declarations for any-type helpers defined later in this file */
static struct_reg_t *try_instantiate_any(cg_t *cg, const char *name);
static LLVMValueRef wrap_in_any(cg_t *cg, struct_reg_t *sr, LLVMValueRef val, usize_t variant_idx);

/* forward declarations for slice helpers defined later in this file */
static LLVMTypeRef get_slice_elem_llvm_type(cg_t *cg, type_info_t elem_ti);
static LLVMTypeRef slice_struct_type(cg_t *cg);
static LLVMTypeRef elem_type_from_sym(cg_t *cg, symbol_t *sym);

/* Find the struct registry entry whose LLVM type matches ty.
 * This is more reliable than LLVMGetStructName + find_struct because the
 * LLVM struct name is mangled (e.g. "dstring__dstring_t") while the registry
 * key is the plain name (e.g. "dstring_t"). */
static struct_reg_t *find_struct_by_llvm_type(cg_t *cg, LLVMTypeRef ty) {
    for (usize_t i = 0; i < cg->struct_count; i++) {
        if (cg->structs[i].llvm_type == ty) return &cg->structs[i];
    }
    return Null;
}

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

static boolean_t const_int_value(node_t *node, long *out) {
    if (!node) return False;
    if (node->kind == NodeIntLitExpr) {
        if (out) *out = node->as.int_lit.value;
        return True;
    }
    if (node->kind == NodeUnaryPrefixExpr && node->as.unary.op == TokMinus
            && node->as.unary.operand
            && node->as.unary.operand->kind == NodeIntLitExpr) {
        if (out) *out = -node->as.unary.operand->as.int_lit.value;
        return True;
    }
    return False;
}

static long count_range_values(node_t *node, boolean_t *ok) {
    long start = 0, end = 0, step = 1;
    if (!node || node->kind != NodeRangeExpr
            || !const_int_value(node->as.range_expr.start, &start)
            || !const_int_value(node->as.range_expr.end, &end)) {
        if (ok) *ok = False;
        return 0;
    }
    if (node->as.range_expr.step && !const_int_value(node->as.range_expr.step, &step)) {
        if (ok) *ok = False;
        return 0;
    }
    if (step == 0) {
        if (ok) *ok = False;
        return 0;
    }

    long count = 0;
    if (step > 0) {
        long limit = node->as.range_expr.inclusive ? end + 1 : end;
        for (long v = start; v < limit; v += step) count++;
    } else {
        long limit = node->as.range_expr.inclusive ? end - 1 : end;
        for (long v = start; v > limit; v += step) count++;
    }
    if (ok) *ok = True;
    return count;
}

static long count_spread_values(cg_t *cg, node_t *node, boolean_t *ok);

static long count_compound_init_values(cg_t *cg, node_t *node, boolean_t *needs_trailing_nul, boolean_t *ok) {
    if (!node || node->kind != NodeCompoundInit) {
        if (ok) *ok = False;
        return 0;
    }

    long cursor = 0;
    long max_index = 0;
    boolean_t local_ok = True;
    boolean_t saw_string_spread = False;

    for (usize_t i = 0; i < node->as.compound_init.items.count; i++) {
        node_t *item = node->as.compound_init.items.items[i];
        if (item->kind == NodeInitIndex) {
            long idx = 0;
            if (!const_int_value(item->as.init_index.index, &idx) || idx < 0) {
                local_ok = False;
                break;
            }
            cursor = idx;
            if (cursor + 1 > max_index) max_index = cursor + 1;
            cursor += 1;
            continue;
        }
        if (item->kind == NodeSpreadExpr) {
            if (item->as.spread_expr.expr && item->as.spread_expr.expr->kind == NodeStrLitExpr)
                saw_string_spread = True;
            long n = count_spread_values(cg, item->as.spread_expr.expr, &local_ok);
            if (!local_ok) break;
            cursor += n;
            if (cursor > max_index) max_index = cursor;
            continue;
        }
        if (item->kind == NodeRangeExpr) {
            long n = count_range_values(item, &local_ok);
            if (!local_ok) break;
            cursor += n;
            if (cursor > max_index) max_index = cursor;
            continue;
        }
        cursor += 1;
        if (cursor > max_index) max_index = cursor;
    }

    if (needs_trailing_nul) *needs_trailing_nul = saw_string_spread;
    if (ok) *ok = local_ok;
    return max_index;
}

static long count_spread_values(cg_t *cg, node_t *node, boolean_t *ok) {
    if (!node) {
        if (ok) *ok = False;
        return 0;
    }
    if (node->kind == NodeStrLitExpr) {
        if (ok) *ok = True;
        return (long)node->as.str_lit.len;
    }
    if (node->kind == NodeRangeExpr)
        return count_range_values(node, ok);
    if (node->kind == NodeCompoundInit)
        return count_compound_init_values(cg, node, Null, ok);
    if (node->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, node->as.ident.name);
        if (sym && sym->array_size >= 0) {
            if (ok) *ok = True;
            return sym->array_size;
        }
    }
    if (ok) *ok = False;
    return 0;
}

static LLVMValueRef gen_compound_init(cg_t *cg, node_t *node);

static void store_array_value(cg_t *cg, LLVMValueRef tmp, LLVMTypeRef arr_ty,
                              LLVMTypeRef elem_ty, long idx, node_t *value_node) {
    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    LLVMValueRef idx_val = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned long long)idx, 0);
    LLVMValueRef indices[2] = { zero, idx_val };
    LLVMValueRef gep = LLVMBuildGEP2(cg->builder, arr_ty, tmp, indices, 2, "init.idx");
    LLVMTypeRef saved_hint = cg->hint_ret_type;
    cg->hint_ret_type = elem_ty;
    LLVMValueRef val = gen_expr(cg, value_node);
    cg->hint_ret_type = saved_hint;
    if (LLVMTypeOf(val) != elem_ty)
        val = coerce_int(cg, val, elem_ty);
    LLVMBuildStore(cg->builder, val, gep);
}

static boolean_t emit_range_values(cg_t *cg, LLVMValueRef tmp, LLVMTypeRef arr_ty,
                                   LLVMTypeRef elem_ty, long *cursor, node_t *node) {
    long start = 0, end = 0, step = 1;
    if (!const_int_value(node->as.range_expr.start, &start)
            || !const_int_value(node->as.range_expr.end, &end)
            || (node->as.range_expr.step && !const_int_value(node->as.range_expr.step, &step))
            || step == 0) {
        diag_begin_error("range bounds and step must be compile-time integer literals");
        diag_span(DIAG_NODE(node), True, "range used here");
        diag_finish();
        return False;
    }

    if (step > 0) {
        long limit = node->as.range_expr.inclusive ? end + 1 : end;
        for (long v = start; v < limit; v += step) {
            node_t fake = {0};
            fake.kind = NodeIntLitExpr;
            fake.as.int_lit.value = v;
            store_array_value(cg, tmp, arr_ty, elem_ty, *cursor, &fake);
            (*cursor)++;
        }
    } else {
        long limit = node->as.range_expr.inclusive ? end - 1 : end;
        for (long v = start; v > limit; v += step) {
            node_t fake = {0};
            fake.kind = NodeIntLitExpr;
            fake.as.int_lit.value = v;
            store_array_value(cg, tmp, arr_ty, elem_ty, *cursor, &fake);
            (*cursor)++;
        }
    }
    return True;
}

static boolean_t emit_spread_values(cg_t *cg, LLVMValueRef tmp, LLVMTypeRef arr_ty,
                                    LLVMTypeRef elem_ty, long *cursor, node_t *expr) {
    if (!expr) return False;

    if (expr->kind == NodeStrLitExpr) {
        for (usize_t i = 0; i < expr->as.str_lit.len; i++) {
            node_t fake = {0};
            fake.kind = NodeCharLitExpr;
            fake.as.char_lit.value = expr->as.str_lit.value[i];
            store_array_value(cg, tmp, arr_ty, elem_ty, *cursor, &fake);
            (*cursor)++;
        }
        return True;
    }

    if (expr->kind == NodeRangeExpr)
        return emit_range_values(cg, tmp, arr_ty, elem_ty, cursor, expr);

    if (expr->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, expr->as.ident.name);
        if (sym && LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind && sym->array_size >= 0) {
            LLVMTypeRef src_elem_ty = LLVMGetElementType(sym->type);
            for (long i = 0; i < sym->array_size; i++) {
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                LLVMValueRef src_idx = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned long long)i, 0);
                LLVMValueRef src_indices[2] = { zero, src_idx };
                LLVMValueRef src_gep = LLVMBuildGEP2(cg->builder, sym->type, sym->value, src_indices, 2, "spread.src");
                LLVMValueRef val = LLVMBuildLoad2(cg->builder, src_elem_ty, src_gep, "spread.val");

                LLVMValueRef dst_idx = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned long long)(*cursor), 0);
                LLVMValueRef dst_indices[2] = { zero, dst_idx };
                LLVMValueRef dst_gep = LLVMBuildGEP2(cg->builder, arr_ty, tmp, dst_indices, 2, "spread.dst");
                if (src_elem_ty != elem_ty)
                    val = coerce_int(cg, val, elem_ty);
                LLVMBuildStore(cg->builder, val, dst_gep);
                (*cursor)++;
            }
            return True;
        }
    }

    diag_begin_error("unsupported spread expression in compound initializer");
    diag_span(DIAG_NODE(expr), True, "cannot spread this value");
    diag_finish();
    return False;
}

static boolean_t store_struct_field(cg_t *cg, LLVMValueRef tmp, struct_reg_t *sr,
                                    const char *field_name, node_t *value_node) {
    for (usize_t i = 0; i < sr->field_count; i++) {
        if (strcmp(sr->fields[i].name, field_name) != 0) continue;
        LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type, tmp,
                                               (unsigned)sr->fields[i].index, field_name);
        LLVMTypeRef field_ty = get_llvm_type(cg, sr->fields[i].type);
        if (sr->fields[i].array_size > 0)
            field_ty = LLVMArrayType2(field_ty, (unsigned long long)sr->fields[i].array_size);
        LLVMTypeRef saved_hint = cg->hint_ret_type;
        cg->hint_ret_type = field_ty;
        LLVMValueRef val = gen_expr(cg, value_node);
        cg->hint_ret_type = saved_hint;
        if (LLVMTypeOf(val) != field_ty)
            val = coerce_int(cg, val, field_ty);
        LLVMBuildStore(cg->builder, val, gep);
        return True;
    }

    diag_begin_error("unknown field '%s' in struct '%s'", field_name, sr->name);
    diag_finish();
    return False;
}

static LLVMValueRef gen_compound_init(cg_t *cg, node_t *node) {
    LLVMTypeRef target_ty = cg->hint_ret_type;
    if (!target_ty) {
        diag_begin_error("compound initializer requires a known target type");
        diag_span(DIAG_NODE(node), True, "target type is not known here");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    if (LLVMGetTypeKind(target_ty) == LLVMArrayTypeKind) {
        LLVMTypeRef elem_ty = LLVMGetElementType(target_ty);
        LLVMValueRef tmp = alloc_in_entry(cg, target_ty, "compound_arr");
        LLVMBuildStore(cg->builder, LLVMConstNull(target_ty), tmp);
        long cursor = 0;

        for (usize_t i = 0; i < node->as.compound_init.items.count; i++) {
            node_t *item = node->as.compound_init.items.items[i];
            if (item->kind == NodeInitField) {
                diag_begin_error("field designators are not valid in array initializers");
                diag_span(DIAG_NODE(item), True, "used here");
                diag_finish();
                continue;
            }
            if (item->kind == NodeInitIndex) {
                long idx = 0;
                if (!const_int_value(item->as.init_index.index, &idx) || idx < 0) {
                    diag_begin_error("array designator index must be a non-negative integer literal");
                    diag_span(DIAG_NODE(item), True, "used here");
                    diag_finish();
                    continue;
                }
                cursor = idx;
                store_array_value(cg, tmp, target_ty, elem_ty, cursor, item->as.init_index.value);
                cursor += 1;
                continue;
            }
            if (item->kind == NodeSpreadExpr) {
                emit_spread_values(cg, tmp, target_ty, elem_ty, &cursor, item->as.spread_expr.expr);
                continue;
            }
            if (item->kind == NodeRangeExpr) {
                emit_range_values(cg, tmp, target_ty, elem_ty, &cursor, item);
                continue;
            }
            store_array_value(cg, tmp, target_ty, elem_ty, cursor, item);
            cursor += 1;
        }

        return LLVMBuildLoad2(cg->builder, target_ty, tmp, "compound_arr_val");
    }

    if (LLVMGetTypeKind(target_ty) == LLVMStructTypeKind) {
        struct_reg_t *sr = find_struct_by_llvm_type(cg, target_ty);
        if (!sr) {
            diag_begin_error("compound initializer target is not a known struct type");
            diag_span(DIAG_NODE(node), True, "used here");
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }

        LLVMValueRef tmp = alloc_in_entry(cg, target_ty, "compound_struct");
        LLVMBuildStore(cg->builder, LLVMConstNull(target_ty), tmp);

        usize_t cursor = 0;
        for (usize_t i = 0; i < node->as.compound_init.items.count; i++) {
            node_t *item = node->as.compound_init.items.items[i];
            if (item->kind == NodeSpreadExpr) {
                LLVMTypeRef saved_hint = cg->hint_ret_type;
                cg->hint_ret_type = target_ty;
                LLVMValueRef val = gen_expr(cg, item->as.spread_expr.expr);
                cg->hint_ret_type = saved_hint;
                LLVMBuildStore(cg->builder, val, tmp);
                cursor = sr->field_count;
                continue;
            }
            if (item->kind == NodeInitField) {
                store_struct_field(cg, tmp, sr, item->as.init_field.name, item->as.init_field.value);
                continue;
            }
            /* positional: assign to field at current cursor position */
            if (cursor < sr->field_count) {
                store_struct_field(cg, tmp, sr, sr->fields[cursor].name, item);
                cursor++;
                continue;
            }
            diag_begin_error("too many values in struct compound initializer '%s'", sr->name);
            diag_span(DIAG_NODE(item), True, "extra value here");
            diag_finish();
        }

        return LLVMBuildLoad2(cg->builder, target_ty, tmp, "compound_struct_val");
    }

    diag_begin_error("compound initializer target must be an array or struct");
    diag_span(DIAG_NODE(node), True, "used here");
    diag_finish();
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
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
        char dedup_key[600];
        snprintf(dedup_key, sizeof(dedup_key), "undef_var:%s", node->as.ident.name);
        if (!cg_error_already_reported(cg, dedup_key)) {
            diag_begin_error("undefined variable '%s'", node->as.ident.name);
            diag_set_category(ErrCatUndefined);
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
        }
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

/* ── comparison chain: x > 10 and < 20 / x == 1 or 2 or 3 ──
 *
 * base_expr is evaluated exactly once.  Conditions are combined with integer
 * AND/OR (non-short-circuit) on the i1 results; AND binds tighter than OR.
 *
 * Precedence grouping: accumulate an AND-sub-result, flush to OR whenever an
 * OR connector is encountered, then OR all flushed groups at the end.
 * Example: cond[0] OR cond[1] AND cond[2]
 *   → or_acc = cond[0], and_res = AND(cond[1], cond[2])
 *   → final  = OR(or_acc, and_res)                                           */

static LLVMValueRef gen_single_cmp(cg_t *cg, LLVMValueRef base_val,
                                   token_kind_t op, LLVMValueRef rhs_val) {
    boolean_t is_fp = llvm_is_float(LLVMTypeOf(base_val));
    switch (op) {
        case TokLt:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOLT, base_val, rhs_val, "clt")
                         : LLVMBuildICmp(cg->builder, LLVMIntSLT,  base_val, rhs_val, "clt");
        case TokGt:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOGT, base_val, rhs_val, "cgt")
                         : LLVMBuildICmp(cg->builder, LLVMIntSGT,  base_val, rhs_val, "cgt");
        case TokLtEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOLE, base_val, rhs_val, "cle")
                         : LLVMBuildICmp(cg->builder, LLVMIntSLE,  base_val, rhs_val, "cle");
        case TokGtEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOGE, base_val, rhs_val, "cge")
                         : LLVMBuildICmp(cg->builder, LLVMIntSGE,  base_val, rhs_val, "cge");
        case TokEqEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOEQ, base_val, rhs_val, "ceq")
                         : LLVMBuildICmp(cg->builder, LLVMIntEQ,   base_val, rhs_val, "ceq");
        case TokBangEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealONE, base_val, rhs_val, "cne")
                         : LLVMBuildICmp(cg->builder, LLVMIntNE,   base_val, rhs_val, "cne");
        default:
            return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0);
    }
}

static LLVMValueRef gen_cmp_chain(cg_t *cg, node_t *node) {
    /* evaluate base once — handles getValue() > 10 and < 20 safely */
    LLVMValueRef base_val = gen_expr(cg, node->as.cmp_chain.base_expr);
    usize_t count = node->as.cmp_chain.count;

    /* evaluate all comparison results eagerly */
    LLVMValueRef cmp[CMP_CHAIN_MAX];
    for (usize_t i = 0; i < count; i++) {
        LLVMValueRef rhs_val = gen_expr(cg, node->as.cmp_chain.rhs_nodes[i]);
        /* coerce rhs to match base type when they are integer constants */
        if (!llvm_is_float(LLVMTypeOf(base_val))
                && LLVMTypeOf(rhs_val) != LLVMTypeOf(base_val)
                && LLVMGetTypeKind(LLVMTypeOf(rhs_val)) == LLVMIntegerTypeKind)
            rhs_val = coerce_int(cg, rhs_val, LLVMTypeOf(base_val));
        cmp[i] = gen_single_cmp(cg, base_val, node->as.cmp_chain.cmp_ops[i], rhs_val);
    }

    /* combine respecting AND > OR precedence:
     * scan left-to-right; accumulate an AND-group; flush on OR connector      */
    LLVMValueRef and_res  = cmp[0];
    LLVMValueRef or_acc   = Null;   /* accumulated OR operands */

    for (usize_t i = 1; i < count; i++) {
        if (node->as.cmp_chain.logical_ops[i] == 0) {
            /* AND: fold into current AND group */
            and_res = LLVMBuildAnd(cg->builder, and_res, cmp[i], "chain.and");
        } else {
            /* OR: flush current AND group into or_acc, start new AND group */
            or_acc  = or_acc ? LLVMBuildOr(cg->builder, or_acc, and_res, "chain.or")
                             : and_res;
            and_res = cmp[i];
        }
    }

    /* combine final AND group with any pending OR accumulator */
    return or_acc ? LLVMBuildOr(cg->builder, or_acc, and_res, "chain.final")
                  : and_res;
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
            const char *_sm_tname = operand->as.self_member.type_name;
            if (!_sm_tname) _sm_tname = cg->current_struct_name;
            struct_reg_t *sr = _sm_tname ? find_struct(cg, _sm_tname) : Null;
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
            type_info_t elem_ti = ti_deref_one(sym->stype);
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
    /* pointer dereference: *ptr — load the value the pointer points to.
     * For multi-level dereferences like **pp or ***ppp, walk the chain of
     * unary-star nodes to the root identifier, count total depth, then apply
     * ti_deref_one that many times to determine the correct pointee type. */
    if (node->as.unary.op == TokStar) {
        LLVMValueRef ptr = gen_expr(cg, node->as.unary.operand);
        LLVMTypeRef pointee_type = Null;

        /* walk the * chain to find the root identifier and total deref count */
        node_t *inner = node->as.unary.operand;
        int deref_depth = 1; /* how many times WE dereference from here */
        while (inner->kind == NodeUnaryPrefixExpr && inner->as.unary.op == TokStar) {
            deref_depth++;
            inner = inner->as.unary.operand;
        }
        if (inner->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, inner->as.ident.name);
            if (sym && sym->stype.is_pointer) {
                type_info_t pt = sym->stype;
                for (int i = 0; i < deref_depth && pt.is_pointer; i++)
                    pt = ti_deref_one(pt);
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
        const char *_psm_tname = operand->as.self_member.type_name;
        if (!_psm_tname) _psm_tname = cg->current_struct_name;
        struct_reg_t *sr = _psm_tname ? find_struct(cg, _psm_tname) : Null;
        /* In a generic instantiation the AST still holds the template base name;
         * fall back to the mangled instantiated name stored in current_struct_name. */
        if (!sr && cg->current_struct_name)
            sr = find_struct(cg, cg->current_struct_name);
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
        type_info_t elem_ti = ti_deref_one(sym->stype);
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

/* Try to coerce an already-generated arg value to an interface fat-pointer type.
   If param_type is an interface fat-pointer ({ ptr, ptr }) and arg is a concrete struct,
   allocate a temporary, store the struct, get its pointer, and build a fat pointer.
   arg_node is used to find the struct name via symbol lookup.
   Returns the coerced value, or the original arg_val if no coercion is needed. */
static LLVMValueRef try_coerce_arg_to_iface(cg_t *cg, LLVMValueRef arg_val,
                                              LLVMTypeRef param_type, node_t *arg_node) {
    if (!arg_val || !param_type) return arg_val;
    LLVMTypeRef arg_type = LLVMTypeOf(arg_val);
    if (arg_type == param_type) return arg_val; /* already matches */

    /* check if param_type is an interface fat-pointer type by looking up the
       struct registry entry — more robust than direct pointer comparison */
    struct_reg_t *param_sr = find_struct_by_llvm_type(cg, param_type);
    if (!param_sr || !param_sr->is_interface) return arg_val; /* not an interface */
    usize_t iface_ii = find_interface_index(cg, param_sr->name);
    if (iface_ii == (usize_t)-1) return arg_val;

    /* find the struct name from the arg node */
    const char *struct_name = Null;
    if (arg_node && arg_node->kind == NodeIdentExpr) {
        symbol_t *asym = cg_lookup(cg, arg_node->as.ident.name);
        if (asym && asym->stype.user_name) struct_name = asym->stype.user_name;
    }
    if (!struct_name) return arg_val; /* can't determine source struct */

    /* ensure vtable exists */
    usize_t vi = ensure_vtable(cg, struct_name, cg->interfaces[iface_ii].name);
    if (vi == (usize_t)-1) return arg_val;

    /* get a pointer to the struct value: store arg in a temp alloca */
    LLVMValueRef tmp = alloc_in_entry(cg, arg_type, "iface_coerce_tmp");
    LLVMBuildStore(cg->builder, arg_val, tmp);

    return construct_fat_ptr(cg, struct_name, cg->interfaces[iface_ii].name, tmp);
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
    /* lazy instantiation for standalone @comptime[T] generic functions */
    if (!sym && strstr(node->as.call.callee, "_G_")) {
        sym = try_instantiate_generic_fn(cg, node->as.call.callee);
    }
    if (!sym) {
        char dedup_key[600];
        snprintf(dedup_key, sizeof(dedup_key), "undef_fn:%s", node->as.call.callee);
        if (!cg_error_already_reported(cg, dedup_key)) {
            diag_begin_error("undefined function or function pointer '%s'", node->as.call.callee);
            diag_set_category(ErrCatUndefined);
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
        }
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
                if (asym && !asym->stype.is_pointer) {
                    /* Only check storage domain for value types, not pointers.
                     * For pointers the symbol's storage is where the pointer
                     * variable lives, not where the pointed-to data lives —
                     * those are different things and can't be compared here. */
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

    /* coerce arguments to the declared parameter types (e.g. i32 literal → f32).
       Also auto-wrap into any.[...] types when needed. */
    if (argc > 0 && n_params > 0) {
        heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
        LLVMTypeRef *param_types = pt_heap.pointer;
        LLVMGetParamTypes(fn_type, param_types);
        usize_t arg_offset = prepend_this ? 1 : 0;
        for (usize_t i = 0; i < argc && i < (usize_t)n_params; i++) {
            LLVMTypeRef pt = param_types[i];
            LLVMTypeRef at = LLVMTypeOf(args[i]);
            if (pt != at) {
                /* Try interface fat-pointer coercion first */
                if (i >= arg_offset && (i - arg_offset) < user_argc) {
                    node_t *arg_node = node->as.call.args.items[i - arg_offset];
                    LLVMValueRef coerced = try_coerce_arg_to_iface(cg, args[i], pt, arg_node);
                    if (coerced != args[i]) { args[i] = coerced; continue; }
                }
                /* Check if the param is an any.[...] struct and arg needs wrapping */
                struct_reg_t *any_sr = find_struct_by_llvm_type(cg, pt);
                if (any_sr && any_sr->is_any_type) {
                    /* Try to instantiate if not yet done */
                    if (strncmp(any_sr->name, "any_G_", 6) == 0)
                        any_sr = try_instantiate_any(cg, any_sr->name);
                    if (any_sr) {
                        /* find which variant matches the arg's LLVM type */
                        for (usize_t vi = 0; vi < any_sr->any_variant_count; vi++) {
                            LLVMTypeRef vt = get_llvm_type(cg, any_sr->any_variants[vi]);
                            if (vt == at) {
                                args[i] = wrap_in_any(cg, any_sr, args[i], vi);
                                break;
                            }
                        }
                    }
                } else {
                    args[i] = coerce_int(cg, args[i], pt);
                }
            }
        }
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
                    type_info_t dummy = {.base=TypeI32, .is_pointer=False, .ptr_perm=PtrNone};
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
        /* lazily instantiate generic if needed (e.g. map_t_G_K_G_V.new()) */
        if (strstr(obj->as.ident.name, "_G_"))
            try_instantiate_generic(cg, obj->as.ident.name);
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

    /* ── interface-qualified call: d.flyable_i.move() ──
       obj is NodeMemberExpr where the field name is an interface name */
    if (obj->kind == NodeMemberExpr) {
        char *iface_qual = obj->as.member_expr.field;
        usize_t iface_ii = find_interface_index(cg, iface_qual);
        if (iface_ii != (usize_t)-1) {
            /* outer object: d */
            node_t *outer = obj->as.member_expr.object;
            LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
            if (outer->kind == NodeIdentExpr) {
                symbol_t *outer_sym = cg_lookup(cg, outer->as.ident.name);
                if (outer_sym && outer_sym->stype.user_name) {
                    /* look up iface-qualified method: "struct.iface.method" */
                    char mangled[512];
                    struct_reg_t *outer_sr = find_struct(cg, outer_sym->stype.user_name);
                    if (outer_sr && outer_sr->mod_prefix && outer_sr->mod_prefix[0]) {
                        snprintf(mangled, sizeof(mangled), "%s__%s__%s__%s",
                                 outer_sr->mod_prefix, outer_sym->stype.user_name,
                                 iface_qual, method);
                    } else {
                        snprintf(mangled, sizeof(mangled), "%s.%s.%s",
                                 outer_sym->stype.user_name, iface_qual, method);
                    }
                    symbol_t *fn_sym = cg_lookup(cg, mangled);
                    if (fn_sym) {
                        usize_t argc = node->as.method_call.args.count + 1;
                        heap_t args_heap = allocate(argc, sizeof(LLVMValueRef));
                        LLVMValueRef *args = args_heap.pointer;
                        args[0] = outer_sym->stype.is_pointer
                            ? LLVMBuildLoad2(cg->builder, ptr_t, outer_sym->value, "obj_ptr")
                            : outer_sym->value;
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
            /* also handle: this.entity.id() where entity is an interface pointer */
            /* fall through to member-expr handling below */
        }
    }

    /* ── method call on a member expression: this.entity.method() ──
       Where entity has an interface type (is_pointer + TypeUser + is_interface) */
    if (obj->kind == NodeMemberExpr) {
        /* evaluate the object to get it */
        node_t *outer = obj->as.member_expr.object;
        char *field   = obj->as.member_expr.field;
        if (outer->kind == NodeIdentExpr || outer->kind == NodeSelfMemberExpr) {
            /* get the type of the field */
            const char *outer_type_name = Null;
            if (outer->kind == NodeIdentExpr) {
                symbol_t *outer_sym = cg_lookup(cg, outer->as.ident.name);
                if (outer_sym) outer_type_name = outer_sym->stype.user_name;
            } else {
                /* NodeSelfMemberExpr: this.field */
                outer_type_name = cg->current_struct_name;
            }
            if (outer_type_name) {
                struct_reg_t *outer_sr = find_struct(cg, outer_type_name);
                if (outer_sr) {
                    /* find the field in the outer struct */
                    for (usize_t fi = 0; fi < outer_sr->field_count; fi++) {
                        if (strcmp(outer_sr->fields[fi].name, field) == 0) {
                            type_info_t fti = outer_sr->fields[fi].type;
                            if (fti.base == TypeUser && fti.user_name) {
                                usize_t fii = find_interface_index(cg, fti.user_name);
                                if (fii != (usize_t)-1) {
                                    /* field is interface-typed — do vtable dispatch */
                                    /* generate the field alloca value */
                                    LLVMValueRef fat_alloca = gen_expr(cg, obj);
                                    /* construct a temporary alloca holding the fat val */
                                    LLVMTypeRef fat_type = cg->interfaces[fii].fat_ptr_type;
                                    LLVMValueRef tmp = alloc_in_entry(cg, fat_type, "iface_tmp");
                                    LLVMBuildStore(cg->builder, fat_alloca, tmp);
                                    symbol_t tmp_sym = {0};
                                    tmp_sym.value = tmp;
                                    tmp_sym.stype = fti;
                                    return gen_iface_method_call(cg, node, &tmp_sym, fii);
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    /* instance method call: obj.method(args) — pass &obj as first arg.
       Use the struct registry's mod_prefix so that methods on imported types
       resolve to the correct mangled symbol (e.g. geom__Vec2__len). */
    if (obj->kind == NodeIdentExpr) {
        symbol_t *obj_sym = cg_lookup(cg, obj->as.ident.name);
        if (obj_sym && obj_sym->stype.base == TypeUser && obj_sym->stype.user_name) {
            /* check if obj is an interface-typed variable — use vtable dispatch */
            struct_reg_t *obj_sr = find_struct(cg, obj_sym->stype.user_name);
            if (obj_sr && obj_sr->is_interface) {
                usize_t iface_ii = find_interface_index(cg, obj_sym->stype.user_name);
                if (iface_ii != (usize_t)-1) {
                    return gen_iface_method_call(cg, node, obj_sym, iface_ii);
                }
            }

            char mangled[512];
            /* lazily instantiate generic if needed (e.g. map_t_G_K_G_V instance method) */
            if (strstr(obj_sym->stype.user_name, "_G_"))
                try_instantiate_generic(cg, obj_sym->stype.user_name);
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
                /* For pointer-type variables (e.g. other: dstring_t *r), the alloca
                 * holds the pointer value — load it first to get the actual struct ptr. */
                if (obj_sym->stype.is_pointer || (obj_sym->flags & SymHeapVar)) {
                    /* pointer var: alloca holds the ptr — load it */
                    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                    args[0] = LLVMBuildLoad2(cg->builder, ptr_ty, obj_sym->value, "obj_ptr");
                } else {
                    args[0] = obj_sym->value; /* alloca = pointer to struct */
                }
                for (usize_t i = 0; i < node->as.method_call.args.count; i++)
                    args[i + 1] = gen_expr(cg, node->as.method_call.args.items[i]);
                LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
                /* coerce args to declared param types (including interface fat-ptr coercion) */
                unsigned n_params_mc = LLVMCountParamTypes(fn_type);
                if (argc > 0 && n_params_mc > 0) {
                    heap_t pt_heap = allocate(n_params_mc, sizeof(LLVMTypeRef));
                    LLVMTypeRef *param_types = pt_heap.pointer;
                    LLVMGetParamTypes(fn_type, param_types);
                    for (usize_t i = 1; i < argc && i < (usize_t)n_params_mc; i++) {
                        node_t *arg_node = node->as.method_call.args.items[i - 1];
                        args[i] = try_coerce_arg_to_iface(cg, args[i], param_types[i], arg_node);
                        args[i] = coerce_int(cg, args[i], param_types[i]);
                    }
                    deallocate(pt_heap);
                }
                LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                                   args, (unsigned)argc, "");
                deallocate(args_heap);
                return ret;
            }
        }
    }

    /* ── method call on NodeSelfMemberExpr: this.field.method() ──
       E.g. this.entity.id() inside entity_holder — field has a pointer type to a struct. */
    if (obj->kind == NodeSelfMemberExpr) {
        const char *struct_name = cg->current_struct_name;
        if (struct_name) {
            struct_reg_t *sr = find_struct(cg, struct_name);
            if (sr) {
                char *field = obj->as.self_member.field;
                for (usize_t fi = 0; fi < sr->field_count; fi++) {
                    if (strcmp(sr->fields[fi].name, field) == 0) {
                        type_info_t fti = sr->fields[fi].type;
                        if (fti.base == TypeUser && fti.user_name) {
                            /* check for interface type */
                            usize_t fii = find_interface_index(cg, fti.user_name);
                            if (fii != (usize_t)-1) {
                                /* interface-typed field — vtable dispatch */
                                LLVMValueRef field_val = gen_expr(cg, obj);
                                LLVMTypeRef fat_type = cg->interfaces[fii].fat_ptr_type;
                                LLVMValueRef tmp = alloc_in_entry(cg, fat_type, "iface_tmp");
                                LLVMBuildStore(cg->builder, field_val, tmp);
                                symbol_t tmp_sym = {0};
                                tmp_sym.value = tmp;
                                tmp_sym.stype = fti;
                                return gen_iface_method_call(cg, node, &tmp_sym, fii);
                            }
                            /* concrete struct field (possibly pointer) */
                            if (strstr(fti.user_name, "_G_"))
                                try_instantiate_generic(cg, fti.user_name);
                            struct_reg_t *fsr = find_struct(cg, fti.user_name);
                            if (fsr) {
                                char mangled[512];
                                if (fsr->mod_prefix && fsr->mod_prefix[0]) {
                                    snprintf(mangled, sizeof(mangled), "%s__%s__%s",
                                             fsr->mod_prefix, fti.user_name, method);
                                } else {
                                    snprintf(mangled, sizeof(mangled), "%s.%s",
                                             fti.user_name, method);
                                }
                                symbol_t *fn_sym = cg_lookup(cg, mangled);
                                if (fn_sym) {
                                    usize_t argc = node->as.method_call.args.count + 1;
                                    heap_t args_heap = allocate(argc, sizeof(LLVMValueRef));
                                    LLVMValueRef *args = args_heap.pointer;
                                    LLVMValueRef field_val2 = gen_expr(cg, obj);
                                    /* if pointer-typed field, it IS the pointer; just use it */
                                    if (fti.is_pointer) {
                                        args[0] = field_val2;
                                    } else {
                                        /* non-pointer field — &field */
                                        LLVMValueRef tmp2 = alloc_in_entry(cg,
                                            get_llvm_type(cg, fti), "field_tmp");
                                        LLVMBuildStore(cg->builder, field_val2, tmp2);
                                        args[0] = tmp2;
                                    }
                                    for (usize_t i = 0; i < node->as.method_call.args.count; i++)
                                        args[i + 1] = gen_expr(cg,
                                            node->as.method_call.args.items[i]);
                                    LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
                                    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type,
                                        fn_sym->value, args, (unsigned)argc, "");
                                    deallocate(args_heap);
                                    return ret;
                                }
                            }
                        }
                        break;
                    }
                }
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

/* Shared lowering for thread.() and async.(): pack args on the heap, call
   __thread_dispatch, return the __future_t*. */
static LLVMValueRef gen_dispatch_to_pool(cg_t *cg, node_t *diag_node,
        const char *callee, node_list_t *args) {
    symbol_t *sym = cg_lookup(cg, callee);
    if (!sym) {
        diag_begin_error("undefined function '%s'", callee);
        diag_span(DIAG_NODE(diag_node), True, "not defined in this module");
        diag_note("thread dispatch requires the function to be visible in this module");
        diag_finish();
        return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }

    node_t *fn_decl = find_fn_decl(cg, callee);
    if (!fn_decl) {
        diag_begin_error("cannot find declaration of '%s' for thread dispatch", callee);
        diag_span(DIAG_NODE(diag_node), True, "dispatched here");
        diag_note("function must be declared in the same compilation unit");
        diag_finish();
        return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }

    thr_wrapper_t *w = get_or_create_thread_wrapper(cg, callee, sym, fn_decl);

    LLVMValueRef args_ptr;
    usize_t argc = args ? args->count : 0;

    if (argc > 0 && w->args_struct_type) {
        LLVMValueRef sz = LLVMSizeOf(w->args_struct_type);
        sz = coerce_int(cg, sz, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef malloc_args[1] = { sz };
        args_ptr = LLVMBuildCall2(cg->builder, cg->malloc_type,
                                   cg->malloc_fn, malloc_args, 1, "thr_args");
        for (usize_t i = 0; i < argc; i++) {
            LLVMValueRef val = gen_expr(cg, args->items[i]);
            LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, w->args_struct_type,
                                                    args_ptr, (unsigned)i, "af");
            LLVMBuildStore(cg->builder, val, gep);
        }
    } else {
        args_ptr = LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }

    LLVMValueRef result_sz = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx),
                                           (unsigned long long)w->result_size, 0);
    LLVMValueRef dispatch_args[3] = { w->wrapper_fn, args_ptr, result_sz };
    return LLVMBuildCall2(cg->builder,
                          cg->thread_dispatch_type,
                          cg->thread_dispatch_fn,
                          dispatch_args, 3, "future");
}

static LLVMValueRef gen_thread_call(cg_t *cg, node_t *node) {
    return gen_dispatch_to_pool(cg, node,
        node->as.thread_call.callee, &node->as.thread_call.args);
}

static LLVMValueRef gen_async_call(cg_t *cg, node_t *node) {
    const char *callee = node->as.async_call.callee;
    node_t *fn_decl = find_fn_decl(cg, callee);
    if (fn_decl && !fn_decl->as.fn_decl.is_async) {
        diag_begin_warning("'async.()' used on non-async function '%s'", callee);
        diag_span(DIAG_NODE(node), True, "dispatched here");
        diag_help("declare '%s' as 'async fn' or use 'thread.(%s)(…)' for opaque dispatch",
                  callee, callee);
        diag_finish();
    }
    return gen_dispatch_to_pool(cg, node, callee, &node->as.async_call.args);
}

/* Resolve the element type `T` of a `future.[T]` handle expression. Returns
   NO_TYPE when the type cannot be determined statically (void future or
   unresolved handle); caller falls back to the wait-and-drop lowering. */
static type_info_t resolve_future_elem_type(cg_t *cg, node_t *handle) {
    if (!handle) return NO_TYPE;
    if (handle->kind == NodeAsyncCall || handle->kind == NodeThreadCall) {
        const char *callee = (handle->kind == NodeAsyncCall)
            ? handle->as.async_call.callee
            : handle->as.thread_call.callee;
        node_t *fn = find_fn_decl(cg, callee);
        if (fn && fn->as.fn_decl.return_count > 0) {
            type_info_t rt = fn->as.fn_decl.return_types[0];
            if (rt.base == TypeVoid && !rt.is_pointer) return NO_TYPE;
            return resolve_alias(cg, rt);
        }
        return NO_TYPE;
    }
    if (handle->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, handle->as.ident.name);
        if (sym && sym->stype.base == TypeFuture && sym->stype.elem_type)
            return resolve_alias(cg, sym->stype.elem_type[0]);
        return NO_TYPE;
    }
    return NO_TYPE;
}

/* await(f) / await.(fn)(args) — block on handle, load typed result, drop. */
static LLVMValueRef gen_await(cg_t *cg, node_t *node) {
    node_t *handle = node->as.await_expr.handle;
    type_info_t et = node->as.await_expr.get_type;
    if (et.base == TypeVoid && !et.is_pointer)
        et = resolve_future_elem_type(cg, handle);

    LLVMValueRef h = gen_expr(cg, handle);
    LLVMValueRef call_args[1] = { h };

    if (et.base == TypeVoid && !et.is_pointer) {
        /* void future: wait + drop, no typed result */
        LLVMBuildCall2(cg->builder, cg->future_wait_type,
                       cg->future_wait_fn, call_args, 1, "");
        LLVMBuildCall2(cg->builder, cg->future_drop_type,
                       cg->future_drop_fn, call_args, 1, "");
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    /* typed: __future_get → raw ptr, load T, __future_drop (frees buffer) */
    LLVMValueRef raw = LLVMBuildCall2(cg->builder, cg->future_get_type,
                                       cg->future_get_fn, call_args, 1, "await_raw");
    LLVMTypeRef lt = get_llvm_type(cg, et);
    LLVMValueRef val = LLVMBuildLoad2(cg->builder, lt, raw, "await_val");
    LLVMBuildCall2(cg->builder, cg->future_drop_type,
                   cg->future_drop_fn, call_args, 1, "");
    return val;
}

/* await.all(f1, ..., fN) / await.any(f1, ..., fN).
   v1: require all handles to resolve to the same element type T.
   all: sequential get+load+drop into a homogeneous struct { T, T, ..., T }.
   any: poll __future_ready, get+load+drop the winner, drop the rest, return T. */
static LLVMValueRef gen_await_combinator(cg_t *cg, node_t *node) {
    node_list_t *hs = &node->as.await_combinator.handles;
    boolean_t    is_any = node->as.await_combinator.is_any;
    usize_t      n = hs->count;

    if (n == 0) {
        diag_begin_error("await.%s() requires at least one future",
                         is_any ? "any" : "all");
        diag_span(DIAG_NODE(node), True, "empty argument list");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    /* Resolve element types up front and verify homogeneity. */
    type_info_t et0 = resolve_future_elem_type(cg, hs->items[0]);
    boolean_t et0_void = (et0.base == TypeVoid && !et0.is_pointer);
    for (usize_t i = 1; i < n; i++) {
        type_info_t eti = resolve_future_elem_type(cg, hs->items[i]);
        boolean_t eti_void = (eti.base == TypeVoid && !eti.is_pointer);
        if (eti_void != et0_void
                || eti.base != et0.base
                || eti.is_pointer != et0.is_pointer) {
            diag_begin_error("await.%s() futures must share one element type",
                             is_any ? "any" : "all");
            diag_span(DIAG_NODE(node), True, "handles differ in T");
            diag_note("v1 requires all futures passed to await.all/await.any to be future.[T] for the same T");
            diag_finish();
            break;
        }
    }

    LLVMTypeRef  ptr_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef  i32_t = LLVMInt32TypeInContext(cg->ctx);

    /* Evaluate each handle once into a local slot so we can get/drop without
       re-evaluating the source expression (which may have side effects). */
    heap_t slots_h = allocate(n, sizeof(LLVMValueRef));
    LLVMValueRef *slots = slots_h.pointer;
    for (usize_t i = 0; i < n; i++) {
        slots[i] = alloc_in_entry(cg, ptr_t, "awaitc_h");
        LLVMValueRef hv = gen_expr(cg, hs->items[i]);
        LLVMBuildStore(cg->builder, hv, slots[i]);
    }

    LLVMTypeRef elem_lt = et0_void ? i32_t : get_llvm_type(cg, et0);

    if (!is_any) {
        /* await.all — sequential get+load+drop into aggregate { T, T, ..., T } */
        if (et0_void) {
            for (usize_t i = 0; i < n; i++) {
                LLVMValueRef hv = LLVMBuildLoad2(cg->builder, ptr_t, slots[i], "ah");
                LLVMValueRef ca[1] = { hv };
                LLVMBuildCall2(cg->builder, cg->future_wait_type,
                               cg->future_wait_fn, ca, 1, "");
                LLVMBuildCall2(cg->builder, cg->future_drop_type,
                               cg->future_drop_fn, ca, 1, "");
            }
            deallocate(slots_h);
            return LLVMConstInt(i32_t, 0, 0);
        }

        heap_t tys_h = allocate(n, sizeof(LLVMTypeRef));
        LLVMTypeRef *tys = tys_h.pointer;
        for (usize_t i = 0; i < n; i++) tys[i] = elem_lt;
        LLVMTypeRef agg_t = LLVMStructTypeInContext(cg->ctx, tys, (unsigned)n, 0);

        LLVMValueRef agg = LLVMGetUndef(agg_t);
        for (usize_t i = 0; i < n; i++) {
            LLVMValueRef hv = LLVMBuildLoad2(cg->builder, ptr_t, slots[i], "ah");
            LLVMValueRef ca[1] = { hv };
            LLVMValueRef raw = LLVMBuildCall2(cg->builder, cg->future_get_type,
                                               cg->future_get_fn, ca, 1, "awall_raw");
            LLVMValueRef v = LLVMBuildLoad2(cg->builder, elem_lt, raw, "awall_v");
            agg = LLVMBuildInsertValue(cg->builder, agg, v, (unsigned)i, "awall");
            LLVMBuildCall2(cg->builder, cg->future_drop_type,
                           cg->future_drop_fn, ca, 1, "");
        }
        deallocate(tys_h);
        deallocate(slots_h);
        return agg;
    }

    /* await.any — poll ready on each handle, round-robin with brief sleep.
       Once a winner is chosen: get+load+drop winner; drop the rest. */
    LLVMValueRef winner_idx = alloc_in_entry(cg, i32_t, "awany_i");
    LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, (unsigned long long)-1, 1), winner_idx);

    LLVMBasicBlockRef poll_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.poll");
    LLVMBasicBlockRef found_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.found");
    LLVMBuildBr(cg->builder, poll_bb);

    LLVMPositionBuilderAtEnd(cg->builder, poll_bb);
    /* Chain of ready checks: for i in 0..N if ready(slots[i]) → store i, goto found. */
    LLVMBasicBlockRef next_check = Null;
    for (usize_t i = 0; i < n; i++) {
        LLVMBasicBlockRef hit = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.hit");
        next_check = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.nxt");
        LLVMValueRef hv = LLVMBuildLoad2(cg->builder, ptr_t, slots[i], "ah");
        LLVMValueRef ca[1] = { hv };
        LLVMValueRef rdy = LLVMBuildCall2(cg->builder, cg->future_ready_type,
                                           cg->future_ready_fn, ca, 1, "rdy");
        LLVMValueRef zero = LLVMConstInt(i32_t, 0, 0);
        LLVMValueRef is_rdy = LLVMBuildICmp(cg->builder, LLVMIntNE, rdy, zero, "is_rdy");
        LLVMBuildCondBr(cg->builder, is_rdy, hit, next_check);

        LLVMPositionBuilderAtEnd(cg->builder, hit);
        LLVMBuildStore(cg->builder, LLVMConstInt(i32_t, i, 0), winner_idx);
        LLVMBuildBr(cg->builder, found_bb);

        LLVMPositionBuilderAtEnd(cg->builder, next_check);
    }
    /* None ready: briefly back off by yielding on future.wait of slot 0's handle.
       Simpler than timed sleeps — blocks on one until it completes, then retry. */
    {
        LLVMValueRef hv = LLVMBuildLoad2(cg->builder, ptr_t, slots[0], "ah");
        LLVMValueRef ca[1] = { hv };
        LLVMBuildCall2(cg->builder, cg->future_wait_type,
                       cg->future_wait_fn, ca, 1, "");
        LLVMBuildBr(cg->builder, poll_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, found_bb);
    LLVMValueRef win_i = LLVMBuildLoad2(cg->builder, i32_t, winner_idx, "wi");

    /* Drop losers: for i in 0..N if i != win_i → drop(slots[i]). Winner:
       get+load+drop, stash value in a slot reused as return. */
    LLVMValueRef result_slot = et0_void ? Null : alloc_in_entry(cg, elem_lt, "awany_v");

    LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.done");
    for (usize_t i = 0; i < n; i++) {
        LLVMBasicBlockRef is_win = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.win");
        LLVMBasicBlockRef is_lose = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.lose");
        LLVMBasicBlockRef after_i = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "awany.aft");

        LLVMValueRef ii = LLVMConstInt(i32_t, i, 0);
        LLVMValueRef eq = LLVMBuildICmp(cg->builder, LLVMIntEQ, win_i, ii, "is_win");
        LLVMBuildCondBr(cg->builder, eq, is_win, is_lose);

        LLVMPositionBuilderAtEnd(cg->builder, is_win);
        {
            LLVMValueRef hv = LLVMBuildLoad2(cg->builder, ptr_t, slots[i], "ah");
            LLVMValueRef ca[1] = { hv };
            if (et0_void) {
                LLVMBuildCall2(cg->builder, cg->future_drop_type,
                               cg->future_drop_fn, ca, 1, "");
            } else {
                LLVMValueRef raw = LLVMBuildCall2(cg->builder, cg->future_get_type,
                                                   cg->future_get_fn, ca, 1, "awany_raw");
                LLVMValueRef v = LLVMBuildLoad2(cg->builder, elem_lt, raw, "awany_vload");
                LLVMBuildStore(cg->builder, v, result_slot);
                LLVMBuildCall2(cg->builder, cg->future_drop_type,
                               cg->future_drop_fn, ca, 1, "");
            }
            LLVMBuildBr(cg->builder, after_i);
        }

        LLVMPositionBuilderAtEnd(cg->builder, is_lose);
        {
            LLVMValueRef hv = LLVMBuildLoad2(cg->builder, ptr_t, slots[i], "ah");
            LLVMValueRef ca[1] = { hv };
            LLVMBuildCall2(cg->builder, cg->future_drop_type,
                           cg->future_drop_fn, ca, 1, "");
            LLVMBuildBr(cg->builder, after_i);
        }

        LLVMPositionBuilderAtEnd(cg->builder, after_i);
    }
    LLVMBuildBr(cg->builder, done_bb);

    LLVMPositionBuilderAtEnd(cg->builder, done_bb);
    LLVMValueRef result = et0_void
        ? LLVMConstInt(i32_t, 0, 0)
        : LLVMBuildLoad2(cg->builder, elem_lt, result_slot, "awany_ret");
    deallocate(slots_h);
    return result;
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
        /* arr[i] / slice[i] compound assign */
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
            if (sym->stype.base == TypeSlice) {
                /* slice compound assign: load struct, GEP, use as store_ptr */
                LLVMTypeRef sl_ty  = slice_struct_type(cg);
                LLVMValueRef sl    = LLVMBuildLoad2(cg->builder, sl_ty, sym->value, "sl");
                LLVMValueRef dptr  = LLVMBuildExtractValue(cg->builder, sl, 0, "sl.ptr");
                LLVMTypeRef  elem_ty = elem_type_from_sym(cg, sym);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                store_ptr  = LLVMBuildGEP2(cg->builder, elem_ty, dptr, &index_val, 1, "sidx");
                store_type = elem_ty;
            } else {
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
                LLVMValueRef indices[2] = { zero, index_val };
                LLVMTypeRef elem_type = LLVMGetElementType(sym->type);
                store_ptr = LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "idx");
                store_type = elem_type;
            }
        }
    } else if (target->kind == NodeSelfMemberExpr) {
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (this_sym) {
            const char *sname = target->as.self_member.type_name;
            if (!sname) sname = cg->current_struct_name;
            struct_reg_t *sr = sname ? find_struct(cg, sname) : Null;
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
    } else if (target->kind == NodeUnaryPrefixExpr && target->as.unary.op == TokStar) {
        /* *.(expr) compound assign */
        store_ptr = gen_expr(cg, target->as.unary.operand);
        node_t *inner = target->as.unary.operand;
        int dd = 1;
        while (inner->kind == NodeUnaryPrefixExpr && inner->as.unary.op == TokStar) {
            dd++;
            inner = inner->as.unary.operand;
        }
        if (inner->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, inner->as.ident.name);
            if (sym && sym->stype.is_pointer) {
                type_info_t pt = sym->stype;
                for (int i = 0; i < dd && pt.is_pointer; i++)
                    pt = ti_deref_one(pt);
                store_type = get_llvm_type(cg, pt);
            }
        }
        if (!store_type) store_type = LLVMInt8TypeInContext(cg->ctx);
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

/* Walk a chain of NodeIndexExpr to find the base array symbol and collect
 * all index values (outermost first).  Returns True iff the base symbol's
 * LLVM type is an array AND every intermediate element type is also an
 * array (pure N-D array access, e.g. i32 m[3][4]).
 * idx_vals[] receives one entry per dimension; *ndim is set to the count.
 * Indices are evaluated in outermost-to-innermost order. */
static boolean_t mdim_collect(cg_t *cg, node_t *node,
                               symbol_t **out_sym,
                               LLVMValueRef *idx_vals, int *ndim) {
    node_t *idx_nodes[8];
    int n = 0;
    node_t *cur = node;
    while (cur->kind == NodeIndexExpr && n < 8) {
        idx_nodes[n++] = cur->as.index_expr.index;
        cur = cur->as.index_expr.object;
    }
    if (cur->kind != NodeIdentExpr || n == 0) return False;
    symbol_t *sym = cg_lookup(cg, cur->as.ident.name);
    if (!sym || LLVMGetTypeKind(sym->type) != LLVMArrayTypeKind) return False;
    /* Every dimension must be backed by an LLVM array type. */
    LLVMTypeRef t = sym->type;
    for (int _i = 0; _i < n; _i++) {
        if (LLVMGetTypeKind(t) != LLVMArrayTypeKind) return False;
        t = LLVMGetElementType(t);
    }
    /* Evaluate indices outermost → innermost (idx_nodes is stored innermost first). */
    for (int _i = n - 1; _i >= 0; _i--)
        idx_vals[n - 1 - _i] = gen_expr(cg, idx_nodes[_i]);
    *ndim    = n;
    *out_sym = sym;
    return True;
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

        /* cross-domain check: reject domain mismatch on pointer assignment */
        if (sym->stype.is_pointer
                && (sym->storage == StorageStack || sym->storage == StorageHeap)) {
            int ak = rhs_addr_kind(cg, node->as.assign.value);
            if (ak != 0)
                check_storage_domain(cg, sym->storage, ak == 1, ak == -1, node->line);
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
        node_t *obj      = target->as.index_expr.object;
        node_t *idx_node = target->as.index_expr.index;

        /* ── nested index: m[i][j] = val (multi-dimensional array store) ─── */
        if (obj->kind == NodeIndexExpr) {
            symbol_t *base_sym = Null;
            LLVMValueRef idx_vals[8];
            int ndim = 0;
            if (mdim_collect(cg, target, &base_sym, idx_vals, &ndim)) {
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                LLVMValueRef gep_indices[9];
                gep_indices[0] = zero;
                for (int _d = 0; _d < ndim; _d++)
                    gep_indices[_d + 1] = coerce_int(cg, idx_vals[_d],
                                                      LLVMInt32TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, base_sym->type,
                                                  base_sym->value, gep_indices, ndim + 1, "mdidx");
                LLVMTypeRef inner_ty = base_sym->type;
                for (int _d = 0; _d < ndim; _d++) inner_ty = LLVMGetElementType(inner_ty);
                rhs = coerce_int(cg, rhs, inner_ty);
                LLVMBuildStore(cg->builder, rhs, gep);
            }
            return rhs;
        }

        LLVMValueRef index_val = gen_expr(cg, idx_node);

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
                type_info_t elem_ti = ti_deref_one(sym->stype);
                LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidx");
                rhs = coerce_int(cg, rhs, elem_ty);
                LLVMBuildStore(cg->builder, rhs, gep);
            } else if (sym->stype.base == TypeSlice) {
                /* slice[i] = rhs: load struct, GEP into data ptr, store */
                LLVMTypeRef sl_ty  = slice_struct_type(cg);
                LLVMValueRef sl    = LLVMBuildLoad2(cg->builder, sl_ty, sym->value, "sl");
                LLVMValueRef dptr  = LLVMBuildExtractValue(cg->builder, sl, 0, "sl.ptr");
                LLVMTypeRef elem_ty = elem_type_from_sym(cg, sym);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep   = LLVMBuildGEP2(cg->builder, elem_ty, dptr, &index_val, 1, "sidx");
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
                    type_info_t elem_ti = ti_deref_one(base_sr->fields[fi].type);
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
                /* apply generic substitution */
                if (smtn && cg->generic_tmpl_name && cg->generic_inst_name
                        && strcmp(smtn, cg->generic_tmpl_name) == 0)
                    smtn = cg->generic_inst_name;
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
                        type_info_t elem_ti = ti_deref_one(sr->fields[i].type);
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
        if (type_name && cg->generic_tmpl_name && cg->generic_inst_name
                && strcmp(type_name, cg->generic_tmpl_name) == 0)
            type_name = cg->generic_inst_name;
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

    if (target->kind == NodeUnaryPrefixExpr && target->as.unary.op == TokStar) {
        /* *.(expr) = rhs — store through pointer */
        LLVMValueRef ptr = gen_expr(cg, target->as.unary.operand);
        /* determine pointee type by walking the deref chain to its base ident */
        LLVMTypeRef pointee_type = Null;
        node_t *inner = target->as.unary.operand;
        int deref_depth = 1;
        while (inner->kind == NodeUnaryPrefixExpr && inner->as.unary.op == TokStar) {
            deref_depth++;
            inner = inner->as.unary.operand;
        }
        if (inner->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, inner->as.ident.name);
            if (sym && sym->stype.is_pointer) {
                type_info_t pt = sym->stype;
                for (int i = 0; i < deref_depth && pt.is_pointer; i++)
                    pt = ti_deref_one(pt);
                pointee_type = get_llvm_type(cg, pt);
            }
        }
        if (!pointee_type) pointee_type = LLVMInt8TypeInContext(cg->ctx);
        rhs = coerce_int(cg, rhs, pointee_type);
        LLVMBuildStore(cg->builder, rhs, ptr);
        return rhs;
    }

    diag_begin_error("invalid assignment target");
    diag_span(DIAG_NODE(target), True, "");
    diag_note("only variables and fields can appear on the left side of =");
    diag_finish();
    return rhs;
}

static LLVMValueRef gen_index(cg_t *cg, node_t *node) {
    node_t *obj = node->as.index_expr.object;

    /* ── nested index expressions: m[i][j], m[i][j][k], table[i][j] ──────── */
    if (obj->kind == NodeIndexExpr) {
        /* Case 1: pure N-D array (every level is an LLVM array type). */
        symbol_t *base_sym = Null;
        LLVMValueRef idx_vals[8];
        int ndim = 0;
        if (mdim_collect(cg, node, &base_sym, idx_vals, &ndim)) {
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            LLVMValueRef gep_indices[9];
            gep_indices[0] = zero;
            for (int _d = 0; _d < ndim; _d++)
                gep_indices[_d + 1] = coerce_int(cg, idx_vals[_d],
                                                  LLVMInt32TypeInContext(cg->ctx));
            LLVMValueRef gep = LLVMBuildGEP2(cg->builder, base_sym->type,
                                              base_sym->value, gep_indices, ndim + 1, "mdidx");
            LLVMTypeRef inner_ty = base_sym->type;
            for (int _d = 0; _d < ndim; _d++) inner_ty = LLVMGetElementType(inner_ty);
            return LLVMBuildLoad2(cg->builder, inner_ty, gep, "mdelem");
        }
        /* Case 2: array-of-slices — 1-D array whose element type is a slice. */
        node_t *inner_obj = obj->as.index_expr.object;
        if (inner_obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, inner_obj->as.ident.name);
            if (sym && LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind
                    && sym->stype.base == TypeSlice) {
                LLVMValueRef arr_idx = gen_expr(cg, obj->as.index_expr.index);
                LLVMValueRef sl_idx  = gen_expr(cg, node->as.index_expr.index);
                LLVMValueRef zero    = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                arr_idx = coerce_int(cg, arr_idx, LLVMInt32TypeInContext(cg->ctx));
                LLVMValueRef arr_indices[2] = { zero, arr_idx };
                LLVMTypeRef  sl_struct_ty   = slice_struct_type(cg);
                LLVMValueRef sl_ptr = LLVMBuildGEP2(cg->builder, sym->type,
                                                     sym->value, arr_indices, 2, "asl.ptr");
                LLVMValueRef sl   = LLVMBuildLoad2(cg->builder, sl_struct_ty, sl_ptr, "asl");
                LLVMValueRef dptr = LLVMBuildExtractValue(cg->builder, sl, 0, "asl.dptr");
                LLVMTypeRef  elem_ty = elem_type_from_sym(cg, sym);
                sl_idx = coerce_int(cg, sl_idx, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, dptr, &sl_idx, 1, "asl.idx");
                return LLVMBuildLoad2(cg->builder, elem_ty, gep, "asl.elem");
            }
        }
    }

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
            type_info_t elem_ti = ti_deref_one(sym->stype);
            LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
            index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
            LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidx");
            return LLVMBuildLoad2(cg->builder, elem_ty, gep, "pelem");
        }
        if (sym->stype.base == TypeSlice) {
            /* slice[i]: load struct, extract ptr, GEP and load.
             * Emit a runtime bounds check unless we are inside unsafe{} or
             * the index is a compile-time constant within [0, known_len). */
            LLVMTypeRef sl_ty    = slice_struct_type(cg);
            LLVMValueRef sl      = LLVMBuildLoad2(cg->builder, sl_ty, sym->value, "sl");
            LLVMValueRef dptr    = LLVMBuildExtractValue(cg->builder, sl, 0, "sl.ptr");
            LLVMValueRef len_val = LLVMBuildExtractValue(cg->builder, sl, 1, "sl.len");
            LLVMTypeRef  elem_ty = elem_type_from_sym(cg, sym);
            LLVMTypeRef  i64_t   = LLVMInt64TypeInContext(cg->ctx);

            /* bounds check: if in_unsafe == 0, emit check */
            boolean_t skip_check = (cg->in_unsafe > 0);
            /* static elimination: constant index within known-constant length */
            if (!skip_check && node->as.index_expr.index->kind == NodeIntLitExpr) {
                long ci = node->as.index_expr.index->as.int_lit.value;
                /* If we have a compile-time known len from sym->array_size, use it.
                 * For slices, sym->array_size stores the declared length if from make.() */
                if (sym->array_size >= 0 && ci >= 0 && ci < sym->array_size)
                    skip_check = True;
            }
            if (!skip_check) {
                /* bounds check: 0 <= i < len
                 * len_val is i64 (from slice struct); coerce index to i64 too */
                LLVMValueRef idx_i64 = coerce_int(cg, index_val, i64_t);
                LLVMValueRef zero64  = LLVMConstInt(i64_t, 0, 0);
                LLVMValueRef len64   = coerce_int(cg, len_val, i64_t);
                LLVMValueRef in_range = LLVMBuildAnd(cg->builder,
                    LLVMBuildICmp(cg->builder, LLVMIntSGE, idx_i64, zero64, "bc.lo"),
                    LLVMBuildICmp(cg->builder, LLVMIntSLT, idx_i64, len64,  "bc.hi"),
                    "bc.ok");
                LLVMBasicBlockRef ok_bb   = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "bc.ok");
                LLVMBasicBlockRef oob_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "bc.oob");
                LLVMBuildCondBr(cg->builder, in_range, ok_bb, oob_bb);

                /* out-of-bounds path: print diagnostic and trap/abort */
                LLVMPositionBuilderAtEnd(cg->builder, oob_bb);
                {
                    const char *msg = "slice index out of bounds\n";
                    LLVMValueRef msg_str = LLVMBuildGlobalStringPtr(cg->builder, msg, "oob_msg");
                    LLVMValueRef pfmt[1] = { msg_str };
                    LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, pfmt, 1, "");
                    /* emit llvm.trap intrinsic (opaque-pointer safe: keep trap_ty) */
                    LLVMTypeRef  trap_ty = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), Null, 0, 0);
                    LLVMValueRef trap_fn = LLVMGetNamedFunction(cg->module, "llvm.trap");
                    if (!trap_fn)
                        trap_fn = LLVMAddFunction(cg->module, "llvm.trap", trap_ty);
                    LLVMBuildCall2(cg->builder, trap_ty, trap_fn, Null, 0, "");
                    LLVMBuildUnreachable(cg->builder);
                }

                LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
            }

            index_val = coerce_int(cg, index_val, i64_t);
            LLVMValueRef gep   = LLVMBuildGEP2(cg->builder, elem_ty, dptr, &index_val, 1, "sidx");
            return LLVMBuildLoad2(cg->builder, elem_ty, gep, "selem");
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
                type_info_t elem_ti = ti_deref_one(base_sr->fields[fi].type);
                LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "midx");
                return LLVMBuildLoad2(cg->builder, elem_type, gep, "melem");
            }
        }
    }

    /* self-member pointer indexing: this.field[idx] inside a method */
    if (obj->kind == NodeSelfMemberExpr) {
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (this_sym) {
            char *type_name = obj->as.self_member.type_name;
            if (!type_name) type_name = cg->current_struct_name;
            /* apply generic substitution: template name → instantiated name */
            if (type_name && cg->generic_tmpl_name && cg->generic_inst_name
                    && strcmp(type_name, cg->generic_tmpl_name) == 0)
                type_name = cg->generic_inst_name;
            struct_reg_t *sr = type_name ? find_struct(cg, type_name) : Null;
            if (sr) {
                LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                LLVMValueRef this_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
                const char *fname = obj->as.self_member.field;
                for (usize_t fi = 0; fi < sr->field_count; fi++) {
                    if (strcmp(sr->fields[fi].name, fname) != 0) continue;
                    LLVMValueRef field_gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type,
                        this_ptr, (unsigned)sr->fields[fi].index, fname);
                    LLVMTypeRef ptr_type = get_llvm_type(cg, sr->fields[fi].type);
                    LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, "smfptr");
                    type_info_t elem_ti = ti_deref_one(sr->fields[fi].type);
                    LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                    index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                    LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "smidx");
                    return LLVMBuildLoad2(cg->builder, elem_type, gep, "smelem");
                }
            }
        }
    }

    /* general pointer indexing — try to recover element type from a cast expression
     * (e.g. ((K *r)(slot + 1))[0] in generic bodies), otherwise default to i8. */
    LLVMValueRef obj_val = gen_expr(cg, obj);
    if (llvm_is_ptr(LLVMTypeOf(obj_val))) {
        LLVMTypeRef elem_ty = Null;
        if (obj->kind == NodeCastExpr) {
            type_info_t cti = subst_type_info(cg, obj->as.cast_expr.target);
            if (cti.is_pointer) {
                cti = ti_deref_one(cti);
                elem_ty = get_llvm_type(cg, cti);
                /* only use the type if it resolves to something useful (not opaque ptr) */
                if (LLVMGetTypeKind(elem_ty) == LLVMPointerTypeKind)
                    elem_ty = Null; /* unknown sub-type, fall back to i8 */
            }
        }
        if (!elem_ty) elem_ty = LLVMInt8TypeInContext(cg->ctx);
        index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, obj_val, &index_val, 1, "stridx");
        return LLVMBuildLoad2(cg->builder, elem_ty, gep, "ch");
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
        if (sym && strcmp(field, "len") == 0 && sym->array_size >= 0) {
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                                (unsigned long long)sym->array_size, 0);
        }
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

    {
        char dedup_key[600];
        /* Include object name in dedup key when available */
        const char *obj_name = (obj->kind == NodeIdentExpr) ? obj->as.ident.name : "";
        snprintf(dedup_key, sizeof(dedup_key), "undef_member:%s.%s", obj_name, field);
        if (!cg_error_already_reported(cg, dedup_key)) {
            diag_begin_error("cannot resolve member '%s'", field);
            diag_set_category(ErrCatUndefined);
            diag_span(DIAG_NODE(node), True, "member not found");
            /* Levenshtein suggestion: scan struct fields for close name */
            if (obj->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
                if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                    struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
                    if (sr) {
                        usize_t best_dist = 3;
                        const char *best = Null;
                        for (usize_t i = 0; i < sr->field_count; i++) {
                            usize_t d = levenshtein(field, sr->fields[i].name);
                            if (d < best_dist) { best_dist = d; best = sr->fields[i].name; }
                        }
                        if (best) diag_help("did you mean '%s'?", best);
                    }
                }
            }
            diag_finish();
        }
    }
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_self_method_call(cg_t *cg, node_t *node) {
    /* this.method(args) — call a method on the current struct instance */
    char *type_name = node->as.self_method_call.type_name;
    char *method    = node->as.self_method_call.method;
    /* Enforce: 'this' is only valid inside struct body methods */
    if (!type_name && !cg->current_fn_is_inline_method) {
        diag_begin_error("'this' used outside of a struct body method");
        diag_span(DIAG_NODE(node), True, "only valid inside 'fn method()' defined within a struct body");
        diag_note("move this function inside the struct body to make it an instance method");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    /* NULL type_name means 'this' keyword was used — resolve from current struct context */
    if (!type_name) type_name = cg->current_struct_name;
    /* In a generic instantiation the AST still holds the template base name;
     * remap to the mangled instantiated name so struct/method lookups succeed. */
    if (type_name && cg->generic_tmpl_name && cg->current_struct_name &&
            strcmp(type_name, cg->generic_tmpl_name) == 0)
        type_name = cg->current_struct_name;

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
    /* this.field — resolve to this->field */
    char *field = node->as.self_member.field;
    /* Enforce: 'this' is only valid inside struct body methods */
    if (!node->as.self_member.type_name && !cg->current_fn_is_inline_method) {
        diag_begin_error("'this' used outside of a struct body method");
        diag_span(DIAG_NODE(node), True, "only valid inside 'fn method()' defined within a struct body");
        diag_note("move this function inside the struct body to make it an instance method");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
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
    /* apply generic substitution: during instantiation the template name is mapped to the inst name */
    if (cg->generic_tmpl_name && cg->generic_inst_name && type_name
            && strcmp(type_name, cg->generic_tmpl_name) == 0)
        type_name = cg->generic_inst_name;
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
    LLVMValueRef result = LLVMBuildCall2(cg->builder, cg->malloc_type, cg->malloc_fn, args, 1, "alloc");
    /* record provenance tag for this allocation */
    if (cg->provenance_count < 256) {
        cg->provenance[cg->provenance_count].name     = Null;  /* filled when stored */
        cg->provenance[cg->provenance_count].tag      = ++cg->next_tag;
        cg->provenance[cg->provenance_count].closed   = False;
        cg->provenance[cg->provenance_count].close_line = 0;
        cg->provenance_count++;
    }
    return result;
}

/* Return the void** address for a zone expression:
 *   NodeIdentExpr  → alloca/global address of the zone variable (SymZone)
 *   NodeSelfMemberExpr (this.xyz)  → GEP of the zone field
 *   NodeMemberExpr (s.xyz)         → GEP of the zone field
 * Returns NULL if the expression is not a recognizable zone. */
static LLVMValueRef get_zone_field_addr(cg_t *cg, node_t *zone_expr) {
    if (!zone_expr) return Null;

    if (zone_expr->kind == NodeIdentExpr) {
        symbol_t *zsym = cg_lookup(cg, zone_expr->as.ident.name);
        if (zsym && (zsym->flags & SymZone))
            return zsym->value; /* alloca or global = void** */
        return Null;
    }

    if (zone_expr->kind == NodeSelfMemberExpr) {
        const char *field = zone_expr->as.self_member.field;
        char *type_name = zone_expr->as.self_member.type_name;
        if (!type_name) type_name = cg->current_struct_name;
        if (cg->generic_tmpl_name && cg->generic_inst_name && type_name
                && strcmp(type_name, cg->generic_tmpl_name) == 0)
            type_name = cg->generic_inst_name;
        struct_reg_t *sr = type_name ? find_struct(cg, type_name) : Null;
        if (!sr) return Null;
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (!this_sym) return Null;
        LLVMValueRef this_ptr = this_sym->value;
        if (LLVMGetTypeKind(this_sym->type) == LLVMPointerTypeKind)
            this_ptr = LLVMBuildLoad2(cg->builder, this_sym->type, this_sym->value, "this");
        for (usize_t i = 0; i < sr->field_count; i++) {
            if (strcmp(sr->fields[i].name, field) == 0
                    && sr->fields[i].type.base == TypeZone) {
                return LLVMBuildStructGEP2(cg->builder, sr->llvm_type, this_ptr,
                                           (unsigned)sr->fields[i].index, field);
            }
        }
        return Null;
    }

    if (zone_expr->kind == NodeMemberExpr) {
        node_t *obj = zone_expr->as.member_expr.object;
        const char *field = zone_expr->as.member_expr.field;
        if (obj->kind != NodeIdentExpr) return Null;
        symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
        if (!sym || sym->stype.base != TypeUser || !sym->stype.user_name) return Null;
        struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
        if (!sr) return Null;
        for (usize_t i = 0; i < sr->field_count; i++) {
            if (strcmp(sr->fields[i].name, field) == 0
                    && sr->fields[i].type.base == TypeZone) {
                return LLVMBuildStructGEP2(cg->builder, sr->llvm_type, sym->value,
                                           (unsigned)sr->fields[i].index, field);
            }
        }
        return Null;
    }

    return Null;
}

/* new.(T) in zone_expr — allocate from zone arena */
static LLVMValueRef gen_new_in_zone(cg_t *cg, node_t *node) {
    node_t *zone_expr = node->as.new_in_zone.zone_expr;
    LLVMValueRef zone_ptr_addr = get_zone_field_addr(cg, zone_expr);
    if (!zone_ptr_addr) {
        const char *zname = (zone_expr && zone_expr->kind == NodeIdentExpr)
                          ? zone_expr->as.ident.name : "?";
        diag_begin_error("undefined zone '%s'", zname);
        diag_span(DIAG_NODE(node), True, "zone not declared in this scope");
        diag_help("declare the zone first: zone %s; or zone %s { }", zname, zname);
        diag_finish();
        return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    }
    /* Pass the alloca/GEP address (void**) so __zone_alloc can lazily initialize */
    LLVMValueRef size = gen_expr(cg, node->as.new_in_zone.size);
    size = coerce_int(cg, size, LLVMInt64TypeInContext(cg->ctx));
    LLVMValueRef args[2] = { zone_ptr_addr, size };
    return LLVMBuildCall2(cg->builder, cg->zone_alloc_type, cg->zone_alloc_fn, args, 2, "zalloc");
}

/* zone.move(ptr) — copy out of zone to independent heap allocation */
static LLVMValueRef gen_zone_move(cg_t *cg, node_t *node) {
    LLVMValueRef ptr = gen_expr(cg, node->as.zone_move.ptr);
    /* determine element size: use sizeof i8 as conservative default — caller
       casts the result.  The zone runtime copies the bytes. */
    LLVMValueRef size = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 1, 0);
    /* Pass void** (the alloca) so the runtime can clear the pointer if needed */
    LLVMValueRef zone_ptr_addr = LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
    if (node->as.zone_move.zone_name) {
        symbol_t *zsym = cg_lookup(cg, node->as.zone_move.zone_name);
        if (zsym)
            zone_ptr_addr = zsym->value; /* alloca = void** */
    }
    LLVMValueRef args[3] = { zone_ptr_addr, ptr, size };
    return LLVMBuildCall2(cg->builder, cg->zone_move_type, cg->zone_move_fn, args, 3, "zmove");
}

/* buf[unchecked: i] — identical to gen_index but never emits bounds check */
static LLVMValueRef gen_flagged_index(cg_t *cg, node_t *node) {
    /* Temporarily set in_unsafe to skip bounds checks in gen_index */
    cg->in_unsafe++;
    /* Reuse gen_index by temporarily changing the node kind */
    node->kind = NodeIndexExpr;
    node->as.index_expr.object = node->as.flagged_index.object;
    node->as.index_expr.index  = node->as.flagged_index.index;
    LLVMValueRef result = gen_index(cg, node);
    node->kind = NodeFlaggedIndex;
    cg->in_unsafe--;
    return result;
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
        /* look up by LLVM type pointer — avoids mismatch between LLVM struct
         * name (e.g. "dstring__dstring_t") and registry key ("dstring_t") */
        struct_reg_t *sr = find_struct_by_llvm_type(cg, ty);
        if (sr) {
            LLVMValueRef hfn = Null;
            if (sr->mod_prefix && sr->mod_prefix[0]) {
                char mname[512];
                snprintf(mname, sizeof(mname), "%s__%s__hash", sr->mod_prefix, sr->name);
                hfn = LLVMGetNamedFunction(cg->module, mname);
            }
            if (!hfn) {
                char mname[256];
                snprintf(mname, sizeof(mname), "%s.hash", sr->name);
                hfn = LLVMGetNamedFunction(cg->module, mname);
            }
            if (hfn) {
                LLVMValueRef tmp = alloc_in_entry(cg, ty, "hs_tmp");
                LLVMBuildStore(cg->builder, val, tmp);
                LLVMTypeRef fty = LLVMGlobalGetValueType(hfn);
                LLVMValueRef ca[1] = { tmp };
                LLVMValueRef r = LLVMBuildCall2(cg->builder, fty, hfn, ca, 1, "fh");
                return coerce_int(cg, r, i64);
            }
            return hash_struct_default(cg, val, sr);
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
        struct_reg_t *sr = find_struct_by_llvm_type(cg, ty);
        if (sr) {
            LLVMValueRef hfn = Null;
            if (sr->mod_prefix && sr->mod_prefix[0]) {
                char mname[512];
                snprintf(mname, sizeof(mname), "%s__%s__hash", sr->mod_prefix, sr->name);
                hfn = LLVMGetNamedFunction(cg->module, mname);
            }
            if (!hfn) {
                char mname[256];
                snprintf(mname, sizeof(mname), "%s.hash", sr->name);
                hfn = LLVMGetNamedFunction(cg->module, mname);
            }
            if (hfn) {
                LLVMValueRef tmp = alloc_in_entry(cg, ty, "hash_self");
                LLVMBuildStore(cg->builder, val, tmp);
                LLVMTypeRef fty = LLVMGlobalGetValueType(hfn);
                LLVMValueRef ca[1] = { tmp };
                LLVMValueRef r = LLVMBuildCall2(cg->builder, fty, hfn, ca, 1, "hash_r");
                return coerce_int(cg, r, i64);
            }
            return hash_struct_default(cg, val, sr);
        }
        return LLVMConstInt(i64, 0, 0);
    }
    return hash_primitive(cg, val, ty);
}

/* ── universal equality ── */

/* Forward declarations for mutual recursion */
static LLVMValueRef equ_value(cg_t *cg, LLVMValueRef lhs, LLVMValueRef rhs, LLVMTypeRef ty);
static LLVMValueRef equ_struct_default(cg_t *cg, LLVMValueRef lhs, LLVMValueRef rhs, struct_reg_t *sr);

/* Lookup the equ override function for a struct type (NULL if none defined). */
static LLVMValueRef find_equ_fn(cg_t *cg, struct_reg_t *sr) {
    if (sr->mod_prefix && sr->mod_prefix[0]) {
        char mname[512];
        snprintf(mname, sizeof(mname), "%s__%s__equ", sr->mod_prefix, sr->name);
        LLVMValueRef fn = LLVMGetNamedFunction(cg->module, mname);
        if (fn) return fn;
    }
    char mname[256];
    snprintf(mname, sizeof(mname), "%s.equ", sr->name);
    return LLVMGetNamedFunction(cg->module, mname);
}

static LLVMValueRef equ_value(cg_t *cg, LLVMValueRef lhs, LLVMValueRef rhs, LLVMTypeRef ty) {
    LLVMTypeRef i1  = LLVMInt1TypeInContext(cg->ctx);

    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
        /* look up by LLVM type pointer — avoids LLVM-name vs registry-key mismatch */
        struct_reg_t *sr = find_struct_by_llvm_type(cg, ty);
        if (sr) {
            LLVMValueRef efn = find_equ_fn(cg, sr);
            if (efn) {
                /* call override: equ(this_ptr, other_ptr) → bool */
                LLVMValueRef a_tmp = alloc_in_entry(cg, ty, "eq_a");
                LLVMValueRef b_tmp = alloc_in_entry(cg, ty, "eq_b");
                LLVMBuildStore(cg->builder, lhs, a_tmp);
                LLVMBuildStore(cg->builder, rhs, b_tmp);
                LLVMTypeRef fty = LLVMGlobalGetValueType(efn);
                LLVMValueRef ca[2] = { a_tmp, b_tmp };
                LLVMValueRef r = LLVMBuildCall2(cg->builder, fty, efn, ca, 2, "eq_r");
                return coerce_int(cg, r, i1);
            }
            return equ_struct_default(cg, lhs, rhs, sr);
        }
        return LLVMConstInt(i1, 0, 0); /* unknown struct: assume not equal (safe default) */
    }

    if (LLVMGetTypeKind(ty) == LLVMPointerTypeKind) {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
        LLVMValueRef li = LLVMBuildPtrToInt(cg->builder, lhs, i64, "lpi");
        LLVMValueRef ri = LLVMBuildPtrToInt(cg->builder, rhs, i64, "rpi");
        return LLVMBuildICmp(cg->builder, LLVMIntEQ, li, ri, "peq");
    }

    if (ty == LLVMFloatTypeInContext(cg->ctx) || ty == LLVMDoubleTypeInContext(cg->ctx)) {
        return LLVMBuildFCmp(cg->builder, LLVMRealOEQ, lhs, rhs, "feq");
    }

    /* integer / bool: coerce rhs to lhs type then compare */
    rhs = coerce_int(cg, rhs, ty);
    return LLVMBuildICmp(cg->builder, LLVMIntEQ, lhs, rhs, "ieq");
}

static LLVMValueRef equ_struct_default(cg_t *cg, LLVMValueRef lhs, LLVMValueRef rhs,
                                        struct_reg_t *sr) {
    LLVMTypeRef i1     = LLVMInt1TypeInContext(cg->ctx);
    LLVMValueRef result = LLVMConstInt(i1, 1, 0); /* start true */
    for (usize_t i = 0; i < sr->field_count; i++) {
        field_info_t *f  = &sr->fields[i];
        LLVMValueRef lf  = LLVMBuildExtractValue(cg->builder, lhs, (unsigned)f->index, "efl");
        LLVMValueRef rf  = LLVMBuildExtractValue(cg->builder, rhs, (unsigned)f->index, "efr");
        LLVMValueRef feq = equ_value(cg, lf, rf, LLVMTypeOf(lf));
        result = LLVMBuildAnd(cg->builder, result, feq, "eqand");
    }
    return result;
}

static LLVMValueRef gen_equ(cg_t *cg, node_t *node) {
    LLVMValueRef lhs = gen_expr(cg, node->as.equ_expr.left);
    LLVMValueRef rhs = gen_expr(cg, node->as.equ_expr.right);
    return equ_value(cg, lhs, rhs, LLVMTypeOf(lhs));
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
    /* 3-arg form: mov.(zone_name, ptr, size) — escape pointer from zone to heap */
    if (node->as.mov_expr.zone_name) {
        symbol_t *zsym = cg_lookup(cg, node->as.mov_expr.zone_name);
        if (!zsym || !(zsym->flags & SymZone)) {
            diag_begin_error("first argument to 3-arg mov.() must be a zone variable");
            diag_span(DIAG_NODE(node), True, "not a zone");
            diag_finish();
            return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
        }
        LLVMValueRef zone_ptr_addr = zsym->value; /* void** alloca */
        LLVMValueRef ptr = gen_expr(cg, node->as.mov_expr.ptr);
        LLVMValueRef sz  = gen_expr(cg, node->as.mov_expr.size);
        sz = coerce_int(cg, sz, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef args[3] = { zone_ptr_addr, ptr, sz };
        return LLVMBuildCall2(cg->builder, cg->zone_move_type, cg->zone_move_fn, args, 3, "zmove");
    }
    /* 2-arg form: mov.(ptr, size) — realloc */
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
                type_info_t elem_ti = ti_deref_one(sym->stype);
                LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                return LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidxptr");
            }
        }
        /* &self.field[idx] — address of element in a self-member pointer field */
        if (obj->kind == NodeSelfMemberExpr) {
            symbol_t *this_sym = cg_lookup(cg, "this");
            if (this_sym) {
                char *type_name = obj->as.self_member.type_name;
                if (!type_name) type_name = cg->current_struct_name;
                if (type_name && cg->generic_tmpl_name && cg->generic_inst_name
                        && strcmp(type_name, cg->generic_tmpl_name) == 0)
                    type_name = cg->generic_inst_name;
                struct_reg_t *sr = type_name ? find_struct(cg, type_name) : Null;
                if (sr) {
                    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                    LLVMValueRef this_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
                    const char *fname = obj->as.self_member.field;
                    for (usize_t fi = 0; fi < sr->field_count; fi++) {
                        if (strcmp(sr->fields[fi].name, fname) != 0) continue;
                        LLVMValueRef field_gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type,
                            this_ptr, (unsigned)sr->fields[fi].index, fname);
                        LLVMTypeRef ptr_type = get_llvm_type(cg, sr->fields[fi].type);
                        LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, "smfptr");
                        type_info_t elem_ti = ti_deref_one(sr->fields[fi].type);
                        LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                        index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                        /* GEP only — no load, we want the address */
                        return LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "smidxptr");
                    }
                }
            }
        }
    }
    /* &Type.(field) — address of a self-member field */
    if (operand->kind == NodeSelfMemberExpr) {
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (this_sym) {
            char *type_name = operand->as.self_member.type_name;
            if (!type_name) type_name = cg->current_struct_name;
            if (type_name && cg->generic_tmpl_name && cg->generic_inst_name
                    && strcmp(type_name, cg->generic_tmpl_name) == 0)
                type_name = cg->generic_inst_name;
            struct_reg_t *sr = type_name ? find_struct(cg, type_name) : Null;
            if (sr) {
                LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                LLVMValueRef this_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, this_sym->value, "this");
                const char *fname = operand->as.self_member.field;
                for (usize_t fi = 0; fi < sr->field_count; fi++) {
                    if (strcmp(sr->fields[fi].name, fname) != 0) continue;
                    return LLVMBuildStructGEP2(cg->builder, sr->llvm_type,
                        this_ptr, (unsigned)sr->fields[fi].index, fname);
                }
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

/* ── comptime format string: @'...' / heap @'...' ──
 * Produces an i8 * pointing to a freshly-formatted string.
 *   on_heap=False: buffer is alloca'd in the current frame (no cleanup).
 *   on_heap=True:  buffer is malloc'd; caller must rem.(). */
static LLVMValueRef gen_comptime_fmt(cg_t *cg, node_t *node) {
    const char  *fmt     = node->as.comptime_fmt.fmt;
    usize_t      flen    = node->as.comptime_fmt.fmt_len;
    node_list_t *args    = &node->as.comptime_fmt.args;
    boolean_t    on_heap = node->as.comptime_fmt.on_heap;

    /* Fast path: no args + stack → global string pointer, zero runtime cost. */
    if (args->count == 0 && !on_heap) {
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
        LLVMValueRef ptr = LLVMBuildGlobalStringPtr(cg->builder, buf, "cfmt.lit");
        deallocate(bh);
        return ptr;
    }

    usize_t argc = args->count;

    /* Evaluate all arg expressions up front. */
    heap_t vh = argc ? allocate(argc, sizeof(LLVMValueRef)) : NullHeap;
    LLVMValueRef *vals = argc ? vh.pointer : Null;
    for (usize_t i = 0; i < argc; i++)
        vals[i] = gen_expr(cg, args->items[i]);

    /* Walk the raw fmt once to: (a) build the C printf format,
     * (b) accumulate the compile-time buffer bound. */
    heap_t fh = allocate(flen * 4 + 64, 1);
    char *cfmt = fh.pointer;
    usize_t cfmt_len = 0;
    usize_t arg_idx = 0;
    usize_t buf_bound = 1; /* null terminator */

    for (usize_t i = 0; i < flen; ) {
        if (fmt[i] == '\\' && i + 1 < flen) {
            char ec = fmt[i + 1];
            if (ec == '{' || ec == '}') { cfmt[cfmt_len++] = ec; buf_bound++; }
            else if (ec == '%')         { cfmt[cfmt_len++] = '%'; cfmt[cfmt_len++] = '%'; buf_bound++; }
            else {
                char d = error_decode_esc(ec);
                if (d != '\0') { cfmt[cfmt_len++] = d; buf_bound++; }
            }
            i += 2; continue;
        }
        if (fmt[i] == '%') {
            cfmt[cfmt_len++] = '%'; cfmt[cfmt_len++] = '%';
            buf_bound++;
            i++; continue;
        }
        if (fmt[i] == '{' && arg_idx < argc) {
            /* scan to matching '}' respecting ()/[] nesting */
            usize_t j = i + 1;
            int depth = 0;
            usize_t colon_pos = 0;
            boolean_t has_colon = False;
            while (j < flen) {
                char c = fmt[j];
                if (c == '\\' && j + 1 < flen) { j += 2; continue; }
                if (c == '(' || c == '[') { depth++; j++; continue; }
                if (c == ')' || c == ']') { if (depth > 0) depth--; j++; continue; }
                if (c == '}' && depth == 0) break;
                if (c == ':' && depth == 0 && !has_colon) { colon_pos = j; has_colon = True; }
                j++;
            }
            if (j >= flen) { cfmt[cfmt_len++] = fmt[i++]; buf_bound++; continue; }

            const char *spec = Null; usize_t slen = 0;
            if (has_colon) {
                spec = fmt + colon_pos + 1;
                slen = j - (colon_pos + 1);
            }

            /* Extract numeric width from spec (for buffer sizing). */
            usize_t width = 0;
            if (slen > 0) {
                usize_t k = 0;
                while (k < slen && (spec[k] == '+' || spec[k] == '-'
                        || spec[k] == '#' || spec[k] == ' ')) k++;
                if (k < slen && spec[k] == '0'
                        && k + 1 < slen && spec[k + 1] >= '0' && spec[k + 1] <= '9') k++;
                while (k < slen && spec[k] >= '0' && spec[k] <= '9') {
                    width = width * 10 + (spec[k] - '0'); k++;
                }
                if (k < slen && spec[k] == '.') {
                    k++;
                    usize_t prec = 0;
                    while (k < slen && spec[k] >= '0' && spec[k] <= '9') {
                        prec = prec * 10 + (spec[k] - '0'); k++;
                    }
                    if (prec + 20 > width) width = prec + 20;
                }
            }

            LLVMTypeRef ty = LLVMTypeOf(vals[arg_idx]);
            usize_t arg_bound;
            if (llvm_is_ptr(ty)) {
                if (width == 0 && !on_heap) {
                    diag_begin_error("comptime format of pointer requires an explicit width, e.g. {p:64}");
                    diag_set_category(ErrCatPointerSafety);
                    diag_span(SRC_LOC(node->line, node->col, 0), True,
                              "pointer argument here has no '{:N}' width spec");
                    diag_note("stack-allocated comptime formats need each pointer's max length at compile time");
                    diag_help("either add a width like '{p:64}' or use 'heap @'...'' to malloc a larger buffer");
                    diag_finish();
                    arg_bound = 64;
                } else {
                    arg_bound = width > 0 ? width : 512;
                }
            } else if (llvm_is_float(ty)) {
                arg_bound = width > 32 ? width : 32;
            } else if (ty == LLVMInt64TypeInContext(cg->ctx)) {
                arg_bound = width > 20 ? width : 20;
            } else if (ty == LLVMInt32TypeInContext(cg->ctx)) {
                arg_bound = width > 11 ? width : 11;
            } else if (ty == LLVMInt16TypeInContext(cg->ctx)) {
                arg_bound = width > 6 ? width : 6;
            } else if (ty == LLVMInt8TypeInContext(cg->ctx)
                    || ty == LLVMInt1TypeInContext(cg->ctx)) {
                arg_bound = width > 4 ? width : 4;
            } else {
                arg_bound = width > 32 ? width : 32;
            }
            buf_bound += arg_bound;

            /* Emit %spec or type-default (mirrors gen_error_expr mapping). */
            cfmt[cfmt_len++] = '%';
            if (slen > 0) {
                for (usize_t k = 0; k < slen; k++) cfmt[cfmt_len++] = spec[k];
                /* widen scalars as printf ABI requires */
                if (llvm_is_float(ty) && ty == LLVMFloatTypeInContext(cg->ctx))
                    vals[arg_idx] = LLVMBuildFPExt(cg->builder, vals[arg_idx],
                                                    LLVMDoubleTypeInContext(cg->ctx), "fpext");
                else if (ty == LLVMInt8TypeInContext(cg->ctx))
                    vals[arg_idx] = LLVMBuildSExt(cg->builder, vals[arg_idx],
                                                   LLVMInt32TypeInContext(cg->ctx), "cext");
                else if (ty == LLVMInt1TypeInContext(cg->ctx))
                    vals[arg_idx] = LLVMBuildZExt(cg->builder, vals[arg_idx],
                                                   LLVMInt32TypeInContext(cg->ctx), "bext");
                else if (ty == LLVMInt16TypeInContext(cg->ctx))
                    vals[arg_idx] = LLVMBuildSExt(cg->builder, vals[arg_idx],
                                                   LLVMInt32TypeInContext(cg->ctx), "iext");
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
        cfmt[cfmt_len++] = fmt[i++]; buf_bound++;
    }
    cfmt[cfmt_len] = '\0';

    /* Declare snprintf once per module. */
    LLVMValueRef snprintf_fn = LLVMGetNamedFunction(cg->module, "snprintf");
    if (!snprintf_fn) {
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
        LLVMTypeRef fixed[3] = { ptr_ty, i64_ty, ptr_ty };
        LLVMTypeRef snfty = LLVMFunctionType(
            LLVMInt32TypeInContext(cg->ctx), fixed, 3, /*varargs=*/True);
        snprintf_fn = LLVMAddFunction(cg->module, "snprintf", snfty);
    }
    LLVMTypeRef snprintf_type = LLVMGlobalGetValueType(snprintf_fn);

    /* Allocate buffer — alloca for stack, malloc for heap. */
    usize_t final_size = on_heap ? (buf_bound > 512 ? buf_bound : 512) : buf_bound;
    if (final_size < 16) final_size = 16;
    LLVMValueRef buf_size_v = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), final_size, 0);
    LLVMValueRef buf;
    if (on_heap) {
        buf = LLVMBuildCall2(cg->builder, cg->malloc_type, cg->malloc_fn,
                             &buf_size_v, 1, "cfmt.buf");
    } else {
        LLVMTypeRef arr_ty = LLVMArrayType(
            LLVMInt8TypeInContext(cg->ctx), (unsigned)final_size);
        buf = alloc_in_entry(cg, arr_ty, "cfmt.buf");
    }

    /* snprintf(buf, size, cfmt, args...) */
    usize_t sn_argc = 3 + arg_idx;
    heap_t sah = allocate(sn_argc, sizeof(LLVMValueRef));
    LLVMValueRef *sn_args = sah.pointer;
    sn_args[0] = buf;
    sn_args[1] = buf_size_v;
    sn_args[2] = LLVMBuildGlobalStringPtr(cg->builder, cfmt, "cfmt.str");
    for (usize_t i = 0; i < arg_idx; i++)
        sn_args[3 + i] = vals[i];
    LLVMBuildCall2(cg->builder, snprintf_type, snprintf_fn,
                   sn_args, (unsigned)sn_argc, "");

    deallocate(sah);
    deallocate(fh);
    if (argc) deallocate(vh);

    return buf;
}

/* ── any type helpers ── */

/* Map a short type name to type_info_t (for any_G_... mangled names). */
static type_info_t type_info_from_name(const char *name) {
    type_info_t ti = NO_TYPE;
    if (strcmp(name, "i8")   == 0) { ti.base = TypeI8;   return ti; }
    if (strcmp(name, "i16")  == 0) { ti.base = TypeI16;  return ti; }
    if (strcmp(name, "i32")  == 0) { ti.base = TypeI32;  return ti; }
    if (strcmp(name, "i64")  == 0) { ti.base = TypeI64;  return ti; }
    if (strcmp(name, "u8")   == 0) { ti.base = TypeU8;   return ti; }
    if (strcmp(name, "u16")  == 0) { ti.base = TypeU16;  return ti; }
    if (strcmp(name, "u32")  == 0) { ti.base = TypeU32;  return ti; }
    if (strcmp(name, "u64")  == 0) { ti.base = TypeU64;  return ti; }
    if (strcmp(name, "f32")  == 0) { ti.base = TypeF32;  return ti; }
    if (strcmp(name, "f64")  == 0) { ti.base = TypeF64;  return ti; }
    if (strcmp(name, "bool") == 0) { ti.base = TypeBool; return ti; }
    if (strcmp(name, "ptr")  == 0) {
        ti.base = TypeI8; ti.is_pointer = True;
        ti.ptr_perm = PtrReadWrite; return ti;
    }
    ti.base = TypeUser;
    /* NOTE: name points into a stack buffer — OK for immediate use in get_llvm_type */
    ti.user_name = (char *)name;
    return ti;
}

/* Byte size of a type (for computing max payload in any.[...] structs). */
static usize_t type_size_bytes(cg_t *cg, type_info_t ti) {
    if (ti.is_pointer) return 8;
    switch (ti.base) {
        case TypeBool: case TypeI8:  case TypeU8:  return 1;
        case TypeI16:  case TypeU16:               return 2;
        case TypeI32:  case TypeU32: case TypeF32: return 4;
        case TypeI64:  case TypeU64: case TypeF64: return 8;
        case TypeUser: {
            if (ti.user_name) {
                struct_reg_t *sr = find_struct(cg, ti.user_name);
                if (sr) {
                    LLVMTargetDataRef dl = LLVMGetModuleDataLayout(cg->module);
                    return (usize_t)LLVMStoreSizeOfType(dl, sr->llvm_type);
                }
            }
            return 8;
        }
        default: return 8;
    }
}

/* Instantiate an any_G_T1_G_T2_... type on first use.
   Returns the struct_reg_t entry (possibly already existing). */
static struct_reg_t *try_instantiate_any(cg_t *cg, const char *name) {
    struct_reg_t *existing = find_struct(cg, name);
    if (existing) return existing;

    /* must start with "any_G_" */
    if (strncmp(name, "any_G_", 6) != 0) return Null;

    /* parse variant type names */
    char type_names[16][128];
    usize_t type_count = 0;
    const char *p = name + 3; /* skip "any" */
    while (*p && type_count < 16) {
        if (strncmp(p, "_G_", 3) != 0) break;
        p += 3;
        usize_t len = 0;
        while (p[len] && strncmp(p + len, "_G_", 3) != 0)
            len++;
        if (len > 0 && len < 128) {
            memcpy(type_names[type_count], p, len);
            type_names[type_count][len] = '\0';
            type_count++;
        }
        p += len;
    }
    if (type_count == 0) return Null;

    /* map to type_info_t and find max size */
    type_info_t variants[16];
    usize_t max_size = 1;
    for (usize_t i = 0; i < type_count; i++) {
        variants[i] = type_info_from_name(type_names[i]);
        usize_t sz = type_size_bytes(cg, variants[i]);
        if (sz > max_size) max_size = sz;
    }

    /* build LLVM struct: { i32 tag; [max_size x i8] data } */
    LLVMTypeRef i32_ty  = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i8_ty   = LLVMInt8TypeInContext(cg->ctx);
    LLVMTypeRef data_ty = LLVMArrayType2(i8_ty, (unsigned long long)max_size);
    LLVMTypeRef field_tys[2] = { i32_ty, data_ty };
    LLVMTypeRef struct_ty = LLVMStructTypeInContext(cg->ctx, field_tys, 2, 0);

    /* register the struct */
    char *heap_name = (char *)name; /* name lifetime: mangled strings from ast_strdup are permanent */
    register_struct(cg, heap_name, struct_ty, False);
    struct_reg_t *sr = find_struct(cg, name);
    if (!sr) return Null;

    /* add field metadata */
    type_info_t tag_ti = NO_TYPE; tag_ti.base = TypeI32;
    struct_add_field(sr, "tag",  tag_ti, 0, LinkageNone, StorageStack);
    type_info_t data_ti = NO_TYPE; data_ti.base = TypeU8;
    struct_add_field(sr, "data", data_ti, 1, LinkageNone, StorageStack);

    /* store any-type metadata */
    sr->is_any_type = True;
    sr->any_variant_count = type_count;
    for (usize_t i = 0; i < type_count; i++)
        sr->any_variants[i] = variants[i];

    return sr;
}

/* Wrap a value into an any.[...] struct.
   sr   = the any struct registry entry
   val  = value to wrap
   tag  = variant index */
static LLVMValueRef wrap_in_any(cg_t *cg, struct_reg_t *sr,
                                 LLVMValueRef val, usize_t tag) {
    LLVMValueRef tmp = alloc_in_entry(cg, sr->llvm_type, "any_tmp");
    /* zero-initialize */
    LLVMBuildStore(cg->builder, LLVMConstNull(sr->llvm_type), tmp);
    /* store tag */
    LLVMValueRef tag_ptr = LLVMBuildStructGEP2(
        cg->builder, sr->llvm_type, tmp, 0, "any_tag_ptr");
    LLVMBuildStore(cg->builder,
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned long long)tag, 0),
        tag_ptr);
    /* store value into data field — works with opaque pointers */
    LLVMValueRef data_ptr = LLVMBuildStructGEP2(
        cg->builder, sr->llvm_type, tmp, 1, "any_data_ptr");
    LLVMBuildStore(cg->builder, val, data_ptr);
    return LLVMBuildLoad2(cg->builder, sr->llvm_type, tmp, "any_val");
}

/* ── NodeConstructorCall: type_name.(args) → type_name.new(args) ── */

static LLVMValueRef gen_constructor_call(cg_t *cg, node_t *node) {
    const char *type_name = node->as.ctor_call.type_name;

    /* Resolve generic substitution if inside a generic instantiation */
    if (cg->generic_n > 0) {
        const char *sub = cg_subst_name(cg, type_name);
        if (sub != type_name) type_name = sub;
    }
    if (cg->generic_tmpl_name && cg->generic_inst_name &&
            strcmp(type_name, cg->generic_tmpl_name) == 0)
        type_name = cg->generic_inst_name;

    /* Trigger generic instantiation if needed */
    if (strstr(type_name, "_G_"))
        try_instantiate_generic(cg, type_name);

    /* Build mangled method name: type_name__new (or legacy type_name.new) */
    char mangled[512];
    symbol_t *fn_sym = Null;

    /* Try module-prefixed form */
    {
        struct_reg_t *sr = find_struct(cg, type_name);
        const char *pfx = (sr && sr->mod_prefix && sr->mod_prefix[0])
                          ? sr->mod_prefix
                          : (cg->current_module_prefix[0] ? cg->current_module_prefix : Null);
        if (pfx) {
            snprintf(mangled, sizeof(mangled), "%s__%s__new", pfx, type_name);
            fn_sym = cg_lookup(cg, mangled);
        }
    }
    if (!fn_sym) {
        snprintf(mangled, sizeof(mangled), "%s__new", type_name);
        fn_sym = cg_lookup(cg, mangled);
    }
    if (!fn_sym) {
        /* legacy: type_name.new */
        snprintf(mangled, sizeof(mangled), "%s.new", type_name);
        fn_sym = cg_lookup(cg, mangled);
    }
    if (!fn_sym) {
        /* If no args and struct has no (non-interface) fields, auto-generate a zero val */
        if (node->as.ctor_call.args.count == 0) {
            struct_reg_t *sr = find_struct(cg, type_name);
            if (sr && !sr->is_interface && sr->field_count == 0) {
                return LLVMConstNull(sr->llvm_type);
            }
        }
        diag_begin_error("undefined constructor for type '%s'", type_name);
        diag_span(DIAG_NODE(node), True, "no 'new' method found");
        diag_help("define: fn %s.new(...): %s { ... }", type_name, type_name);
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
    usize_t user_argc = node->as.ctor_call.args.count;
    unsigned n_params = LLVMCountParamTypes(fn_type);
    usize_t argc = user_argc;

    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (argc > 0) {
        args_heap = allocate(argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        for (usize_t i = 0; i < argc; i++)
            args[i] = gen_expr(cg, node->as.ctor_call.args.items[i]);
    }
    /* coerce to param types */
    if (argc > 0 && n_params > 0) {
        heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
        LLVMTypeRef *ptypes = pt_heap.pointer;
        LLVMGetParamTypes(fn_type, ptypes);
        for (usize_t i = 0; i < argc && i < (usize_t)n_params; i++)
            args[i] = coerce_int(cg, args[i], ptypes[i]);
        deallocate(pt_heap);
    }
    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                       args, (unsigned)argc, "");
    if (argc > 0) deallocate(args_heap);
    return ret;
}

/* ── NodeErrPropCall: fn.?(args) — call fn; if error, return it early ── */

static LLVMValueRef gen_err_prop_call(cg_t *cg, node_t *node) {
    const char *callee = node->as.err_prop_call.callee;
    symbol_t *fn_sym = cg_lookup(cg, callee);
    if (!fn_sym && cg->current_module_prefix[0]) {
        char mod_name[512];
        snprintf(mod_name, sizeof(mod_name), "%s__%s", cg->current_module_prefix, callee);
        fn_sym = cg_lookup(cg, mod_name);
    }
    if (!fn_sym) {
        diag_begin_error("undefined function '%s' in error propagation call", callee);
        diag_span(DIAG_NODE(node), True, "function not found");
        diag_finish();
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
    usize_t user_argc = node->as.err_prop_call.args.count;
    unsigned n_params = LLVMCountParamTypes(fn_type);

    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (user_argc > 0) {
        args_heap = allocate(user_argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        for (usize_t i = 0; i < user_argc; i++)
            args[i] = gen_expr(cg, node->as.err_prop_call.args.items[i]);
        if (n_params > 0) {
            heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
            LLVMTypeRef *ptypes = pt_heap.pointer;
            LLVMGetParamTypes(fn_type, ptypes);
            for (usize_t i = 0; i < user_argc && i < (usize_t)n_params; i++)
                args[i] = coerce_int(cg, args[i], ptypes[i]);
            deallocate(pt_heap);
        }
    }

    LLVMValueRef result = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                          args, (unsigned)user_argc, "errprop");
    if (user_argc > 0) deallocate(args_heap);

    /* result is an error struct { i1 has_error; ptr msg } */
    LLVMValueRef has_err = LLVMBuildExtractValue(cg->builder, result, 0, "err_flag");

    LLVMBasicBlockRef prop_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->current_fn, "errprop.ret");
    LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->current_fn, "errprop.cont");
    LLVMBuildCondBr(cg->builder, has_err, prop_bb, cont_bb);

    /* propagate: return the error from current function */
    LLVMPositionBuilderAtEnd(cg->builder, prop_bb);
    LLVMBuildRet(cg->builder, result);

    LLVMPositionBuilderAtEnd(cg->builder, cont_bb);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

/* ── NodeErrProp: expr? — evaluate expr; if error flag set, propagate ── */
/* Semantics: expr must yield an error struct { i1 has_err; ptr msg }.
   If has_err is set, early-return the error from the current function
   (which must itself return error).  Otherwise execution continues.
   Mirrors gen_err_prop_call; valid only inside error-returning functions. */

static LLVMValueRef gen_err_prop(cg_t *cg, node_t *node) {
    LLVMValueRef result = gen_expr(cg, node->as.err_prop.operand);

    LLVMTypeRef result_type = LLVMTypeOf(result);
    if (LLVMGetTypeKind(result_type) != LLVMStructTypeKind ||
        LLVMCountStructElementTypes(result_type) < 1) {
        /* operand does not have error type — no-op propagation */
        return result;
    }

    LLVMValueRef has_err = LLVMBuildExtractValue(cg->builder, result, 0, "err_flag");

    LLVMBasicBlockRef prop_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->current_fn, "errop.ret");
    LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->current_fn, "errop.cont");
    LLVMBuildCondBr(cg->builder, has_err, prop_bb, cont_bb);

    /* propagate path: return the error (enclosing function must return error) */
    LLVMPositionBuilderAtEnd(cg->builder, prop_bb);
    LLVMBuildRet(cg->builder, result);

    LLVMPositionBuilderAtEnd(cg->builder, cont_bb);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

/* ── slice helpers ──────────────────────────────────────────────────────────── */

/* Apply generic type-param substitution to a slice element type. */
static LLVMTypeRef get_slice_elem_llvm_type(cg_t *cg, type_info_t elem_ti) {
    if (elem_ti.base == TypeUser && elem_ti.user_name && cg->generic_n > 0) {
        for (usize_t i = 0; i < cg->generic_n; i++) {
            if (strcmp(elem_ti.user_name, cg->generic_params[i]) == 0) {
                elem_ti = cg->generic_concs[i];
                break;
            }
        }
        if (elem_ti.base == TypeUser && elem_ti.user_name) {
            const char *sub = cg_subst_name(cg, elem_ti.user_name);
            if (sub != elem_ti.user_name) elem_ti.user_name = (char *)sub;
        }
    }
    return get_llvm_type(cg, elem_ti);
}

/* Build the anonymous slice struct type { ptr, i32, i32 }. */
static LLVMTypeRef slice_struct_type(cg_t *cg) {
    LLVMTypeRef fields[3] = {
        LLVMPointerTypeInContext(cg->ctx, 0),
        LLVMInt32TypeInContext(cg->ctx),
        LLVMInt32TypeInContext(cg->ctx),
    };
    return LLVMStructTypeInContext(cg->ctx, fields, 3, 0);
}

/* Get element LLVM type from a symbol whose stype is TypeSlice. */
static LLVMTypeRef elem_type_from_sym(cg_t *cg, symbol_t *sym) {
    if (sym && sym->stype.base == TypeSlice && sym->stype.elem_type)
        return get_slice_elem_llvm_type(cg, sym->stype.elem_type[0]);
    return LLVMInt8TypeInContext(cg->ctx);
}

/* ── NodeSliceExpr: arr[lo:hi] / arr[:] / arr[lo:] / arr[:hi] ── */

static LLVMValueRef gen_slice_expr(cg_t *cg, node_t *node) {
    node_t *obj    = node->as.slice_expr.object;
    node_t *lo_nd  = node->as.slice_expr.lo;
    node_t *hi_nd  = node->as.slice_expr.hi;

    LLVMTypeRef i32_ty  = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i64_ty  = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef ptr_ty  = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef sl_ty   = slice_struct_type(cg);

    LLVMValueRef base_ptr = Null;
    LLVMValueRef orig_len = Null;
    LLVMValueRef orig_cap = Null;
    LLVMTypeRef  elem_ty  = LLVMInt8TypeInContext(cg->ctx);

    if (obj->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
        if (sym) {
            if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind) {
                /* fixed array → borrow as slice */
                long arr_sz = sym->array_size > 0 ? sym->array_size : 0;
                LLVMValueRef zero = LLVMConstInt(i32_ty, 0, 0);
                LLVMValueRef idx[2] = { zero, zero };
                base_ptr = LLVMBuildGEP2(cg->builder, sym->type, sym->value, idx, 2, "arr2sl.ptr");
                elem_ty  = LLVMGetElementType(sym->type);
                orig_len = LLVMConstInt(i32_ty, (unsigned long long)arr_sz, 0);
                orig_cap = orig_len;
            } else if (sym->stype.base == TypeSlice) {
                /* slice → reslice */
                LLVMValueRef sl = LLVMBuildLoad2(cg->builder, sl_ty, sym->value, "rsl");
                base_ptr = LLVMBuildExtractValue(cg->builder, sl, 0, "rsl.ptr");
                orig_len = LLVMBuildExtractValue(cg->builder, sl, 1, "rsl.len");
                orig_cap = LLVMBuildExtractValue(cg->builder, sl, 2, "rsl.cap");
                elem_ty  = elem_type_from_sym(cg, sym);
            }
        }
    }

    /* fallback: evaluate the object as an expression */
    if (!base_ptr) {
        LLVMValueRef val = gen_expr(cg, obj);
        if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMStructTypeKind) {
            base_ptr = LLVMBuildExtractValue(cg->builder, val, 0, "rsl.ptr");
            orig_len = LLVMBuildExtractValue(cg->builder, val, 1, "rsl.len");
            orig_cap = LLVMBuildExtractValue(cg->builder, val, 2, "rsl.cap");
        } else if (llvm_is_ptr(LLVMTypeOf(val))) {
            base_ptr = val;
            orig_len = LLVMConstInt(i32_ty, 0, 0);
            orig_cap = orig_len;
        } else {
            return LLVMConstNull(sl_ty);
        }
    }

    LLVMValueRef lo = lo_nd
        ? coerce_int(cg, gen_expr(cg, lo_nd), i32_ty)
        : LLVMConstInt(i32_ty, 0, 0);
    LLVMValueRef hi = hi_nd
        ? coerce_int(cg, gen_expr(cg, hi_nd), i32_ty)
        : orig_len;

    /* new_ptr = GEP(base_ptr, lo) */
    LLVMValueRef lo64    = LLVMBuildZExt(cg->builder, lo, i64_ty, "lo64");
    LLVMValueRef new_ptr = LLVMBuildGEP2(cg->builder, elem_ty, base_ptr, &lo64, 1, "sl.nptr");

    /* new_len = hi - lo,  new_cap = orig_cap - lo */
    LLVMValueRef new_len = LLVMBuildSub(cg->builder, hi,       lo, "sl.nlen");
    LLVMValueRef new_cap = LLVMBuildSub(cg->builder, orig_cap, lo, "sl.ncap");

    LLVMValueRef result = LLVMGetUndef(sl_ty);
    result = LLVMBuildInsertValue(cg->builder, result, new_ptr, 0, "sl.0");
    result = LLVMBuildInsertValue(cg->builder, result, new_len, 1, "sl.1");
    result = LLVMBuildInsertValue(cg->builder, result, new_cap, 2, "sl.2");
    return result;
}

/* ── NodeMakeExpr: make.([]T, len) / make.([]T, len, cap) ── */

static LLVMValueRef gen_make_expr(cg_t *cg, node_t *node) {
    type_info_t elem_ti = node->as.make_expr.elem_type;
    LLVMTypeRef elem_ty = get_slice_elem_llvm_type(cg, elem_ti);

    LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef sl_ty  = slice_struct_type(cg);

    LLVMValueRef len_val = coerce_int(cg, gen_expr(cg, node->as.make_expr.len), i32_ty);
    LLVMValueRef cap_val = node->as.make_expr.cap
        ? coerce_int(cg, gen_expr(cg, node->as.make_expr.cap), i32_ty)
        : len_val;

    /* bytes = (i64)cap * sizeof(elem) */
    LLVMValueRef elem_sz = LLVMSizeOf(elem_ty);
    LLVMValueRef cap64   = LLVMBuildZExt(cg->builder, cap_val, i64_ty, "cap64");
    LLVMValueRef bytes   = LLVMBuildMul(cg->builder, cap64, elem_sz, "mk.bytes");

    /* ptr = malloc(bytes); zero-init with memset */
    LLVMValueRef margs[1] = { bytes };
    LLVMValueRef ptr = LLVMBuildCall2(cg->builder, cg->malloc_type, cg->malloc_fn,
                                      margs, 1, "mk.ptr");
    LLVMBuildMemSet(cg->builder, ptr,
                    LLVMConstInt(LLVMInt8TypeInContext(cg->ctx), 0, 0),
                    bytes, 0);

    LLVMValueRef result = LLVMGetUndef(sl_ty);
    result = LLVMBuildInsertValue(cg->builder, result, ptr,     0, "mk.0");
    result = LLVMBuildInsertValue(cg->builder, result, len_val, 1, "mk.1");
    result = LLVMBuildInsertValue(cg->builder, result, cap_val, 2, "mk.2");
    return result;
}

/* ── NodeAppendExpr: append.(slice, val) ── */

static LLVMValueRef gen_append_expr(cg_t *cg, node_t *node) {
    node_t *slice_nd = node->as.append_expr.slice;
    node_t *val_nd   = node->as.append_expr.val;

    LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef sl_ty  = slice_struct_type(cg);

    /* determine element type from the slice variable */
    LLVMTypeRef elem_ty = LLVMInt8TypeInContext(cg->ctx);
    if (slice_nd->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, slice_nd->as.ident.name);
        elem_ty = elem_type_from_sym(cg, sym);
    }
    LLVMValueRef elem_sz = LLVMSizeOf(elem_ty);

    /* load or synthesize the slice struct (nil → zero struct) */
    LLVMValueRef sl;
    if (slice_nd->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, slice_nd->as.ident.name);
        if (sym && sym->stype.base == TypeSlice)
            sl = LLVMBuildLoad2(cg->builder, sl_ty, sym->value, "ap.sl");
        else
            sl = LLVMConstNull(sl_ty);
    } else {
        sl = gen_expr(cg, slice_nd);
        if (LLVMGetTypeKind(LLVMTypeOf(sl)) != LLVMStructTypeKind)
            sl = LLVMConstNull(sl_ty);
    }

    LLVMValueRef old_ptr = LLVMBuildExtractValue(cg->builder, sl, 0, "ap.optr");
    LLVMValueRef old_len = LLVMBuildExtractValue(cg->builder, sl, 1, "ap.olen");
    LLVMValueRef old_cap = LLVMBuildExtractValue(cg->builder, sl, 2, "ap.ocap");

    /* if len == cap, reallocate */
    LLVMValueRef need_grow = LLVMBuildICmp(cg->builder, LLVMIntEQ, old_len, old_cap, "ap.grow");

    LLVMBasicBlockRef grow_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "ap.grow");
    LLVMBasicBlockRef cont_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "ap.cont");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "ap.merge");

    LLVMBuildCondBr(cg->builder, need_grow, grow_bb, cont_bb);

    /* grow path: new_cap = cap == 0 ? 1 : cap*2; realloc(ptr, new_cap*elem_sz) */
    LLVMPositionBuilderAtEnd(cg->builder, grow_bb);
    LLVMValueRef is_zero  = LLVMBuildICmp(cg->builder, LLVMIntEQ, old_cap,
                                           LLVMConstInt(i32_ty, 0, 0), "ap.czero");
    LLVMValueRef dbl_cap  = LLVMBuildMul(cg->builder, old_cap,
                                          LLVMConstInt(i32_ty, 2, 0), "ap.dbl");
    LLVMValueRef new_cap_g = LLVMBuildSelect(cg->builder, is_zero,
                                              LLVMConstInt(i32_ty, 1, 0), dbl_cap, "ap.ncap");
    LLVMValueRef ncap64   = LLVMBuildZExt(cg->builder, new_cap_g, i64_ty, "ap.ncap64");
    LLVMValueRef nbytes   = LLVMBuildMul(cg->builder, ncap64, elem_sz, "ap.nbytes");
    LLVMValueRef realloc_args[2] = { old_ptr, nbytes };
    LLVMValueRef new_ptr_g = LLVMBuildCall2(cg->builder, cg->realloc_type, cg->realloc_fn,
                                             realloc_args, 2, "ap.nptr");
    LLVMBuildBr(cg->builder, merge_bb);

    /* no-grow path: keep old ptr/cap */
    LLVMPositionBuilderAtEnd(cg->builder, cont_bb);
    LLVMBuildBr(cg->builder, merge_bb);

    /* merge: phi for ptr and cap */
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);

    LLVMValueRef phi_ptr = LLVMBuildPhi(cg->builder, ptr_ty, "ap.rptr");
    {
        LLVMValueRef phi_vals[2] = { new_ptr_g, old_ptr };
        LLVMBasicBlockRef phi_bbs[2] = { grow_bb, cont_bb };
        LLVMAddIncoming(phi_ptr, phi_vals, phi_bbs, 2);
    }

    LLVMValueRef phi_cap = LLVMBuildPhi(cg->builder, i32_ty, "ap.rcap");
    {
        LLVMValueRef phi_vals[2] = { new_cap_g, old_cap };
        LLVMBasicBlockRef phi_bbs[2] = { grow_bb, cont_bb };
        LLVMAddIncoming(phi_cap, phi_vals, phi_bbs, 2);
    }

    /* store val at ptr[len] */
    LLVMValueRef ilen64 = LLVMBuildZExt(cg->builder, old_len, i64_ty, "ap.ilen64");
    LLVMValueRef slot   = LLVMBuildGEP2(cg->builder, elem_ty, phi_ptr, &ilen64, 1, "ap.slot");
    LLVMValueRef val    = coerce_int(cg, gen_expr(cg, val_nd), elem_ty);
    LLVMBuildStore(cg->builder, val, slot);

    LLVMValueRef new_len = LLVMBuildAdd(cg->builder, old_len,
                                         LLVMConstInt(i32_ty, 1, 0), "ap.nlen");

    LLVMValueRef result = LLVMGetUndef(sl_ty);
    result = LLVMBuildInsertValue(cg->builder, result, phi_ptr, 0, "ap.s0");
    result = LLVMBuildInsertValue(cg->builder, result, new_len,  1, "ap.s1");
    result = LLVMBuildInsertValue(cg->builder, result, phi_cap,  2, "ap.s2");
    return result;
}

/* ── NodeCopyExpr: copy.(dst, src) → i32 ── */

static LLVMValueRef gen_copy_expr(cg_t *cg, node_t *node) {
    LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);

    /* determine element type from dst */
    LLVMTypeRef elem_ty = LLVMInt8TypeInContext(cg->ctx);
    if (node->as.copy_expr.dst->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, node->as.copy_expr.dst->as.ident.name);
        elem_ty = elem_type_from_sym(cg, sym);
    }
    LLVMValueRef elem_sz = LLVMSizeOf(elem_ty);

    LLVMValueRef dst_sl = gen_expr(cg, node->as.copy_expr.dst);
    LLVMValueRef src_sl = gen_expr(cg, node->as.copy_expr.src);

    LLVMValueRef dst_ptr = LLVMBuildExtractValue(cg->builder, dst_sl, 0, "cp.dptr");
    LLVMValueRef dst_len = LLVMBuildExtractValue(cg->builder, dst_sl, 1, "cp.dlen");
    LLVMValueRef src_ptr = LLVMBuildExtractValue(cg->builder, src_sl, 0, "cp.sptr");
    LLVMValueRef src_len = LLVMBuildExtractValue(cg->builder, src_sl, 1, "cp.slen");

    /* n = min(dst_len, src_len) */
    LLVMValueRef cmp = LLVMBuildICmp(cg->builder, LLVMIntSLE, dst_len, src_len, "cp.cmp");
    LLVMValueRef n   = LLVMBuildSelect(cg->builder, cmp, dst_len, src_len, "cp.n");

    /* bytes = n * sizeof(elem); memmove for overlap safety */
    LLVMValueRef n64   = LLVMBuildZExt(cg->builder, n, i64_ty, "cp.n64");
    LLVMValueRef bytes = LLVMBuildMul(cg->builder, n64, elem_sz, "cp.bytes");
    LLVMBuildMemMove(cg->builder, dst_ptr, 0, src_ptr, 0, bytes);

    return n;
}

/* ── NodeLenExpr / NodeCapExpr ── */

static LLVMValueRef gen_len_expr(cg_t *cg, node_t *node) {
    LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->ctx);
    LLVMValueRef val   = gen_expr(cg, node->as.len_expr.operand);
    if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMStructTypeKind)
        return LLVMBuildExtractValue(cg->builder, val, 1, "sl.len");
    return LLVMConstInt(i32_ty, 0, 0);
}

static LLVMValueRef gen_cap_expr(cg_t *cg, node_t *node) {
    LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->ctx);
    LLVMValueRef val   = gen_expr(cg, node->as.len_expr.operand);
    if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMStructTypeKind)
        return LLVMBuildExtractValue(cg->builder, val, 2, "sl.cap");
    return LLVMConstInt(i32_ty, 0, 0);
}

/* ── NodeAnyTypeExpr: any.(expr) — extract type discriminant tag ── */

static LLVMValueRef gen_any_type_expr(cg_t *cg, node_t *node) {
    LLVMValueRef val = gen_expr(cg, node->as.any_type_expr.operand);
    /* val is an any struct; extract field 0 (the tag) */
    return LLVMBuildExtractValue(cg->builder, val, 0, "any_tag");
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
        case NodeAsyncCall:        return gen_async_call(cg, node);
        case NodeAwaitExpr:        return gen_await(cg, node);
        case NodeAwaitCombinator:  return gen_await_combinator(cg, node);
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
        case NodeEquExpr:          return gen_equ(cg, node);
        case NodeNilExpr:          return gen_nil(cg);
        case NodeMovExpr:          return gen_mov(cg, node);
        case NodeAddrOf:           return gen_addr_of(cg, node);
        case NodeErrorExpr:        return gen_error_expr(cg, node);
        case NodeComptimeFmt:      return gen_comptime_fmt(cg, node);
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
        case NodeCompoundInit:
            return gen_compound_init(cg, node);
        case NodeDesigInit: {
            const char *di_name = node->as.desig_init.type_name;
            if (di_name && cg->generic_n > 0) {
                const char *sub = cg_subst_name(cg, di_name);
                if (sub != di_name) di_name = sub;
            }
            if (di_name && cg->generic_tmpl_name && cg->generic_inst_name
                    && strcmp(di_name, cg->generic_tmpl_name) == 0)
                di_name = cg->generic_inst_name;
            struct_reg_t *sr = find_struct(cg, di_name);
            if (!sr && di_name != node->as.desig_init.type_name)
                sr = find_struct(cg, node->as.desig_init.type_name);
            if (!sr) {
                diag_begin_error("unknown struct '%s' in designated initializer",
                        node->as.desig_init.type_name);
                diag_finish();
                return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
            LLVMTypeRef saved_hint = cg->hint_ret_type;
            cg->hint_ret_type = sr->llvm_type;
            node_t lowered = {0};
            lowered.kind = NodeCompoundInit;
            node_list_init(&lowered.as.compound_init.items);
            for (usize_t di = 0; di < node->as.desig_init.fields.count; di++) {
                node_t *fname_node = node->as.desig_init.fields.items[di];
                node_t *fval_node  = node->as.desig_init.values.items[di];
                node_t *item = make_node(NodeInitField, fname_node->line);
                item->as.init_field.name = fname_node->as.ident.name;
                item->as.init_field.value = fval_node;
                node_list_push(&lowered.as.compound_init.items, item);
            }
            LLVMValueRef val = gen_compound_init(cg, &lowered);
            cg->hint_ret_type = saved_hint;
            return val;
        }
        case NodeRangeExpr:
            diag_begin_error("range expressions can only appear in compound initializers");
            diag_span(DIAG_NODE(node), True, "used here");
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        case NodeSpreadExpr:
        case NodeInitField:
        case NodeInitIndex:
            diag_begin_error("initializer-only syntax used outside a compound initializer");
            diag_span(DIAG_NODE(node), True, "used here");
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        case NodeRemStmt: {
            node_t *ptr_node = node->as.rem_stmt.ptr;
            /* zone member: rem.(this.xyz) or rem.(s.xyz) */
            if (ptr_node->kind == NodeSelfMemberExpr || ptr_node->kind == NodeMemberExpr) {
                LLVMValueRef zone_addr = get_zone_field_addr(cg, ptr_node);
                if (zone_addr) {
                    LLVMValueRef fa[1] = { zone_addr };
                    LLVMBuildCall2(cg->builder, cg->zone_free_type, cg->zone_free_fn, fa, 1, "");
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                }
            }
            if (ptr_node->kind == NodeIdentExpr) {
                symbol_t *hsym = cg_lookup(cg, ptr_node->as.ident.name);
                /* zone variable: call __zone_free(&zone_ptr) so it nulls *zone_ptr */
                if (hsym && (hsym->flags & SymZone)) {
                    LLVMValueRef fa[1] = { hsym->value }; /* alloca = void** */
                    LLVMBuildCall2(cg->builder, cg->zone_free_type, cg->zone_free_fn, fa, 1, "");
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                }
                /* zone-allocated pointer: rem.() is a no-op (zone owns the memory) */
                if (hsym && (hsym->flags & SymZoneAlloc)) {
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                }
                /* guard: cannot rem.() a stack-qualified pointer */
                if (cg->in_unsafe == 0 && hsym && hsym->stype.is_pointer
                        && hsym->storage == StorageStack) {
                    diag_begin_error("cannot call rem.() on a stack pointer");
                    diag_span(DIAG_NODE(node), True, "'%s' is declared as a stack pointer",
                              ptr_node->as.ident.name);
                    diag_note("only heap pointers (declared with 'heap') can be freed");
                    diag_finish();
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                }
                /* heap []T slice: load slice struct, extract data ptr, call free */
                if (hsym && hsym->stype.base == TypeSlice && hsym->storage == StorageHeap) {
                    LLVMTypeRef ptr_ty  = LLVMPointerTypeInContext(cg->ctx, 0);
                    LLVMTypeRef i32_ty  = LLVMInt32TypeInContext(cg->ctx);
                    LLVMTypeRef sfields[3] = { ptr_ty, i32_ty, i32_ty };
                    LLVMTypeRef sl_ty   = LLVMStructTypeInContext(cg->ctx, sfields, 3, 0);
                    LLVMValueRef sl     = LLVMBuildLoad2(cg->builder, sl_ty, hsym->value, "sl");
                    LLVMValueRef dptr   = LLVMBuildExtractValue(cg->builder, sl, 0, "sl.ptr");
                    LLVMValueRef fargs[1] = { dptr };
                    LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, fargs, 1, "");
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                }
                /* heap primitive var: free the heap ptr and null the alloca */
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
        case NodeConstructorCall: return gen_constructor_call(cg, node);
        case NodeErrPropCall:     return gen_err_prop_call(cg, node);
        case NodeErrProp:         return gen_err_prop(cg, node);
        case NodeAnyTypeExpr:     return gen_any_type_expr(cg, node);
        case NodeSliceExpr:       return gen_slice_expr(cg, node);
        case NodeMakeExpr:        return gen_make_expr(cg, node);
        case NodeAppendExpr:      return gen_append_expr(cg, node);
        case NodeCopyExpr:        return gen_copy_expr(cg, node);
        case NodeLenExpr:         return gen_len_expr(cg, node);
        case NodeCapExpr:         return gen_cap_expr(cg, node);
        case NodeFlaggedIndex:    return gen_flagged_index(cg, node);
        case NodeCmpChain:        return gen_cmp_chain(cg, node);
        case NodeNewInZone:       return gen_new_in_zone(cg, node);
        case NodeZoneMoveExpr:    return gen_zone_move(cg, node);
        case NodeZoneFreeStmt:
            /* zone.free(name) used as expression — emit and return void-like zero */
            {
                const char *zn = node->as.zone_free.name;
                symbol_t *zsym = cg_lookup(cg, zn);
                if (zsym) {
                    LLVMValueRef zptr = LLVMBuildLoad2(cg->builder,
                        LLVMPointerTypeInContext(cg->ctx, 0), zsym->value, "zfp");
                    LLVMValueRef args[1] = { zptr };
                    LLVMBuildCall2(cg->builder, cg->zone_free_type, cg->zone_free_fn, args, 1, "");
                }
            }
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        default:
            diag_begin_error("unexpected node kind %d in expression", node->kind);
            diag_finish();
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}
