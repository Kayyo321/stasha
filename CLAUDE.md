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
- [ ] Built-in testing framework — `test 'name' { ... }` blocks with `expect.(expr)`, `expect_eq.(a, b)`, `test_fail.('msg')`; compiled only when running in test mode

### Types
- [x] `i8`, `i16`, `i32`, `i64`
- [x] `u8`, `u16`, `u32`, `u64`
- [x] `f32`, `f64`
- [x] `bool`
- [x] `void`
- [x] User-defined types (struct, enum, alias)
- [x] `nil` keyword — null pointer literal
- [ ] Built-in `error` type — Go-style; `nil` = no error; used as a second return value (`fn foo(): [i32, error]`); created with `error.('message')`; compared with `== nil`

### Memory & Storage
- [x] `stack`, `heap`, `atomic`, `const`, `final`
      (if it's const or final, you can't derive a writable pointer from it)
- [x] String literal allocation rules (`'...'` = stack, `"..."` = stack — both identical)
- [x] Storage qualifiers required on function parameters (`fn foo(stack i32 x)`)
- [x] Storage qualifiers required on tagged enum payloads (`Circle(stack f64)`)
- [x] Storage group blocks (`stack ( i32 x = 0; i32 y = 1; )`)
- [x] Cross-domain pointer conversion rejected by compiler (`stack` addr → `heap` ptr forbidden)
- [x] `heap` primitive variables auto-allocate via `malloc` and are auto-`free`'d at scope exit (unless already `rem.()`'d by the user)
- [ ] Global variable semantics — module-level `var_decl` nodes: `int` globals are module-private static, `ext` globals are exported; `atomic` globals use atomic builtins; initialiser must be a constant expression

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
- [ ] No `ext` pointer to `int` data — `ext` functions/fields must not expose a pointer to a private (`int`) global variable or `int` struct field; doing so leaks internals across the module boundary
- [ ] No stack pointer escape — returning a pointer to a local stack variable is a compile error; the pointed-to value dies when the function returns
- [ ] Permission widening forbidden — a `*r` pointer cannot be cast or coerced to `*rw`/`*w`; a `*w` pointer cannot be widened to `*rw`; narrowing is always permitted
- [ ] Pointer lifetime rule — a pointer stored in a longer-lived variable must not point to a shorter-lived variable; the compiler should reject assignments where the pointee's scope is narrower than the pointer's scope
- [ ] No writable pointer from `const`/`final` — `&x` where `x` is `const` or `final` may only produce a `*r` pointer; a `*rw` or `*w` derivation is a compile error
- [ ] No `int` global pointer export — an `ext` function in another module cannot receive or return a pointer whose provenance is an `int` global of a foreign module
- [ ] Pointer arithmetic bounds enforcement — for stack-allocated arrays whose size is statically known, the compiler rejects pointer arithmetic that provably exceeds the allocation bounds
- [ ] Null dereference detection — dereferencing a pointer that is statically known to be `nil` (e.g., never assigned after `p = nil`) is a compile error
- [ ] Function pointer domain tags — a function pointer type must encode the storage domain of its parameters so that calling through a pointer cannot silently violate domain rules

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

## Design Notes & Future Work

### Error Type

A built-in `error` primitive modelled loosely on Go's `error` interface, adapted for a non-GC systems context.

**Semantics:**
- `error` is a built-in value type that is either `nil` (no error) or holds a stack string message.
- Its zero value is `nil` — a function that returns `error` and falls off the end implicitly returns `nil`.
- Created with `error.('message')`.
- Compared with `== nil` or `!= nil`.

**Typical usage:**

```
fn divide(stack i32 a, stack i32 b): [i32, error] {
    if b == 0 {
        ret 0, error.('division by zero');
    }
    ret a / b, nil;
}

stack i32 [result, err] = divide(10, 0);
if err != nil {
    debug err;
}
```

**Open questions:**
- Should `error` carry an integer code alongside the message (like `errno`)?
- Should errors be propagatable with a `?` suffix operator like Rust/Zig, i.e. `stack i32 x = divide(a, b)?;` auto-returns the error?

---

### Testing Framework

Zig-style inline tests: `test` blocks live alongside the code they test and are compiled only when the compiler is invoked in test mode.

**Syntax:**

```
test 'division by zero returns error' {
    stack i32 [r, err] = divide(10, 0);
    expect.(err != nil);
    expect_eq.(r, 0);
}

test 'normal division succeeds' {
    stack i32 [r, err] = divide(10, 2);
    expect.(err == nil);
    expect_eq.(r, 5);
}
```

**Builtins available inside `test` blocks:**
- `expect.(expr)` — fail the test if `expr` is false; print the failing expression
- `expect_eq.(a, b)` — fail if `a != b`; print both values on failure
- `expect_neq.(a, b)` — fail if `a == b`
- `test_fail.('message')` — unconditionally fail with a message

**Compilation model:**
- `test` blocks are stripped from normal builds; zero overhead in production.
- Running `stasha test file.sts` compiles a test binary that executes every `test` block and reports pass/fail counts.
- A test block that panics (e.g., null deref) counts as a failure, not a compiler crash.

---

### Global Variables

Module-level variable declarations currently parse and appear in the AST but their codegen semantics need to be fully specified and verified.

**Rules:**
- Module-level variables are always statically allocated (neither stack nor heap in the local-variable sense). The `stack`/`heap` qualifier on a global is therefore meaningless and should be rejected; globals use static storage implicitly.
- `int` globals are module-private (LLVM `internal` linkage).
- `ext` globals are exported (LLVM `external` linkage) and visible to modules that `imp` this one.
- `atomic` globals are declared with `_Atomic` / `__atomic` semantics for safe concurrent access.
- Initialisers must be constant expressions (no function calls at global scope).

**Verification needed:**
- Confirm codegen emits correct LLVM global variables (not alloca).
- Confirm `int`/`ext` linkage is applied correctly.
- Confirm `atomic` globals use atomic load/store in codegen.
- Confirm global struct-typed variables work (zero-init + field assignments).

---

### Expanded Pointer Safety Rules

The current pointer system enforces storage-domain correctness and permission levels. The following additional rules are planned to close remaining safety gaps.

#### No `ext` Pointer to `int` Data

An `ext`-visible function must not return or expose a pointer whose provenance is an `int` (private) global variable or an `int` struct field. Doing so would let external modules read or write private state through the pointer, circumventing visibility rules entirely.

```
int stack i32 secret = 42;

// ERROR: ext function exposes pointer to int global
ext fn leak(): stack i32 *rw { ret &secret; }
```

The rule applies equally to `int` struct fields:

```
type Foo: struct {
    int i32 priv;
    ext fn get_priv_ptr(): stack i32 *rw { ret &Foo.(priv); }  // ERROR
}
```

#### No Stack Pointer Escape

A pointer to a local stack variable must not outlive that variable's scope. The most common case is returning a pointer to a local from a function — the variable is destroyed on return, leaving the caller with a dangling pointer.

```
fn bad(): stack i32 *rw {
    stack i32 x = 5;
    ret &x;   // ERROR: x dies here; returned pointer is immediately dangling
}
```

The compiler should track pointer provenance through assignments and reject any path where a stack pointer is stored in a variable whose scope outlives the pointee:

```
stack i32 *rw outer = nil;
{
    stack i32 local = 10;
    outer = &local;   // ERROR: local's scope ends before outer's
}
debug outer[0];   // would be dangling
```

#### Permission Widening Forbidden

Pointer permissions are one-way: a `*rw` may narrow to `*r` or `*w`; a `*r` or `*w` may never widen back to `*rw`. This prevents a callee from silently upgrading a read-only view to a read-write one.

```
stack i32 x = 5;
stack i32 *r  ro = &x;
stack i32 *rw rw = ro;   // ERROR: *r widened to *rw
```

#### Pointer Lifetime Rule

When assigning `&local` to a pointer variable, the pointer variable must not have a longer lifetime than `local`. The compiler should enforce this through static scope analysis.

#### No Writable Pointer from `const`/`final`

The address-of operator applied to a `const` or `final` variable may only produce a `*r` pointer. Requesting `*rw` or `*w` is a compile error regardless of whether the pointer is used for writes.

```
const i32 k = 99;
stack i32 *r  ok  = &k;   // fine
stack i32 *rw bad = &k;   // ERROR: *rw from const
```

#### Pointer Arithmetic Bounds

For stack arrays with a statically known size, the compiler can reject pointer arithmetic that provably exceeds the bounds:

```
stack i32 arr[4];
stack i32 *rw p = &arr;
p = p + 10;   // ERROR: index 10 is out of bounds for arr[4]
```

#### Null Dereference Detection

If a pointer is statically known to be `nil` (never reassigned after `p = nil`, or declared as `= nil` with no subsequent assignment), any dereference of that pointer should be a compile error.

#### Function Pointer Domain Tags

A function pointer type must encode the storage domains of its parameters so that indirect calls cannot silently violate domain rules. For example, a variable holding a pointer to `fn(heap i32 *rw)` must not be called with a stack pointer argument.

---

## Summary

Stasha is intended to be:
- As powerful as C/C++
- Safer through explicit constraints
- More modern in syntax and structure
- Built for parallel and systems-level programming from the ground up
