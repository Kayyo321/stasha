/* Shared target-layout primitive, also built by the independent C oracle.
 * Callers validate that each member is sized before invoking this function. */
static LLVMTypeRef sts_layout_storage(LLVMContextRef ctx, LLVMTargetDataRef data,
                                      LLVMTypeRef *members, size_t count,
                                      int packed) {
    uint64_t max_size = 1;
    unsigned max_align = 1;
    LLVMTypeRef anchor = LLVMInt8TypeInContext(ctx);
    for (size_t i = 0; i < count; i++) {
        uint64_t size = LLVMABISizeOfType(data, members[i]);
        unsigned align = LLVMABIAlignmentOfType(data, members[i]);
        if (size > max_size) max_size = size;
        if (align > max_align) { max_align = align; anchor = members[i]; }
    }
    if (packed) return LLVMArrayType2(LLVMInt8TypeInContext(ctx), max_size);
    uint64_t anchor_size = LLVMABISizeOfType(data, anchor);
    LLVMTypeRef body[2] = {
        anchor, LLVMArrayType2(LLVMInt8TypeInContext(ctx), max_size - anchor_size)
    };
    return LLVMStructTypeInContext(ctx, body, 2, 0);
}
