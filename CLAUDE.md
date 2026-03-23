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
| `codegen.h` (10) | `codegen()` declaration |

### `src/runtime/`
| File | Contents |
|------|----------|
| `thread_runtime.h` | Public API: `__future_t`, `__thread_dispatch`, `__future_get/wait/ready/drop`, `__thread_runtime_init/shutdown` |
| `thread_runtime.c` | Thread pool (POSIX pthreads), ring-buffer job queue, future implementation. Auto-init/shutdown via `__attribute__((constructor/destructor))`. Compiled to `bin/thread_runtime.a` and automatically linked into every executable. |

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

// Struct methods / constructor / destructor
fn Foo.new(stack i32 x): Foo { ... }
fn Foo.method(stack i32 x): void { Foo.(x) = x; }
fn Foo.rem(void): void { ... }  // auto-called at scope exit

// Control flow
for (stack i32 i = 0; i < n; i++) {}
while (cond) {}    do {} while (cond);    inf {}
if (x) {} else if (y) {} else {}
match s { Shape.Circle(r) => { } _ => {} }
switch (x) { case 0: break; default: }
defer rem.(buf);

// Operators: + - * / %  +% -% *% (wrap)  +! -! *! (trap)
//            & | ^ ~ << >>  && || !  < > <= >= == !=
//            &x (addr-of)   x[i] (index)

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

// Struct attributes: @packed  @align(N)  @c_layout
// Fn/var attributes: @weak  @hidden  @restrict
// Variadic: fn foo(stack i32 n, ...): void
// Union: type U: union { i32 x; f32 y; }
// Bitfield: i32 flags: 3;  (inside struct)
```

**Module system**: every file starts with `mod name;` (root) or `mod dir.subdir.name;` (nested). The dotted name mirrors the directory path relative to the entry file: `mod printer.typewriter;` lives at `printer/typewriter.sts`. `imp other.mod;` splices that module's types/sigs into the current AST; dots in the name are converted to path separators for lookup. `int` = module-private, `ext` = exported. Library-backed imports: `lib "x" from "libx.a"; imp x;` — codegen skips bodies, linker resolves from `.a`.

---

## TODO

- [ ] Module system: build Stasha modules that import other `.sts` files into static libraries
- [x] Dotted module names + sts.sproj project file
- [x] Thread parallelism: `thread.(fn)(args)` + `future` type (thread pool, POSIX pthreads)
- [ ] Build system / package manager
- [ ] Standard library (string, I/O, math, collections in Stasha)
- [ ] Self-hosting compiler

**Open design questions:**
- `error` type: carry integer code alongside message?
- Error propagation `?` suffix operator (Rust/Zig style)?
