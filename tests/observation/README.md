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
selftests, the ordinary imported-test family, the observed-source family, and
the bounded observed-array-query family. All three children receive the exact selected compiler and optimization set. Missing,
malformed, nonterminal, changed-identity, skipped, timed-out or failing child
receipts prevent wrapper success. Runner sources and fixed fixtures belong to
the frozen input inventory. Both observed families require the exact selected runtime archive identity.
There is no default skip for any family.

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

## Bounded fixed-primitive-array query increment

The admission policy is `closed-direct-source-fixed-array-queries-v3`, replacing
`closed-direct-source-scalar-v2`. This is a configuration-identity revision inside
observer wire/profile v2, not ABI 3. Allocation metric 1 and line metric 2 stay
unchanged. The independent canonical configuration vector changes from
`bac9c0710c372aa6ccf949ce3def394ca04953638c22b915b0772279c4873e53`
(472 bytes) to
`b3ceb27bd1814a1f71e560649fd6354ec718310490ae23a4d9b072479d2d30bf`
(485 bytes). The vector uses compiler digest `dd` repeated 32 times, runtime
`aa` repeated 32 times, target `x86_64-unknown-linux-gnu`, O2, debug off,
allocation/coverage enabled, warning on, maximum 8 and minimum 90. Graph/map
vectors whose explicit inputs do not change retain their old answers.

Observed `len.(array)` and `cap.(array)` admit only fixed primitive,
nonpointer-element array identifiers or ordinary indexed subarrays with positive
remaining rank: `0 <= indexes < root rank <= 8`. Direct, named, inferred, zero,
const/final and global/local arrays keep ordinary extent resolution. Each query
returns the next dimension as exact nonnegative `i32`, not flattened elements
or bytes. Both values are equal for these arrays. An extent over `INT32_MAX`
fails rather than narrowing. Querying an empty array does not load an element.
Every containing index is evaluated and checked exactly once through the
ordinary address path; helper calls retain source identity, allocation hooks and
coverage. An attempted address lowering never retries through a pointer fallback.

Queries do not create a new coverage anchor family. They retain statement,
control, short-circuit and conditional-arm anchors, deduplicated by file and
physical line. Multiline operand punctuation creates no extra eligible lines.
The complete operand inventory runs even inside uncalled or structurally
non-emitted statements. Raw-pointer, scalar, fully indexed, slice, member,
alias, pointer-element, array-parameter, cast/address/deref/ternary roots and
unsafe/unchecked forms remain excluded. Queries remain non-static for global
initializers and compile-time assertions. This does not add general unreachable
extent validation, heap-slice measurement, ownership checks or other targets.

`test_array_queries.py` is the mandatory third family, `observed-array-queries`.
It selects exactly these ordinary `RuntimeCase` names: `direct`, `named_extent`,
`inferred_extent`, `empty_extent`, `empty_inferred`, `const_array`, `final_array`,
`matrix`, `deep_rank`, `global_array`. Original generated bytes, including their
crash-disable header and all checks, are unchanged. The observed adapter requires
exactly one `@[[crash: disable]];` header line at byte zero and replaces only its
content with a blank first line, because observation rejects that root header.
Its LF and every downstream physical source line are preserved. It also requires
exactly one `fn main(void): i32`, renames that signature to
`fn array_query_case(void): i32`, then appends one test asserting its result is
zero. All remaining body bytes and checks stay unchanged. It never calls the
replaced application main or broadens header admission. Missing, duplicate,
misplaced or differently spelled headers fail the adapter's pure checks. Original/adapted SHA-256
identities and the exact transformation are frozen before compiler qualification.
The five reused ordinary generator dependencies are mandatory read-only inputs.

The six literal fixtures and ten adapters have independent source-derived line,
site, assertion and allocation oracles. The central fixture remains exactly
8/10 with bitmap `ff00`, five passing assertions, one completed test and one
sentinel. Its two successful 8-byte helper allocations are balanced; the only
sites are production `index` new at line 4 and rem at line 6. Minimum 80 passes;
81 fails only the driver's threshold, with child exit zero. Coverage-only,
allocation-only and combined modes preserve full source/site metadata. The
disabled allocation-only hit bitmap is `0000` and is not a coverage oracle.

Additional literal oracles are control anchors 15/18 (`ffb903`, two assertions),
retained helper 7/7 (`7f`, three assertions, 13 live bytes/one allocation, maximum
13 passes/12 fails), later-test cleanup 7/7 (three assertions, two tests, final
zero), and root/leaf queries 13/13 (`ff1f`, fourteen assertions, two tests and
two balanced 8-byte allocations). Released allocation counts are established
by source/IR exactly-once proof plus successful-allocation assertions and final
balance; the endpoint report itself lists only allocations still live.

```
python3 tests/observation/test_array_queries.py --selftest-only --artifacts build/observation/array-mocks
python3 tests/observation/test_array_queries.py --stage prepare --artifacts build/observation/array-prepared
python3 tests/observation/test_array_queries.py --stage compile-only --compiler bin/stasha \
  --ir-compiler /path/to/reviewed-fork-disabled-stasha --opt 0,1,2,3 --artifacts build/observation/array-nonlaunch
python3 tests/observation/test_array_queries.py --stage native --compiler bin/stasha \
  --opt 0,1,2,3 --artifacts build/observation/array-native
```

Preparation is explicitly nonterminal, and neither it nor a pure test can satisfy
native family enrollment. All new negative sources are separate nonlaunch-only
controls, including huge arrays and unsupported nested operands. The hosted
family records them as `NOT_RUN`, never passed. Independent exact source and
pre/post-optimization IR review must precede native execution. At O0–O3 the
native family requires 40 original ordinary runs and 76 observed runs (40
adapters and 36 fixed-mode cases), with exact child/report/runtime identities.
A successful final unchanged-candidate aggregate is required before claiming
qualification. Existing leaf 9/10 and 8/10, direct-import 7/8 and all rejection
partitions/oracles are retained. This increment alone does not qualify general
array queries or full language support.
