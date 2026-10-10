/* Source-level scalar semantics. LLVM integer types deliberately do not encode
 * signedness. Never recover signedness from an LLVMValueRef or its bit width.
 * Context is attached to one AST node, so a surrounding initializer cannot
 * accidentally change a call argument, index, condition, or cast operand. */
#include <stdint.h>
#include <limits.h>
static type_info_t resolve_future_elem_type(cg_t *, node_t *);
static type_info_t scalar_expr_type(cg_t *, node_t *, type_info_t);

static type_info_t scalar_type(type_kind_t k) {
    type_info_t t = NO_TYPE; t.base = k; return t;
}
static boolean_t scalar_integer(type_info_t t) {
    return !t.is_pointer && (is_integer_type(t) || t.base == TypeBool);
}
static boolean_t scalar_numeric(type_info_t t) {
    return scalar_integer(t) || (!t.is_pointer && is_float_type(t));
}
static unsigned scalar_bits(type_info_t t) {
    switch (t.base) {
    case TypeBool: return 1;
    case TypeI8: case TypeU8: return 8;
    case TypeI16: case TypeU16: return 16;
    case TypeI32: case TypeU32: return 32;
    case TypeI64: case TypeU64: return 64;
    default: return 0;
    }
}
static boolean_t scalar_unsigned(type_info_t t) {
    return t.base == TypeBool || is_unsigned_type(t);
}
static uint64_t scalar_mask(unsigned w) {
    return w == 64 ? UINT64_MAX : (UINT64_C(1) << w) - 1;
}
static type_info_t scalar_resolve(cg_t *cg, type_info_t t) {
    for (unsigned i = 0; i < 64; ++i) {
        type_info_t next = resolve_alias(cg, t);
        if (next.base == t.base && next.user_name == t.user_name && next.is_pointer == t.is_pointer) break;
        t = next;
    }
    if (t.base == TypeUser && t.user_name) {
        for (usize_t i = 0; i < cg->generic_n; ++i) {
            if (!strcmp(t.user_name, cg->generic_params[i])) {
                type_info_t r = cg->generic_concs[i];
                if (t.is_pointer) { r.is_pointer = True; r.ptr_depth = t.ptr_depth; r.ptr_perm = t.ptr_perm; }
                return resolve_alias(cg, r);
            }
        }
    }
    if (t.base == TypeUser && t.user_name && !t.is_pointer) {
        enum_reg_t *er = find_enum(cg, t.user_name);
        if (er && !er->is_tagged) return scalar_type(TypeI32);
    }
    return t;
}
static void scalar_error(node_t *node, const char *message) {
    diag_begin_error("%s", message);
    if (node) diag_span(DIAG_NODE(node), True, "scalar expression here");
    diag_finish();
}

/* Slice element identity is a source property: neither LLVM's erased slice
 * descriptor nor scalar_resolve (which lowers ordinary enums to i32) can prove
 * it. Resolve aliases and active generic parameters without erasing nominal
 * types or dropping a concrete generic argument's pointer layers. */
static boolean_t slice_source_resolve(cg_t *cg, type_info_t input,
                                      type_info_t *out, node_t *node) {
    type_info_t t = input;
    usize_t remaining = cg->alias_count + cg->generic_n + 1;
    while (t.base == TypeUser && t.user_name) {
        type_info_t next = t;
        boolean_t rewritten = False;
        for (usize_t i = 0; i < cg->alias_count; ++i) {
            if (!strcmp(t.user_name, cg->aliases[i].name)) {
                next = resolve_alias(cg, t);
                /* resolve_alias composes pointer layers but does not carry
                 * the use-site outer nullable marker onto an alias result. */
                if (t.is_pointer) next.nullable = t.nullable;
                rewritten = True; break;
            }
        }
        if (!rewritten) for (usize_t i = 0; i < cg->generic_n; ++i) {
            if (strcmp(t.user_name, cg->generic_params[i])) continue;
            next = cg->generic_concs[i];
            if (t.is_pointer) {
                int outer = t.ptr_depth > 0 ? t.ptr_depth : 1;
                int inner = next.is_pointer ? (next.ptr_depth > 0 ? next.ptr_depth : 1) : 0;
                if (outer + inner > 8) {
                    scalar_error(node, "slice element pointer composition exceeds the supported depth of 8");
                    return False;
                }
                ptr_perm_t permissions[8] = {0};
                for (int p = 0; p < outer; ++p)
                    permissions[p] = p == 0 ? t.ptr_perm : t.ptr_perms[p];
                for (int p = 0; p < inner; ++p)
                    permissions[outer + p] = p == 0 ? next.ptr_perm : next.ptr_perms[p];
                next.is_pointer = True; next.ptr_depth = outer + inner;
                memcpy(next.ptr_perms, permissions, sizeof(permissions));
                next.ptr_perm = permissions[0]; next.nullable = t.nullable;
            }
            rewritten = True; break;
        }
        if (!rewritten) {
            const char *name = cg_subst_name(cg, t.user_name);
            t.user_name = (char *)name;
            break;
        }
        if (!remaining--) {
            scalar_error(node, "cyclic or unresolved slice element type");
            return False;
        }
        t = next;
    }
    *out = t;
    return True;
}

typedef struct slice_type_path {
    const type_info_t *element;
    const fn_ptr_desc_t *signature;
    const struct slice_type_path *parent;
} slice_type_path_t;

/* Capture concrete nested types while a generic instantiation is active.
 * Detect actual descriptor cycles, rather than accepting an arbitrary depth
 * cutoff as equality. Struct/enum bodies are nominal leaves, not traversed. */
static boolean_t slice_canonical_type(cg_t *cg, type_info_t input,
                                      type_info_t *out, node_t *node,
                                      const slice_type_path_t *path) {
    type_info_t t;
    if (!slice_source_resolve(cg, input, &t, node)) return False;
    if (t.base == TypeUser) {
        if (t.user_name && !find_struct(cg, t.user_name) && !find_enum(cg, t.user_name)) {
            if (strstr(t.user_name, "_G_")) (void)get_llvm_type(cg, t);
        }
        if (!t.user_name || (!find_struct(cg, t.user_name) && !find_enum(cg, t.user_name))) {
            scalar_error(node, "unresolved slice element type"); return False;
        }
    }
    if (t.base == TypeInfer || (t.base == TypeSlice && !t.elem_type)) {
        scalar_error(node, "unresolved slice element type"); return False;
    }
    for (const slice_type_path_t *p = path; p; p = p->parent) {
        if ((t.elem_type && t.elem_type == p->element) ||
            (t.fn_ptr_desc && t.fn_ptr_desc == p->signature)) {
            scalar_error(node, "cyclic slice element type"); return False;
        }
    }
    slice_type_path_t here = {t.elem_type, t.fn_ptr_desc, path};
    if (t.elem_type) {
        type_info_t element;
        if (!slice_canonical_type(cg, t.elem_type[0], &element, node, &here)) return False;
        t.elem_type = alloc_type_array(1); t.elem_type[0] = element;
    }
    if (t.fn_ptr_desc) {
        fn_ptr_desc_t *old = t.fn_ptr_desc, *sig = alloc_fn_ptr_desc(old->param_count);
        sig->is_c_abi = old->is_c_abi; sig->is_variadic = old->is_variadic;
        if (!slice_canonical_type(cg, old->ret_type, &sig->ret_type, node, &here)) return False;
        for (usize_t i = 0; i < old->param_count; ++i) {
            sig->params[i].storage = old->params[i].storage;
            if (!slice_canonical_type(cg, old->params[i].type, &sig->params[i].type, node, &here)) return False;
        }
        t.fn_ptr_desc = sig;
    } else if (t.base == TypeFnPtr || t.base == TypeClosure) {
        scalar_error(node, "unresolved slice element function signature"); return False;
    }
    *out = t; return True;
}

static boolean_t slice_same_type(type_info_t a, type_info_t b) {
    if (a.base != b.base || a.is_pointer != b.is_pointer || a.nullable != b.nullable) return False;
    if (a.is_pointer) {
        int ad = a.ptr_depth > 0 ? a.ptr_depth : 1, bd = b.ptr_depth > 0 ? b.ptr_depth : 1;
        if (ad != bd) return False;
        for (int i = 0; i < ad; ++i)
            if ((i ? a.ptr_perms[i] : a.ptr_perm) != (i ? b.ptr_perms[i] : b.ptr_perm)) return False;
    }
    if (a.base == TypeUser && strcmp(a.user_name, b.user_name)) return False;
    if (!!a.elem_type != !!b.elem_type || !!a.fn_ptr_desc != !!b.fn_ptr_desc) return False;
    if (a.elem_type && !slice_same_type(a.elem_type[0], b.elem_type[0])) return False;
    if (a.fn_ptr_desc) {
        fn_ptr_desc_t *x = a.fn_ptr_desc, *y = b.fn_ptr_desc;
        if (x->param_count != y->param_count || x->is_c_abi != y->is_c_abi ||
            x->is_variadic != y->is_variadic || !slice_same_type(x->ret_type, y->ret_type)) return False;
        for (usize_t i = 0; i < x->param_count; ++i)
            if (x->params[i].storage != y->params[i].storage ||
                !slice_same_type(x->params[i].type, y->params[i].type)) return False;
    }
    return True;
}

static type_info_t scalar_signature_type(cg_t *cg, type_info_t t, node_t *node) {
    t = scalar_resolve(cg, t);
    if (t.base == TypeSlice) {
        type_info_t concrete;
        if (slice_canonical_type(cg, t, &concrete, node, Null)) return concrete;
    }
    return t;
}

/* Array-to-slice views retain the declared element identity, even though enum
 * values participating in scalar arithmetic still lower to their integer tag.
 * Recurse through storage projections without applying that scalar erasure. */
static type_info_t slice_view_source_type(cg_t *cg, node_t *node) {
    if (!node) return NO_TYPE;
    type_info_t t = NO_TYPE;
    if (node->kind == NodeIdentExpr) {
        symbol_t *s = cg_lookup(cg, node->as.ident.name);
        if (s) t = s->stype;
    } else if (node->kind == NodeMemberExpr || node->kind == NodeSelfMemberExpr) {
        const char *field;
        if (node->kind == NodeMemberExpr) {
            t = slice_view_source_type(cg, node->as.member_expr.object);
            field = node->as.member_expr.field;
        } else {
            t.base = TypeUser;
            t.user_name = node->as.self_member.type_name ? node->as.self_member.type_name : cg->current_struct_name;
            field = node->as.self_member.field;
        }
        struct_reg_t *sr = t.user_name ? find_struct(cg, t.user_name) : Null;
        t = NO_TYPE;
        if (sr) for (usize_t i = 0; i < sr->field_count; ++i)
            if (!strcmp(sr->fields[i].name, field)) { t = sr->fields[i].type; break; }
    } else if (node->kind == NodeIndexExpr || node->kind == NodeFlaggedIndex) {
        node_t *object = node->kind == NodeIndexExpr ? node->as.index_expr.object : node->as.flagged_index.object;
        t = slice_view_source_type(cg, object);
        LLVMTypeRef storage = access_storage_type(cg, object);
        if (!storage || LLVMGetTypeKind(storage) != LLVMArrayTypeKind) {
            if (t.base == TypeSlice && !t.is_pointer && t.elem_type) t = t.elem_type[0];
            else if (t.is_pointer) t = ti_deref_one(t);
        }
    } else if (node->kind == NodeUnaryPrefixExpr && node->as.unary.op == TokStar) {
        t = ti_deref_one(slice_view_source_type(cg, node->as.unary.operand));
    } else if (node->kind == NodeCastExpr) t = node->as.cast_expr.target;
    else t = scalar_expr_type(cg, node, NO_TYPE);
    if (!slice_source_resolve(cg, t, &t, node)) return NO_TYPE;
    return t;
}
static boolean_t scalar_literal(node_t *n, uint64_t *magnitude, boolean_t *negative) {
    if (!n) return False;
    *negative = False;
    if (n->kind == NodeUnaryPrefixExpr && n->as.unary.op == TokMinus &&
        n->as.unary.operand && n->as.unary.operand->kind == NodeIntLitExpr) {
        *negative = True; n = n->as.unary.operand;
    }
    if (n->kind != NodeIntLitExpr) return False;
    *magnitude = n->as.int_lit.value; return True;
}
static boolean_t scalar_literal_tree(node_t *n) {
    if (!n) return False;
    switch (n->kind) {
    case NodeIntLitExpr: case NodeCharLitExpr: return True;
    case NodeUnaryPrefixExpr:
        return (n->as.unary.op == TokMinus || n->as.unary.op == TokTilde || n->as.unary.op == TokPlus)
            && scalar_literal_tree(n->as.unary.operand);
    case NodeBinaryExpr:
        switch (n->as.binary.op) {
        case TokPlus: case TokMinus: case TokStar: case TokSlash: case TokPercent:
        case TokPlusPercent: case TokMinusPercent: case TokStarPercent:
        case TokPlusBang: case TokMinusBang: case TokStarBang:
        case TokAmp: case TokPipe: case TokCaret: case TokLtLt: case TokGtGt:
            return scalar_literal_tree(n->as.binary.left) && scalar_literal_tree(n->as.binary.right);
        default: return False;
        }
    default: return False;
    }
}
static type_info_t scalar_literal_type(node_t *n, type_info_t expected) {
    uint64_t m; boolean_t neg;
    if (!scalar_literal(n, &m, &neg)) return NO_TYPE;
    if (scalar_integer(expected) && expected.base != TypeBool) return expected;
    if (neg) return scalar_type(m <= UINT64_C(2147483648) ? TypeI32 : TypeI64);
    return scalar_type(m <= INT32_MAX ? TypeI32 : m <= INT64_MAX ? TypeI64 : TypeU64);
}
static boolean_t scalar_literal_fits(node_t *n, type_info_t t) {
    uint64_t m; boolean_t neg;
    if (!scalar_literal(n, &m, &neg)) return False;
    unsigned w = scalar_bits(t);
    if (!w) return False;
    if (scalar_unsigned(t)) return (!neg || !m) && m <= scalar_mask(w);
    uint64_t bound = UINT64_C(1) << (w - 1);
    return m <= (neg ? bound : bound - 1);
}
static symbol_t *scalar_symbol(cg_t *cg, const char *name) {
    return cg_lookup(cg, name);
}
static void scalar_register_signature(cg_t *cg, symbol_t *s, node_t *decl) {
    if (!strcmp(s->name, "__stasha_panic")) {
        boolean_t valid = decl->as.fn_decl.return_count == 1 && decl->as.fn_decl.params.count == 1 && !decl->as.fn_decl.is_variadic;
        if (valid) {
            type_info_t rt = scalar_resolve(cg, decl->as.fn_decl.return_types[0]);
            type_info_t pt = scalar_resolve(cg, decl->as.fn_decl.params.items[0]->as.var_decl.type);
            valid = rt.base == TypeVoid && !rt.is_pointer && pt.base == TypeU32 && !pt.is_pointer;
        }
        if (!valid) scalar_error(decl, "__stasha_panic must have ABI void(u32) and must not return");
        if (decl->as.fn_decl.body)
            scalar_error(decl, "__stasha_panic requires a reviewed nonreturning C or assembly implementation; source definitions are not supported");
    }
    usize_t pc = decl->as.fn_decl.params.count;
    fn_ptr_desc_t *sig = alloc_fn_ptr_desc(pc);
    for (usize_t i = 0; i < pc; ++i) {
        sig->params[i].type = scalar_signature_type(cg, decl->as.fn_decl.params.items[i]->as.var_decl.type, decl);
        sig->params[i].storage = decl->as.fn_decl.params.items[i]->as.var_decl.storage;
    }
    sig->ret_type = decl->as.fn_decl.return_count == 1
        ? scalar_signature_type(cg, decl->as.fn_decl.return_types[0], decl) : NO_TYPE;
    sig->is_c_abi = s->c_abi != Null;
    sig->is_variadic = decl->as.fn_decl.is_variadic;
    s->scalar_signature = sig;
}
static symbol_t *scalar_call_symbol(cg_t *cg, node_t *n) {
    char name[512];
    if (n->kind == NodeColonCall) {
        lib_entry_t *lib = find_lib_entry(cg, n->as.colon_call.module_name);
        if (!lib || !lib->mod_prefix) return Null;
        if (n->as.colon_call.type_name) snprintf(name, sizeof(name), "%s__%s__%s", lib->mod_prefix, n->as.colon_call.type_name, n->as.colon_call.method_name);
        else snprintf(name, sizeof(name), "%s__%s", lib->mod_prefix, n->as.colon_call.method_name);
        return cg_lookup_symbol(cg, name);
    }
    if (n->kind == NodeErrPropCall) return scalar_symbol(cg, n->as.err_prop_call.callee);
    if (n->kind == NodeConstructorCall) {
        if (strstr(n->as.ctor_call.type_name, "_G_"))
            try_instantiate_generic(cg, n->as.ctor_call.type_name);
        struct_reg_t *sr = find_struct(cg, n->as.ctor_call.type_name);
        if (sr && sr->mod_prefix && sr->mod_prefix[0]) snprintf(name, sizeof(name), "%s__%s__new", sr->mod_prefix, sr->name);
        else snprintf(name, sizeof(name), "%s.new", n->as.ctor_call.type_name);
        return cg_lookup_symbol(cg, name);
    }
    if (n->kind == NodeCallExpr) {
        symbol_t *s = scalar_symbol(cg, n->as.call.callee);
        if (!s && strstr(n->as.call.callee, "_G_"))
            s = try_instantiate_generic_fn(cg, n->as.call.callee);
        if (s) return s;
        if (cg->current_struct_name) {
            if (cg->current_module_prefix[0]) snprintf(name, sizeof(name), "%s__%s__%s", cg->current_module_prefix, cg->current_struct_name, n->as.call.callee);
            else snprintf(name, sizeof(name), "%s.%s", cg->current_struct_name, n->as.call.callee);
            return cg_lookup_symbol(cg, name);
        }
    }
    const char *tn = Null, *method = Null;
    if (n->kind == NodeMethodCall && n->as.method_call.object->kind == NodeIdentExpr) {
        const char *obj = n->as.method_call.object->as.ident.name;
        method = n->as.method_call.method;
        lib_entry_t *lib = find_lib_entry(cg, obj);
        if (lib && lib->mod_prefix) { snprintf(name, sizeof(name), "%s__%s", lib->mod_prefix, method); return cg_lookup_symbol(cg, name); }
        if (lib) return cg_lookup_symbol(cg, method);
        symbol_t *s = scalar_symbol(cg, obj); tn = s ? s->stype.user_name : obj;
    } else if (n->kind == NodeMethodCall) {
        type_info_t receiver = scalar_resolve(cg, scalar_expr_type(cg, n->as.method_call.object, NO_TYPE));
        tn = receiver.base == TypeUser ? receiver.user_name : Null;
        method = n->as.method_call.method;
    } else if (n->kind == NodeSelfMethodCall) {
        tn = n->as.self_method_call.type_name ? n->as.self_method_call.type_name : cg->current_struct_name;
        method = n->as.self_method_call.method;
    }
    if (tn && method) {
        /* Resolve the source signature before gen_scalar_as records the result
         * type. Instantiating only later during call emission loses signedness
         * on the first use of a generic static method. */
        if (strstr(tn, "_G_")) try_instantiate_generic(cg, tn);
        struct_reg_t *sr = find_struct(cg, tn);
        if (sr && sr->mod_prefix && sr->mod_prefix[0]) snprintf(name, sizeof(name), "%s__%s__%s", sr->mod_prefix, tn, method);
        else snprintf(name, sizeof(name), "%s.%s", tn, method);
        return cg_lookup_symbol(cg, name);
    }
    return Null;
}
/* Resolve declarations by emitted symbol, including module and method names. */
static node_t *scalar_fn_named(cg_t *cg, const char *name) {
    if (!cg->ast || !name) return Null;
    node_list_t ds = cg->ast->as.module.decls;
    for (usize_t i = 0; i < ds.count; ++i) {
        node_t *d = ds.items[i];
        node_list_t ms = {0};
        if (d->kind == NodeTypeDecl) ms = d->as.type_decl.methods;
        usize_t count = d->kind == NodeFnDecl ? 1 : ms.count;
        for (usize_t j = 0; j < count; ++j) {
            node_t *f = d->kind == NodeFnDecl ? d : ms.items[j];
            if (f->kind != NodeFnDecl) continue;
            char buf[512];
            const char *mod = f->is_c_extern ? Null : f->module_name;
            if (d->kind == NodeTypeDecl)
                mangle_method(d->module_name, d->as.type_decl.name, f->as.fn_decl.name, buf, sizeof(buf));
            else if (f->as.fn_decl.is_method)
                mangle_method(mod, f->as.fn_decl.struct_name, f->as.fn_decl.name, buf, sizeof(buf));
            else mangle_fn(mod, f->as.fn_decl.name, buf, sizeof(buf));
            if (!strcmp(buf, name)) return f;
        }
    }
    return Null;
}
static node_t *scalar_call_decl(cg_t *cg, node_t *n) {
    char buf[512]; const char *name = Null;
    if (n->kind == NodeCallExpr) {
        symbol_t *s = scalar_symbol(cg, n->as.call.callee);
        if (s) { node_t *d = scalar_fn_named(cg, s->name); if (d) return d; }
        if (cg->current_struct_name) {
            snprintf(buf, sizeof(buf), "%s.%s", cg->current_struct_name, n->as.call.callee);
            node_t *d = scalar_fn_named(cg, buf); if (d) return d;
        }
        name = n->as.call.callee;
    } else if (n->kind == NodeMethodCall) {
        node_t *obj = n->as.method_call.object;
        if (obj->kind == NodeIdentExpr) {
            lib_entry_t *lib = find_lib_entry(cg, obj->as.ident.name);
            if (lib && lib->mod_prefix) {
                snprintf(buf, sizeof(buf), "%s__%s", lib->mod_prefix, n->as.method_call.method);
                return scalar_fn_named(cg, buf);
            }
            symbol_t *s = scalar_symbol(cg, obj->as.ident.name);
            const char *tn = s ? s->stype.user_name : obj->as.ident.name;
            if (tn && strstr(tn, "_G_")) try_instantiate_generic(cg, tn);
            struct_reg_t *sr = tn ? find_struct(cg, tn) : Null;
            if (sr) {
                if (sr->mod_prefix && sr->mod_prefix[0]) snprintf(buf, sizeof(buf), "%s__%s__%s", sr->mod_prefix, sr->name, n->as.method_call.method);
                else snprintf(buf, sizeof(buf), "%s.%s", sr->name, n->as.method_call.method);
                return scalar_fn_named(cg, buf);
            }
        }
    } else if (n->kind == NodeSelfMethodCall) {
        const char *tn = n->as.self_method_call.type_name;
        if (!tn) tn = cg->current_struct_name;
        if (tn) { snprintf(buf, sizeof(buf), "%s.%s", tn, n->as.self_method_call.method); return scalar_fn_named(cg, buf); }
    }
    return name ? scalar_fn_named(cg, name) : Null;
}
static type_info_t scalar_expr_type(cg_t *cg, node_t *n, type_info_t expected);
static type_info_t scalar_common(cg_t *cg, node_t *a, node_t *b, type_info_t expected) {
    if (expected.base == TypeBool) expected = NO_TYPE;
    boolean_t al = scalar_literal_tree(a), bl = scalar_literal_tree(b);
    type_info_t at = scalar_expr_type(cg, a, al && bl ? expected : NO_TYPE);
    type_info_t bt = scalar_expr_type(cg, b, al && bl ? expected : NO_TYPE);
    if (al && !bl && scalar_integer(bt)) return bt;
    if (bl && !al && scalar_integer(at)) return at;
    if (scalar_integer(at) && scalar_integer(bt)) {
        if (scalar_unsigned(at) != scalar_unsigned(bt)) {
            uint64_t am, bm; boolean_t an, bn;
            if (al && bl && scalar_literal(a, &am, &an) && scalar_literal(b, &bm, &bn) && !an && !bn)
                return scalar_type(TypeU64);
            return NO_TYPE;
        }
        return scalar_bits(at) >= scalar_bits(bt) ? at : bt;
    }
    if (scalar_numeric(at) && scalar_numeric(bt) && (is_float_type(at) || is_float_type(bt)))
        return scalar_type(at.base == TypeF64 || bt.base == TypeF64 ? TypeF64 : TypeF32);
    if (at.is_pointer) return at;
    return at;
}
static type_info_t scalar_expr_type(cg_t *cg, node_t *n, type_info_t expected) {
    if (!n) return NO_TYPE;
    expected = scalar_resolve(cg, expected);
    uint64_t m; boolean_t neg;
    if (scalar_literal(n, &m, &neg)) return scalar_literal_type(n, expected);
    switch (n->kind) {
    case NodeBoolLitExpr: return scalar_type(TypeBool);
    case NodeCharLitExpr: return scalar_integer(expected) && expected.base != TypeBool ? expected : scalar_type(TypeU8);
    case NodeFloatLitExpr: return scalar_type(TypeF64);
    case NodeStrLitExpr: {
        type_info_t t = scalar_type(TypeI8);
        t.is_pointer = True; t.ptr_depth = 1;
        t.ptr_perm = PtrRead; t.ptr_perms[0] = PtrRead;
        return t;
    }
    case NodeIdentExpr: {
        symbol_t *s = scalar_symbol(cg, n->as.ident.name);
        if (s && s->scalar_signature && LLVMIsAFunction(s->value)) {
            type_info_t fn = scalar_type(TypeFnPtr); fn.fn_ptr_desc = s->scalar_signature; return fn;
        }
        return s ? scalar_resolve(cg, s->stype) : NO_TYPE;
    }
    case NodeAddrOf: {
        node_t *operand = n->as.addr_of.operand;
        if (operand->kind == NodeIdentExpr) {
            symbol_t *s = scalar_symbol(cg, operand->as.ident.name);
            if (s && s->scalar_signature && LLVMIsAFunction(s->value)) {
                type_info_t fn = scalar_type(TypeFnPtr); fn.fn_ptr_desc = s->scalar_signature; return fn;
            }
        }
        type_info_t t = scalar_resolve(cg, scalar_expr_type(cg, operand, NO_TYPE));
        if (t.base == TypeVoid && !t.is_pointer) return NO_TYPE;
        int depth = t.is_pointer ? (t.ptr_depth > 0 ? t.ptr_depth : 1) : 0;
        if (depth >= 8) return NO_TYPE;
        if (depth == 1) t.ptr_perms[0] = t.ptr_perm;
        for (int i = depth; i > 0; --i) t.ptr_perms[i] = t.ptr_perms[i - 1];
        t.is_pointer = True; t.ptr_depth = depth + 1; t.nullable = False;
        t.ptr_perm = access_address_permissions(cg, operand); t.ptr_perms[0] = t.ptr_perm;
        return t;
    }
    case NodeCastExpr: return scalar_resolve(cg, n->as.cast_expr.target);
    case NodeUnaryPrefixExpr: case NodeUnaryPostfixExpr:
        if (n->as.unary.op == TokBang) return scalar_type(TypeBool);
        if (n->as.unary.op == TokStar) return ti_deref_one(scalar_expr_type(cg, n->as.unary.operand, NO_TYPE));
        return scalar_expr_type(cg, n->as.unary.operand, expected);
    case NodeBinaryExpr:
        switch (n->as.binary.op) {
        case TokEqEq: case TokBangEq: case TokLt: case TokGt: case TokLtEq: case TokGtEq:
        case TokAmpAmp: case TokPipePipe: return scalar_type(TypeBool);
        case TokLtLt: case TokGtGt: return scalar_expr_type(cg, n->as.binary.left, expected);
        default: return scalar_common(cg, n->as.binary.left, n->as.binary.right, expected);
        }
    case NodeCmpChain: return scalar_type(TypeBool);
    case NodeTernaryExpr: return scalar_common(cg, n->as.ternary.then_expr, n->as.ternary.else_expr, expected);
    case NodeAssignExpr: return scalar_expr_type(cg, n->as.assign.target, NO_TYPE);
    case NodeCompoundAssign: return scalar_expr_type(cg, n->as.compound_assign.target, NO_TYPE);
    case NodeIndexExpr: case NodeFlaggedIndex: {
        node_t *object = n->kind == NodeIndexExpr ? n->as.index_expr.object : n->as.flagged_index.object;
        type_info_t t = scalar_resolve(cg, scalar_expr_type(cg, object, NO_TYPE));
        LLVMTypeRef storage = access_storage_type(cg, object);
        if (storage && LLVMGetTypeKind(storage) == LLVMArrayTypeKind) return t;
        if (t.base == TypeSlice && !t.is_pointer && t.elem_type) return scalar_resolve(cg, t.elem_type[0]);
        return t.is_pointer ? ti_deref_one(t) : t;
    }
    case NodeSliceExpr: {
        type_info_t source = slice_view_source_type(cg, n->as.slice_expr.object);
        LLVMTypeRef storage = access_storage_type(cg, n->as.slice_expr.object);
        if (source.base == TypeSlice && !source.is_pointer &&
            (!storage || LLVMGetTypeKind(storage) != LLVMArrayTypeKind)) return source;
        type_info_t t = scalar_type(TypeSlice);
        t.elem_type = alloc_type_array(1);
        t.elem_type[0] = source.is_pointer && (!storage || LLVMGetTypeKind(storage) != LLVMArrayTypeKind)
            ? ti_deref_one(source) : source;
        return t;
    }
    case NodeMakeExpr: {
        if (n->as.make_expr.init) return NO_TYPE; /* context-typed make.{...} is separate */
        type_info_t t = scalar_type(TypeSlice), element;
        if (!slice_source_resolve(cg, n->as.make_expr.elem_type, &element, n)) return NO_TYPE;
        t.elem_type = alloc_type_array(1); t.elem_type[0] = element;
        return t;
    }
    case NodeMemberExpr: case NodeSelfMemberExpr: {
        type_info_t t = NO_TYPE; const char *field;
        if (n->kind == NodeMemberExpr) { t = scalar_expr_type(cg, n->as.member_expr.object, NO_TYPE); field = n->as.member_expr.field; }
        else { t.base = TypeUser; t.user_name = n->as.self_member.type_name ? n->as.self_member.type_name : cg->current_struct_name; field = n->as.self_member.field; }
        struct_reg_t *sr = t.user_name ? find_struct(cg, t.user_name) : Null;
        if (sr) for (usize_t i = 0; i < sr->field_count; ++i)
            if (!strcmp(sr->fields[i].name, field)) return scalar_resolve(cg, sr->fields[i].type);
        if (t.base == TypeSlice && (!strcmp(field, "len") || !strcmp(field, "cap"))) return scalar_type(TypeI32);
        return NO_TYPE;
    }
    case NodeCallExpr: case NodeMethodCall: case NodeSelfMethodCall: case NodeColonCall: case NodeConstructorCall: {
        if (n->kind == NodeCallExpr) {
            symbol_t *s = scalar_symbol(cg, n->as.call.callee);
            if (s && s->stype.fn_ptr_desc) return scalar_resolve(cg, s->stype.fn_ptr_desc->ret_type);
        }
        symbol_t *fs = scalar_call_symbol(cg, n);
        if (fs && fs->scalar_signature) return fs->scalar_signature->ret_type;
        node_t *f = scalar_call_decl(cg, n);
        return f && f->as.fn_decl.return_count == 1 ? scalar_resolve(cg, f->as.fn_decl.return_types[0]) : NO_TYPE;
    }
    case NodeLambdaCall: return scalar_resolve(cg, n->as.lambda_call.lambda->as.lambda_expr.ret_type);
    case NodeSizeofExpr: return scalar_type(TypeU64);
    case NodeVaOp: return scalar_resolve(cg, n->as.va_op.next_type);
    case NodeFutureOp:
        if (n->as.future_op.op == FutureReady || n->as.future_op.op == StreamDone) return scalar_type(TypeBool);
        if (n->as.future_op.op == FutureGet || n->as.future_op.op == StreamFinal) return scalar_resolve(cg, n->as.future_op.get_type);
        return scalar_type(TypeI32);
    case NodeAwaitExpr:
        return n->as.await_expr.get_type.base != TypeVoid ? scalar_resolve(cg, n->as.await_expr.get_type)
            : scalar_resolve(cg, resolve_future_elem_type(cg, n->as.await_expr.handle));
    case NodeLenExpr: case NodeCapExpr: case NodeCopyExpr: case NodeAnyTypeExpr:
    case NodeErrPropCall: case NodeErrProp: case NodeExpectExpr: case NodeExpectEqExpr:
    case NodeExpectNeqExpr: case NodeTestFailExpr: return scalar_type(TypeI32);
    case NodeHashExpr: return scalar_type(TypeU64);
    case NodeEquExpr: return scalar_type(TypeBool);
    default: return NO_TYPE;
    }
}
static type_info_t scalar_expected(cg_t *cg, node_t *n) {
    return cg->scalar_context_node == n ? cg->scalar_context_type : NO_TYPE;
}
static LLVMValueRef scalar_emit_literal(cg_t *cg, node_t *n, type_info_t t) {
    uint64_t m; boolean_t neg; scalar_literal(n, &m, &neg);
    if (!scalar_literal_fits(n, t)) {
        scalar_error(n, "integer literal is outside the range of its contextual type");
        m = 0;
    }
    /* LLVM23 no longer implicitly truncates unsigned ConstantInt inputs.
     * Canonicalize before construction; the IR printer hides unused high bits. */
    uint64_t bits = (neg ? UINT64_C(0) - m : m) & scalar_mask(scalar_bits(t));
    return LLVMConstInt(get_llvm_type(cg, t), bits, 0);
}
/* Stable freestanding failure ABI. The hook must not return. Hosted mode
 * supplies an overridable, non-I/O trap; freestanding mode deliberately leaves
 * a required undefined symbol for the kernel/harness to provide. */
static LLVMValueRef scalar_panic_fn(cg_t *cg, LLVMTypeRef *out_type) {
    LLVMTypeRef param = LLVMInt32TypeInContext(cg->ctx);
    LLVMTypeRef ft = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), &param, 1, 0);
    node_t *decl = scalar_fn_named(cg, "__stasha_panic");
    if (decl) {
        boolean_t valid = decl->as.fn_decl.return_count == 1 && decl->as.fn_decl.params.count == 1 && !decl->as.fn_decl.is_variadic;
        if (valid) {
            type_info_t rt = scalar_resolve(cg, decl->as.fn_decl.return_types[0]);
            type_info_t pt = scalar_resolve(cg, decl->as.fn_decl.params.items[0]->as.var_decl.type);
            valid = rt.base == TypeVoid && !rt.is_pointer && pt.base == TypeU32 && !pt.is_pointer;
        }
        if (!valid) { scalar_error(decl, "__stasha_panic must have ABI void(u32) and must not return"); *out_type = ft; return Null; }
    }
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, "__stasha_panic");
    if (f && LLVMGlobalGetValueType(f) != ft) {
        scalar_error(Null, "__stasha_panic must have ABI void(u32) and must not return");
        *out_type = ft; return Null;
    }
    boolean_t fresh = !f;
    if (!f) f = LLVMAddFunction(cg->module, "__stasha_panic", ft);
    const char *attrs[] = { "noreturn", "cold", "noinline" };
    for (unsigned i = 0; i < 3; ++i) {
        unsigned kind = LLVMGetEnumAttributeKindForName(attrs[i], strlen(attrs[i]));
        LLVMAddAttributeAtIndex(f, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(cg->ctx, kind, 0));
    }
    if (fresh && !cg->freestanding && !scalar_fn_named(cg, "__stasha_panic")) {
        LLVMSetLinkage(f, LLVMWeakAnyLinkage);
        LLVMBasicBlockRef saved = LLVMGetInsertBlock(cg->builder);
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, f, "entry");
        LLVMPositionBuilderAtEnd(cg->builder, entry);
        LLVMTypeRef trap_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), Null, 0, 0);
        LLVMValueRef trap = LLVMGetNamedFunction(cg->module, "llvm.trap");
        if (!trap) trap = LLVMAddFunction(cg->module, "llvm.trap", trap_type);
        LLVMBuildCall2(cg->builder, trap_type, trap, Null, 0, "");
        LLVMBuildUnreachable(cg->builder);
        LLVMPositionBuilderAtEnd(cg->builder, saved);
    }
    *out_type = ft; return f;
}
static void scalar_trap_if(cg_t *cg, LLVMValueRef bad, unsigned reason) {
    LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "scalar.panic");
    LLVMBasicBlockRef ok = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "scalar.ok");
    LLVMBuildCondBr(cg->builder, bad, fail, ok);
    LLVMPositionBuilderAtEnd(cg->builder, fail);
    LLVMTypeRef ft;
    LLVMValueRef panic = scalar_panic_fn(cg, &ft);
    LLVMValueRef arg = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), reason, 0);
    if (panic) LLVMBuildCall2(cg->builder, ft, panic, &arg, 1, "");
    LLVMBuildUnreachable(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder, ok);
}

/* Implemented by the source constant evaluator, not LLVM constant folding. */
static boolean_t scalar_const_conversion_fits(cg_t *, node_t *, type_info_t, type_info_t);

/* A pointer slot that remains writable cannot change its stored pointer
 * qualifiers: storing a narrowed value there would change the capabilities
 * observed through the original alias. Read-only paths permit narrowing.
 * Arithmetic permission behavior is unchanged by this read/write check. */
static boolean_t scalar_check_pointer_permissions(cg_t *cg, type_info_t from,
                                                  type_info_t to, node_t *n) {
    from = scalar_resolve(cg, from); to = scalar_resolve(cg, to);
    if (cg->in_unsafe > 0 || !from.is_pointer || !to.is_pointer) return True;
    int from_depth = from.ptr_depth > 0 ? from.ptr_depth : 1;
    int to_depth = to.ptr_depth > 0 ? to.ptr_depth : 1;
    boolean_t writable_slot = False;
    for (int level = 0; level < from_depth && level < to_depth && level < 8; ++level) {
        ptr_perm_t have = level == 0 ? from.ptr_perm : from.ptr_perms[level];
        ptr_perm_t need = level == 0 ? to.ptr_perm : to.ptr_perms[level];
        if (need & ~have & PtrReadWrite) {
            scalar_error(n, "cannot widen pointer permissions outside unsafe");
            return False;
        }
        if (writable_slot && ((have ^ need) & PtrReadWrite)) {
            scalar_error(n, "cannot change nested pointer permissions through a writable pointer slot outside unsafe");
            return False;
        }
        writable_slot = writable_slot || (need & PtrWrite);
    }
    return True;
}

static LLVMValueRef scalar_coerce(cg_t *cg, LLVMValueRef v, type_info_t from,
                                  type_info_t to, node_t *n, boolean_t explicit_cast) {
    from = scalar_resolve(cg, from); to = scalar_resolve(cg, to);
    cg_abi_check_fnptr_conversion(cg, from, to, n);
    LLVMTypeRef dst = get_llvm_type(cg, to), src = LLVMTypeOf(v);
    if (!explicit_cast && from.base == TypeSlice && !from.is_pointer &&
        to.base == TypeSlice && !to.is_pointer) {
        type_info_t a, b;
        if (!slice_canonical_type(cg, from, &a, n, Null) ||
            !slice_canonical_type(cg, to, &b, n, Null)) return LLVMConstNull(dst);
        if (!slice_same_type(a, b)) {
            scalar_error(n, "implicit slice conversion requires identical element types");
            return LLVMConstNull(dst);
        }
    }
    if (!scalar_check_pointer_permissions(cg, from, to, n)) return LLVMConstNull(dst);
    /* Source typing is incomplete for some address-producing expressions.
     * Checking the emitted pointer shape too closes the legacy NO_TYPE path
     * without inferring integer signedness from LLVM's signless types. */
    if (!explicit_cast && scalar_integer(to) &&
        (from.is_pointer || from.base == TypeFnPtr || LLVMGetTypeKind(src) == LLVMPointerTypeKind)) {
        scalar_error(n, "implicit pointer-to-integer conversion requires an explicit cast");
        return LLVMConstNull(dst);
    }
    if (scalar_integer(from) && scalar_integer(to) && LLVMGetTypeKind(src) == LLVMIntegerTypeKind) {
        unsigned sw = LLVMGetIntTypeWidth(src), tw = scalar_bits(to);
        if (!explicit_cast && to.base != TypeBool &&
            (tw < sw || (scalar_unsigned(from) != scalar_unsigned(to) && !(scalar_unsigned(from) && tw > sw))) &&
            !scalar_const_conversion_fits(cg, n, from, to))
            scalar_error(n, "implicit integer conversion may change value; use an explicit cast");
        if (to.base == TypeBool)
            return LLVMBuildICmp(cg->builder, LLVMIntNE, v, LLVMConstNull(src), "scalar.bool");
        if (tw > sw) return scalar_unsigned(from) ? LLVMBuildZExt(cg->builder, v, dst, "scalar.zext") : LLVMBuildSExt(cg->builder, v, dst, "scalar.sext");
        if (tw < sw) return LLVMBuildTrunc(cg->builder, v, dst, "scalar.trunc");
        return v;
    }
    if (scalar_integer(from) && LLVMGetTypeKind(src) == LLVMIntegerTypeKind && is_float_type(to) && !to.is_pointer)
        return scalar_unsigned(from) ? LLVMBuildUIToFP(cg->builder, v, dst, "scalar.uitofp") : LLVMBuildSIToFP(cg->builder, v, dst, "scalar.sitofp");
    if (is_float_type(from) && !from.is_pointer && scalar_integer(to)) {
        if (to.base == TypeBool)
            return LLVMBuildFCmp(cg->builder, LLVMRealUNE, v, LLVMConstReal(src, 0.0), "scalar.fp.bool");
        /* fptosi/fptoui poison for NaN/out-of-range. Check in the source FP
         * type, with inclusive lower and exclusive power-of-two upper bound. */
        unsigned w = scalar_bits(to);
        double limit = 1.0;
        for (unsigned i = 0; i < (scalar_unsigned(to) ? w : w - 1); ++i) limit *= 2.0;
        double low = scalar_unsigned(to) ? 0.0 : -limit;
        LLVMValueRef lo = LLVMBuildFCmp(cg->builder, LLVMRealOGE, v, LLVMConstReal(src, low), "scalar.fp.low");
        LLVMValueRef hi = LLVMBuildFCmp(cg->builder, LLVMRealOLT, v, LLVMConstReal(src, limit), "scalar.fp.high");
        scalar_trap_if(cg, LLVMBuildNot(cg->builder, LLVMBuildAnd(cg->builder, lo, hi, "scalar.fp.valid"), "scalar.fp.bad"), 5);
        return scalar_unsigned(to) ? LLVMBuildFPToUI(cg->builder, v, dst, "scalar.fptoui") : LLVMBuildFPToSI(cg->builder, v, dst, "scalar.fptosi");
    }
    return coerce_int(cg, v, dst);
}
static LLVMValueRef gen_scalar_as(cg_t *cg, node_t *n, type_info_t to) {
    to = scalar_resolve(cg, to);
    node_t *saved_node = cg->scalar_context_node;
    type_info_t saved_type = cg->scalar_context_type;
    type_info_t saved_element = cg->scalar_aggregate_element;
    if (n->kind == NodeCompoundInit) cg->scalar_aggregate_element = to;
    else if (n->kind == NodeMakeExpr && to.base == TypeSlice && to.elem_type)
        cg->scalar_aggregate_element = to.elem_type[0];
    cg->scalar_context_node = n; cg->scalar_context_type = to;
    type_info_t from = scalar_expr_type(cg, n, to);
    LLVMValueRef v = gen_expr(cg, n);
    cg->scalar_context_node = saved_node; cg->scalar_context_type = saved_type;
    cg->scalar_aggregate_element = saved_element;
    if (to.base == TypeVoid && !to.is_pointer) return v;
    return scalar_coerce(cg, v, from, to, n, False);
}
static LLVMValueRef scalar_integer_op(cg_t *cg, token_kind_t op, LLVMValueRef a,
                                      LLVMValueRef b, type_info_t t) {
    LLVMTypeRef ty = LLVMTypeOf(a); unsigned w = LLVMGetIntTypeWidth(ty);
    boolean_t u = scalar_unsigned(t);
    switch (op) {
    case TokPlus: case TokPlusPercent: return LLVMBuildAdd(cg->builder, a, b, "add");
    case TokMinus: case TokMinusPercent: return LLVMBuildSub(cg->builder, a, b, "sub");
    case TokStar: case TokStarPercent: return LLVMBuildMul(cg->builder, a, b, "mul");
    case TokPlusBang: case TokMinusBang: case TokStarBang: {
        char name[80]; snprintf(name, sizeof(name), "llvm.%c%s.with.overflow.i%u", u ? 'u' : 's', op == TokPlusBang ? "add" : op == TokMinusBang ? "sub" : "mul", w);
        LLVMTypeRef fs[2] = { ty, LLVMInt1TypeInContext(cg->ctx) }, ps[2] = { ty, ty };
        LLVMTypeRef ft = LLVMFunctionType(LLVMStructTypeInContext(cg->ctx, fs, 2, 0), ps, 2, 0);
        LLVMValueRef f = LLVMGetNamedFunction(cg->module, name);
        if (!f) f = LLVMAddFunction(cg->module, name, ft);
        LLVMValueRef args[2] = { a, b }, pair = LLVMBuildCall2(cg->builder, ft, f, args, 2, "checked");
        LLVMValueRef value = LLVMBuildExtractValue(cg->builder, pair, 0, "checked.value");
        scalar_trap_if(cg, LLVMBuildExtractValue(cg->builder, pair, 1, "checked.overflow"), 1);
        return value;
    }
    case TokSlash: case TokPercent: {
        LLVMValueRef bad = LLVMBuildICmp(cg->builder, LLVMIntEQ, b, LLVMConstNull(ty), "scalar.div.zero");
        scalar_trap_if(cg, bad, 2);
        if (!u) {
            LLVMValueRef min = LLVMConstInt(ty, UINT64_C(1) << (w - 1), 0);
            LLVMValueRef amin = LLVMBuildICmp(cg->builder, LLVMIntEQ, a, min, "scalar.div.min");
            LLVMValueRef bneg = LLVMBuildICmp(cg->builder, LLVMIntEQ, b, LLVMConstAllOnes(ty), "scalar.div.negone");
            scalar_trap_if(cg, LLVMBuildAnd(cg->builder, amin, bneg, "scalar.div.overflow"), 3);
        }
        if (op == TokSlash) return u ? LLVMBuildUDiv(cg->builder, a, b, "udiv") : LLVMBuildSDiv(cg->builder, a, b, "sdiv");
        return u ? LLVMBuildURem(cg->builder, a, b, "urem") : LLVMBuildSRem(cg->builder, a, b, "srem");
    }
    case TokLtLt: case TokGtGt: {
        /* Caller checks original count before narrowing it to the LHS width. */
        if (op == TokLtLt) return LLVMBuildShl(cg->builder, a, b, "shl");
        return u ? LLVMBuildLShr(cg->builder, a, b, "lshr") : LLVMBuildAShr(cg->builder, a, b, "ashr");
    }
    case TokLt: return LLVMBuildICmp(cg->builder, u ? LLVMIntULT : LLVMIntSLT, a, b, "lt");
    case TokGt: return LLVMBuildICmp(cg->builder, u ? LLVMIntUGT : LLVMIntSGT, a, b, "gt");
    case TokLtEq: return LLVMBuildICmp(cg->builder, u ? LLVMIntULE : LLVMIntSLE, a, b, "le");
    case TokGtEq: return LLVMBuildICmp(cg->builder, u ? LLVMIntUGE : LLVMIntSGE, a, b, "ge");
    case TokEqEq: return LLVMBuildICmp(cg->builder, LLVMIntEQ, a, b, "eq");
    case TokBangEq: return LLVMBuildICmp(cg->builder, LLVMIntNE, a, b, "ne");
    case TokAmp: return LLVMBuildAnd(cg->builder, a, b, "and");
    case TokPipe: return LLVMBuildOr(cg->builder, a, b, "or");
    case TokCaret: return LLVMBuildXor(cg->builder, a, b, "xor");
    default: return a;
    }
}
static LLVMValueRef scalar_shift_count(cg_t *cg, node_t *n, type_info_t left) {
    LLVMValueRef v = gen_expr(cg, n);
    type_info_t t = scalar_expr_type(cg, n, NO_TYPE);
    if (!scalar_integer(t)) { scalar_error(n, "shift count must be an integer"); return LLVMConstNull(get_llvm_type(cg, left)); }
    LLVMTypeRef ty = LLVMInt64TypeInContext(cg->ctx);
    LLVMValueRef wide = scalar_coerce(cg, v, t, scalar_type(TypeU64), n, True);
    LLVMValueRef bad = LLVMBuildICmp(cg->builder, LLVMIntUGE, wide, LLVMConstInt(ty, scalar_bits(left), 0), "scalar.shift.bad");
    scalar_trap_if(cg, bad, 4);
    return scalar_coerce(cg, v, t, left, n, True);
}

static type_info_t scalar_param_type(cg_t *cg, node_t *call, usize_t index) {
    if (call->kind == NodeLambdaCall) {
        node_t *lam = call->as.lambda_call.lambda;
        if (index < lam->as.lambda_expr.params.count)
            return scalar_resolve(cg, lam->as.lambda_expr.params.items[index]->as.var_decl.type);
    }
    if (call->kind == NodeCallExpr) {
        symbol_t *s = scalar_symbol(cg, call->as.call.callee);
        if (s && s->stype.fn_ptr_desc && index < s->stype.fn_ptr_desc->param_count)
            return scalar_resolve(cg, s->stype.fn_ptr_desc->params[index].type);
    }
    symbol_t *fs = scalar_call_symbol(cg, call);
    if (fs && fs->scalar_signature && index < fs->scalar_signature->param_count)
        return fs->scalar_signature->params[index].type;
    node_t *f = scalar_call_decl(cg, call);
    if (f && index < f->as.fn_decl.params.count)
        return scalar_resolve(cg, f->as.fn_decl.params.items[index]->as.var_decl.type);
    return NO_TYPE;
}

/* LLVM23 switch-case API keeps duplicate source cases out of invalid IR. */
static void scalar_add_case(cg_t *cg, node_t *node, LLVMValueRef sw,
                            LLVMValueRef value, LLVMBasicBlockRef destination) {
    (void)cg;
    unsigned successors = LLVMGetNumSuccessors(sw);
    for (unsigned i = 1; i < successors; ++i) {
        LLVMValueRef previous = LLVMGetSwitchCaseValue(sw, i);
        if (LLVMConstIntGetZExtValue(previous) == LLVMConstIntGetZExtValue(value)) {
            scalar_error(node, "duplicate match case value"); return;
        }
    }
    LLVMAddCase(sw, value, destination);
}
