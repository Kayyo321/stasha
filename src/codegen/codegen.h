#ifndef CodegenH
#define CodegenH

#include "../ast/ast.h"
#include "../target/target.h"
#include "../common/observation_options.h"

result_t codegen_resolve_target(const char *requested, sts_target_spec *out);

result_t codegen(node_t *ast, const char *obj_output, boolean_t test_mode,
                 const char *target_triple, const char *source_file,
                 boolean_t debug_mode, int optimization_level, boolean_t emit_ir);

result_t codegen_observation_check_source(const char *source, const char *path);
result_t codegen_observation_check_providers(node_t *ast, const sts_observation_config *config);
result_t codegen_observed(node_t *ast, const char *obj_output, boolean_t test_mode,
                 const char *target_triple, const char *source_file,
                 boolean_t debug_mode, int optimization_level, boolean_t emit_ir,
                 const sts_observation_config *config, sts_observation_manifest *manifest);

#endif
