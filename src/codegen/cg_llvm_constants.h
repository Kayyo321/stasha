#ifndef STASHA_CG_LLVM_CONSTANTS_H
#define STASHA_CG_LLVM_CONSTANTS_H
#include <stdint.h>

/* LLVM23 APInt's unsigned single-word constructor no longer silently clears
 * bits above its declared width. Noncanonical values may print plausible IR
 * but corrupt in-process optimization. This is a bit-pattern constructor, NOT
 * source range checking: the typed semantic layer must validate source values.
 * Above 64 bits, preserve LLVM's requested signed/unsigned extension semantics. */
static LLVMValueRef cg_const_int(LLVMTypeRef type, unsigned long long bits,
                                LLVMBool sign_extend) {
    if (!type || LLVMGetTypeKind(type) != LLVMIntegerTypeKind) {
        diag_begin_error("internal compiler error: integer constant requires an integer LLVM type");
        diag_finish();
        quit(Err);
        return NULL;
    }
    unsigned width = LLVMGetIntTypeWidth(type);
    if (width < 64) {
        bits &= (UINT64_C(1) << width) - 1;
        sign_extend = 0; /* canonical low bits already represent the target */
    }
    return LLVMConstInt(type, bits, sign_extend);
}

/* Restricted to codegen.c and its unity includes; undefined at the TU end. */
#define LLVMConstInt cg_const_int
#endif
