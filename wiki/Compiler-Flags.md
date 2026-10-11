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

### Target operands, defaults, and errors

The supported spelling is `--target <triple>`, with a separate, nonempty operand
that does not begin with `-`. A missing, empty, or option-like operand returns
exit status 1 with this fixed diagnostic:

```text
invalid --target argument (expected --target <triple> with a nonempty, non-option operand)
```

The exact `--target=` prefix is rejected, with or without a value:

```text
unsupported --target= spelling (use --target <triple>)
```

Omitting `--target` retains the native/default target. With `--kernel`, omission
retains the existing `x86_64-unknown-none-elf` default. `native` is not a new
literal target spelling. Nonempty, non-option operands continue through the
existing LLVM normalization and target resolver; this argument check adds no
supported targets. An unknown nonempty triple retains its existing resolver
error and compilation-failure output invalidation, rather than the early
argument-error behavior below.

Repeated valid target options use the last value. Every malformed occurrence
actually parsed by the common option loop fails immediately, even if another
valid target follows. An earlier nonempty unknown triple may still be replaced
by a later valid triple before resolution. Tokens consumed as another option's
operand (for example, by `-o` or `--path`) are not rescanned as target options.
Existing command/file/option positioning rules are unchanged.

The first `--help`, `-h`, or `--version` encountered in the global early scan
still exits successfully before ordinary option parsing, including when it
appears where a target operand would be. Otherwise, common-loop order determines
the first error: an earlier malformed editor coordinate can precede a malformed
target; a parsed malformed target precedes the later kernel/editor restriction
and missing-coordinate checks.

Early target-argument rejection occurs before project loading, stdin or source
reading, target resolution, code generation, output invalidation, and linking.
Ordinary commands have empty stdout and one actionable target error on stderr;
the existing logger prefix and abort summary may accompany it. Editor commands
return exactly one diagnostics JSON document with one error and empty stderr.

For these early argument errors, pre-existing compiler output bytes, modes,
symlinks, and directories are preserved, and no new compiler output or sidecar
is created. This guarantee excludes paths and aliases in the logger-owned
namespace: ordinary logger initialization happens before option parsing and
can create or rotate logs. It is not a promise of zero filesystem writes.

---

## Editor / Tooling Flags

These are used by IDE integrations, not normally invoked by hand:

| Flag | Description |
|------|-------------|
| `stasha tokens <file>` | Tokenize source, output JSON token stream |
| `stasha symbols <file>` | List all symbols in the file |
| `stasha definition <file>` | Find definition at a position |
| `--path <path>` | Virtual file path (for stdin-based usage) |
| `--line <n>` | Zero-based editor cursor line |
| `--col <n>` | Zero-based editor cursor column |
| `--stdin` | Read source from stdin |

`--line` and `--col` require a nonempty sequence of ASCII decimal digits in
the range `0` through `ULONG_MAX - 1` for the compiler host (`18446744073709551614`
on 64-bit Linux). The upper limit reserves room for the editor's internal
one-based position conversion. Leading zeros are accepted; signs, whitespace,
suffixes, and overflowing values are rejected before source input is read.
Missing or invalid operands return exit status 1 and a JSON diagnostic for
editor commands. Repeated valid options use the last value. `definition`
requires both options; existing default coordinates for other commands are
unchanged. An in-range position may lie outside the source file.

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
stasha hello.sts && ./a.out

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
