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

1. **Explicit Memory Ownership**
   - Developers must consciously choose between stack and heap allocation.
   - Pointer types enforce memory-region correctness.

2. **Predictable Performance**
   - No hidden allocations.
   - No implicit runtime overhead.

3. **Interop-First**
   - C interop is a first-class feature, not an afterthought.

4. **Structured Parallelism**
   - Parallel execution (CPU/GPU) is explicit and integrated into the language.

5. **Minimal but Powerful Syntax**
   - Inspired by Go-style declarations.
   - Consistent and uniform type system.

---

## Type System

### Type Declaration Model

All types follow a unified declaration pattern:

    type <name>: <definition>;

Examples:

    type ch_t: i8;

    type my_struct_t: struct {
        ...
    };


### Supported Type Categories

- Integer types: `i8`, `i16`, `i32`, `i64`
- Unsigned: `u8`, `u16`, `u32`, `u64`
- Floating point: `f32`, `f64`
- Boolean: `bool`
- `void` type
- `nil` — null pointer literal, assignable to any pointer type

---

## Memory Model

### Storage Qualifiers

- `stack` — stack allocation
- `heap` — heap allocation (primitives auto-`malloc`/`free`; pointers indicate the pointed-to domain)
- `atomic` — concurrency-safe storage
- `const` — immutable; no writable pointer may be derived
- `final` — write-once; no writable pointer may be derived

Storage qualifiers are required everywhere data is declared: local variables, function parameters, and tagged enum payloads.

### String Allocation

Both string literal forms are stack-allocated:

- `'...'` → stack string
- `"..."` → stack string (identical to `'...'`)

### Heap Primitive Variables

A `heap` primitive is automatically `malloc`'d on declaration and `free`'d when it leaves scope (unless `rem.()` is called first):

    heap i32 x = 42;   // malloc'd; freed at scope exit

### Storage Group Blocks

Applying one storage qualifier to multiple declarations:

    stack (
        i32 x = 0;
        i32 y = 1;
    )

    heap (
        i32 count = 0;
    )

All declarations inside inherit the block's qualifier. Pure syntactic sugar.

### Pointer Safety Rules

Pointers carry both a memory-domain tag and a permission.

#### Memory Domain

- A `stack` pointer must only reference stack memory.
- A `heap` pointer must only reference heap memory.
- Cross-domain assignment is a compile error:

      stack i32 x = 5;
      heap  i32 *p = &x;   // ERROR: stack address → heap pointer

#### Permissions

- `*r`  → read-only
- `*w`  → write-only
- `*rw` → read-write (default; `*` is shorthand)

Example:

    stack i8 *w buf;   // write-only pointer to stack memory

---

## Functions

### Function Types

- `int fn` — internal (module-private)
- `ext fn` — exported/public

### Syntax

    fn name(stack i32 x, heap u8 *buf): return_type

Every parameter requires a storage qualifier.

### Features

- Parameter grouping — adjacent params share a qualifier:

      fn sum3(stack i32 x, y, z): i32

- Multiple return values:

      fn min_max(stack i32 a, stack i32 b): [i32, i32]

- Multiple assignment:

      stack i32 [lo, hi] = min_max(3, 7);
      [lo, hi] = 0, 100;

- Explicit no-parameter functions:

      fn foo(void): void

- Return statements: `ret xyz;`

---

## Struct System

### Declaration

    type MyType: struct {
        ...
    };

### Features

- Exported structs:

      ext type MyType: struct { ... };

- Memory partitioning inside structs:

      struct {
      stack:
          ...
      heap:
          ...
      }

- Member functions (defined inside struct body)
- Static functions:

      fn MyType.method(stack i32 x): void

- Self-reference syntax:

      MyType.(field)

- Constructors:

      fn MyType.new(stack i32 x): MyType {}

- Destructors (auto-called on scope exit):

      fn MyType.rem(void): void

- Field visibility: `int` = private, `ext` = public

---

## Enums

### Basic Enums

    type Direction: enum { North, South, East, West };

    stack Direction d = Direction.North;

### Tagged Enums

Variants may carry a payload; the payload's storage domain must be explicit:

    type Shape: enum {
        Circle(stack f64),
        Rect(stack f64, stack f64),
        Blob(heap u8 *rw),
    };

    stack Shape s = Shape.Circle(3.14);

    match s {
        Shape.Circle(r) => { debug r; }
        Shape.Rect(w, h) => { debug w; }
        _ => {}
    }

---

## Memory Management

### Built-ins

- `new.(byte_count)` — allocate heap memory
- `sizeof.(type)` — size of a type in bytes
- `rem.(ptr)` — free heap memory
- `mov.(ptr, new_byte_count)` — resize a heap allocation (realloc)

### Allocation

    stack u8 *rw buf = new.(sizeof.(u8) * 64);
    buf = mov.(buf, 128);   // grow to 128 bytes
    rem.(buf);

---

## Control Flow

### Supported Constructs

- `for`, `while`, `do`-`while`
- Infinite loop: `inf {...}`
- `if` / `else` / else-if chaining
- `break`, `continue`
- Ternary: `cond ? then : else`
- `defer` — run a statement or block at scope exit (LIFO order):

      stack u8 *rw buf = new.(256);
      defer rem.(buf);   // freed on any exit path

### Operators

- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- Increment/decrement: `++x`, `x++`, `--x`, `x--`
- Comparison: `<`, `>`, `<=`, `>=`, `==`, `!=`
- Logical: `&&`, `||`, `!`
- Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`
- Address-of: `&x`

---

## Parallelism

### Execution Model

- GPU dispatch: `gpu.(fn_name)()`
- CPU parallel dispatch: `cpu.(fn_name)()`

Parallelism is explicit and controlled by the programmer.

---

## C Interoperability

### Importing Headers

    cinclude 'stdio' = io;
    cinclude 'assert';

### Calling C Functions

    io.printf('Hello, %s!\n', 'world');

---

## Module System

### Module Declaration
(At the top of every file)

    mod name;

### Imports

    imp other_module;

### Goals

- Clear module boundaries
- Namespacing support
- Controlled symbol visibility

---

## Debugging

    debug expr;

Prints the value of any expression with type-aware formatting (supports i32, i64, f32/f64, char, bool, pointers).

---

## Comments

- `// line comment`
- `/* block comment */` (supports nesting)

---

## Feature Roadmap

### Language Core
- [x] Module declarations (`mod`)
- [x] Import system (`imp`)
- [x] Comments (`//` line, `/* */` nested block)
- [x] Debug statement
- [x] Return statements (`ret`)
- [x] `defer` statement — executes a statement/block at scope exit regardless of control flow

### Types
- [x] `i8`, `i16`, `i32`, `i64`
- [x] `u8`, `u16`, `u32`, `u64`
- [x] `f32`, `f64`
- [x] `bool`
- [x] `void`
- [x] User-defined types (struct, enum, alias)
- [x] `nil` keyword — null pointer literal

### Memory & Storage
- [x] `stack`, `heap`, `atomic`, `const`, `final`
      (if it's const or final, you can't derive a writable pointer from it)
- [x] String literal allocation rules (`'...'` = stack, `"..."` = stack — both identical)
- [x] Storage qualifiers required on function parameters (`fn foo(stack i32 x)`)
- [x] Storage qualifiers required on tagged enum payloads (`Circle(stack f64)`)
- [x] Storage group blocks (`stack ( i32 x = 0; i32 y = 1; )`)
- [x] Cross-domain pointer conversion rejected by compiler (`stack` addr → `heap` ptr forbidden)
- [x] `heap` primitive variables auto-allocate via `malloc` and are auto-`free`'d at scope exit (unless already `rem.()`'d by the user)

### Functions
- [x] Internal vs external functions (`int fn`, `ext fn`)
- [x] Multiple return values (`fn foo(): [i32, i32]`)
- [x] Multiple assignment (`stack i32 [x, y] = foo();`)
- [x] Parameter grouping (`fn bar(stack i32 x, y, z)`)

### Structs
- [x] Full struct system (`type Name: struct { ... }`)
- [x] Methods, statics, constructors (`fn Type.method()`, `fn Type.new()`)
- [x] Destructors + auto cleanup (`fn Type.rem()` — auto-called on scope exit)
- [x] Memory partitioning (`stack:` / `heap:` sections)
- [x] Field visibility (`int` / `ext`)

### Enums
- [x] Basic enums (`type Name: enum { A, B, C }`)
- [x] Tagged payloads (`Variant(stack type)` or `Variant(heap type)`)

### Type Aliases
- [x] `type Name: existing_type;`

### Pointers & Allocation
- [x] Permissioned pointers (`*r`, `*w`, `*rw`)
- [x] `new.(size)`, `rem.(ptr)`, `sizeof.(type)`
- [x] `mov.(ptr, new_size)` — realloc
- [x] Arrays (`type name[size]`)

### Control Flow
- [x] `for` loop
- [x] `while` loop
- [x] `do`-`while` loop
- [x] `inf` infinite loop
- [x] `if` / `else` / else-if chaining
- [x] `break`, `continue`
- [x] Ternary (`cond ? then : else`)
- [x] Full operator set (arithmetic, bitwise, logical, compound assign)

### Parallelism
- [x] CPU dispatch (`cpu.(fn)()`)
- [x] GPU dispatch (`gpu.(fn)()`)

### Interop
- [x] C header inclusion (`cinclude 'header' = alias;`)
- [x] Function binding (auto-declared varargs externs)

### Expressions
- [x] Cast expressions (`(type)expr`)
- [x] Character literals (`` `c` ``)
- [x] Float literals (`3.14`)
- [x] Boolean literals (`true`, `false`)
- [x] Index expressions (`arr[i]`)
- [x] Member access (`obj.field`)
- [x] Self-member access (`Type.(field)`)
- [x] Method calls (`obj.method()`, `Type.static()`)
- [x] Increment/decrement (`++x`, `x++`, `--x`, `x--`)

### Enums
- [x] Basic C style enums
- [x] Rust style tagged enums (`Variant(stack type)` payloads, `match` statement)

---

## Summary

Stasha is intended to be:
- As powerful as C/C++
- Safer through explicit constraints
- More modern in syntax and structure
- Built for parallel and systems-level programming from the ground up
