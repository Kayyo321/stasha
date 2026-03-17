# Stasha Language

Stasha is a systems programming language with explicit memory placement (stack/heap), C interop, and GPU/CPU parallelism support. It compiles via LLVM.

## Feature TODO

Features derived from the example programs that need to be implemented:

### Core Language
- [ ] Module declarations (`mod name;`)
- [ ] Comments (`(* ... )`)
- [ ] `debug` statement (print/inspect values)
- [ ] `ret` / `return` statements
- [ ] Integer types: `i16`, `i32`
- [ ] String type: `str`
- [ ] `void` type
- [ ] Boolean/comparison operators (`<`, `++i`, `+=`)
- [ ] Arithmetic operators (`+`, `*`)

### Storage Classes
- [ ] `stack` storage qualifier
- [ ] `heap` storage qualifier
- [ ] `atomic` storage qualifier
- [ ] Stack-allocated string literals with single quotes (`'...'`)
- [ ] Heap-allocated string literals with double quotes (`"..."`)

### Functions
- [ ] Internal functions (`int fn`)
- [ ] External/exported functions (`ext fn`)
- [ ] Function parameters and return types (`fn name(params): rettype`)
- [ ] Multiple return values (`fn foo(): [i32, i32]` returning `a, b`)
- [ ] Destructuring assignment (`stack i32 [x, y] = multi(10, 20)`)
- [ ] `void` parameters

### Control Flow
- [ ] `for` loops with C-style syntax

### Structs
- [ ] Struct type declarations (`type name: struct { ... }`)
- [ ] `ext type` for exported struct types
- [ ] Stack/heap sections within structs (`stack:` / `heap:`)
- [ ] Member functions within struct body
- [ ] Self-reference via `TypeName.(field)` syntax
- [ ] Static functions (`fn TypeName.fn_name(...)`)
- [ ] Constructor pattern (`TypeName.new(...)`)
- [ ] Shorthand parameter type grouping (`i32 x, y, z`)

### Memory Management
- [ ] Pointers (`*data`)
- [ ] Heap allocation with `new.(sizeof.(type) * count)`
- [ ] `sizeof.()` builtin
- [ ] Deallocation with `rem.()`
- [ ] Destructors (`fn ~TypeName(void): void`)
- [ ] Automatic destructor calls at scope exit

### Parallelism
- [ ] GPU parallel dispatch (`gpu.(fn_name)()`)
- [ ] CPU parallel dispatch (`cpu.(fn_name)()`)

### C Interop
- [ ] `cinclude "header"` for C header imports
- [ ] `cinclude "header" = alias` for aliased C imports
- [ ] Calling C functions via alias (`io.printf(...)`)

### Module / Import System
- [ ] `mod` declarations for defining modules
- [ ] `imp` (import) for importing other Stasha modules
- [ ] Module-level visibility and namespacing
