#ifndef EditorToolingH
#define EditorToolingH

#include "../ast/ast.h"

typedef enum {
    EditorModeNone,
    EditorModeCheck,
    EditorModeTokens,
    EditorModeSymbols,
    EditorModeDefinition,
    EditorModeComplete,
    EditorModeHints,
    EditorModeFormat,
    EditorModeRefs,
    EditorModeRename,
} editor_mode_t;

char *editor_read_stdin(void);
void editor_free_buffer(char *buffer);
void editor_print_diagnostics_json(void);
void editor_print_tokens_json(const char *source, const char *path);
void editor_print_symbols_json(const node_t *ast, const char *path);
void editor_print_definition_json(const node_t *ast, const char *path, usize_t line, usize_t col);
void editor_print_completions_json(const node_t *ast, const char *source, const char *path,
                                   usize_t line, usize_t col);
void editor_print_hints_json(const node_t *ast, const char *path);
void editor_print_format(const char *source);
void editor_print_refs_json(const node_t *ast, const char *path, usize_t line, usize_t col);
void editor_print_rename_json(const node_t *ast, const char *path, usize_t line, usize_t col,
                               const char *new_name);

#endif
