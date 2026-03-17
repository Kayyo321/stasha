#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <string.h>
#include "codegen.h"

/* ── symbol table ── */

typedef struct {
    char *name;
    LLVMValueRef value;
    LLVMTypeRef type;
    type_kind_t stype;
    boolean_t is_atomic;
} symbol_t;

typedef struct {
    symbol_t *entries;
    usize_t count;
    usize_t capacity;
    heap_t heap;
} symtab_t;

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMValueRef current_fn;
    LLVMValueRef printf_fn;
    LLVMTypeRef printf_type;

    symtab_t globals;
    symtab_t locals;
} cg_t;

static void symtab_init(symtab_t *st) {
    st->entries = Null;
    st->count = 0;
    st->capacity = 0;
    st->heap = NullHeap;
}

static void symtab_free(symtab_t *st) {
    if (st->heap.pointer != Null) {
        deallocate(st->heap);
        st->heap = NullHeap;
        st->entries = Null;
        st->count = 0;
        st->capacity = 0;
    }
}

static void symtab_add(symtab_t *st, const char *name, LLVMValueRef value,
                        LLVMTypeRef type, type_kind_t stype, boolean_t is_atomic) {
    if (st->count >= st->capacity) {
        usize_t new_cap = st->capacity < 8 ? 8 : st->capacity * 2;
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

/* ── LLVM type helpers ── */

static LLVMTypeRef get_llvm_type(cg_t *cg, type_kind_t t) {
    switch (t) {
        case TypeVoid: return LLVMVoidTypeInContext(cg->ctx);
        case TypeI8:   return LLVMInt8TypeInContext(cg->ctx);
        case TypeI16:  return LLVMInt16TypeInContext(cg->ctx);
        case TypeI32:  return LLVMInt32TypeInContext(cg->ctx);
        case TypeI64:  return LLVMInt64TypeInContext(cg->ctx);
        case TypeStr:  return LLVMPointerTypeInContext(cg->ctx, 0);
        case TypeBool: return LLVMInt1TypeInContext(cg->ctx);
    }
    return LLVMVoidTypeInContext(cg->ctx);
}

static LLVMValueRef coerce_int(cg_t *cg, LLVMValueRef val, LLVMTypeRef target) {
    LLVMTypeRef src = LLVMTypeOf(val);
    if (src == target) return val;
    if (LLVMGetTypeKind(target) != LLVMIntegerTypeKind
        || LLVMGetTypeKind(src) != LLVMIntegerTypeKind)
        return val;
    unsigned tw = LLVMGetIntTypeWidth(target);
    unsigned sw = LLVMGetIntTypeWidth(src);
    if (tw > sw) return LLVMBuildSExt(cg->builder, val, target, "sext");
    if (tw < sw) return LLVMBuildTrunc(cg->builder, val, target, "trunc");
    return val;
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
    LLVMValueRef left = gen_expr(cg, node->as.binary.left);
    LLVMValueRef right = gen_expr(cg, node->as.binary.right);

    LLVMTypeRef lt = LLVMTypeOf(left);
    LLVMTypeRef rt = LLVMTypeOf(right);
    if (lt != rt && LLVMGetTypeKind(lt) == LLVMIntegerTypeKind
        && LLVMGetTypeKind(rt) == LLVMIntegerTypeKind) {
        unsigned lw = LLVMGetIntTypeWidth(lt);
        unsigned rw = LLVMGetIntTypeWidth(rt);
        LLVMTypeRef wider = lw >= rw ? lt : rt;
        left = coerce_int(cg, left, wider);
        right = coerce_int(cg, right, wider);
    }

    switch (node->as.binary.op) {
        case TokPlus:   return LLVMBuildAdd(cg->builder, left, right, "add");
        case TokMinus:  return LLVMBuildSub(cg->builder, left, right, "sub");
        case TokStar:   return LLVMBuildMul(cg->builder, left, right, "mul");
        case TokSlash:  return LLVMBuildSDiv(cg->builder, left, right, "div");
        case TokLt:     return LLVMBuildICmp(cg->builder, LLVMIntSLT, left, right, "lt");
        case TokGt:     return LLVMBuildICmp(cg->builder, LLVMIntSGT, left, right, "gt");
        case TokLtEq:   return LLVMBuildICmp(cg->builder, LLVMIntSLE, left, right, "le");
        case TokGtEq:   return LLVMBuildICmp(cg->builder, LLVMIntSGE, left, right, "ge");
        case TokEqEq:   return LLVMBuildICmp(cg->builder, LLVMIntEQ,  left, right, "eq");
        case TokBangEq: return LLVMBuildICmp(cg->builder, LLVMIntNE,  left, right, "ne");
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
        return LLVMBuildNeg(cg->builder, operand, "neg");
    }
    if (node->as.unary.op == TokBang) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        return LLVMBuildNot(cg->builder, operand, "not");
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
    if (target->kind != NodeIdentExpr) {
        log_err("compound assignment target must be an identifier");
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    symbol_t *sym = cg_lookup(cg, target->as.ident.name);
    if (!sym) {
        log_err("undefined variable '%s'", target->as.ident.name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    LLVMValueRef rhs = gen_expr(cg, node->as.compound_assign.value);
    rhs = coerce_int(cg, rhs, sym->type);

    if (sym->is_atomic) {
        LLVMAtomicRMWBinOp op = (node->as.compound_assign.op == TokPlusEq)
            ? LLVMAtomicRMWBinOpAdd : LLVMAtomicRMWBinOpSub;
        return LLVMBuildAtomicRMW(cg->builder, op, sym->value, rhs,
                                   LLVMAtomicOrderingSequentiallyConsistent, 0);
    }
    LLVMValueRef lhs = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "");
    LLVMValueRef result = (node->as.compound_assign.op == TokPlusEq)
        ? LLVMBuildAdd(cg->builder, lhs, rhs, "add")
        : LLVMBuildSub(cg->builder, lhs, rhs, "sub");
    LLVMBuildStore(cg->builder, result, sym->value);
    return result;
}

static LLVMValueRef gen_assign(cg_t *cg, node_t *node) {
    node_t *target = node->as.assign.target;
    if (target->kind != NodeIdentExpr) {
        log_err("assignment target must be an identifier");
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    symbol_t *sym = cg_lookup(cg, target->as.ident.name);
    if (!sym) {
        log_err("undefined variable '%s'", target->as.ident.name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    LLVMValueRef rhs = gen_expr(cg, node->as.assign.value);
    rhs = coerce_int(cg, rhs, sym->type);
    LLVMBuildStore(cg->builder, rhs, sym->value);
    return rhs;
}

static LLVMValueRef gen_expr(cg_t *cg, node_t *node) {
    switch (node->kind) {
        case NodeIntLitExpr:       return gen_int_lit(cg, node);
        case NodeStrLitExpr:       return gen_str_lit(cg, node);
        case NodeIdentExpr:        return gen_ident(cg, node);
        case NodeBinaryExpr:       return gen_binary(cg, node);
        case NodeUnaryPrefixExpr:  return gen_unary_prefix(cg, node);
        case NodeUnaryPostfixExpr: return gen_unary_postfix(cg, node);
        case NodeCallExpr:         return gen_call(cg, node);
        case NodeParallelCall:     return gen_parallel_call(cg, node);
        case NodeCompoundAssign:   return gen_compound_assign(cg, node);
        case NodeAssignExpr:       return gen_assign(cg, node);
        default:
            log_err("unexpected node kind %d in expression", node->kind);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

/* ── statements ── */

static void gen_local_var(cg_t *cg, node_t *node) {
    LLVMTypeRef type = get_llvm_type(cg, node->as.var_decl.type);
    LLVMValueRef alloca_val = LLVMBuildAlloca(cg->builder, type, node->as.var_decl.name);

    if (node->as.var_decl.init) {
        LLVMValueRef init = gen_expr(cg, node->as.var_decl.init);
        init = coerce_int(cg, init, type);
        LLVMBuildStore(cg->builder, init, alloca_val);
    }

    symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, type,
               node->as.var_decl.type, node->as.var_decl.is_atomic);
}

static void gen_for(cg_t *cg, node_t *node) {
    gen_local_var(cg, node->as.for_stmt.init);

    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.body");
    LLVMBasicBlockRef inc_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.inc");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.end");

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = gen_expr(cg, node->as.for_stmt.cond);
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx))
        cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                             LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.for_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, inc_bb);

    LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
    gen_expr(cg, node->as.for_stmt.update);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
}

static void gen_ret(cg_t *cg, node_t *node) {
    if (node->as.ret_stmt.value) {
        LLVMValueRef val = gen_expr(cg, node->as.ret_stmt.value);
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        val = coerce_int(cg, val, ret_type);
        LLVMBuildRet(cg->builder, val);
    } else {
        LLVMBuildRetVoid(cg->builder);
    }
}

static void gen_debug(cg_t *cg, node_t *node) {
    LLVMValueRef value = gen_expr(cg, node->as.debug_stmt.value);
    LLVMTypeRef vtype = LLVMTypeOf(value);

    const char *fmt;
    if (vtype == LLVMPointerTypeInContext(cg->ctx, 0))
        fmt = "%s\n";
    else if (vtype == LLVMInt64TypeInContext(cg->ctx))
        fmt = "%lld\n";
    else
        fmt = "%d\n";

    if (vtype != LLVMInt32TypeInContext(cg->ctx)
        && vtype != LLVMInt64TypeInContext(cg->ctx)
        && vtype != LLVMPointerTypeInContext(cg->ctx, 0)) {
        value = LLVMBuildSExt(cg->builder, value, LLVMInt32TypeInContext(cg->ctx), "dbgext");
    }

    LLVMValueRef fmt_str = LLVMBuildGlobalStringPtr(cg->builder, fmt, "dbg_fmt");
    LLVMValueRef args[2] = { fmt_str, value };
    LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 2, "");
}

static void gen_stmt(cg_t *cg, node_t *node) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;
    switch (node->kind) {
        case NodeVarDecl:   gen_local_var(cg, node); break;
        case NodeForStmt:   gen_for(cg, node); break;
        case NodeRetStmt:   gen_ret(cg, node); break;
        case NodeDebugStmt: gen_debug(cg, node); break;
        case NodeExprStmt:  gen_expr(cg, node->as.expr_stmt.expr); break;
        default: log_err("unexpected statement kind %d", node->kind); break;
    }
}

static void gen_block(cg_t *cg, node_t *node) {
    for (usize_t i = 0; i < node->as.block.stmts.count; i++)
        gen_stmt(cg, node->as.block.stmts.items[i]);
}

/* ── top-level codegen ── */

result_t codegen(node_t *ast, const char *obj_output) {
    cg_t cg;
    cg.ctx = LLVMContextCreate();
    cg.module = LLVMModuleCreateWithNameInContext(ast->as.module.name, cg.ctx);
    cg.builder = LLVMCreateBuilderInContext(cg.ctx);
    cg.current_fn = Null;
    symtab_init(&cg.globals);
    symtab_init(&cg.locals);

    /* declare printf */
    LLVMTypeRef printf_param_types[] = { LLVMPointerTypeInContext(cg.ctx, 0) };
    cg.printf_type = LLVMFunctionType(LLVMInt32TypeInContext(cg.ctx),
                                       printf_param_types, 1, 1);
    cg.printf_fn = LLVMAddFunction(cg.module, "printf", cg.printf_type);

    /* pass 1: forward-declare all globals and functions */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];

        if (decl->kind == NodeVarDecl) {
            LLVMTypeRef type = get_llvm_type(&cg, decl->as.var_decl.type);
            LLVMValueRef global = LLVMAddGlobal(cg.module, type, decl->as.var_decl.name);
            if (decl->as.var_decl.linkage == LinkageInternal)
                LLVMSetLinkage(global, LLVMInternalLinkage);

            LLVMValueRef init_val = LLVMConstNull(type);
            if (decl->as.var_decl.init && decl->as.var_decl.init->kind == NodeIntLitExpr)
                init_val = LLVMConstInt(type,
                    (unsigned long long)decl->as.var_decl.init->as.int_lit.value, 1);
            LLVMSetInitializer(global, init_val);

            symtab_add(&cg.globals, decl->as.var_decl.name, global, type,
                       decl->as.var_decl.type, decl->as.var_decl.is_atomic);
        }

        if (decl->kind == NodeFnDecl) {
            usize_t pc = decl->as.fn_decl.params.count;
            LLVMTypeRef *ptypes = Null;
            heap_t ptypes_heap = NullHeap;
            if (pc > 0) {
                ptypes_heap = allocate(pc, sizeof(LLVMTypeRef));
                ptypes = ptypes_heap.pointer;
                for (usize_t j = 0; j < pc; j++)
                    ptypes[j] = get_llvm_type(&cg,
                        decl->as.fn_decl.params.items[j]->as.var_decl.type);
            }
            boolean_t is_main = strcmp(decl->as.fn_decl.name, "main") == 0;
            LLVMTypeRef ret_type = is_main
                ? LLVMInt32TypeInContext(cg.ctx)
                : get_llvm_type(&cg, decl->as.fn_decl.return_type);
            LLVMTypeRef fn_type = LLVMFunctionType(ret_type, ptypes, (unsigned)pc, 0);
            LLVMValueRef fn = LLVMAddFunction(cg.module, decl->as.fn_decl.name, fn_type);
            if (decl->as.fn_decl.linkage == LinkageInternal)
                LLVMSetLinkage(fn, LLVMInternalLinkage);
            symtab_add(&cg.globals, decl->as.fn_decl.name, fn, Null, TypeVoid, False);
            if (pc > 0) deallocate(ptypes_heap);
        }
    }

    /* pass 2: generate function bodies */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeFnDecl) continue;

        symbol_t *sym = cg_lookup(&cg, decl->as.fn_decl.name);
        cg.current_fn = sym->value;
        cg.locals.count = 0;

        LLVMBasicBlockRef entry =
            LLVMAppendBasicBlockInContext(cg.ctx, cg.current_fn, "entry");
        LLVMPositionBuilderAtEnd(cg.builder, entry);

        for (usize_t j = 0; j < decl->as.fn_decl.params.count; j++) {
            node_t *param = decl->as.fn_decl.params.items[j];
            LLVMTypeRef ptype = get_llvm_type(&cg, param->as.var_decl.type);
            LLVMValueRef alloca_val = LLVMBuildAlloca(cg.builder, ptype,
                                                       param->as.var_decl.name);
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, (unsigned)j),
                           alloca_val);
            symtab_add(&cg.locals, param->as.var_decl.name, alloca_val, ptype,
                       param->as.var_decl.type, False);
        }

        gen_block(&cg, decl->as.fn_decl.body);

        LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg.builder);
        if (!LLVMGetBasicBlockTerminator(cur_bb)) {
            if (decl->as.fn_decl.return_type == TypeVoid)
                LLVMBuildRetVoid(cg.builder);
            else
                LLVMBuildRet(cg.builder,
                    LLVMConstNull(get_llvm_type(&cg, decl->as.fn_decl.return_type)));
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

    return Ok;
}
