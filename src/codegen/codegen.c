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

/* ── symtab ── */

static void symtab_init(symtab_t *st) {
    st->entries = Null; st->count = 0; st->capacity = 0; st->heap = NullHeap;
}
static void symtab_free(symtab_t *st) {
    if (st->heap.pointer != Null) {
        deallocate(st->heap); st->heap = NullHeap;
        st->entries = Null; st->count = 0; st->capacity = 0;
    }
}
static void symtab_add(symtab_t *st, const char *name, LLVMValueRef value,
                        LLVMTypeRef type, type_info_t stype, int flags) {
    if (st->count >= st->capacity) {
        usize_t new_cap = st->capacity < 16 ? 16 : st->capacity * 2;
        if (st->heap.pointer == Null)
            st->heap = allocate(new_cap, sizeof(symbol_t));
        else
            st->heap = reallocate(st->heap, new_cap * sizeof(symbol_t));
        st->entries = st->heap.pointer;
        st->capacity = new_cap;
    }
    symbol_t sym = {0};
    sym.name = (char *)name;
    sym.value = value;
    sym.type = type;
    sym.stype = stype;
    sym.flags = flags;
    sym.array_size = -1; /* -1 = not an array / unknown size */
    st->entries[st->count++] = sym;
}
static symbol_t *symtab_lookup(symtab_t *st, const char *name) {
    for (usize_t i = st->count; i > 0; i--) {
        if (strcmp(st->entries[i - 1].name, name) == 0)
            return &st->entries[i - 1];
    }
    return Null;
}
static symbol_t *cg_lookup(cg_t *cg, const char *name) {
    symbol_t *s = symtab_lookup(&cg->locals, name);
    if (s) return s;
    return symtab_lookup(&cg->globals, name);
}

static void symtab_set_last_storage(symtab_t *st, storage_t storage, boolean_t is_heap_var) {
    if (st->count > 0) {
        st->entries[st->count - 1].storage = storage;
        if (is_heap_var)
            st->entries[st->count - 1].flags |= SymHeapVar;
        else
            st->entries[st->count - 1].flags &= ~SymHeapVar;
    }
}

static void symtab_set_last_extra(symtab_t *st, boolean_t is_const, boolean_t is_final,
                                   linkage_t linkage, usize_t scope_depth, long array_size) {
    if (st->count > 0) {
        symbol_t *s = &st->entries[st->count - 1];
        if (is_const)  s->flags |= SymConst;  else s->flags &= ~SymConst;
        if (is_final)  s->flags |= SymFinal;  else s->flags &= ~SymFinal;
        s->linkage     = linkage;
        s->scope_depth = scope_depth;
        s->array_size  = array_size;
    }
}

static void symtab_set_last_nil(symtab_t *st, boolean_t is_nil) {
    if (st->count > 0) {
        if (is_nil) st->entries[st->count - 1].flags |= SymNil;
        else        st->entries[st->count - 1].flags &= ~SymNil;
    }
}

/* ── pointer safety checks ── */

/* Check: no writable pointer from const/final variable */
static void check_const_addr_of(cg_t *cg, node_t *init, type_info_t target_type, usize_t line) {
    if (!init || init->kind != NodeAddrOf || !target_type.is_pointer) return;
    node_t *operand = init->as.addr_of.operand;
    if (operand->kind != NodeIdentExpr) return;
    symbol_t *src = cg_lookup(cg, operand->as.ident.name);
    if (!src) return;
    boolean_t src_const = (src->flags & SymConst) != 0;
    boolean_t src_final = (src->flags & SymFinal) != 0;
    if ((src_const || src_final) && (target_type.ptr_perm & (PtrWrite)))
        log_err("line %lu: cannot derive writable pointer from %s variable '%s'",
                line, src_const ? "const" : "final", src->name);
}

/* Check: permission widening forbidden (e.g. *r → *rw) */
static void check_permission_widening(cg_t *cg, node_t *init, type_info_t target_type, usize_t line) {
    if (!init || !target_type.is_pointer) return;
    if (init->kind == NodeIdentExpr) {
        symbol_t *src = cg_lookup(cg, init->as.ident.name);
        if (!src || !src->stype.is_pointer) return;
        ptr_perm_t sp = src->stype.ptr_perm;
        ptr_perm_t tp = target_type.ptr_perm;
        /* a permission bit present in tp but absent in sp is a widening — forbidden */
        if (tp & ~sp & (PtrRead | PtrWrite | PtrArith))
            log_err("line %lu: cannot widen pointer permissions (source: %s%s%s → target: %s%s%s)",
                    line,
                    (sp & PtrRead)  ? "r" : "", (sp & PtrWrite) ? "w" : "",
                    (sp & PtrArith) ? "+" : "",
                    (tp & PtrRead)  ? "r" : "", (tp & PtrWrite) ? "w" : "",
                    (tp & PtrArith) ? "+" : "");
    }
}

/* Check: no stack pointer escape via ret */
static void check_stack_escape(cg_t *cg, node_t *ret_val, usize_t line) {
    if (!ret_val) return;
    if (ret_val->kind == NodeAddrOf) {
        node_t *operand = ret_val->as.addr_of.operand;
        if (operand->kind == NodeIdentExpr) {
            symbol_t *src = cg_lookup(cg, operand->as.ident.name);
            /* locals are stack-allocated; returning their address is always wrong */
            if (src && src->storage == StorageStack
                && symtab_lookup(&cg->locals, operand->as.ident.name))
                log_err("line %lu: cannot return pointer to local stack variable '%s'",
                        line, operand->as.ident.name);
        }
    }
}

/* Check: no ext function returning pointer to int (private) global */
static void check_ext_returns_int_ptr(cg_t *cg, node_t *ret_val, linkage_t fn_linkage, usize_t line) {
    if (fn_linkage != LinkageExternal || !ret_val) return;
    if (ret_val->kind == NodeAddrOf) {
        node_t *operand = ret_val->as.addr_of.operand;
        if (operand->kind == NodeIdentExpr) {
            symbol_t *src = symtab_lookup(&cg->globals, operand->as.ident.name);
            if (src && src->linkage == LinkageInternal)
                log_err("line %lu: ext function cannot expose pointer to int global '%s'",
                        line, operand->as.ident.name);
        }
    }
}

/* Check: pointer lifetime — pointee must outlive the pointer */
static void check_pointer_lifetime(cg_t *cg, node_t *init, usize_t ptr_scope_depth, usize_t line) {
    if (!init || init->kind != NodeAddrOf) return;
    node_t *operand = init->as.addr_of.operand;
    if (operand->kind != NodeIdentExpr) return;
    symbol_t *src = cg_lookup(cg, operand->as.ident.name);
    if (!src) return;
    /* global variables live forever (scope_depth 0); no problem */
    if (src->scope_depth > ptr_scope_depth)
        log_err("line %lu: pointer outlives pointee '%s' (scope mismatch)",
                line, src->name);
}

/* Check: null dereference of a statically-nil pointer */
static void check_null_deref(cg_t *cg, const char *name, usize_t line) {
    symbol_t *sym = cg_lookup(cg, name);
    if (sym && (sym->flags & SymNil))
        log_err("line %lu: dereference of nil pointer '%s'", line, name);
}

/* Check: pointer arithmetic permission and known array bounds */
static void check_ptr_arith_bounds(cg_t *cg, node_t *node) {
    if (node->as.binary.op != TokPlus && node->as.binary.op != TokMinus) return;
    node_t *ptr_node = node->as.binary.left;
    node_t *idx_node = node->as.binary.right;
    if (ptr_node->kind != NodeIdentExpr) return;
    symbol_t *sym = cg_lookup(cg, ptr_node->as.ident.name);
    if (!sym) return;
    /* enforce + permission for pointer arithmetic */
    if (sym->stype.is_pointer && !(sym->stype.ptr_perm & PtrArith))
        log_err("line %lu: pointer arithmetic not permitted on '%s' (pointer lacks '+' permission)",
                node->line, sym->name);
    if (idx_node->kind != NodeIntLitExpr) return;
    if (sym->array_size < 0) return; /* unknown size */
    long offset = idx_node->as.int_lit.value;
    if (node->as.binary.op == TokMinus) offset = -offset;
    if (offset < 0 || offset >= sym->array_size)
        log_err("line %lu: pointer arithmetic index %ld out of bounds for '%s[%ld]'",
                node->line, offset, sym->name, sym->array_size);
}

/* ── struct / enum / alias lookup ── */

static struct_reg_t *find_struct(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->struct_count; i++)
        if (strcmp(cg->structs[i].name, name) == 0) return &cg->structs[i];
    return Null;
}

static enum_reg_t *find_enum(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->enum_count; i++)
        if (strcmp(cg->enums[i].name, name) == 0) return &cg->enums[i];
    return Null;
}

static type_info_t resolve_alias(cg_t *cg, type_info_t ti) {
    if (ti.base == TypeUser && ti.user_name) {
        for (usize_t i = 0; i < cg->alias_count; i++) {
            if (strcmp(cg->aliases[i].name, ti.user_name) == 0)
                return cg->aliases[i].actual;
        }
    }
    return ti;
}

static const char *find_lib_alias(cg_t *cg, const char *alias) {
    for (usize_t i = 0; i < cg->lib_count; i++) {
        if (cg->libs[i].alias && strcmp(cg->libs[i].alias, alias) == 0)
            return cg->libs[i].name;
        if (!cg->libs[i].alias && strcmp(cg->libs[i].name, alias) == 0)
            return cg->libs[i].name;
    }
    return Null;
}

/* Walk the module's flat declaration list and return the NodeFnDecl whose
   bare name matches `name` (ignoring method qualifications).  Used by
   let-binding type inference. */
static node_t *find_fn_decl(cg_t *cg, const char *name) {
    if (!cg->ast) return Null;
    node_list_t *decls = &cg->ast->as.module.decls;
    for (usize_t i = 0; i < decls->count; i++) {
        node_t *d = decls->items[i];
        if (d->kind == NodeFnDecl && d->as.fn_decl.name
                && strcmp(d->as.fn_decl.name, name) == 0)
            return d;
        /* also search inline struct methods */
        if (d->kind == NodeTypeDecl) {
            for (usize_t j = 0; j < d->as.type_decl.methods.count; j++) {
                node_t *m = d->as.type_decl.methods.items[j];
                if (m->kind == NodeFnDecl && m->as.fn_decl.name
                        && strcmp(m->as.fn_decl.name, name) == 0)
                    return m;
            }
        }
    }
    return Null;
}

/* gen_stmt forward declaration (emit_dtor_calls calls it for deferred stmts) */
static void gen_stmt(cg_t *cg, node_t *node);

/* forward declarations for mutual recursion in struct cleanup */
static void emit_struct_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val);
static void emit_struct_field_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val);

/* ── destructor scope helpers ── */

static void push_dtor_scope(cg_t *cg) {
    if (cg->dtor_depth >= cg->dtor_cap) {
        usize_t new_cap = cg->dtor_cap < 8 ? 8 : cg->dtor_cap * 2;
        if (cg->dtor_stack_heap.pointer == Null)
            cg->dtor_stack_heap = allocate(new_cap, sizeof(dtor_scope_t));
        else
            cg->dtor_stack_heap = reallocate(cg->dtor_stack_heap, new_cap * sizeof(dtor_scope_t));
        cg->dtor_stack = cg->dtor_stack_heap.pointer;
        cg->dtor_cap = new_cap;
    }
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth++];
    scope->vars = Null;
    scope->count = 0;
    scope->capacity = 0;
    scope->heap = NullHeap;
    scope->deferred = Null;
    scope->deferred_count = 0;
    scope->deferred_cap = 0;
    scope->deferred_heap = NullHeap;
}

static void add_deferred_stmt(cg_t *cg, node_t *stmt) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    if (scope->deferred_count >= scope->deferred_cap) {
        usize_t new_cap = scope->deferred_cap < 4 ? 4 : scope->deferred_cap * 2;
        if (scope->deferred_heap.pointer == Null)
            scope->deferred_heap = allocate(new_cap, sizeof(node_t *));
        else
            scope->deferred_heap = reallocate(scope->deferred_heap, new_cap * sizeof(node_t *));
        scope->deferred = scope->deferred_heap.pointer;
        scope->deferred_cap = new_cap;
    }
    scope->deferred[scope->deferred_count++] = stmt;
}

static void add_dtor_var(cg_t *cg, LLVMValueRef alloca_val, const char *struct_name) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    if (scope->count >= scope->capacity) {
        usize_t new_cap = scope->capacity < 4 ? 4 : scope->capacity * 2;
        if (scope->heap.pointer == Null)
            scope->heap = allocate(new_cap, sizeof(dtor_var_t));
        else
            scope->heap = reallocate(scope->heap, new_cap * sizeof(dtor_var_t));
        scope->vars = scope->heap.pointer;
        scope->capacity = new_cap;
    }
    scope->vars[scope->count].alloca_val = alloca_val;
    scope->vars[scope->count].struct_name = (char *)struct_name;
    scope->vars[scope->count].is_heap_alloc = False;
    scope->count++;
}

static void add_heap_var(cg_t *cg, LLVMValueRef alloca_val) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    if (scope->count >= scope->capacity) {
        usize_t new_cap = scope->capacity < 4 ? 4 : scope->capacity * 2;
        if (scope->heap.pointer == Null)
            scope->heap = allocate(new_cap, sizeof(dtor_var_t));
        else
            scope->heap = reallocate(scope->heap, new_cap * sizeof(dtor_var_t));
        scope->vars = scope->heap.pointer;
        scope->capacity = new_cap;
    }
    scope->vars[scope->count].alloca_val = alloca_val;
    scope->vars[scope->count].struct_name = Null;
    scope->vars[scope->count].is_heap_alloc = True;
    scope->count++;
}

/* ── struct auto-cleanup helpers ── */

/* Recursively free heap pointer fields and call nested struct destructors.
   This runs AFTER the user-defined rem() (if any).  Heap pointer fields are
   freed with free() — safe if null (user should null after manual rem.()).
   Nested struct-typed fields are fully cleaned up recursively. */
static void emit_struct_field_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val) {
    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
    for (usize_t i = 0; i < sr->field_count; i++) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) return;
        field_info_t *f = &sr->fields[i];

        /* auto-free heap pointer fields (free(NULL) is a no-op) */
        if (f->storage == StorageHeap && f->type.is_pointer) {
            LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type,
                                                    alloca_val, (unsigned)f->index, "hfld.gep");
            LLVMValueRef hptr = LLVMBuildLoad2(cg->builder, ptr_ty, gep, "hfld.ptr");
            LLVMValueRef fargs[1] = { hptr };
            LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, fargs, 1, "");
        }

        /* recursively clean up nested struct-typed (non-pointer) fields */
        if (f->type.base == TypeUser && !f->type.is_pointer && f->type.user_name) {
            struct_reg_t *nested = find_struct(cg, f->type.user_name);
            if (nested) {
                LLVMValueRef gep = LLVMBuildStructGEP2(cg->builder, sr->llvm_type,
                                                        alloca_val, (unsigned)f->index, "sfld.gep");
                emit_struct_cleanup(cg, nested, gep);
            }
        }
    }
}

/* Call the user's rem() destructor (if any), then auto-clean all fields. */
static void emit_struct_cleanup(cg_t *cg, struct_reg_t *sr, LLVMValueRef alloca_val) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) return;
    if (sr->destructor) {
        LLVMTypeRef fn_type = LLVMGlobalGetValueType(sr->destructor);
        LLVMValueRef args[1] = { alloca_val };
        LLVMBuildCall2(cg->builder, fn_type, sr->destructor, args, 1, "");
    }
    emit_struct_field_cleanup(cg, sr, alloca_val);
}

static void emit_dtor_calls(cg_t *cg, dtor_scope_t *scope) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;
    /* run deferred statements in LIFO order first */
    for (usize_t i = scope->deferred_count; i > 0; i--) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) break;
        gen_stmt(cg, scope->deferred[i - 1]);
    }
    /* then run destructors / heap-var frees in LIFO order */
    for (usize_t i = scope->count; i > 0; i--) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) break;
        dtor_var_t *dv = &scope->vars[i - 1];
        if (dv->is_heap_alloc) {
            /* free heap-allocated primitive (free(null) is safe, no branch needed) */
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty,
                                                    dv->alloca_val, "hptr");
            LLVMValueRef args[1] = { heap_ptr };
            LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, args, 1, "");
        } else if (dv->struct_name) {
            struct_reg_t *sr = find_struct(cg, dv->struct_name);
            /* emit_struct_cleanup calls rem() then auto-frees heap fields
               and recursively destroys nested struct-typed fields */
            if (sr) emit_struct_cleanup(cg, sr, dv->alloca_val);
        }
    }
}

static void emit_all_dtor_calls(cg_t *cg) {
    for (usize_t i = cg->dtor_depth; i > 0; i--)
        emit_dtor_calls(cg, &cg->dtor_stack[i - 1]);
}

/* Remove a variable from all dtor scopes so its destructor isn't called.
   Used when a struct is returned by value (moved to caller). */
static void remove_from_dtor_scopes(cg_t *cg, LLVMValueRef alloca_val) {
    for (usize_t d = 0; d < cg->dtor_depth; d++) {
        dtor_scope_t *scope = &cg->dtor_stack[d];
        for (usize_t i = 0; i < scope->count; i++) {
            if (scope->vars[i].alloca_val == alloca_val) {
                for (usize_t j = i; j + 1 < scope->count; j++)
                    scope->vars[j] = scope->vars[j + 1];
                scope->count--;
                return;
            }
        }
    }
}

static void pop_dtor_scope(cg_t *cg) {
    if (cg->dtor_depth == 0) return;
    dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
    emit_dtor_calls(cg, scope);
    if (scope->heap.pointer != Null) deallocate(scope->heap);
    if (scope->deferred_heap.pointer != Null) deallocate(scope->deferred_heap);
    cg->dtor_depth--;
}

/* ── LLVM type helpers ── */

static boolean_t is_float_type(type_info_t ti) {
    return ti.base == TypeF32 || ti.base == TypeF64;
}

static boolean_t is_unsigned_type(type_info_t ti) {
    return ti.base == TypeU8 || ti.base == TypeU16
        || ti.base == TypeU32 || ti.base == TypeU64;
}

static boolean_t is_integer_type(type_info_t ti) {
    return ti.base >= TypeI8 && ti.base <= TypeU64;
}

static LLVMTypeRef get_llvm_base_type(cg_t *cg, type_info_t ti) {
    ti = resolve_alias(cg, ti);
    if (ti.is_pointer)
        return LLVMPointerTypeInContext(cg->ctx, 0);
    switch (ti.base) {
        case TypeVoid: return LLVMVoidTypeInContext(cg->ctx);
        case TypeBool: return LLVMInt1TypeInContext(cg->ctx);
        case TypeI8:  case TypeU8:  return LLVMInt8TypeInContext(cg->ctx);
        case TypeI16: case TypeU16: return LLVMInt16TypeInContext(cg->ctx);
        case TypeI32: case TypeU32: return LLVMInt32TypeInContext(cg->ctx);
        case TypeI64: case TypeU64: return LLVMInt64TypeInContext(cg->ctx);
        case TypeF32: return LLVMFloatTypeInContext(cg->ctx);
        case TypeF64: return LLVMDoubleTypeInContext(cg->ctx);
        case TypeUser: {
            struct_reg_t *sr = ti.user_name ? find_struct(cg, ti.user_name) : Null;
            if (sr) return sr->llvm_type;
            enum_reg_t *er = ti.user_name ? find_enum(cg, ti.user_name) : Null;
            if (er) return er->llvm_type;
            return LLVMInt32TypeInContext(cg->ctx);
        }
        case TypeError:
            return cg->error_type;
        case TypeFnPtr:
            /* function pointers are opaque pointers in LLVM's opaque pointer model */
            return LLVMPointerTypeInContext(cg->ctx, 0);
    }
    return LLVMVoidTypeInContext(cg->ctx);
}

static LLVMTypeRef get_llvm_type(cg_t *cg, type_info_t ti) {
    return get_llvm_base_type(cg, ti);
}

/* Build the LLVM function type for a TypeFnPtr descriptor. */
static LLVMTypeRef build_fn_ptr_llvm_type(cg_t *cg, fn_ptr_desc_t *desc) {
    LLVMTypeRef ret_ty = get_llvm_type(cg, desc->ret_type);
    LLVMTypeRef *param_types = Null;
    heap_t pt_heap = NullHeap;
    if (desc->param_count > 0) {
        pt_heap = allocate(desc->param_count, sizeof(LLVMTypeRef));
        param_types = pt_heap.pointer;
        for (usize_t i = 0; i < desc->param_count; i++)
            param_types[i] = get_llvm_type(cg, desc->params[i].type);
    }
    LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, param_types,
                                          (unsigned)desc->param_count, 0);
    if (desc->param_count > 0) deallocate(pt_heap);
    return fn_ty;
}

static LLVMValueRef coerce_int(cg_t *cg, LLVMValueRef val, LLVMTypeRef target) {
    LLVMTypeRef src = LLVMTypeOf(val);
    if (src == target) return val;
    LLVMTypeKind sk = LLVMGetTypeKind(src);
    LLVMTypeKind tk = LLVMGetTypeKind(target);
    if (sk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned tw = LLVMGetIntTypeWidth(target);
        unsigned sw = LLVMGetIntTypeWidth(src);
        if (tw > sw) return LLVMBuildSExt(cg->builder, val, target, "sext");
        if (tw < sw) return LLVMBuildTrunc(cg->builder, val, target, "trunc");
    }
    /* int -> float */
    if (sk == LLVMIntegerTypeKind && (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind))
        return LLVMBuildSIToFP(cg->builder, val, target, "itof");
    /* float -> int */
    if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) && tk == LLVMIntegerTypeKind)
        return LLVMBuildFPToSI(cg->builder, val, target, "ftoi");
    /* float -> float */
    if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
        (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
        if (sk == LLVMFloatTypeKind && tk == LLVMDoubleTypeKind)
            return LLVMBuildFPExt(cg->builder, val, target, "fpext");
        if (sk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
            return LLVMBuildFPTrunc(cg->builder, val, target, "fptrunc");
    }
    return val;
}

/* Create a nil error value: {i1 false, ptr null} */
static LLVMValueRef make_nil_error(cg_t *cg) {
    LLVMValueRef err = LLVMGetUndef(cg->error_type);
    err = LLVMBuildInsertValue(cg->builder, err,
        LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0), 0, "nil_err.has");
    err = LLVMBuildInsertValue(cg->builder, err,
        LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0)), 1, "nil_err.msg");
    return err;
}

static boolean_t llvm_is_float(LLVMTypeRef t) {
    LLVMTypeKind k = LLVMGetTypeKind(t);
    return k == LLVMFloatTypeKind || k == LLVMDoubleTypeKind;
}

static boolean_t llvm_is_int(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMIntegerTypeKind;
}

static boolean_t llvm_is_ptr(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMPointerTypeKind;
}

/* ── payload size helper ── */

static usize_t payload_type_size(type_info_t ti) {
    if (ti.is_pointer) return 8;
    switch (ti.base) {
        case TypeBool: case TypeI8:  case TypeU8:  return 1;
        case TypeI16:  case TypeU16:               return 2;
        case TypeI32:  case TypeU32: case TypeF32: return 4;
        case TypeI64:  case TypeU64: case TypeF64: return 8;
        default: return 8; /* conservative default for user types */
    }
}

/* ── alloca in entry block helper ── */

static LLVMValueRef alloc_in_entry(cg_t *cg, LLVMTypeRef type, const char *name) {
    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBasicBlockRef entry_bb = LLVMGetEntryBasicBlock(cg->current_fn);
    LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_bb);
    if (first_instr)
        LLVMPositionBuilderBefore(cg->builder, first_instr);
    else
        LLVMPositionBuilderAtEnd(cg->builder, entry_bb);
    LLVMValueRef a = LLVMBuildAlloca(cg->builder, type, name);
    LLVMPositionBuilderAtEnd(cg->builder, saved_bb);
    return a;
}

/* ── forward declarations ── */

static LLVMValueRef gen_expr(cg_t *cg, node_t *node);
static void gen_stmt(cg_t *cg, node_t *node);
static void gen_block(cg_t *cg, node_t *node);

/* DI helpers — defined after the registry helpers but called from gen_local_var
   and gen_stmt which appear earlier in the file. */
static LLVMMetadataRef get_di_type(cg_t *cg, type_info_t ti);
static LLVMMetadataRef di_make_location(cg_t *cg, usize_t line);
static void             di_set_location(cg_t *cg, usize_t line);

/* ── expressions ── */

static LLVMValueRef gen_int_lit(cg_t *cg, node_t *node) {
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                        (unsigned long long)node->as.int_lit.value, 1);
}

static LLVMValueRef gen_float_lit(cg_t *cg, node_t *node) {
    return LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), node->as.float_lit.value);
}

static LLVMValueRef gen_bool_lit(cg_t *cg, node_t *node) {
    return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), node->as.bool_lit.value, 0);
}

static LLVMValueRef gen_char_lit(cg_t *cg, node_t *node) {
    return LLVMConstInt(LLVMInt8TypeInContext(cg->ctx),
                        (unsigned char)node->as.char_lit.value, 0);
}

static LLVMValueRef gen_str_lit(cg_t *cg, node_t *node) {
    return LLVMBuildGlobalStringPtr(cg->builder, node->as.str_lit.value, "str");
}

static LLVMValueRef gen_ident(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.ident.name);
    if (!sym) {
        log_err("undefined variable '%s'", node->as.ident.name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    if (sym->flags & SymHeapVar) {
        /* heap primitive: alloca holds the malloc ptr; do double-load */
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        return LLVMBuildLoad2(cg->builder, sym->type, heap_ptr, node->as.ident.name);
    }
    /* arrays decay to a pointer (C semantics): return the alloca directly */
    if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind)
        return sym->value;
    LLVMValueRef load = LLVMBuildLoad2(cg->builder, sym->type, sym->value,
                                        node->as.ident.name);
    if (sym->flags & SymAtomic)
        LLVMSetOrdering(load, LLVMAtomicOrderingSequentiallyConsistent);
    if (sym->flags & SymVolatile)
        LLVMSetVolatile(load, 1);
    return load;
}

static LLVMValueRef gen_binary(cg_t *cg, node_t *node) {
    /* short-circuit logical AND */
    if (node->as.binary.op == TokAmpAmp) {
        LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "and.rhs");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "and.merge");

        LLVMValueRef lhs = gen_expr(cg, node->as.binary.left);
        LLVMBasicBlockRef lhs_bb = LLVMGetInsertBlock(cg->builder);
        if (LLVMTypeOf(lhs) != LLVMInt1TypeInContext(cg->ctx))
            lhs = LLVMBuildICmp(cg->builder, LLVMIntNE, lhs,
                                LLVMConstInt(LLVMTypeOf(lhs), 0, 0), "tobool");
        LLVMBuildCondBr(cg->builder, lhs, rhs_bb, merge_bb);

        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef rhs = gen_expr(cg, node->as.binary.right);
        if (LLVMTypeOf(rhs) != LLVMInt1TypeInContext(cg->ctx))
            rhs = LLVMBuildICmp(cg->builder, LLVMIntNE, rhs,
                                LLVMConstInt(LLVMTypeOf(rhs), 0, 0), "tobool");
        LLVMBuildBr(cg->builder, merge_bb);
        rhs_bb = LLVMGetInsertBlock(cg->builder);

        LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->ctx), "and");
        LLVMValueRef false_val = LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, 0);
        LLVMValueRef vals[2] = { false_val, rhs };
        LLVMBasicBlockRef bbs[2] = { lhs_bb, rhs_bb };
        LLVMAddIncoming(phi, vals, bbs, 2);
        return phi;
    }

    /* short-circuit logical OR */
    if (node->as.binary.op == TokPipePipe) {
        LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "or.rhs");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "or.merge");

        LLVMValueRef lhs = gen_expr(cg, node->as.binary.left);
        LLVMBasicBlockRef lhs_bb = LLVMGetInsertBlock(cg->builder);
        if (LLVMTypeOf(lhs) != LLVMInt1TypeInContext(cg->ctx))
            lhs = LLVMBuildICmp(cg->builder, LLVMIntNE, lhs,
                                LLVMConstInt(LLVMTypeOf(lhs), 0, 0), "tobool");
        LLVMBuildCondBr(cg->builder, lhs, merge_bb, rhs_bb);

        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef rhs = gen_expr(cg, node->as.binary.right);
        if (LLVMTypeOf(rhs) != LLVMInt1TypeInContext(cg->ctx))
            rhs = LLVMBuildICmp(cg->builder, LLVMIntNE, rhs,
                                LLVMConstInt(LLVMTypeOf(rhs), 0, 0), "tobool");
        LLVMBuildBr(cg->builder, merge_bb);
        rhs_bb = LLVMGetInsertBlock(cg->builder);

        LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->ctx), "or");
        LLVMValueRef true_val = LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0);
        LLVMValueRef vals[2] = { true_val, rhs };
        LLVMBasicBlockRef bbs[2] = { lhs_bb, rhs_bb };
        LLVMAddIncoming(phi, vals, bbs, 2);
        return phi;
    }

    /* pointer arithmetic bounds check */
    check_ptr_arith_bounds(cg, node);

    /* error == nil / error != nil: extract has_error flag */
    if ((node->as.binary.op == TokEqEq || node->as.binary.op == TokBangEq)
        && (node->as.binary.right->kind == NodeNilExpr
            || node->as.binary.left->kind == NodeNilExpr)) {
        /* figure out which side is the error value */
        node_t *err_node = node->as.binary.left;
        if (err_node->kind == NodeNilExpr) err_node = node->as.binary.right;
        if (err_node->kind == NodeIdentExpr) {
            symbol_t *esym = cg_lookup(cg, err_node->as.ident.name);
            if (esym && esym->stype.base == TypeError) {
                LLVMValueRef err_val = gen_expr(cg, err_node);
                LLVMValueRef has_err = LLVMBuildExtractValue(cg->builder, err_val, 0, "has_err");
                if (node->as.binary.op == TokEqEq)
                    return LLVMBuildNot(cg->builder, has_err, "is_nil");
                else
                    return has_err;
            }
        }
    }

    LLVMValueRef left = gen_expr(cg, node->as.binary.left);
    LLVMValueRef right = gen_expr(cg, node->as.binary.right);

    LLVMTypeRef lt = LLVMTypeOf(left);
    LLVMTypeRef rt = LLVMTypeOf(right);

    /* promote to matching types */
    if (lt != rt) {
        if (llvm_is_float(lt) || llvm_is_float(rt)) {
            LLVMTypeRef wider = LLVMDoubleTypeInContext(cg->ctx);
            left = coerce_int(cg, left, wider);
            right = coerce_int(cg, right, wider);
        } else if (llvm_is_int(lt) && llvm_is_int(rt)) {
            unsigned lw = LLVMGetIntTypeWidth(lt);
            unsigned rw = LLVMGetIntTypeWidth(rt);
            LLVMTypeRef wider = lw >= rw ? lt : rt;
            left = coerce_int(cg, left, wider);
            right = coerce_int(cg, right, wider);
        }
        lt = LLVMTypeOf(left);
    }

    boolean_t is_fp = llvm_is_float(lt);

    switch (node->as.binary.op) {
        case TokPlus:
            return is_fp ? LLVMBuildFAdd(cg->builder, left, right, "fadd")
                         : LLVMBuildAdd(cg->builder, left, right, "add");
        case TokMinus:
            return is_fp ? LLVMBuildFSub(cg->builder, left, right, "fsub")
                         : LLVMBuildSub(cg->builder, left, right, "sub");
        case TokStar:
            return is_fp ? LLVMBuildFMul(cg->builder, left, right, "fmul")
                         : LLVMBuildMul(cg->builder, left, right, "mul");
        case TokSlash:
            if (is_fp) return LLVMBuildFDiv(cg->builder, left, right, "fdiv");
            return LLVMBuildSDiv(cg->builder, left, right, "div");
        case TokPercent:
            if (is_fp) return LLVMBuildFRem(cg->builder, left, right, "fmod");
            return LLVMBuildSRem(cg->builder, left, right, "mod");

        case TokLt:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOLT, left, right, "flt")
                         : LLVMBuildICmp(cg->builder, LLVMIntSLT, left, right, "lt");
        case TokGt:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOGT, left, right, "fgt")
                         : LLVMBuildICmp(cg->builder, LLVMIntSGT, left, right, "gt");
        case TokLtEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOLE, left, right, "fle")
                         : LLVMBuildICmp(cg->builder, LLVMIntSLE, left, right, "le");
        case TokGtEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOGE, left, right, "fge")
                         : LLVMBuildICmp(cg->builder, LLVMIntSGE, left, right, "ge");
        case TokEqEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealOEQ, left, right, "feq")
                         : LLVMBuildICmp(cg->builder, LLVMIntEQ, left, right, "eq");
        case TokBangEq:
            return is_fp ? LLVMBuildFCmp(cg->builder, LLVMRealONE, left, right, "fne")
                         : LLVMBuildICmp(cg->builder, LLVMIntNE, left, right, "ne");

        /* wrapping arithmetic (+% -% *%) — LLVM integer add/sub/mul already wraps */
        case TokPlusPercent:
            return LLVMBuildAdd(cg->builder, left, right, "wadd");
        case TokMinusPercent:
            return LLVMBuildSub(cg->builder, left, right, "wsub");
        case TokStarPercent:
            return LLVMBuildMul(cg->builder, left, right, "wmul");

        /* trapping arithmetic (+! -! *!) — overflow intrinsics + trap */
        case TokPlusBang: case TokMinusBang: case TokStarBang: {
            const char *intrinsic;
            if (node->as.binary.op == TokPlusBang)      intrinsic = "llvm.sadd.with.overflow.i32";
            else if (node->as.binary.op == TokMinusBang) intrinsic = "llvm.ssub.with.overflow.i32";
            else                                          intrinsic = "llvm.smul.with.overflow.i32";

            LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);
            left  = coerce_int(cg, left,  i32);
            right = coerce_int(cg, right, i32);

            /* build { i32, i1 } return type */
            LLVMTypeRef ret_fields[2] = { i32, LLVMInt1TypeInContext(cg->ctx) };
            LLVMTypeRef ret_type = LLVMStructTypeInContext(cg->ctx, ret_fields, 2, 0);
            LLVMTypeRef param_types[2] = { i32, i32 };
            LLVMTypeRef fn_type = LLVMFunctionType(ret_type, param_types, 2, 0);

            LLVMValueRef fn = LLVMGetNamedFunction(cg->module, intrinsic);
            if (!fn) fn = LLVMAddFunction(cg->module, intrinsic, fn_type);

            LLVMValueRef args[2] = { left, right };
            LLVMValueRef result = LLVMBuildCall2(cg->builder, fn_type, fn, args, 2, "ov");
            LLVMValueRef val = LLVMBuildExtractValue(cg->builder, result, 0, "ov.val");
            LLVMValueRef overflowed = LLVMBuildExtractValue(cg->builder, result, 1, "ov.flag");

            /* branch: if overflowed, call llvm.trap; else continue */
            LLVMBasicBlockRef trap_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "ov.trap");
            LLVMBasicBlockRef ok_bb   = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "ov.ok");
            LLVMBuildCondBr(cg->builder, overflowed, trap_bb, ok_bb);

            LLVMPositionBuilderAtEnd(cg->builder, trap_bb);
            {
                LLVMTypeRef trap_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), Null, 0, 0);
                LLVMValueRef trap_fn = LLVMGetNamedFunction(cg->module, "llvm.trap");
                if (!trap_fn) trap_fn = LLVMAddFunction(cg->module, "llvm.trap", trap_type);
                LLVMBuildCall2(cg->builder, trap_type, trap_fn, Null, 0, "");
                LLVMBuildUnreachable(cg->builder);
            }
            LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
            return val;
        }

        /* bitwise */
        case TokAmp:   return LLVMBuildAnd(cg->builder, left, right, "and");
        case TokPipe:  return LLVMBuildOr(cg->builder, left, right, "or");
        case TokCaret: return LLVMBuildXor(cg->builder, left, right, "xor");
        case TokLtLt:  return LLVMBuildShl(cg->builder, left, right, "shl");
        case TokGtGt:  return LLVMBuildAShr(cg->builder, left, right, "shr");

        default:
            log_err("unknown binary operator");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

static LLVMValueRef gen_unary_prefix(cg_t *cg, node_t *node) {
    if (node->as.unary.op == TokPlusPlus || node->as.unary.op == TokMinusMinus) {
        node_t *operand = node->as.unary.operand;
        if (operand->kind != NodeIdentExpr) {
            log_err("prefix ++/-- requires an identifier");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", operand->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        LLVMValueRef store_ptr;
        LLVMValueRef val;
        if (sym->flags & SymHeapVar) {
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            store_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
            val = LLVMBuildLoad2(cg->builder, sym->type, store_ptr, "");
        } else {
            store_ptr = sym->value;
            val = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "");
        }
        LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
        LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
            ? LLVMBuildAdd(cg->builder, val, one, "inc")
            : LLVMBuildSub(cg->builder, val, one, "dec");
        LLVMBuildStore(cg->builder, result, store_ptr);
        return result;
    }
    if (node->as.unary.op == TokMinus) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        if (llvm_is_float(LLVMTypeOf(operand)))
            return LLVMBuildFNeg(cg->builder, operand, "fneg");
        return LLVMBuildNeg(cg->builder, operand, "neg");
    }
    if (node->as.unary.op == TokBang) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        if (LLVMTypeOf(operand) != LLVMInt1TypeInContext(cg->ctx))
            operand = LLVMBuildICmp(cg->builder, LLVMIntNE, operand,
                                    LLVMConstInt(LLVMTypeOf(operand), 0, 0), "tobool");
        return LLVMBuildNot(cg->builder, operand, "not");
    }
    if (node->as.unary.op == TokTilde) {
        LLVMValueRef operand = gen_expr(cg, node->as.unary.operand);
        return LLVMBuildNot(cg->builder, operand, "bnot");
    }
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_unary_postfix(cg_t *cg, node_t *node) {
    node_t *operand = node->as.unary.operand;
    if (operand->kind != NodeIdentExpr) {
        log_err("postfix ++/-- requires an identifier");
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
    if (!sym) {
        log_err("undefined variable '%s'", operand->as.ident.name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    LLVMValueRef store_ptr;
    LLVMValueRef val;
    if (sym->flags & SymHeapVar) {
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        store_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        val = LLVMBuildLoad2(cg->builder, sym->type, store_ptr, "");
    } else {
        store_ptr = sym->value;
        val = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "");
    }
    LLVMValueRef one = LLVMConstInt(sym->type, 1, 0);
    LLVMValueRef result = (node->as.unary.op == TokPlusPlus)
        ? LLVMBuildAdd(cg->builder, val, one, "inc")
        : LLVMBuildSub(cg->builder, val, one, "dec");
    LLVMBuildStore(cg->builder, result, store_ptr);
    return val;
}

static LLVMValueRef gen_call(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.call.callee);
    if (!sym) {
        log_err("undefined function or function pointer '%s'", node->as.call.callee);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    usize_t argc = node->as.call.args.count;
    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (argc > 0) {
        args_heap = allocate(argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        for (usize_t i = 0; i < argc; i++)
            args[i] = gen_expr(cg, node->as.call.args.items[i]);
    }

    LLVMTypeRef fn_type;
    LLVMValueRef fn_val;

    if (sym->stype.base == TypeFnPtr && sym->stype.fn_ptr_desc) {
        /* ── indirect call through a domain-tagged function pointer variable ── */
        fn_ptr_desc_t *desc = sym->stype.fn_ptr_desc;

        /* domain check: actual argument storage must match declared parameter domain */
        for (usize_t i = 0; i < argc && i < desc->param_count; i++) {
            node_t *arg_node = node->as.call.args.items[i];
            if (arg_node->kind == NodeIdentExpr) {
                symbol_t *asym = cg_lookup(cg, arg_node->as.ident.name);
                if (asym) {
                    storage_t declared = desc->params[i].storage;
                    storage_t actual   = asym->storage;
                    if (declared != StorageDefault && actual != StorageDefault
                            && declared != actual) {
                        const char *exp_s = (declared == StorageStack) ? "stack" : "heap";
                        const char *got_s = (actual   == StorageStack) ? "stack" : "heap";
                        log_err("line %lu: argument %lu to function pointer '%s' has "
                                "wrong storage domain (expected %s, got %s)",
                                node->line, (unsigned long)(i + 1),
                                sym->name, exp_s, got_s);
                    }
                }
            }
        }

        /* build the LLVM function type from the descriptor */
        fn_type = build_fn_ptr_llvm_type(cg, desc);

        /* load the actual function pointer value from the variable's alloca */
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        fn_val = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "fnptr");
    } else {
        /* ── direct call to a named function ── */
        fn_type = LLVMGlobalGetValueType(sym->value);
        fn_val  = sym->value;
    }

    /* coerce arguments to the declared parameter types (e.g. i32 literal → f32) */
    unsigned n_params = LLVMCountParamTypes(fn_type);
    if (argc > 0 && n_params > 0) {
        heap_t pt_heap = allocate(n_params, sizeof(LLVMTypeRef));
        LLVMTypeRef *param_types = pt_heap.pointer;
        LLVMGetParamTypes(fn_type, param_types);
        for (usize_t i = 0; i < argc && i < (usize_t)n_params; i++)
            args[i] = coerce_int(cg, args[i], param_types[i]);
        deallocate(pt_heap);
    }

    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_val,
                                       args, (unsigned)argc, "");
    if (argc > 0) deallocate(args_heap);
    return ret;
}

static LLVMValueRef gen_method_call(cg_t *cg, node_t *node) {
    node_t *obj = node->as.method_call.object;
    char *method = node->as.method_call.method;

    /* tagged enum variant construction: Enum.Variant(payload) */
    if (obj->kind == NodeIdentExpr) {
        enum_reg_t *er = find_enum(cg, obj->as.ident.name);
        if (er && er->is_tagged) {
            for (usize_t i = 0; i < er->variant_count; i++) {
                if (strcmp(er->variants[i].name, method) == 0) {
                    LLVMValueRef tmp = alloc_in_entry(cg, er->llvm_type, "enum_tmp");

                    /* store discriminant */
                    LLVMValueRef disc_ptr = LLVMBuildStructGEP2(
                        cg->builder, er->llvm_type, tmp, 0, "disc_ptr");
                    LLVMBuildStore(cg->builder,
                        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned long long)i, 0),
                        disc_ptr);

                    /* store payload if variant carries one */
                    if (er->variants[i].has_payload &&
                            node->as.method_call.args.count > 0) {
                        LLVMValueRef payload_val =
                            gen_expr(cg, node->as.method_call.args.items[0]);
                        LLVMTypeRef payload_lltype =
                            get_llvm_type(cg, er->variants[i].payload_type);
                        payload_val = coerce_int(cg, payload_val, payload_lltype);
                        LLVMValueRef payload_ptr = LLVMBuildStructGEP2(
                            cg->builder, er->llvm_type, tmp, 1, "payload_ptr");
                        LLVMBuildStore(cg->builder, payload_val, payload_ptr);
                    }

                    return LLVMBuildLoad2(cg->builder, er->llvm_type, tmp, "enum_val");
                }
            }
        }
    }

    /* check if object is a lib alias: alias.func(args) */
    if (obj->kind == NodeIdentExpr) {
        const char *header = find_lib_alias(cg, obj->as.ident.name);
        if (header) {
            /* generate args first so we can inspect their types */
            usize_t argc = node->as.method_call.args.count;
            heap_t args_heap = NullHeap;
            LLVMValueRef *args = Null;
            if (argc > 0) {
                args_heap = allocate(argc, sizeof(LLVMValueRef));
                args = args_heap.pointer;
                for (usize_t i = 0; i < argc; i++)
                    args[i] = gen_expr(cg, node->as.method_call.args.items[i]);
            }

            /* auto-declare C function from actual arg types */
            symbol_t *fn_sym = cg_lookup(cg, method);
            if (!fn_sym) {
                LLVMTypeRef ret_type;
                LLVMTypeRef *param_types = Null;
                heap_t ptypes_heap = NullHeap;
                unsigned param_count = 0;
                boolean_t is_varargs = False;

                if (argc > 0 &&
                    LLVMGetTypeKind(LLVMTypeOf(args[0])) == LLVMPointerTypeKind) {
                    /* first arg is a pointer → printf-like varargs */
                    LLVMTypeRef ptr_type = LLVMPointerTypeInContext(cg->ctx, 0);
                    param_types = &ptr_type;
                    param_count = 1;
                    ret_type = LLVMInt32TypeInContext(cg->ctx);
                    is_varargs = True;
                } else if (argc > 0) {
                    /* build exact signature from arg types */
                    ptypes_heap = allocate(argc, sizeof(LLVMTypeRef));
                    param_types = ptypes_heap.pointer;
                    for (usize_t i = 0; i < argc; i++)
                        param_types[i] = LLVMTypeOf(args[i]);
                    param_count = (unsigned)argc;
                    ret_type = LLVMTypeOf(args[0]);
                } else {
                    ret_type = LLVMInt32TypeInContext(cg->ctx);
                }

                LLVMTypeRef ftype = LLVMFunctionType(ret_type, param_types,
                                                      param_count, is_varargs);
                LLVMValueRef fn = LLVMAddFunction(cg->module, method, ftype);
                type_info_t dummy = {TypeI32, Null, False, PtrNone, Null};
                symtab_add(&cg->globals, method, fn, Null, dummy, False);
                fn_sym = cg_lookup(cg, method);
                if (ptypes_heap.pointer) deallocate(ptypes_heap);
            }

            LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
            LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                               args, (unsigned)argc, "");
            if (argc > 0) deallocate(args_heap);
            return ret;
        }

        /* check if it's a static method call: Type.method(args) */
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s.%s", obj->as.ident.name, method);
        symbol_t *fn_sym = cg_lookup(cg, mangled);
        if (fn_sym) {
            usize_t argc = node->as.method_call.args.count;
            heap_t args_heap = NullHeap;
            LLVMValueRef *args = Null;
            if (argc > 0) {
                args_heap = allocate(argc, sizeof(LLVMValueRef));
                args = args_heap.pointer;
                for (usize_t i = 0; i < argc; i++)
                    args[i] = gen_expr(cg, node->as.method_call.args.items[i]);
            }
            LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
            LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                               args, (unsigned)argc, "");
            if (argc > 0) deallocate(args_heap);
            return ret;
        }
    }

    /* instance method call: obj.method(args) — pass &obj as first arg */
    if (obj->kind == NodeIdentExpr) {
        symbol_t *obj_sym = cg_lookup(cg, obj->as.ident.name);
        if (obj_sym && obj_sym->stype.base == TypeUser && obj_sym->stype.user_name) {
            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s.%s", obj_sym->stype.user_name, method);
            symbol_t *fn_sym = cg_lookup(cg, mangled);
            if (fn_sym) {
                usize_t argc = node->as.method_call.args.count + 1;
                heap_t args_heap = allocate(argc, sizeof(LLVMValueRef));
                LLVMValueRef *args = args_heap.pointer;
                args[0] = obj_sym->value; /* pass pointer to struct */
                for (usize_t i = 0; i < node->as.method_call.args.count; i++)
                    args[i + 1] = gen_expr(cg, node->as.method_call.args.items[i]);
                LLVMTypeRef fn_type = LLVMGlobalGetValueType(fn_sym->value);
                LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, fn_sym->value,
                                                   args, (unsigned)argc, "");
                deallocate(args_heap);
                return ret;
            }
        }
    }

    log_err("cannot resolve method call '%s'", method);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_parallel_call(cg_t *cg, node_t *node) {
    symbol_t *sym = cg_lookup(cg, node->as.parallel_call.callee);
    if (!sym) {
        log_err("undefined function '%s'", node->as.parallel_call.callee);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    usize_t argc = node->as.parallel_call.args.count;
    LLVMValueRef *args = Null;
    heap_t args_heap = NullHeap;
    if (argc > 0) {
        args_heap = allocate(argc, sizeof(LLVMValueRef));
        args = args_heap.pointer;
        for (usize_t i = 0; i < argc; i++)
            args[i] = gen_expr(cg, node->as.parallel_call.args.items[i]);
    }
    LLVMTypeRef fn_type = LLVMGlobalGetValueType(sym->value);
    LLVMValueRef ret = LLVMBuildCall2(cg->builder, fn_type, sym->value,
                                       args, (unsigned)argc, "");
    if (argc > 0) deallocate(args_heap);
    return ret;
}

static LLVMValueRef gen_compound_assign(cg_t *cg, node_t *node) {
    node_t *target = node->as.compound_assign.target;

    /* get the storage location */
    symbol_t *sym = Null;
    LLVMValueRef store_ptr = Null;
    LLVMTypeRef store_type = Null;
    boolean_t atomic = False;

    if (target->kind == NodeIdentExpr) {
        sym = cg_lookup(cg, target->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", target->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        if (sym->flags & SymHeapVar) {
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            store_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        } else {
            store_ptr = sym->value;
        }
        store_type = sym->type;
        atomic = (sym->flags & SymAtomic) != 0;
    } else if (target->kind == NodeIndexExpr) {
        /* arr[i] compound assign */
        node_t *obj = target->as.index_expr.object;
        node_t *idx = target->as.index_expr.index;
        if (obj->kind == NodeIdentExpr) {
            sym = cg_lookup(cg, obj->as.ident.name);
            if (!sym) {
                log_err("undefined variable '%s'", obj->as.ident.name);
                return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
            LLVMValueRef index_val = gen_expr(cg, idx);
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
            LLVMValueRef indices[2] = { zero, index_val };
            LLVMTypeRef elem_type = LLVMGetElementType(sym->type);
            store_ptr = LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "idx");
            store_type = elem_type;
        }
    } else {
        log_err("compound assignment target must be assignable");
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }

    if (!store_ptr) return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);

    LLVMValueRef rhs = gen_expr(cg, node->as.compound_assign.value);
    rhs = coerce_int(cg, rhs, store_type);

    if (atomic && (node->as.compound_assign.op == TokPlusEq
                || node->as.compound_assign.op == TokMinusEq)) {
        LLVMAtomicRMWBinOp op = (node->as.compound_assign.op == TokPlusEq)
            ? LLVMAtomicRMWBinOpAdd : LLVMAtomicRMWBinOpSub;
        return LLVMBuildAtomicRMW(cg->builder, op, store_ptr, rhs,
                                   LLVMAtomicOrderingSequentiallyConsistent, 0);
    }

    LLVMValueRef lhs = LLVMBuildLoad2(cg->builder, store_type, store_ptr, "");
    boolean_t is_fp = llvm_is_float(store_type);
    LLVMValueRef result;

    switch (node->as.compound_assign.op) {
        case TokPlusEq:
            result = is_fp ? LLVMBuildFAdd(cg->builder, lhs, rhs, "fadd")
                           : LLVMBuildAdd(cg->builder, lhs, rhs, "add"); break;
        case TokMinusEq:
            result = is_fp ? LLVMBuildFSub(cg->builder, lhs, rhs, "fsub")
                           : LLVMBuildSub(cg->builder, lhs, rhs, "sub"); break;
        case TokStarEq:
            result = is_fp ? LLVMBuildFMul(cg->builder, lhs, rhs, "fmul")
                           : LLVMBuildMul(cg->builder, lhs, rhs, "mul"); break;
        case TokSlashEq:
            result = is_fp ? LLVMBuildFDiv(cg->builder, lhs, rhs, "fdiv")
                           : LLVMBuildSDiv(cg->builder, lhs, rhs, "div"); break;
        case TokPercentEq:  result = LLVMBuildSRem(cg->builder, lhs, rhs, "mod"); break;
        case TokAmpEq:      result = LLVMBuildAnd(cg->builder, lhs, rhs, "and"); break;
        case TokPipeEq:     result = LLVMBuildOr(cg->builder, lhs, rhs, "or"); break;
        case TokCaretEq:    result = LLVMBuildXor(cg->builder, lhs, rhs, "xor"); break;
        case TokLtLtEq:     result = LLVMBuildShl(cg->builder, lhs, rhs, "shl"); break;
        case TokGtGtEq:     result = LLVMBuildAShr(cg->builder, lhs, rhs, "shr"); break;
        default: result = lhs; break;
    }
    LLVMBuildStore(cg->builder, result, store_ptr);
    return result;
}

static LLVMValueRef gen_assign(cg_t *cg, node_t *node) {
    node_t *target = node->as.assign.target;

    if (target->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, target->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", target->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }

        /* cross-domain check when assigning address-of */
        if (node->as.assign.value->kind == NodeAddrOf && sym->stype.is_pointer) {
            node_t *addr_op = node->as.assign.value->as.addr_of.operand;
            if (addr_op->kind == NodeIdentExpr) {
                symbol_t *src = cg_lookup(cg, addr_op->as.ident.name);
                if (src) {
                    if (sym->storage == StorageHeap && src->storage == StorageStack)
                        log_err("line %lu: cannot assign stack address to heap pointer",
                                node->line);
                    else if (sym->storage == StorageStack && src->storage == StorageHeap)
                        log_err("line %lu: cannot assign heap address to stack pointer",
                                node->line);
                }
            }
        }

        /* pointer safety checks on assignment */
        check_const_addr_of(cg, node->as.assign.value, sym->stype, node->line);
        check_permission_widening(cg, node->as.assign.value, sym->stype, node->line);
        check_pointer_lifetime(cg, node->as.assign.value, sym->scope_depth, node->line);

        /* track nil state */
        if (node->as.assign.value->kind == NodeNilExpr)
            sym->flags |= SymNil;
        else
            sym->flags &= ~SymNil;

        LLVMValueRef rhs = gen_expr(cg, node->as.assign.value);
        rhs = coerce_int(cg, rhs, sym->type);
        if (sym->flags & SymHeapVar) {
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty,
                                                    sym->value, "hptr");
            LLVMBuildStore(cg->builder, rhs, heap_ptr);
        } else {
            LLVMBuildStore(cg->builder, rhs, sym->value);
        }
        return rhs;
    }

    LLVMValueRef rhs = gen_expr(cg, node->as.assign.value);

    if (target->kind == NodeIndexExpr) {
        node_t *obj = target->as.index_expr.object;
        node_t *idx = target->as.index_expr.index;
        LLVMValueRef index_val = gen_expr(cg, idx);

        if (obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
            if (!sym) {
                log_err("undefined variable '%s'", obj->as.ident.name);
                return rhs;
            }
            /* check if it's an array or pointer */
            if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind) {
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
                LLVMValueRef indices[2] = { zero, index_val };
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "idx");
                LLVMTypeRef elem_type = LLVMGetElementType(sym->type);
                rhs = coerce_int(cg, rhs, elem_type);
                LLVMBuildStore(cg->builder, rhs, gep);
            } else if (llvm_is_ptr(sym->type)) {
                /* pointer indexing: load pointer, then GEP with element type */
                LLVMValueRef ptr = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "ptr");
                type_info_t elem_ti = sym->stype;
                elem_ti.is_pointer = False;
                LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidx");
                rhs = coerce_int(cg, rhs, elem_ty);
                LLVMBuildStore(cg->builder, rhs, gep);
            }
            return rhs;
        }

        if (obj->kind == NodeMemberExpr) {
            /* obj.field[idx] = rhs: e.g. b.data[i] = val
               Also handles nested: obj.field1.field2[idx] = rhs */
            node_t *mobj = obj->as.member_expr.object;
            char   *mfield = obj->as.member_expr.field;

            /* resolve the base alloca and struct for the member chain */
            LLVMValueRef base_alloca = Null;
            struct_reg_t *base_sr = Null;

            if (mobj->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, mobj->as.ident.name);
                if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                    base_sr = find_struct(cg, sym->stype.user_name);
                    base_alloca = sym->value;
                }
            } else if (mobj->kind == NodeMemberExpr) {
                /* one extra level: obj.field1.field2[idx] */
                node_t *outer_obj = mobj->as.member_expr.object;
                char   *outer_field = mobj->as.member_expr.field;
                if (outer_obj->kind == NodeIdentExpr) {
                    symbol_t *sym = cg_lookup(cg, outer_obj->as.ident.name);
                    if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                        struct_reg_t *outer_sr = find_struct(cg, sym->stype.user_name);
                        if (outer_sr) {
                            for (usize_t fi = 0; fi < outer_sr->field_count; fi++) {
                                if (strcmp(outer_sr->fields[fi].name, outer_field) != 0) continue;
                                base_alloca = LLVMBuildStructGEP2(cg->builder, outer_sr->llvm_type,
                                    sym->value, (unsigned)outer_sr->fields[fi].index, outer_field);
                                if (outer_sr->fields[fi].type.base == TypeUser
                                    && outer_sr->fields[fi].type.user_name) {
                                    base_sr = find_struct(cg, outer_sr->fields[fi].type.user_name);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            if (base_alloca && base_sr) {
                for (usize_t fi = 0; fi < base_sr->field_count; fi++) {
                    if (strcmp(base_sr->fields[fi].name, mfield) != 0) continue;
                    LLVMValueRef field_gep = LLVMBuildStructGEP2(
                        cg->builder, base_sr->llvm_type, base_alloca,
                        (unsigned)base_sr->fields[fi].index, mfield);
                    LLVMTypeRef ptr_type = get_llvm_type(cg, base_sr->fields[fi].type);
                    LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, mfield);
                    type_info_t elem_ti = base_sr->fields[fi].type;
                    elem_ti.is_pointer = False;
                    LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                    index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                    LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "midx");
                    rhs = coerce_int(cg, rhs, elem_type);
                    LLVMBuildStore(cg->builder, rhs, gep);
                    return rhs;
                }
            }
        }
    }

    if (target->kind == NodeMemberExpr) {
        node_t *obj = target->as.member_expr.object;
        char *field = target->as.member_expr.field;
        if (obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
            if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
                if (sr) {
                    for (usize_t i = 0; i < sr->field_count; i++) {
                        if (strcmp(sr->fields[i].name, field) == 0) {
                            LLVMValueRef gep = LLVMBuildStructGEP2(
                                cg->builder, sr->llvm_type, sym->value,
                                (unsigned)sr->fields[i].index, field);
                            LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
                            rhs = coerce_int(cg, rhs, ft);
                            /* bitfield: read-modify-write */
                            if (sr->fields[i].bit_width > 0) {
                                int boff = sr->fields[i].bit_offset;
                                int bw   = sr->fields[i].bit_width;
                                unsigned long long mask_val = ((unsigned long long)1 << bw) - 1;
                                /* mask the new value to bit_width bits */
                                LLVMValueRef masked_new = LLVMBuildAnd(cg->builder, rhs,
                                    LLVMConstInt(ft, mask_val, 0), "bf_new_mask");
                                /* shift to position */
                                LLVMValueRef shifted_new = masked_new;
                                if (boff > 0)
                                    shifted_new = LLVMBuildShl(cg->builder, masked_new,
                                        LLVMConstInt(ft, (unsigned long long)boff, 0), "bf_shl");
                                /* load old value, clear the target bits, OR in new */
                                LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, ft, gep, "bf_old");
                                unsigned long long clear_mask = ~(mask_val << boff);
                                LLVMValueRef cleared = LLVMBuildAnd(cg->builder, old_val,
                                    LLVMConstInt(ft, clear_mask, 0), "bf_clear");
                                LLVMValueRef result = LLVMBuildOr(cg->builder, cleared,
                                    shifted_new, "bf_insert");
                                LLVMBuildStore(cg->builder, result, gep);
                                return rhs;
                            }
                            LLVMBuildStore(cg->builder, rhs, gep);
                            return rhs;
                        }
                    }
                }
            }
        }
    }

    if (target->kind == NodeSelfMemberExpr) {
        char *field = target->as.self_member.field;
        char *type_name = target->as.self_member.type_name;
        symbol_t *this_sym = cg_lookup(cg, "this");
        if (this_sym) {
            struct_reg_t *sr = find_struct(cg, type_name);
            if (sr) {
                LLVMValueRef this_ptr = this_sym->value;
                if (LLVMGetTypeKind(this_sym->type) == LLVMPointerTypeKind)
                    this_ptr = LLVMBuildLoad2(cg->builder, this_sym->type, this_sym->value, "this");
                for (usize_t i = 0; i < sr->field_count; i++) {
                    if (strcmp(sr->fields[i].name, field) == 0) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(
                            cg->builder, sr->llvm_type, this_ptr,
                            (unsigned)sr->fields[i].index, field);
                        LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
                        rhs = coerce_int(cg, rhs, ft);
                        LLVMBuildStore(cg->builder, rhs, gep);
                        return rhs;
                    }
                }
            }
        }
    }

    log_err("invalid assignment target");
    return rhs;
}

static LLVMValueRef gen_index(cg_t *cg, node_t *node) {
    node_t *obj = node->as.index_expr.object;
    LLVMValueRef index_val = gen_expr(cg, node->as.index_expr.index);

    /* null dereference check */
    if (obj->kind == NodeIdentExpr)
        check_null_deref(cg, obj->as.ident.name, node->line);

    if (obj->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", obj->as.ident.name);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        if (LLVMGetTypeKind(sym->type) == LLVMArrayTypeKind) {
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            index_val = coerce_int(cg, index_val, LLVMInt32TypeInContext(cg->ctx));
            LLVMValueRef indices[2] = { zero, index_val };
            LLVMValueRef gep = LLVMBuildGEP2(cg->builder, sym->type, sym->value, indices, 2, "idx");
            LLVMTypeRef elem_type = LLVMGetElementType(sym->type);
            return LLVMBuildLoad2(cg->builder, elem_type, gep, "elem");
        }
        if (llvm_is_ptr(sym->type)) {
            LLVMValueRef ptr = LLVMBuildLoad2(cg->builder, sym->type, sym->value, "ptr");
            /* use the declared element type for correct stride */
            type_info_t elem_ti = sym->stype;
            elem_ti.is_pointer = False;
            LLVMTypeRef elem_ty = get_llvm_type(cg, elem_ti);
            index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
            LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_ty, ptr, &index_val, 1, "pidx");
            return LLVMBuildLoad2(cg->builder, elem_ty, gep, "pelem");
        }
    }

    /* member pointer indexing: obj.field[idx], e.g. b.data[i]
       Also handles nested: obj.field1.field2[idx], e.g. c.inner.data[i] */
    if (obj->kind == NodeMemberExpr) {
        node_t *mobj = obj->as.member_expr.object;
        char   *mfield = obj->as.member_expr.field;

        LLVMValueRef base_alloca = Null;
        struct_reg_t *base_sr = Null;

        if (mobj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, mobj->as.ident.name);
            if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                base_sr = find_struct(cg, sym->stype.user_name);
                base_alloca = sym->value;
            }
        } else if (mobj->kind == NodeMemberExpr) {
            node_t *outer_obj = mobj->as.member_expr.object;
            char   *outer_field = mobj->as.member_expr.field;
            if (outer_obj->kind == NodeIdentExpr) {
                symbol_t *sym = cg_lookup(cg, outer_obj->as.ident.name);
                if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                    struct_reg_t *outer_sr = find_struct(cg, sym->stype.user_name);
                    if (outer_sr) {
                        for (usize_t fi = 0; fi < outer_sr->field_count; fi++) {
                            if (strcmp(outer_sr->fields[fi].name, outer_field) != 0) continue;
                            base_alloca = LLVMBuildStructGEP2(cg->builder, outer_sr->llvm_type,
                                sym->value, (unsigned)outer_sr->fields[fi].index, outer_field);
                            if (outer_sr->fields[fi].type.base == TypeUser
                                && outer_sr->fields[fi].type.user_name) {
                                base_sr = find_struct(cg, outer_sr->fields[fi].type.user_name);
                            }
                            break;
                        }
                    }
                }
            }
        }

        if (base_alloca && base_sr) {
            for (usize_t fi = 0; fi < base_sr->field_count; fi++) {
                if (strcmp(base_sr->fields[fi].name, mfield) != 0) continue;
                LLVMValueRef field_gep = LLVMBuildStructGEP2(
                    cg->builder, base_sr->llvm_type, base_alloca,
                    (unsigned)base_sr->fields[fi].index, mfield);
                LLVMTypeRef ptr_type = get_llvm_type(cg, base_sr->fields[fi].type);
                LLVMValueRef inner_ptr = LLVMBuildLoad2(cg->builder, ptr_type, field_gep, mfield);
                type_info_t elem_ti = base_sr->fields[fi].type;
                elem_ti.is_pointer = False;
                LLVMTypeRef elem_type = get_llvm_type(cg, elem_ti);
                index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
                LLVMValueRef gep = LLVMBuildGEP2(cg->builder, elem_type, inner_ptr, &index_val, 1, "midx");
                return LLVMBuildLoad2(cg->builder, elem_type, gep, "melem");
            }
        }
    }

    /* string literal indexing: expr[idx] where expr might be a str_lit */
    LLVMValueRef obj_val = gen_expr(cg, obj);
    if (llvm_is_ptr(LLVMTypeOf(obj_val))) {
        LLVMTypeRef i8ty = LLVMInt8TypeInContext(cg->ctx);
        index_val = coerce_int(cg, index_val, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef gep = LLVMBuildGEP2(cg->builder, i8ty, obj_val, &index_val, 1, "stridx");
        return LLVMBuildLoad2(cg->builder, i8ty, gep, "ch");
    }

    log_err("cannot index non-array/pointer type");
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_member(cg_t *cg, node_t *node) {
    node_t *obj = node->as.member_expr.object;
    char *field = node->as.member_expr.field;

    if (obj->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, obj->as.ident.name);
        if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
            struct_reg_t *sr = find_struct(cg, sym->stype.user_name);
            if (sr) {
                for (usize_t i = 0; i < sr->field_count; i++) {
                    if (strcmp(sr->fields[i].name, field) == 0) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(
                            cg->builder, sr->llvm_type, sym->value,
                            (unsigned)sr->fields[i].index, field);
                        LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
                        LLVMValueRef val = LLVMBuildLoad2(cg->builder, ft, gep, field);
                        /* bitfield: extract bits via shift+mask */
                        if (sr->fields[i].bit_width > 0) {
                            int boff = sr->fields[i].bit_offset;
                            int bw   = sr->fields[i].bit_width;
                            if (boff > 0)
                                val = LLVMBuildLShr(cg->builder, val,
                                    LLVMConstInt(ft, (unsigned long long)boff, 0), "bf_shr");
                            unsigned long long mask = ((unsigned long long)1 << bw) - 1;
                            val = LLVMBuildAnd(cg->builder, val,
                                LLVMConstInt(ft, mask, 0), "bf_mask");
                        }
                        return val;
                    }
                }
            }
        }
    }

    /* nested member: obj.field1.field2, e.g. c.inner.data */
    if (obj->kind == NodeMemberExpr) {
        node_t *outer_obj = obj->as.member_expr.object;
        char   *outer_field = obj->as.member_expr.field;
        if (outer_obj->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, outer_obj->as.ident.name);
            if (sym && sym->stype.base == TypeUser && sym->stype.user_name) {
                struct_reg_t *outer_sr = find_struct(cg, sym->stype.user_name);
                if (outer_sr) {
                    for (usize_t i = 0; i < outer_sr->field_count; i++) {
                        if (strcmp(outer_sr->fields[i].name, outer_field) != 0) continue;
                        LLVMValueRef outer_gep = LLVMBuildStructGEP2(
                            cg->builder, outer_sr->llvm_type, sym->value,
                            (unsigned)outer_sr->fields[i].index, outer_field);
                        /* outer field must be an embedded struct (non-pointer) */
                        if (outer_sr->fields[i].type.base != TypeUser
                            || outer_sr->fields[i].type.is_pointer
                            || !outer_sr->fields[i].type.user_name) break;
                        struct_reg_t *inner_sr = find_struct(cg, outer_sr->fields[i].type.user_name);
                        if (!inner_sr) break;
                        for (usize_t j = 0; j < inner_sr->field_count; j++) {
                            if (strcmp(inner_sr->fields[j].name, field) != 0) continue;
                            LLVMValueRef inner_gep = LLVMBuildStructGEP2(
                                cg->builder, inner_sr->llvm_type, outer_gep,
                                (unsigned)inner_sr->fields[j].index, field);
                            LLVMTypeRef ft = get_llvm_type(cg, inner_sr->fields[j].type);
                            return LLVMBuildLoad2(cg->builder, ft, inner_gep, field);
                        }
                        break;
                    }
                }
            }
        }
    }

    if (obj->kind == NodeIdentExpr) {
        /* might be an enum value: EnumName.Variant */
        enum_reg_t *er = find_enum(cg, obj->as.ident.name);
        if (er) {
            for (usize_t i = 0; i < er->variant_count; i++) {
                if (strcmp(er->variants[i].name, field) == 0) {
                    if (!er->is_tagged) {
                        /* C-style: return i32 constant */
                        return LLVMConstInt(er->llvm_type,
                                           (unsigned long long)er->variants[i].value, 0);
                    } else {
                        /* tagged enum: build { i32, [N x i8] } with discriminant set */
                        LLVMValueRef disc = LLVMConstInt(
                            LLVMInt32TypeInContext(cg->ctx), (unsigned long long)i, 0);
                        LLVMValueRef val = LLVMGetUndef(er->llvm_type);
                        return LLVMBuildInsertValue(cg->builder, val, disc, 0, "enum_disc");
                    }
                }
            }
        }
    }

    log_err("cannot resolve member '%s'", field);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_self_member(cg_t *cg, node_t *node) {
    /* Type.(field) — resolve to this->field */
    char *field = node->as.self_member.field;
    symbol_t *this_sym = cg_lookup(cg, "this");
    if (!this_sym) {
        log_err("self-member '%s' used outside of method", field);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    char *type_name = node->as.self_member.type_name;
    struct_reg_t *sr = find_struct(cg, type_name);
    if (!sr) {
        log_err("unknown struct '%s'", type_name);
        return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
    /* this is a pointer to struct — load it first if needed */
    LLVMValueRef this_ptr = this_sym->value;
    if (LLVMGetTypeKind(this_sym->type) == LLVMPointerTypeKind)
        this_ptr = LLVMBuildLoad2(cg->builder, this_sym->type, this_sym->value, "this");

    for (usize_t i = 0; i < sr->field_count; i++) {
        if (strcmp(sr->fields[i].name, field) == 0) {
            LLVMValueRef gep = LLVMBuildStructGEP2(
                cg->builder, sr->llvm_type, this_ptr,
                (unsigned)sr->fields[i].index, field);
            LLVMTypeRef ft = get_llvm_type(cg, sr->fields[i].type);
            return LLVMBuildLoad2(cg->builder, ft, gep, field);
        }
    }
    log_err("unknown field '%s' in struct '%s'", field, type_name);
    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
}

static LLVMValueRef gen_ternary(cg_t *cg, node_t *node) {
    LLVMValueRef cond = gen_expr(cg, node->as.ternary.cond);
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx))
        cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                             LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");

    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "tern.then");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "tern.else");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "tern.merge");
    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    LLVMValueRef then_val = gen_expr(cg, node->as.ternary.then_expr);
    LLVMBuildBr(cg->builder, merge_bb);
    then_bb = LLVMGetInsertBlock(cg->builder);

    LLVMPositionBuilderAtEnd(cg->builder, else_bb);
    LLVMValueRef else_val = gen_expr(cg, node->as.ternary.else_expr);
    LLVMBuildBr(cg->builder, merge_bb);
    else_bb = LLVMGetInsertBlock(cg->builder);

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    LLVMTypeRef result_type = LLVMTypeOf(then_val);
    else_val = coerce_int(cg, else_val, result_type);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, result_type, "ternary");
    LLVMValueRef vals[2] = { then_val, else_val };
    LLVMBasicBlockRef bbs[2] = { then_bb, else_bb };
    LLVMAddIncoming(phi, vals, bbs, 2);
    return phi;
}

static LLVMValueRef gen_cast(cg_t *cg, node_t *node) {
    LLVMValueRef val = gen_expr(cg, node->as.cast_expr.expr);
    LLVMTypeRef target = get_llvm_type(cg, node->as.cast_expr.target);
    LLVMTypeRef src = LLVMTypeOf(val);

    if (src == target) return val;

    /* int <-> ptr */
    if (llvm_is_int(src) && llvm_is_ptr(target))
        return LLVMBuildIntToPtr(cg->builder, val, target, "itoptr");
    if (llvm_is_ptr(src) && llvm_is_int(target))
        return LLVMBuildPtrToInt(cg->builder, val, target, "ptrtoi");
    /* ptr <-> ptr */
    if (llvm_is_ptr(src) && llvm_is_ptr(target))
        return val; /* opaque pointers, no-op */

    return coerce_int(cg, val, target);
}

static LLVMValueRef gen_new(cg_t *cg, node_t *node) {
    LLVMValueRef size = gen_expr(cg, node->as.new_expr.size);
    size = coerce_int(cg, size, LLVMInt64TypeInContext(cg->ctx));
    LLVMValueRef args[1] = { size };
    return LLVMBuildCall2(cg->builder, cg->malloc_type, cg->malloc_fn, args, 1, "alloc");
}

static LLVMValueRef gen_sizeof(cg_t *cg, node_t *node) {
    LLVMTypeRef ty = get_llvm_type(cg, node->as.sizeof_expr.type);
    return LLVMSizeOf(ty);
}

static LLVMValueRef gen_nil(cg_t *cg) {
    return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
}

static LLVMValueRef gen_mov(cg_t *cg, node_t *node) {
    LLVMValueRef ptr = gen_expr(cg, node->as.mov_expr.ptr);
    LLVMValueRef sz  = gen_expr(cg, node->as.mov_expr.size);
    sz = coerce_int(cg, sz, LLVMInt64TypeInContext(cg->ctx));
    LLVMValueRef args[2] = { ptr, sz };
    return LLVMBuildCall2(cg->builder, cg->realloc_type, cg->realloc_fn, args, 2, "realloc");
}

static LLVMValueRef gen_addr_of(cg_t *cg, node_t *node) {
    node_t *operand = node->as.addr_of.operand;
    if (operand->kind == NodeIdentExpr) {
        symbol_t *sym = cg_lookup(cg, operand->as.ident.name);
        if (!sym) {
            log_err("undefined variable '%s'", operand->as.ident.name);
            return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
        }
        if (sym->flags & SymHeapVar) {
            /* heap var: address is the malloc'd pointer */
            LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
            return LLVMBuildLoad2(cg->builder, ptr_ty, sym->value, "hptr");
        }
        return sym->value; /* alloca = address of the stack variable */
    }
    log_err("address-of requires an lvalue");
    return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
}

static LLVMValueRef gen_expr(cg_t *cg, node_t *node) {
    switch (node->kind) {
        case NodeIntLitExpr:       return gen_int_lit(cg, node);
        case NodeFloatLitExpr:     return gen_float_lit(cg, node);
        case NodeBoolLitExpr:      return gen_bool_lit(cg, node);
        case NodeCharLitExpr:      return gen_char_lit(cg, node);
        case NodeStrLitExpr:       return gen_str_lit(cg, node);
        case NodeIdentExpr:        return gen_ident(cg, node);
        case NodeBinaryExpr:       return gen_binary(cg, node);
        case NodeUnaryPrefixExpr:  return gen_unary_prefix(cg, node);
        case NodeUnaryPostfixExpr: return gen_unary_postfix(cg, node);
        case NodeCallExpr:         return gen_call(cg, node);
        case NodeMethodCall:       return gen_method_call(cg, node);
        case NodeParallelCall:     return gen_parallel_call(cg, node);
        case NodeCompoundAssign:   return gen_compound_assign(cg, node);
        case NodeAssignExpr:       return gen_assign(cg, node);
        case NodeIndexExpr:        return gen_index(cg, node);
        case NodeMemberExpr:       return gen_member(cg, node);
        case NodeSelfMemberExpr:   return gen_self_member(cg, node);
        case NodeTernaryExpr:      return gen_ternary(cg, node);
        case NodeCastExpr:         return gen_cast(cg, node);
        case NodeNewExpr:          return gen_new(cg, node);
        case NodeSizeofExpr:       return gen_sizeof(cg, node);
        case NodeNilExpr:          return gen_nil(cg);
        case NodeMovExpr:          return gen_mov(cg, node);
        case NodeAddrOf:           return gen_addr_of(cg, node);
        case NodeErrorExpr: {
            /* error.('message') → {i1 true, ptr message} */
            LLVMValueRef msg = gen_expr(cg, node->as.error_expr.message);
            LLVMValueRef err = LLVMGetUndef(cg->error_type);
            err = LLVMBuildInsertValue(cg->builder, err,
                LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0), 0, "err.has");
            err = LLVMBuildInsertValue(cg->builder, err, msg, 1, "err.msg");
            return err;
        }
        case NodeExpectExpr: {
            /* expect.(expr) — if !expr, print failure and increment fail count */
            LLVMValueRef val = gen_expr(cg, node->as.expect_expr.expr);
            if (LLVMTypeOf(val) != LLVMInt1TypeInContext(cg->ctx))
                val = LLVMBuildICmp(cg->builder, LLVMIntNE, val,
                    LLVMConstInt(LLVMTypeOf(val), 0, 0), "tobool");
            LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expect.pass");
            LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expect.fail");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expect.end");
            LLVMBuildCondBr(cg->builder, val, pass_bb, fail_bb);
            LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "  FAIL: expect at line %lu\n", node->line);
                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, msg, "efmt");
                LLVMValueRef args[1] = { fmt };
                LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 1, "");
                if (cg->test_fail_count) {
                    LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                    cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                    LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
                }
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, pass_bb);
            if (cg->test_pass_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_pass_count, "pc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "pi");
                LLVMBuildStore(cg->builder, cnt, cg->test_pass_count);
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeExpectEqExpr: {
            LLVMValueRef left = gen_expr(cg, node->as.expect_eq.left);
            LLVMValueRef right = gen_expr(cg, node->as.expect_eq.right);
            right = coerce_int(cg, right, LLVMTypeOf(left));
            LLVMValueRef eq = llvm_is_float(LLVMTypeOf(left))
                ? LLVMBuildFCmp(cg->builder, LLVMRealOEQ, left, right, "feq")
                : LLVMBuildICmp(cg->builder, LLVMIntEQ, left, right, "eq");
            LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expeq.pass");
            LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expeq.fail");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expeq.end");
            LLVMBuildCondBr(cg->builder, eq, pass_bb, fail_bb);
            LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "  FAIL: expect_eq at line %lu\n", node->line);
                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, msg, "eqfmt");
                LLVMValueRef args[1] = { fmt };
                LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 1, "");
                if (cg->test_fail_count) {
                    LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                    cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                    LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
                }
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, pass_bb);
            if (cg->test_pass_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_pass_count, "pc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "pi");
                LLVMBuildStore(cg->builder, cnt, cg->test_pass_count);
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeExpectNeqExpr: {
            LLVMValueRef left = gen_expr(cg, node->as.expect_neq.left);
            LLVMValueRef right = gen_expr(cg, node->as.expect_neq.right);
            right = coerce_int(cg, right, LLVMTypeOf(left));
            LLVMValueRef neq = llvm_is_float(LLVMTypeOf(left))
                ? LLVMBuildFCmp(cg->builder, LLVMRealONE, left, right, "fne")
                : LLVMBuildICmp(cg->builder, LLVMIntNE, left, right, "ne");
            LLVMBasicBlockRef pass_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expneq.pass");
            LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expneq.fail");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "expneq.end");
            LLVMBuildCondBr(cg->builder, neq, pass_bb, fail_bb);
            LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "  FAIL: expect_neq at line %lu\n", node->line);
                LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, msg, "neqfmt");
                LLVMValueRef args[1] = { fmt };
                LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 1, "");
                if (cg->test_fail_count) {
                    LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                    cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                    LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
                }
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, pass_bb);
            if (cg->test_pass_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_pass_count, "pc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "pi");
                LLVMBuildStore(cg->builder, cnt, cg->test_pass_count);
            }
            LLVMBuildBr(cg->builder, merge_bb);
            LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeTestFailExpr: {
            LLVMValueRef msg = gen_expr(cg, node->as.test_fail.message);
            LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, "  FAIL: %s\n", "tffmt");
            LLVMValueRef args[2] = { fmt, msg };
            LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 2, "");
            if (cg->test_fail_count) {
                LLVMValueRef cnt = LLVMBuildLoad2(cg->builder, LLVMInt32TypeInContext(cg->ctx), cg->test_fail_count, "fc");
                cnt = LLVMBuildAdd(cg->builder, cnt, LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0), "fi");
                LLVMBuildStore(cg->builder, cnt, cg->test_fail_count);
            }
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        case NodeDesigInit: {
            /* Type { .x = 1, .y = 2 } — designated initializer */
            struct_reg_t *sr = find_struct(cg, node->as.desig_init.type_name);
            if (!sr) {
                log_err("line %lu: unknown struct '%s' in designated initializer",
                        node->line, node->as.desig_init.type_name);
                return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
            LLVMValueRef tmp = alloc_in_entry(cg, sr->llvm_type, "desig_tmp");
            /* zero-initialize first */
            LLVMValueRef zero = LLVMConstNull(sr->llvm_type);
            LLVMBuildStore(cg->builder, zero, tmp);
            /* set each named field */
            for (usize_t di = 0; di < node->as.desig_init.fields.count; di++) {
                node_t *fname_node = node->as.desig_init.fields.items[di];
                node_t *fval_node  = node->as.desig_init.values.items[di];
                char *fname = fname_node->as.ident.name;
                for (usize_t fi = 0; fi < sr->field_count; fi++) {
                    if (strcmp(sr->fields[fi].name, fname) == 0) {
                        LLVMValueRef gep = LLVMBuildStructGEP2(
                            cg->builder, sr->llvm_type, tmp,
                            (unsigned)sr->fields[fi].index, fname);
                        LLVMTypeRef ft = get_llvm_type(cg, sr->fields[fi].type);
                        LLVMValueRef val = gen_expr(cg, fval_node);
                        val = coerce_int(cg, val, ft);
                        LLVMBuildStore(cg->builder, val, gep);
                        break;
                    }
                }
            }
            return LLVMBuildLoad2(cg->builder, sr->llvm_type, tmp, "desig_val");
        }
        case NodeRemStmt: {
            node_t *ptr_node = node->as.rem_stmt.ptr;
            /* for heap primitive vars: free the heap ptr and null the alloca */
            if (ptr_node->kind == NodeIdentExpr) {
                symbol_t *hsym = cg_lookup(cg, ptr_node->as.ident.name);
                if (hsym && (hsym->flags & SymHeapVar)) {
                    LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
                    LLVMValueRef heap_ptr = LLVMBuildLoad2(cg->builder, ptr_ty,
                                                            hsym->value, "hptr");
                    LLVMValueRef fargs[1] = { heap_ptr };
                    LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, fargs, 1, "");
                    LLVMBuildStore(cg->builder, LLVMConstNull(ptr_ty), hsym->value);
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
                }
            }
            LLVMValueRef ptr = gen_expr(cg, ptr_node);
            LLVMValueRef args[1] = { ptr };
            LLVMBuildCall2(cg->builder, cg->free_type, cg->free_fn, args, 1, "");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
        }
        default:
            log_err("unexpected node kind %d in expression", node->kind);
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

/* ── statements ── */

/* Returns True if type_info is a primitive (non-struct, non-enum) that can be heap-allocated */
static boolean_t is_primitive_type(type_info_t ti) {
    if (ti.is_pointer || ti.base == TypeVoid) return False;
    if (ti.base == TypeUser)   return False;
    if (ti.base == TypeFnPtr)  return False; /* function pointers are not heap primitives */
    return True;
}

static void gen_local_var(cg_t *cg, node_t *node) {
    /* blank identifier: silently skip — the value will be discarded */
    if (node->as.var_decl.name && strcmp(node->as.var_decl.name, "_") == 0)
        return;

    type_info_t ti = resolve_alias(cg, node->as.var_decl.type);
    LLVMTypeRef type;

    if (node->as.var_decl.flags & VdeclArray) {
        LLVMTypeRef elem = get_llvm_type(cg, ti);
        unsigned long long array_len = (unsigned long long)node->as.var_decl.array_size;
        if (node->as.var_decl.array_size_name) {
            symbol_t *sym = cg_lookup(cg, node->as.var_decl.array_size_name);
            if (sym) {
                LLVMValueRef init = LLVMGetInitializer(sym->value);
                if (init) array_len = LLVMConstIntGetZExtValue(init);
            } else {
                log_err("line %lu: undefined constant '%s' used as array size",
                        node->line, node->as.var_decl.array_size_name);
            }
        }
        type = LLVMArrayType2(elem, array_len);
    } else {
        type = get_llvm_type(cg, ti);
    }

    /* heap primitive: allocate via malloc, register for auto-free */
    boolean_t is_heap = (node->as.var_decl.storage == StorageHeap)
                        && is_primitive_type(ti)
                        && !(node->as.var_decl.flags & VdeclArray);

    if (is_heap) {
        /* alloca holds the heap pointer (alloca of ptr type, in entry block) */
        LLVMTypeRef ptr_ty = LLVMPointerTypeInContext(cg->ctx, 0);
        LLVMValueRef alloca_val = alloc_in_entry(cg, ptr_ty, node->as.var_decl.name);

        /* check cross-domain: if init is addr-of a stack variable, error */
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeAddrOf) {
            node_t *addr_op = node->as.var_decl.init->as.addr_of.operand;
            if (addr_op->kind == NodeIdentExpr) {
                symbol_t *src = cg_lookup(cg, addr_op->as.ident.name);
                if (src && src->storage == StorageStack)
                    log_err("line %lu: cannot assign stack address to heap pointer",
                            node->line);
            }
        }

        /* call malloc(sizeof(type)) */
        LLVMValueRef sz = LLVMSizeOf(type);
        sz = coerce_int(cg, sz, LLVMInt64TypeInContext(cg->ctx));
        LLVMValueRef args[1] = { sz };
        LLVMValueRef heap_ptr = LLVMBuildCall2(cg->builder, cg->malloc_type,
                                                cg->malloc_fn, args, 1, "hmalloc");
        LLVMBuildStore(cg->builder, heap_ptr, alloca_val);

        if (node->as.var_decl.init) {
            LLVMValueRef init = gen_expr(cg, node->as.var_decl.init);
            init = coerce_int(cg, init, type);
            LLVMBuildStore(cg->builder, init, heap_ptr);
        }

        symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, type,
                   ti, (node->as.var_decl.flags & VdeclAtomic) ? SymAtomic : 0);
        symtab_set_last_storage(&cg->locals, StorageHeap, True);
        symtab_set_last_extra(&cg->locals, node->as.var_decl.flags & VdeclConst,
                              node->as.var_decl.flags & VdeclFinal, node->as.var_decl.linkage,
                              cg->dtor_depth, -1);
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeNilExpr)
            symtab_set_last_nil(&cg->locals, True);

        /* DI: heap primitive — declare the alloca holding the heap ptr. */
        if (cg->debug_mode && cg->di_builder && cg->di_scope && node->line > 0) {
            LLVMMetadataRef di_pty = get_di_type(cg, ti);
            LLVMMetadataRef di_var = LLVMDIBuilderCreateAutoVariable(
                cg->di_builder, cg->di_scope,
                node->as.var_decl.name, strlen(node->as.var_decl.name),
                cg->di_file, (unsigned)node->line,
                di_pty, /* alwaysPreserve= */ 0, LLVMDIFlagZero, /* alignInBits= */ 0);
            LLVMMetadataRef di_expr =
                LLVMDIBuilderCreateExpression(cg->di_builder, Null, 0);
            LLVMMetadataRef di_loc = di_make_location(cg, node->line);
            LLVMDIBuilderInsertDeclareRecordAtEnd(
                cg->di_builder, alloca_val, di_var, di_expr, di_loc,
                LLVMGetInsertBlock(cg->builder));
        }

        add_heap_var(cg, alloca_val);
        return;
    }

    /* emit alloca in the entry block so loop variables get a fixed slot */
    LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBasicBlockRef entry_bb = LLVMGetEntryBasicBlock(cg->current_fn);
    LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_bb);
    if (first_instr)
        LLVMPositionBuilderBefore(cg->builder, first_instr);
    else
        LLVMPositionBuilderAtEnd(cg->builder, entry_bb);
    LLVMValueRef alloca_val = LLVMBuildAlloca(cg->builder, type, node->as.var_decl.name);
    LLVMPositionBuilderAtEnd(cg->builder, saved_bb);

    /* cross-domain check for stack pointer assigned a heap address (and vice-versa) */
    if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeAddrOf && ti.is_pointer) {
        node_t *addr_op = node->as.var_decl.init->as.addr_of.operand;
        if (addr_op->kind == NodeIdentExpr) {
            symbol_t *src = cg_lookup(cg, addr_op->as.ident.name);
            if (src) {
                storage_t ptr_domain = node->as.var_decl.storage;
                if (ptr_domain == StorageHeap && src->storage == StorageStack)
                    log_err("line %lu: cannot assign stack address to heap pointer",
                            node->line);
                else if (ptr_domain == StorageStack && src->storage == StorageHeap)
                    log_err("line %lu: cannot assign heap address to stack pointer",
                            node->line);
            }
        }
    }

    if (node->as.var_decl.init) {
        LLVMValueRef init;
        /* nil → error type produces a nil error struct */
        if (node->as.var_decl.init->kind == NodeNilExpr && ti.base == TypeError) {
            init = make_nil_error(cg);
        } else {
            init = gen_expr(cg, node->as.var_decl.init);
            if (!(node->as.var_decl.flags & VdeclArray))
                init = coerce_int(cg, init, type);
        }
        LLVMBuildStore(cg->builder, init, alloca_val);
    }

    {
        int sym_flags = 0;
        if (node->as.var_decl.flags & VdeclAtomic)   sym_flags |= SymAtomic;
        if (node->as.var_decl.flags & VdeclVolatile)  sym_flags |= SymVolatile;
        symtab_add(&cg->locals, node->as.var_decl.name, alloca_val, type, ti, sym_flags);
    }
    symtab_set_last_storage(&cg->locals, node->as.var_decl.storage, False);
    {
        long arr_sz = (node->as.var_decl.flags & VdeclArray) ? node->as.var_decl.array_size : -1;
        symtab_set_last_extra(&cg->locals, node->as.var_decl.flags & VdeclConst,
                              node->as.var_decl.flags & VdeclFinal, node->as.var_decl.linkage,
                              cg->dtor_depth, arr_sz);
        if (node->as.var_decl.init && node->as.var_decl.init->kind == NodeNilExpr)
            symtab_set_last_nil(&cg->locals, True);
    }

    /* pointer safety checks on initializer */
    if (node->as.var_decl.init) {
        check_const_addr_of(cg, node->as.var_decl.init, ti, node->line);
        check_permission_widening(cg, node->as.var_decl.init, ti, node->line);
        check_pointer_lifetime(cg, node->as.var_decl.init, cg->dtor_depth, node->line);
    }

    /* ── DI: local variable declaration ── */
    if (cg->debug_mode && cg->di_builder && cg->di_scope && node->line > 0) {
        LLVMMetadataRef di_vty = get_di_type(cg, ti);
        LLVMMetadataRef di_var = LLVMDIBuilderCreateAutoVariable(
            cg->di_builder, cg->di_scope,
            node->as.var_decl.name, strlen(node->as.var_decl.name),
            cg->di_file, (unsigned)node->line,
            di_vty, /* alwaysPreserve= */ 0, LLVMDIFlagZero, /* alignInBits= */ 0);
        LLVMMetadataRef di_expr =
            LLVMDIBuilderCreateExpression(cg->di_builder, Null, 0);
        LLVMMetadataRef di_loc = di_make_location(cg, node->line);
        LLVMDIBuilderInsertDeclareRecordAtEnd(
            cg->di_builder, alloca_val, di_var, di_expr, di_loc,
            LLVMGetInsertBlock(cg->builder));
    }

    /* track struct variables for destructor auto-call */
    if (ti.base == TypeUser && ti.user_name && !ti.is_pointer) {
        struct_reg_t *sr = find_struct(cg, ti.user_name);
        if (sr && sr->destructor)
            add_dtor_var(cg, alloca_val, ti.user_name);
    }
}

static void gen_for(cg_t *cg, node_t *node) {
    push_dtor_scope(cg);

    if (node->as.for_stmt.init->kind == NodeVarDecl)
        gen_local_var(cg, node->as.for_stmt.init);
    else
        gen_expr(cg, node->as.for_stmt.init);

    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.body");
    LLVMBasicBlockRef inc_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.inc");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "for.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = inc_bb;

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = gen_expr(cg, node->as.for_stmt.cond);
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx)) {
        if (llvm_is_float(LLVMTypeOf(cond)))
            cond = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond,
                                 LLVMConstReal(LLVMTypeOf(cond), 0.0), "tobool");
        else
            cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                                 LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
    }
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.for_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, inc_bb);

    LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
    gen_expr(cg, node->as.for_stmt.update);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;

    /* don't emit destructors here — pop without emitting */
    if (cg->dtor_depth > 0) {
        dtor_scope_t *scope = &cg->dtor_stack[cg->dtor_depth - 1];
        if (scope->heap.pointer != Null) deallocate(scope->heap);
        if (scope->deferred_heap.pointer != Null) deallocate(scope->deferred_heap);
        cg->dtor_depth--;
    }
}

static void gen_while(cg_t *cg, node_t *node) {
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "while.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "while.body");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "while.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = cond_bb;

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = gen_expr(cg, node->as.while_stmt.cond);
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx))
        cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                             LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.while_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;
}

static void gen_do_while(cg_t *cg, node_t *node) {
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "do.body");
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "do.cond");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "do.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = cond_bb;

    LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.do_while_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = gen_expr(cg, node->as.do_while_stmt.cond);
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx))
        cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                             LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;
}

static void gen_inf_loop(cg_t *cg, node_t *node) {
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "inf.body");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "inf.end");

    LLVMBasicBlockRef saved_break = cg->break_target;
    LLVMBasicBlockRef saved_cont  = cg->continue_target;
    cg->break_target = end_bb;
    cg->continue_target = body_bb;

    LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    gen_block(cg, node->as.inf_loop.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
    cg->continue_target = saved_cont;
}

static void gen_if(cg_t *cg, node_t *node) {
    LLVMValueRef cond = gen_expr(cg, node->as.if_stmt.cond);
    if (LLVMTypeOf(cond) != LLVMInt1TypeInContext(cg->ctx)) {
        if (llvm_is_float(LLVMTypeOf(cond)))
            cond = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond,
                                 LLVMConstReal(LLVMTypeOf(cond), 0.0), "tobool");
        else
            cond = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                                 LLVMConstInt(LLVMTypeOf(cond), 0, 0), "tobool");
    }

    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "if.then");
    LLVMBasicBlockRef else_bb = node->as.if_stmt.else_block
        ? LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "if.else") : Null;
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "if.end");

    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb ? else_bb : merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    if (node->as.if_stmt.then_block->kind == NodeBlock)
        gen_block(cg, node->as.if_stmt.then_block);
    else
        gen_stmt(cg, node->as.if_stmt.then_block);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, merge_bb);

    if (else_bb) {
        LLVMPositionBuilderAtEnd(cg->builder, else_bb);
        node_t *eb = node->as.if_stmt.else_block;
        if (eb->kind == NodeBlock)
            gen_block(cg, eb);
        else if (eb->kind == NodeIfStmt)
            gen_if(cg, eb);
        else
            gen_stmt(cg, eb);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, merge_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}

static void gen_ret(cg_t *cg, node_t *node) {
    /* pointer safety: check each return value */
    for (usize_t i = 0; i < node->as.ret_stmt.values.count; i++) {
        node_t *rv = node->as.ret_stmt.values.items[i];
        check_stack_escape(cg, rv, node->line);
        check_ext_returns_int_ptr(cg, rv, cg->current_fn_linkage, node->line);
    }

    /* If returning a named struct variable, move it out of dtor scopes so
       the destructor isn't called — the caller takes ownership. */
    if (node->as.ret_stmt.values.count == 1) {
        node_t *rv = node->as.ret_stmt.values.items[0];
        if (rv->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, rv->as.ident.name);
            if (sym && sym->stype.base == TypeUser && !sym->stype.is_pointer)
                remove_from_dtor_scopes(cg, sym->value);
        }
    }

    /* call destructors for all active scopes before returning */
    emit_all_dtor_calls(cg);

    if (node->as.ret_stmt.values.count == 0) {
        LLVMBuildRetVoid(cg->builder);
    } else if (node->as.ret_stmt.values.count == 1) {
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        LLVMValueRef val;
        /* nil returning as error → {i1=0, ptr=null} */
        if (node->as.ret_stmt.values.items[0]->kind == NodeNilExpr
            && ret_type == cg->error_type) {
            val = make_nil_error(cg);
        } else {
            val = gen_expr(cg, node->as.ret_stmt.values.items[0]);
            val = coerce_int(cg, val, ret_type);
        }
        LLVMBuildRet(cg->builder, val);
    } else {
        /* multi-return: pack into struct */
        LLVMTypeRef ret_type = LLVMGetReturnType(LLVMGlobalGetValueType(cg->current_fn));
        LLVMValueRef agg = LLVMGetUndef(ret_type);
        for (usize_t i = 0; i < node->as.ret_stmt.values.count; i++) {
            LLVMTypeRef field_type = LLVMStructGetTypeAtIndex(ret_type, (unsigned)i);
            LLVMValueRef val;
            /* nil in an error slot → {i1=0, ptr=null} */
            if (node->as.ret_stmt.values.items[i]->kind == NodeNilExpr
                && field_type == cg->error_type) {
                val = make_nil_error(cg);
            } else {
                val = gen_expr(cg, node->as.ret_stmt.values.items[i]);
                val = coerce_int(cg, val, field_type);
            }
            agg = LLVMBuildInsertValue(cg->builder, agg, val, (unsigned)i, "");
        }
        LLVMBuildRet(cg->builder, agg);
    }
}

static void gen_debug(cg_t *cg, node_t *node) {
    LLVMValueRef value = gen_expr(cg, node->as.debug_stmt.value);
    LLVMTypeRef vtype = LLVMTypeOf(value);

    const char *fmt;
    if (llvm_is_ptr(vtype))
        fmt = "%s\n";
    else if (llvm_is_float(vtype))
        fmt = "%f\n";
    else if (vtype == LLVMInt64TypeInContext(cg->ctx))
        fmt = "%lld\n";
    else if (vtype == LLVMInt8TypeInContext(cg->ctx))
        fmt = "%c\n";
    else if (vtype == LLVMInt1TypeInContext(cg->ctx))
        fmt = "%d\n";
    else
        fmt = "%d\n";

    /* promote small ints for printf */
    if (llvm_is_int(vtype) && vtype != LLVMInt32TypeInContext(cg->ctx)
        && vtype != LLVMInt64TypeInContext(cg->ctx)
        && vtype != LLVMInt8TypeInContext(cg->ctx)) {
        value = LLVMBuildSExt(cg->builder, value, LLVMInt32TypeInContext(cg->ctx), "dbgext");
    }
    if (llvm_is_float(vtype) && vtype != LLVMDoubleTypeInContext(cg->ctx)) {
        value = LLVMBuildFPExt(cg->builder, value, LLVMDoubleTypeInContext(cg->ctx), "dbgfext");
    }

    LLVMValueRef fmt_str = LLVMBuildGlobalStringPtr(cg->builder, fmt, "dbg_fmt");
    LLVMValueRef args[2] = { fmt_str, value };
    LLVMBuildCall2(cg->builder, cg->printf_type, cg->printf_fn, args, 2, "");
}

static void gen_multi_assign(cg_t *cg, node_t *node) {
    node_list_t *targets = &node->as.multi_assign.targets;
    node_list_t *values = &node->as.multi_assign.values;

    /* let binding: infer types from the callee's return type list */
    boolean_t is_let = (targets->count > 0)
        && (targets->items[0]->as.var_decl.flags & VdeclLet);

    if (is_let && values->count == 1) {
        /* Extract the callee name from the single RHS expression */
        node_t *rhs = values->items[0];
        const char *callee = Null;
        if (rhs->kind == NodeCallExpr)
            callee = rhs->as.call.callee;

        node_t *fn_decl = callee ? find_fn_decl(cg, callee) : Null;

        if (!fn_decl || fn_decl->as.fn_decl.return_count < 1) {
            log_err("line %lu: 'let' binding requires a multi-return function call",
                    node->line);
            return;
        }

        /* Assign inferred types to each non-blank target */
        for (usize_t i = 0; i < targets->count; i++) {
            node_t *tgt = targets->items[i];
            if (tgt->as.var_decl.name && strcmp(tgt->as.var_decl.name, "_") == 0)
                continue;
            if (i < fn_decl->as.fn_decl.return_count)
                tgt->as.var_decl.type = fn_decl->as.fn_decl.return_types[i];
            else
                tgt->as.var_decl.type = NO_TYPE;
        }
    }

    /* first, declare all target variables */
    for (usize_t i = 0; i < targets->count; i++)
        gen_local_var(cg, targets->items[i]);

    if (values->count == 1 && targets->count > 1) {
        /* single call returning multi-value: unpack struct */
        LLVMValueRef ret_val = gen_expr(cg, values->items[0]);
        for (usize_t i = 0; i < targets->count; i++) {
            LLVMValueRef elem = LLVMBuildExtractValue(cg->builder, ret_val, (unsigned)i, "");
            symbol_t *sym = cg_lookup(cg, targets->items[i]->as.var_decl.name);
            if (sym) {
                elem = coerce_int(cg, elem, sym->type);
                LLVMBuildStore(cg->builder, elem, sym->value);
            }
        }
    } else {
        /* multiple values: assign 1:1 */
        usize_t count = targets->count < values->count ? targets->count : values->count;
        for (usize_t i = 0; i < count; i++) {
            LLVMValueRef val = gen_expr(cg, values->items[i]);
            symbol_t *sym = cg_lookup(cg, targets->items[i]->as.var_decl.name);
            if (sym) {
                val = coerce_int(cg, val, sym->type);
                LLVMBuildStore(cg->builder, val, sym->value);
            }
        }
    }
}

static void gen_match(cg_t *cg, node_t *node) {
    node_t *subject_node = node->as.match_stmt.expr;
    usize_t arm_count = node->as.match_stmt.arms.count;
    if (arm_count == 0) return;

    /* find enum registry from the first non-wildcard arm */
    enum_reg_t *er = Null;
    for (usize_t i = 0; i < arm_count; i++) {
        node_t *arm = node->as.match_stmt.arms.items[i];
        if (!arm->as.match_arm.is_wildcard && arm->as.match_arm.enum_name) {
            er = find_enum(cg, arm->as.match_arm.enum_name);
            break;
        }
    }

    /* get subject alloca pointer (for payload extraction in tagged enums) */
    LLVMValueRef subject_alloca = Null;
    LLVMValueRef discriminant;

    if (er && er->is_tagged) {
        if (subject_node->kind == NodeIdentExpr) {
            symbol_t *sym = cg_lookup(cg, subject_node->as.ident.name);
            if (sym) subject_alloca = sym->value;
        }
        if (!subject_alloca) {
            /* expression result: store into temp alloca for GEP access */
            LLVMValueRef val = gen_expr(cg, subject_node);
            subject_alloca = alloc_in_entry(cg, er->llvm_type, "match_subj");
            LLVMBuildStore(cg->builder, val, subject_alloca);
        }
        LLVMValueRef disc_ptr = LLVMBuildStructGEP2(
            cg->builder, er->llvm_type, subject_alloca, 0, "disc_ptr");
        discriminant = LLVMBuildLoad2(
            cg->builder, LLVMInt32TypeInContext(cg->ctx), disc_ptr, "disc");
    } else {
        /* C-style enum or unknown: subject is already an integer discriminant */
        discriminant = gen_expr(cg, subject_node);
        discriminant = coerce_int(cg, discriminant, LLVMInt32TypeInContext(cg->ctx));
    }

    /* collect wildcard arm and count cases */
    node_t *wildcard_arm = Null;
    unsigned case_count = 0;
    for (usize_t i = 0; i < arm_count; i++) {
        node_t *arm = node->as.match_stmt.arms.items[i];
        if (arm->as.match_arm.is_wildcard)
            wildcard_arm = arm;
        else
            case_count++;
    }

    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "match.end");
    LLVMBasicBlockRef default_bb = end_bb;

    if (wildcard_arm) {
        default_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->current_fn, "match.default");
    }

    LLVMValueRef sw = LLVMBuildSwitch(cg->builder, discriminant, default_bb, case_count);

    /* generate each non-wildcard arm */
    for (usize_t i = 0; i < arm_count; i++) {
        node_t *arm = node->as.match_stmt.arms.items[i];
        if (arm->as.match_arm.is_wildcard) continue;

        /* find variant discriminant value */
        long disc_val = -1;
        variant_info_t *vi = Null;
        enum_reg_t *arm_er = er ? er : find_enum(cg, arm->as.match_arm.enum_name);
        if (arm_er) {
            for (usize_t j = 0; j < arm_er->variant_count; j++) {
                if (strcmp(arm_er->variants[j].name, arm->as.match_arm.variant_name) == 0) {
                    disc_val = arm_er->variants[j].value;
                    vi = &arm_er->variants[j];
                    break;
                }
            }
        }
        if (disc_val < 0) {
            log_err("line %lu: unknown variant '%s'",
                    arm->line, arm->as.match_arm.variant_name);
            continue;
        }

        LLVMBasicBlockRef arm_bb =
            LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "match.arm");
        LLVMAddCase(sw,
            LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                         (unsigned long long)disc_val, 0),
            arm_bb);
        LLVMPositionBuilderAtEnd(cg->builder, arm_bb);

        /* scope: save locals count so binding is invisible after this arm */
        usize_t saved_locals = cg->locals.count;

        /* bind payload value if requested */
        if (arm->as.match_arm.bind_name && vi && vi->has_payload && subject_alloca) {
            LLVMValueRef payload_ptr = LLVMBuildStructGEP2(
                cg->builder, arm_er->llvm_type, subject_alloca, 1, "payload_ptr");
            LLVMTypeRef payload_lltype = get_llvm_type(cg, vi->payload_type);
            LLVMValueRef payload_val = LLVMBuildLoad2(
                cg->builder, payload_lltype, payload_ptr, arm->as.match_arm.bind_name);
            LLVMValueRef bind_alloca =
                alloc_in_entry(cg, payload_lltype, arm->as.match_arm.bind_name);
            LLVMBuildStore(cg->builder, payload_val, bind_alloca);
            symtab_add(&cg->locals, arm->as.match_arm.bind_name,
                       bind_alloca, payload_lltype, vi->payload_type, False);
        }

        push_dtor_scope(cg);
        for (usize_t s = 0; s < arm->as.match_arm.body->as.block.stmts.count; s++)
            gen_stmt(cg, arm->as.match_arm.body->as.block.stmts.items[s]);
        pop_dtor_scope(cg);

        cg->locals.count = saved_locals;

        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, end_bb);
    }

    /* generate wildcard / default arm */
    if (wildcard_arm) {
        LLVMPositionBuilderAtEnd(cg->builder, default_bb);
        push_dtor_scope(cg);
        for (usize_t s = 0; s < wildcard_arm->as.match_arm.body->as.block.stmts.count; s++)
            gen_stmt(cg, wildcard_arm->as.match_arm.body->as.block.stmts.items[s]);
        pop_dtor_scope(cg);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, end_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
}

static void gen_switch(cg_t *cg, node_t *node) {
    LLVMValueRef subject = gen_expr(cg, node->as.switch_stmt.expr);
    subject = coerce_int(cg, subject, LLVMInt32TypeInContext(cg->ctx));

    usize_t case_count = node->as.switch_stmt.cases.count;
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "sw.end");

    /* find default case */
    LLVMBasicBlockRef default_bb = end_bb;
    for (usize_t i = 0; i < case_count; i++) {
        node_t *c = node->as.switch_stmt.cases.items[i];
        if (c->as.switch_case.is_default) {
            default_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "sw.default");
            break;
        }
    }

    /* count non-default cases */
    unsigned num_cases = 0;
    for (usize_t i = 0; i < case_count; i++) {
        node_t *c = node->as.switch_stmt.cases.items[i];
        if (!c->as.switch_case.is_default)
            num_cases += (unsigned)c->as.switch_case.values.count;
    }

    LLVMValueRef sw = LLVMBuildSwitch(cg->builder, subject, default_bb, num_cases);

    LLVMBasicBlockRef saved_break = cg->break_target;
    cg->break_target = end_bb;

    for (usize_t i = 0; i < case_count; i++) {
        node_t *c = node->as.switch_stmt.cases.items[i];

        if (c->as.switch_case.is_default) {
            LLVMPositionBuilderAtEnd(cg->builder, default_bb);
            push_dtor_scope(cg);
            if (c->as.switch_case.body)
                gen_block(cg, c->as.switch_case.body);
            pop_dtor_scope(cg);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
                LLVMBuildBr(cg->builder, end_bb);
            continue;
        }

        LLVMBasicBlockRef case_bb =
            LLVMAppendBasicBlockInContext(cg->ctx, cg->current_fn, "sw.case");

        /* add each case value to the switch */
        for (usize_t v = 0; v < c->as.switch_case.values.count; v++) {
            node_t *val_node = c->as.switch_case.values.items[v];
            LLVMValueRef case_val;
            if (val_node->kind == NodeIntLitExpr)
                case_val = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                    (unsigned long long)val_node->as.int_lit.value, 1);
            else
                case_val = coerce_int(cg, gen_expr(cg, val_node),
                    LLVMInt32TypeInContext(cg->ctx));
            LLVMAddCase(sw, case_val, case_bb);
        }

        LLVMPositionBuilderAtEnd(cg->builder, case_bb);
        push_dtor_scope(cg);
        if (c->as.switch_case.body)
            gen_block(cg, c->as.switch_case.body);
        pop_dtor_scope(cg);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, end_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg->break_target = saved_break;
}

static void gen_asm_stmt(cg_t *cg, node_t *node) {
    char *code = node->as.asm_stmt.code;
    char *constraints = node->as.asm_stmt.constraints;
    if (!constraints) constraints = "";

    usize_t argc = node->as.asm_stmt.operands.count;
    LLVMTypeRef *arg_types = Null;
    LLVMValueRef *args = Null;
    heap_t at_heap = NullHeap, a_heap = NullHeap;
    if (argc > 0) {
        at_heap = allocate(argc, sizeof(LLVMTypeRef));
        a_heap  = allocate(argc, sizeof(LLVMValueRef));
        arg_types = at_heap.pointer;
        args      = a_heap.pointer;
        for (usize_t i = 0; i < argc; i++) {
            args[i] = gen_expr(cg, node->as.asm_stmt.operands.items[i]);
            arg_types[i] = LLVMTypeOf(args[i]);
        }
    }

    LLVMTypeRef fn_type = LLVMFunctionType(
        LLVMVoidTypeInContext(cg->ctx), arg_types, (unsigned)argc, 0);
    LLVMValueRef asm_val = LLVMGetInlineAsm(
        fn_type, code, strlen(code),
        constraints, strlen(constraints),
        1 /* has side effects */, 0 /* not align stack */,
        LLVMInlineAsmDialectATT, 0 /* can throw */);
    LLVMBuildCall2(cg->builder, fn_type, asm_val, args, (unsigned)argc, "");

    if (argc > 0) { deallocate(at_heap); deallocate(a_heap); }
}

static void gen_comptime_if(cg_t *cg, node_t *node) {
    char *key = node->as.comptime_if.key;
    char *value = node->as.comptime_if.value;
    boolean_t match = False;

    if (strcmp(key, "platform") == 0 || strcmp(key, "os") == 0) {
#if defined(__APPLE__)
        match = (strcmp(value, "macos") == 0 || strcmp(value, "darwin") == 0);
#elif defined(__linux__)
        match = (strcmp(value, "linux") == 0);
#elif defined(_WIN32)
        match = (strcmp(value, "windows") == 0);
#endif
    } else if (strcmp(key, "arch") == 0) {
#if defined(__x86_64__) || defined(_M_X64)
        match = (strcmp(value, "x86_64") == 0 || strcmp(value, "amd64") == 0);
#elif defined(__aarch64__) || defined(_M_ARM64)
        match = (strcmp(value, "aarch64") == 0 || strcmp(value, "arm64") == 0);
#elif defined(__i386__) || defined(_M_IX86)
        match = (strcmp(value, "x86") == 0 || strcmp(value, "i386") == 0);
#endif
    }

    if (match) {
        if (node->as.comptime_if.body)
            gen_block(cg, node->as.comptime_if.body);
    } else {
        if (node->as.comptime_if.else_body)
            gen_block(cg, node->as.comptime_if.else_body);
    }
}

static void gen_comptime_assert(cg_t *cg, node_t *node) {
    /* compile-time assertion: evaluate constant expression */
    node_t *expr = node->as.comptime_assert.expr;
    char *message = node->as.comptime_assert.message;

    /* try to evaluate simple constant comparisons */
    if (expr->kind == NodeBinaryExpr && expr->as.binary.op == TokEqEq) {
        node_t *l = expr->as.binary.left;
        node_t *r = expr->as.binary.right;
        if (l->kind == NodeSizeofExpr && r->kind == NodeIntLitExpr) {
            LLVMTypeRef ty = get_llvm_type(cg, l->as.sizeof_expr.type);
            unsigned long long sz = LLVMABISizeOfType(
                LLVMGetModuleDataLayout(cg->module), ty);
            if (sz != (unsigned long long)r->as.int_lit.value) {
                if (message)
                    log_err("comptime_assert failed: %s (sizeof = %llu, expected %ld)",
                            message, sz, r->as.int_lit.value);
                else
                    log_err("comptime_assert failed: sizeof = %llu, expected %ld",
                            sz, r->as.int_lit.value);
            }
            return;
        }
        /* int == int */
        if (l->kind == NodeIntLitExpr && r->kind == NodeIntLitExpr) {
            if (l->as.int_lit.value != r->as.int_lit.value) {
                if (message)
                    log_err("comptime_assert failed: %s (%ld != %ld)",
                            message, l->as.int_lit.value, r->as.int_lit.value);
                else
                    log_err("comptime_assert failed: %ld != %ld",
                            l->as.int_lit.value, r->as.int_lit.value);
            }
            return;
        }
    }
    /* boolean literal */
    if (expr->kind == NodeBoolLitExpr) {
        if (!expr->as.bool_lit.value) {
            if (message)
                log_err("comptime_assert failed: %s", message);
            else
                log_err("comptime_assert failed");
        }
        return;
    }
    log_err("comptime_assert: expression too complex to evaluate at compile time");
}

static void gen_stmt(cg_t *cg, node_t *node) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        return;

    /* Attach source-line location to every instruction emitted for this
       statement.  Skipped for declarations handled by gen_local_var (which
       sets its own location) and for pure compile-time nodes that emit no IR. */
    if (cg->debug_mode && node->line > 0
        && node->kind != NodeVarDecl
        && node->kind != NodeComptimeIf
        && node->kind != NodeComptimeAssert)
        di_set_location(cg, node->line);

    switch (node->kind) {
        case NodeVarDecl:      gen_local_var(cg, node); break;
        case NodeMultiAssign:  gen_multi_assign(cg, node); break;
        case NodeForStmt:      gen_for(cg, node); break;
        case NodeWhileStmt:    gen_while(cg, node); break;
        case NodeDoWhileStmt:  gen_do_while(cg, node); break;
        case NodeInfLoop:      gen_inf_loop(cg, node); break;
        case NodeIfStmt:       gen_if(cg, node); break;
        case NodeRetStmt:      gen_ret(cg, node); break;
        case NodeDebugStmt:    gen_debug(cg, node); break;
        case NodeExprStmt:     gen_expr(cg, node->as.expr_stmt.expr); break;
        case NodeRemStmt:      gen_expr(cg, node); break;
        case NodeMatchStmt:    gen_match(cg, node); break;
        case NodeSwitchStmt:   gen_switch(cg, node); break;
        case NodeAsmStmt:      gen_asm_stmt(cg, node); break;
        case NodeComptimeIf:   gen_comptime_if(cg, node); break;
        case NodeComptimeAssert: gen_comptime_assert(cg, node); break;
        case NodeBreakStmt:
            if (cg->break_target)
                LLVMBuildBr(cg->builder, cg->break_target);
            else
                log_err("break outside of loop");
            break;
        case NodeContinueStmt:
            if (cg->continue_target)
                LLVMBuildBr(cg->builder, cg->continue_target);
            else
                log_err("continue outside of loop");
            break;
        case NodeDeferStmt:
            add_deferred_stmt(cg, node->as.defer_stmt.body);
            break;
        case NodeBlock:
            gen_block(cg, node);
            break;
        default:
            log_err("unexpected statement kind %d", node->kind);
            break;
    }
}

static void gen_block(cg_t *cg, node_t *node) {
    push_dtor_scope(cg);
    for (usize_t i = 0; i < node->as.block.stmts.count; i++)
        gen_stmt(cg, node->as.block.stmts.items[i]);
    pop_dtor_scope(cg);
}

/* ── registry helpers ── */

static void register_struct(cg_t *cg, const char *name, LLVMTypeRef llvm_type,
                             boolean_t is_union) {
    if (cg->struct_count >= cg->struct_cap) {
        usize_t new_cap = cg->struct_cap < 8 ? 8 : cg->struct_cap * 2;
        if (cg->structs_heap.pointer == Null)
            cg->structs_heap = allocate(new_cap, sizeof(struct_reg_t));
        else
            cg->structs_heap = reallocate(cg->structs_heap, new_cap * sizeof(struct_reg_t));
        cg->structs = cg->structs_heap.pointer;
        cg->struct_cap = new_cap;
    }
    struct_reg_t *sr = &cg->structs[cg->struct_count++];
    sr->name = (char *)name;
    sr->llvm_type = llvm_type;
    sr->fields = Null;
    sr->field_count = 0;
    sr->field_capacity = 0;
    sr->fields_heap = NullHeap;
    sr->destructor = Null;
    sr->is_union = is_union;
}

static void struct_add_field_ex(struct_reg_t *sr, const char *name, type_info_t type,
                               usize_t index, linkage_t linkage, storage_t storage,
                               int bit_offset, int bit_width) {
    if (sr->field_count >= sr->field_capacity) {
        usize_t new_cap = sr->field_capacity < 8 ? 8 : sr->field_capacity * 2;
        if (sr->fields_heap.pointer == Null)
            sr->fields_heap = allocate(new_cap, sizeof(field_info_t));
        else
            sr->fields_heap = reallocate(sr->fields_heap, new_cap * sizeof(field_info_t));
        sr->fields = sr->fields_heap.pointer;
        sr->field_capacity = new_cap;
    }
    sr->fields[sr->field_count].name    = (char *)name;
    sr->fields[sr->field_count].type    = type;
    sr->fields[sr->field_count].storage = storage;
    sr->fields[sr->field_count].index = index;
    sr->fields[sr->field_count].linkage = linkage;
    sr->fields[sr->field_count].bit_offset = bit_offset;
    sr->fields[sr->field_count].bit_width  = bit_width;
    sr->field_count++;
}

static void struct_add_field(struct_reg_t *sr, const char *name, type_info_t type,
                             usize_t index, linkage_t linkage, storage_t storage) {
    struct_add_field_ex(sr, name, type, index, linkage, storage, 0, 0);
}

static void register_enum(cg_t *cg, const char *name, node_list_t *variants) {
    if (cg->enum_count >= cg->enum_cap) {
        usize_t new_cap = cg->enum_cap < 8 ? 8 : cg->enum_cap * 2;
        if (cg->enums_heap.pointer == Null)
            cg->enums_heap = allocate(new_cap, sizeof(enum_reg_t));
        else
            cg->enums_heap = reallocate(cg->enums_heap, new_cap * sizeof(enum_reg_t));
        cg->enums = cg->enums_heap.pointer;
        cg->enum_cap = new_cap;
    }
    enum_reg_t *er = &cg->enums[cg->enum_count++];
    er->name = (char *)name;
    er->is_tagged = False;
    er->variant_count = variants->count;

    if (variants->count > 0) {
        er->variants_heap = allocate(variants->count, sizeof(variant_info_t));
        er->variants = er->variants_heap.pointer;

        /* determine if any variant has a payload (tagged enum) */
        usize_t max_payload = 0;
        for (usize_t i = 0; i < variants->count; i++) {
            node_t *v = variants->items[i];
            er->variants[i].name = v->as.enum_variant.name;
            er->variants[i].value = (long)i;
            er->variants[i].has_payload = v->as.enum_variant.has_payload;
            er->variants[i].payload_type = v->as.enum_variant.payload_type;
            if (v->as.enum_variant.has_payload) {
                er->is_tagged = True;
                usize_t sz = payload_type_size(v->as.enum_variant.payload_type);
                if (sz > max_payload) max_payload = sz;
            }
        }

        if (er->is_tagged) {
            /* tagged union: { i32, [max_payload x i8] } */
            LLVMTypeRef byte_arr = LLVMArrayType2(
                LLVMInt8TypeInContext(cg->ctx), (unsigned long long)max_payload);
            LLVMTypeRef fields[2] = { LLVMInt32TypeInContext(cg->ctx), byte_arr };
            er->llvm_type = LLVMStructTypeInContext(cg->ctx, fields, 2, 0);
        } else {
            er->llvm_type = LLVMInt32TypeInContext(cg->ctx);
        }
    } else {
        er->variants_heap = NullHeap;
        er->variants = Null;
        er->llvm_type = LLVMInt32TypeInContext(cg->ctx);
    }
}

static void register_alias(cg_t *cg, const char *name, type_info_t actual) {
    if (cg->alias_count >= cg->alias_cap) {
        usize_t new_cap = cg->alias_cap < 8 ? 8 : cg->alias_cap * 2;
        if (cg->aliases_heap.pointer == Null)
            cg->aliases_heap = allocate(new_cap, sizeof(type_alias_t));
        else
            cg->aliases_heap = reallocate(cg->aliases_heap, new_cap * sizeof(type_alias_t));
        cg->aliases = cg->aliases_heap.pointer;
        cg->alias_cap = new_cap;
    }
    cg->aliases[cg->alias_count].name = (char *)name;
    cg->aliases[cg->alias_count].actual = actual;
    cg->alias_count++;
}

static void register_lib(cg_t *cg, const char *name, const char *alias,
                          const char *path) {
    if (cg->lib_count >= cg->lib_cap) {
        usize_t new_cap = cg->lib_cap < 8 ? 8 : cg->lib_cap * 2;
        if (cg->libs_heap.pointer == Null)
            cg->libs_heap = allocate(new_cap, sizeof(lib_entry_t));
        else
            cg->libs_heap = reallocate(cg->libs_heap, new_cap * sizeof(lib_entry_t));
        cg->libs = cg->libs_heap.pointer;
        cg->lib_cap = new_cap;
    }
    cg->libs[cg->lib_count].name  = (char *)name;
    cg->libs[cg->lib_count].alias = (char *)alias;
    cg->libs[cg->lib_count].path  = (char *)path;
    cg->lib_count++;
}

/* ── DWARF debug info helpers ── */

/* Look up a cached DI type by name. Returns Null if not found. */
static LLVMMetadataRef di_cache_lookup(cg_t *cg, const char *name) {
    for (usize_t i = 0; i < cg->di_type_count; i++)
        if (strcmp(cg->di_types[i].name, name) == 0)
            return cg->di_types[i].di_type;
    return Null;
}

/* Insert or update a DI type in the cache. */
static void di_cache_set(cg_t *cg, const char *name, LLVMMetadataRef di_type) {
    for (usize_t i = 0; i < cg->di_type_count; i++) {
        if (strcmp(cg->di_types[i].name, name) == 0) {
            cg->di_types[i].di_type = di_type;
            return;
        }
    }
    if (cg->di_type_count >= cg->di_type_cap) {
        usize_t new_cap = cg->di_type_cap < 8 ? 8 : cg->di_type_cap * 2;
        if (cg->di_types_heap.pointer == Null)
            cg->di_types_heap = allocate(new_cap, sizeof(di_type_entry_t));
        else
            cg->di_types_heap = reallocate(cg->di_types_heap,
                                            new_cap * sizeof(di_type_entry_t));
        cg->di_types = cg->di_types_heap.pointer;
        cg->di_type_cap = new_cap;
    }
    cg->di_types[cg->di_type_count].name    = (char *)name;
    cg->di_types[cg->di_type_count].di_type = di_type;
    cg->di_type_count++;
}

/* Forward declaration so get_di_named_type can call get_di_type recursively. */
static LLVMMetadataRef get_di_type(cg_t *cg, type_info_t ti);

/*
 * Build (or retrieve from cache) the DWARF composite type for a named
 * user-defined type (struct, union, or enum).
 *
 * To break potential circular references (e.g. a struct with a pointer
 * field to itself), a sentinel unspecified-type is inserted into the cache
 * before members are processed; cycles get the sentinel rather than crashing.
 */
static LLVMMetadataRef get_di_named_type(cg_t *cg, const char *name) {
    if (!name) return Null;

    LLVMMetadataRef cached = di_cache_lookup(cg, name);
    if (cached) return cached;

    /* Sentinel: break cycles by inserting a placeholder immediately. */
    LLVMMetadataRef sentinel =
        LLVMDIBuilderCreateUnspecifiedType(cg->di_builder, name, strlen(name));
    di_cache_set(cg, name, sentinel);

    /* ── struct / union ── */
    struct_reg_t *sr = find_struct(cg, name);
    if (sr) {
        uint64_t size_bits  = 0;
        uint32_t align_bits = 0;
        if (cg->di_data_layout) {
            size_bits  = LLVMABISizeOfType(cg->di_data_layout, sr->llvm_type) * 8;
            align_bits = (uint32_t)(LLVMABIAlignmentOfType(cg->di_data_layout,
                                                            sr->llvm_type) * 8);
        }

        usize_t member_count = sr->field_count;
        heap_t  members_heap = NullHeap;
        LLVMMetadataRef *members = Null;

        if (member_count > 0) {
            members_heap = allocate(member_count, sizeof(LLVMMetadataRef));
            members = members_heap.pointer;

            for (usize_t m = 0; m < member_count; m++) {
                field_info_t *f    = &sr->fields[m];
                LLVMMetadataRef mty = get_di_type(cg, f->type);

                if (f->bit_width > 0) {
                    /* Bitfield: storage offset is the offset of the backing
                       integer field; bit offset is position within that integer. */
                    uint64_t storage_off_bits = 0;
                    if (cg->di_data_layout)
                        storage_off_bits =
                            LLVMOffsetOfElement(cg->di_data_layout,
                                                sr->llvm_type,
                                                (unsigned)f->index) * 8;
                    uint64_t field_off_bits =
                        storage_off_bits + (uint64_t)f->bit_offset;

                    members[m] = LLVMDIBuilderCreateBitFieldMemberType(
                        cg->di_builder, cg->di_compile_unit,
                        f->name, strlen(f->name),
                        cg->di_file, 0,
                        (uint64_t)f->bit_width,
                        field_off_bits,
                        storage_off_bits,
                        LLVMDIFlagZero, mty);
                } else {
                    /* Normal field. */
                    uint64_t offset_bits      = 0;
                    uint64_t member_size_bits = 0;
                    uint32_t member_align_bits = 0;
                    if (cg->di_data_layout) {
                        LLVMTypeRef mllvm = get_llvm_type(cg, f->type);
                        offset_bits      = LLVMOffsetOfElement(cg->di_data_layout,
                                                               sr->llvm_type,
                                                               (unsigned)f->index) * 8;
                        member_size_bits = LLVMABISizeOfType(cg->di_data_layout,
                                                             mllvm) * 8;
                        member_align_bits = (uint32_t)(
                            LLVMABIAlignmentOfType(cg->di_data_layout, mllvm) * 8);
                    }
                    /* Unions: all fields start at offset 0. */
                    if (sr->is_union) offset_bits = 0;

                    members[m] = LLVMDIBuilderCreateMemberType(
                        cg->di_builder, cg->di_compile_unit,
                        f->name, strlen(f->name),
                        cg->di_file, 0,
                        member_size_bits, member_align_bits,
                        offset_bits,
                        LLVMDIFlagZero, mty);
                }
            }
        }

        LLVMMetadataRef di_composite;
        if (sr->is_union) {
            di_composite = LLVMDIBuilderCreateUnionType(
                cg->di_builder, cg->di_compile_unit,
                name, strlen(name),
                cg->di_file, 0,
                size_bits, align_bits,
                LLVMDIFlagZero,
                members, (unsigned)member_count,
                0, "", 0);
        } else {
            di_composite = LLVMDIBuilderCreateStructType(
                cg->di_builder, cg->di_compile_unit,
                name, strlen(name),
                cg->di_file, 0,
                size_bits, align_bits,
                LLVMDIFlagZero,
                Null,  /* no base struct */
                members, (unsigned)member_count,
                0, Null, /* no runtime lang, no vtable */
                "", 0);
        }

        if (member_count > 0) deallocate(members_heap);
        di_cache_set(cg, name, di_composite);
        return di_composite;
    }

    /* ── enum ── */
    enum_reg_t *er = find_enum(cg, name);
    if (er) {
        usize_t var_count = er->variant_count;
        heap_t  vars_heap = NullHeap;
        LLVMMetadataRef *vars = Null;

        if (var_count > 0) {
            vars_heap = allocate(var_count, sizeof(LLVMMetadataRef));
            vars = vars_heap.pointer;
            for (usize_t v = 0; v < var_count; v++) {
                variant_info_t *vi = &er->variants[v];
                vars[v] = LLVMDIBuilderCreateEnumerator(
                    cg->di_builder,
                    vi->name, strlen(vi->name),
                    vi->value, /* isUnsigned= */ 0);
            }
        }

        uint64_t size_bits = 32; /* simple enums are i32 */
        if (er->is_tagged && cg->di_data_layout)
            size_bits = LLVMABISizeOfType(cg->di_data_layout, er->llvm_type) * 8;

        /* Underlying integer type for the enum tag. */
        LLVMMetadataRef di_base = LLVMDIBuilderCreateBasicType(
            cg->di_builder, "i32", 3, 32,
            STS_DW_ATE_signed, LLVMDIFlagZero);

        LLVMMetadataRef di_enum = LLVMDIBuilderCreateEnumerationType(
            cg->di_builder, cg->di_compile_unit,
            name, strlen(name),
            cg->di_file, 0,
            size_bits, 32,
            vars, (unsigned)var_count,
            di_base);

        if (var_count > 0) deallocate(vars_heap);
        di_cache_set(cg, name, di_enum);
        return di_enum;
    }

    /* Unknown user type: keep the sentinel unspecified type. */
    return sentinel;
}

/*
 * Convert a Stasha type_info_t to an LLVMMetadataRef DWARF type.
 * Returns Null for void (which is how DWARF represents void).
 */
static LLVMMetadataRef get_di_type(cg_t *cg, type_info_t ti) {
    if (!cg->debug_mode) return Null;

    type_info_t resolved = resolve_alias(cg, ti);

    if (resolved.is_pointer) {
        /* Pointee type (de-reference the pointer flag). */
        type_info_t pointee   = resolved;
        pointee.is_pointer    = False;
        LLVMMetadataRef inner = get_di_type(cg, pointee);
        /* Pointer width: 64-bit on all currently supported targets. */
        return LLVMDIBuilderCreatePointerType(
            cg->di_builder, inner, 64, 0, 0, "", 0);
    }

    switch (resolved.base) {
        case TypeVoid:
            return Null;
        case TypeBool:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "bool", 4, 1,
                STS_DW_ATE_boolean, LLVMDIFlagZero);
        case TypeI8:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i8",  2,  8,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeI16:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i16", 3, 16,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeI32:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i32", 3, 32,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeI64:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "i64", 3, 64,
                STS_DW_ATE_signed, LLVMDIFlagZero);
        case TypeU8:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u8",  2,  8,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeU16:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u16", 3, 16,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeU32:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u32", 3, 32,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeU64:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "u64", 3, 64,
                STS_DW_ATE_unsigned, LLVMDIFlagZero);
        case TypeF32:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "f32", 3, 32,
                STS_DW_ATE_float, LLVMDIFlagZero);
        case TypeF64:
            return LLVMDIBuilderCreateBasicType(cg->di_builder, "f64", 3, 64,
                STS_DW_ATE_float, LLVMDIFlagZero);
        case TypeError:
            /* Built-in error: represent as an opaque struct. */
            return LLVMDIBuilderCreateUnspecifiedType(
                cg->di_builder, "error", 5);
        case TypeFnPtr:
            /* Function pointers are opaque pointers in LLVM's model. */
            return LLVMDIBuilderCreatePointerType(
                cg->di_builder, Null, 64, 0, 0, "", 0);
        case TypeUser:
            return get_di_named_type(cg, resolved.user_name);
    }
    return Null;
}

/*
 * Build a DILocation metadata node for (line, col=0) in the current scope.
 * Returns Null when debug info is disabled or no scope is set.
 */
static LLVMMetadataRef di_make_location(cg_t *cg, usize_t line) {
    if (!cg->debug_mode || !cg->di_scope || line == 0) return Null;
    return LLVMDIBuilderCreateDebugLocation(
        cg->ctx, (unsigned)line, 0, cg->di_scope, Null);
}

/*
 * Attach a debug location to the IR builder so that all subsequently
 * emitted instructions carry source-line information.
 */
static void di_set_location(cg_t *cg, usize_t line) {
    if (!cg->debug_mode || !cg->di_scope || line == 0) return;
    LLVMMetadataRef loc = di_make_location(cg, line);
    LLVMSetCurrentDebugLocation2(cg->builder, loc);
}

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
