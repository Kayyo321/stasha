# How the Compiler Works

A high-level tour of the Stasha compiler internals. Useful for contributors, language learners, and anyone curious about how source code becomes a binary.

---

## Pipeline Overview

```
Source (.sts)
    │
    ▼
[Lexer]          src/lexer/lexer.c
    │  token stream
    ▼
[Preprocessor]   src/preprocessor/preprocessor.c
    │  macro-expanded token stream
    ▼
[Parser]         src/parser/parser.c + sub-files
    │  AST
    ▼
[Import Resolver] src/main.c (resolve_imports)
    │  merged AST (all modules)
    ▼
[Code Generator] src/codegen/codegen.c + sub-files
    │  LLVM IR
    ▼
[LLVM]           (via LLVM C API)
    │  native object file (.o)
    ▼
[Linker]         src/linker/linker.cpp
    │  + thread_runtime.a + zone_runtime.a + user libs
    ▼
Output (executable / .a / .dylib)
```

---

## 1. Lexer (`src/lexer/`)

The lexer converts raw source text into a flat stream of tokens.

**Key files:**
- `lexer.h` — `token_kind_t` enum (all 100+ token types), `token_t`, `lexer_t`
- `lexer.c` — `next_token()`, all scanning logic

**Token categories:**
- Literals: `TokIntLit`, `TokFloatLit`, `TokStackStr`, `TokCharLit`
- Keywords: `TokFn`, `TokFor`, `TokMatch`, `TokType`, `TokForeach`, etc.
- Operators: `TokPlus`, `TokAmpAmp`, `TokFatArrow`, `TokPlusPercent`, etc.
- Special: `TokAnd`, `TokOr` (comparison chain), `TokForeach`, `TokIn`

Each token records: kind, source pointer, length, line, column, file path.

**Block comments are nestable:** `/* /* inner */ outer */` works.

---

## 2. Preprocessor (`src/preprocessor/preprocessor.c`)

The preprocessor runs after lexing and expands macros before parsing.

**Macro types:**
- `macro fn name! { (args) => { body }; }` — function-style macros
- `macro let name! = tokens;` — token alias macros

**Macro features:**
- `@msg`, `@count` etc. — named macro parameters
- `...@args` — variadic macro args
- `@foreach arg : args { ... }` — iterate over variadic args

Macros are hygienic in the sense that they produce token sequences, not AST nodes.

---

## 3. Parser (`src/parser/`)

Converts the token stream to an AST (Abstract Syntax Tree).

**Key files:**
- `parser.c` — `parse()` entry, helpers (`advance_parser`, `check`, `match_tok`, `consume`), `parse_type()`
- `parse_expr.c` — expression parsing: primary, postfix, all precedence levels
- `parse_stmt.c` — statement parsing: blocks, loops, if, defer, var declarations
- `parse_decls.c` — top-level declarations: structs, enums, functions, imports

The parser is a recursive-descent parser. Each grammar rule is a function.

**AST nodes** (`src/ast/ast.h`):
- All node kinds are in `node_kind_t`
- Nodes carry source location info for error messages
- The union `node_t` holds the full node payload

---

## 4. Import Resolver (`src/main.c`)

`resolve_imports()` handles multi-file programs:

1. For each `imp module.name;` declaration, locate the `.sts` file
2. Parse that file recursively
3. Splice its exported AST nodes into the current module's AST
4. For library-backed modules (`lib "x" from "y.a"; imp x;`), mark declarations with `from_lib=True` — codegen skips their bodies but keeps their type signatures

---

## 5. Code Generator (`src/codegen/`)

The code generator walks the AST and emits LLVM IR. It runs in **3 passes**:

**Pass 1: Register Types**
- Registers all structs, enums, aliases, and unions
- Builds the struct/enum registry for later lookup

**Pass 2: Generate Function Bodies**
- Walks every function declaration
- Emits LLVM IR for each function body
- Handles generics by instantiating templates per concrete type

**Pass 3: Generate Test Blocks**
- Emits a special `run_tests()` function that calls all `test 'name' { }` blocks
- Only runs when `stasha test` is invoked

**Key sub-files:**
- `cg_expr.c` — expression codegen: literals, operators, calls, member access
- `cg_stmt.c` — statement codegen: loops, if, match, defer, var declarations
- `cg_types.c` — type → LLVM type conversion
- `cg_safety.c` — safety checks: domain, permissions, stack escape, bounds
- `cg_symtab.c` — symbol table: lookup, add, scope management
- `cg_dtors.c` — destructor tracking: `push/pop_dtor_scope`, automatic `rem()` calls
- `cg_debug.c` — DWARF debug info generation
- `cg_generics.c` — generic instantiation
- `cg_registry.c` — struct/enum/alias registration
- `cg_lookup.c` — find struct, enum, alias, function by name

---

## 6. Safety Checks (`src/codegen/cg_safety.c`)

Safety checks are integrated into code generation, not a separate pass:

- `check_const_addr_of` — can't take writable pointer from const
- `check_permission_widening` — narrowing only rule
- `check_stack_escape` — can't return local address
- `check_ext_returns_int_ptr` — ext functions can't expose private globals
- `check_pointer_lifetime` — scope-based lifetime
- `check_null_deref` — nullable pointer checks
- `check_ptr_arith_bounds` — arithmetic permission check

---

## 7. Destructor Tracking (`src/codegen/cg_dtors.c`)

When a struct with a `rem()` method goes out of scope, the compiler automatically inserts the destructor call.

Deferred statements (`defer rem.(ptr)`) are registered in a scope stack and emitted in reverse order at scope exit.

---

## 8. LLVM Backend

The compiler uses the LLVM C API to:
1. Build an LLVM IR module (`LLVMModuleRef`)
2. Run optimization passes (`LLVMPassManagerRef`)
3. Emit a native object file via LLVM's target machine

LLVM handles architecture-specific instruction selection, register allocation, and linking model details.

---

## 9. Linker (`src/linker/linker.cpp`)

Uses **LLD** (LLVM's built-in linker) — no system `ld` required.

**Functions:**
- `link_object()` — link object + libraries → executable
- `archive_object()` — bundle object → static `.a`
- `link_dynamic()` — link object + libraries → `.dylib`/`.so`

Automatically links:
- `bin/thread_runtime.a`
- `bin/zone_runtime.a`
- Standard system libraries

---

## 10. Runtimes (`src/runtime/`)

### Thread Runtime (`thread_runtime.c`)
- POSIX pthreads-based thread pool
- Auto-initialized via `__attribute__((constructor))`
- Auto-shutdown via `__attribute__((destructor))`
- Lock-free ring-buffer job queue
- Future management: reference-counted, heap-allocated

### Zone Runtime (`zone_runtime.c`)
- Bump allocator with 64 KiB linked-list blocks
- `__zone_alloc()` — O(1) allocation
- `__zone_free()` — O(n blocks) deallocation of entire zone
- `__zone_move()` — escape a pointer to the global heap

---

## 11. Bootstrap Compiler (`src.bootstrap/`)

Stasha is being self-hosted. The `src.bootstrap/` directory contains the beginning of a Stasha compiler written in Stasha itself. The lexer (`src.bootstrap/lex/lexer.sts`) is already implemented in Stasha, serving as both a real module and a demonstration of the language's expressiveness for compiler-writing tasks.

---

## Name Mangling

Symbols are mangled with module prefixes: `module__name__symbol`. C-extern functions (imported with `lib`) skip mangling and use their original C names.

---

## Unity Build

The compiler itself uses a "unity build": sub-files (like `cg_expr.c`, `parse_stmt.c`) are `#include`d into their parent (`codegen.c`, `parser.c`), keeping all functions `static` and allowing the C compiler to see everything at once for better optimization.
