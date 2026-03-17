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

fn ~MyType(void): void

- Automatic destructor invocation on scope exit

- Parameter grouping:

fn foo(i32 x, y, z)

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

cinclude "stdio.h";
cinclude "stdio.h" = io;

### Calling C Functions

io.printf(...);

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

Planned:

debug expr;


Used for inspecting values during execution.

---

## Comments (Planned Change)

Current:

(* Comment)


Planned:
- Replace with a more intuitive syntax
- Can't use this because deref syntax is the same
- Still retain parenthesis-based style

---

## Feature Roadmap

### Language Core
- [ ] Module declarations (`mod`)
- [ ] Import system (`imp`)
- [ ] Comments redesign
- [ ] Debug statement
- [ ] Return statements

### Types
- [ ] `i16`, `i32`
- [ ] `f32`, `f64`
- [ ] `str`
- [ ] `void`

### Memory & Storage
- [ ] `stack`, `heap`, `atomic`
- [ ] String literal allocation rules

### Functions
- [ ] Internal vs external functions
- [ ] Multiple return values
- [ ] Multiple assignment

### Structs
- [ ] Full struct system
- [ ] Methods, statics, constructors
- [ ] Destructors + auto cleanup
- [ ] Memory partitioning

### Pointers & Allocation
- [ ] Permissioned pointers (`*r`, `*w`, `*rw`)
- [ ] `new`, `rem`, `sizeof`

### Control Flow
- [ ] Full loop support
- [ ] Operator completeness

### Parallelism
- [ ] CPU dispatch
- [ ] GPU dispatch

### Interop
- [ ] C header inclusion
- [ ] Function binding

---

## Summary

Stasha is intended to be:
- As powerful as C/C++
- Safer through explicit constraints
- More modern in syntax and structure
- Built for parallel and systems-level programming from the ground up