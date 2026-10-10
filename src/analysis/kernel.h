#ifndef STASHA_KERNEL_ANALYSIS_H
#define STASHA_KERNEL_ANALYSIS_H
#include "../ast/ast.h"
#include "../target/target.h"
/* Conservative capability boundary, not a general borrow/lifetime proof. */
result_t analyze_kernel_profile(node_t *ast, const sts_target_spec *target);
#endif
