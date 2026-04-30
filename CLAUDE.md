# Stasha — Compiler Reference

Systems language: explicit memory (stack/heap), pointer safety, C interop, LLVM backend.

## Build & Run

```
make                                    # build bin/stasha
stasha [build] <file.sts> [-o out]      # compile to executable
stasha lib     <file.sts> [-o out.a]    # static library
stasha dylib   <file.sts>               # dynamic library
stasha test    <file.sts>               # run test blocks
stasha build   <file.sts> -g            # debug info (DWARF + .dSYM on macOS)
stasha build   <file.sts> --target triple
stasha                                  # project-mode: reads sts.sproj in CWD
stasha build                            # project-mode: reads sts.sproj in CWD
```

Library workflow: `stasha lib veclib.sts -o examples/libveclib.a` then `lib "veclib" from "libveclib.a"; imp veclib;` — lib path resolved relative to source file directory.

**Project file** (`sts.sproj` in project root):
```
main     = "src/main.sts"     # entry point
binary   = "bin/myapp"        # output executable  (or: library = "bin/lib.a")
ext_libs = []                 # precompiled libraries
# ext_libs = [ ("libs/x.a" : "libs/x.sts"), ... ]
```

---

## Source Map

Unity-build layout: sub-files are `#include`d into their parent, keeping all functions `static`.

### `src/main.c` (494 lines)
CLI parsing, subcommand dispatch, `resolve_imports()` (splices imported module ASTs, tags `from_lib=True` for library-backed modules), calls `codegen()` + `link_object()`/`archive_object()`/`link_dynamic()`.

### `src/common/`
| File | Contents |
|------|----------|
| `common.c` (56) | includes, static vars, `quit()` — includes logger.c + heap.c |
| `logger.c` (371) | log rotation, `open_logger`, `log_msg/warn/err`, `get_error_count` |
| `heap.c` (142) | `allocate`, `reallocate`, `deallocate`, `scan_and_deallocate`, leak tracking |
| `common.h` (55) | `heap_t`, `result_t`, `boolean_t`, `usize_t`, logging API, `NullHeap` |

### `src/lexer/`
| File | Contents |
|------|----------|
| `lexer.c` (339) | `next_token()`, all token types, string/char/number scanning |
| `lexer.h` (180) | `token_kind_t` enum, `token_t`, `lexer_t` |

### `src/ast/`
| File | Contents |
|------|----------|
| `ast.h` (336) | All AST node kinds (`node_kind_t`), `node_t` union, `type_info_t`, `storage_t`, `linkage_t`, `ptr_perm_t` |
| `ast.c` (178) | `ast_alloc()`, `node_list_push()`, `ast_free_all()` |

### `src/parser/`
| File | Contents |
|------|----------|
| `parser.c` (260) | `parser_t`, helpers (`advance_parser`, `check`, `match_tok`, `consume`), `parse_type()`, forward decls, `parse()` entry point — includes sub-files |
| `parse_expr.c` (729) | `parse_primary`, `parse_postfix`, all precedence levels through `parse_expr` |
| `parse_stmt.c` (329) | `parse_block`, `parse_for/while/do_while/inf/if/ret/debug/var_decl/defer/storage_group/statement` |
| `parse_decls.c` (674) | `parse_struct_body`, `parse_match_stmt`, `parse_enum_body`, `parse_type_decl`, `parse_lib`, `parse_imp`, `parse_fn_decl`, `parse_switch_stmt`, `parse_asm_stmt`, `parse_comptime_if`, `parse_test_block`, `parse_top_decl` |
| `parser.h` (8) | `parse()` declaration |

### `src/codegen/`
| File | Contents |
|------|----------|
| `codegen.c` (1205) | All data structs (`symbol_t`, `symtab_t`, `struct_reg_t`, `enum_reg_t`, `cg_t`, etc.), forward decls, includes sub-files, `codegen()` entry point (3 passes: register types → gen fn bodies → gen test blocks) |
| `cg_symtab.c` (72) | `symtab_init/free/add/lookup`, `cg_lookup`, `symtab_set_last_*` |
| `cg_safety.c` (105) | `check_const_addr_of`, `check_permission_widening`, `check_stack_escape`, `check_ext_returns_int_ptr`, `check_pointer_lifetime`, `check_null_deref`, `check_ptr_arith_bounds` |
| `cg_lookup.c` (57) | `find_struct`, `find_enum`, `resolve_alias`, `find_lib_alias` (matches by alias or bare name), `find_fn_decl` |
| `cg_dtors.c` (175) | `push/pop_dtor_scope`, `add_deferred_stmt/dtor_var/heap_var`, `emit_struct_field_cleanup`, `emit_struct_cleanup`, `emit_dtor_calls`, `emit_all_dtor_calls`, `remove_from_dtor_scopes` |
| `cg_types.c` (143) | `get_llvm_base_type`, `get_llvm_type`, `build_fn_ptr_llvm_type`, `coerce_int`, `make_nil_error`, type-kind predicates, `payload_type_size`, `alloc_in_entry` |
| `cg_expr.c` | `gen_int/float/bool/char/str_lit`, `gen_ident`, `gen_binary`, `gen_unary_prefix/postfix`, `gen_call`, `gen_method_call`, `get_or_create_thread_wrapper`, `gen_thread_call`, `gen_future_op`, `gen_compound_assign`, `gen_assign`, `gen_index`, `gen_member`, `gen_self_member`, `gen_ternary`, `gen_cast`, `gen_new/sizeof/nil/mov/addr_of`, `gen_expr` dispatcher |
| `cg_stmt.c` (900) | `gen_local_var`, `gen_for/while/do_while/inf_loop/if/ret/debug/multi_assign/match/switch/asm_stmt/comptime_if/comptime_assert`, `gen_stmt` dispatcher, `gen_block` |
| `cg_registry.c` (132) | `register_struct/enum/alias/lib`, `struct_add_field/field_ex` |
| `cg_debug.c` (288) | `di_cache_lookup/set`, `get_di_named_type`, `get_di_type`, `di_make_location`, `di_set_location` |
| `cg_coro.c` | LLVM coroutine lowering for **both** task and stream `async fn`: `sts_emit_coro_stream/task_prologue`, `sts_emit_yield_value/now`, `sts_emit_stream/task_ret`, `sts_emit_await_next`, `sts_emit_await_task`, `sts_emit_stream_done/drop/cancel`, `sts_emit_task_wait/ready/get_raw/drop`, `sts_emit_coro_cancel`. Promise header (`__sts_coro_prom_hdr`: complete/eos/item_ready/is_stream/cancelled/continuation) + inline `T` slot. `presplitcoroutine` attribute + pass pipeline `coro-early,cgscc(coro-split),coro-cleanup,globaldce`. |
| `codegen.h` (10) | `codegen()` declaration |

### `src/runtime/`
| File | Contents |
|------|----------|
| `thread_runtime.h` | Public API: `__future_t`, `__thread_dispatch`, `__future_get/wait/ready/drop`, `__thread_runtime_init/shutdown` |
| `thread_runtime.c` | Thread pool (POSIX pthreads), ring-buffer job queue, future implementation. Backs `thread.(fn)(args)` only — `async fn` now uses llvm.coro.*. Auto-init/shutdown via `__attribute__((constructor/destructor))`. Compiled to `bin/thread_runtime.a` and automatically linked into every executable. |
| `coro_runtime.h` | Executor queue for `thread.(fn)`: `__async_dispatch/get/wait/ready/cancel/drop/wait_any`. Not used by `async fn` coroutines — those are self-contained coro frames. |
| `coro_runtime.c` | POSIX thread-pool executor. Backs `thread.(fn)` dispatch wrapper when explicit parallelism is needed. |

### `src/linker/`
| File | Contents |
|------|----------|
| `linker.cpp` (171) | LLD wrappers: `link_object`, `link_dynamic`, `archive_object` |
| `linker.h` (27) | declarations + `archive_object` |

---

## Language Quick Reference

**Storage qualifiers** (required everywhere): `stack`, `heap`, `atomic`, `const`, `final`, `volatile`, `tls`
**Pointer permissions**: `*r` (read-only), `*w` (write-only), `*+` (pointer allows pointer arithmatic), `*rw` / `*` (read-write), for example: `*w+`
**Types**: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `bool`, `void`, `nil`, `error`, `future`

```
// Declarations
stack i32 x = 0;          heap i32 y = 42;   // heap: auto malloc/free
stack (i32 a = 0; i32 b;) // storage group block
type Foo: struct { stack: ext i32 x; heap: int i32 *rw buf; };
type Dir: enum { North, South };
type Shape: enum { Circle(stack f64), Blob(heap u8 *rw) };
type Alias: i32;

// Functions
int fn add(stack i32 a, b): i32 { ret a + b; }
ext fn min_max(stack i32 a, stack i32 b): [i32, i32] { ... }
stack i32 [lo, hi] = min_max(3, 7);
fn foo(void): void

// Struct methods — instance methods are defined INSIDE the struct body and use `this`
// Static methods (e.g. constructors) are defined outside with `fn Foo.name()`
type Foo: struct {
    ext i32 x;
    ext fn method(stack i32 v): void { this.x = v; }  // instance method — this valid here
    ext fn rem(void): void { ... }                      // destructor — auto-called at scope exit
}
fn Foo.new(stack i32 x): Foo { ... }  // static constructor — no `this`

// `this` is ONLY valid inside functions defined within a struct body.
// Using `this` in an external `fn Foo.method()` is a compile error.

// Generics — @comptime[T] on structs and standalone functions
type arr_t: @comptime[T] struct {
    heap ext T *rw buf; ext i32 len;
    ext fn push(T val): void { ... }  // instance method uses this
}
fn @comptime[T] arr_t.new(stack i32 cap): arr_t.[T] { ... }  // static constructor
arr_t.[i32] a = arr_t.[i32].new(8);  // instantiate with concrete type
fn @comptime[T] identity(stack T val): T { ret val; }  // standalone generic fn
stack i32 x = identity.[i32](42);    // instantiate at call site

// Control flow
for (stack i32 i = 0; i < n; i++) {}
while (cond) {}    do {} while (cond);    inf {}
if (x) {} else if (y) {} else {}
match s { Shape.Circle(r) => { } _ => {} }
match n { 0 => {} 1 => {} _ => {} }            // integer literal arms
match s { Shape.Circle(r) if r > 10 => {} _ => {} }  // guard clauses
match st { Status.Ok => {} other => {} }        // wildcard binding
switch (x) { case 0: break; default: }
defer rem.(buf);
break;      // exit innermost for/while/do-while/inf/switch
continue;   // next iteration of innermost loop (not valid in switch)

// Operators: + - * / %  +% -% *% (wrap)  +! -! *! (trap)
//            & | ^ ~ << >>  && || !  < > <= >= == !=
//            &x (addr-of)   x[i] (index)

// Compound init `.{...}` — preferred over manual element-by-element fills.
// Works for arrays AND structs (also nested).  Always prefer this over
// `arr[0] = a; arr[1] = b; ...` — it's shorter and codegen pre-fills zeros.
i32 a1[]  = .{1, 2, 3};                   // length inferred from initializer
i32 b1[5] = .{1, 2};                       // remaining slots zero-filled
i32 c1[3] = .{[1] = 5};                    // designated index: c1 = {0, 5, 0}
stack i32 buf[16] = .{1, 2, 3, 4, 5};      // typical stack array — rest zero
stack i32 z[64]   = .{};                    // zero-initialise the whole array
player_t p = .{ .pos = .{ .x = 1, .y = 2 }, .hp = 100 };  // nested + designated
player_t q = .{ ..p, .hp = 200 };          // struct merge with `..` spread
i32 arr2[] = .{..arr1, 4, 5};              // array spread with `..`
i8 s[]     = .{.."Hello", .." ", .."World"};  // string spread

// Range expressions `start..end`, `start..=end`, `start..end:step`.
// Half-open by default (`..` excludes end).  `..=` is inclusive.  `:step`
// sets a stride.  Useful inside compound initializers and (where supported)
// foreach/iteration contexts.
i32 r1[] = .{0..5};        // 0 1 2 3 4
i32 r2[] = .{0..=5};       // 0 1 2 3 4 5
i32 r3[] = .{0..10:2};     // 0 2 4 6 8
i32 mix[] = .{ ..(0..5), 10, ..(20..=25), ..(0..10:2) };  // ranges + spread compose

// Slice ranges share the syntax: arr[lo:hi]  (half-open) — see foreach below.

// Built-in formatted output (no import required)
// First arg must be a string literal; {} inserts the next argument.
// Format specs after ':' inside {}: x/X (hex), b (binary), o (octal),
//   .N (float precision), <N (left-align width N), N (right-align width N),
//   0N (zero-padded width), + (force sign), # (alt prefix: 0x/0b/0).
//   Flags compose: {:+#08x}  Literal brace: \{
print.('hello\n');
print.('x = {}, y = {:08x}\n', x, y);

// C interop
lib "stdio" = io;    lib "math";    lib "mylib" from "libmylib.a";
io.printf("hello\n");    math.sqrt(x);

// Memory
new.(bytes)   rem.(ptr)   mov.(ptr, new_bytes)   sizeof.(Type)

// Misc
asm { "nop" }        // inline assembly
comptime_assert.(sizeof.(Foo) == 8);
#if os == "macos" && arch == "arm64" { ... }
test 'name' { expect.(x); expect_eq.(a, b); test_fail.('msg'); }

// Thread parallelism (thread pool, POSIX threads)
stack future f = thread.(fn_name)(arg1, arg2);  // dispatch to thread pool
future.wait(f);                    // block until done
stack bool r = future.ready(f);    // non-blocking check
stack i32 v = future.get.(i32)(f); // block and return typed result
stack void *p = future.get(f);     // block and return raw void*
future.drop(f);                    // wait + free future

// Task coroutines (llvm.coro.* — synchronous drive on caller's thread)
async fn add(i32 a, i32 b): i32 { ret a + b; }
stack future.[i32] f = async.(add)(1, 2);
stack i32 v = await(f);                  // drive to completion, return i32
stack i32 sum = await.(add)(1, 2);       // one-shot: dispatch + drive + return
stack i32 [a, b] = await.all(add(1,2), add(3,4));   // drive both, return in order
stack i32 winner = await.any(add(1,2), add(3,4));    // drive first, cancel rest
// await(f) is legal anywhere (not just inside async fn)

// Stream coroutines (real LLVM coroutines lowered through llvm.coro.*)
async fn fib(i32 n): stream.[i64] {
    stack i64 a = 0; stack i64 b = 1; stack i32 i = 0;
    while (i < n) { yield a; stack i64 t = a + b; a = b; b = t; i = i + 1; }
    ret;                            // ret expr; is rejected in stream coros
}
stream.[i64] f = fib(10);
inf {
    stack i64 v = await.next(f);    // drives producer to next yield (or eos)
    if (stream.done(f)) { break; }  // post-call eos check
    print.('{}\n', v);
}
stream.drop(f);                     // destroy coro frame (safe at any state)
stream.cancel(f);                   // set cancelled flag; producer sees at next yield
// `yield expr;` produces an item; `yield;` is a bare cooperative reschedule.
// `await.next(s)` is legal anywhere — synchronous drive on caller's thread.

// Struct attributes: @packed  @align(N)  @c_layout
// Fn/var attributes: @weak  @hidden  @restrict
// Variadic: fn foo(stack i32 n, ...): void
// Union: type U: union { i32 x; f32 y; }
// Bitfield: i32 flags: 3;  (inside struct)
```

### Sugar — pipeline `|>`, lambdas `lam.()`, trailing closures

```
// Lambda — non-capturing in v1.  Lifted to a module-level fn; expression
// value is a plain function pointer (fn*(...) : ret).
stack fn*(stack i32): i32 sq = lam.(stack i32 x): i32 { ret x * x; };

// Pipeline — left-associative, low-precedence.  `a |> f`        → f.(a)
//                                                `a |> f.(b,c)` → f.(a, b, c)
stack i32 r = 5 |> double |> negate;          // -10
stack i32 s = 100 |> add(50);                 // 150

// Trailing closure — short-form lambda after a `.()` call.  Param types
// are inferred from the callee's matching fn-pointer parameter slot.
//   f.(args) { |p1, p2| body-expr-or-stmts }    // typed-param form
//   f.(args) { stmts; }                          // zero-arg form
// The closure is appended as the LAST argument, so write higher-order fns
// with the fn-pointer parameter LAST: `fn map(arr, len, out, fn*(T):U f)`.
filter(&nums[0], 5, &out[0], &out_len) { |n| n % 2 == 0 };
sum = reduce(&arr[0], len, 0) { |acc, n| acc + n };
run() { print.('hi\n'); };

// Capture is rejected in v1 — the lambda body may reference module-scope
// names (functions, globals, types) but NOT enclosing locals.  Capturing
// closures land in v2 with explicit `heap`/`stack` env storage.
//
// Trailing-closure parsing is suppressed inside if/while/for/do-while/
// match/switch conditions, so `if pred(x) { ... }` still parses the brace
// as the if-body.
```


**Module system**: every file starts with `mod name;` (root) or `mod dir.subdir.name;` (nested). The dotted name mirrors the directory path relative to the entry file: `mod printer.typewriter;` lives at `printer/typewriter.sts`. `imp other.mod;` splices that module's types/sigs into the current AST; dots in the name are converted to path separators for lookup. `int` = module-private, `ext` = exported. Library-backed imports: `lib "x" from "libx.a"; imp x;` — codegen skips bodies, linker resolves from `.a`.

**`libimp` — combined lib+imp shorthand**:
```
libimp "name" from "path/libname.a";   // load archive from explicit path
libimp "name" from std;                // load from <bin_dir>/stdlib/libname.a
```
Equivalent to writing `lib "name" from <path>; imp name;` but in one declaration. The module name is always the same as the library name. `from std` resolves the archive to `<bin_dir>/stdlib/lib<name>.a` and looks up the interface source at `<bin_dir>/stdlib/<name>.sts`. Build stdlib modules with `make stdlib` before using `from std` imports.

---

## Pointer & Memory Safety Rules

These rules are enforced by the compiler at every declaration and assignment.

### Storage domain — `heap` vs `stack` on pointer variables

The storage qualifier on a **pointer** variable describes where the *pointed-to data* lives:

| Qualifier | Pointed-to data | `new.()` allowed? | `rem.()` required? |
|-----------|----------------|-------------------|-------------------|
| `heap T *perm p` | Heap-allocated | Yes (`new.(n)`) | Yes — programmer is responsible |
| `stack T *perm p` | Stack variable (`&local`) or zone allocation | Only via `new.(n) in zone` | Never — zone or scope owns the memory |
| Unqualified (`const`/`final`/bare) | Either | No check applied | No check applied |

**Domain is permanent.** Once declared, every subsequent assignment to the pointer must respect the same domain. A heap pointer can never be made to point at stack memory, and vice versa.

### What the compiler enforces

1. **Non-pointer heap vars are rejected.** `heap i32 x = 5;` is a compile error. Use `heap i32 *rw p = new.(sizeof.(i32));`. Exception: `heap []T` slice types are allowed (they carry an internal heap-owned data pointer).

2. **Wrong-domain pointer init/assign is rejected.**
   - `stack T *rw p = new.(n);` → error: heap alloc into stack pointer
   - `heap T *rw p = &some_local;` → error: stack address into heap pointer
   - `heap T *rw p = new.(n); p = &some_local;` → same error on the assignment

3. **Zone allocations are stack-like.** `new.(n) in zone_name` is zone-managed, not heap-owned. It may be stored in a `stack` pointer. Never call `rem.()` on it — call `rem.(zone_name)` to free the whole arena.

4. **`rem.()` on a stack pointer is rejected.** Only `heap` pointers may be freed with `rem.()`.

5. **All checks are suppressed inside `unsafe {}` blocks.**

### Correct patterns

```
heap i32 *rw p = new.(sizeof.(i32));   // heap pointer — must call rem.(p)
stack i32 x = 42;
stack i32 *r  q = &x;                  // stack pointer — scope-owned
zone z;
stack u8 *rw buf = new.(64) in z;      // zone alloc — rem.(z) frees it
rem.(z);                               // frees entire zone
```

### Common mistakes that produce compile errors

```
heap i32 x = 5;                        // non-pointer heap var — ERROR
stack i32 *rw p = new.(sizeof.(i32));  // heap alloc → stack ptr — ERROR
heap i32 *rw p = new.(4);
p = &some_local;                       // stack addr → heap ptr on assign — ERROR
stack i32 *rw q = new.(4) in z;
rem.(q);                               // rem on stack pointer — ERROR
```

### Implementation

- `src/codegen/cg_safety.c` — `rhs_addr_kind` (classifies RHS as heap/stack/unknown), `check_storage_domain` (emits domain mismatch errors)
- `src/codegen/cg_stmt.c` — `gen_local_var`: calls `rhs_addr_kind` + `check_storage_domain` at declaration
- `src/codegen/cg_expr.c` — `gen_assign`: same checks on every assignment; `NodeRemStmt` handler guards against `rem.()` on stack pointers

---

## TODO

- [ ] Module system: build Stasha modules that import other `.sts` files into static libraries
- [x] Dotted module names + sts.sproj project file
- [x] Thread parallelism: `thread.(fn)(args)` + `future` type (thread pool, POSIX pthreads)
- [x] Stream coroutines: `async fn ...: stream.[T]` — `yield expr;`, `yield;`, `await.next(s)`, `stream.done(s)`, `stream.drop(s)`, `stream.cancel(s)`
- [x] Task coroutines: `async fn ...: T` lowered through `llvm.coro.*` — synchronous drive via `await(f)`, `await.all`, `await.any`, `future.[T]` ops
- [x] Async methods: `async fn` inside struct body with `this` access (both task + stream)
- [x] Generic async fns: `@comptime[T] async fn` (task + stream, instantiated per call site)
- [x] Executor queue: `coro_runtime.c` thread pool retained for `thread.(fn)`; dead `__async_*` codegen path removed
- [ ] Executor queue + real async scheduling (continuations, yield; pause semantics)
- [ ] Cancellation propagation through `await(child_task)`
- [ ] Build system / package manager
- [ ] Standard library (string, I/O, math, collections in Stasha)
- [ ] Self-hosting compiler
