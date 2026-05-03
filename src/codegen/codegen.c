#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "codegen.h"

/* ── DI type cache ── */

static LLVMCodeGenOptLevel map_opt_level(int level) {
    switch (level) {
    case 0:
        return LLVMCodeGenLevelNone;
    case 1:
        return LLVMCodeGenLevelLess;
    case 3:
        return LLVMCodeGenLevelAggressive;
    default:
        return LLVMCodeGenLevelDefault;
    }
}

static boolean_t ast_requires_coro_pipeline(node_t *ast) {
    if (!ast || ast->kind != NodeModule) return False;
    /* Any `async fn` is a coroutine after the v2 migration: tasks lower
       through llvm.coro.* identically to streams, so the presence of any
       async function — even one with no body await/yield — needs the
       coroutine pass pipeline. */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind == NodeFnDecl && decl->as.fn_decl.is_async)
            return True;
        if (decl->kind == NodeTypeDecl) {
            for (usize_t j = 0; j < decl->as.type_decl.methods.count; j++) {
                node_t *method = decl->as.type_decl.methods.items[j];
                if (method->kind == NodeFnDecl && method->as.fn_decl.is_async)
                    return True;
            }
        }
    }
    return False;
}

typedef struct {
    char *name;
    LLVMMetadataRef di_type;
} di_type_entry_t;

/* Raw DWARF type-encoding constants (DW_ATE_* from the DWARF spec).
   The LLVM C API uses a plain typedef unsigned for LLVMDWARFTypeEncoding. */
#define STS_DW_ATE_boolean  0x02u
#define STS_DW_ATE_float    0x04u
#define STS_DW_ATE_signed   0x05u
#define STS_DW_ATE_unsigned 0x07u

/* ── symbol table ── */

/* symbol attribute flags */
enum {
    SymAtomic   = (1 << 0),  /* atomic qualifier            */
    SymHeapVar  = (1 << 1),  /* heap primitive (malloc'd)   */
    SymConst    = (1 << 2),  /* const qualifier             */
    SymFinal    = (1 << 3),  /* final qualifier             */
    SymNil      = (1 << 4),  /* statically known to be nil  */
    SymVolatile = (1 << 5),  /* volatile qualifier          */
    SymTls      = (1 << 6),  /* thread-local storage        */
    SymZone     = (1 << 7),  /* zone variable (freed by __zone_free) */
    SymZoneAlloc = (1 << 8), /* zone-allocated pointer — rem.() is a no-op */
};

typedef struct {
    char *name;
    LLVMValueRef value;
    LLVMTypeRef type;
    type_info_t stype;
    int flags;              /* SymAtomic | SymHeapVar | SymConst | SymFinal | SymNil */
    storage_t storage;      /* storage domain of the variable */
    linkage_t linkage;
    usize_t scope_depth;    /* nesting level for lifetime checks */
    long array_size;        /* ≥0 for known-size arrays, -1 otherwise */
    long long const_int_val; /* compile-time value of a const integer, -1 if not known */
    boolean_t used;         /* True if the variable has been read at least once */
    usize_t line;           /* source line where the variable was declared (0 = unknown) */
    usize_t col;            /* source column (1-based, 0 = unknown) */
    usize_t len;            /* name span in bytes */
} symbol_t;

typedef struct {
    symbol_t *entries;
    usize_t count;
    usize_t capacity;
    heap_t heap;
} symtab_t;

/* ── struct registry ── */

typedef struct {
    char *name;
    type_info_t type;
    usize_t index;
    linkage_t linkage;
    storage_t storage;  /* StorageStack or StorageHeap — from struct memory section */
    int bit_offset;     /* bit position within backing field (0 if not a bitfield) */
    int bit_width;      /* bitfield width in bits (0 = not a bitfield) */
    long array_size;    /* > 0 if field is a fixed-size array, 0 otherwise */
} field_info_t;

/* compile-time-only field (from @comptime: section) — excluded from runtime layout */
typedef struct {
    char *name;
    long  value;
} comptime_field_t;

typedef struct {
    char *name;         /* source name, e.g. "Vec2" */
    char *mod_prefix;   /* mangled module prefix, e.g. "geom"; NULL for root module */
    LLVMTypeRef llvm_type;
    field_info_t *fields;
    usize_t field_count;
    usize_t field_capacity;
    heap_t fields_heap;
    LLVMValueRef destructor;
    boolean_t is_union;     /* True if this is a union type */
    /* @comptime: fields — compile-time metadata, excluded from runtime layout */
    comptime_field_t ct_fields[16];
    usize_t ct_field_count;
    /* any.[T1,T2,...] type metadata */
    boolean_t   is_any_type;
    type_info_t any_variants[16];
    usize_t     any_variant_count;
    /* interface support */
    boolean_t   is_interface;   /* True if this is an interface (fat pointer type) */
    LLVMTypeRef fat_ptr_type;   /* { ptr, ptr } fat pointer type (same as llvm_type for interfaces) */
    /* destructor-freed fields (provenance tracking) */
    char *dtor_freed_fields[16]; /* field names that rem()'d by .rem method */
    usize_t dtor_freed_count;
} struct_reg_t;

/* ── enum registry ── */

typedef struct {
    char *name;
    long value;
    boolean_t has_payload;
    type_info_t payload_type;
} variant_info_t;

typedef struct {
    char *name;
    LLVMTypeRef llvm_type;   /* i32 for C-style; { i32, [N x i8] } for tagged */
    boolean_t is_tagged;     /* True if any variant carries a payload */
    variant_info_t *variants;
    usize_t variant_count;
    heap_t variants_heap;
} enum_reg_t;

/* ── lib registry ── */

typedef struct {
    char *name;       /* library name, e.g. "stdio" or "raylib" */
    char *alias;      /* namespace alias used in source, e.g. "io" */
    char *path;       /* path to .a file for custom libs; null for C stdlib headers */
    char *mod_prefix; /* mangled module prefix for Stasha modules, e.g. "net__socket";
                         NULL for C libs — determines whether to mangle call-site symbols */
} lib_entry_t;

/* ── destructor scope tracking ── */

typedef struct {
    LLVMValueRef alloca_val;
    char *struct_name;       /* non-null for struct dtors */
} dtor_var_t;

typedef struct {
    dtor_var_t *vars;
    usize_t count;
    usize_t capacity;
    heap_t heap;
    /* deferred statements (run in LIFO at scope exit) */
    node_t **deferred;
    usize_t deferred_count;
    usize_t deferred_cap;
    heap_t deferred_heap;
} dtor_scope_t;

/* ── thread wrapper cache ── */

typedef struct {
    char         *fn_name;          /* name of the target function        */
    LLVMValueRef  wrapper_fn;       /* generated __thr_wrap_<name> func   */
    LLVMTypeRef   args_struct_type; /* struct { param_types... }          */
    usize_t       param_count;
    usize_t       result_size;      /* sizeof return value (0 = void)     */
} thr_wrapper_t;

/* ── type alias registry ── */

typedef struct {
    char *name;
    type_info_t actual;
} type_alias_t;

/* ── signal dispatch (watch/send/quit) ──
 * Per-type storage for registered handlers.  Four globals per type,
 * all linkonce_odr so cross-TU storage merges. */
typedef struct {
    char *mt;                /* signal type key (e.g. "i32" or "ex_signals__sig_t") */
    LLVMValueRef data_gv;    /* ptr: heap array of fn pointers     */
    LLVMValueRef len_gv;     /* i64: current number of registered  */
    LLVMValueRef cap_gv;     /* i64: current allocated capacity    */
    LLVMValueRef lock_gv;    /* i32: atomic spinlock state         */
} signal_storage_t;

/* ── coroutine lowering context (defined before cg_t so cg_t can embed it) ── */

typedef struct {
    LLVMValueRef       handle;            /* result of llvm.coro.begin */
    LLVMValueRef       id_token;          /* result of llvm.coro.id    */
    LLVMValueRef       promise;           /* alloca ptr for promise    */
    LLVMTypeRef        promise_type;      /* %__sts_stream_prom_<T>    */
    LLVMTypeRef        hdr_type;          /* %__sts_coro_prom_hdr      */
    LLVMTypeRef        item_type;         /* T (or i8 placeholder)     */
    LLVMBasicBlockRef  final_suspend_bb;
    LLVMBasicBlockRef  cleanup_bb;
    LLVMBasicBlockRef  suspend_bb;
    int                active;
    int                susp_counter;
} sts_coro_ctx_t;

/* ── code generator state ── */

typedef struct {
    node_t *ast;            /* module root — kept for type-inference lookups */
    LLVMContextRef ctx;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMValueRef current_fn;
    LLVMValueRef printf_fn;
    LLVMTypeRef printf_type;
    LLVMValueRef fprintf_fn;
    LLVMTypeRef fprintf_type;
    LLVMValueRef stderr_var;
    LLVMValueRef malloc_fn;
    LLVMTypeRef malloc_type;
    LLVMValueRef free_fn;
    LLVMTypeRef free_type;
    LLVMValueRef realloc_fn;
    LLVMTypeRef realloc_type;

    symtab_t globals;
    symtab_t locals;

    struct_reg_t *structs;
    usize_t struct_count;
    usize_t struct_cap;
    heap_t structs_heap;

    enum_reg_t *enums;
    usize_t enum_count;
    usize_t enum_cap;
    heap_t enums_heap;

    lib_entry_t *libs;
    usize_t lib_count;
    usize_t lib_cap;
    heap_t libs_heap;

    type_alias_t *aliases;
    usize_t alias_count;
    usize_t alias_cap;
    heap_t aliases_heap;

    dtor_scope_t *dtor_stack;
    usize_t dtor_depth;
    usize_t dtor_cap;
    heap_t dtor_stack_heap;

    LLVMBasicBlockRef break_target;
    LLVMBasicBlockRef continue_target;

    linkage_t current_fn_linkage;   /* linkage of the function being compiled */
    boolean_t current_fn_is_entry_main; /* root main lowered to C entry point */
    boolean_t current_fn_is_inline_method; /* True only for methods defined inside a struct body */
    char *current_struct_name;      /* non-null when inside a struct method body */
    char current_module_prefix[512]; /* mangled prefix of module being compiled, e.g. "net__socket";
                                        empty string for root module — used by gen_call for
                                        unqualified intra-module function resolution */

    LLVMTypeRef error_type;         /* {i1, ptr} for built-in error */
    LLVMTypeRef hint_ret_type;      /* hint for C lib call return type (set by gen_local_var) */

    /* hints for make.{...} when LHS is []T — set by gen_local_var around the
     * init expression. hint_slice_elem.base == TypeVoid means "no slice ctx".
     * hint_storage is the LHS storage (StorageHeap / StorageStack / Default). */
    type_info_t hint_slice_elem;
    storage_t   hint_storage;
    int         hint_var_flags;     /* VdeclConst / VdeclFinal of LHS, when known */

    /* ── thread runtime function declarations ── */
    LLVMValueRef thread_dispatch_fn;
    LLVMTypeRef  thread_dispatch_type;
    LLVMValueRef future_get_fn;
    LLVMTypeRef  future_get_type;
    LLVMValueRef future_wait_fn;
    LLVMTypeRef  future_wait_type;
    LLVMValueRef future_ready_fn;
    LLVMTypeRef  future_ready_type;
    LLVMValueRef future_drop_fn;
    LLVMTypeRef  future_drop_type;
    LLVMValueRef future_wait_any_fn;
    LLVMTypeRef  future_wait_any_type;

    /* ── thread wrapper cache ── */
    thr_wrapper_t *thr_wrappers;
    usize_t        thr_wrap_count;
    usize_t        thr_wrap_cap;
    heap_t         thr_wrap_heap;

    boolean_t test_mode;            /* True when compiling in test mode */
    LLVMValueRef test_pass_count;   /* global i32 for test pass counter */
    LLVMValueRef test_fail_count;   /* global i32 for test fail counter */

    /* ── active coroutine lowering context ──
     * `cur_coro.active != 0` means the function currently being lowered is
     * a coroutine (task or stream).  yield/yield;/ret consult these fields
     * when they emit suspend points or branch to final-suspend. */
    sts_coro_ctx_t cur_coro;
    /* True when cur_coro is a task (CoroTask).  False for streams. */
    boolean_t      current_fn_is_async_task;

    /* ── generics / @comptime[T] ── */
    /* names of generic template struct types (not instantiated — skipped in passes) */
    char *generic_templates[64];
    node_t *generic_template_decls[64]; /* parallel AST decl nodes */
    usize_t generic_template_count;
    /* standalone generic function templates (@comptime[T] fn foo(...)) */
    char *generic_fn_templates[64];
    node_t *generic_fn_template_decls[64];
    usize_t generic_fn_template_count;
    /* active generic substitution context (set during instantiation of a generic struct) */
    char *generic_params[8];        /* formal param names, e.g. "T", "K", "V" */
    type_info_t generic_concs[8];   /* concrete types, e.g. TypeI32 */
    usize_t generic_n;              /* number of active substitutions */
    char *generic_tmpl_name;        /* template struct name, e.g. "arr_t" */
    char *generic_inst_name;        /* instantiated struct name, e.g. "arr_t_G_i32" */
    /* root AST — used by cg_generics.c to scan top-level decls */
    node_t *root_ast;

    /* ── interface registry ── */
    /* flattened method list for one interface (own + inherited) */
    struct {
        char *name;
        char *method_names[32];
        type_info_t method_ret_types[32];
        usize_t method_count;
        char *parent_names[8];
        usize_t parent_count;
        LLVMTypeRef fat_ptr_type; /* { ptr, ptr } */
    } *interfaces;
    usize_t iface_count;
    usize_t iface_cap;
    heap_t ifaces_heap;

    /* vtable for one (struct, interface) pair */
    struct {
        char *struct_name;
        char *iface_name;
        LLVMValueRef vtable_global;
        LLVMTypeRef vtable_type;
    } *vtables;
    usize_t vtable_count;
    usize_t vtable_cap;
    heap_t vtables_heap;

    /* ── DWARF debug info (active only when debug_mode is True) ── */
    boolean_t debug_mode;
    const char *source_file;        /* absolute/relative path to the source file */
    LLVMDIBuilderRef di_builder;
    LLVMMetadataRef di_file;        /* DIFile for the source file */
    LLVMMetadataRef di_compile_unit;
    LLVMMetadataRef di_scope;       /* current scope: CU at top-level, SP inside functions */
    LLVMTargetDataRef di_data_layout; /* target data layout for size/offset queries */
    di_type_entry_t *di_types;
    usize_t di_type_count;
    usize_t di_type_cap;
    heap_t di_types_heap;

    /* ── safety: unsafe block nesting level ── */
    int in_unsafe;   /* > 0 when inside unsafe { } — suppresses safety checks */

    /* ── safety: provenance tracking ── */
    /* Each heap allocation (result of new.() or zone alloc) gets a tag.
     * When rem.(p) is called, the tag is "closed".  Any subsequent load/store
     * through a pointer carrying a closed tag is a compile error. */
    struct {
        char *name;           /* variable name that holds this allocation */
        int   tag;            /* unique tag for this allocation */
        boolean_t closed;     /* True after rem.(p) was called */
        usize_t close_line;   /* line where rem was called */
    } provenance[256];
    int provenance_count;
    int next_tag;             /* monotonically increasing tag counter */

    /* ── safety: zone runtime function declarations ── */
    LLVMValueRef zone_alloc_fn;
    LLVMTypeRef  zone_alloc_type;
    LLVMValueRef zone_free_fn;
    LLVMTypeRef  zone_free_type;
    LLVMValueRef zone_move_fn;
    LLVMTypeRef  zone_move_type;

    /* ── error deduplication ── */
    /* Keys like "undefined:varname" suppress repeat diagnostics for the same
     * symbol/type/member across the compilation unit. */
    char *reported_errors[512];
    usize_t reported_error_count;

    /* ── target & fileheader state ── */
    const char *target_triple;

    /* Lifecycle blocks collected from the module for @llvm.global_ctors/dtors. */
    node_t *init_blocks[128];
    usize_t init_block_count;
    node_t *exit_blocks[128];
    usize_t exit_block_count;

    /* Module-level freestanding flag (blocks auto-stdlib/runtime linking hints). */
    boolean_t freestanding;

    /* ── signal dispatch (watch/send/quit) ── */
    signal_storage_t *signal_storages;
    usize_t           signal_storage_count;
    usize_t           signal_storage_cap;
    heap_t            signal_storage_heap;
    usize_t           signal_watcher_counter; /* unique id for synthesized handler fns */
    LLVMValueRef      watch_register_fn;      LLVMTypeRef watch_register_type;
    LLVMValueRef      watch_dereg_fn;         LLVMTypeRef watch_dereg_type;
    LLVMValueRef      watch_dispatch_fn;      LLVMTypeRef watch_dispatch_type;
    LLVMValueRef      quit_fn;                LLVMTypeRef quit_type;
    LLVMValueRef      quitting_gv;            /* i32 atomic re-entry flag */
    LLVMValueRef      exit_fn;                LLVMTypeRef exit_type;
    LLVMValueRef      underscore_exit_fn;     LLVMTypeRef underscore_exit_type;
    LLVMValueRef      fflush_fn;              LLVMTypeRef fflush_type;

    /* ── lambda lifting (sugar pack v1: non-capturing only) ── */
    int    lambda_depth;            /* > 0 while emitting a lambda body */
    char **lambda_blocked_names;    /* outer-scope local names — capture is forbidden */
    usize_t lambda_blocked_count;
    usize_t lambda_counter;         /* monotonic id for synthesised lambda fn names */
} cg_t;

/* ── helpers ── */

/* Returns True if this error key was already reported (and should be skipped).
 * On first occurrence, records the key and returns False. */
static boolean_t cg_error_already_reported(cg_t *cg, const char *key) {
    for (usize_t i = 0; i < cg->reported_error_count; i++)
        if (strcmp(cg->reported_errors[i], key) == 0) return True;
    if (cg->reported_error_count < 512)
        cg->reported_errors[cg->reported_error_count++] = strdup(key);
    return False;
}

/* check whether a struct name is a registered generic template */
static boolean_t is_generic_template(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->generic_template_count; i++)
        if (strcmp(cg->generic_templates[i], name) == 0) return True;
    return False;
}

/* gen_stmt forward declaration (emit_dtor_calls calls it for deferred stmts) */
static void gen_stmt(cg_t *cg, node_t *node);

/* forward declarations for mutual recursion in struct cleanup */
static void emit_struct_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val);
static void emit_struct_field_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val);

/* ── forward declarations ── */

static LLVMValueRef gen_expr(cg_t *cg, node_t *node);
static void gen_stmt(cg_t *cg, node_t *node);
static void gen_block(cg_t *cg, node_t *node);
static symbol_t *try_instantiate_generic_fn(cg_t *cg, const char *mangled_name);

/* DI helpers — defined after the registry helpers but called from gen_local_var
   and gen_stmt which appear earlier in the file. */
static LLVMMetadataRef get_di_type(cg_t *cg, type_info_t ti);
static LLVMMetadataRef di_make_location(cg_t *cg, usize_t line);
static void             di_set_location(cg_t *cg, usize_t line);

/* Signal-dispatch entry points (defined in cg_signals.c, which is included
   later).  Forward-declared so cg_stmt.c can dispatch NodeWatch/Send/Quit. */
static void gen_watch_stmt(cg_t *cg, node_t *node);
static void gen_send_stmt(cg_t *cg, node_t *node);
static void gen_quit_stmt(cg_t *cg, node_t *node);

#include "name_mangle.c"
#include "cg_symtab.c"
#include "cg_safety.c"
#include "cg_lookup.c"
#include "cg_dtors.c"
#include "cg_types.c"
#include "cg_coro.c"
#include "cg_registry.c"
#include "cg_interfaces.c"
#include "cg_expr.c"
#include "cg_stmt.c"
#include "cg_generics.c"
#include "cg_debug.c"
#include "cg_fileheaders.c"
#include "cg_signals.c"

/* ── top-level codegen ── */

result_t codegen(node_t *ast, const char *obj_output, boolean_t test_mode,
                 const char *target_triple, const char *source_file,
                 boolean_t debug_mode, int optimization_level) {
    usize_t errors_before = get_error_count();
    result_t emit_result = Ok;
    cg_t cg;
    memset(&cg, 0, sizeof(cg));
    cg.ast         = ast;
    cg.root_ast    = ast;
    cg.test_mode   = test_mode;
    cg.debug_mode  = debug_mode;
    cg.source_file = source_file;
    cg.target_triple = target_triple;
    if (ast && ast->kind == NodeModule) {
        cg.freestanding = ast->as.module.freestanding;
    }
    cg.ctx    = LLVMContextCreate();
    cg.module = LLVMModuleCreateWithNameInContext(ast->as.module.name, cg.ctx);
    cg.builder     = LLVMCreateBuilderInContext(cg.ctx);
    cg.current_fn  = Null;
    cg.current_struct_name = Null;
    cg.current_fn_is_inline_method = False;
    symtab_init(&cg.globals);
    symtab_init(&cg.locals);

    /* ── early target initialisation ──
     * Done here (not at emit time) so the data layout is available for DWARF
     * member offset/size queries during type-declaration passes. */
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers();

    char *early_error = Null;
    char *triple;
    if (target_triple && target_triple[0] != '\0')
        triple = LLVMCreateMessage(target_triple);
    else
        triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(cg.module, triple);

    LLVMTargetRef early_target;
    if (LLVMGetTargetFromTriple(triple, &early_target, &early_error)) {
        diag_begin_error("unknown target triple '%s': %s", triple, early_error);
        diag_finish();
        LLVMDisposeMessage(early_error);
        LLVMDisposeMessage(triple);
        LLVMContextDispose(cg.ctx);
        return Err;
    }
    LLVMCodeGenOptLevel opt_level = map_opt_level(optimization_level);
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
        early_target, triple, "generic", "",
        opt_level, LLVMRelocPIC, LLVMCodeModelDefault);

    cg.di_data_layout = LLVMCreateTargetDataLayout(machine);
    LLVMSetModuleDataLayout(cg.module, cg.di_data_layout);

    /* ── DI builder initialisation ── */
    if (cg.debug_mode && cg.source_file) {
        /* Split the path into directory + basename. */
        const char *fname = strrchr(cg.source_file, '/');
        if (!fname) fname = strrchr(cg.source_file, '\\');
        const char *basename = fname ? fname + 1 : cg.source_file;

        char dir[512];
        if (fname) {
            usize_t dir_len = (usize_t)(fname - cg.source_file);
            if (dir_len >= sizeof(dir)) dir_len = sizeof(dir) - 1;
            memcpy(dir, cg.source_file, dir_len);
            dir[dir_len] = '\0';
        } else {
            dir[0] = '.'; dir[1] = '\0';
        }

        cg.di_builder = LLVMCreateDIBuilder(cg.module);
        cg.di_file    = LLVMDIBuilderCreateFile(
            cg.di_builder,
            basename, strlen(basename),
            dir,      strlen(dir));

        cg.di_compile_unit = LLVMDIBuilderCreateCompileUnit(
            cg.di_builder,
            LLVMDWARFSourceLanguageC,  /* closest standard language */
            cg.di_file,
            "Stasha 0.1.0", 12,
            /* isOptimized= */ optimization_level != 0,
            /* Flags= */       "", 0,
            /* RuntimeVer= */  0,
            /* SplitName= */   "", 0,
            LLVMDWARFEmissionFull,
            /* DWOId= */             0,
            /* SplitDebugInlining= */ 0,
            /* DebugInfoForProfiling= */ 0,
            /* SysRoot= */ "", 0,
            /* SDK= */     "", 0);

        cg.di_scope = cg.di_compile_unit;

        /* Required module-level flags for DWARF consumers. */
        LLVMAddModuleFlag(cg.module, LLVMModuleFlagBehaviorWarning,
            "Dwarf Version", 13,
            LLVMValueAsMetadata(
                LLVMConstInt(LLVMInt32TypeInContext(cg.ctx), 4, 0)));
        LLVMAddModuleFlag(cg.module, LLVMModuleFlagBehaviorError,
            "Debug Info Version", 18,
            LLVMValueAsMetadata(
                LLVMConstInt(LLVMInt32TypeInContext(cg.ctx), 3, 0)));
    }

    /* declare C runtime functions */
    type_info_t rt_dummy = {.base=TypeVoid, .is_pointer=False, .ptr_perm=PtrNone};

    LLVMTypeRef printf_param[] = { LLVMPointerTypeInContext(cg.ctx, 0) };
    cg.printf_type = LLVMFunctionType(LLVMInt32TypeInContext(cg.ctx), printf_param, 1, 1);
    cg.printf_fn = LLVMAddFunction(cg.module, "printf", cg.printf_type);
    symtab_add(&cg.globals, "printf", cg.printf_fn, Null, rt_dummy, False);

    {
        LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg.ctx, 0);
        LLVMTypeRef fprintf_params[] = { ptr_t, ptr_t };
        cg.fprintf_type = LLVMFunctionType(LLVMInt32TypeInContext(cg.ctx), fprintf_params, 2, 1);
        cg.fprintf_fn = LLVMAddFunction(cg.module, "fprintf", cg.fprintf_type);

        /* On Apple/Darwin targets, stderr is a macro expanding to __stderrp;
           on all other platforms the symbol is stderr directly. */
        const char *stderr_sym = (strstr(triple, "apple") || strstr(triple, "darwin"))
                                 ? "__stderrp" : "stderr";
        cg.stderr_var = LLVMAddGlobal(cg.module, ptr_t, stderr_sym);
        LLVMSetLinkage(cg.stderr_var, LLVMExternalLinkage);
        LLVMSetExternallyInitialized(cg.stderr_var, 1);
    }

    LLVMTypeRef malloc_param[] = { LLVMInt64TypeInContext(cg.ctx) };
    cg.malloc_type = LLVMFunctionType(LLVMPointerTypeInContext(cg.ctx, 0), malloc_param, 1, 0);
    cg.malloc_fn = LLVMAddFunction(cg.module, "malloc", cg.malloc_type);
    symtab_add(&cg.globals, "malloc", cg.malloc_fn, Null, rt_dummy, False);

    LLVMTypeRef free_param[] = { LLVMPointerTypeInContext(cg.ctx, 0) };
    cg.free_type = LLVMFunctionType(LLVMVoidTypeInContext(cg.ctx), free_param, 1, 0);
    cg.free_fn = LLVMAddFunction(cg.module, "free", cg.free_type);
    symtab_add(&cg.globals, "free", cg.free_fn, Null, rt_dummy, False);

    LLVMTypeRef realloc_params[] = { LLVMPointerTypeInContext(cg.ctx, 0),
                                      LLVMInt64TypeInContext(cg.ctx) };
    cg.realloc_type = LLVMFunctionType(LLVMPointerTypeInContext(cg.ctx, 0),
                                        realloc_params, 2, 0);
    cg.realloc_fn = LLVMAddFunction(cg.module, "realloc", cg.realloc_type);
    symtab_add(&cg.globals, "realloc", cg.realloc_fn, Null, rt_dummy, False);

    /* ── thread runtime function declarations ── */
    {
        LLVMTypeRef ptr_t  = LLVMPointerTypeInContext(cg.ctx, 0);
        LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg.ctx);
        LLVMTypeRef i32_t  = LLVMInt32TypeInContext(cg.ctx);
        LLVMTypeRef void_t = LLVMVoidTypeInContext(cg.ctx);
        type_info_t rt_dummy2 = {.base=TypeVoid, .is_pointer=False, .ptr_perm=PtrNone};

        /* __thread_dispatch(fn_ptr, args_ptr, result_size) -> future_ptr */
        LLVMTypeRef td_params[3] = { ptr_t, ptr_t, i64_t };
        cg.thread_dispatch_type = LLVMFunctionType(ptr_t, td_params, 3, 0);
        cg.thread_dispatch_fn   = LLVMAddFunction(cg.module, "__thread_dispatch", cg.thread_dispatch_type);
        symtab_add(&cg.globals, "__thread_dispatch", cg.thread_dispatch_fn, Null, rt_dummy2, False);

        /* __future_get(future_ptr) -> void_ptr */
        LLVMTypeRef fg_params[1] = { ptr_t };
        cg.future_get_type = LLVMFunctionType(ptr_t, fg_params, 1, 0);
        cg.future_get_fn   = LLVMAddFunction(cg.module, "__future_get", cg.future_get_type);
        symtab_add(&cg.globals, "__future_get", cg.future_get_fn, Null, rt_dummy2, False);

        /* __future_wait(future_ptr) -> void */
        LLVMTypeRef fw_params[1] = { ptr_t };
        cg.future_wait_type = LLVMFunctionType(void_t, fw_params, 1, 0);
        cg.future_wait_fn   = LLVMAddFunction(cg.module, "__future_wait", cg.future_wait_type);
        symtab_add(&cg.globals, "__future_wait", cg.future_wait_fn, Null, rt_dummy2, False);

        /* __future_ready(future_ptr) -> i32 */
        LLVMTypeRef fr_params[1] = { ptr_t };
        cg.future_ready_type = LLVMFunctionType(i32_t, fr_params, 1, 0);
        cg.future_ready_fn   = LLVMAddFunction(cg.module, "__future_ready", cg.future_ready_type);
        symtab_add(&cg.globals, "__future_ready", cg.future_ready_fn, Null, rt_dummy2, False);

        /* __future_drop(future_ptr) -> void */
        LLVMTypeRef fdr_params[1] = { ptr_t };
        cg.future_drop_type = LLVMFunctionType(void_t, fdr_params, 1, 0);
        cg.future_drop_fn   = LLVMAddFunction(cg.module, "__future_drop", cg.future_drop_type);
        symtab_add(&cg.globals, "__future_drop", cg.future_drop_fn, Null, rt_dummy2, False);

        /* __future_wait_any(future_ptr *, i32) -> i32 */
        LLVMTypeRef fwa_params[2] = { ptr_t, i32_t };
        cg.future_wait_any_type = LLVMFunctionType(i32_t, fwa_params, 2, 0);
        cg.future_wait_any_fn   = LLVMAddFunction(cg.module, "__future_wait_any", cg.future_wait_any_type);
        symtab_add(&cg.globals, "__future_wait_any", cg.future_wait_any_fn, Null, rt_dummy2, False);
    }

    /* ── zone runtime function declarations ──
     * __zone_alloc(zone_ptr, size) -> void_ptr
     * __zone_free(zone_ptr)
     * __zone_move(zone_ptr, ptr, size) -> void_ptr  (copy out of zone, independent alloc) */
    {
        LLVMTypeRef ptr_t  = LLVMPointerTypeInContext(cg.ctx, 0);
        LLVMTypeRef i64_t  = LLVMInt64TypeInContext(cg.ctx);
        LLVMTypeRef void_t = LLVMVoidTypeInContext(cg.ctx);
        type_info_t rt_z   = {.base=TypeVoid, .is_pointer=False, .ptr_perm=PtrNone};

        LLVMTypeRef za_params[2] = { ptr_t, i64_t };
        cg.zone_alloc_type = LLVMFunctionType(ptr_t, za_params, 2, 0);
        cg.zone_alloc_fn   = LLVMAddFunction(cg.module, "__zone_alloc", cg.zone_alloc_type);
        symtab_add(&cg.globals, "__zone_alloc", cg.zone_alloc_fn, Null, rt_z, False);

        LLVMTypeRef zf_params[1] = { ptr_t };
        cg.zone_free_type = LLVMFunctionType(void_t, zf_params, 1, 0);
        cg.zone_free_fn   = LLVMAddFunction(cg.module, "__zone_free", cg.zone_free_type);
        symtab_add(&cg.globals, "__zone_free", cg.zone_free_fn, Null, rt_z, False);

        LLVMTypeRef zm_params[3] = { ptr_t, ptr_t, i64_t };
        cg.zone_move_type = LLVMFunctionType(ptr_t, zm_params, 3, 0);
        cg.zone_move_fn   = LLVMAddFunction(cg.module, "__zone_move", cg.zone_move_type);
        symtab_add(&cg.globals, "__zone_move", cg.zone_move_fn, Null, rt_z, False);
    }

    /* built-in error type: { i1 has_error, ptr message } */
    {
        LLVMTypeRef err_fields[2] = {
            LLVMInt1TypeInContext(cg.ctx),
            LLVMPointerTypeInContext(cg.ctx, 0)
        };
        cg.error_type = LLVMStructTypeInContext(cg.ctx, err_fields, 2, 0);
    }

    /* test mode: create global pass/fail counters */
    if (cg.test_mode) {
        cg.test_pass_count = LLVMAddGlobal(cg.module, LLVMInt32TypeInContext(cg.ctx), "__test_pass");
        LLVMSetInitializer(cg.test_pass_count, LLVMConstInt(LLVMInt32TypeInContext(cg.ctx), 0, 0));
        cg.test_fail_count = LLVMAddGlobal(cg.module, LLVMInt32TypeInContext(cg.ctx), "__test_fail");
        LLVMSetInitializer(cg.test_fail_count, LLVMConstInt(LLVMInt32TypeInContext(cg.ctx), 0, 0));
    }

    /* pre-pass: flatten NodeSubMod wrappers.
       Register each submod as a module alias so greeter.fn() / greeter:fn()
       resolve correctly, then splice its children into the top-level decl list
       (children already have module_name set by the parser). */
    {
        node_list_t flat;
        node_list_init(&flat);
        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            node_t *d = ast->as.module.decls.items[i];
            if (d->kind == NodeSubMod) {
                /* register submod name as a module alias (like imp does) */
                char pfx[512];
                mangle_module_prefix(d->as.submod.name, pfx, sizeof(pfx));
                register_lib(&cg, d->as.submod.name, d->as.submod.name, Null);
                if (cg.lib_count > 0)
                    cg.libs[cg.lib_count - 1].mod_prefix = ast_strdup(pfx, strlen(pfx));
                /* splice children */
                for (usize_t j = 0; j < d->as.submod.decls.count; j++)
                    node_list_push(&flat, d->as.submod.decls.items[j]);
            } else {
                node_list_push(&flat, d);
            }
        }
        ast->as.module.decls = flat;
    }

    /* pass 0: register type declarations, lib declarations */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];

        if (decl->kind == NodeLib) {
            register_lib(&cg, decl->as.lib_decl.name,
                         decl->as.lib_decl.alias, decl->as.lib_decl.path);
            continue;
        }

        /* libimp "name" from ...: register as a Stasha module alias so that
           qualified calls like "json.parse()" resolve via the mangled symtab
           entry rather than being treated as a plain C extern declaration. */
        if (decl->kind == NodeLibImp) {
            register_lib(&cg, decl->as.libimp_decl.name,
                         decl->as.libimp_decl.name, decl->as.libimp_decl.path);
            if (cg.lib_count > 0) {
                char pfx[512];
                mangle_module_prefix(decl->as.libimp_decl.name, pfx, sizeof(pfx));
                cg.libs[cg.lib_count - 1].mod_prefix = ast_strdup(pfx, strlen(pfx));
            }
            continue;
        }

        /* imp a.b.c: register the last segment as a module alias so that
           qualified calls like "typewriter.scribe()" resolve correctly.
           We also store the mangled module prefix (e.g. "printer__typewriter")
           so gen_method_call can mangle Stasha symbols rather than
           auto-declaring them as C externs. */
        if (decl->kind == NodeImpDecl) {
            const char *mod = decl->as.imp_decl.module_name;
            const char *dot = strrchr(mod, '.');
            const char *alias = dot ? dot + 1 : mod;

            /* allow "imp mod = alias" syntax: the AST stores alias in module_name
               only when explicitly provided; fall back to last segment otherwise */
            char mod_pfx_buf[512];
            mangle_module_prefix(mod, mod_pfx_buf, sizeof(mod_pfx_buf));
            char *mod_pfx_copy = ast_strdup(mod_pfx_buf, strlen(mod_pfx_buf));

            register_lib(&cg, alias, alias, Null);
            /* set mod_prefix on the entry we just added */
            if (cg.lib_count > 0)
                cg.libs[cg.lib_count - 1].mod_prefix = mod_pfx_copy;
            continue;
        }

        if (decl->kind == NodeTypeDecl) {
            /* @[[if: ...]] / @[[require: ...]] gating */
            if (cg_fh_skip_decl(&cg, decl->headers, decl->line))
                continue;
            if (decl->as.type_decl.decl_kind == TypeDeclInterface) {
                /* Register interface as a fat-pointer struct { ptr, ptr } */
                char llvm_type_name[512];
                mangle_type(decl->module_name, decl->as.type_decl.name,
                            llvm_type_name, sizeof(llvm_type_name));
                LLVMTypeRef ptr_t = LLVMPointerTypeInContext(cg.ctx, 0);
                LLVMTypeRef fat_fields[2] = { ptr_t, ptr_t };
                /* Use a named struct so each interface has a distinct LLVMTypeRef —
                   anonymous literal structs are deduplicated by LLVM across all interfaces */
                LLVMTypeRef fat_type = LLVMStructCreateNamed(cg.ctx, llvm_type_name);
                LLVMStructSetBody(fat_type, fat_fields, 2, 0);
                register_struct(&cg, decl->as.type_decl.name, fat_type, False);
                if (cg.struct_count > 0) {
                    cg.structs[cg.struct_count - 1].is_interface = True;
                    cg.structs[cg.struct_count - 1].fat_ptr_type = fat_type;
                    if (decl->module_name) {
                        char pfx[512];
                        mangle_module_prefix(decl->module_name, pfx, sizeof(pfx));
                        cg.structs[cg.struct_count - 1].mod_prefix =
                            ast_strdup(pfx, strlen(pfx));
                    }
                }
                /* Register in interface registry */
                register_interface(&cg, decl);
            } else if (decl->as.type_decl.decl_kind == TypeDeclStruct
                || decl->as.type_decl.decl_kind == TypeDeclUnion) {

                /* @comptime[T] generic template structs — register name but skip LLVM type */
                if (decl->as.type_decl.type_param_count > 0) {
                    if (cg.generic_template_count < 64) {
                        cg.generic_templates[cg.generic_template_count] =
                            decl->as.type_decl.name;
                        cg.generic_template_decls[cg.generic_template_count] = decl;
                        cg.generic_template_count++;
                    }
                    continue;
                }

                /* Use mangled LLVM type name to avoid collisions across modules */
                char llvm_type_name[512];
                mangle_type(decl->module_name, decl->as.type_decl.name,
                            llvm_type_name, sizeof(llvm_type_name));
                LLVMTypeRef stype = LLVMStructCreateNamed(cg.ctx, llvm_type_name);
                register_struct(&cg, decl->as.type_decl.name, stype,
                                decl->as.type_decl.decl_kind == TypeDeclUnion);
                /* store module prefix on the struct entry for method lookup */
                if (cg.struct_count > 0 && decl->module_name) {
                    char pfx[512];
                    mangle_module_prefix(decl->module_name, pfx, sizeof(pfx));
                    cg.structs[cg.struct_count - 1].mod_prefix =
                        ast_strdup(pfx, strlen(pfx));
                }
            } else if (decl->as.type_decl.decl_kind == TypeDeclEnum) {
                register_enum(&cg, decl->as.type_decl.name,
                              &decl->as.type_decl.variants);
            } else if (decl->as.type_decl.decl_kind == TypeDeclAlias) {
                register_alias(&cg, decl->as.type_decl.name, decl->as.type_decl.alias_type);
            }
        }
    }

    /* pass 0b: set struct/union bodies (now that all types are registered) */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeTypeDecl) continue;
        if (decl->as.type_decl.decl_kind != TypeDeclStruct
            && decl->as.type_decl.decl_kind != TypeDeclUnion)
            continue;

        /* skip generic template structs — they have no concrete LLVM type */
        if (decl->as.type_decl.type_param_count > 0) continue;

        struct_reg_t *sr = find_struct(&cg, decl->as.type_decl.name);
        if (!sr) continue;

        boolean_t is_packed = (decl->as.type_decl.attr_flags & AttrPacked) != 0
                            || (decl->as.type_decl.attr_flags & AttrCLayout) != 0;

        if (decl->as.type_decl.decl_kind == TypeDeclUnion) {
            /* Union: single body = byte array of largest member size */
            usize_t fc = decl->as.type_decl.fields.count;
            usize_t max_size = 0;
            for (usize_t j = 0; j < fc; j++) {
                node_t *field = decl->as.type_decl.fields.items[j];
                type_info_t fti = resolve_alias(&cg, field->as.var_decl.type);
                usize_t sz = payload_type_size(fti);
                if (sz > max_size) max_size = sz;
                /* all union fields map to index 0 (they overlap) */
                struct_add_field(sr, field->as.var_decl.name, fti, 0,
                                 field->as.var_decl.linkage,
                                 field->as.var_decl.storage);
            }
            if (max_size == 0) max_size = 1;
            LLVMTypeRef body = LLVMArrayType2(LLVMInt8TypeInContext(cg.ctx),
                                               (unsigned long long)max_size);
            LLVMStructSetBody(sr->llvm_type, &body, 1, is_packed ? 1 : 0);
        } else {
            /* Struct: field layout with bitfield packing */
            usize_t fc = decl->as.type_decl.fields.count;
            LLVMTypeRef *field_types = Null;
            heap_t ft_heap = NullHeap;
            usize_t llvm_field_count = 0;

            /* first pass: collect @comptime: fields */
            for (usize_t j = 0; j < fc; j++) {
                node_t *field = decl->as.type_decl.fields.items[j];
                if (!(field->as.var_decl.flags & VdeclComptimeField)) continue;
                if (sr->ct_field_count >= 16) continue;
                long val = 0;
                if (field->as.var_decl.init && field->as.var_decl.init->kind == NodeIntLitExpr)
                    val = field->as.var_decl.init->as.int_lit.value;
                sr->ct_fields[sr->ct_field_count].name = field->as.var_decl.name;
                sr->ct_fields[sr->ct_field_count].value = val;
                sr->ct_field_count++;
            }

            if (fc > 0) {
                ft_heap = allocate(fc, sizeof(LLVMTypeRef));
                field_types = ft_heap.pointer;
                usize_t j = 0;
                while (j < fc) {
                    node_t *field = decl->as.type_decl.fields.items[j];

                    /* skip @comptime: fields — they have no runtime layout */
                    if (field->as.var_decl.flags & VdeclComptimeField) { j++; continue; }

                    int bw = field->as.var_decl.bitfield_width;
                    if (bw > 0) {
                        /* start of a bitfield group — pack consecutive bitfields
                           of the same base type into one backing integer */
                        type_info_t base_ti = resolve_alias(&cg, field->as.var_decl.type);
                        LLVMTypeRef backing_type = get_llvm_type(&cg, base_ti);
                        usize_t backing_idx = llvm_field_count;
                        int bit_pos = 0;

                        while (j < fc) {
                            node_t *bf = decl->as.type_decl.fields.items[j];
                            int w = bf->as.var_decl.bitfield_width;
                            if (w <= 0) break; /* not a bitfield, stop grouping */
                            type_info_t bfti = resolve_alias(&cg, bf->as.var_decl.type);
                            struct_add_field_ex(sr, bf->as.var_decl.name, bfti,
                                                backing_idx,
                                                bf->as.var_decl.linkage,
                                                bf->as.var_decl.storage,
                                                bit_pos, w);
                            bit_pos += w;
                            j++;
                        }
                        field_types[llvm_field_count++] = backing_type;
                    } else {
                        /* normal (non-bitfield) field */
                        type_info_t fti = resolve_alias(&cg, field->as.var_decl.type);
                        int _fndim = (field->as.var_decl.flags & VdeclArray)
                                     ? (field->as.var_decl.array_ndim > 0
                                        ? field->as.var_decl.array_ndim : 1)
                                     : 0;
                        if (_fndim > 0) {
                            /* Build nested LLVM array type from innermost to outermost. */
                            LLVMTypeRef _ft = get_llvm_type(&cg, fti);
                            for (int _d = _fndim - 1; _d >= 0; _d--)
                                _ft = LLVMArrayType2(_ft,
                                          (unsigned long long)field->as.var_decl.array_sizes[_d]);
                            field_types[llvm_field_count] = _ft;
                        } else {
                            field_types[llvm_field_count] = get_llvm_type(&cg, fti);
                        }
                        struct_add_field(sr, field->as.var_decl.name, fti,
                                         llvm_field_count,
                                         field->as.var_decl.linkage,
                                         field->as.var_decl.storage);
                        /* store outermost dimension in field_info for GEP/decay */
                        if (_fndim > 0 && sr->field_count > 0)
                            sr->fields[sr->field_count - 1].array_size =
                                field->as.var_decl.array_sizes[0];
                        llvm_field_count++;
                        j++;
                    }
                }
            }
            LLVMStructSetBody(sr->llvm_type, field_types, (unsigned)llvm_field_count,
                              is_packed ? 1 : 0);
            if (fc > 0) deallocate(ft_heap);
        }

        /* alignment attribute */
        if (decl->as.type_decl.align_value > 0) {
            /* LLVM doesn't directly set alignment on struct type;
               alignment is applied per-variable. We store it for later use. */
        }
    }

    /* early abort: type registration errors make function codegen unreliable */
    if (get_error_count() > errors_before) goto cg_cleanup;

    /* pass 1: forward-declare all globals and functions */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];

        /* comptime_if at top level: conditionally include declarations */
        if (decl->kind == NodeComptimeIf) {
            /* evaluate and emit the block's declarations inline */
            /* (handled in pass 2 via gen_stmt, but at top level we skip for now) */
            continue;
        }
        if (decl->kind == NodeComptimeAssert) {
            /* deferred to pass 2 where we have the data layout */
            continue;
        }

        if (decl->kind == NodeVarDecl) {
            /* library-backed internal globals live in the .a — skip */
            if (decl->from_lib && decl->as.var_decl.linkage == LinkageInternal)
                continue;

            /* @[[if: ...]] / @[[require: ...]] gating */
            if (cg_fh_skip_decl(&cg, decl->headers, decl->line))
                continue;

            type_info_t ti = resolve_alias(&cg, decl->as.var_decl.type);
            LLVMTypeRef type = get_llvm_type(&cg, ti);
            /* Globals declared as fixed-size arrays (e.g. `extern int32_t
             * buf[8];` from cheader) must be laid out as LLVM array types
             * so indexing/sizeof do the right thing.  Mirror the wrapping
             * already done for fn params and struct fields. */
            if (decl->as.var_decl.flags & VdeclArray) {
                int _nd = decl->as.var_decl.array_ndim > 0
                          ? decl->as.var_decl.array_ndim : 1;
                for (int _d = _nd - 1; _d >= 0; _d--)
                    type = LLVMArrayType2(type,
                        (unsigned long long)decl->as.var_decl.array_sizes[_d]);
            }
            /* mangle the LLVM symbol name for imported-module globals */
            char var_llvm_name[512];
            mangle_global(decl->is_c_extern ? Null : decl->module_name,
                          decl->as.var_decl.name,
                          var_llvm_name, sizeof(var_llvm_name));
            /* fileheader: @[[export_name: "..."]] / @[[abi: c]] override */
            char override_name[512];
            boolean_t abi_c = False;
            if (cg_fh_override_symbol(decl->headers, decl->as.var_decl.name,
                                      override_name, sizeof(override_name), &abi_c)) {
                strncpy(var_llvm_name, override_name, sizeof(var_llvm_name) - 1);
                var_llvm_name[sizeof(var_llvm_name) - 1] = '\0';
            }
            LLVMValueRef global = LLVMAddGlobal(cg.module, type, var_llvm_name);

            /* linkage: int → internal, ext → external (default) */
            if (decl->as.var_decl.linkage == LinkageInternal)
                LLVMSetLinkage(global, LLVMInternalLinkage);

            /* @weak attribute */
            if (decl->as.var_decl.attr_flags & AttrWeak)
                LLVMSetLinkage(global, LLVMWeakAnyLinkage);
            /* @hidden attribute */
            if (decl->as.var_decl.attr_flags & AttrHidden)
                LLVMSetVisibility(global, LLVMHiddenVisibility);

            /* fileheader: section/align/weak/hidden */
            cg_fh_apply_to_global(&cg, global, decl->headers);

            /* const/final globals are LLVM-constant */
            if (decl->as.var_decl.flags & (VdeclConst | VdeclFinal))
                LLVMSetGlobalConstant(global, 1);

            /* thread-local storage */
            if (decl->as.var_decl.flags & VdeclTls)
                LLVMSetThreadLocal(global, 1);

            /* library-backed ext globals: no initializer — linker resolves from .a */
            if (decl->from_lib) {
                LLVMSetExternallyInitialized(global, 1);
            } else {
                /* initializer — must be a constant expression */
                LLVMValueRef init_val = LLVMConstNull(type);
                if (decl->as.var_decl.init) {
                    node_t *iv = decl->as.var_decl.init;
                    if (iv->kind == NodeIntLitExpr)
                        init_val = LLVMConstInt(type,
                            (unsigned long long)iv->as.int_lit.value, 1);
                    else if (iv->kind == NodeFloatLitExpr)
                        init_val = LLVMConstReal(type, iv->as.float_lit.value);
                    else if (iv->kind == NodeBoolLitExpr)
                        init_val = LLVMConstInt(type, iv->as.bool_lit.value, 0);
                    else if (iv->kind == NodeCharLitExpr)
                        init_val = LLVMConstInt(type, (unsigned char)iv->as.char_lit.value, 0);
                    else if (iv->kind == NodeNilExpr)
                        init_val = LLVMConstNull(type);
                    else if (iv->kind == NodeStrLitExpr)
                        init_val = LLVMConstNull(type); /* string globals handled below */
                }
                LLVMSetInitializer(global, init_val);
            }

            int sym_flags = 0;
            if (decl->as.var_decl.flags & VdeclAtomic)   sym_flags |= SymAtomic;
            if (decl->as.var_decl.flags & VdeclVolatile)  sym_flags |= SymVolatile;
            if (decl->as.var_decl.flags & VdeclTls)       sym_flags |= SymTls;
            /* store under the mangled name so lookups by alias prefix work.
               ast_strdup is required: var_llvm_name is a stack-local buffer that
               gets reused on each loop iteration — without a copy, all entries
               would point to the same address (containing only the last name). */
            symtab_add(&cg.globals, ast_strdup(var_llvm_name, strlen(var_llvm_name)),
                       global, type, ti, sym_flags);
            symtab_set_last_storage(&cg.globals, decl->as.var_decl.storage, False);
            symtab_set_last_extra(&cg.globals, decl->as.var_decl.flags & VdeclConst,
                                  decl->as.var_decl.flags & VdeclFinal, decl->as.var_decl.linkage,
                                  0, -1); /* scope_depth 0 = global lifetime */

            /* ── DI: global variable expression ── */
            if (cg.debug_mode && cg.di_builder) {
                LLVMMetadataRef di_gtype = get_di_type(&cg, ti);
                LLVMMetadataRef di_expr  =
                    LLVMDIBuilderCreateExpression(cg.di_builder, Null, 0);
                boolean_t local_to_unit =
                    (decl->as.var_decl.linkage == LinkageInternal);
                LLVMMetadataRef gv_expr =
                    LLVMDIBuilderCreateGlobalVariableExpression(
                        cg.di_builder,
                        cg.di_compile_unit,
                        decl->as.var_decl.name,
                        strlen(decl->as.var_decl.name),
                        "", 0,          /* linkage name */
                        cg.di_file,
                        (unsigned)decl->line,
                        di_gtype,
                        local_to_unit,
                        di_expr,
                        Null, 0);       /* no declaration, no align override */
                LLVMGlobalSetMetadata(global,
                    LLVMGetMDKindIDInContext(cg.ctx, "dbg", 3),
                    gv_expr);
            }
        }

        /* global zone declaration: zone name; at module level */
        if (decl->kind == NodeZoneStmt) {
            const char *zname = decl->as.zone_stmt.name;
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg.ctx, 0);
            LLVMValueRef gz = LLVMAddGlobal(cg.module, ptr_ty, zname);
            LLVMSetInitializer(gz, LLVMConstNull(ptr_ty));
            type_info_t zone_ti = NO_TYPE;
            zone_ti.base = TypeZone;
            symtab_add(&cg.globals, ast_strdup(zname, strlen(zname)), gz, ptr_ty, zone_ti, SymZone);
            symtab_set_last_storage(&cg.globals, StorageStack, False);
            symtab_set_last_extra(&cg.globals, False, False, LinkageNone, 0, -1);
            continue;
        }

        /* Collect @[[init]] / @[[exit]] blocks for global_ctors/global_dtors. */
        if (decl->kind == NodeInitBlock) {
            if (cg.init_block_count < 128)
                cg.init_blocks[cg.init_block_count++] = decl;
            continue;
        }
        if (decl->kind == NodeExitBlock) {
            if (cg.exit_block_count < 128)
                cg.exit_blocks[cg.exit_block_count++] = decl;
            continue;
        }

        if (decl->kind == NodeFnDecl) {
            /* library-backed internal functions live in the .a — skip */
            if (decl->from_lib && decl->as.fn_decl.linkage == LinkageInternal)
                continue;

            /* @[[if: ...]] / @[[require: ...]] gating */
            if (cg_fh_skip_decl(&cg, decl->headers, decl->line))
                continue;

            /* skip generic template functions (methods of generic structs) */
            if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name
                    && is_generic_template(&cg, decl->as.fn_decl.struct_name))
                continue;
            /* standalone generic functions: register template, generate on demand */
            if (decl->as.fn_decl.type_param_count > 0 && !decl->as.fn_decl.is_method) {
                if (cg.generic_fn_template_count < 64) {
                    cg.generic_fn_templates[cg.generic_fn_template_count] =
                        decl->as.fn_decl.name;
                    cg.generic_fn_template_decls[cg.generic_fn_template_count] = decl;
                    cg.generic_fn_template_count++;
                }
                continue;
            }
            /* method-level @comptime[T] params: generated on demand by instantiate_method */
            if (decl->as.fn_decl.type_param_count > 0) continue;

            /* build the LLVM symbol name: mangle for imported Stasha modules,
               leave raw for root-module and C extern symbols */
            char fn_name[512];
            const char *fn_module = decl->is_c_extern ? Null : decl->module_name;
            if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name) {
                mangle_method(fn_module, decl->as.fn_decl.struct_name,
                              decl->as.fn_decl.name, fn_name, sizeof(fn_name));
            } else {
                mangle_fn(fn_module, decl->as.fn_decl.name, fn_name, sizeof(fn_name));
            }
            /* fileheader: @[[export_name: "..."]] / @[[abi: c]] override */
            {
                char override_name[512];
                boolean_t abi_c = False;
                if (cg_fh_override_symbol(decl->headers, decl->as.fn_decl.name,
                                          override_name, sizeof(override_name), &abi_c)) {
                    strncpy(fn_name, override_name, sizeof(fn_name) - 1);
                    fn_name[sizeof(fn_name) - 1] = '\0';
                }
            }

            /* in test mode, skip the user's main — we generate our own */
            if (cg.test_mode && strcmp(decl->as.fn_decl.name, "main") == 0
                    && decl->module_name == Null) continue;

            /* build param types */
            usize_t pc = decl->as.fn_decl.params.count;
            boolean_t is_instance = decl->as.fn_decl.is_method
                && strcmp(decl->as.fn_decl.name, "new") != 0;
            usize_t total_params = pc + (is_instance ? 1 : 0);

            LLVMTypeRef *ptypes = Null;
            heap_t ptypes_heap = NullHeap;
            if (total_params > 0) {
                ptypes_heap = allocate(total_params, sizeof(LLVMTypeRef));
                ptypes = ptypes_heap.pointer;
                usize_t offset = 0;
                if (is_instance) {
                    ptypes[0] = LLVMPointerTypeInContext(cg.ctx, 0);
                    offset = 1;
                }
                for (usize_t j = 0; j < pc; j++) {
                    node_t *_p = decl->as.fn_decl.params.items[j];
                    type_info_t pti = resolve_alias(&cg, _p->as.var_decl.type);
                    LLVMTypeRef _pt = get_llvm_type(&cg, pti);
                    if (_p->as.var_decl.flags & VdeclArray) {
                        int _nd = _p->as.var_decl.array_ndim > 0 ? _p->as.var_decl.array_ndim : 1;
                        for (int _d = _nd - 1; _d >= 0; _d--)
                            _pt = LLVMArrayType2(_pt, (unsigned long long)_p->as.var_decl.array_sizes[_d]);
                    }
                    ptypes[j + offset] = _pt;
                }
            }

            /* return type */
            LLVMTypeRef ret_type;
            /* is_main: only the root module's "main" function is the C entry point */
            boolean_t is_main = (decl->module_name == Null)
                             && strcmp(decl->as.fn_decl.name, "main") == 0;
            /* Coroutines (task or stream) lower to a function returning a
               coroutine handle (raw `ptr`).  The declared T appears as the
               result/item type only inside the promise. */
            boolean_t is_coro_decl = decl->as.fn_decl.is_async
                && (decl->as.fn_decl.coro_flavor == CoroTask
                    || decl->as.fn_decl.coro_flavor == CoroStream);
            if (is_coro_decl) {
                ret_type = LLVMPointerTypeInContext(cg.ctx, 0);
            } else if (is_main) {
                ret_type = LLVMInt32TypeInContext(cg.ctx);
            } else if (decl->as.fn_decl.return_count > 1) {
                /* multi-return: create an anonymous struct type */
                heap_t rt_heap = allocate(decl->as.fn_decl.return_count, sizeof(LLVMTypeRef));
                LLVMTypeRef *rt_fields = rt_heap.pointer;
                for (usize_t j = 0; j < decl->as.fn_decl.return_count; j++) {
                    type_info_t rti = resolve_alias(&cg, decl->as.fn_decl.return_types[j]);
                    rt_fields[j] = get_llvm_type(&cg, rti);
                }
                ret_type = LLVMStructTypeInContext(cg.ctx, rt_fields,
                    (unsigned)decl->as.fn_decl.return_count, 0);
                deallocate(rt_heap);
            } else {
                type_info_t rti = resolve_alias(&cg, decl->as.fn_decl.return_types[0]);
                ret_type = get_llvm_type(&cg, rti);
            }

            LLVMTypeRef fn_type = LLVMFunctionType(ret_type, ptypes,
                (unsigned)total_params, decl->as.fn_decl.is_variadic ? 1 : 0);
            LLVMValueRef fn = LLVMAddFunction(cg.module, fn_name, fn_type);
            if (decl->as.fn_decl.linkage == LinkageInternal)
                LLVMSetLinkage(fn, LLVMInternalLinkage);

            /* @weak / @hidden attributes on functions */
            if (decl->as.fn_decl.attr_flags & AttrWeak)
                LLVMSetLinkage(fn, LLVMWeakAnyLinkage);
            if (decl->as.fn_decl.attr_flags & AttrHidden)
                LLVMSetVisibility(fn, LLVMHiddenVisibility);

            /* fileheader-based fn attributes: section/align/weak/target/features */
            cg_fh_apply_to_fn(&cg, fn, decl->headers);

            /* restrict pointer params → noalias attribute */
            for (usize_t j = 0; j < pc; j++) {
                node_t *param = decl->as.fn_decl.params.items[j];
                if ((param->as.var_decl.flags & VdeclRestrict)
                    && param->as.var_decl.type.is_pointer) {
                    unsigned pidx = (unsigned)(j + (is_instance ? 1 : 0));
                    LLVMValueRef pval = LLVMGetParam(fn, pidx);
                    unsigned attr_kind = LLVMGetEnumAttributeKindForName("noalias", 7);
                    LLVMAttributeRef attr = LLVMCreateEnumAttribute(cg.ctx, attr_kind, 0);
                    LLVMAddAttributeAtIndex(fn, pidx + 1, attr); /* 1-based for params */
                    (void)pval;
                }
            }

            type_info_t dummy = {.base=TypeVoid, .is_pointer=False, .ptr_perm=PtrNone};
            symtab_add(&cg.globals, ast_strdup(fn_name, strlen(fn_name)), fn, Null, dummy, False);

            if (total_params > 0) deallocate(ptypes_heap);

            /* register destructor */
            if (decl->as.fn_decl.is_method && strcmp(decl->as.fn_decl.name, "rem") == 0
                && decl->as.fn_decl.struct_name) {
                struct_reg_t *sr = find_struct(&cg, decl->as.fn_decl.struct_name);
                if (sr) sr->destructor = fn;
            }
        }
    }

    /* also forward-declare inline struct/union methods */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeTypeDecl) continue;
        if (decl->as.type_decl.decl_kind != TypeDeclStruct
            && decl->as.type_decl.decl_kind != TypeDeclUnion)
            continue;
        /* skip generic template structs */
        if (decl->as.type_decl.type_param_count > 0) continue;
        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];

            /* library-backed internal inline methods live in the .a — skip */
            if (decl->from_lib && method->as.fn_decl.linkage == LinkageInternal)
                continue;
            char fn_name[512];
            /* interface-qualified method: fn flyable_i.move() inside duck_t */
            if (method->as.fn_decl.iface_qualifier &&
                strcmp(method->as.fn_decl.iface_qualifier, decl->as.type_decl.name) != 0) {
                /* mangle as "struct.iface.method" or "mod__struct__iface__method" */
                const char *mod = decl->module_name;
                if (mod && mod[0]) {
                    char pfx[512];
                    mangle_module_prefix(mod, pfx, sizeof(pfx));
                    snprintf(fn_name, sizeof(fn_name), "%s__%s__%s__%s",
                             pfx, decl->as.type_decl.name,
                             method->as.fn_decl.iface_qualifier,
                             method->as.fn_decl.name);
                } else {
                    snprintf(fn_name, sizeof(fn_name), "%s.%s.%s",
                             decl->as.type_decl.name,
                             method->as.fn_decl.iface_qualifier,
                             method->as.fn_decl.name);
                }
            } else {
                mangle_method(decl->module_name, decl->as.type_decl.name,
                              method->as.fn_decl.name, fn_name, sizeof(fn_name));
            }

            /* all inline methods are instance methods: get implicit this */
            usize_t pc = method->as.fn_decl.params.count;
            usize_t total_params = pc + 1;
            heap_t ptypes_heap = allocate(total_params, sizeof(LLVMTypeRef));
            LLVMTypeRef *ptypes = ptypes_heap.pointer;
            ptypes[0] = LLVMPointerTypeInContext(cg.ctx, 0);
            for (usize_t j = 0; j < pc; j++) {
                type_info_t pti = resolve_alias(&cg,
                    method->as.fn_decl.params.items[j]->as.var_decl.type);
                ptypes[j + 1] = get_llvm_type(&cg, pti);
            }

            LLVMTypeRef ret_type;
            boolean_t method_is_coro = method->as.fn_decl.is_async
                && (method->as.fn_decl.coro_flavor == CoroTask
                    || method->as.fn_decl.coro_flavor == CoroStream);
            if (method_is_coro) {
                ret_type = LLVMPointerTypeInContext(cg.ctx, 0);
            } else if (method->as.fn_decl.return_count > 1) {
                heap_t rt_heap = allocate(method->as.fn_decl.return_count, sizeof(LLVMTypeRef));
                LLVMTypeRef *rt_fields = rt_heap.pointer;
                for (usize_t j = 0; j < method->as.fn_decl.return_count; j++) {
                    type_info_t rti = resolve_alias(&cg, method->as.fn_decl.return_types[j]);
                    rt_fields[j] = get_llvm_type(&cg, rti);
                }
                ret_type = LLVMStructTypeInContext(cg.ctx, rt_fields,
                    (unsigned)method->as.fn_decl.return_count, 0);
                deallocate(rt_heap);
            } else {
                type_info_t rti = resolve_alias(&cg, method->as.fn_decl.return_types[0]);
                ret_type = get_llvm_type(&cg, rti);
            }

            LLVMTypeRef fn_type = LLVMFunctionType(ret_type, ptypes,
                (unsigned)total_params, 0);
            LLVMValueRef fn = LLVMAddFunction(cg.module, fn_name, fn_type);
            if (method->as.fn_decl.linkage == LinkageInternal)
                LLVMSetLinkage(fn, LLVMInternalLinkage);

            type_info_t dummy = {.base=TypeVoid, .is_pointer=False, .ptr_perm=PtrNone};
            symtab_add(&cg.globals, ast_strdup(fn_name, strlen(fn_name)), fn, Null, dummy, False);
            deallocate(ptypes_heap);

            if (strcmp(method->as.fn_decl.name, "rem") == 0) {
                struct_reg_t *sr = find_struct(&cg, decl->as.type_decl.name);
                if (sr) sr->destructor = fn;
            }
        }
    }

    /* pass 1b: generate vtable globals for structs implementing interfaces.
       All functions are forward-declared in pass 1, so we can now create vtables. */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeTypeDecl) continue;
        if (decl->as.type_decl.decl_kind != TypeDeclStruct) continue;
        if (decl->as.type_decl.impl_iface_count == 0) continue;
        if (decl->as.type_decl.type_param_count > 0) continue; /* skip generic templates */
        for (usize_t j = 0; j < decl->as.type_decl.impl_iface_count; j++) {
            ensure_vtable(&cg, decl->as.type_decl.name,
                          decl->as.type_decl.impl_ifaces[j]);
        }
    }

    /* early abort: declaration errors make body generation unreliable */
    if (get_error_count() > errors_before) goto cg_cleanup;

    /* pass 2: generate function bodies */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeFnDecl) continue;

        /* library-backed functions: body lives in the .a — skip codegen */
        if (decl->from_lib) continue;

        /* skip generic template functions */
        if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name
                && is_generic_template(&cg, decl->as.fn_decl.struct_name))
            continue;
        if (decl->as.fn_decl.type_param_count > 0) continue;

        /* @[[if: ...]] / @[[require: ...]] gating — skip body if decl elided */
        if (cg_fh_skip_decl(&cg, decl->headers, decl->line))
            continue;

        /* body-less extern declaration — forward decl only, nothing to emit */
        if (decl->as.fn_decl.body == Null) continue;

        /* rebuild the mangled name the same way pass 1 did */
        char fn_name[512];
        const char *fn_module2 = decl->is_c_extern ? Null : decl->module_name;
        if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name)
            mangle_method(fn_module2, decl->as.fn_decl.struct_name,
                          decl->as.fn_decl.name, fn_name, sizeof(fn_name));
        else
            mangle_fn(fn_module2, decl->as.fn_decl.name, fn_name, sizeof(fn_name));
        {
            char override_name[512];
            boolean_t abi_c = False;
            if (cg_fh_override_symbol(decl->headers, decl->as.fn_decl.name,
                                      override_name, sizeof(override_name), &abi_c)) {
                strncpy(fn_name, override_name, sizeof(fn_name) - 1);
                fn_name[sizeof(fn_name) - 1] = '\0';
            }
        }

        /* in test mode, skip the user's main — we generate our own */
        if (cg.test_mode && strcmp(decl->as.fn_decl.name, "main") == 0
                && decl->module_name == Null) continue;

        /* track which module we are currently compiling so gen_call can
           resolve unqualified intra-module calls correctly */
        mangle_module_prefix(decl->module_name ? decl->module_name : "",
                             cg.current_module_prefix,
                             sizeof(cg.current_module_prefix));

        symbol_t *sym = cg_lookup(&cg, fn_name);
        cg.current_fn = sym->value;
        cg.current_fn_linkage = decl->as.fn_decl.linkage;
        cg.current_fn_is_entry_main = (decl->module_name == Null)
                                   && strcmp(decl->as.fn_decl.name, "main") == 0;
        cg.current_struct_name = decl->as.fn_decl.is_method ? decl->as.fn_decl.struct_name : Null;
        cg.current_fn_is_inline_method = False; /* external fn decl — this keyword is not allowed */
        cg.locals.count = 0;
        cg.dtor_depth = 0;

        LLVMBasicBlockRef entry =
            LLVMAppendBasicBlockInContext(cg.ctx, cg.current_fn, "entry");
        LLVMPositionBuilderAtEnd(cg.builder, entry);

        boolean_t is_instance = decl->as.fn_decl.is_method
            && strcmp(decl->as.fn_decl.name, "new") != 0;
        usize_t param_offset = 0;

        /* ── DI: create DISubprogram for this function ── */
        if (cg.debug_mode && cg.di_builder) {
            usize_t nparam = decl->as.fn_decl.params.count;
            /* subroutine type: [return_type, param0_type, ...] */
            usize_t total_di = nparam + 1 + (is_instance ? 1 : 0);
            heap_t  di_pt_heap = allocate(total_di, sizeof(LLVMMetadataRef));
            LLVMMetadataRef *di_ptypes = di_pt_heap.pointer;

            /* index 0 = return type */
            di_ptypes[0] = get_di_type(&cg, decl->as.fn_decl.return_types[0]);
            usize_t di_pi = 1;
            if (is_instance && decl->as.fn_decl.struct_name) {
                type_info_t thi = {.base=TypeUser, .user_name=decl->as.fn_decl.struct_name, .is_pointer=True, .ptr_perm=PtrReadWrite, .ptr_depth=1, .ptr_perms={PtrReadWrite}};
                di_ptypes[di_pi++] = get_di_type(&cg, thi);
            }
            for (usize_t j = 0; j < nparam; j++) {
                type_info_t pti =
                    resolve_alias(&cg, decl->as.fn_decl.params.items[j]->as.var_decl.type);
                di_ptypes[di_pi++] = get_di_type(&cg, pti);
            }

            LLVMMetadataRef di_func_ty = LLVMDIBuilderCreateSubroutineType(
                cg.di_builder, cg.di_file,
                di_ptypes, (unsigned)total_di,
                LLVMDIFlagZero);
            deallocate(di_pt_heap);

            boolean_t is_local = (decl->as.fn_decl.linkage == LinkageInternal);
            LLVMMetadataRef di_sp = LLVMDIBuilderCreateFunction(
                cg.di_builder,
                cg.di_compile_unit,
                fn_name, strlen(fn_name),      /* display name */
                fn_name, strlen(fn_name),      /* linkage name */
                cg.di_file,
                (unsigned)decl->line,
                di_func_ty,
                is_local ? 1 : 0,              /* isLocalToUnit */
                /* isDefinition= */ 1,
                (unsigned)decl->line,          /* scope line */
                LLVMDIFlagZero,
                /* isOptimized= */ 0);

            LLVMSetSubprogram(cg.current_fn, di_sp);
            cg.di_scope = di_sp;

            /* Seed the builder with the function's opening line so that
               the prologue allocas carry a valid location. */
            di_set_location(&cg, decl->line);
        }

        /* implicit this parameter for instance methods */
        if (is_instance && decl->as.fn_decl.struct_name) {
            struct_reg_t *sr = find_struct(&cg, decl->as.fn_decl.struct_name);
            LLVMTypeRef this_type = sr ? LLVMPointerTypeInContext(cg.ctx, 0)
                                       : LLVMPointerTypeInContext(cg.ctx, 0);
            LLVMValueRef this_alloca = LLVMBuildAlloca(cg.builder, this_type, "this");
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, 0), this_alloca);
            type_info_t this_ti = {.base=TypeUser, .user_name=decl->as.fn_decl.struct_name, .is_pointer=True, .ptr_perm=PtrReadWrite, .ptr_depth=1, .ptr_perms={PtrReadWrite}};
            symtab_add(&cg.locals, "this", this_alloca, this_type, this_ti, False);
            param_offset = 1;
        }

        for (usize_t j = 0; j < decl->as.fn_decl.params.count; j++) {
            node_t *param = decl->as.fn_decl.params.items[j];
            type_info_t pti = resolve_alias(&cg, param->as.var_decl.type);
            LLVMTypeRef ptype = get_llvm_type(&cg, pti);
            if (param->as.var_decl.flags & VdeclArray) {
                int _nd = param->as.var_decl.array_ndim > 0 ? param->as.var_decl.array_ndim : 1;
                for (int _d = _nd - 1; _d >= 0; _d--)
                    ptype = LLVMArrayType2(ptype, (unsigned long long)param->as.var_decl.array_sizes[_d]);
            }
            LLVMValueRef alloca_val = LLVMBuildAlloca(cg.builder, ptype,
                                                       param->as.var_decl.name);
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, (unsigned)(j + param_offset)),
                           alloca_val);
            {
                int param_flags = 0;
                if (pti.base == TypeZone) param_flags |= SymZone;
                symtab_add(&cg.locals, param->as.var_decl.name, alloca_val, ptype,
                           pti, param_flags);
            }

            /* ── DI: parameter variable ── */
            if (cg.debug_mode && cg.di_builder && cg.di_scope) {
                LLVMMetadataRef di_pty = get_di_type(&cg, pti);
                LLVMMetadataRef di_param = LLVMDIBuilderCreateParameterVariable(
                    cg.di_builder,
                    cg.di_scope,
                    param->as.var_decl.name, strlen(param->as.var_decl.name),
                    (unsigned)(j + param_offset + 1), /* 1-based arg number */
                    cg.di_file,
                    (unsigned)(param->line ? param->line : decl->line),
                    di_pty,
                    /* alwaysPreserve= */ 1,
                    LLVMDIFlagZero);
                LLVMMetadataRef di_expr =
                    LLVMDIBuilderCreateExpression(cg.di_builder, Null, 0);
                LLVMMetadataRef di_loc =
                    di_make_location(&cg, param->line ? param->line : decl->line);
                LLVMDIBuilderInsertDeclareRecordAtEnd(
                    cg.di_builder,
                    alloca_val, di_param, di_expr, di_loc,
                    LLVMGetInsertBlock(cg.builder));
            }
        }

        /* Coroutine bodies: stream coroutines wrap with the streaming
           prologue (item slot = T item, is_stream=1); task coroutines wrap
           with the task prologue (item slot = T result, is_stream=0).
           `gen_yield_*` and `gen_ret` consult cg.cur_coro to branch into
           the suspend/cleanup blocks built here. */
        boolean_t is_stream_coro = decl->as.fn_decl.is_async
            && decl->as.fn_decl.coro_flavor == CoroStream;
        boolean_t is_task_coro   = decl->as.fn_decl.is_async
            && decl->as.fn_decl.coro_flavor == CoroTask;
        sts_coro_ctx_t prev_coro = cg.cur_coro;
        boolean_t      prev_is_task = cg.current_fn_is_async_task;
        cg.current_fn_is_async_task = False;
        if (is_stream_coro) {
            sts_emit_coro_stream_prologue(&cg, decl->as.fn_decl.yield_type, &cg.cur_coro);
        } else if (is_task_coro) {
            type_info_t rt = decl->as.fn_decl.return_count > 0
                ? decl->as.fn_decl.return_types[0] : NO_TYPE;
            sts_emit_coro_task_prologue(&cg, rt, &cg.cur_coro);
            cg.current_fn_is_async_task = True;
        } else {
            cg.cur_coro.active = 0;
        }

        gen_block(&cg, decl->as.fn_decl.body);

        /* Leak check: warn on any future.[T] local never passed to
           await / future.* / a combinator / defer. */
        check_unconsumed_futures(&cg, decl);

        LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg.builder);
        if (is_stream_coro || is_task_coro) {
            sts_finish_coro_body_if_open(&cg, &cg.cur_coro);
            cg.cur_coro = prev_coro;
            cg.current_fn_is_async_task = prev_is_task;
            if (cg.debug_mode) cg.di_scope = cg.di_compile_unit;
            continue;
        }
        cg.cur_coro = prev_coro;
        cg.current_fn_is_async_task = prev_is_task;
        if (!LLVMGetBasicBlockTerminator(cur_bb)) {
            type_info_t rti = decl->as.fn_decl.return_types[0];
            if (cg.current_fn_is_entry_main)
                LLVMBuildRet(cg.builder, LLVMConstInt(LLVMInt32TypeInContext(cg.ctx), 0, 0));
            else if (rti.base == TypeVoid && !rti.is_pointer)
                LLVMBuildRetVoid(cg.builder);
            else {
                /* Missing return: function may not return a value on all paths */
                diag_begin_optional_warning(WarnMissingReturn,
                    "function '%s' may not return a value on all paths",
                    decl->as.fn_decl.name);
                diag_set_category(ErrCatOther);
                diag_span(DIAG_NODE(decl), True,
                          "declared to return a non-void type");
                diag_help("add explicit 'ret' statements on all code paths");
                diag_finish();
                LLVMBuildRet(cg.builder,
                    LLVMConstNull(get_llvm_type(&cg, rti)));
            }
        }

        /* Restore scope to compile unit after leaving the function. */
        if (cg.debug_mode)
            cg.di_scope = cg.di_compile_unit;
    }

    /* Thread-pool wrappers were used by the legacy `async fn` dispatch.
       After the v2 migration, every `async fn` is a real coroutine — the
       function itself returns a `future.[T]` / `stream.[T]` handle directly.
       The thread runtime stays linked for explicit `thread.(fn)(args)`,
       which still goes through `get_or_create_thread_wrapper` lazily on its
       first call site, so we no longer eagerly materialize wrappers here. */

    /* handle top-level comptime_assert (now that data layout is available) */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind == NodeComptimeAssert)
            gen_comptime_assert(&cg, decl);
    }

    /* also generate inline struct/union method bodies */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeTypeDecl) continue;
        if (decl->as.type_decl.decl_kind != TypeDeclStruct
            && decl->as.type_decl.decl_kind != TypeDeclUnion)
            continue;

        /* library-backed: all method bodies live in the .a — skip */
        if (decl->from_lib) continue;

        /* skip generic template structs — methods generated lazily */
        if (decl->as.type_decl.type_param_count > 0) continue;

        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];
            char fn_name[512];
            /* interface-qualified method: fn flyable_i.move() inside duck_t */
            if (method->as.fn_decl.iface_qualifier &&
                strcmp(method->as.fn_decl.iface_qualifier, decl->as.type_decl.name) != 0) {
                const char *mod = decl->module_name;
                if (mod && mod[0]) {
                    char pfx[512];
                    mangle_module_prefix(mod, pfx, sizeof(pfx));
                    snprintf(fn_name, sizeof(fn_name), "%s__%s__%s__%s",
                             pfx, decl->as.type_decl.name,
                             method->as.fn_decl.iface_qualifier,
                             method->as.fn_decl.name);
                } else {
                    snprintf(fn_name, sizeof(fn_name), "%s.%s.%s",
                             decl->as.type_decl.name,
                             method->as.fn_decl.iface_qualifier,
                             method->as.fn_decl.name);
                }
            } else {
                mangle_method(decl->module_name, decl->as.type_decl.name,
                              method->as.fn_decl.name, fn_name, sizeof(fn_name));
            }

            symbol_t *sym = cg_lookup(&cg, fn_name);
            if (!sym) continue;
            cg.current_fn = sym->value;
            cg.current_struct_name = decl->as.type_decl.name;
            cg.current_fn_is_inline_method = True; /* inline struct method — this keyword allowed */
            mangle_module_prefix(decl->module_name ? decl->module_name : "",
                                 cg.current_module_prefix,
                                 sizeof(cg.current_module_prefix));
            cg.locals.count = 0;
            cg.dtor_depth = 0;

            LLVMBasicBlockRef entry =
                LLVMAppendBasicBlockInContext(cg.ctx, cg.current_fn, "entry");
            LLVMPositionBuilderAtEnd(cg.builder, entry);

            /* ── DI: DISubprogram for inline method ── */
            if (cg.debug_mode && cg.di_builder) {
                usize_t nparam = method->as.fn_decl.params.count;
                usize_t total_di = nparam + 2; /* ret + this + params */
                heap_t  di_pt_heap = allocate(total_di, sizeof(LLVMMetadataRef));
                LLVMMetadataRef *di_ptypes = di_pt_heap.pointer;
                di_ptypes[0] = get_di_type(&cg, method->as.fn_decl.return_types[0]);
                type_info_t thi = {.base=TypeUser, .user_name=decl->as.type_decl.name, .is_pointer=True, .ptr_perm=PtrReadWrite, .ptr_depth=1, .ptr_perms={PtrReadWrite}};
                di_ptypes[1] = get_di_type(&cg, thi);
                for (usize_t j = 0; j < nparam; j++) {
                    type_info_t pti = resolve_alias(&cg,
                        method->as.fn_decl.params.items[j]->as.var_decl.type);
                    di_ptypes[j + 2] = get_di_type(&cg, pti);
                }
                LLVMMetadataRef di_func_ty = LLVMDIBuilderCreateSubroutineType(
                    cg.di_builder, cg.di_file,
                    di_ptypes, (unsigned)total_di, LLVMDIFlagZero);
                deallocate(di_pt_heap);

                LLVMMetadataRef di_sp = LLVMDIBuilderCreateFunction(
                    cg.di_builder, cg.di_compile_unit,
                    fn_name, strlen(fn_name),
                    fn_name, strlen(fn_name),
                    cg.di_file,
                    (unsigned)method->line,
                    di_func_ty,
                    /* isLocalToUnit= */ 1,
                    /* isDefinition=  */ 1,
                    (unsigned)method->line,
                    LLVMDIFlagZero,
                    /* isOptimized= */ 0);
                LLVMSetSubprogram(cg.current_fn, di_sp);
                cg.di_scope = di_sp;
                di_set_location(&cg, method->line);
            }

            /* implicit this */
            struct_reg_t *sr = find_struct(&cg, decl->as.type_decl.name);
            LLVMTypeRef this_type = LLVMPointerTypeInContext(cg.ctx, 0);
            LLVMValueRef this_alloca = LLVMBuildAlloca(cg.builder, this_type, "this");
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, 0), this_alloca);
            type_info_t this_ti = {.base=TypeUser, .user_name=decl->as.type_decl.name, .is_pointer=True, .ptr_perm=PtrReadWrite, .ptr_depth=1, .ptr_perms={PtrReadWrite}};
            symtab_add(&cg.locals, "this", this_alloca, this_type, this_ti, False);
            (void)sr;

            for (usize_t j = 0; j < method->as.fn_decl.params.count; j++) {
                node_t *param = method->as.fn_decl.params.items[j];
                type_info_t pti = resolve_alias(&cg, param->as.var_decl.type);
                LLVMTypeRef ptype = get_llvm_type(&cg, pti);
                if (param->as.var_decl.flags & VdeclArray) {
                    int _nd = param->as.var_decl.array_ndim > 0 ? param->as.var_decl.array_ndim : 1;
                    for (int _d = _nd - 1; _d >= 0; _d--)
                        ptype = LLVMArrayType2(ptype, (unsigned long long)param->as.var_decl.array_sizes[_d]);
                }
                LLVMValueRef alloca_val = LLVMBuildAlloca(cg.builder, ptype,
                                                           param->as.var_decl.name);
                LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, (unsigned)(j + 1)),
                               alloca_val);
                {
                    int param_flags = 0;
                    if (pti.base == TypeZone) param_flags |= SymZone;
                    symtab_add(&cg.locals, param->as.var_decl.name, alloca_val, ptype,
                               pti, param_flags);
                }

                /* DI: parameter */
                if (cg.debug_mode && cg.di_builder && cg.di_scope) {
                    LLVMMetadataRef di_pty = get_di_type(&cg, pti);
                    LLVMMetadataRef di_p = LLVMDIBuilderCreateParameterVariable(
                        cg.di_builder, cg.di_scope,
                        param->as.var_decl.name, strlen(param->as.var_decl.name),
                        (unsigned)(j + 2), /* 1-based; slot 1 = this */
                        cg.di_file,
                        (unsigned)(param->line ? param->line : method->line),
                        di_pty, 1, LLVMDIFlagZero);
                    LLVMMetadataRef di_expr =
                        LLVMDIBuilderCreateExpression(cg.di_builder, Null, 0);
                    LLVMMetadataRef di_loc =
                        di_make_location(&cg, param->line ? param->line : method->line);
                    LLVMDIBuilderInsertDeclareRecordAtEnd(
                        cg.di_builder, alloca_val, di_p, di_expr, di_loc,
                        LLVMGetInsertBlock(cg.builder));
                }
            }

            /* Coroutine methods: same prologue/epilogue wrap as a top-level
               coroutine; cur_coro tells gen_yield/gen_ret to branch into
               the coroutine cleanup blocks. */
            boolean_t method_is_stream_coro = method->as.fn_decl.is_async
                && method->as.fn_decl.coro_flavor == CoroStream;
            boolean_t method_is_task_coro = method->as.fn_decl.is_async
                && method->as.fn_decl.coro_flavor == CoroTask;
            sts_coro_ctx_t prev_method_coro = cg.cur_coro;
            boolean_t      prev_method_is_task = cg.current_fn_is_async_task;
            cg.current_fn_is_async_task = False;
            if (method_is_stream_coro) {
                sts_emit_coro_stream_prologue(&cg,
                    method->as.fn_decl.yield_type, &cg.cur_coro);
            } else if (method_is_task_coro) {
                type_info_t rt = method->as.fn_decl.return_count > 0
                    ? method->as.fn_decl.return_types[0] : NO_TYPE;
                sts_emit_coro_task_prologue(&cg, rt, &cg.cur_coro);
                cg.current_fn_is_async_task = True;
            } else {
                cg.cur_coro.active = 0;
            }

            gen_block(&cg, method->as.fn_decl.body);
            check_unconsumed_futures(&cg, method);

            LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg.builder);
            if (method_is_stream_coro || method_is_task_coro) {
                sts_finish_coro_body_if_open(&cg, &cg.cur_coro);
                cg.cur_coro = prev_method_coro;
            } else if (!LLVMGetBasicBlockTerminator(cur_bb)) {
                type_info_t rti = method->as.fn_decl.return_types[0];
                if (rti.base == TypeVoid && !rti.is_pointer)
                    LLVMBuildRetVoid(cg.builder);
                else
                    LLVMBuildRet(cg.builder,
                        LLVMConstNull(get_llvm_type(&cg, rti)));
            }
            cg.cur_coro = prev_method_coro;
            cg.current_fn_is_async_task = prev_method_is_task;

            if (cg.debug_mode)
                cg.di_scope = cg.di_compile_unit;
        }
    }

    /* pass 3: generate test blocks (test mode only) */
    if (cg.test_mode) {
        /* count test blocks */
        usize_t test_count = 0;
        for (usize_t i = 0; i < ast->as.module.decls.count; i++)
            if (ast->as.module.decls.items[i]->kind == NodeTestBlock)
                test_count++;

        /* forward-declare test functions */
        LLVMValueRef *test_fns = Null;
        char **test_names = Null;
        heap_t tf_heap = NullHeap, tn_heap = NullHeap;
        if (test_count > 0) {
            tf_heap = allocate(test_count, sizeof(LLVMValueRef));
            test_fns = tf_heap.pointer;
            tn_heap = allocate(test_count, sizeof(char *));
            test_names = tn_heap.pointer;
        }
        usize_t ti = 0;
        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            node_t *decl = ast->as.module.decls.items[i];
            if (decl->kind != NodeTestBlock) continue;
            char fn_name[256];
            snprintf(fn_name, sizeof(fn_name), "__test_%lu", ti);
            LLVMTypeRef fn_type = LLVMFunctionType(LLVMVoidTypeInContext(cg.ctx), Null, 0, 0);
            LLVMValueRef fn = LLVMAddFunction(cg.module, fn_name, fn_type);
            LLVMSetLinkage(fn, LLVMInternalLinkage);
            test_fns[ti] = fn;
            test_names[ti] = decl->as.test_block.name;
            ti++;
        }

        /* generate test function bodies */
        ti = 0;
        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            node_t *decl = ast->as.module.decls.items[i];
            if (decl->kind != NodeTestBlock) continue;
            cg.current_fn = test_fns[ti];
            cg.locals.count = 0;
            cg.dtor_depth = 0;
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg.ctx, cg.current_fn, "entry");
            LLVMPositionBuilderAtEnd(cg.builder, entry);
            gen_block(&cg, decl->as.test_block.body);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg.builder)))
                LLVMBuildRetVoid(cg.builder);
            ti++;
        }

        /* generate test main: calls each test, prints results */
        {
            LLVMTypeRef main_type = LLVMFunctionType(LLVMInt32TypeInContext(cg.ctx), Null, 0, 0);
            LLVMValueRef main_fn = LLVMAddFunction(cg.module, "main", main_type);
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg.ctx, main_fn, "entry");
            LLVMPositionBuilderAtEnd(cg.builder, entry);

            for (usize_t t = 0; t < test_count; t++) {
                /* print test name */
                char banner[256];
                snprintf(banner, sizeof(banner), "test '%s' ... ", test_names[t]);
                LLVMValueRef bstr = LLVMBuildGlobalStringPtr(cg.builder, banner, "tbanner");
                LLVMValueRef bargs[1] = { bstr };
                LLVMBuildCall2(cg.builder, cg.printf_type, cg.printf_fn, bargs, 1, "");

                /* save fail count before test */
                LLVMValueRef pre_fail = LLVMBuildLoad2(cg.builder,
                    LLVMInt32TypeInContext(cg.ctx), cg.test_fail_count, "pf");

                /* call test function */
                LLVMTypeRef test_ft = LLVMFunctionType(LLVMVoidTypeInContext(cg.ctx), Null, 0, 0);
                LLVMBuildCall2(cg.builder, test_ft, test_fns[t], Null, 0, "");

                /* check if fail count increased */
                LLVMValueRef post_fail = LLVMBuildLoad2(cg.builder,
                    LLVMInt32TypeInContext(cg.ctx), cg.test_fail_count, "qf");
                LLVMValueRef passed = LLVMBuildICmp(cg.builder, LLVMIntEQ, pre_fail, post_fail, "ok");
                LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(cg.ctx, main_fn, "tok");
                LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(cg.ctx, main_fn, "tfail");
                LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(cg.ctx, main_fn, "tnext");
                LLVMBuildCondBr(cg.builder, passed, ok_bb, fail_bb);

                LLVMPositionBuilderAtEnd(cg.builder, ok_bb);
                LLVMValueRef ok_str = LLVMBuildGlobalStringPtr(cg.builder, "PASS\n", "pstr");
                LLVMValueRef ok_args[1] = { ok_str };
                LLVMBuildCall2(cg.builder, cg.printf_type, cg.printf_fn, ok_args, 1, "");
                LLVMBuildBr(cg.builder, next_bb);

                LLVMPositionBuilderAtEnd(cg.builder, fail_bb);
                LLVMValueRef fl_str = LLVMBuildGlobalStringPtr(cg.builder, "FAIL\n", "fstr");
                LLVMValueRef fl_args[1] = { fl_str };
                LLVMBuildCall2(cg.builder, cg.printf_type, cg.printf_fn, fl_args, 1, "");
                LLVMBuildBr(cg.builder, next_bb);

                LLVMPositionBuilderAtEnd(cg.builder, next_bb);
            }

            /* print summary */
            LLVMValueRef final_pass = LLVMBuildLoad2(cg.builder,
                LLVMInt32TypeInContext(cg.ctx), cg.test_pass_count, "fp");
            LLVMValueRef final_fail = LLVMBuildLoad2(cg.builder,
                LLVMInt32TypeInContext(cg.ctx), cg.test_fail_count, "ff");
            LLVMValueRef sum_fmt = LLVMBuildGlobalStringPtr(cg.builder,
                "\n%d passed, %d failed\n", "sfmt");
            LLVMValueRef sargs[3] = { sum_fmt, final_pass, final_fail };
            LLVMBuildCall2(cg.builder, cg.printf_type, cg.printf_fn, sargs, 3, "");

            /* return fail count (0 = success) */
            LLVMValueRef exit_code = LLVMBuildICmp(cg.builder, LLVMIntNE, final_fail,
                LLVMConstInt(LLVMInt32TypeInContext(cg.ctx), 0, 0), "hasf");
            LLVMValueRef ret = LLVMBuildZExt(cg.builder, exit_code,
                LLVMInt32TypeInContext(cg.ctx), "ec");
            LLVMBuildRet(cg.builder, ret);
        }

        if (test_count > 0) {
            deallocate(tf_heap);
            deallocate(tn_heap);
        }
    }

    /* ── @[[init]] / @[[exit]] lifecycle blocks → global_ctors/global_dtors ── */
    cg_emit_lifecycle_blocks(&cg,
                             cg.init_blocks, cg.init_block_count,
                             cg.exit_blocks, cg.exit_block_count);

    /* ── DI: finalize before verification ──
     * Must happen after all IR is emitted and before the module is verified,
     * so that unresolved DI forward-references are resolved first. */
    if (cg.debug_mode && cg.di_builder) {
        LLVMDIBuilderFinalize(cg.di_builder);
        LLVMDisposeDIBuilder(cg.di_builder);
        cg.di_builder = Null;
    }

    /* verify */
    /* ── LLVM verification and emission (skipped when earlier passes had errors) ── */
    if (get_error_count() > errors_before) goto cg_cleanup;

    char *error = Null;
    if (LLVMVerifyModule(cg.module, LLVMReturnStatusAction, &error)) {
        diag_begin_error("LLVM IR verification failed: %s", error);
        diag_finish();
        LLVMDisposeMessage(error);
    } else {
        if (error) LLVMDisposeMessage(error);
    }

    if (getenv("STS_DUMP_IR_PRE")) {
        /* Dump IR before the coroutine pass pipeline runs. Useful when
         * triaging an "Instruction does not dominate all uses" verifier
         * abort that fires inside the coro passes. */
        char *ir_str = LLVMPrintModuleToString(cg.module);
        FILE *f = fopen(getenv("STS_DUMP_IR_PRE"), "w");
        if (f) { fputs(ir_str, f); fclose(f); }
        LLVMDisposeMessage(ir_str);
    }

    if (get_error_count() == errors_before && ast_requires_coro_pipeline(ast)) {
        LLVMPassBuilderOptionsRef pb_opts = LLVMCreatePassBuilderOptions();
        LLVMPassBuilderOptionsSetVerifyEach(pb_opts, 1);
        LLVMErrorRef pass_err = LLVMRunPasses(
            cg.module,
            "coro-early,cgscc(coro-split),coro-cleanup,globaldce",
            machine,
            pb_opts);
        LLVMDisposePassBuilderOptions(pb_opts);
        if (pass_err) {
            char *msg = LLVMGetErrorMessage(pass_err);
            diag_begin_error("LLVM coroutine pass pipeline failed: %s", msg ? msg : "unknown error");
            diag_finish();
            if (msg) LLVMDisposeErrorMessage(msg);
            LLVMConsumeError(pass_err);
            emit_result = Err;
            goto cg_cleanup;
        }
    }

    /* debug: dump IR if STS_DUMP_IR env var is set */
    if (getenv("STS_DUMP_IR")) {
        char *ir_str = LLVMPrintModuleToString(cg.module);
        FILE *f = fopen(getenv("STS_DUMP_IR"), "w");
        if (f) { fputs(ir_str, f); fclose(f); }
        LLVMDisposeMessage(ir_str);
    }

    /* emit object file — reuse the machine created during early target init */
    error = Null;
    emit_result = Ok;
    if (LLVMTargetMachineEmitToFile(machine, cg.module, (char *)obj_output,
                                     LLVMObjectFile, &error)) {
        diag_begin_error("object file emission failed: %s", error);
        diag_finish();
        LLVMDisposeMessage(error);
        emit_result = Err;
    }

cg_cleanup:
    LLVMDisposeTargetMachine(machine);
    LLVMDisposeMessage(triple);
    if (cg.di_data_layout) LLVMDisposeTargetData(cg.di_data_layout);

    LLVMDisposeBuilder(cg.builder);
    LLVMDisposeModule(cg.module);
    LLVMContextDispose(cg.ctx);

    symtab_free(&cg.globals);
    symtab_free(&cg.locals);

    /* error dedup strings (strdup'd) are intentionally not freed here;
     * they are small and the process is about to exit. */

    /* free registries */
    for (usize_t i = 0; i < cg.struct_count; i++)
        if (cg.structs[i].fields_heap.pointer) deallocate(cg.structs[i].fields_heap);
    if (cg.structs_heap.pointer) deallocate(cg.structs_heap);
    for (usize_t i = 0; i < cg.enum_count; i++)
        if (cg.enums[i].variants_heap.pointer) deallocate(cg.enums[i].variants_heap);
    if (cg.enums_heap.pointer) deallocate(cg.enums_heap);
    if (cg.libs_heap.pointer) deallocate(cg.libs_heap);
    if (cg.aliases_heap.pointer) deallocate(cg.aliases_heap);
    if (cg.dtor_stack_heap.pointer) deallocate(cg.dtor_stack_heap);
    if (cg.di_types_heap.pointer) deallocate(cg.di_types_heap);
    if (cg.thr_wrap_heap.pointer) deallocate(cg.thr_wrap_heap);
    if (cg.signal_storage_heap.pointer) deallocate(cg.signal_storage_heap);

    if (emit_result != Ok) return Err;
    return get_error_count() > errors_before ? Err : Ok;
}
