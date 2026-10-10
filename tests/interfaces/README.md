# Interface qualification: I1b.1 source-call contracts

I1b.1 adds bounded source-actual admission to the retained I1a declaration,
implementation-binding, table-publication and native-call contracts. It does
**not** establish complete source-flow safety,
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
  receivers reject before the legacy descriptor load. I1b.1 now checks the
  admitted source-actual contracts below before lowering actual expressions.
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

## I1b.1 source-actual admission

- Dynamic interface slots and concrete qualified-interface calls compare raw
  source contracts before side effects or native coercion. Primitive conversions
  reuse the established scalar policy only after source admission. Enum values
  require exact nominal identity; native integer layout is never identity proof.
- Pointer checks retain nominal pointee, depth, invariant inner permission
  layers, outer permission/arithmetic attenuation, nonnull-to-nullable widening
  and the required outer pointee domain. An ordinary pointer value cannot gain
  arithmetic from its variable slot. Fresh object/field/fixed-array addresses
  use the addressed storage's access and domain. `&*p` preserves the existing
  pointer capability. Nil requires a nullable, default-domain pointer formal.
- Plain readable exact-interface descriptor copies may cross default/stack
  by-value formal storage. The same rule permits exact enum/descriptor
  conditional arms with different value-storage domains. Pointer joins do not
  discard domain/capability differences. Each evaluated operand must be readable;
  forming an address is distinct from reading the addressed value.
- Direct interface-valued native formals are checked on ordinary calls, module
  and colon calls, instance/static/self/sibling/member methods, constructors,
  declared function pointers and independently declared interfaces in generic
  functions. Selected callable declaration/native identity must agree; a
  generated-name collision cannot stand in for original declaration evidence.
- Direct/aliased interface descriptors and structural callback/slice carriers on
  unsupported C, variadic, closure, literal-lambda, thread, async or error routes
  reject. Unknown/dependent interface-bearing signatures cannot escape through
  a native interface representation. Named aggregate fields containing interface
  descriptors remain explicitly unqualified transfer work, including special
  carriers; there is no blanket recursive aggregate ban.
- Facts are owned per compilation and retain raw declarations across symbol-table
  growth and shadowing. Pure queries do not emit code, mark symbols used,
  instantiate callees or emit diagnostics for unrelated unsupported types.
  Preparation/selection and emission are separate; selected native metadata is
  copied before operations that may grow the symbol table. Valid actuals execute
  once in original order.

Source expressions include declared locals/parameters/globals, plain enum
members, raw ordinary fields, fixed-array/slice elements, readable dereferences,
fresh addresses, supported primitive operations/casts, same-type conditionals,
declared call results and narrowly checked plain-identifier assignments. An
unsupported expression at a checked boundary diagnoses rather than borrowing the
expected type as source evidence. Complex assignment, pointer joins with
conflicting evidence and inner-domain alias composition remain unsupported.

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
a field. This interim restriction remains for later I1b refinement; argument checking
does not introduce new boxing/projection syntax.

The retained exclusion reason is **not** proof that arbitrary earlier writes or
return bodies are safe. Ordinary declared parameters and results remain trust
boundaries. General store/return/merge/projection/container flows, generic cache
lifecycle, initialization/default/null/fabricated descriptors, object/table
pairing, ownership, borrowing, lifetime, nullable flow and concurrency remain
later work. By-value descriptor admission certifies the source declaration at
this call, not the descriptor's backing object or complete transfer history.
Contextual compound initialization does not establish field/container
member-transfer safety. Seven production compile-only gap characterizations
record these exclusions and never execute accepted deferred programs.

## Archive compatibility and history

The current semantic ABI marker is **8**. Rebuild source-backed archives with the
current compiler. Descriptor bytes remain `{ptr, ptr}`; the marker records the
stronger source-call guarantees, not a new physical descriptor layout.
The genuine ABI6 provider fixture is rejected before body import, codegen or
link. Its original source independently fails the new declared-relation check.
A genuine ABI7 provider compiled with frozen I1a r9 retains a wrong-enum call
body that the current source rejects. An unchanged-marker intermediate compiler
still compiled/linked that old body; neither invalid program was executed. This
evidence established the mandatory ABI8 rebuild boundary. Current consumers
reject old ABI7 metadata before body import/codegen/link; corrected ABI8 native
provider/consumer calls execute at O0–O3. Never relabel a sidecar to simulate
this boundary.

Historical I0 used ABI6 and validated retained metadata/declaration graphs. At
that stage distinct origins remained pending and table consumption rejected even
equal signatures. Those historical oracles are superseded by current equal-origin
positives and eager conflict negatives. Immutable I0 fixtures/receipts remain in
the checkpoint archive; their acceptance is not a current safety claim.

A separate genuine ABI5-to-current graph archive control preserves B1 history.
The pinned B1 provider build returns 1 after creating its archive because of two
known registry cleanup leaks (25,344 and 256 bytes). Its receipt records that
provider cleanup as **FAIL**, with an exact degraded-fixture profile. It is not a
clean build. The ABI5 consumer is compile-only and must reject; the rebuilt ABI8
consumer performs real inherited dispatch.

## Reproduction

From the repository root with the preserved versioned binaries and historical
archive fixture available:

```sh
source ../env.sh
python3 tests/interfaces/run_i1b.py \
  --compiler bin/stasha-interface-i1b1-dev7 --artifacts /tmp/i1b-current
python3 tests/interfaces/test_i1b_archives.py \
  --compiler bin/stasha-interface-i1b1-dev7 \
  --historical-archive-dir ../hardening-checkpoint/i1b1/genuine-abi7 \
  --artifacts /tmp/i1b-archives
python3 tests/interfaces/test_i1b_runner.py
python3 tests/interfaces/test_i1b_units.py \
  --llvm-build build/llvm --artifacts /tmp/i1b-units
python3 tests/interfaces/run_i1a.py \
  --compiler bin/stasha-interface-i1b1-dev7 --artifacts /tmp/i1a-current
python3 tests/interfaces/test_i1a_archives.py \
  --compiler bin/stasha-interface-i1b1-dev7 \
  --historical-archive-dir ../hardening-checkpoint/i1-archive-baseline \
  --artifacts /tmp/i1a-archives
python3 tests/interfaces/test_i1a_units.py \
  --llvm-build build/llvm --artifacts /tmp/i1a-units
python3 tests/interfaces/run_i0.py \
  --compiler bin/stasha-interface-i1b1-dev7 --artifacts /tmp/i0-current
python3 tests/interfaces/test_i0_runner.py
python3 tests/interfaces/test_i0_units.py \
  --llvm-build build/llvm --artifacts /tmp/i0-units
python3 tests/interfaces/test_i0_archives.py \
  --compiler bin/stasha-interface-i1b1-dev7 \
  --b1-compiler bin/stasha-make-b1-r5 --artifacts /tmp/i0-archives
```

`i1b_inventory.json` enumerates **142 programs** at O0–O3, yielding **568
mandatory checks**: 79 valid runtime programs and 63 compile-only negatives at
each optimization level. It also enumerates seven compile-only deferred gaps.
The separate I1b archive gate has eight controls; the source-fact sanitizer probe
checks pure queries, raw identity, shape/access and stable records through growth
in four compilation contexts. Paired ordinary callback/error/slice field calls
with and without an unused interface protect unrelated-call compatibility.

`i1a_inventory.json` retains **274 mandatory controls**, including 37
compile-only initial-origin controls and valid native dispatch at O0–O3. The
separate I1a archive gate has 9 controls, including genuine old-archive rejection,
rebuilt provider/consumer calls with 16 retained indirect calls per optimization
level, provider-owned internal tables and unavailable local-table rejection.
The maintained I0 inventory has 82 controls; supplying its optional B1 compiler
adds the strict historical archive family as an 83rd top-level check.

Missing, extra, duplicate or empty inventory entries fail. `--write-inventory`
is an explicit maintenance operation, never a CI option. The I1a and I1b runners record
the absolute compiler path and assert identical before/after hashes. Never
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
