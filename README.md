
# Stasha

Stasha is a modern, compiled systems programming language designed as a safer, more expressive alternative to C, with explicit control over memory, strong pointer safety, and seamless C interop. Built on LLVM, Stasha aims to combine the performance and predictability of low-level languages with quality-of-life features inspired by modern language design.

## Why Use Stasha?

- **Explicit Memory Control:** Every variable and pointer is annotated with storage (`stack`, `heap`, etc.) and permission (`*r`, `*w`, `*rw`, etc.), making memory management and pointer safety explicit and verifiable at compile time.
- **Predictable Performance:** No hidden allocations or garbage collection. Heap allocations are explicit and automatically freed at scope exit, reducing leaks and lifetime bugs.
- **C Interoperability:** Directly import and call C libraries, making it easy to reuse existing code and incrementally adopt Stasha in C projects.
- **LLVM Backend:** Generates highly optimized native code for multiple platforms, with support for debug info, static and dynamic libraries, and project-based builds.
- **Modern Language Features:**
	- Storage groups for bundling variables
	- Tagged enums and algebraic data types
	- Struct methods, constructors, and destructors
	- Pattern matching (`match`, `switch`)
	- Defer and RAII-style cleanup
	- Inline assembly, compile-time assertions, and conditional compilation
	- Parallel dispatch (CPU/GPU)
	- Built-in test blocks and expectations
- **Safe by Design:** Compile-time checks for pointer lifetime, permission widening, stack escape, and null dereference.
- **Modular and Extensible:** Dotted module system, static and dynamic library support, and a growing standard library.

## Language Features (Quick Reference)

- **Storage qualifiers:** `stack`, `heap`, `atomic`, `const`, `final`, `volatile`, `tls`
- **Pointer permissions:** `*r`, `*w`, `*rw`, `*+`, etc.
- **Types:** `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `bool`, `void`, `nil`, `error`, user-defined structs/enums/aliases
- **Functions:** First-class, with explicit argument storage, variadics, and C ABI interop
- **Structs/Enums:** Methods, destructors, tagged unions, bitfields, attributes (`@packed`, `@align`, etc.)
- **Control flow:** `for`, `while`, `do`, `inf`, `if`, `match`, `switch`, `defer`, `comptime_assert`, etc.
- **Operators:** Full set, including wrapping/trapping arithmetic, pointer arithmetic, and address-of
- **Module system:** Dotted names mirror directory structure; `imp` and `lib` for code and library imports
- **Testing:** Inline `test` blocks with expectations and failure reporting

## When to Use Stasha

Stasha excels in:

- **Systems programming:** OS kernels, drivers, embedded, and performance-critical code where C or C++ would traditionally be used
- **Safety-critical applications:** Where explicit memory and pointer safety are required
- **Incremental modernization:** Gradually replacing C codebases, or building new projects that need C interop
- **Tooling and compilers:** Where predictable, low-level control is essential

## Project Status

Stasha is under active development. Core language features are implemented, including:

- Explicit stack/heap/atomic/const storage
- Pointer permission system
- Structs, enums, tagged unions, bitfields
- Methods, destructors, and RAII
- Pattern matching, switch, and control flow
- C interop and library import
- Modular build system and project files
- Inline assembly, compile-time checks, and test blocks

Planned features include a package manager, a full standard library, and a self-hosting compiler.

## Getting Started

See `examples/` for sample programs.

---
Stasha is best for those who want the power and control of C, but with modern safety, expressiveness, and tooling.

Thanks :)
