# Typed scalar boundary matrix (P1)

This suite is a generated, arbitrary-precision differential oracle for Stasha's
integer contract. It requires Python 3.8+ and no third-party packages. The Python
self-tests exercise the reference model and inject mock compiler/program failures;
they **do not qualify the actual compiler**.

## Commands

From the repository root:

```sh
# Python oracle + fail-closed runner self-tests; no LLVM or Stasha executable.
python3 tests/scalars/test_scalars.py

# Inspect/generate every source and its exact reference output without compiling.
python3 tests/scalars/run.py --list
python3 tests/scalars/generate.py --output /tmp/stasha-scalar-corpus

# Host C-only hook/linkage verification; no Stasha/LLVM compiler needed.
python3 tests/scalars/run.py --self-test-hook --cc cc

# After building the provenance-verified compiler and runtime archives:
python3 tests/scalars/run.py --compiler ./bin/stasha --opt 0,1,2,3 \
  --build-trap-hook --require-trap-reasons \
  --artifacts /tmp/stasha-scalar-results

# Narrow a development iteration without silently changing the inventory.
python3 tests/scalars/run.py --compiler ./bin/stasha --opt 0 \
  --filter '^(boundaries_u64|arithmetic_u64|casts_all_widths)$'
```

The driver's optimization option is `-o=N`; its output option is separately
`-o PATH`. The runner uses those exact forms. Every invocation creates a new
artifact directory; each compiler invocation also uses its case directory as
the working directory to isolate logs and other process artifacts. The runner
retains all generated sources, a manifest, source/compiler/
executable SHA-256 hashes, pre/post IR when available, exact commands, environment
overrides, elapsed times, compiler and target return codes, separate stdout/stderr,
and a machine-readable `summary.json`. A `RUNNING` summary is not a passing result.

The default corpus has 577 cases: 33 success, 208 runtime traps, and 336 diagnostic
rejections. At four optimization levels this is 2,304 independent compiler runs.
All positive numeric observations print a u64 bit pattern with 16 hex digits;
Python supplies the entire expected output, including a unique completion marker.
Boolean observations take a branch and emit `0` or `1`. Compiler-side comparisons
to expected numeric literals are not the runtime oracle.

## Contract and coverage

- i/u8/16/32/64 zero, one, signed minimum/maximum, negative one, unsigned top bit
- Local/global/const literals, function argument/return contexts, nested literal
  contexts, default i32 → i64 → u64 selection, and one-beyond-range rejection
- All 64 integer cast pairs, source-signedness extension and truncation,
  same-signedness widening in both operand orders, mixed-signedness arithmetic
  rejection, and local/argument/return implicit narrowing or signedness-loss rejection
- Boundary cross-products for ordinary/wrapping/checked arithmetic, all comparisons,
  bitwise operators, signed/unsigned division and remainder, right/left shifts
- Modular ordinary `+`, `-`, `*`, unary negation and explicit `+%`, `-%`, `*%`;
  width-specific `+!`, `-!`, `*!` overflow traps and non-overflow results
- Invalid shift counts -1, width, 256, and u64 maximum; division/remainder by zero; signed
  MIN/-1 division **and remainder**; ordinary and compound paths
- Global constant expressions and matching runtime observations, compile-time
  rejection of exceptional arithmetic, and runtime/compile-time short-circuiting
- An untouched high-half address constant roundtrip through `void *`; this is a
  64-bit-host roundtrip check, **not** a proof of target relocation/boot correctness

Arithmetic exceptions have independent Python labels `overflow`,
`division-by-zero`, `division-overflow`, and `invalid-shift`. Division truncates
quotients toward zero and gives remainder the dividend's sign. Shift counts are
validated before any count narrowing. Explicit casts reinterpret modulo the
receiving width after interpreting the source at its own signedness and width.

## Exact panic-hook qualification

The common runtime ABI is `__stasha_panic(uint32_t reason)`, a non-returning
function. Hosted code supplies a weak default that executes `llvm.trap`;
freestanding code declares the symbol for its runtime to supply. This suite's
`panic_hook.c` provides a **test-only strong override**:

- 1: `overflow`
- 2: `division-by-zero`
- 3: `division-overflow` (signed MIN/-1 division and remainder)
- 4: `invalid-shift`
- 5: `fp-cast-range` (hook implemented/tested; FP casts are outside this integer corpus)

Known reasons print exactly `STASHA_TRAP:<reason>\n` to stderr and terminate with
status 86. Unknown IDs terminate with status 87 and an `unknown:<id>` diagnostic;
reserved IDs 6/7/8 are not silently accepted as an arithmetic trap.

`--build-trap-hook` uses the installed host `cc`, or `--cc /path/to/trusted/compiler`
(`CC` is also respected as a single executable name/path). It compiles the hook
with C11, warnings-as-errors, and no LTO, retaining source/object/compiler hashes,
compiler version, commands, and diagnostics. A hook-build failure fails the suite.
Only runtime-trap cases link it, through the existing driver interface:

```sh
stasha build case.sts -o=2 -o program -l /absolute/path/panic_hook.o
```

The driver passes that object directly to the linker. Using a plain static archive
would be unsafe here: the program's weak definition can satisfy the symbol before
the linker extracts the archive's strong definition. Direct object linkage avoids
that trap. Hook linkage does not modify positive or diagnostic-rejection cases.
The hook is host-native; cross-target builds need a matching compiler/runtime and
are not qualified by this hosted suite.

The acceptance invocation uses `--build-trap-hook --require-trap-reasons`. It
compares the complete stderr output and exact status against the intended case
reason, so an unrelated crash, wrong reason, extra diagnostic, or generic nonzero
exit cannot pass. `--self-test-hook` separately uses an actual host C weak-default
probe to verify strong override/linkage for IDs 1–5 and an unknown-ID case. That
probe is **not** Stasha/LLVM execution evidence.

For a separately integrated reason-reporting hook, the runner also accepts
`--trap-exit-code 86 --trap-hook-pattern '^STASHA_TRAP:{reason}$'` instead of
`--build-trap-hook`. The pattern must contain `{reason}`. A reason-bearing,
signal-terminating hook can omit `--trap-exit-code`.

### Signal-only development mode

Without a hook, fixtures disable the hosted crash handler with
`@[[crash: disable]];` and require the exact direct POSIX signal: SIGILL on x86,
SIGTRAP on AArch64. `--trap-signal` explicitly selects a verified backend signal.
A signal proves the trap mechanism but **cannot prove its intended reason**.
Signal-only results set `trap_reason_verified: false` and
`full_p1_trap_reason_qualification: false`; `--require-trap-reasons` makes those
cases fail. They must not be reported as full P1 acceptance.

## Failure policy and limitations

There are no skips or xfails in compiler execution. Missing compilers, empty case
selections, malformed options, diagnostics without source locations, unexpected
warnings, errors returned with exit zero, stale or missing outputs, abnormal
compiler termination, missing completion markers, mismatched bytes, unexpected
stderr, and timeouts are failures. Compilation failure never runs a prior binary.
Timeouts terminate the compiler/program process group on POSIX.

`test_scalars.py` uses executable Python mocks to prove these failure paths. The
reference self-test exhausts all signed/unsigned 8-bit input pairs for add/sub/mul,
checked and wrapping behavior, and checks division/remainder invariants. Larger
widths have deterministic random and boundary tests. The corpus is intentionally
small and deterministic rather than a long-running fuzzer.

Hosted numeric execution depends on the language's formatting and pointer-cast
paths too; such failures stay visible instead of being silently excused. It is not
a substitute for freestanding target, ABI, relocation, or exact hook qualification.
Actual Stasha/LLVM execution results must be recorded separately after the pinned
compiler build is available. Python self-test success alone makes no such claim.

## Production constant engine check

`python3 tests/scalars/test_constant_engine.py --exhaustive` compiles the exact
production `scalar_integer_semantics.h` engine into a small C probe with UBSan and
compares 2,752,600 results against the Python oracle. This covers all signed and
unsigned 8-bit pairs for every binary operation, plus wider random/boundary and
shift-count cases. It does not execute emitted LLVM code.

The suite also tests full-width signed/unsigned match and switch dispatch,
hexadecimal patterns, signed minima, duplicate patterns, and contextual rejection.
Implicit unsigned-to-strictly-wider-signed conversion is accepted because every
source value fits; signedness-changing binary arithmetic still requires a cast.

## Production LLVM construction check

```sh
python3 tests/scalars/test_ir_lowering.py --llvm-build /path/to/pinned/llvm-build
```

This builds `scalar_ir_probe.c` against the pinned LLVM23 libraries, invokes the
production integer builders for all eight types, checks pre-optimization opcode
signedness and overflow intrinsic widths, and verifies all 64 generated functions
before and after O0–O3 passes. It does not exercise frontend typing or run emitted
code. Shift opcode checks here assume a previously validated count; invalid-count
semantics are covered by the full compiler matrix.

## Focused floating conversion complement

```sh
python3 tests/scalars/test_fp_conversions.py --compiler ./bin/stasha --opt 0,1,2,3 \
  --build-trap-hook --require-trap-reasons --artifacts /tmp/stasha-fp-results
```

These 14 cases (56 executions) exercise f32/f64 signed and unsigned conversion,
truncation, integer boundary conversion, NaN truth/inequality/conditional behavior,
and exact reason-5 NaN/infinity/range traps. This is conversion coverage, not a
complete floating arithmetic or cross-platform ABI qualification.

## Imported module scope regression

```sh
python3 tests/scalars/test_module_scope.py --compiler ./bin/stasha \
  --artifacts /tmp/stasha-module-scope
```

This O0–O3 object/C execution test separates source-name lookup from exact emitted
symbol lookup. Imported globals/functions intentionally collide with root names;
the test checks reads, assignment, compound/index assignment, address-of and
export-name overrides. The K0 component harness separately tests the unchanged
component built through static-library mode.


## Ordinary imported test context

```
python3 tests/scalars/test_imported_tests.py --compiler ./bin/stasha --opt 0,1,2,3 \
  --artifacts build/ordinary-imported-tests
```

This focused eight-case family checks generated tests in their owning lexical
module: colliding root/leaf helper and global names, root tests after a leaf's
last helper, imported tests with no helper, empty imported tests, sibling aliases,
reversed import order, dependencies beside a symlinked requested entry, and fresh
local-pointer provenance between prior helpers and separate test bodies. Pointer
controls assert each allocation succeeds and release it without dereferencing it.
Repeated labels remain separate tests, with literal marker order and assertion
counts. It does not specify or change ordinary duplicate-import behavior.

Each unflagged compilation must create a fresh image without executing tests or
linking observer hooks. The runner then executes that image exactly once at each
requested optimization level. Compiler/source/image identities, commands,
outputs and exact required case counts are retained in a fresh terminal receipt.
Missing sources, changed identities, absent images, unexpected output, timeout or
missing cases fail. `--prepare` only writes sources and a nonterminal prepared
inventory; it never qualifies execution. The observation wrapper permanently
requires this family in addition to its observed-source tests.
