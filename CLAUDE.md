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


### Supported Type Categories (Planned)

- Integer types: `i8`, `i16`, `i32`
- Floating point: `f32`, `f64`
- Boolean type
- String type: `str`
- `void` type

---

## Memory Model

### Storage Qualifiers

- `stack` — stack allocation
- `heap` — heap allocation
- `atomic` — concurrency-safe storage

### String Allocation

- `'...'` → stack-allocated string
- `"..."` → heap-allocated string

### Pointer Safety Rules

Pointers are annotated with memory and permission constraints.

#### Memory Domain

- Stack pointers must only reference stack memory.
- Heap pointers must only reference heap memory.

#### Permissions

- `*r`  → read-only  
- `*w`  → write-only  
- `*rw` → read-write (default, can be written as `*`)

Example:

    stack i8 *w buf;

---

## Functions

### Function Types

- `int fn` — internal (module-private)
- `ext fn` — exported/public

### Syntax

    fn name(params): return_type

### Features

- Multiple return values:

    fn foo(): [i32, i32]

- Multiple assignment:

    stack i32 [x, y] = foo();
    [x, y] = 10, 20;

- Explicit no-parameter functions:

    fn foo(void): void

- Return statements:
  - `ret xyz;`

---

## Struct System

### Declaration

type MyType: struct {
...
};

### Features (Planned)

- Exported structs:

    ext type MyType: struct { ... };

- Memory partitioning inside structs:

struct {
stack:
...
heap:
...
}

- Member functions (defined inside struct)
- Static functions:

fn MyType.method(...)

- Self-reference syntax:

MyType.(field)

- Constructors:

fn MyType.new(...): MyType {}

- Destructors:

fn MyType.rem(void): void

- Automatic destructor invocation on scope exit

- Parameter grouping:

fn foo(i32 x, y, z)

- Field hiding
`int` internal fields are basically just private
`ext` external fields are basically just public

---

## Memory Management

### Allocation

new.(sizeof.(type) * count)

### Built-ins

- `new.(byte_count)`
- `sizeof.(type)`
- `rem.(ptr)` — deallocation

---

## Control Flow

### Supported Constructs

- `for`
- `while`
- `do-while`
- Infinite loop:
  - `inf {...}`


### Operators

- Arithmetic: `+`, `*`, etc.
- Assignment: `+=`, etc.
- Increment: `++i`
- Comparison: `<`, `>`, etc.
- Full C-style operator support (arithmetic, logical, bitwise)

---

## Parallelism

### Execution Model

- GPU dispatch:

gpu.(fn_name)()

- CPU parallel dispatch:

cpu.(fn_name)()

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
> mod name;

### Imports

imp other_module;


### Goals

- Clear module boundaries
- Namespacing support
- Controlled symbol visibility

---

## Debugging

    debug expr;

Prints the value of any expression with type-aware formatting (supports i32, i64, f32/f64, char, pointers).

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

### Types
- [x] `i8`, `i16`, `i32`, `i64`
- [x] `u8`, `u16`, `u32`, `u64`
- [x] `f32`, `f64`
- [x] `bool`
- [x] `void`
- [x] User-defined types (struct, enum, alias)

### Memory & Storage
- [x] `stack`, `heap`, `atomic`, `const`, `final`
      (if it's const or final, you can't derive a writable pointer from it)
- [x] String literal allocation rules (`'...'` = stack, `"..."` = heap)

### Functions
- [x] Internal vs external functions (`int fn`, `ext fn`)
- [x] Multiple return values (`fn foo(): [i32, i32]`)
- [x] Multiple assignment (`stack i32 [x, y] = foo();`)
- [x] Parameter grouping (`fn bar(i32 x, y, z)`)

### Structs
- [x] Full struct system (`type Name: struct { ... }`)
- [x] Methods, statics, constructors (`fn Type.method()`, `fn Type.new()`)
- [x] Destructors + auto cleanup (`fn Type.rem()` — auto-called on scope exit)
- [x] Memory partitioning (`stack:` / `heap:` sections)
- [x] Field visibility (`int` / `ext`)

### Enums
- [x] Basic enums (`type Name: enum { A, B, C }`)
- [x] Tagged payloads (`Variant(type)`)

### Type Aliases
- [x] `type Name: existing_type;`

### Pointers & Allocation
- [x] Permissioned pointers (`*r`, `*w`, `*rw`)
- [x] `new.(size)`, `rem.(ptr)`, `sizeof.(type)`
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

---

## Summary

Stasha is intended to be:
- As powerful as C/C++
- Safer through explicit constraints
- More modern in syntax and structure
- Built for parallel and systems-level programming from the ground up