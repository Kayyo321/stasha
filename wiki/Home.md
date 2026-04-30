# Welcome to Stasha

**Stasha** is a modern systems programming language designed for safety, performance, and seamless C interoperability. It compiles to native machine code via LLVM, gives you full control over memory, and enforces pointer safety at compile time — without a garbage collector.

---

## What Makes Stasha Different?

| Feature | C | C++ | Stasha |
|---------|---|-----|--------|
| Manual memory management | Yes | Yes | Yes |
| Pointer safety checks | No | Partial | **Yes (compile-time)** |
| Storage domain tracking | No | No | **Yes** |
| Modern pattern matching | No | Partial | **Yes** |
| Built-in generics | No | Templates | **Yes (`@comptime[T]`)** |
| Built-in concurrency | No | `std::thread` | **Yes (`thread.(fn)(args)`)** |
| C interoperability | Native | Yes | **Yes (`lib`, `cheader`)** |
| Test blocks | No | No | **Yes (`test 'name' {}`)** |
| Arena allocators | Manual | Manual | **Yes (`zone`)** |

---

## Quick Links

### Learning Stasha
- [Getting Started](Getting-Started) — Install and write your first program
- [Language Basics](Language-Basics) — Core syntax overview
- [Variables](Variables) — Declarations and storage qualifiers
- [Types](Types) — Full type system guide
- [Functions](Functions) — Declarations, methods, generics
- [Control Flow](Control-Flow) — If, loops, match, switch
- [Structs](Structs) — Struct types and methods
- [Enums](Enums) — Simple and tagged enums
- [Interfaces](Interfaces) — Interface contracts
- [Generics](Generics) — Generic programming with `@comptime[T]`
- [Compile-Time Features](Compile-Time-Features) — `@comptime if`, metadata fields, assertions
- [Syntax Sugar](Sugar) — Lambdas, pipelines, and trailing closures
- [Compound Initializers](Compound-Initializers) — Inline array and struct initialization
- [Slices](Slices) — Fat-pointer slice views and heap slices
- [Hashing](Hashing) — `hash.(expr)` and custom struct hashes

### Memory & Safety
- [Memory Management](Memory-Management) — Stack, heap, zones, rem, defer
- [Safety System](Safety-System) — Pointer permissions, domains, lifetime checks
- [Error Handling](Error-Handling) — The `error` type and propagation

### Modules & Libraries
- [Modules and Imports](Modules-and-Imports) — `mod`, `imp`, `lib`, `libimp`
- [Submodules](Submodules) — Named scopes inside one source file
- [Preprocessor Macros](Preprocessor-Macros) — Token macros and macro imports
- [File Headers](File-Headers) — Declarative ABI, target, lifecycle, and diagnostic metadata
- [Standard Library](Standard-Library) — Built-in modules reference
- [Creating Libraries](Creating-Libraries) — Compile to `.a` or `.dylib`
- [Using Libraries](Using-Libraries) — Link and import external libraries

### Tooling
- [Building Projects](Building-Projects) — `stasha build`, flags, targets
- [Project System](Project-System) — `sts.sproj` project files
- [Testing](Testing) — `stasha test` and test blocks
- [Debugging](Debugging) — Debug builds and DWARF symbols
- [Compiler Flags](Compiler-Flags) — Full flag reference
- [Linking System](Linking-System) — Static, dynamic, C libraries

### Advanced
- [Concurrency](Concurrency) — Thread pool, futures, atomic
- [Async / Await](Async-Await) — Coroutine tasks, streams, and cooperative fan-in
- [Signals](Signals) — Synchronous typed signal dispatch
- [C Interoperability](C-Interoperability) — Call C from Stasha and vice versa
- [How the Compiler Works](How-the-Compiler-Works) — Internals overview

### Reference
- [Language Reference](Language-Reference) — Complete syntax reference
- [Roadmap](Roadmap) — What's coming next
- [Contributing](Contributing) — How to contribute

---

## Hello, World

```stasha
mod hello;

ext fn main(void): i32 {
    print.('Hello, World!\n');
    ret 0;
}
```

```bash
stasha hello.sts
./hello
Hello, World!
```

---

## Design Philosophy

Stasha is built around three principles:

1. **You own your memory.** No garbage collector. No hidden allocations. Every byte is accounted for.
2. **Safety is verified, not assumed.** Pointer permissions, storage domains, and lifetime rules are checked at compile time.
3. **C is a first-class citizen.** Call any C library with a single line. Export Stasha code to C with no friction.

---

*This wiki is the official language reference for Stasha. It covers everything from beginner syntax to compiler internals.*
