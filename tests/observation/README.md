# Native leaf-test observations (developmental checkpoint)

This partial profile remains opt-in until complete language support is qualified;
coverage minimums are always explicit choices. See the
[full-language support roadmap](../../wiki/Leak-Coverage-Support.md).

```
stasha test module.sts --warn-memleak
stasha test module.sts --max-leaked-bytes 0
stasha test module.sts --min-test-coverage 90
stasha test module.sts --warn-memleak --max-leaked-bytes 0 --min-test-coverage 90
```

An observation-enabled command compiles a fresh native image and runs it exactly
once. Ordinary unflagged single-file `test` retains the existing compile-only
behavior; the old external language harness must still run that image itself.
Observation flags are rejected for projects, imports/libraries/providers,
non-test commands, stdin overrides, extra link libraries and non-native targets.
This checkpoint supports hosted Linux x86-64 LP64 only. Workspace installation
includes `observation_runtime.a` beside the compiler. External release workflow
packaging and other platforms remain deferred.

## What the measurements mean

* `tracked_outstanding_bytes` is the checked sum of requested sizes of live
  successful explicit `new`/`mov` allocations at the end of the entire test suite.
  Production and test-body allocations are equally tracked. A later test may
  release a buffer allocated by an earlier test. No per-test subtraction occurs.
* Allocation count is separate. A nonnull zero-byte allocation may warn while
  satisfying `--max-leaked-bytes 0`. A failed nonzero resize preserves the old
  record and size. All reached zero-sized resize operations fail incomplete
  before calling realloc. Unknown/non-base free/resize also fails before calling
  the allocator. Generations do not prove pointer freshness or ownership.
* This is allocation balance, not reachability or whole-process leak detection.
  Reachable buffers count until released. Stack/static storage, libc/runtime
  private memory, allocator slack and observer bookkeeping are excluded.
* Coverage is **executable statement/control-line coverage**. Eligible lines are
  source statement starts and independently evaluated control/short-circuit/
  conditional-arm starts. Multiple anchors on a line count once. An untaken
  multiline conditional arm is not covered by evaluating its parent statement.
  Deferred bodies count at cleanup, not registration. For-init/condition/update,
  while/do-while conditions, match guards and switch discriminants are included.
* All ordinary production functions, including never-called functions, contribute
  eligible lines before optimization. Tests, replaced root main, non-executable
  text/static initialization, inactive target branches and statements after a
  structural terminator are excluded with recorded reason codes. Discarded `_`
  declarations emit no runtime work and are excluded. Unsupported eligible
  syntax fails compilation rather than shrinking the denominator.
* Maximum bytes and minimum coverage are inclusive. Integer arithmetic decides
  thresholds, independently of display rounding. A requested coverage minimum
  fails for zero eligible lines, including a minimum of zero. Zero tests fails;
  one normally returned test with zero assertions is allowed and reported.
* Assertion failure, abnormal child termination, absent/incomplete/invalid report,
  compilation/link/launch failure or a failed threshold always makes the CLI
  fail. Warning-only outstanding allocations do not turn success into failure.

## Admitted source profile

Single-file, sequential, directly named non-generic source functions with
primitive scalar/pointer parameters and results; ordinary scalar/pointer locals
and static globals; fixed primitive nonpointer-element arrays; literal strings;
primitive arithmetic/casts and array/pointer operations; lexical blocks;
if/else-if, for/while/do-while/inf, integer match/switch with guards, return,
break/continue and defer; explicit `new`, two-argument `mov`, `rem`; primitive
test assertions and literal-message failure; compiler-generated primitive print.

The source walker rejects all other node families by default, even in uncalled
functions and after a structural terminator. Explicitly rejected families include
all user-defined composite types/interfaces/generics, function values/indirect
calls, lambdas/closures/variadics, slices and implicit heap constructors/formatting,
zones, threads/futures/coroutines, source lifecycle blocks/signals, macros,
inline assembly, unsafe/unchecked forms and integer-to-pointer construction.
Native/runtime symbol calls and reserved compiler/native declaration names are
not admitted. Bindings must be conventional ASCII identifiers of at most 127
bytes. The private observed image gives admitted source functions/globals
internal linkage, preventing them from interposing observer or libc internals;
unflagged compilation and source AST linkage are unchanged. Only already-supported target conditional headers and local
compile-time block selection are admitted; chained compile-time else bodies, wildcard match guards that the ordinary
lowerer does not evaluate, and top-level compile-time declaration blocks fail
closed. Literal integer match-arm guards are supported.

Source identities and anchors are owned at parse time. A canonical source path,
content SHA-256, configuration identity, compiler identity and the complete
source manifest bind the runtime's dense IDs. The optimizer cannot remove
zero-hit denominator metadata because the generated harness explicitly registers
its fixed inventory, independent of otherwise removable function bodies.

The runtime writes an exclusive, bounded, versioned report in a fresh private
run directory. The driver validates identity, exact grammar, completed test
inventory, hit bitmap and allocation records, then independently recomputes both
metrics. It records the exact launched image hash and actual child status in
`driver.json`, and prints the private evidence directory. Evidence is retained;
there is no overwrite/export option. These identities prevent accidental stale
or mixed runs; they are not cryptographic attestation against a malicious test.

## Qualification

Before new native fixture execution, review the exact source diff and pre/post
optimization IR through a separately reviewed nonlaunching qualification image.
A source-only or IR-only result is not evidence that the runtime metrics passed.
The isolated fork-wrapped image used for this gate is distinctly hashed and may
fail native linker discovery; it does not establish successful linking/execution.
Namespace-collision controls in `COMPILE_ONLY_CONTROLS` are reserved for this
fork-disabled IR staging path and are never launched by the native suite.

After that gate, run:

```
python3 tests/observation/test_observation.py --compiler bin/stasha \
  --cc build/llvm/bin/clang --opt 0,1,2,3 --artifacts build/observation/native
```

Each invocation creates a new `run-*` evidence directory. `LATEST` identifies
that directory; only its own `result.json` can establish that run's success.
The script retains source/command/status/stdout/stderr, pre/post IR, the raw
runtime report and independently validated driver metadata. Compiler, runtime,
source and executed binary identities are retained. Literal expected source line
inventories are in the script; they must not be regenerated from compiler output.

`runtime_ledger_tests.c` uses synthetic integer allocation IDs and callback spies.
Unknown/interior/double-release bookkeeping, overflow, exhaustion, address reuse,
resize failure/move/in-place outcomes and resize-zero rejection do not execute
invalid native allocator operations. `test_driver.c` supplies pure schema,
threshold and status controls. Their results are separate from native fixtures.
The native suite uses ordinary defined-behavior programs only, and checks exact
9/10 versus 8/10 coverage, threshold equality, test-only leaks, zero assertions,
zero denominators, suite-wide cleanup, every control anchor family, optimizer
invariance, explicit unsupported-family rejections and exactly-once sentinels.

The existing language harness and descriptor/default semantic oracles are not
modified by this suite. Its hosted enrollment is an additional bounded gate,
not a claim that every Stasha program or runtime family has been qualified.
