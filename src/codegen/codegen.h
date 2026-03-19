#ifndef CodegenH
#define CodegenH

#include "../ast/ast.h"

result_t codegen(node_t *ast, const char *obj_output, boolean_t test_mode,
                 const char *target_triple);

#endif
