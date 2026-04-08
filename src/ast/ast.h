#ifndef AstH
#define AstH

#include "../common/common.h"
#include "../lexer/lexer.h"

/* ── base type kinds ── */

typedef enum {
    TypeVoid,
    TypeBool,
    TypeI8,
    TypeI16,
    TypeI32,
    TypeI64,
    TypeU8,
    TypeU16,
    TypeU32,
    TypeU64,
    TypeF32,
    TypeF64,
    TypeUser,       /* struct, enum, or alias — name stored in type_info_t */
    TypeError,      /* built-in error type: nil or message */
    TypeFnPtr,      /* function pointer with domain-tagged parameters */
    TypeFuture,     /* future handle — opaque ptr to __future_t (thread result) */
} type_kind_t;

/* ── forward declaration for fn_ptr_desc_t used inside type_info_t ── */
typedef struct fn_ptr_desc fn_ptr_desc_t;

/* ── pointer permissions (bit flags) ── */

typedef int ptr_perm_t;
enum {
    PtrNone = 0,
    PtrRead = (1 << 0),                  // readable
    PtrWrite = (1 << 1),                 // writable
    PtrArith = (1 << 2),                 // pointer arithmic
    PtrReadWrite = (PtrRead | PtrWrite), // rw
};

/* ── rich type descriptor ── */

typedef struct {
    type_kind_t base;
    char *user_name;            /* non-null when base == TypeUser */
    boolean_t is_pointer;
    ptr_perm_t ptr_perm;        /* permissions of the outermost pointer level */
    fn_ptr_desc_t *fn_ptr_desc; /* non-null when base == TypeFnPtr */
    int ptr_depth;              /* 0 = not a pointer; 1 = *; 2 = **; 3 = ***; etc. */
    ptr_perm_t ptr_perms[8];    /* per-level perms: [0]=outermost … [depth-1]=innermost */
} type_info_t;

/* ── storage / linkage ── */

typedef enum {
    StorageDefault,
    StorageStack,
    StorageHeap,
} storage_t;

/* ── function pointer descriptor (domain-tagged parameter list) ── */

typedef struct {
    storage_t storage;  /* storage domain of the parameter */
    type_info_t type;
} fn_ptr_param_t;

struct fn_ptr_desc {
    fn_ptr_param_t *params;
    usize_t param_count;
    type_info_t ret_type;
};

#define NO_TYPE ((type_info_t){.base=TypeVoid, .user_name=Null, .is_pointer=False, .ptr_perm=PtrNone, .fn_ptr_desc=Null, .ptr_depth=0})

typedef enum {
    LinkageNone,
    LinkageInternal,    /* int */
    LinkageExternal,    /* ext */
} linkage_t;

/* ── var_decl attribute flags ── */

enum {
    VdeclAtomic        = (1 << 0),  /* atomic qualifier              */
    VdeclConst         = (1 << 1),  /* const qualifier               */
    VdeclFinal         = (1 << 2),  /* final qualifier               */
    VdeclArray         = (1 << 3),  /* array declaration             */
    VdeclVolatile      = (1 << 4),  /* volatile qualifier            */
    VdeclTls           = (1 << 5),  /* thread-local storage          */
    VdeclRestrict      = (1 << 6),  /* restrict pointer hint         */
    VdeclLet           = (1 << 7),  /* type-inferred (let binding)   */
    VdeclComptimeField = (1 << 8),  /* @comptime: section — compile-time only, excluded from runtime layout */
};

/* ── type declaration flavours ── */

enum { TypeDeclStruct = 0, TypeDeclEnum = 1, TypeDeclAlias = 2, TypeDeclUnion = 3, TypeDeclInterface = 4 };

/* ── struct/union attribute flags ── */
enum {
    AttrPacked  = (1 << 0),
    AttrCLayout = (1 << 1),
    AttrWeak    = (1 << 2),
    AttrHidden  = (1 << 3),
};

/* ── bitfield width (0 = not a bitfield) ── */
/* stored in var_decl.bitfield_width */

/* ── node kinds ── */

typedef enum {
    /* top-level */
    NodeModule,
    NodeFnDecl,
    NodeVarDecl,
    NodeTypeDecl,
    NodeEnumVariant,
    NodeLib,
    NodeImpDecl,
    NodeCHeader,

    /* statements */
    NodeBlock,
    NodeForStmt,
    NodeWhileStmt,
    NodeDoWhileStmt,
    NodeInfLoop,
    NodeIfStmt,
    NodeRetStmt,
    NodeBreakStmt,
    NodeContinueStmt,
    NodePrintStmt,
    NodeExprStmt,
    NodeRemStmt,
    NodeMatchStmt,
    NodeMatchArm,
    NodeDeferStmt,
    NodeSwitchStmt,
    NodeSwitchCase,
    NodeAsmStmt,
    NodeComptimeIf,
    NodeComptimeAssert,
    NodeDesigInit,
    NodeCompoundInit,
    NodeInitField,
    NodeInitIndex,

    /* expressions */
    NodeBinaryExpr,
    NodeUnaryPrefixExpr,
    NodeUnaryPostfixExpr,
    NodeCallExpr,
    NodeMethodCall,
    NodeThreadCall,     /* thread.(fn)(args) — dispatch to thread pool */
    NodeCompoundAssign,
    NodeAssignExpr,
    NodeMultiAssign,
    NodeIdentExpr,
    NodeIntLitExpr,
    NodeFloatLitExpr,
    NodeStrLitExpr,
    NodeCharLitExpr,
    NodeBoolLitExpr,
    NodeIndexExpr,
    NodeMemberExpr,
    NodeSelfMemberExpr,
    NodeTernaryExpr,
    NodeCastExpr,
    NodeNewExpr,
    NodeSizeofExpr,
    NodeNilExpr,
    NodeMovExpr,
    NodeAddrOf,
    NodeErrorExpr,      /* error.('message') */
    NodeTestBlock,      /* test 'name' { ... } */
    NodeExpectExpr,     /* expect.(expr) */
    NodeExpectEqExpr,   /* expect_eq.(a, b) */
    NodeExpectNeqExpr,  /* expect_neq.(a, b) */
    NodeTestFailExpr,   /* test_fail.('msg') */
    NodeSelfMethodCall, /* Type.(method)(args) — self method call inside struct body */

    /* added after initial release — keep at end to avoid shifting existing values */
    NodeLibImp,          /* libimp "name" from "path"|std */
    NodeHashExpr,        /* hash.(expr) — universal hash */
    NodeEquExpr,         /* equ.(a, b) — universal equality */
    NodeFutureOp,        /* future.wait/ready/get/drop(handle) */
    NodeConstructorCall, /* type_name.(args) — alternate constructor syntax */
    NodeWithStmt,        /* with decl; cond { body } — scoped binding */
    NodeErrPropCall,     /* fn.?(args) — error propagation call */
    NodeErrProp,         /* expr? — postfix error propagation operator */
    NodeAnyTypeExpr,     /* any.(expr) — extract runtime type tag from any value */
    NodeSpreadExpr,      /* ..expr inside compound initializers */
    NodeRangeExpr,       /* start..end / start..=end / start..end:step */
} node_kind_t;

/* ── future operation kinds ── */
typedef enum {
    FutureWait,  /* future.wait(f)              — block, no return        */
    FutureReady, /* future.ready(f)             — non-blocking bool check  */
    FutureGet,   /* future.get.(Type)(f)        — block, return typed val  */
    FutureGetRaw,/* future.get(f)               — block, return void ptr   */
    FutureDrop,  /* future.drop(f)              — wait + free future       */
} future_op_t;

/* ── node list ── */

typedef struct node node_t;

typedef struct {
    node_t **items;
    usize_t count;
    usize_t capacity;
    heap_t heap;
} node_list_t;

/* ── AST node ── */

struct node {
    node_kind_t kind;
    usize_t line;        /* 1-based source line (0 = unknown)   */
    usize_t col;         /* 1-based source column (0 = unknown) */
    char *source_file;   /* source file that produced this node */
    boolean_t from_lib;  /* True if spliced from a library-backed import (imp + lib) */
    char *module_name;   /* dotted module path this decl came from, e.g. "net.socket"; NULL = root module */
    boolean_t is_c_extern; /* True if symbol must not be mangled (C library symbol) */

    union {
        /* ── top-level ── */
        struct { char *name; node_list_t decls; } module;

        struct {
            char *name;
            linkage_t linkage;
            type_info_t *return_types;  /* arena-allocated array */
            usize_t return_count;       /* 1 = single, >1 = multi */
            node_list_t params;         /* VarDecl nodes */
            node_t *body;               /* Block */
            boolean_t is_method;        /* fn Type.method(...) */
            char *struct_name;          /* the Type part (null if !is_method) */
            boolean_t is_variadic;      /* fn foo(stack i32 n, ...): void */
            int attr_flags;             /* AttrWeak | AttrHidden */
            char *type_params[8];       /* @comptime[T, U, ...] — generic type parameter names */
            usize_t type_param_count;
            char *iface_qualifier;      /* non-null for "fn flyable_i.move()" inside struct body */
        } fn_decl;

        struct {
            char *name;
            type_info_t type;
            storage_t storage;
            linkage_t linkage;
            int flags;             /* VdeclAtomic | VdeclConst | VdeclFinal | VdeclArray | VdeclVolatile | VdeclTls | VdeclRestrict */
            long array_size;
            char *array_size_name; /* non-null when size is a named const */
            node_t *init;
            int bitfield_width;    /* >0 for bitfield: i32 flags: 3 */
            int attr_flags;        /* AttrWeak | AttrHidden */
        } var_decl;

        struct {
            char *name;
            linkage_t linkage;
            int decl_kind;              /* TypeDeclStruct / TypeDeclEnum / TypeDeclAlias / TypeDeclUnion / TypeDeclInterface */
            node_list_t fields;         /* VarDecl nodes (struct/union fields) */
            node_list_t methods;        /* FnDecl nodes (inline methods) */
            node_list_t variants;       /* EnumVariant nodes */
            type_info_t alias_type;     /* for TypeDeclAlias */
            int attr_flags;             /* AttrPacked | AttrCLayout */
            unsigned align_value;       /* @align(N): 0 = default */
            char *type_params[8];       /* @comptime[T, U, ...] — generic type parameter names */
            usize_t type_param_count;
            char *impl_ifaces[8];       /* interface parents (if interface) OR implemented interfaces (if struct) */
            usize_t impl_iface_count;
        } type_decl;

        struct {
            char *name;
            boolean_t has_payload;
            type_info_t payload_type;
            storage_t payload_storage;
        } enum_variant;

        /* lib "name" [from "path"] [= alias] */
        struct { char *name; char *alias; char *path; } lib_decl;
        struct { char *module_name; } imp_decl;
        struct { char *path; char *search_dirs; } cheader_decl;
        /* libimp "name" from "path"|std — lib + imp in one */
        struct { char *name; char *path; boolean_t from_std; } libimp_decl;

        /* ── statements ── */
        struct { node_list_t stmts; } block;
        struct { node_t *init; node_t *cond; node_t *update; node_t *body; } for_stmt;
        struct { node_t *cond; node_t *body; } while_stmt;
        struct { node_t *body; node_t *cond; } do_while_stmt;
        struct { node_t *body; } inf_loop;
        struct { node_t *cond; node_t *then_block; node_t *else_block; } if_stmt;
        struct { node_list_t values; } ret_stmt;
        struct { char *fmt; usize_t fmt_len; node_list_t args; } print_stmt;
        struct { node_t *expr; } expr_stmt;
        struct { node_t *ptr; } rem_stmt;
        struct { node_t *body; } defer_stmt;
        struct { node_t *expr; node_list_t arms; } match_stmt;
        struct {
            boolean_t is_wildcard;
            char *enum_name;       /* null for wildcard / literal / bind-wildcard */
            char *variant_name;    /* null for wildcard / literal / bind-wildcard */
            char *bind_name;       /* payload/subject binding name, null if none */
            node_t *body;
            node_t *guard_expr;    /* optional: `if expr` guard, null = no guard */
            boolean_t is_literal;  /* True: integer literal pattern arm */
            long literal_value;    /* value when is_literal */
        } match_arm;

        /* switch statement */
        struct { node_t *expr; node_list_t cases; } switch_stmt;
        struct {
            boolean_t is_default;
            node_list_t values;     /* case value expressions (empty for default) */
            node_t *body;           /* Block */
        } switch_case;

        /* asm { "..." : outputs : inputs : clobbers } */
        struct { char *code; char *constraints; node_list_t operands; } asm_stmt;

        /* comptime_if platform == "..." { ... } */
        struct { char *key; char *value; node_t *body; node_t *else_body; } comptime_if;

        /* comptime_assert.(expr) */
        struct { node_t *expr; char *message; } comptime_assert;

        /* designated init: Type { .x = 1, .y = 2 } */
        struct { char *type_name; node_list_t fields; node_list_t values; } desig_init;
        struct { node_list_t items; } compound_init;
        struct { char *name; node_t *value; } init_field;
        struct { node_t *index; node_t *value; } init_index;

        /* ── expressions ── */
        struct { token_kind_t op; node_t *left; node_t *right; } binary;
        struct { token_kind_t op; node_t *operand; } unary;
        struct { char *callee; node_list_t args; } call;
        struct { node_t *object; char *method; node_list_t args; } method_call;
        struct { char *callee; node_list_t args; } thread_call;   /* NodeThreadCall */
        struct {
            future_op_t op;
            node_t     *handle;    /* the future variable expression      */
            type_info_t get_type;  /* for FutureGet: the return type      */
        } future_op;               /* NodeFutureOp */
        struct { token_kind_t op; node_t *target; node_t *value; } compound_assign;
        struct { node_t *target; node_t *value; } assign;
        struct { node_list_t targets; node_list_t values; } multi_assign;
        struct { char *name; } ident;
        struct { long value; } int_lit;
        struct { double value; } float_lit;
        struct { char *value; boolean_t is_heap; usize_t len; } str_lit;
        struct { char value; } char_lit;
        struct { boolean_t value; } bool_lit;
        struct { node_t *object; node_t *index; } index_expr;
        struct { node_t *object; char *field; } member_expr;
        struct { char *type_name; char *field; } self_member;
        struct { node_t *cond; node_t *then_expr; node_t *else_expr; } ternary;
        struct { type_info_t target; node_t *expr; } cast_expr;
        struct { node_t *size; } new_expr;
        struct { type_info_t type; } sizeof_expr;
        struct { node_t *ptr; node_t *size; } mov_expr;
        struct { node_t *operand; } addr_of;
        struct { char *fmt; usize_t fmt_len; node_list_t args; } error_expr; /* error.('fmt', ...) */
        struct { char *name; node_t *body; } test_block; /* test 'name' { ... } */
        struct { node_t *expr; } expect_expr;         /* expect.(expr) */
        struct { node_t *left; node_t *right; } expect_eq; /* expect_eq.(a,b) */
        struct { node_t *left; node_t *right; } expect_neq; /* expect_neq.(a,b) */
        struct { node_t *message; } test_fail;        /* test_fail.('msg') */
        struct { char *type_name; char *method; node_list_t args; } self_method_call; /* Type.(method)(args) */
        struct { node_t *operand; } hash_expr;   /* hash.(expr) */
        struct { node_t *left; node_t *right; } equ_expr; /* equ.(a, b) */

        /* NodeConstructorCall: type_name.(args) — sugar for type_name.new(args) */
        struct { char *type_name; node_list_t args; } ctor_call;

        /* NodeWithStmt: with decl; cond { body } [else { else_block }] */
        struct { node_t *decl; node_t *cond; node_t *body; node_t *else_block; } with_stmt;

        /* NodeErrPropCall: fn.?(args) — call fn, propagate error if non-nil */
        struct { char *callee; node_list_t args; } err_prop_call;

        /* NodeErrProp: expr? — evaluate expr, propagate if error, else yield value */
        struct { node_t *operand; } err_prop;

        /* NodeAnyTypeExpr: any.(expr) — extract runtime type discriminant */
        struct { node_t *operand; } any_type_expr;
        struct { node_t *expr; } spread_expr;
        struct { node_t *start; node_t *end; node_t *step; boolean_t inclusive; } range_expr;
    } as;

    /* Extra fields for any-variant match arms (used on NodeMatchArm) */
    boolean_t is_any_arm;        /* True for any.[...] match arm patterns */
    char     *any_type_name;     /* e.g. "i32" — the concrete type being matched */
    char     *any_bind_name;     /* binding variable name */
    type_info_t any_bind_ti;     /* type of the binding variable */
    storage_t   any_bind_storage;
};

/* ── API ── */

node_t *make_node(node_kind_t kind, usize_t line);
void ast_set_loc(node_t *node, token_t tok);
char *copy_token_text(token_t tok);
char *ast_strdup(const char *src, usize_t len);
char *ast_strdup_escape(const char *src, usize_t len, usize_t *out_len);
type_info_t *alloc_type_array(usize_t count);
fn_ptr_desc_t *alloc_fn_ptr_desc(usize_t param_count);

void node_list_init(node_list_t *list);
void node_list_push(node_list_t *list, node_t *node);

void ast_free_all(void);

#endif
