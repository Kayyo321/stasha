# File Headers

File headers (`@[[ ... ]]`) are declarative metadata that ride alongside Stasha source code. They control linkage, ABI, target features, codegen sections, conditional compilation, lifecycle (constructor / destructor) hooks, and per-decl diagnostics — all without a separate build-system file.

Every fileheader lives in one of three positions:

| Form | Attached to | Example |
|------|-------------|---------|
| File-wide | The whole module | `@[[freestanding]];` at the very top |
| Declaration-scoped | The next `fn` / `type` / global | `@[[weak]] fn panic_handler(...) ...` |
| Lifecycle block | A new `init` / `exit` block | `@[[init]] { ... }` |

Multiple keys may appear comma-separated inside one `@[[ ... ]]`, or stacked in several `@[[ ... ]]` groups. They merge.

---

## File-Wide Headers

A file-wide header ends with a semicolon and appears before `mod` (or anywhere at top level if no `mod` is present — see *Freestanding*).

```stasha
@[[freestanding]];
@[[org: 0x7c00]];

fn main(void): i32 => 0;
```

File-wide headers apply to the whole translation unit:

- Compiler driver flags (`freestanding`, `org`)
- Module-level documentation (`@[[doc: "..."]]`)
- Default ABI for unannotated symbols

### `freestanding`

Compile a non-module / non-mangled object file. Freestanding files have **no access to the Stasha standard library** and must be self-contained:

```stasha
@[[freestanding]];

ext fn main(void): i32 {
    print.('Hello, Freestanding!\n');
    ret 0;
}
```

Rules:

- No `mod` declaration is required.
- No symbol mangling — function names appear in the object file exactly as written.
- The thread runtime, zone runtime, libc, libm, and libpthread are **not** auto-linked. The linker is invoked with `-lSystem` / `-lc` / `-lm` / `-lpthread` removed and undefined symbols allowed (macOS).
- You may use `cheader` and `lib` for plain C interop, but you cannot `imp` Stasha modules — they would re-introduce the runtime.

### `org`

Set the load address of the produced binary. Useful for boot sectors, bare-metal firmware, and ROMs:

```stasha
@[[freestanding]];
@[[org: 0x7c00]];

fn main(void): i32 => 0;
```

The address is passed verbatim to the linker as the image base.

---

## Declaration-Scoped Headers

Place a header immediately before any top-level declaration to attach it to that single decl:

```stasha
@[[weak]]
fn panic_handler(i8 *msg): void {
    print.error.("panic: {}\n", msg);
}
```

Multiple keys can be combined inside one bracketed group:

```stasha
@[[weak, export_name: "alloc_error_handler"]]
fn on_alloc_error(usize_t requested): void { ... }
```

Or stacked:

```stasha
@[[weak]]
@[[export_name: "alloc_error_handler"]]
fn on_alloc_error(usize_t requested): void { ... }
```

### Linkage Attributes

| Key | Effect |
|-----|--------|
| `weak` | Symbol becomes weak — can be overridden by a strong definition at link time |
| `hidden` | Symbol is not exported from a shared library (`-fvisibility=hidden`) |
| `export_name: "..."` | Override the linker symbol name (skip module mangling) |
| `abi: c` | Force C ABI on a single declaration (struct return / variadic / arg layout) |
| `section: "name"` | Place the symbol in a specific linker section |
| `align: N` | Force a specific alignment in bytes |

```stasha
// Plugin-style API: hide internal helpers, expose a stable C ABI
@[[export_name: "plugin_init"]]
fn initialize_plugin(void): void { ... }

@[[abi: c, export_name: "plugin_tick"]]
fn tick(stack i32 frame_no): void { ... }
```

Build with `stasha lib plugin.sts -o libplugin.a` and the resulting archive exposes `plugin_init`, `plugin_shutdown`, `plugin_tick` as plain C symbols — no module-mangled prefix.

### Section Placement

`section:` writes the symbol into a specific Mach-O / ELF section. Combine with `align:` for hardware alignment:

```stasha
@[[section: "__TEXT,__vectors"]]
int fn reset_vector(void): i32 {
    ret 0;
}

@[[section: "__DATA,__boot_config"]]
int stack u32 boot_magic = 0x53545348; // 'STSH' little-endian

@[[section: "__TEXT,__fastcode", align: 64]]
fn crc32_step(stack u32 state, stack u32 byte): u32 {
    ret (state << 1) ^ byte;
}
```

Section names use the platform's native syntax. The compiler does not validate them — invalid sections surface at link time.

### Target Features

`target:` sets LLVM's `target-cpu`. `features:` sets `target-features`. Both apply to that single function and override the host default:

```stasha
@[[target: "x86_64", features: "+sse4.2,+popcnt"]]
fn hash_sse(stack u8 *r text): u32 { ... }

@[[target: "aarch64", features: "+neon"]]
fn hash_neon(stack u8 *r text): u32 { ... }
```

This is per-function, not per-module — you can ship multiple feature variants and let a runtime dispatcher pick one.

### Conditional Compilation: `if` and `require`

`if:` silently elides the next declaration when the condition evaluates false:

```stasha
@[[if: os == "linux"]]
ext fn epoll_create1(stack i32 flags): i32;

@[[if: os == "windows"]]
ext fn CreateEventA(stack void *r attrs, stack i32 manual_reset,
                    stack i32 initial_state, stack i8 *r name): void *;

@[[if: pointer_width == 64]]
type Handle: u64;

@[[if: pointer_width == 32]]
type Handle: u32;
```

`require:` is the strict form — if the condition is false on the current target, compilation **fails** with a clear error rather than silently dropping the symbol:

```stasha
@[[require: os == "linux"]]
ext fn inotify_init1(stack i32 flags): i32;
```

Available condition keys:

| Key | Values |
|-----|--------|
| `os` | `"linux"`, `"macos"`, `"windows"`, `"freebsd"`, ... |
| `arch` | `"x86_64"`, `"aarch64"`, `"riscv64"`, ... |
| `pointer_width` | `32`, `64` |
| `target.os` / `target.arch` | Same as above, scoped to the explicit `--target` triple |
| `endian` | `"little"`, `"big"` |

Operators inside the condition: `==`, `!=`, `&&`, `||`, parentheses.

### Diagnostic Scope

Locally tune compiler warnings using a `push` / `pop` pair. Inside the scope, `@[[diagnostic: ignore("...")]]` silences specific warnings without affecting other code:

```stasha
@[[diagnostic: push]]
@[[diagnostic: ignore("unused-param")]]
@[[diagnostic: ignore("shadow")]]
fn legacy_callback(i32 code, i8 *data): void {
    i32 code = 4;                 // shadowing
    print.("{} {}\n", code, data);
}
@[[diagnostic: pop]]
```

If `push` is not paired with `pop`, the diagnostic state leaks into the rest of the file. The compiler emits a warning at end-of-file when this happens.

### Documentation

`@[[doc: "..."]]` (or a multi-key bare-text group) attaches a doc comment to the next declaration. Tools like `stasha doc` (planned) consume this:

```stasha
@[[
    doc: div(i32, i32) -> i32 (returns 0 on divide-by-zero)
    returns: i32
    params: two 32-bit integers a and b
]];
int fn div(stack i32 a, stack i32 b): i32 { ... }
```

The bare-text form (no `key:`) is captured verbatim until the closing `]]`. Use it for free-form prose; use keyed entries (`returns:`, `params:`, `since:`) for structured fields.

---

## Lifecycle Blocks: `@[[init]]` and `@[[exit]]`

Lifecycle blocks register code to run **before `main`** (init) or **after `main` returns** (exit). They are the Stasha equivalent of C's `__attribute__((constructor))` / `((destructor))`.

```stasha
mod on_init;

int heap i8 *rw data = nil;
int macro let byte_cnt! = 32;

@[[init]] {
    print.('hi! this code actually runs before main!\n');
}

@[[init]] "title_of_this" {
    for (stack i32 i = 0; i < byte_cnt.!; i++) {
        data[i] = `1` + i;
    }
}

@[[init: before("title_of_this")]] {
    data = new.(sizeof.(i8) * byte_cnt.!);
}

@[[exit]] "this_other_thing" {
    print.('bye! this code is going to run after main returns!\n');
    rem.(data);
    data = nil;
}

@[[exit: after("this_other_thing")]] {
    if (data != nil) {
        print.('Data was not cleaned up!\n');
    }
}

fn main(void): i32 {
    print.('data = {}\n', data);
    ret 0;
}
```

Output:

```
hi! this code actually runs before main!
data = 1234567890123456789012345678901
bye! this code is going to run after main returns!
```

### Titles, `before`, `after`

A bare-string label (`"title_of_this"`) names a block. Names are file-wide unique and cross-file: a block in `b.sts` may declare itself `before("title_in_a")` if `a.sts` defined that title.

The compiler topologically sorts blocks using the `before`/`after` graph. A cycle (`A before B`, `B before A`) is a hard error at compile time.

### Dispatcher Emission

Mach-O and ELF disagree about same-priority constructor order. Rather than relying on the linker, Stasha emits **one** dispatcher function per phase:

- `__fh_ctor_dispatch` — registered as the sole entry in `@llvm.global_ctors`. Calls every `@[[init]]` block in topological order.
- `__fh_dtor_dispatch` — registered in `@llvm.global_dtors`. Calls `@[[exit]]` blocks in reverse topological order.

This gives identical behavior on every supported platform.

### Restrictions

- Lifecycle blocks must be `void`-returning and take no parameters.
- They cannot use `this`.
- They run with the thread runtime / zone runtime already initialised on hosted builds. On `freestanding` builds, the runtimes are not available — keep init/exit blocks pure.

---

## Combining Headers

Headers compose. The next declaration receives the union of every preceding `@[[...]]` group:

```stasha
@[[weak]]
@[[export_name: "alloc_error_handler"]]
@[[section: "__TEXT,__error_handlers", align: 16]]
fn on_alloc_error(usize_t requested): void { ... }
```

Keys are commutative within a single declaration except for `diagnostic: push/pop`, which is positional and lexically scoped.

---

## Body-less Declarations

Headers are most useful on `ext` declarations imported from C or other libraries. Stasha allows body-less `ext fn` so a header can gate it:

```stasha
@[[if: os == "linux"]]
ext fn epoll_create1(stack i32 flags): i32;

@[[if: os == "windows"]]
ext fn CreateEventA(stack void *r attrs, stack i32 manual_reset,
                    stack i32 initial_state, stack i8 *r name): void *;
```

The semicolon (no `{ ... }` body) is required — the function is provided by the linker, not Stasha.

---

## Error Reference

| Error | Cause |
|-------|-------|
| `expected fileheader attribute name` | Empty `@[[ ]]` or non-identifier first token |
| `unknown fileheader key 'foo'` | `foo:` is not a recognised key |
| `unexpected token in fileheader value` | Value couldn't be parsed (e.g. unbalanced quotes) |
| `cyclic dependency in init blocks` | Two `@[[init]]` blocks reference each other via before/after |
| `init block references unknown title 'x'` | No `@[[init]] "x"` exists in any compiled file |
| `diagnostic: pop without matching push` | Unbalanced push/pop in same file |
| `freestanding modules cannot import other Stasha modules` | `imp other_mod;` in a `@[[freestanding]]` file |
| `require condition not met for current target` | `@[[require: ...]]` failed |

---

## Implementation Notes

The fileheader pipeline is wired through:

- `src/ast/ast.h` — `fileheader_t`, `fh_entry_t`, `fh_cond_t`, lifecycle node kinds (`NodeInitBlock`, `NodeExitBlock`)
- `src/parser/parse_decls.c` — fileheader parser, lifecycle block parser, integration with `parse_top_decl`
- `src/codegen/cg_fileheaders.c` — target descriptor evaluation, condition checks, lifecycle topo sort, dispatcher emission
- `src/main.c` — freestanding driver branch (skips runtime libs and switches the linker invocation)
- `src/linker/linker.cpp` — `link_object_freestanding` strips `-lSystem` / `-lc` / `-lm` / `-lpthread` and allows undefined symbols on macOS

For more, read `tests/fileheaders/` — every example in this page corresponds to a passing test in that directory.
