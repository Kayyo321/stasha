#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/DebugInfo.h>

#include <string.h>
#include <stdio.h>
#include "codegen.h"

/* ── DI type cache ── */

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
} field_info_t;

typedef struct {
    char *name;
    LLVMTypeRef llvm_type;
    field_info_t *fields;
    usize_t field_count;
    usize_t field_capacity;
    heap_t fields_heap;
    LLVMValueRef destructor;
    boolean_t is_union;     /* True if this is a union type */
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
    char *name;     /* library name, e.g. "stdio" or "raylib" */
    char *alias;    /* namespace alias used in source, e.g. "io" */
    char *path;     /* path to .a file for custom libs; null for C stdlib headers */
} lib_entry_t;

/* ── destructor scope tracking ── */

typedef struct {
    LLVMValueRef alloca_val;
    char *struct_name;      /* non-null for struct dtors */
    boolean_t is_heap_alloc; /* True for heap primitive auto-free */
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

/* ── type alias registry ── */

typedef struct {
    char *name;
    type_info_t actual;
} type_alias_t;

/* ── code generator state ── */

typedef struct {
    node_t *ast;            /* module root — kept for type-inference lookups */
    LLVMContextRef ctx;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMValueRef current_fn;
    LLVMValueRef printf_fn;
    LLVMTypeRef printf_type;
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
    char *current_struct_name;      /* non-null when inside a struct method body */

    LLVMTypeRef error_type;         /* {i1, ptr} for built-in error */
    boolean_t test_mode;            /* True when compiling in test mode */
    LLVMValueRef test_pass_count;   /* global i32 for test pass counter */
    LLVMValueRef test_fail_count;   /* global i32 for test fail counter */

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
} cg_t;

/* gen_stmt forward declaration (emit_dtor_calls calls it for deferred stmts) */
static void gen_stmt(cg_t *cg, node_t *node);

/* forward declarations for mutual recursion in struct cleanup */
static void emit_struct_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val);
static void emit_struct_field_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val);

/* ── forward declarations ── */

static LLVMValueRef gen_expr(cg_t *cg, node_t *node);
static void gen_stmt(cg_t *cg, node_t *node);
static void gen_block(cg_t *cg, node_t *node);

/* DI helpers — defined after the registry helpers but called from gen_local_var
   and gen_stmt which appear earlier in the file. */
static LLVMMetadataRef get_di_type(cg_t *cg, type_info_t ti);
static LLVMMetadataRef di_make_location(cg_t *cg, usize_t line);
static void             di_set_location(cg_t *cg, usize_t line);

#include "cg_symtab.c"
#include "cg_safety.c"
#include "cg_lookup.c"
#include "cg_dtors.c"
#include "cg_types.c"
#include "cg_expr.c"
#include "cg_stmt.c"
#include "cg_registry.c"
#include "cg_debug.c"

/* ── top-level codegen ── */

result_t codegen(node_t *ast, const char *obj_output, boolean_t test_mode,
                 const char *target_triple, const char *source_file,
                 boolean_t debug_mode) {
    cg_t cg;
    memset(&cg, 0, sizeof(cg));
    cg.ast         = ast;
    cg.test_mode   = test_mode;
    cg.debug_mode  = debug_mode;
    cg.source_file = source_file;
    cg.ctx    = LLVMContextCreate();
    cg.module = LLVMModuleCreateWithNameInContext(ast->as.module.name, cg.ctx);
    cg.builder     = LLVMCreateBuilderInContext(cg.ctx);
    cg.current_fn  = Null;
    cg.current_struct_name = Null;
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
        log_err("target lookup failed: %s", early_error);
        LLVMDisposeMessage(early_error);
        LLVMDisposeMessage(triple);
        LLVMContextDispose(cg.ctx);
        return Err;
    }
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
        early_target, triple, "generic", "",
        LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);

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
            /* isOptimized= */ 0,
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
    type_info_t rt_dummy = {TypeVoid, Null, False, PtrNone, Null};

    LLVMTypeRef printf_param[] = { LLVMPointerTypeInContext(cg.ctx, 0) };
    cg.printf_type = LLVMFunctionType(LLVMInt32TypeInContext(cg.ctx), printf_param, 1, 1);
    cg.printf_fn = LLVMAddFunction(cg.module, "printf", cg.printf_type);
    symtab_add(&cg.globals, "printf", cg.printf_fn, Null, rt_dummy, False);

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

    /* pass 0: register type declarations, lib declarations */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];

        if (decl->kind == NodeLib) {
            register_lib(&cg, decl->as.lib_decl.name,
                         decl->as.lib_decl.alias, decl->as.lib_decl.path);
            continue;
        }

        /* libimp "name" from ...: register as both lib alias and module alias */
        if (decl->kind == NodeLibImp) {
            register_lib(&cg, decl->as.libimp_decl.name,
                         decl->as.libimp_decl.name, decl->as.libimp_decl.path);
            continue;
        }

        /* imp a.b.c: register the last segment as a module alias so that
           qualified calls like "typewriter.scribe()" resolve correctly. */
        if (decl->kind == NodeImpDecl) {
            const char *mod = decl->as.imp_decl.module_name;
            const char *dot = strrchr(mod, '.');
            const char *alias = dot ? dot + 1 : mod;
            register_lib(&cg, alias, alias, Null);
            continue;
        }

        if (decl->kind == NodeTypeDecl) {
            if (decl->as.type_decl.decl_kind == TypeDeclStruct
                || decl->as.type_decl.decl_kind == TypeDeclUnion) {
                LLVMTypeRef stype = LLVMStructCreateNamed(cg.ctx, decl->as.type_decl.name);
                register_struct(&cg, decl->as.type_decl.name, stype,
                                decl->as.type_decl.decl_kind == TypeDeclUnion);
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

            if (fc > 0) {
                ft_heap = allocate(fc, sizeof(LLVMTypeRef));
                field_types = ft_heap.pointer;
                usize_t j = 0;
                while (j < fc) {
                    node_t *field = decl->as.type_decl.fields.items[j];
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
                        field_types[llvm_field_count] = get_llvm_type(&cg, fti);
                        struct_add_field(sr, field->as.var_decl.name, fti,
                                         llvm_field_count,
                                         field->as.var_decl.linkage,
                                         field->as.var_decl.storage);
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

            type_info_t ti = resolve_alias(&cg, decl->as.var_decl.type);
            LLVMTypeRef type = get_llvm_type(&cg, ti);
            LLVMValueRef global = LLVMAddGlobal(cg.module, type, decl->as.var_decl.name);

            /* linkage: int → internal, ext → external (default) */
            if (decl->as.var_decl.linkage == LinkageInternal)
                LLVMSetLinkage(global, LLVMInternalLinkage);

            /* @weak attribute */
            if (decl->as.var_decl.attr_flags & AttrWeak)
                LLVMSetLinkage(global, LLVMWeakAnyLinkage);
            /* @hidden attribute */
            if (decl->as.var_decl.attr_flags & AttrHidden)
                LLVMSetVisibility(global, LLVMHiddenVisibility);

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
            symtab_add(&cg.globals, decl->as.var_decl.name, global, type, ti, sym_flags);
            symtab_set_last_storage(&cg.globals, StorageStack, False); /* globals use static storage */
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

        if (decl->kind == NodeFnDecl) {
            /* library-backed internal functions live in the .a — skip */
            if (decl->from_lib && decl->as.fn_decl.linkage == LinkageInternal)
                continue;

            char fn_name[256];
            if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name) {
                snprintf(fn_name, sizeof(fn_name), "%s.%s",
                         decl->as.fn_decl.struct_name, decl->as.fn_decl.name);
            } else {
                snprintf(fn_name, sizeof(fn_name), "%s", decl->as.fn_decl.name);
            }

            /* in test mode, skip the user's main — we generate our own */
            if (cg.test_mode && strcmp(fn_name, "main") == 0) continue;

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
                    type_info_t pti = resolve_alias(&cg,
                        decl->as.fn_decl.params.items[j]->as.var_decl.type);
                    ptypes[j + offset] = get_llvm_type(&cg, pti);
                }
            }

            /* return type */
            LLVMTypeRef ret_type;
            boolean_t is_main = strcmp(fn_name, "main") == 0;
            if (is_main) {
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

            type_info_t dummy = {TypeVoid, Null, False, PtrNone, Null};
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
        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];

            /* library-backed internal inline methods live in the .a — skip */
            if (decl->from_lib && method->as.fn_decl.linkage == LinkageInternal)
                continue;
            char fn_name[256];
            snprintf(fn_name, sizeof(fn_name), "%s.%s",
                     decl->as.type_decl.name, method->as.fn_decl.name);

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
            if (method->as.fn_decl.return_count > 1) {
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

            type_info_t dummy = {TypeVoid, Null, False, PtrNone, Null};
            symtab_add(&cg.globals, ast_strdup(fn_name, strlen(fn_name)), fn, Null, dummy, False);
            deallocate(ptypes_heap);

            if (strcmp(method->as.fn_decl.name, "rem") == 0) {
                struct_reg_t *sr = find_struct(&cg, decl->as.type_decl.name);
                if (sr) sr->destructor = fn;
            }
        }
    }

    /* pass 2: generate function bodies */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeFnDecl) continue;

        /* library-backed functions: body lives in the .a — skip codegen */
        if (decl->from_lib) continue;

        char fn_name[256];
        if (decl->as.fn_decl.is_method && decl->as.fn_decl.struct_name)
            snprintf(fn_name, sizeof(fn_name), "%s.%s",
                     decl->as.fn_decl.struct_name, decl->as.fn_decl.name);
        else
            snprintf(fn_name, sizeof(fn_name), "%s", decl->as.fn_decl.name);

        /* in test mode, skip the user's main — we generate our own */
        if (cg.test_mode && strcmp(fn_name, "main") == 0) continue;

        symbol_t *sym = cg_lookup(&cg, fn_name);
        cg.current_fn = sym->value;
        cg.current_fn_linkage = decl->as.fn_decl.linkage;
        cg.current_struct_name = decl->as.fn_decl.is_method ? decl->as.fn_decl.struct_name : Null;
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
                type_info_t thi = {TypeUser, decl->as.fn_decl.struct_name,
                                   True, PtrReadWrite, Null};
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
            type_info_t this_ti = {TypeUser, decl->as.fn_decl.struct_name, True, PtrReadWrite, Null};
            symtab_add(&cg.locals, "this", this_alloca, this_type, this_ti, False);
            param_offset = 1;
        }

        for (usize_t j = 0; j < decl->as.fn_decl.params.count; j++) {
            node_t *param = decl->as.fn_decl.params.items[j];
            type_info_t pti = resolve_alias(&cg, param->as.var_decl.type);
            LLVMTypeRef ptype = get_llvm_type(&cg, pti);
            LLVMValueRef alloca_val = LLVMBuildAlloca(cg.builder, ptype,
                                                       param->as.var_decl.name);
            LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, (unsigned)(j + param_offset)),
                           alloca_val);
            symtab_add(&cg.locals, param->as.var_decl.name, alloca_val, ptype,
                       pti, False);

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

        gen_block(&cg, decl->as.fn_decl.body);

        LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg.builder);
        if (!LLVMGetBasicBlockTerminator(cur_bb)) {
            type_info_t rti = decl->as.fn_decl.return_types[0];
            if (rti.base == TypeVoid && !rti.is_pointer)
                LLVMBuildRetVoid(cg.builder);
            else
                LLVMBuildRet(cg.builder,
                    LLVMConstNull(get_llvm_type(&cg, rti)));
        }

        /* Restore scope to compile unit after leaving the function. */
        if (cg.debug_mode)
            cg.di_scope = cg.di_compile_unit;
    }

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

        for (usize_t m = 0; m < decl->as.type_decl.methods.count; m++) {
            node_t *method = decl->as.type_decl.methods.items[m];
            char fn_name[256];
            snprintf(fn_name, sizeof(fn_name), "%s.%s",
                     decl->as.type_decl.name, method->as.fn_decl.name);

            symbol_t *sym = cg_lookup(&cg, fn_name);
            if (!sym) continue;
            cg.current_fn = sym->value;
            cg.current_struct_name = decl->as.type_decl.name;
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
                type_info_t thi = {TypeUser, decl->as.type_decl.name, True, PtrReadWrite, Null};
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
            type_info_t this_ti = {TypeUser, decl->as.type_decl.name, True, PtrReadWrite, Null};
            symtab_add(&cg.locals, "this", this_alloca, this_type, this_ti, False);
            (void)sr;

            for (usize_t j = 0; j < method->as.fn_decl.params.count; j++) {
                node_t *param = method->as.fn_decl.params.items[j];
                type_info_t pti = resolve_alias(&cg, param->as.var_decl.type);
                LLVMTypeRef ptype = get_llvm_type(&cg, pti);
                LLVMValueRef alloca_val = LLVMBuildAlloca(cg.builder, ptype,
                                                           param->as.var_decl.name);
                LLVMBuildStore(cg.builder, LLVMGetParam(cg.current_fn, (unsigned)(j + 1)),
                               alloca_val);
                symtab_add(&cg.locals, param->as.var_decl.name, alloca_val, ptype,
                           pti, False);

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

            gen_block(&cg, method->as.fn_decl.body);

            LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg.builder);
            if (!LLVMGetBasicBlockTerminator(cur_bb)) {
                type_info_t rti = method->as.fn_decl.return_types[0];
                if (rti.base == TypeVoid && !rti.is_pointer)
                    LLVMBuildRetVoid(cg.builder);
                else
                    LLVMBuildRet(cg.builder,
                        LLVMConstNull(get_llvm_type(&cg, rti)));
            }

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

    /* ── DI: finalize before verification ──
     * Must happen after all IR is emitted and before the module is verified,
     * so that unresolved DI forward-references are resolved first. */
    if (cg.debug_mode && cg.di_builder) {
        LLVMDIBuilderFinalize(cg.di_builder);
        LLVMDisposeDIBuilder(cg.di_builder);
        cg.di_builder = Null;
    }

    /* verify */
    char *error = Null;
    if (LLVMVerifyModule(cg.module, LLVMReturnStatusAction, &error)) {
        log_err("LLVM verify: %s", error);
        LLVMDisposeMessage(error);
    } else {
        if (error) LLVMDisposeMessage(error);
    }

    /* emit object file — reuse the machine created during early target init */
    error = Null;
    if (LLVMTargetMachineEmitToFile(machine, cg.module, (char *)obj_output,
                                     LLVMObjectFile, &error)) {
        log_err("emit failed: %s", error);
        LLVMDisposeMessage(error);
        LLVMDisposeTargetMachine(machine);
        LLVMDisposeMessage(triple);
        return Err;
    }

    LLVMDisposeTargetMachine(machine);
    LLVMDisposeMessage(triple);
    if (cg.di_data_layout) LLVMDisposeTargetData(cg.di_data_layout);

    LLVMDisposeBuilder(cg.builder);
    LLVMDisposeModule(cg.module);
    LLVMContextDispose(cg.ctx);

    symtab_free(&cg.globals);
    symtab_free(&cg.locals);

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

    return Ok;
}
