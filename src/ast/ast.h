#ifndef AstH
#define AstH

#include "../common/common.h"
#include "../lexer/lexer.h"

typedef enum {
    TypeVoid,
    TypeI8,
    TypeI16,
    TypeI32,
    TypeI64,
    TypeStr,
    TypeBool,
} type_kind_t;

typedef enum {
    StorageDefault,
    StorageStack,
    StorageHeap,
} storage_t;

typedef enum {
    LinkageNone,
    LinkageInternal,
    LinkageExternal,
} linkage_t;

typedef enum {
    NodeModule,
    NodeFnDecl,
    NodeVarDecl,
    NodeBlock,
    NodeForStmt,
    NodeRetStmt,
    NodeDebugStmt,
    NodeExprStmt,
    NodeBinaryExpr,
    NodeUnaryPrefixExpr,
    NodeUnaryPostfixExpr,
    NodeCallExpr,
    NodeParallelCall,
    NodeCompoundAssign,
    NodeAssignExpr,
    NodeIdentExpr,
    NodeIntLitExpr,
    NodeStrLitExpr,
} node_kind_t;

typedef struct node node_t;

typedef struct {
    node_t **items;
    usize_t count;
    usize_t capacity;
    heap_t heap;
} node_list_t;

struct node {
    node_kind_t kind;
    usize_t line;

    union {
        struct { char *name; node_list_t decls; } module;
        struct { char *name; linkage_t linkage; type_kind_t return_type; node_list_t params; node_t *body; } fn_decl;
        struct { char *name; type_kind_t type; storage_t storage; linkage_t linkage; boolean_t is_atomic; node_t *init; } var_decl;
        struct { node_list_t stmts; } block;
        struct { node_t *init; node_t *cond; node_t *update; node_t *body; } for_stmt;
        struct { node_t *value; } ret_stmt;
        struct { node_t *value; } debug_stmt;
        struct { node_t *expr; } expr_stmt;
        struct { token_kind_t op; node_t *left; node_t *right; } binary;
        struct { token_kind_t op; node_t *operand; } unary;
        struct { char *callee; node_list_t args; } call;
        struct { boolean_t is_gpu; char *callee; node_list_t args; } parallel_call;
        struct { token_kind_t op; node_t *target; node_t *value; } compound_assign;
        struct { node_t *target; node_t *value; } assign;
        struct { char *name; } ident;
        struct { long value; } int_lit;
        struct { char *value; boolean_t is_heap; } str_lit;
    } as;
};

node_t *make_node(node_kind_t kind, usize_t line);
char *copy_token_text(token_t tok);
char *ast_strdup(const char *src, usize_t len);

void node_list_init(node_list_t *list);
void node_list_push(node_list_t *list, node_t *node);

void ast_free_all(void);

#endif
