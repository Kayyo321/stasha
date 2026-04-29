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
    TypeFuture,     /* future.[T] task handle — opaque runtime pointer */
    TypeStream,     /* stream.[T] async stream handle — opaque runtime pointer */
    TypeSlice,      /* []T — fat pointer (ptr, len, cap) */
    TypeZone,       /* zone — opaque arena allocator handle (void* state pointer) */
    TypeInfer,      /* sentinel: "infer me" — used for trailing-closure params */
} type_kind_t;

/* ── forward declaration for fn_ptr_desc_t and type_info_t ── */
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

typedef struct type_info type_info_t;
struct type_info {
    type_kind_t base;
    char *user_name;            /* non-null when base == TypeUser */
    boolean_t is_pointer;
    ptr_perm_t ptr_perm;        /* permissions of the outermost pointer level */
    fn_ptr_desc_t *fn_ptr_desc; /* non-null when base == TypeFnPtr */
    int ptr_depth;              /* 0 = not a pointer; 1 = *; 2 = **; 3 = ***; etc. */
    ptr_perm_t ptr_perms[8];    /* per-level perms: [0]=outermost … [depth-1]=innermost */
    type_info_t *elem_type;     /* non-null when base == TypeSlice; heap-allocated [1] */
    boolean_t nullable;         /* True when written as ?T *p — pointer may be nil */
};

/* ── storage / linkage ── */

typedef enum {
    StorageDefault,
    StorageStack,
    StorageHeap,
} storage_t;

typedef enum {
    CoroNone,
    CoroTask,
    CoroStream,
    CoroUnknown,
} coro_flavor_t;

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

#define NO_TYPE ((type_info_t){.base=TypeVoid, .user_name=Null, .is_pointer=False, .ptr_perm=PtrNone, .fn_ptr_desc=Null, .ptr_depth=0, .elem_type=Null})

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
    VdeclFrees         = (1 << 9),  /* @frees: parameter must be rem()'d on all paths */
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

/* ── fileheader (@[[...]]) entries ── */

typedef enum {
    FhFlag,  /* @[[weak]] — key with no value */
    FhStr,   /* @[[export_name: "x"]] */
    FhInt,   /* @[[align: 64]] */
    FhIdent, /* @[[abi: c]] / @[[diagnostic: push]] */
    FhCond,  /* @[[if: os == "linux"]] / @[[require: pointer_width == 64]] */
    FhCall,  /* @[[diagnostic: ignore("unused-param")]] / before("name") */
    FhText,  /* free-form text value for doc/returns/params */
} fh_value_kind_t;

typedef enum {
    FhCondNone,
    FhCondOsEq,        /* os == "linux" */
    FhCondArchEq,      /* target.arch == "x86_64" (or bare arch ==) */
    FhCondPtrWidthEq,  /* pointer_width == 64 */
    FhCondAlwaysFalse, /* unknown lhs — skip decl silently; error for require */
} fh_cond_kind_t;

typedef struct {
    fh_cond_kind_t op;
    char *str_rhs;
    int int_rhs;
} fh_cond_t;

typedef struct {
    char *key;
    fh_value_kind_t vkind;
    char *str_val;     /* FhStr / FhIdent / FhText / FhCall (call-fn name) */
    long int_val;      /* FhInt */
    char *call_arg;    /* FhCall: inner string argument */
    fh_cond_t cond;    /* FhCond */
    usize_t line;
    usize_t col;
} fh_entry_t;

typedef struct {
    fh_entry_t *items;
    usize_t count;
    usize_t capacity;
    heap_t heap;
} fileheader_t;

void fileheader_init(fileheader_t *fh);
void fileheader_push(fileheader_t *fh, fh_entry_t entry);
fileheader_t *fileheader_alloc(void);
void fileheader_merge(fileheader_t *dst, fileheader_t *src);

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

    /* slice builtins — keep at end to avoid shifting existing values */
    NodeSliceExpr,   /* arr[lo:hi] / arr[lo:] / arr[:hi] / arr[:]       */
    NodeMakeExpr,    /* make.([]T, len) / make.([]T, len, cap)           */
    NodeAppendExpr,  /* append.(s, val)                                   */
    NodeCopyExpr,    /* copy.(dst, src) → i32                            */
    NodeLenExpr,     /* len.(s) → i32                                    */
    NodeCapExpr,     /* cap.(s) → i32                                    */

    /* memory-safety constructs — added for new safety redesign */
    NodeUnsafeBlock,  /* unsafe { ... } — suppresses safety checks        */
    NodeZoneDecl,     /* zone name { ... } — lexical zone                 */
    NodeZoneStmt,     /* zone name; — manual zone declaration statement    */
    NodeZoneFreeStmt, /* zone.free(name) — manual zone release             */
    NodeZoneMoveExpr, /* zone.move(p) — extract pointer from zone         */
    NodeNewInZone,    /* new.(T) in name — allocate into zone              */
    NodeFlaggedIndex, /* buf[unchecked: i] — bounds-check-free index      */

    /* comparison-chain — x > 10 and < 20 / x == 1 or 2 or 3 */
    NodeCmpChain,

    /* foreach slice iteration — keep at end to avoid shifting existing values */
    NodeForeachStmt,  /* foreach elem in slice { body } */

    /* fileheader lifecycle blocks — @[[init]] { ... } / @[[exit]] { ... } */
    NodeInitBlock,
    NodeExitBlock,

    /* comptime format string: @'...' / heap @'...' */
    NodeComptimeFmt,

    /* inline sub-module block: [int|ext] mod name { decls... } */
    NodeSubMod,

    /* colon static accessor: module:fn(args) or module:Type:method(args) */
    NodeColonCall,

    /* signals: type-routed synchronous dispatch */
    NodeWatchStmt,   /* watch.(T name) => { body } */
    NodeSendStmt,    /* send.(expr) */
    NodeQuitStmt,    /* quit.(code) */

    /* async/await — ergonomic surface over thread pool + future */
    NodeAsyncCall,       /* async.(fn)(args) — dispatch, returns future.[T] */
    NodeAwaitExpr,       /* await(f) / await.(fn)(args) — block, drop, return value */
    NodeAwaitCombinator, /* await.all(...) / await.any(...) */
    NodeYieldExpr,       /* yield expr; */
    NodeYieldNowExpr,    /* yield;      */

    /* sugar pack: lambda expression */
    NodeLambda,          /* lam.(params): ret { body } — lifted to module-level fn */
} node_kind_t;

/* maximum conditions in a single comparison chain */
#define CMP_CHAIN_MAX 32

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
    boolean_t is_c_extern;        /* True if symbol must not be mangled (C library symbol) */
    boolean_t hidden_from_import; /* True for decls inside int mod blocks — skipped by resolve_imports */

    /* fileheader entries attached to the node (decls + module).
       NULL if no @[[...]] attributes were applied to this node. */
    fileheader_t *headers;

    union {
        /* ── top-level ── */
        struct { char *name; node_list_t decls; boolean_t freestanding; long org_addr; boolean_t has_org; } module;

        /* @[[init]] / @[[exit]] blocks */
        struct { char *title; char *before_name; char *after_name; node_t *body; int priority; } lifecycle_block;

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
            boolean_t is_async;         /* `async fn ...` marker — enables async.()/await.() shorthand */
            coro_flavor_t coro_flavor;  /* inferred coroutine flavor for async fns */
            type_info_t yield_type;     /* inferred stream item type for yielding async fns */
            boolean_t has_await;
            boolean_t has_yield_value;
            boolean_t has_yield_now;
        } fn_decl;

        struct {
            char *name;
            type_info_t type;
            storage_t storage;
            linkage_t linkage;
            int flags;             /* VdeclAtomic | VdeclConst | VdeclFinal | VdeclArray | VdeclVolatile | VdeclTls | VdeclRestrict */
            int  array_ndim;           /* number of array dimensions (0 = not an array) */
            long array_sizes[8];       /* per-dimension sizes; [0]=outermost … [ndim-1]=innermost */
            char *array_size_names[8]; /* non-null when a dimension's size is a named const */
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
        struct { char *iter_name; node_t *slice; node_t *body; } foreach_stmt;
        struct { node_t *body; } inf_loop;
        struct { node_t *cond; node_t *then_block; node_t *else_block; } if_stmt;
        struct { node_list_t values; } ret_stmt;
        struct { char *fmt; usize_t fmt_len; node_list_t args; boolean_t to_stderr; } print_stmt;
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
        struct { node_t *ptr; node_t *size; char *zone_name; } mov_expr;
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

        /* ── memory-safety constructs ── */

        /* NodeUnsafeBlock: unsafe { body } */
        struct { node_t *body; } unsafe_block;

        /* NodeZoneDecl: zone name { body } — lexical zone freed at '}' */
        struct { char *name; node_t *body; } zone_decl;

        /* NodeZoneStmt: zone name; — manual zone variable declaration */
        struct { char *name; } zone_stmt;

        /* NodeZoneFreeStmt: zone.free(name) */
        struct { char *name; } zone_free;

        /* NodeZoneMoveExpr: zone.move(ptr_expr) → fresh-provenance pointer */
        struct { node_t *ptr; char *zone_name; } zone_move;

        /* NodeNewInZone: new.(T) in zone_expr — zone_expr is ident, member, or self-member */
        struct { node_t *size; node_t *zone_expr; } new_in_zone;

        /* NodeFlaggedIndex: buf[unchecked: i] — skip bounds check */
        struct { node_t *object; node_t *index; } flagged_index;

        /* NodeCmpChain: x > 10 and < 20 — base evaluated once, conditions chained
         *   logical_ops[0] unused (no connector before the first condition)
         *   logical_ops[i] = 0 → AND, 1 → OR  (connects cond[i-1] to cond[i])
         *   AND has higher precedence than OR (same as && vs ||)               */
        struct {
            node_t       *base_expr;
            boolean_t     needs_tmp;              /* base has side effects — eval once */
            usize_t       count;
            token_kind_t  cmp_ops[CMP_CHAIN_MAX];
            node_t       *rhs_nodes[CMP_CHAIN_MAX];
            int           logical_ops[CMP_CHAIN_MAX];
        } cmp_chain;

        /* ── slice builtins — keep at end ── */

        /* NodeSliceExpr: arr[lo:hi] / arr[lo:] / arr[:hi] / arr[:]
           lo==Null means 0; hi==Null means len(object) */
        struct { node_t *object; node_t *lo; node_t *hi; } slice_expr;

        /* NodeMakeExpr: make.([]T, len) / make.([]T, len, cap)
           cap==Null means cap=len.
           init!=Null is the inline-initialised form make.{...}; init points
           at a NodeCompoundInit. In that form elem_type is filled in lazily
           by codegen from cg->hint_slice_elem (LHS context); len/cap are
           ignored. */
        struct { type_info_t elem_type; node_t *len; node_t *cap; node_t *init; } make_expr;

        /* NodeAppendExpr: append.(slice, val) */
        struct { node_t *slice; node_t *val; } append_expr;

        /* NodeCopyExpr: copy.(dst, src) → i32 */
        struct { node_t *dst; node_t *src; } copy_expr;

        /* NodeLenExpr / NodeCapExpr — reuse len_expr for both */
        struct { node_t *operand; } len_expr;

        /* NodeComptimeFmt: @'...' / heap @'...' — raw fmt stored with
           unexpanded escapes; each {expr}/{expr:spec} span pre-parsed into args */
        struct { char *fmt; usize_t fmt_len; node_list_t args; boolean_t on_heap; } comptime_fmt;

        /* NodeWatchStmt: watch.(T name) => { body } */
        struct { type_info_t type; char *param_name; node_t *body; } watch_stmt;

        /* NodeSendStmt: send.(value) */
        struct { node_t *value; } send_stmt;

        /* NodeQuitStmt: quit.(code) */
        struct { node_t *code; } quit_stmt;

        /* NodeAsyncCall: async.(fn)(args) — typed variant of thread.() */
        struct { char *callee; node_list_t args; } async_call;

        /* NodeAwaitExpr: await(f) / await.(fn)(args) — handle is either
           a plain expression (future handle) or a NodeAsyncCall produced by
           await.(fn)(args) desugaring. Codegen blocks, loads typed value,
           then drops the future. `get_type.base == TypeVoid` means "infer
           from handle" at codegen (the usual case). */
        struct { node_t *handle; type_info_t get_type; boolean_t is_stream_next; } await_expr;

        /* NodeAwaitCombinator: await.all(...) / await.any(...) */
        struct { boolean_t is_any; node_list_t handles; } await_combinator;

        /* NodeYieldExpr: yield expr; */
        struct { node_t *value; } yield_expr;

        /* NodeYieldNowExpr: yield; */
        struct { int unused; } yield_now_expr;

        /* NodeLambda: lam.(params): ret { body }
           Lifted to a module-level LLVM fn during codegen.  v1: non-capturing only.
           When `inferred_params` is True, params carry TypeInfer types — gen_call
           backfills concrete types from the callee's matching parameter slot. */
        struct {
            node_list_t params;       /* VarDecl nodes (name + type + storage)      */
            type_info_t ret_type;     /* TypeInfer means infer from body's `ret`    */
            node_t     *body;         /* Block                                       */
            char       *mangled_name; /* assigned at codegen time                    */
            boolean_t   inferred_params; /* True for trailing-closure short form     */
            boolean_t   inferred_ret;    /* True when ret type was omitted           */
        } lambda_expr;

        /* NodeSubMod: [int|ext] mod name { decls... } */
        struct { char *name; linkage_t linkage; node_list_t decls; } submod;

        /* NodeColonCall: module:fn(args)  or  module:Type:method(args) */
        struct {
            char *module_name;  /* "greeter" */
            char *type_name;    /* "Builder" — NULL for 2-segment form */
            char *method_name;  /* "company" or "greet" */
            node_list_t args;
        } colon_call;
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
