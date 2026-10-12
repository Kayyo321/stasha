# Native direct-source and primitive-slice observations (v3, developmental)

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
  successful explicit `new`/`mov` allocations and admitted generated primitive
  slice backing allocations at the end of the entire test suite. There are two
  separate domain totals, whose checked sum is the thresholded total.
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
break/continue and defer; explicit `new`, two-argument `mov`, `rem`; the bounded
local explicit-count primitive slices described below; primitive
test assertions and literal-message failure; compiler-generated primitive print.

The source walker rejects all other node families by default, even in uncalled
functions and after a structural terminator. Explicitly rejected families include
all user-defined composite types/interfaces/generics, function values/indirect
calls, lambdas/closures/variadics, general slices and other implicit heap constructors/formatting,
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

The v3 profile is `native-sequential-direct-source-slice-v3`: observer ABI 3,
raw report/driver JSON/manifest schema 3, allocation metric 2 and line metric 2.
It uses `__stasha_obs_init_v3`, with no v2 alias; mixed image/runtime/report
versions are rejected. Input graph tag/schema, byte encoding, old graph-profile
identity and known-answer vector remain v2. Qualification-family receipt schema
also remains 2: it is an evidence structure, independent of observer wire schema.
The admission policy is
`closed-direct-source-array-and-local-primitive-slices-v1` and resource policy is
`direct-source-slice-bounds-v3`. File, declaration, anchor, test, operation and
denominator metadata remain present with coverage collection disabled. Per-file
exclusion bitmasks preserve mixed test/production ownership on a physical line.

Bounds are fail-closed: 256 files, 255 imports, 16 MiB aggregate source, 1,000,000
physical source lines, 127-byte identifier components, 255-byte module spellings,
4096-byte test labels, 4096 tests, 65,536 declarations/anchors/eligible lines/sites,
16,384 live allocation records, 2 MiB raw reports, and separate 32 MiB canonical
manifest and driver-JSON limits. Slice bindings, slice references and brace-block
identities each have a 65,536-record limit; lexical depth is bounded at 1,024.
Physical line count is one plus the LF count.
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
requalified under v3, not regenerated from new compiler output.

The existing hosted entry runs `test_observation.py`, which now requires all
three pure component tests (runtime, driver, source graph), Python-only enrollment
selftests, the ordinary imported-test family, the observed-source family, and
the bounded observed-array-query family, and the bounded observed-primitive-slice
family. All four children receive the exact selected compiler and optimization set. Missing,
malformed, nonterminal, changed-identity, skipped, timed-out or failing child
receipts prevent wrapper success. Runner sources and fixed fixtures belong to
the frozen input inventory. All three observed families require the exact selected runtime archive identity
and v3 profile. All children retain terminal artifact manifests; altered artifacts,
duplicate cases/runs, substituted receipts and changed sources fail enrollment.
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

The preceding array checkpoint used policy
`closed-direct-source-fixed-array-queries-v3` within observer v2. Its exact source,
semantic and coverage inventories remain unchanged under observer v3, allocation
metric 2 and line metric 2. Only the protocol/recomputation boundary advances.
The new configuration known answer is 526 bytes, SHA-256
`1dc9f3d3e2be65e986cf997e1350dea939ad3517d392f67a3d9220f98d3ed457`
for compiler digest `dd` repeated 32 times, runtime `aa` repeated 32 times,
`x86_64-unknown-linux-gnu`, O2, debug off, allocation/coverage enabled, warning on,
maximum 8 and minimum 90. The graph-only vector remains unchanged.

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
non-emitted statements. Raw-pointer, scalar, fully indexed, borrowed-view, member,
alias, pointer-element, array-parameter, cast/address/deref/ternary roots and
unsafe/unchecked forms remain excluded. Queries remain non-static for global
initializers and compile-time assertions. This does not add general unreachable
extent validation, general ownership checks or other targets. The new live local
slice proof below is separate from this fixed-array storage/rank proof.

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

## Bounded local primitive-slice backing increment

Only direct `heap []T name = make.([]T, length[, capacity]);` declarations in
real brace-delimited ordinary bodies are admitted. T is exactly builtin
`i8/u8/i16/u16/i32/u32/i64/u64/bool/f32/f64`, with semantic element identity
preserved. Counts retain their full ordinary integer width and signedness,
including bool; explicit length then capacity evaluate once before checks.
Omitted capacity reuses the length value. Zero capacity creates an empty
null-backed descriptor without an allocation event/generation. Positive capacity
requests exactly target ABI stride times capacity and zeroes that entire backing.
Assertions read only indices below length; spare-capacity zeroing is an IR claim.

Each declaration has exactly one closed source schedule:

- Retained, with no rem or deferred references
- One later direct rem in the same block, with no later references or deferred
  references; a return/break/continue bypass intentionally retains the backing
- Immediately adjacent `defer { rem.(name); }`, containing only that direct rem;
  cleanup follows ordinary fallthrough, return, break and continue edges

Direct live len/cap, checked scalar element reads and simple indexed writes may
use that immutable binding identity. Scalar count expressions may use admitted
calls and other still-live slices. Nested/sibling same-name declarations keep
separate identities, including outer-name lookup while evaluating an inner
initializer. Descriptors cannot be copied, reassigned, passed, returned, captured,
addressed or otherwise escape. Element addresses, views, append/copy, compound or
postfix element writes, aliases/composite elements, constructors in deferred or
synthetic/fat-arrow contexts, and general ownership/lifetime analysis remain open.
The inherited borrowed-view array rejection remains rejected.

Domain 1 is `explicit_user_heap`; domain 2 is
`generated_primitive_slice_backing`. Descriptor storage and allocator slack are
excluded. Slice make/rem sites are kinds 4/5, separate from explicit new/mov/rem
kinds 1/2/3. Every site carries element/stride/count-arity metadata; only make has
nonzero values. Every retained record includes generation/domain/requested bytes/
allocation site/resize site/dynamic test. Domain 2 cannot resize. Static source
ownership and originating dynamic test identity are independently checked.
Parsed-only `allocation_domains` totals are recomputed from all records, even in
coverage-only mode. Unsupported allocation providers are never reported as zero.

`test_primitive_slices.py` is the mandatory fourth family. The independently
reviewed literal inventory is five fixed source files and 43 generated native
sources, forming 47 cases. At O0–O3 it requires 188 ordinary executions and 800
observed executions: warn/max-equal/coverage-only/combined for every case, ten
max-below cases, and coverage-66/67 for the central fixture. Test-only production
coverage remains N/A and fails even at an explicit minimum zero. The central
fixture is exactly 4/6 with `0f`; the imported fixture retains three slice records
of 8,12,12 bytes with test IDs 0,0,1. Mixed domains retain independent 7+20=27
bytes. Every source/site/line/record/threshold oracle is frozen before compilation.

The separate no-launch inventory has exactly 177 rejection sources and 22
count-value IR controls. None can enter native dispatch, even after erroneous
admission; missing nonlaunch instrumentation is a blocker, never a normal-runner
fallback. Count failures establish reason-6 edges before allocation through IR,
not native panic runs. No enormous valid allocation or native-invalid-memory
source is added. The largest approved positive native request is 256 bytes.

```
python3 tests/observation/test_primitive_slices.py --selftest-only --artifacts build/observation/slice-mocks
python3 tests/observation/test_primitive_slices.py --stage prepare --artifacts build/observation/slice-prepared
python3 tests/observation/test_primitive_slices.py --stage compile-only --compiler bin/stasha \
  --ir-compiler /path/to/reviewed-fork-disabled-stasha --opt 0,1,2,3 --artifacts build/observation/slice-nonlaunch
python3 tests/observation/test_primitive_slices.py --stage native --compiler bin/stasha \
  --opt 0,1,2,3 --artifacts build/observation/slice-native
```

Independent source/ordinary and observed pre/post-O0–O3 IR, pure components,
separately accepted small real native positives, and the complete four-family
unchanged-candidate aggregate are distinct required gates. A prepared receipt,
source review or pure spy does not establish native qualification. Endpoint
records prove retained backing; balanced totals alone do not prove every callback
or cleanup edge, so the separate literal dynamic event/CFG oracle remains required.

Genuine native allocator OOM is **NOT_RUN / outside initial native qualification**.
Reason-8 null dominance over zeroing/descriptor stores is source/IR evidence;
callback-NULL behavior is pure simulated failure evidence. Neither is native OOM
execution. Other slices, transfers, growth, providers, targets, generated allocation
families and broad lifetime analysis stay unsupported/open. This bounded increment
never means full language support, default checker activation or a default coverage
minimum, and authorizes no restricted descriptor/archive or LeakSanitizer work.
