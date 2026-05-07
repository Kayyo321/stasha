# Linking System

Stasha uses LLD (LLVM's built-in linker) for all linking. No external `ld` or `link.exe` is required.

---

## What Gets Linked Automatically

Every Stasha executable automatically links:

- **`bin/thread_runtime.a`** — thread pool, future implementation
- **`bin/zone_runtime.a`** — arena allocator runtime
- System libraries (libc, etc.) as needed by the platform

You don't need to specify these manually.

---

## Link Modes

### Executable

```bash
stasha app.sts -o bin/app
```

Produces a fully linked native binary.

### Static Library

```bash
stasha lib mylib.sts -o libmylib.a
```

Produces a `.a` archive. Does not link — the consumer links it when building their executable.

### Dynamic Library

```bash
stasha dylib mylib.sts -o libmylib.dylib    # macOS
stasha dylib mylib.sts -o libmylib.so       # Linux
```

Produces a shared library linked at load time.

---

## Linking External C Libraries

### System Libraries

```stasha
lib "ssl";              // links -lssl
lib "crypto";           // links -lcrypto
lib "pthread";          // links -lpthread
```

Plus any extra via command line:
```bash
stasha app.sts -l ssl -l crypto -o bin/app
```

### Custom Static Libraries

```stasha
lib "mylib" from "path/to/libmylib.a";
```

Or in `sts.sproj`:
```
ext_libs = [("libs/mylib.a" : "libs/mylib.sts")]
```

---

## Library Search Order

When resolving `lib "name" from "path.a"`:

1. Path is resolved relative to the **importing source file**
2. Falls back to system library search paths (`-L` paths)

For `from std`:
1. Resolved to `<compiler_dir>/bin/stdlib/lib<name>.a`

---

## Name Mangling

Stasha mangles symbol names to include module paths:

```
module__submodule__function_name
```

C functions imported with `lib` are never mangled — they use their original C names.

`ext fn` functions exported from Stasha use the mangled name. Use `@c_layout` structs and `ext fn` to create a clean C-compatible ABI.

---

## Link Flags Summary

| Situation | How to link |
|-----------|-------------|
| C standard library | `lib "stdio";` etc. |
| Custom static library | `lib "name" from "path.a";` |
| Stdlib module | `libimp "name" from std;` |
| Extra C lib (command line) | `-l name` |
| Project libraries | `ext_libs` in `sts.sproj` |

---

## LLD Internals

The linker is implemented in `src/linker/linker.cpp`. It wraps LLD's C++ API:

- `link_object(obj, output, extra_libs[])` — link to executable
- `archive_object(obj, output)` — bundle to `.a`
- `link_dynamic(obj, output, extra_libs[])` — link to `.dylib`/`.so`

LLD supports MachO (macOS), ELF (Linux), PE/COFF (Windows), and Wasm.
