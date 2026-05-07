# Building Projects

This page covers all the ways to build Stasha programs and libraries.

---

## Single-File Programs

```bash
stasha hello.sts              # compile + link → ./hello
stasha hello.sts -o greet     # specify output name
```

---

## Project Mode

When `sts.sproj` is present in the current directory:

```bash
stasha           # build using sts.sproj
stasha build     # same
```

---

## Static Libraries

```bash
stasha lib mylib.sts -o libmylib.a
```

---

## Dynamic Libraries

```bash
stasha dylib mylib.sts -o libmylib.dylib   # macOS
stasha dylib mylib.sts -o libmylib.so      # Linux
```

---

## Running Tests

```bash
stasha test myfile.sts
```

Finds and runs all `test 'name' { ... }` blocks in the file. See [Testing](Testing).

---

## Compiler Flags

### Output Path

```bash
stasha hello.sts -o bin/hello
```

### Optimization Level

```bash
stasha hello.sts -o=0    # no optimization (fastest compile)
stasha hello.sts -o=1    # basic optimization
stasha hello.sts -o=2    # default (balanced)
stasha hello.sts -o=3    # aggressive optimization
```

### Debug Symbols

```bash
stasha hello.sts -g      # emit DWARF debug info (.dSYM on macOS)
```

Enables source-level debugging with `lldb` or `gdb`. See [Debugging](Debugging).

### Cross-Compilation

```bash
stasha hello.sts --target x86_64-unknown-linux-gnu
stasha hello.sts --target aarch64-apple-macosx13.0
stasha hello.sts --target x86_64-pc-windows-msvc
```

### Additional Link Libraries

```bash
stasha hello.sts -l ssl -l crypto
```

---

## Build Workflow for Multi-Module Projects

### 1. Build dependencies first

```bash
stasha lib src/engine.sts -o libs/libengine.a
stasha lib src/net.sts    -o libs/libnet.a
```

### 2. Build the main program

```bash
stasha build src/main.sts -o bin/app
```

Or with a project file, just:
```bash
stasha
```

### 3. Build the standard library

The stdlib must be built before using `from std` imports:

```bash
make stdlib
```

Outputs go to `bin/stdlib/lib<module>.a`.

---

## Compiler Build Stages

When you run `stasha file.sts`, the following pipeline runs:

1. **Lex** — tokenize the source
2. **Preprocess** — expand macros
3. **Parse** — build the AST
4. **Resolve imports** — splice imported module ASTs, tag library-backed modules
5. **Codegen** — 3 passes:
   - Register types (structs, enums, aliases)
   - Generate function bodies → LLVM IR
   - Generate test blocks → LLVM IR
6. **LLVM compile** — IR → native object file
7. **Link** — object + runtime + libraries → executable

The thread runtime (`bin/thread_runtime.a`) and zone runtime (`bin/zone_runtime.a`) are linked into every executable automatically.

---

## Makefile Targets (for the Compiler Itself)

If you're building Stasha from source:

```bash
make              # build bin/stasha (the compiler)
make stdlib       # compile all stdlib modules → bin/stdlib/
make stdlib-test  # run stdlib test suite
make clean        # remove build artifacts
make clean-stdlib # remove stdlib artifacts
make llvm         # build LLVM from source (first-time setup)
```

---

## Example: Full Project Build

```
project/
  sts.sproj
  src/
    main.sts
    utils.sts
  libs/
    myutil.a
    myutil.sts
```

`sts.sproj`:
```
main     = "src/main.sts"
binary   = "bin/app"
ext_libs = [("libs/myutil.a" : "libs/myutil.sts")]
```

```bash
cd project
stasha          # → bin/app
./bin/app
```

---

## Debug Build vs Release Build

```bash
# Debug build (slower, full info)
stasha myapp.sts -g -o=0 -o bin/myapp_debug

# Release build (optimized)
stasha myapp.sts -o=3 -o bin/myapp
```
