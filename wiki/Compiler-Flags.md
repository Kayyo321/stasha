# Compiler Flags

Complete reference for all `stasha` command-line options.

---

## Subcommands

```
stasha [build] <file.sts>     Compile to executable (default)
stasha lib     <file.sts>     Compile to static library (.a)
stasha dylib   <file.sts>     Compile to dynamic library (.dylib / .so)
stasha test    <file.sts>     Compile a test executable (see observation flags below)
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

## Opt-in Test Observations

These options compile a fresh test executable and run that exact image once:

```bash
stasha test module.sts --warn-memleak
stasha test module.sts --max-leaked-bytes 0
stasha test module.sts --min-test-coverage 90
stasha test module.sts --warn-memleak --max-leaked-bytes 0 --min-test-coverage 90
```

Without these flags, explicit single-file `stasha test file.sts` retains its
compile-only behavior. Run its executable separately as before. Existing project
and external test harness behavior is unchanged. Do not execute an observation
image a second time to obtain this command's measurement.

- `--warn-memleak` enables allocation balance and warns if any tracked allocation
  remains. A leak warning alone does not fail an otherwise successful invocation,
  including with `--strict`.
- `--max-leaked-bytes B` enables allocation balance and passes when complete
  outstanding requested bytes are at most B. Equality passes.
- `--min-test-coverage P` enables executable statement/control-line coverage and
  passes when the complete nonempty covered/eligible ratio is at least P/100.
  Equality passes. Supply `90` for a 90% minimum; there is no implicit minimum or
  separate report-only coverage flag.

B is an unsigned decimal uint64 integer (`0` through `18446744073709551615`). P is
an integer from `0` through `100`. Only ASCII decimal digits are accepted, with
leading zeros allowed. Signs, whitespace, fractions, suffixes, NaN, infinity,
exponents, missing operands and overflow fail immediately. Use separate operands,
not `--flag=value`. Repeated well-formed thresholds use their last values; every
malformed occurrence fails even if followed by a valid one. Enablers are
idempotent. Help/version keep their existing early-exit precedence. Early errors
use the editor's single diagnostics JSON document when invoked in an editor mode.

### Qualified profile and exclusions

The initial profile is `native-sequential-leaf-v1`: one ordinary on-disk `.sts`
file, explicit test mode, native hosted Linux x86-64 GNU LP64. Its source is
bounded to 16 MiB and one million physical lines. Other targets, project mode,
stdin or virtual-path overrides, imports/libraries/C providers and additional
link libraries fail before observation compilation. Ordinary unflagged commands
retain their existing behavior.

The admitted language subset is sequential leaf code: directly named plain source
functions, primitive scalar/pointer variables, fixed primitive arrays and literal
strings; supported arithmetic and primitive operations; ordinary conditionals,
loops, integer switch/match and guards; lexical blocks, return/break/continue and
defer; explicit new/mov/rem; primitive test assertions and fixed-literal print.
The compiler checks all admitted bodies, including never-called functions and
untaken branches. Unknown origins, kinds or calls fail closed.

Generic/aggregate/interface/descriptor routes, indirect calls, closures,
variadics, slices and implicit allocation families, zones, threads/futures/async,
coroutines, watch/send/quit, lifecycle blocks, macro-bearing input, assembly,
unsafe/unchecked forms, raw native calls and integer-to-pointer construction are
unsupported. This profile is not a general memory-safety proof. The exhaustive
qualification contract and fixtures are in [tests/observation/README.md](../tests/observation/README.md).

### What the measurements mean

Allocation balance is the sum of requested sizes of successful live explicit
allocations at the suite endpoint, after all discovered tests return and lexical
cleanup/defer runs. Production and test-body allocations both count. Stack/static
storage, allocator slack, runtime/libc-private storage and tracker metadata do not
count. Reachability is irrelevant: a reachable allocation still counts until
released. A non-null size-zero allocation warns while satisfying a zero-byte
maximum. Zero bytes does not prove whole-process leak freedom.

Failed nonzero allocation creates no record. Failed nonzero resize preserves the
original record. A known-base free closes one record; null free is a no-op.
Reached zero-size resize (including null-base resize), unknown/non-base release or
resize, capacity failure and accounting overflow make observation incomplete and
stop before an unsupported allocator call. This is a closed-profile guard, not a
claim to detect stale aliases or ownership errors.

Coverage counts unique owned source lines containing admitted executable statement
or explicit control-clause anchors. Multiple statements on one line count once.
Multiline control clauses and selected short-circuit/ternary arms use their actual
anchors. Deferred body lines count when cleanup executes. Never-called production
functions remain eligible. Test bodies, replaced root main, nonexecutable and
compile-time-only text, inactive target branches and structurally unreachable
post-terminator statements are excluded with recorded reasons. This is neither
branch coverage nor full expression coverage; optimization does not redefine the
source denominator.

A coverage minimum fails for `N/A (0/0)`, even at P=0. Zero discovered tests fails
all observation invocations. A normally completed test with zero assertions is
allowed and reports zero. Missing/incomplete data, failed tests, child failure or
signal, launch/wait errors and any failed threshold always fail the CLI. A valid
report cannot override a nonzero child exit. Disabled metrics are labeled disabled.

### Reports and runtime packaging

The command prints a summary to stderr and the path of a unique private evidence
directory under `/tmp/stasha-observation-*`. It retains a separate bounded machine
report and driver JSON with source/map/configuration/compiler/runtime/binary
identities, expected tests, sites and exclusions, actual child status and result.
Valid UTF-8 paths and names remain readable in driver JSON. Dynamic text also has
`*_bytes_hex` companions preserving exact source/filename bytes, including invalid
UTF-8 bytes. Program stdout is never parsed as a measurement. Reports use schema/runtime/metric
version 1; stale, mismatched, duplicate, malformed, oversized, symlinked or incomplete
reports fail. Private run IDs prevent accidental mixups, not adversarial same-user
forgery or tampering. Evidence directories are retained for inspection and can be
removed by their owner when no longer needed.

The compiler requires its matching `observation_runtime.a` beside the compiler
executable. Native Linux x86-64 `make all` builds it and `make install PREFIX=...`
copies it into that workspace's `bin` directory. Other hosts do not acquire this
runtime dependency for ordinary compilation. The release workflow is not yet
updated for observation packaging; this checkpoint makes no public binary-release
claim.

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

# Compile, then run ordinary tests:
stasha test mymodule.sts && ./a.test

# Compile and run observed native leaf tests exactly once:
stasha test mymodule.sts --max-leaked-bytes 0 --min-test-coverage 90

# Build project:
stasha          # reads sts.sproj

# Link extra libraries:
stasha net_app.sts -l ssl -l crypto -o bin/net_app
```
