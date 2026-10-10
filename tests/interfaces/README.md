# Interface qualification: current I1a checkpoint

I1a qualifies bounded declaration, implementation-binding, table-publication and
native-call contracts. It does **not** establish complete source-flow safety,
object lifetime, memory safety, runtime CFI, or kernel readiness. The maintained
I0 graph tests now use the current I1a oracles described below.

## Current supported contract

- Canonical source contracts retain ordered parameter/result identity, scalar
  signedness and width, nominal enum/interface/pointee identity, every pointer
  permission layer, outer nullability, storage domains and relevant effects.
  Aliases expand without mutating shared AST metadata.
- Supported interface slots are fixed native instance signatures using ordinary
  scalars, plain enums, data pointers and exact nominal interface descriptors.
  Unsupported aggregates, callbacks/closures, slices, C ABI, variadic, coroutine,
  generic/polymorphic and effect contracts diagnose before invalid declarations.
  Unrelated valid ordinary generic/callback functions retain their behavior.
- Preflight checks every distinct method origin. Equal distinct origins are
  accepted; conflicting ones reject even before a table is consumed. Candidate
  selection precedes signature comparison; an invalid higher-priority method
  cannot fall back to a compatible lower-priority one. Declared transitive
  relations are supported, including inherited qualified implementations.
- Native functions are keyed by original declaration/owner identity. Binding
  checks formal LLVM types, receiver, convention and admitted parameter/result
  attributes. Tables publish all slots atomically or do not publish at all.
  Imported internal methods may support provider-owned tables; a new local table
  requiring an unavailable internal implementation rejects.
- Existing ordinary readable-identifier value boxing requires a ready declared
  relation. Dynamic/qualified calls check arity, receiver and emitted-value
  representation against the formal ABI. Pointer-to-interface descriptor
  receivers reject before the legacy descriptor load. These checks do not
  replace complete source-actual typing.
- I0 ordering is retained: source-order parents first; the same originating
  declaration occurs once across a diamond. Graph readiness alone is still not
  signature readiness. Reachability visits each interface identity at most once.
- Limits remain 8 direct parents/implemented interfaces, 32 own methods, 32 unique
  flattened names, 64 interfaces on an inheritance path, 256 metadata/alias
  layers and 8 pointer layers. Empty unused declarations are valid; constructing
  an empty interface table remains unsupported.
- Owned source diagnostics, bounded parser recovery and repeated compilation
  contexts are tested. Unsupported interface parameter flags must exit normally
  rather than leave parser recovery unable to advance.

## Concrete-origin eligibility restriction

The current boxing/qualified-call seam retains known generic/dependent initial
origins from raw types, aliases, parameters, constructors, declared call/lambda/
callback results and identifier copies. Typed and inferred local initializers
both retain those exclusions. Address/dereference forms preserve operand origin.
An ordinary destination annotation cannot erase a retained generic result.

Literal ordinary names such as `S_G_i32` remain valid; no name-pattern ban is
used. Ordinary values, ordinary parameters and existing interface descriptors
inside an unrelated generic function remain admissible. A generic factory with
an explicitly ordinary declared result is also admissible.

Field/index-derived concrete initialization and other unresolved expression
origins conservatively reject when consumed at these interface seams, including
explicitly typed local copies. Use a directly declared ordinary value or a call
with a declared ordinary result. Diagnostics provide that guidance. A directly
known ordinary call result remains supported even when its function body reads
a field. This interim restriction is prioritized for I1b refinement.

The retained exclusion reason is **not** proof that arbitrary later assignments
are safe. General call/store/cast/return/merge/projection/container flows, generic
specialization lifecycle, initialization/default/null/fabricated descriptors,
exact-descriptor transfer validity, object/table pairing, ownership, borrowing,
lifetime, nullable flow and concurrency remain I1b/later work. Contextual compound
initialization does not establish field/container member-transfer safety.

## Archive compatibility and history

The current semantic ABI marker is **7**. Rebuild source-backed archives with the
current compiler. Descriptor bytes remain `{ptr, ptr}`; the marker records the
stronger declaration/binding guarantees, not a new physical descriptor layout.
The genuine ABI6 provider fixture is rejected before body import, codegen or
link. Its original source independently fails the new declared-relation check.
Never relabel an old/current sidecar to simulate this boundary.

Historical I0 used ABI6 and validated retained metadata/declaration graphs. At
that stage distinct origins remained pending and table consumption rejected even
equal signatures. Those historical oracles are superseded by current equal-origin
positives and eager conflict negatives. Immutable I0 fixtures/receipts remain in
the checkpoint archive; their acceptance is not a current safety claim.

A separate genuine ABI5-to-current graph archive control preserves B1 history.
The pinned B1 provider build returns 1 after creating its archive because of two
known registry cleanup leaks (25,344 and 256 bytes). Its receipt records that
provider cleanup as **FAIL**, with an exact degraded-fixture profile. It is not a
clean build. The ABI5 consumer is compile-only and must reject; the rebuilt ABI7
consumer performs real inherited dispatch.

## Reproduction

From the repository root with the preserved versioned binaries and historical
archive fixture available:

```sh
source ../env.sh
python3 tests/interfaces/run_i1a.py \
  --compiler bin/stasha-interface-i1a-r9 --artifacts /tmp/i1a-current
python3 tests/interfaces/test_i1a_archives.py \
  --compiler bin/stasha-interface-i1a-r9 \
  --historical-archive-dir ../hardening-checkpoint/i1-archive-baseline \
  --artifacts /tmp/i1a-archives
python3 tests/interfaces/test_i1a_units.py \
  --llvm-build build/llvm --artifacts /tmp/i1a-units
python3 tests/interfaces/run_i0.py \
  --compiler bin/stasha-interface-i1a-r9 --artifacts /tmp/i0-current
python3 tests/interfaces/test_i0_runner.py
python3 tests/interfaces/test_i0_units.py \
  --llvm-build build/llvm --artifacts /tmp/i0-units
python3 tests/interfaces/test_i0_archives.py \
  --compiler bin/stasha-interface-i1a-r9 \
  --b1-compiler bin/stasha-make-b1-r5 --artifacts /tmp/i0-archives
```

`i1a_inventory.json` enumerates **274 mandatory controls**, including 37
compile-only initial-origin controls and valid native dispatch at O0–O3. The
separate I1a archive gate has 9 controls, including genuine old-archive rejection,
rebuilt provider/consumer calls with 16 retained indirect calls per optimization
level, provider-owned internal tables and unavailable local-table rejection.
The maintained I0 inventory has 82 controls; supplying its optional B1 compiler
adds the strict historical archive family as an 83rd top-level check.

Missing, extra, duplicate or empty inventory entries fail. `--write-inventory`
is an explicit maintenance operation, never a CI option. The I1a runner records
the absolute compiler path and asserts identical before/after hashes. Never
replace a tested versioned binary; a production change needs a new freeze.

The historical B1 publication is `e8a1d04fe3cf64aed321139571c461c5977163c9`, tree
`d768d467a665eae676923d451654e70e915702f0`; the archive runner requires compiler
SHA-256 `c1fb7d98ae03a1d4b783f7b8c0a03b6df22346ac471f46f7190947049ab832dc`.
The ABI6 fixture archive requires SHA-256
`ac803ecaf607010f5f162b6a43d4f6a81ffaf9428b948597ba21d95eb9b0a77c`.
Missing genuine history is an unrun gate, not permission to fabricate a marker.

## Oracles and remaining qualification

Invalid fixtures are never executed, even if unexpectedly accepted. Rejection
requires bounded normal exit 1, expected error text and source coordinates, no
artifact, and no internal/verifier/sanitizer/warning output. Initial-origin
negatives also require actionable guidance. Valid native fixtures require exact
zero exit and empty runtime output. Generated sources, commands and receipts
are retained in unique artifact directories.

The compiler uses pinned Clang/LLVM 23. Isolated unit probes use host GCC/G++
with ASan+UBSan and the pinned LLVM libraries because this Clang build lacks
sanitizer runtimes. LeakSanitizer is unavailable under the executor's ptrace
configuration; no leak-free claim follows. Sanitizer failures are not accepted
diagnostics or silently retried passes. Graph tests separately invoke codegen
four times in one process; CLI repetition is not substituted for that test.

Explicit root-import controls do not claim transitive import resolution is
repaired. The full hosted gate, rebuilt standard library, independent review
and publication remain separate coordinator-owned qualification steps.
