/* Deterministic, host-width-independent integer evaluator. Values are width-
 * masked bit patterns, never signed C arithmetic. No signed overflow, signed
 * right shift, oversized shift, or MIN/-1 host division can occur here. */
#include "scalar_integer_semantics.h"
typedef struct { uint64_t bits; type_info_t type; } scalar_constant_t;
static boolean_t lookup_comptime_field(cg_t *, const char *, const char *, long *);

static boolean_t scalar_const_convert(scalar_constant_t *v, type_info_t to,
                                      boolean_t explicit_cast) {
    if (!scalar_integer(v->type) || !scalar_integer(to)) return False;
    unsigned sw = scalar_bits(v->type), tw = scalar_bits(to);
    if (!explicit_cast && to.base != TypeBool && (tw < sw ||
        (scalar_unsigned(v->type) != scalar_unsigned(to) && !(scalar_unsigned(v->type) && tw > sw)))) return False;
    if (to.base == TypeBool) v->bits = v->bits != 0;
    else {
        if (!scalar_unsigned(v->type) && (v->bits & (UINT64_C(1) << (sw - 1))))
            v->bits |= ~scalar_mask(sw);
        v->bits &= scalar_mask(tw);
    }
    v->type = to; return True;
}
static boolean_t scalar_const_eval_inner(cg_t *cg, node_t *n, type_info_t expected,
                                          scalar_constant_t *out, unsigned depth,
                                          unsigned *budget) {
    if (!n || depth > 256 || !*budget) return False;
    --*budget;
    type_info_t t = scalar_expr_type(cg, n, expected);
    uint64_t m; boolean_t negative;
    if (scalar_literal(n, &m, &negative)) {
        if (!scalar_literal_fits(n, t)) return False;
        out->bits = (negative ? UINT64_C(0) - m : m) & scalar_mask(scalar_bits(t));
        out->type = t; return True;
    }
    scalar_constant_t a, b;
#define SC_EVAL(N,T,O) scalar_const_eval_inner(cg, (N), (T), (O), depth + 1, budget)
    switch (n->kind) {
    case NodeBoolLitExpr: out->bits = n->as.bool_lit.value != 0; out->type = scalar_type(TypeBool); return True;
    case NodeCharLitExpr: {
        node_t literal = *n; literal.kind = NodeIntLitExpr;
        literal.as.int_lit.value = (unsigned char)n->as.char_lit.value;
        if (!scalar_literal_fits(&literal, t)) return False;
        out->bits = literal.as.int_lit.value; out->type = t; return True;
    }
    case NodeSizeofExpr: {
        type_info_t st = n->as.sizeof_expr.type;
        symbol_t *sym = st.base == TypeUser && !st.is_pointer && st.user_name ? scalar_symbol(cg, st.user_name) : Null;
        if (sym) {
            if (!LLVMTypeIsSized(sym->type)) return False;
            out->bits = LLVMABISizeOfType(LLVMGetModuleDataLayout(cg->module), sym->type);
        } else if (!cg_layout_query(cg, st, Null, n->line, &out->bits, Null, Null)) return False;
        out->type = scalar_type(TypeU64); return True;
    }
    case NodeMemberExpr: {
        node_t *obj = n->as.member_expr.object; long value;
        if (!obj || obj->kind != NodeIdentExpr ||
            !lookup_comptime_field(cg, obj->as.ident.name, n->as.member_expr.field, &value)) return False;
        out->bits = (uint64_t)value; out->type = scalar_type(TypeI64); return True;
    }
    case NodeIdentExpr: {
        symbol_t *s = scalar_symbol(cg, n->as.ident.name);
        if (!s || !(s->flags & SymConst) || !LLVMIsAGlobalVariable(s->value)) return False;
        LLVMValueRef v = LLVMGetInitializer(s->value);
        if (!v || !LLVMIsAConstantInt(v) || !scalar_integer(t)) return False;
        out->bits = LLVMConstIntGetZExtValue(v); out->type = t; return True;
    }
    case NodeCastExpr:
        if (!SC_EVAL(n->as.cast_expr.expr, NO_TYPE, &a) || !scalar_const_convert(&a, t, True)) return False;
        *out = a; return True;
    case NodeTernaryExpr:
        if (!SC_EVAL(n->as.ternary.cond, NO_TYPE, &a)) return False;
        if (!SC_EVAL(a.bits ? n->as.ternary.then_expr : n->as.ternary.else_expr, t, out)) return False;
        return scalar_const_convert(out, t, False);
    case NodeUnaryPrefixExpr:
        if (!SC_EVAL(n->as.unary.operand, n->as.unary.op == TokBang ? NO_TYPE : t, &a)) return False;
        if (n->as.unary.op == TokBang) { out->bits = !a.bits; out->type = scalar_type(TypeBool); return True; }
        if (!scalar_const_convert(&a, t, False)) return False;
        out->type = t;
        if (n->as.unary.op == TokMinus) out->bits = (UINT64_C(0) - a.bits) & scalar_mask(scalar_bits(t));
        else if (n->as.unary.op == TokTilde) out->bits = (~a.bits) & scalar_mask(scalar_bits(t));
        else if (n->as.unary.op == TokPlus) out->bits = a.bits;
        else return False;
        return True;
    case NodeBinaryExpr: {
        token_kind_t op = n->as.binary.op;
        if (op == TokAmpAmp || op == TokPipePipe) {
            if (!SC_EVAL(n->as.binary.left, NO_TYPE, &a)) return False;
            out->type = scalar_type(TypeBool);
            if ((op == TokAmpAmp && !a.bits) || (op == TokPipePipe && a.bits)) { out->bits = a.bits != 0; return True; }
            if (!SC_EVAL(n->as.binary.right, NO_TYPE, &b)) return False;
            out->bits = b.bits != 0; return True;
        }
        boolean_t shift = op == TokLtLt || op == TokGtGt;
        type_info_t common = shift ? scalar_expr_type(cg, n->as.binary.left, expected)
            : scalar_common(cg, n->as.binary.left, n->as.binary.right, expected);
        if (!scalar_integer(common) ||
            !SC_EVAL(n->as.binary.left, common, &a) || !scalar_const_convert(&a, common, False) ||
            !SC_EVAL(n->as.binary.right, shift ? NO_TYPE : common, &b)) return False;
        if (!shift && !scalar_const_convert(&b, common, False)) return False;
        uint64_t result; boolean_t boolean_result; unsigned reason;
        if (!sts_scalar_fold_integer(op, a.bits, b.bits, scalar_bits(common), scalar_unsigned(common),
                scalar_bits(b.type), scalar_unsigned(b.type), &result, &boolean_result, &reason)) return False;
        out->bits = result;
        out->type = boolean_result ? scalar_type(TypeBool) : t;
        return True;
    }
    default: return False;
    }
#undef SC_EVAL
}
static boolean_t scalar_const_eval(cg_t *cg, node_t *n, type_info_t expected, scalar_constant_t *out) {
    unsigned budget = 10000;
    return scalar_const_eval_inner(cg, n, scalar_resolve(cg, expected), out, 0, &budget);
}
static LLVMValueRef scalar_global_initializer(cg_t *cg, node_t *n, type_info_t target) {
    target = scalar_resolve(cg, target);
    scalar_constant_t value;
    if (target.is_pointer && n->kind == NodeCastExpr && n->as.cast_expr.target.is_pointer) {
        if (scalar_const_eval(cg, n->as.cast_expr.expr, NO_TYPE, &value)) {
            LLVMValueRef integer = LLVMConstInt(get_llvm_type(cg, value.type), value.bits, 0);
            return LLVMConstIntToPtr(integer, get_llvm_type(cg, target));
        }
    } else if (scalar_integer(target) && scalar_const_eval(cg, n, target, &value) && scalar_const_convert(&value, target, False)) {
        return LLVMConstInt(get_llvm_type(cg, target), value.bits, 0);
    }
    scalar_error(n, "global scalar initializer is invalid, out of range, or not a supported constant expression");
    return LLVMConstNull(get_llvm_type(cg, target));
}
