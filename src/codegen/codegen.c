#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <string.h>
#include <stdio.h>
#include "codegen.h"

/* ── symbol table ── */

typedef struct {
    char *name;
    LLVMValueRef value;
    LLVMTypeRef type;
    type_info_t stype;
    boolean_t is_atomic;
} symbol_t;

typedef struct {
    symbol_t *entries;
    usize_t count;
    usize_t capacity;
    heap_t heap;
} symtab_t;

/* ── struct registry ── */

typedef struct {
    char *name;
    type_info_t type;
    usize_t index;
    linkage_t linkage;
} field_info_t;

typedef struct {
    char *name;
    LLVMTypeRef llvm_type;
    field_info_t *fields;
    usize_t field_count;
    usize_t field_capacity;
    heap_t fields_heap;
    LLVMValueRef destructor;
} struct_reg_t;

/* ── enum registry ── */

typedef struct {
    char *name;
    long value;
} variant_info_t;

typedef struct {
    char *name;
    LLVMTypeRef llvm_type;
    variant_info_t *variants;
    usize_t variant_count;
    heap_t variants_heap;
} enum_reg_t;

/* ── cinclude registry ── */

typedef struct {
    char *header;
    char *alias;
} cinclude_entry_t;

/* ── destructor scope tracking ── */

typedef struct {
    LLVMValueRef alloca_val;
    char *struct_name;
} dtor_var_t;

typedef struct {
    dtor_var_t *vars;
    usize_t count;
    usize_t capacity;
    heap_t heap;
} dtor_scope_t;

/* ── type alias registry ── */

typedef struct {
    char *name;
    type_info_t actual;
} type_alias_t;

/* ── code generator state ── */

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMValueRef current_fn;
    LLVMValueRef printf_fn;
    LLVMTypeRef printf_type;
    LLVMValueRef malloc_fn;
    LLVMTypeRef malloc_type;
    LLVMValueRef free_fn;
    LLVMTypeRef free_type;

    symtab_t globals;
    symtab_t locals;

    struct_reg_t *structs;
    usize_t struct_count;
    usize_t struct_cap;
    heap_t structs_heap;

    enum_reg_t *enums;
    usize_t enum_count;
    usize_t enum_cap;
    heap_t enums_heap;

    cinclude_entry_t *cincludes;
    usize_t cinclude_count;
    usize_t cinclude_cap;
    heap_t cincludes_heap;

    type_alias_t *aliases;
    usize_t alias_count;
    usize_t alias_cap;
    heap_t aliases_heap;

    dtor_scope_t *dtor_stack;
    usize_t dtor_depth;
    usize_t dtor_cap;
    heap_t dtor_stack_heap;

    LLVMBasicBlockRef break_target;
    LLVMBasicBlockRef continue_target;
} cg_t;

/* ── symtab ── */

static void symtab_init(symtab_t *st) {
    st->entries = Null; st->count = 0; st->capacity = 0; st->heap = NullHeap;
}
static void symtab_free(symtab_t *st) {
    if (st->heap.pointer != Null) {
        deallocate(st->heap); st->heap = NullHeap;
        st->entries = Null; st->count = 0; st->capacity = 0;
    }
}
static void symtab_add(symtab_t *st, const char *name, LLVMValueRef value,
                        LLVMTypeRef type, type_info_t stype, boolean_t is_atomic) {
    if (st->count >= st->capacity) {
        usize_t new_cap = st->capacity < 16 ? 16 : st->capacity * 2;
        if (st->heap.pointer == Null)
            st->heap = allocate(new_cap, sizeof(symbol_t));
        else
            st->heap = reallocate(st->heap, new_cap * sizeof(symbol_t));
        st->entries = st->heap.pointer;
        st->capacity = new_cap;
    }
    symbol_t sym;
    sym.name = (char *)name;
    sym.value = value;
    sym.type = type;
    sym.stype = stype;
    sym.is_atomic = is_atomic;
    st->entries[st->count++] = sym;
}
static symbol_t *symtab_lookup(symtab_t *st, const char *name) {
    for (usize_t i = st->count; i > 0; i--) {
        if (strcmp(st->entries[i - 1].name, name) == 0)
            return &st->entries[i - 1];
    }
    return Null;
}
static symbol_t *cg_lookup(cg_t *cg, const char *name) {
    symbol_t *s = symtab_lookup(&cg->locals, name);
    if (s) return s;
    return symtab_lookup(&cg->globals, name);
}

/* ── struct / enum / alias lookup ── */

static struct_reg_t *find_struct(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->struct_count; i++)
        if (strcmp(cg->structs[i].name, name) == 0) return &cg->structs[i];
    return Null;
}

static enum_reg_t *find_enum(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->enum_count; i++)
        if (strcmp(cg->enums[i].name, name) == 0) return &cg->enums[i];
    return Null;
}

static type_info_t resolve_alias(cg_t *cg, type_info_t ti) {
    if (ti.base == TypeUser && ti.user_name) {
        for (usize_t i = 0; i < cg->alias_count; i++) {
            if (strcmp(cg->aliases[i].name, ti.user_name) == 0)
                return cg->aliases[i].actual;
        }
    }
    return ti;
}

static const char *find_cinclude_alias(cg_t *cg, const char *alias) {
    for (usize_t i = 0; i < cg->cinclude_count; i++)
        if (cg->cincludes[i].alias && strcmp(cg->cincludes[i].alias, alias) == 0)
            return cg->cincludes[i].header;
    return Null;
}

/* ── destructor scope helpers ── */

static void push_dtor_scope(cg_t *cg) {
    if (cg->dtor_depth >= cg->dtor_cap) {
        usize_t new_cap = cg->dtor_cap < 8 ? 8 : cg->dtor_cap * 2;
        if (cg->dtor_stack_heap.pointer == Null)
            cg->dtor_stack_heap = allocate(new_cap, sizeof(dtor_scope_t));
        else
            cg->dtor_stack_heap = reallocate(cg->dtor_stack_heap, new_cap * sizeof(dtor_scope_t));
        cg->dtor_stack = cg->dtor_stack_heap.pointer;
        cg->dtor_cap = new_cap;
    }
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth++];
    scope->vars = Null;
    scope->count = 0;
    scope->capacity = 0;
    scope->heap = NullHeap;
}

static void add_dtor_var(cg_t *cg, LLVMValueRef alloca_val, const char *struct_name) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    if (scope->count >= scope->capacity) {
        usize_t new_cap = scope->capacity < 4 ? 4 : scope->capacity * 2;
        if (scope->heap.pointer == Null)
            scope->heap = allocate(new_cap, sizeof(dtor_var_t));
        else
            scope->heap = reallocate(scope->heap, new_cap * sizeof(dtor_var_t));
        scope->vars = scope->heap.pointer;
        scope->capacity = new_cap;
    }
    scope->vars[scope->count].alloca_val = alloca_val;
    scope->vars[scope->count].struct_name = (char *)struct_name;
    scope->count++;
}

static void emit_dtor_calls(cg_t *cg, dtor_scope_t *scope) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;
    for (usize_t i = scope->count; i > 0; i--) {
        dtor_var_t *dv = &scope->vars[i - 1];
        struct_reg_t *sr = find_struct(cg, dv->struct_name);
        if (sr && sr->destructor) {
            LLVMTypeRef fn_type = LLVMGlobalGetValueType(sr->destructor);
            LLVMValueRef args[1] = { dv->alloca_val };
            LLVMBuildCall2(cg->builder, fn_type, sr->destructor, args, 1, "");
        }
    }
}

static void emit_all_dtor_calls(cg_t *cg) {
    for (usize_t i = cg->dtor_depth; i > 0; i--)
        emit_dtor_calls(cg, &cg->dtor_stack[i - 1]);
}

/* Remove a variable from all dtor scopes so its destructor isn't called.
   Used when a struct is returned by value (moved to caller). */
static void remove_from_dtor_scopes(cg_t *cg, LLVMValueRef alloca_val) {
    for (usize_t d = 0; d < cg->dtor_depth; d++) {
        dtor_scope_t *scope = &cg->dtor_stack[d];
        for (usize_t i = 0; i < scope->count; i++) {
            if (scope->vars[i].alloca_val == alloca_val) {
                for (usize_t j = i; j + 1 < scope->count; j++)
                    scope->vars[j] = scope->vars[j + 1];
                scope->count--;
                return;
            }
        }
    }
}

static void pop_dtor_scope(cg_t *cg) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    emit_dtor_calls(cg, scope);
    if (scope->heap.pointer != Null) deallocate(scope->heap);
    cg->dtor_depth--;
}

/* ── LLVM type helpers ── */

static boolean_t is_float_type(type_info_t ti) {
    return ti.base == TypeF32 || ti.base == TypeF64;
}

static boolean_t is_unsigned_type(type_info_t ti) {
    return ti.base == TypeU8 || ti.base == TypeU16
        || ti.base == TypeU32 || ti.base == TypeU64;
}

static boolean_t is_integer_type(type_info_t ti) {
    return ti.base >= TypeI8 && ti.base <= TypeU64;
}

static LLVMTypeRef get_llvm_base_type(cg_t *cg, type_info_t ti) {
    ti = resolve_alias(cg, ti);
    if (ti.is_pointer)
        return LLVMPointerTypeInContext(cg->ctx, 0);
    switch (ti.base) {
        case TypeVoid: return LLVMVoidTypeInContext(cg->ctx);
        case TypeBool: return LLVMInt1TypeInContext(cg->ctx);
        case TypeI8:  case TypeU8:  return LLVMInt8TypeInContext(cg->ctx);
        case TypeI16: case TypeU16: return LLVMInt16TypeInContext(cg->ctx);
        case TypeI32: case TypeU32: return LLVMInt32TypeInContext(cg->ctx);
        case TypeI64: case TypeU64: return LLVMInt64TypeInContext(cg->ctx);
        case TypeF32: return LLVMFloatTypeInContext(cg->ctx);
        case TypeF64: return LLVMDoubleTypeInContext(cg->ctx);
        case TypeUser: {
            struct_reg_t *sr = ti.user_name ? find_struct(cg, ti.user_name) : Null;
            if (sr) return sr->llvm_type;
            enum_reg_t *er = ti.user_name ? find_enum(cg, ti.user_name) : Null;
            if (er) return er->llvm_type;
            return LLVMInt32TypeInContext(cg->ctx);
        }
    }
    return LLVMVoidTypeInContext(cg->ctx);
}

static LLVMTypeRef get_llvm_type(cg_t *cg, type_info_t ti) {
    return get_llvm_base_type(cg, ti);
}

static LLVMValueRef coerce_int(cg_t *cg, LLVMValueRef val, LLVMTypeRef target) {
    LLVMTypeRef src = LLVMTypeOf(val);
    if (src == target) return val;
    LLVMTypeKind sk = LLVMGetTypeKind(src);
    LLVMTypeKind tk = LLVMGetTypeKind(target);
    if (sk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned tw = LLVMGetIntTypeWidth(target);
        unsigned sw = LLVMGetIntTypeWidth(src);
        if (tw > sw) return LLVMBuildSExt(cg->builder, val, target, "sext");
        if (tw < sw) return LLVMBuildTrunc(cg->builder, val, target, "trunc");
    }
    /* int -> float */
    if (sk == LLVMIntegerTypeKind && (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind))
        return LLVMBuildSIToFP(cg->builder, val, target, "itof");
    /* float -> int */
    if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) && tk == LLVMIntegerTypeKind)
        return LLVMBuildFPToSI(cg->builder, val, target, "ftoi");
    /* float -> float */
    if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
        (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
        if (sk == LLVMFloatTypeKind && tk == LLVMDoubleTypeKind)
            return LLVMBuildFPExt(cg->builder, val, target, "fpext");
        if (sk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
            return LLVMBuildFPTrunc(cg->builder, val, target, "fptrunc");
    }
    return val;
}

static boolean_t llvm_is_float(LLVMTypeRef t) {
    LLVMTypeKind k = LLVMGetTypeKind(t);
    return k == LLVMFloatTypeKind || k == LLVMDoubleTypeKind;
}

static boolean_t llvm_is_int(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMIntegerTypeKind;
}

static boolean_t llvm_is_ptr(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMPointerTypeKind;
}

/* ── forward declarations ── */

static LLVMValueRef gen_expr(cg_t *cg, node_t *node);
static void gen_stmt(cg_t *cg, node_t *node);
static void gen_block(cg_t *cg, node_t *node);

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
    LLVMValueRef load = LLVMBuildLoad2(cg->builder, sym->type, sym->value,
                                        node->as.ident.name);
    if (sym->is_atomic)
        LLVMSetOrdering(load, LLVMAtomicOrderingSequentiallyConsistent);
    return load;
}

static LLVMValueRef gen_binary(cg_t *cg, node_t *node) {
    /* short-circuit logical AND */
    if (node->as.binary.op == TokAmpAmp) {
        LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "and.rhs");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "and.merge");
        LLVMBasicBlockRef lhs_bb = LLVMGetInsertBlock(cg->builder);

        LLVMValueRef lhs = gen_expr(cg, node->as.binary.left);
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
        LLVMBasicBlockRef lhs_bb = LLVMGetInsertBlock(cg->builder);

        LLVMValueRef lhs = gen_expr(cg, node->as.binary.left);
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
        LLVMValueRef val = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "");
        LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
        LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
            ? LLVMBuildAdd(cg->builder, val, one, "inc")
            : LLVMBuildSub(cg->builder, val, one, "dec");
        LLVMBuildStore(cg->builder, result, sym->value);
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
    LLVMValueRef val = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "");
    LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
    LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
        ? LLVMBuildAdd(cg->builder, val, one, "inc")
        : LLVMBuildSub(cg->builder, val, one, "dec");
    LLVMBuildStore(cg->builder, result, sym->value);
    return val;
}

static LLVMValueRef gen_call(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.call.callee);
    if (!sym) {
        log_err("undefined function '%s'", node->as.call.callee);
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
    LLVMTypeRef fn_type = LLVMGlobalGetValueType(sym->value);
    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, sym->value,
                                       args, (unsigned)argc, "");
    if (argc > 0) deallocate(args_heap);
    return ret;
}

static LLVMValueRef gen_method_call(cg_t *cg, node_t *node) {
    node_t *obj = node->as.method_call.object;
    char *method = node->as.method_call.method;

    /* check if object is a cinclude alias: alias.func(args) */
    if (obj->kind == NodeIdentExpr) {
        const char *header = find_cinclude_alias(cg, obj->as.ident.name);
        if (header) {
            /* auto-declare C function as varargs extern */
            symbol_t *fn_sym = cg_lookup(cg, method);
            if (!fn_sym) {
                LLVMTypeRef param_types[] = { LLVMPointerTypeInContext(cg->ctx, 0) };
                LLVMTypeRef ftype = LLVMFunctionType(
                    LLVMInt32TypeInContext(cg->ctx), param_types, 1, 1);
                LLVMValueRef fn = LLVMAddFunction(cg->module, method, ftype);
                type_info_t dummy = {TypeI32, Null, False, PtrNone};
                symtab_add(&cg->globals, method, fn, Null, dummy, False);
                fn_sym = cg_lookup(cg, method);
            }
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
        store_ptr = sym->value;
        store_type = sym->type;
        atomic = sym->is_atomic;
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
    LLVMValueRef rhs = gen_expr(cg, node->as.assign.value);

    if (target->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, target->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", target->as.ident.name);
            return rhs;
        }
        rhs = coerce_int(cg, rhs, sym->type);
        LLVMBuildStore(cg->builder, rhs, sym->value);
        return rhs;
    }

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
                /* pointer indexing: load pointer, then GEP */
                LLVMValueRef ptr = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "ptr");
                LLVMTypeRef i8ty = LLVMInt8TypeInContext(cg->ctx);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, i8ty, ptr, &index_val, 1, "pidx");
                rhs = coerce_int(cg, rhs, i8ty);
                LLVMBuildStore(cg->builder, rhs, gep);
            }
            return rhs;
        }

        if (obj->kind == NodeMemberExpr) {
            /* obj.field[idx] = rhs: e.g. b.data[i] = val */
            node_t *mobj = obj->as.member_expr.object;
            char   *mfield = obj->as.member_expr.field;
            if (mobj->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, mobj->as.ident.name);
                if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                    struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
                    if (sr) {
                        for (usize_t fi = 0; fi < sr->field_count; fi++) {
                            if (strcmp(sr->fields[fi].name, mfield) != 0) continue;
                            LLVMValueRef field_gep = LLVMBuildStructGEP2(
                                cg->builder, sr->llvm_type, sym->value,
                                (unsigned)sr->fields[fi].index, mfield);
                            LLVMTypeRef ptr_type = get_llvm_type(cg, sr->fields[fi].type);
                            LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, mfield);
                            type_info_t elem_ti = sr->fields[fi].type;
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
                            LLVMBuildStore(cg->builder, rhs, gep);
                            return rhs;
                        }
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
            LLVMTypeRef i8ty = LLVMInt8TypeInContext(cg->ctx);
            index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
            LLVMValueRef gep = LLVMBuildGEP2(cg->builder, i8ty, ptr, &index_val, 1, "pidx");
            return LLVMBuildLoad2(cg->builder, i8ty, gep, "pelem");
        }
    }

    /* member pointer indexing: obj.field[idx], e.g. b.data[i] */
    if (obj->kind == NodeMemberExpr) {
        node_t *mobj = obj->as.member_expr.object;
        char   *mfield = obj->as.member_expr.field;
        if (mobj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, mobj->as.ident.name);
            if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
                if (sr) {
                    for (usize_t fi = 0; fi < sr->field_count; fi++) {
                        if (strcmp(sr->fields[fi].name, mfield) != 0) continue;
                        LLVMValueRef field_gep = LLVMBuildStructGEP2(
                            cg->builder, sr->llvm_type, sym->value,
                            (unsigned)sr->fields[fi].index, mfield);
                        LLVMTypeRef ptr_type = get_llvm_type(cg, sr->fields[fi].type);
                        LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, mfield);
                        type_info_t elem_ti = sr->fields[fi].type;
                        elem_ti.is_pointer = False;
                        LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                        index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                        LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "midx");
                        return LLVMBuildLoad2(cg->builder, elem_type, gep, "melem");
                    }
                }
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
                        return LLVMBuildLoad2(cg->builder, ft, gep, field);
                    }
                }
            }
        }
        /* might be an enum value: EnumName.Variant */
        enum_reg_t *er = find_enum(cg, obj->as.ident.name);
        if (er) {
            for (usize_t i = 0; i < er->variant_count; i++) {
                if (strcmp(er->variants[i].name, field) == 0)
                    return LLVMConstInt(er->llvm_type, (unsigned long long)er->variants[i].value, 0);
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
        case NodeRemStmt: {
            LLVMValueRef ptr = gen_expr(cg, node->as.rem_stmt.ptr);
            LLVMValueRef args[1] = { ptr };
            LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, args, 1, "");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        default:
            log_err("unexpected node kind %d in expression", node->kind);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

/* ── statements ── */

static void gen_local_var(cg_t *cg, node_t *node) {
    type_info_t ti = resolve_alias(cg, node->as.var_decl.type);
    LLVMTypeRef type;

    if (node->as.var_decl.is_array) {
        LLVMTypeRef elem = get_llvm_type(cg, ti);
        type = LLVMArrayType2(elem, (unsigned long long)node->as.var_decl.array_size);
    } else {
        type = get_llvm_type(cg, ti);
    }

    LLVMValueRef alloca_val = LLVMBuildAlloca(cg->builder, type, node->as.var_decl.name);

    if (node->as.var_decl.init) {
        LLVMValueRef init = gen_expr(cg, node->as.var_decl.init);
        if (!node->as.var_decl.is_array)
            init = coerce_int(cg, init, type);
        LLVMBuildStore(cg->builder, init, alloca_val);
    }

    symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, type,
               ti, node->as.var_decl.is_atomic);

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
        LLVMValueRef val = gen_expr(cg, node->as.ret_stmt.values.items[0]);
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        val = coerce_int(cg, val, ret_type);
        LLVMBuildRet(cg->builder, val);
    } else {
        /* multi-return: pack into struct */
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        LLVMValueRef agg = LLVMGetUndef(ret_type);
        for (usize_t i = 0; i < node->as.ret_stmt.values.count; i++) {
            LLVMValueRef val = gen_expr(cg, node->as.ret_stmt.values.items[i]);
            LLVMTypeRef field_type = LLVMStructGetTypeAtIndex(ret_type, (unsigned)i);
            val = coerce_int(cg, val, field_type);
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

static void gen_stmt(cg_t *cg, node_t *node) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;
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

/* ── registry helpers ── */

static void register_struct(cg_t *cg, const char *name, LLVMTypeRef llvm_type) {
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
    sr->llvm_type = llvm_type;
    sr->fields = Null;
    sr->field_count = 0;
    sr->field_capacity = 0;
    sr->fields_heap = NullHeap;
    sr->destructor = Null;
}

static void struct_add_field(struct_reg_t *sr, const char *name, type_info_t type,
                             usize_t index, linkage_t linkage) {
    if (sr->field_count >= sr->field_capacity) {
        usize_t new_cap = sr->field_capacity < 8 ? 8 : sr->field_capacity * 2;
        if (sr->fields_heap.pointer == Null)
            sr->fields_heap = allocate(new_cap, sizeof(field_info_t));
        else
            sr->fields_heap = reallocate(sr->fields_heap, new_cap * sizeof(field_info_t));
        sr->fields = sr->fields_heap.pointer;
        sr->field_capacity = new_cap;
    }
    sr->fields[sr->field_count].name = (char *)name;
    sr->fields[sr->field_count].type = type;
    sr->fields[sr->field_count].index = index;
    sr->fields[sr->field_count].linkage = linkage;
    sr->field_count++;
}

static void register_enum(cg_t *cg, const char *name, LLVMTypeRef llvm_type,
                           node_list_t *variants) {
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
    er->llvm_type = llvm_type;
    er->variant_count = variants->count;
    if (variants->count > 0) {
        er->variants_heap = allocate(variants->count, sizeof(variant_info_t));
        er->variants = er->variants_heap.pointer;
        for (usize_t i = 0; i < variants->count; i++) {
            er->variants[i].name = variants->items[i]->as.enum_variant.name;
            er->variants[i].value = (long)i;
        }
    } else {
        er->variants_heap = NullHeap;
        er->variants = Null;
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

static void register_cinclude(cg_t *cg, const char *header, const char *alias) {
    if (cg->cinclude_count >= cg->cinclude_cap) {
        usize_t new_cap = cg->cinclude_cap < 8 ? 8 : cg->cinclude_cap * 2;
        if (cg->cincludes_heap.pointer == Null)
            cg->cincludes_heap = allocate(new_cap, sizeof(cinclude_entry_t));
        else
            cg->cincludes_heap = reallocate(cg->cincludes_heap, new_cap * sizeof(cinclude_entry_t));
        cg->cincludes = cg->cincludes_heap.pointer;
        cg->cinclude_cap = new_cap;
    }
    cg->cincludes[cg->cinclude_count].header = (char *)header;
    cg->cincludes[cg->cinclude_count].alias = (char *)alias;
    cg->cinclude_count++;
}

/* ── top-level codegen ── */

result_t codegen(node_t *ast, const char *obj_output) {
    cg_t cg;
    memset(&cg, 0, sizeof(cg));
    cg.ctx = LLVMContextCreate();
    cg.module = LLVMModuleCreateWithNameInContext(ast->as.module.name, cg.ctx);
    cg.builder = LLVMCreateBuilderInContext(cg.ctx);
    cg.current_fn = Null;
    symtab_init(&cg.globals);
    symtab_init(&cg.locals);

    /* declare C runtime functions */
    type_info_t rt_dummy = {TypeVoid, Null, False, PtrNone};

    LLVMTypeRef printf_param[] = { LLVMPointerTypeInContext(cg.ctx, 0) };
    cg.printf_type = LLVMFunctionType(LLVMInt32TypeInContext(cg.ctx), printf_param, 1, 1);
    cg.printf_fn = LLVMAddFunction(cg.module, "printf", cg.printf_type);
    symtab_add(&cg.globals, "printf", cg.printf_fn, Null, rt_dummy, False);

    LLVMTypeRef malloc_param[] = { LLVMInt64TypeInContext(cg.ctx) };
    cg.malloc_type = LLVMFunctionType(LLVMPointerTypeInContext(cg.ctx, 0), malloc_param, 1, 0);
    cg.malloc_fn = LLVMAddFunction(cg.module, "malloc", cg.malloc_type);
    symtab_add(&cg.globals, "malloc", cg.malloc_fn, Null, rt_dummy, False);

    LLVMTypeRef free_param[] = { LLVMPointerTypeInContext(cg.ctx, 0) };
    cg.free_type = LLVMFunctionType(LLVMVoidTypeInContext(cg.ctx), free_param, 1, 0);
    cg.free_fn = LLVMAddFunction(cg.module, "free", cg.free_type);
    symtab_add(&cg.globals, "free", cg.free_fn, Null, rt_dummy, False);

    /* pass 0: register type declarations, cincludes */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];

        if (decl->kind == NodeCinclude) {
            register_cinclude(&cg, decl->as.cinclude.header, decl->as.cinclude.alias);
            continue;
        }

        if (decl->kind == NodeTypeDecl) {
            if (decl->as.type_decl.decl_kind == TypeDeclStruct) {
                LLVMTypeRef stype = LLVMStructCreateNamed(cg.ctx, decl->as.type_decl.name);
                register_struct(&cg, decl->as.type_decl.name, stype);
            } else if (decl->as.type_decl.decl_kind == TypeDeclEnum) {
                LLVMTypeRef etype = LLVMInt32TypeInContext(cg.ctx);
                register_enum(&cg, decl->as.type_decl.name, etype,
                              &decl->as.type_decl.variants);
            } else if (decl->as.type_decl.decl_kind == TypeDeclAlias) {
                register_alias(&cg, decl->as.type_decl.name, decl->as.type_decl.alias_type);
            }
        }
    }

    /* pass 0b: set struct bodies (now that all types are registered) */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeTypeDecl || decl->as.type_decl.decl_kind != TypeDeclStruct)
            continue;

        struct_reg_t *sr = find_struct(&cg, decl->as.type_decl.name);
        if (!sr) continue;

        usize_t fc = decl->as.type_decl.fields.count;
        LLVMTypeRef *field_types = Null;
        heap_t ft_heap = NullHeap;
        if (fc > 0) {
            ft_heap = allocate(fc, sizeof(LLVMTypeRef));
            field_types = ft_heap.pointer;
            for (usize_t j = 0; j < fc; j++) {
                node_t *field = decl->as.type_decl.fields.items[j];
                type_info_t fti = resolve_alias(&cg, field->as.var_decl.type);
                field_types[j] = get_llvm_type(&cg, fti);
                struct_add_field(sr, field->as.var_decl.name, fti, j,
                                 field->as.var_decl.linkage);
            }
        }
        LLVMStructSetBody(sr->llvm_type, field_types, (unsigned)fc, 0);
        if (fc > 0) deallocate(ft_heap);
    }

    /* pass 1: forward-declare all globals and functions */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];

        if (decl->kind == NodeVarDecl) {
            type_info_t ti = resolve_alias(&cg, decl->as.var_decl.type);
            LLVMTypeRef type = get_llvm_type(&cg, ti);
            LLVMValueRef global = LLVMAddGlobal(cg.module, type, decl->as.var_decl.name);
            if (decl->as.var_decl.linkage == LinkageInternal)
                LLVMSetLinkage(global, LLVMInternalLinkage);

            LLVMValueRef init_val = LLVMConstNull(type);
            if (decl->as.var_decl.init && decl->as.var_decl.init->kind == NodeIntLitExpr)
                init_val = LLVMConstInt(type,
                    (unsigned long long)decl->as.var_decl.init->as.int_lit.value, 1);
            LLVMSetInitializer(global, init_val);

            symtab_add(&cg.globals, decl->as.var_decl.name, global, type,
                       ti, decl->as.var_decl.is_atomic);
        }

        if (decl->kind == NodeFnDecl) {
            char fn_name[256];
            if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name) {
                snprintf(fn_name, sizeof(fn_name), "%s.%s",
                         decl->as.fn_decl.struct_name, decl->as.fn_decl.name);
            } else {
                snprintf(fn_name, sizeof(fn_name), "%s", decl->as.fn_decl.name);
            }

            /* build param types */
            usize_t pc = decl->as.fn_decl.params.count;
            boolean_t is_instance = decl->as.fn_decl.is_method
                && strcmp(decl->as.fn_decl.name, "new") != 0;
            usize_t total_params = pc + (is_instance ? 1 : 0);

            LLVMTypeRef *ptypes = Null;
            heap_t ptypes_heap = NullHeap;
            if (total_params > 0) {
                ptypes_heap = allocate(total_params, sizeof(LLVMTypeRef));
                ptypes = ptypes_heap.pointer;
                usize_t offset = 0;
                if (is_instance) {
                    ptypes[0] = LLVMPointerTypeInContext(cg.ctx, 0);
                    offset = 1;
                }
                for (usize_t j = 0; j < pc; j++) {
                    type_info_t pti = resolve_alias(&cg,
                        decl->as.fn_decl.params.items[j]->as.var_decl.type);
                    ptypes[j + offset] = get_llvm_type(&cg, pti);
                }
            }

            /* return type */
            LLVMTypeRef ret_type;
            boolean_t is_main = strcmp(fn_name, "main") == 0;
            if (is_main) {
                ret_type = LLVMInt32TypeInContext(cg.ctx);
            } else if (decl->as.fn_decl.return_count > 1) {
                /* multi-return: create an anonymous struct type */
                heap_t rt_heap = allocate(decl->as.fn_decl.return_count, sizeof(LLVMTypeRef));
                LLVMTypeRef *rt_fields = rt_heap.pointer;
                for (usize_t j = 0; j < decl->as.fn_decl.return_count; j++) {
                    type_info_t rti = resolve_alias(&cg, decl->as.fn_decl.return_types[j]);
                    rt_fields[j] = get_llvm_type(&cg, rti);
                }
                ret_type = LLVMStructTypeInContext(cg.ctx, rt_fields,
                    (unsigned)decl->as.fn_decl.return_count, 0);
                deallocate(rt_heap);
            } else {
                type_info_t rti = resolve_alias(&cg, decl->as.fn_decl.return_types[0]);
                ret_type = get_llvm_type(&cg, rti);
            }

            LLVMTypeRef fn_type = LLVMFunctionType(ret_type, ptypes,
                (unsigned)total_params, 0);
            LLVMValueRef fn = LLVMAddFunction(cg.module, fn_name, fn_type);
            if (decl->as.fn_decl.linkage == LinkageInternal)
                LLVMSetLinkage(fn, LLVMInternalLinkage);

            type_info_t dummy = {TypeVoid, Null, False, PtrNone};
            symtab_add(&cg.globals, ast_strdup(fn_name, strlen(fn_name)), fn, Null, dummy, False);

            if (total_params > 0) deallocate(ptypes_heap);

            /* register destructor */
            if (decl->as.fn_decl.is_method && strcmp(decl->as.fn_decl.name, "rem") == 0
                && decl->as.fn_decl.struct_name) {
                struct_reg_t *sr = find_struct(&cg, decl->as.fn_decl.struct_name);
                if (sr) sr->destructor = fn;
            }
        }
    }

    /* also forward-declare inline struct methods */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeTypeDecl || decl->as.type_decl.decl_kind != TypeDeclStruct)
            continue;
        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];
            char fn_name[256];
            snprintf(fn_name, sizeof(fn_name), "%s.%s",
                     decl->as.type_decl.name, method->as.fn_decl.name);

            /* all inline methods are instance methods: get implicit this */
            usize_t pc = method->as.fn_decl.params.count;
            usize_t total_params = pc + 1;
            heap_t ptypes_heap = allocate(total_params, sizeof(LLVMTypeRef));
            LLVMTypeRef *ptypes = ptypes_heap.pointer;
            ptypes[0] = LLVMPointerTypeInContext(cg.ctx, 0);
            for (usize_t j = 0; j < pc; j++) {
                type_info_t pti = resolve_alias(&cg,
                    method->as.fn_decl.params.items[j]->as.var_decl.type);
                ptypes[j + 1] = get_llvm_type(&cg, pti);
            }

            LLVMTypeRef ret_type;
            if (method->as.fn_decl.return_count > 1) {
                heap_t rt_heap = allocate(method->as.fn_decl.return_count, sizeof(LLVMTypeRef));
                LLVMTypeRef *rt_fields = rt_heap.pointer;
                for (usize_t j = 0; j < method->as.fn_decl.return_count; j++) {
                    type_info_t rti = resolve_alias(&cg, method->as.fn_decl.return_types[j]);
                    rt_fields[j] = get_llvm_type(&cg, rti);
                }
                ret_type = LLVMStructTypeInContext(cg.ctx, rt_fields,
                    (unsigned)method->as.fn_decl.return_count, 0);
                deallocate(rt_heap);
            } else {
                type_info_t rti = resolve_alias(&cg, method->as.fn_decl.return_types[0]);
                ret_type = get_llvm_type(&cg, rti);
            }

            LLVMTypeRef fn_type = LLVMFunctionType(ret_type, ptypes,
                (unsigned)total_params, 0);
            LLVMValueRef fn = LLVMAddFunction(cg.module, fn_name, fn_type);
            if (method->as.fn_decl.linkage == LinkageInternal)
                LLVMSetLinkage(fn, LLVMInternalLinkage);

            type_info_t dummy = {TypeVoid, Null, False, PtrNone};
            symtab_add(&cg.globals, ast_strdup(fn_name, strlen(fn_name)), fn, Null, dummy, False);
            deallocate(ptypes_heap);

            if (strcmp(method->as.fn_decl.name, "rem") == 0) {
                struct_reg_t *sr = find_struct(&cg, decl->as.type_decl.name);
                if (sr) sr->destructor = fn;
            }
        }
    }

    /* pass 2: generate function bodies */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeFnDecl) continue;

        char fn_name[256];
        if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name)
            snprintf(fn_name, sizeof(fn_name), "%s.%s",
                     decl->as.fn_decl.struct_name, decl->as.fn_decl.name);
        else
            snprintf(fn_name, sizeof(fn_name), "%s", decl->as.fn_decl.name);

        symbol_t *sym = cg_lookup(&cg, fn_name);
        cg.current_fn = sym->value;
        cg.locals.count = 0;
        cg.dtor_depth = 0;

        LLVMBasicBlockRef entry =
            LLVMAppendBasicBlockInContext(cg.ctx, cg.current_fn, "entry");
        LLVMPositionBuilderAtEnd(cg.builder, entry);

        boolean_t is_instance = decl->as.fn_decl.is_method
            && strcmp(decl->as.fn_decl.name, "new") != 0;
        usize_t param_offset = 0;

        /* implicit this parameter for instance methods */
        if (is_instance && decl->as.fn_decl.struct_name) {
            struct_reg_t *sr = find_struct(&cg, decl->as.fn_decl.struct_name);
            LLVMTypeRef this_type = sr ? LLVMPointerTypeInContext(cg.ctx, 0)
                                       : LLVMPointerTypeInContext(cg.ctx, 0);
            LLVMValueRef this_alloca = LLVMBuildAlloca(cg.builder, this_type, "this");
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, 0), this_alloca);
            type_info_t this_ti = {TypeUser, decl->as.fn_decl.struct_name, True, PtrReadWrite};
            symtab_add(&cg.locals, "this", this_alloca, this_type, this_ti, False);
            param_offset = 1;
        }

        for (usize_t j = 0; j < decl->as.fn_decl.params.count; j++) {
            node_t *param = decl->as.fn_decl.params.items[j];
            type_info_t pti = resolve_alias(&cg, param->as.var_decl.type);
            LLVMTypeRef ptype = get_llvm_type(&cg, pti);
            LLVMValueRef alloca_val = LLVMBuildAlloca(cg.builder, ptype,
                                                       param->as.var_decl.name);
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, (unsigned)(j + param_offset)),
                           alloca_val);
            symtab_add(&cg.locals, param->as.var_decl.name, alloca_val, ptype,
                       pti, False);
        }

        gen_block(&cg, decl->as.fn_decl.body);

        LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg.builder);
        if (!LLVMGetBasicBlockTerminator(cur_bb)) {
            type_info_t rti = decl->as.fn_decl.return_types[0];
            if (rti.base == TypeVoid && !rti.is_pointer)
                LLVMBuildRetVoid(cg.builder);
            else
                LLVMBuildRet(cg.builder,
                    LLVMConstNull(get_llvm_type(&cg, rti)));
        }
    }

    /* also generate inline struct method bodies */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeTypeDecl || decl->as.type_decl.decl_kind != TypeDeclStruct)
            continue;
        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];
            char fn_name[256];
            snprintf(fn_name, sizeof(fn_name), "%s.%s",
                     decl->as.type_decl.name, method->as.fn_decl.name);

            symbol_t *sym = cg_lookup(&cg, fn_name);
            if (!sym) continue;
            cg.current_fn = sym->value;
            cg.locals.count = 0;
            cg.dtor_depth = 0;

            LLVMBasicBlockRef entry =
                LLVMAppendBasicBlockInContext(cg.ctx, cg.current_fn, "entry");
            LLVMPositionBuilderAtEnd(cg.builder, entry);

            /* implicit this */
            struct_reg_t *sr = find_struct(&cg, decl->as.type_decl.name);
            LLVMTypeRef this_type = LLVMPointerTypeInContext(cg.ctx, 0);
            LLVMValueRef this_alloca = LLVMBuildAlloca(cg.builder, this_type, "this");
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, 0), this_alloca);
            type_info_t this_ti = {TypeUser, decl->as.type_decl.name, True, PtrReadWrite};
            symtab_add(&cg.locals, "this", this_alloca, this_type, this_ti, False);
            (void)sr;

            for (usize_t j = 0; j < method->as.fn_decl.params.count; j++) {
                node_t *param = method->as.fn_decl.params.items[j];
                type_info_t pti = resolve_alias(&cg, param->as.var_decl.type);
                LLVMTypeRef ptype = get_llvm_type(&cg, pti);
                LLVMValueRef alloca_val = LLVMBuildAlloca(cg.builder, ptype,
                                                           param->as.var_decl.name);
                LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, (unsigned)(j + 1)),
                               alloca_val);
                symtab_add(&cg.locals, param->as.var_decl.name, alloca_val, ptype,
                           pti, False);
            }

            gen_block(&cg, method->as.fn_decl.body);

            LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg.builder);
            if (!LLVMGetBasicBlockTerminator(cur_bb)) {
                type_info_t rti = method->as.fn_decl.return_types[0];
                if (rti.base == TypeVoid && !rti.is_pointer)
                    LLVMBuildRetVoid(cg.builder);
                else
                    LLVMBuildRet(cg.builder,
                        LLVMConstNull(get_llvm_type(&cg, rti)));
            }
        }
    }

    /* verify */
    char *error = Null;
    if (LLVMVerifyModule(cg.module, LLVMReturnStatusAction, &error)) {
        log_err("LLVM verify: %s", error);
        LLVMDisposeMessage(error);
    } else {
        if (error) LLVMDisposeMessage(error);
    }

    /* emit object file */
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target;
    error = Null;
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        log_err("target lookup failed: %s", error);
        LLVMDisposeMessage(error);
        LLVMDisposeMessage(triple);
        return Err;
    }

    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
        target, triple, "generic", "",
        LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);

    error = Null;
    if (LLVMTargetMachineEmitToFile(machine, cg.module, (char *)obj_output,
                                     LLVMObjectFile, &error)) {
        log_err("emit failed: %s", error);
        LLVMDisposeMessage(error);
        LLVMDisposeMessage(triple);
        LLVMDisposeTargetMachine(machine);
        return Err;
    }

    LLVMDisposeMessage(triple);
    LLVMDisposeTargetMachine(machine);
    LLVMDisposeBuilder(cg.builder);
    LLVMDisposeModule(cg.module);
    LLVMContextDispose(cg.ctx);

    symtab_free(&cg.globals);
    symtab_free(&cg.locals);

    /* free registries */
    for (usize_t i = 0; i < cg.struct_count; i++)
        if (cg.structs[i].fields_heap.pointer) deallocate(cg.structs[i].fields_heap);
    if (cg.structs_heap.pointer) deallocate(cg.structs_heap);
    for (usize_t i = 0; i < cg.enum_count; i++)
        if (cg.enums[i].variants_heap.pointer) deallocate(cg.enums[i].variants_heap);
    if (cg.enums_heap.pointer) deallocate(cg.enums_heap);
    if (cg.cincludes_heap.pointer) deallocate(cg.cincludes_heap);
    if (cg.aliases_heap.pointer) deallocate(cg.aliases_heap);
    if (cg.dtor_stack_heap.pointer) deallocate(cg.dtor_stack_heap);

    return Ok;
}
