# Compiler Flags

Complete reference for all `stasha` command-line options.

---

## Subcommands

```
stasha [build] <file.sts>     Compile to executable (default)
stasha lib     <file.sts>     Compile to static library (.a)
stasha dylib   <file.sts>     Compile to dynamic library (.dylib / .so)
stasha test    <file.sts>     Run test blocks
stasha                        Project mode — reads sts.sproj in CWD
stasha build                  Project mode (explicit)
```

---

## General Flags

| Flag | Description |
|------|-------------|
| `-o <path>` | Set output file path |
| `-g` | Emit DWARF debug info (+ `.dSYM` bundle on macOS) |
| `-o=N` | Optimization level: 0 (none), 1, 2 (default), 3 (max) |
| `--target <triple>` | Cross-compile target (LLVM triple) |
| `-l <lib>` | Link additional library (e.g. `-l ssl`) |
| `--version` | Print version string and exit |
| `-h`, `--help` | Print help and exit |

---

## Cross-Compilation Targets

Pass an LLVM target triple:

```bash
stasha app.sts --target x86_64-unknown-linux-gnu      # Linux x86_64
stasha app.sts --target aarch64-apple-macosx13.0      # macOS ARM64
stasha app.sts --target x86_64-pc-windows-msvc        # Windows x86_64
stasha app.sts --target wasm32-unknown-wasi            # WebAssembly
```

---

## Editor / Tooling Flags

These are used by IDE integrations, not normally invoked by hand:

| Flag | Description |
|------|-------------|
| `stasha tokens <file>` | Tokenize source, output JSON token stream |
| `stasha symbols <file>` | List all symbols in the file |
| `stasha definition <file>` | Find definition at a position |
| `--path <path>` | Virtual file path (for stdin-based usage) |
| `--line <n>` | Editor cursor line |
| `--col <n>` | Editor cursor column |
| `--stdin` | Read source from stdin |

---

## Optimization Levels

| Level | Effect |
|-------|--------|
| `-o=0` | No optimization. Fast compile, slowest code. Use for debug builds. |
| `-o=1` | Basic optimization. Fast compile, reasonable code. |
| `-o=2` | Balanced optimization (default). Most programs use this. |
| `-o=3` | Aggressive optimization. Slowest compile, fastest code. Use for release builds. |

---

## Common Invocations

```bash
# Build and run quickly:
stasha hello.sts && ./hello

# Debug build:
stasha app.sts -g -o=0 -o bin/debug_app

# Release build:
stasha app.sts -o=3 -o bin/app

# Build a library:
stasha lib engine.sts -o libs/libengine.a

# Cross-compile for Linux:
stasha app.sts --target x86_64-unknown-linux-gnu -o bin/app_linux

# Run tests:
stasha test mymodule.sts

# Build project:
stasha          # reads sts.sproj

# Link extra libraries:
stasha net_app.sts -l ssl -l crypto -o bin/net_app
```
