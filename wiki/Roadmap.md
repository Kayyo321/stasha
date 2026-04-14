# Roadmap

What's coming to Stasha.

---

## In Progress

- **Bootstrap compiler** (`src.bootstrap/`) — The Stasha compiler is being rewritten in Stasha itself. The lexer is already complete. Parser and codegen are in progress. Self-hosting is the ultimate goal.

---

## Planned

### Language Features

- [ ] **Module build system** — Stasha modules that import other `.sts` files and compile to static libraries (beyond `sts.sproj`)
- [ ] **Build system / package manager** — Dependency management, reproducible builds, package registry
- [ ] **`error` type: integer code** — Carry an integer error code alongside the message string
- [ ] **Error propagation `?` operator (Rust/Zig style)** — More ergonomic error propagation
- [ ] **String interpolation** — `"hello {name}!"` string literals

### Standard Library

- [ ] **Full standard library in Stasha** — String, I/O, math, collections fully implemented in Stasha (replacing C wrappers)
- [ ] **Async I/O** — Non-blocking file and network I/O using the thread pool

### Tooling

- [ ] **Language Server Protocol (LSP)** — Full IDE integration: hover, completion, diagnostics, rename
- [ ] **Package registry** — Central package hosting and `stasha install`
- [ ] **Formatter** — `stasha fmt` for consistent code style
- [ ] **Documentation generator** — `stasha doc` produces HTML docs from comments

---

## Completed

- [x] Dotted module names + `sts.sproj` project files
- [x] Thread parallelism: `thread.(fn)(args)` + `future` type
- [x] Zone allocation: `zone name { }` and `zone name;`
- [x] Slice type `[]T` with `make`, `append`, `copy`, `len`, `cap`
- [x] `foreach elem in slice { }` iteration
- [x] Generics: `@comptime[T]` on structs and functions
- [x] Interfaces: `interface`, `struct.[iface]`, constrained generics
- [x] Pattern matching: `match` with guards, payload binding, negative literals
- [x] Comparison chains: `x > 1 and < 10`, `x == 1 or 2 or 3`
- [x] `any.[T1, T2, ...]` inline tagged union
- [x] Macros: `macro fn name! { }` and `macro let name! = token`
- [x] Expression body functions: `fn f(): T => expr;`
- [x] Compound initializers: `.{...}`, ranges, spread
- [x] Safety system: pointer permissions, domains, bounds checks, nullable
- [x] `unsafe {}` blocks and `[unchecked: i]`
- [x] `@frees` parameter attribute
- [x] `cheader` C header import
- [x] `with` statement for scoped bindings
- [x] `comptime_if` and `comptime_assert`
- [x] `@packed`, `@align`, `@c_layout`, `@weak`, `@hidden`
- [x] `restrict`, `volatile`, `tls`, `atomic` qualifiers
- [x] Inline assembly `asm { }`
- [x] Full debug info generation (DWARF + dSYM)
- [x] Cross-compilation via LLVM targets
- [x] LLD embedded linker (no system ld required)
- [x] Standard library: collections, strings, math, I/O, net, JSON, crypto, etc.

---

## Design Questions

These are open questions in the language design:

- **`error` type**: Should it carry an integer code alongside the message string? (Like `errno` + string)
- **Error propagation**: Should `?` be a postfix operator on function calls, or a more general mechanism?
- **Generics**: Should interface constraints be required, or optional hints?
- **Async**: Beyond thread pool — should there be coroutine/async-await support?
