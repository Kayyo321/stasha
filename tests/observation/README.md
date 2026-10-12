# Native direct-source test observations (v2, developmental)

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
Observation flags are rejected for projects, libraries/providers,
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
  conditional-arm starts. Multiple anchors on a line count once per file; identical line numbers in
  different files remain distinct. Global coverage counts unique `(file_id,line)`
  pairs rather than averaging file percentages. An untaken
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

One explicit root file, optionally importing distinct ordinary leaf source files,
with sequential, directly named non-generic source functions and
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

The root may use unconditional top-level plain `imp a.b;` declarations. Imported
files must declare the exact requested module and contain no imports. Repeated
modules, repeated canonical source files, self-imports and duplicate final-segment
aliases are rejected. Transitive imports, explicit aliases and imported file-wide
headers are outside the profile. This does not change ordinary import deduplication.
Root import aliases remain visible to sibling leaves, but every cross-module call
must select an unambiguous exported source function. Same-module private calls
remain valid. Root and imported tests run in root-declaration order, then import
order and declaration order; repeated labels remain separate tests. Imported
`main` functions remain production code; only root `main` is replaced.

Source identities and anchors are owned at parse time. Immutable source snapshots
retain original lookup and canonical paths, bytes, hashes and declaration owners.
Dependencies are selected beside the requested entry spelling, including a
symlinked entry, rather than silently beside its canonical target. All original
lookup mappings and contents are checked again before launch. A graph SHA-256,
configuration identity, compiler identity and complete source manifest bind the
runtime's dense IDs. The optimizer cannot remove
zero-hit denominator metadata because the generated harness explicitly registers
its fixed inventory, independent of otherwise removable function bodies.

The v2 profile is `native-sequential-direct-source-v2`: observer ABI 2, report and
manifest schema 2, allocation metric 1 and line metric 2. It uses the explicit
`__stasha_obs_init_v2` entry; v1/v2 mixing is rejected. Allocation semantics are
unchanged. File, declaration, anchor, test, operation and denominator metadata
remain present even when coverage collection is disabled. Per-file exclusion
bitmasks retain mixed test/production ownership on a physical line.

Bounds are fail-closed: 256 files, 255 imports, 16 MiB aggregate source, 1,000,000
physical source lines, 127-byte identifier components, 255-byte module spellings,
4096-byte test labels, 4096 tests, 65,536 declarations/anchors/eligible lines/sites,
16,384 live allocation records, 2 MiB raw reports, and separate 32 MiB canonical
manifest and driver-JSON limits. Physical line count is one plus the LF count.
Combinations of individual maxima may exceed a total bound and are rejected.

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
The native positive suite uses ordinary defined-behavior programs only, and checks exact
9/10 versus 8/10 coverage, threshold equality, test-only leaks, zero assertions,
zero denominators, suite-wide cleanup, every control anchor family, optimizer
invariance and exactly-once sentinels. Preserved leaf line/allocation oracles are
requalified under v2, not regenerated from new compiler output.

The existing hosted entry runs `test_observation.py`, which now requires all
three pure component tests (runtime, driver, source graph), Python-only enrollment
selftests, the ordinary imported-test family, and the observed-source family.
Both children receive the exact selected compiler and optimization set. Missing,
malformed, nonterminal, changed-identity, skipped, timed-out or failing child
receipts prevent wrapper success. Runner sources and fixed fixtures belong to
the frozen input inventory. There is no default skip for either family.

`test_source_imports.py` keeps preparation, nonlaunching controls, reviewed benign
rejection-expected cases, and positive native execution distinct:

```
python3 tests/observation/test_source_imports.py --selftest-only --artifacts build/observation/mocks
python3 tests/observation/test_source_imports.py --stage prepare --artifacts build/observation/prepared
python3 tests/observation/test_source_imports.py --stage compile-only --compiler bin/stasha \
  --ir-compiler /path/to/reviewed-fork-disabled-stasha --opt 0,1,2,3 --artifacts build/observation/nonlaunch
python3 tests/observation/test_source_imports.py --stage native --compiler bin/stasha \
  --opt 0,1,2,3 --artifacts build/observation/source-native
```

The nonlaunch compiler is a separately identified local qualification instrument
linked from the selected compiler's exact objects with a reviewed fork-disabling
shim. This runner does not build it, install it, or require it for the hosted
benign/native subset. Its complete inventory preserves every old rejection
expectation, namespace control and raw-native-call control, plus the new source
rejections and ordinary/observed positive pre/post-IR files. Inspect those files independently
before authorizing native positives. Nonlaunch success alone is not native or
independent-IR-review success.

The permanent rejection-expected subset contains only individually reviewed
benign sources. They may execute defined-behavior marker or empty test bodies if
admission is broken; erroneous acceptance still fails qualification. They are
not described as compile-only. Namespace/interposition/layout/provider and other
unqualified controls remain in the separate nonlaunching gate. Hosted receipts
list that gate as `NOT_RUN`; its cases are never counted as passed there.

The fixed direct-source oracle has root hit/eligible 1/1 and imported 6/7,
global 7/8 (87.5%), raw bitmap `7f`, five passing assertions including successful
allocation, and a balanced 8-byte allocation. Minimum 87 passes and 88 fails.
A distinct imported-test fixture asserts a successful retained 13-byte allocation
and checks its imported test/source attribution despite zero production coverage.
Additional positives cover same-name and same-line identities, mixed test and
production owners on one line, sibling aliases, empty imported tests, imported
`main`, equal root/leaf module text, symlink lookup and O0–O3 stability.

The existing language harness and descriptor/default semantic oracles are not
modified by this suite. Its hosted enrollment is an additional bounded gate,
not a claim that every Stasha program or runtime family has been qualified.
