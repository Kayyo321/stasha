/* Exercises production scalar LLVM builders using only the pinned LLVM libs.
 * This is IR construction/verification, not optimized object execution. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"

int main(int argc, char **argv) {
    (void)argv;
    cg_t cg = {0};
    cg.ctx = LLVMContextCreate();
    cg.module = LLVMModuleCreateWithNameInContext("scalar.ir.probe", cg.ctx);
    cg.builder = LLVMCreateBuilderInContext(cg.ctx);
    if (argc > 1) cg_const_int(LLVMDoubleTypeInContext(cg.ctx), 1, 0);
    /* Noncanonical high APInt bits were invisible in printed IR yet broke
     * in-process optimization in LLVM23. Check actual constant storage. */
    for (unsigned width = 8; width <= 64; width *= 2) {
        type_info_t type = scalar_type(width == 8 ? TypeI8 : width == 16 ? TypeI16 : width == 32 ? TypeI32 : TypeI64);
        node_t magnitude = {0}, negative = {0};
        magnitude.kind = NodeIntLitExpr; magnitude.as.int_lit.value = 1;
        negative.kind = NodeUnaryPrefixExpr; negative.as.unary.op = TokMinus;
        negative.as.unary.operand = &magnitude;
        LLVMValueRef literal = scalar_emit_literal(&cg, &negative, type);
        if (LLVMConstIntGetZExtValue(literal) != scalar_mask(width)) {
            fprintf(stderr, "noncanonical negative literal at width %u\n", width); return 3;
        }
    }
    LLVMTypeRef wide = LLVMIntTypeInContext(cg.ctx, 128);
    if (LLVMConstIntGetZExtValue(cg_const_int(LLVMInt8TypeInContext(cg.ctx), UINT64_MAX, 0)) != 255 ||
        LLVMConstIntGetZExtValue(cg_const_int(LLVMInt8TypeInContext(cg.ctx), 128, 1)) != 128 ||
        LLVMConstIntGetSExtValue(cg_const_int(wide, UINT64_MAX, 1)) != -1 ||
        LLVMConstIntGetZExtValue(cg_const_int(wide, UINT64_MAX, 0)) != UINT64_MAX) return 4;
    const type_kind_t kinds[] = {TypeI8, TypeU8, TypeI16, TypeU16, TypeI32, TypeU32, TypeI64, TypeU64};
    const token_kind_t ops[] = {TokPlus, TokPlusBang, TokMinusBang, TokStarBang, TokSlash, TokPercent, TokGt, TokGtGt};
    const char *names[] = {"add", "checked_add", "checked_sub", "checked_mul", "div", "rem", "gt", "right"};
    for (unsigned k = 0; k < 8; ++k) for (unsigned o = 0; o < 8; ++o) {
        type_info_t source = scalar_type(kinds[k]);
        LLVMTypeRef ty = get_llvm_type(&cg, source);
        LLVMTypeRef params[2] = {ty, ty};
        LLVMTypeRef result = ops[o] == TokGt ? LLVMInt1TypeInContext(cg.ctx) : ty;
        LLVMTypeRef ft = LLVMFunctionType(result, params, 2, 0);
        char name[64]; snprintf(name, sizeof(name), "%s_%s", type_name_for_base(kinds[k]), names[o]);
        cg.current_fn = LLVMAddFunction(cg.module, name, ft);
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg.ctx, cg.current_fn, "entry");
        LLVMPositionBuilderAtEnd(cg.builder, entry);
        LLVMValueRef value = scalar_integer_op(&cg, ops[o], LLVMGetParam(cg.current_fn, 0), LLVMGetParam(cg.current_fn, 1), source);
        LLVMBuildRet(cg.builder, value);
    }
    char *error = Null;
    if (LLVMVerifyModule(cg.module, LLVMReturnStatusAction, &error)) {
        fprintf(stderr, "%s\n", error); LLVMDisposeMessage(error); return 1;
    }
    if (error) LLVMDisposeMessage(error);
    char *ir = LLVMPrintModuleToString(cg.module);
    fputs(ir, stdout);
    LLVMDisposeMessage(ir);
    LLVMDisposeBuilder(cg.builder);
    LLVMDisposeModule(cg.module);
    LLVMContextDispose(cg.ctx);
    return get_error_count() ? 2 : 0;
}
