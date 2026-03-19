# Stasha Language Specification

## Overview

**Stasha** is a systems programming language designed for:
- Explicit memory control (stack vs heap)
- Strong safety guarantees around pointer usage
- Seamless C interoperability
- Built-in CPU and GPU parallelism
- LLVM-based compilation

The language aims to combine low-level control (like C/C++) with safer abstractions and modern syntax.

---

## Core Design Principles

1. **Explicit Memory Ownership** — developers must consciously choose between stack and heap; pointer types enforce memory-region correctness.
2. **Predictable Performance** — no hidden allocations, no implicit runtime overhead.
3. **Interop-First** — C interop is a first-class feature, not an afterthought.
4. **Structured Parallelism** — parallel execution (CPU/GPU) is explicit and integrated.
5. **Minimal but Powerful Syntax** — Go-style declarations, consistent and uniform type system.

---

## Type System

All types follow a unified declaration pattern:

    type <name>: <definition>;

**Supported types:** `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, `f64`, `bool`, `void`, `nil` (null pointer literal), `error` (built-in Go-style error), user-defined structs/enums/aliases.

---

## Memory Model

### Storage Qualifiers

- `stack` — stack allocation
- `heap` — heap allocation (primitives auto-`malloc`/`free`; pointers indicate the pointed-to domain)
- `atomic` — concurrency-safe storage
- `const` — immutable; no writable pointer may be derived
- `final` — write-once; no writable pointer may be derived

Storage qualifiers are required everywhere data is declared: local variables, function parameters, and tagged enum payloads.

Both string literal forms are stack-allocated: `'...'` and `"..."` are identical.

A `heap` primitive is automatically `malloc`'d on declaration and `free`'d when it leaves scope (unless `rem.()` is called first):

    heap i32 x = 42;   // malloc'd; freed at scope exit

Storage group blocks apply one qualifier to multiple declarations:

    stack (
        i32 x = 0;
        i32 y = 1;
    )

### Pointer Safety Rules

Pointers carry a memory-domain tag and a permission:
- `*r` → read-only, `*w` → write-only, `*rw` → read-write (default; `*` is shorthand)
- Cross-domain assignment is a compile error (`stack` addr → `heap` ptr forbidden)
- No stack pointer escape — returning `&local` is a compile error
- Permission widening forbidden — `*r` cannot be widened to `*rw`/`*w`
- No writable pointer from `const`/`final`
- Pointer lifetime rule — pointer must not outlive its pointee's scope
- Pointer arithmetic bounds enforcement for statically-sized stack arrays
- Null dereference detection for statically-known `nil` pointers
- No `ext` pointer to `int` data — `ext` functions must not expose pointers to private globals/fields
- Function pointer domain tags — `fn*(params): ret_type`; compiler rejects mismatched storage domains at call sites

---

## Functions

- `int fn` — internal (module-private), `ext fn` — exported/public
- Every parameter requires a storage qualifier
- Parameter grouping: `fn sum3(stack i32 x, y, z): i32`
- Multiple return values: `fn min_max(stack i32 a, stack i32 b): [i32, i32]`
- Multiple assignment: `stack i32 [lo, hi] = min_max(3, 7);`
- Explicit no-parameter: `fn foo(void): void`
- Return: `ret xyz;`

---

## Struct System

    type MyType: struct { ... };

- Exported: `ext type MyType: struct { ... };`
- Memory partitioning: `stack:` / `heap:` sections inside struct body
- Field visibility: `int` = private, `ext` = public
- Methods: `fn MyType.method(stack i32 x): void`
- Constructor: `fn MyType.new(stack i32 x): MyType {}`
- Destructor (auto-called on scope exit): `fn MyType.rem(void): void`
- Self-reference: `MyType.(field)`
- Heap pointer fields are auto-freed on destruction; nested struct destructors called recursively

---

## Enums

    type Direction: enum { North, South, East, West };

Tagged enums with storage-qualified payloads:

    type Shape: enum {
        Circle(stack f64),
        Rect(stack f64, stack f64),
        Blob(heap u8 *rw),
    };

    match s {
        Shape.Circle(r) => { debug r; }
        _ => {}
    }

---

## Memory Management

- `new.(byte_count)` — allocate heap memory
- `rem.(ptr)` — free heap memory
- `mov.(ptr, new_byte_count)` — resize (realloc)
- `sizeof.(type)` — size of a type in bytes

---

## Control Flow

- `for`, `while`, `do`-`while`, `inf` (infinite loop)
- `if` / `else` / else-if, `break`, `continue`
- Ternary: `cond ? then : else`
- `defer` — run a statement/block at scope exit (LIFO):

      defer rem.(buf);

**Operators:** arithmetic (`+`, `-`, `*`, `/`, `%`), bitwise (`&`, `|`, `^`, `~`, `<<`, `>>`), logical (`&&`, `||`, `!`), comparison (`<`, `>`, `<=`, `>=`, `==`, `!=`), compound assign, increment/decrement, address-of (`&x`).

---

## Parallelism

- `cpu.(fn_name)()` — CPU parallel dispatch
- `gpu.(fn_name)()` — GPU dispatch

---

## C Interoperability

The `lib` keyword is the single entry point for all external library access:

```
lib "name";                               // C stdlib header, no alias
lib "name" = alias;                       // C stdlib header with alias
lib "name" from "path/to/lib.a";          // custom .a library
lib "name" from "path/to/lib.a" = alias;  // custom .a library with alias
```

    io.printf("Hello, %s!\n", "world");
    stack f64 c = cmath.sin(3.14);

---

## Module System

    mod name;        // module declaration (top of every file)
    imp other_mod;   // import

- `int` globals are module-private (LLVM `internal` linkage)
- `ext` globals are exported (LLVM `external` linkage)
- `atomic` globals use atomic load/store semantics
- Global initialisers must be constant expressions

---

## Debugging & Comments

    debug expr;           // type-aware print
    // line comment
    /* block comment */   // supports nesting

---

## Feature Roadmap

### Language Core
- [x] Module declarations (`mod`), import system (`imp`)
- [x] Comments (`//`, `/* */` nested)
- [x] Debug statement, return statements (`ret`)
- [x] `defer` statement
- [x] Built-in testing framework (`test 'name' { }`, `expect.()`, `expect_eq.()`, `test_fail.()`)

### Types
- [x] All numeric primitives (`i8`–`i64`, `u8`–`u64`, `f32`, `f64`, `bool`, `void`)
- [x] User-defined types (struct, enum, alias)
- [x] `nil`, built-in `error` type

### Memory & Storage
- [x] All storage qualifiers (`stack`, `heap`, `atomic`, `const`, `final`)
- [x] Storage group blocks, cross-domain pointer rejection
- [x] `heap` primitives auto-malloc/free, global variable semantics

### Functions
- [x] `int fn` / `ext fn`, multiple return values, multiple assignment, parameter grouping

### Structs & Enums
- [x] Full struct system, methods, constructors, destructors, memory partitioning, field visibility
- [x] Basic enums, tagged payloads, `match` statement

### Pointers & Allocation
- [x] Permissioned pointers (`*r`, `*w`, `*rw`), `new.()`, `rem.()`, `mov.()`, `sizeof.()`
- [x] Arrays, no stack pointer escape, permission widening forbidden
- [x] Pointer lifetime rule, no writable pointer from `const`/`final`
- [x] Bounds enforcement, null dereference detection, no `ext` pointer to `int` data
- [x] Function pointer domain tags

### Control Flow
- [x] `for`, `while`, `do`-`while`, `inf`, `if`/`else`, `break`, `continue`, ternary
- [x] Full operator set

### Parallelism
- [x] CPU dispatch (`cpu.(fn)()`), GPU dispatch (`gpu.(fn)()`)

### Expressions
- [x] Casts, character literals, float/bool literals, index expressions
- [x] Member access, self-member access, method calls, increment/decrement

### Interop & Toolchain
- [x] `lib` keyword — unified C stdlib + custom `.a` library declarations
- [x] Internal LLVM LLD linker (no external linker required)

---

## Design Notes & Future Work

### Error Type

Built-in `error` type modelled on Go's error, adapted for non-GC systems context.
- Zero value is `nil`; created with `error.('message')`; compared with `== nil` / `!= nil`
- A function returning `error` that falls off the end implicitly returns `nil`

**Open questions:**
- Should `error` carry an integer code alongside the message (like `errno`)?
- Should errors be propagatable with a `?` suffix operator like Rust/Zig?

### Testing Framework

Zig-style inline test blocks — compiled only in test mode, zero overhead in production:

```
test 'division by zero' {
    stack i32 [r, err] = divide(10, 0);
    expect.(err != nil);
    expect_eq.(r, 0);
}
```

Available builtins: `expect.(expr)`, `expect_eq.(a, b)`, `expect_neq.(a, b)`, `test_fail.('msg')`.

Running `stasha test file.sts` compiles and executes all test blocks, reporting pass/fail counts.

---

## TODO

### Module System

- [ ] Support building Stasha modules (that import other modules from other `.sts` files) into static libraries

---

## C Competitor Requirements

For Stasha to be a valid C competitor — replacing C in the domains where it dominates (OS kernels, embedded, drivers, system utilities, HPC, protocol implementations, libraries) — the following must be implemented:

### Critical (C programs routinely rely on these)

- [x] **Variadic functions** — `fn foo(stack i32 n, ...): void`; needed for printf-style APIs and FFI
- [x] **Union types** — `type Foo: union { i32 x; f32 y; }`; essential for protocol parsing, type punning, hardware registers
- [x] **Bitfields** — `i32 flags: 3;` inside structs; required for packed hardware/protocol structures
- [x] **`volatile` qualifier** — for memory-mapped I/O and hardware registers where the compiler must not reorder or elide accesses
- [x] **Inline assembly** — `asm { ... }` blocks; required for OS kernels, bootloaders, SIMD, and low-level hardware access
- [x] **`switch` statement** — integer/enum dispatch with fall-through control; C's switch is distinct from `match` on tagged enums
- [x] **Packed and aligned struct attributes** — `@packed`, `@align(N)`; required for wire formats, ABI compatibility, SIMD alignment
- [x] **Compile-time conditional compilation** — platform/arch/OS detection; equivalent to `#ifdef` guards for portability

### Important (limits adoption without these)

- [x] **Designated initializers** — `MyStruct { .x = 1, .y = 2 }`; ubiquitous in C for readable struct/array init
- [x] **`static_assert` / compile-time assertions** — `comptime_assert.(sizeof.(Foo) == 8)`; catch layout/ABI assumptions at compile time
- [x] **Thread-local storage** — `tls` qualifier or `@thread_local`; needed for per-thread state in runtimes and OS code
- [x] **Integer overflow semantics** — explicit wrapping (`+%`) vs trapping (`+!`) arithmetic; predictable behavior for crypto and embedded
- [x] **C-compatible struct layout guarantee** — explicit opt-in (e.g. `@c_layout`) to guarantee ABI compatibility with C structs for FFI
- [x] **Weak symbols / link-time visibility** — `@weak`, `@hidden`; needed for library authors overriding defaults and linker-level optimisation
- [x] **`restrict` pointer hint** — aliasing hint to enable C-level auto-vectorization and optimizer performance

### Toolchain & Ecosystem

- [x] **Cross-compilation** — `--target triple` flag; required for embedded and OS dev where host ≠ target
- [ ] **Build system / package manager** — distributing and consuming Stasha libraries without manual `.a` path management
- [x] **Dynamic library output** — produce `.so`/`.dylib`/`.dll`; needed for plugin systems and shared runtimes
- [ ] **Debug info quality** — DWARF emission sufficient for `gdb`/`lldb` to show Stasha source, types, and variable names
- [ ] **Standard library** — native string, I/O, math, and collection types so programs don't depend entirely on C interop
- [ ] **Self-hosting compiler** — compiler written in Stasha; proves the language is production-ready for systems work
