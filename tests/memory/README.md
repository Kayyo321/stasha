# Hosted memory-access contract regressions

These suites cover bounded hosted access contracts, including A1 explicit
array/slice checks and source-typed dereference width. They do not qualify
general memory safety: K0 still rejects slices/heap and requires explicit unsafe
raw-pointer access. The historical P3 review below predates the A1 contract.

## A1 current contract and runner

```sh
python3 tests/memory/run_bounds_a1.py --compiler ./bin/stasha \
  --opt 0,1,2,3 --artifacts /tmp/stasha-bounds-a1
```

Use a frozen compiler binary with its exact-source receipt for qualification.
`--case NAME` is repeatable; `--list` prints the current inventory. The runner
links an exact strong `__stasha_panic` reason-6 hook; hook output and exit status
also verify the expected operand trace where specified. This test hook's I/O is
instrumentation, not compiler-generated bounds-path I/O. Runtime cases run at
each requested level; diagnostic cases remain compile-only even if accidentally
accepted. Preserve compiler/runner/hook hashes and per-case compile/runtime/IR
artifacts. Do not infer a passing A1 result from the historical receipts below.

The implemented A1 contract is:

- Explicit fixed-array reads, stores, compound updates, and recursive indexes
  require `0 <= index < actual LLVM array extent`. Only pure terminal
  `&array[N]` permits one-past formation, with no element load or dereference
  permission. Nested bases remain strict; `&matrix[rows][0]` is invalid.
- Slice reads, stores, compound updates, and indexed addresses use logical
  `len`; `&slice[len]` is invalid. Descriptors must have `len >= 0` and
  `cap >= len`, including explicit unchecked accesses. Shape is numerical
  validation, not backing-allocation, lifetime, or initialization proof.
- All source index/range widths and signedness survive until validation;
  negative signed, narrow unsigned high-bit, `2^32+k`, and full `u64` boundaries
  must not be made valid by truncation. Extents use actual array storage.
- Base/index and source/lo/hi evaluate once in that order. Plain assignment
  evaluates RHS before indexed target; compound assignment evaluates target
  before RHS. Guards follow the operation's relevant operands. Descriptor
  rebinding must affect later accesses. An RHS invalidating a saved compound
  target is a lifetime issue beyond A1.
- Ordinary bounds/ranges/shape checks remain active in `unsafe`.
  `[unchecked: index]` requires `unsafe` and suppresses only its own extent
  guard, preserving shape/type validation and checks in its base, index
  expression, and surrounding operations.
- Array views require `0 <= lo <= hi <= N`, rejecting array-to-slice conversion
  when `N > INT32_MAX`. Slice views require `0 <= lo <= hi <= current len`, not
  capacity. Result `len = hi-lo`, `cap = old_cap-lo`; empty-at-end is valid.
- Raw data-pointer ranges require `unsafe`, explicit `hi`, and a resolved
  non-void sized element. Pointer/lo/hi evaluate once. Full-width guards require
  `0 <= lo <= hi <= INT32_MAX`, plus `hi <= target_max_size / ABI_element_size`
  before the range offset when element size is nonzero. Zero-sized elements
  avoid division. Result `data = ptr + lo`, `len = cap = hi-lo`.
  Functions, futures, and opaque handles require an explicit typed data-pointer
  cast before view construction. The caller asserts a live, correctly aligned,
  initialized, correctly typed interval with mutation permission. Empty views
  still need a valid in-allocation or one-past pointer; null is permitted only
  for `0:0` as a precondition, not runtime null/lifetime proof.
- All bounds/shape failures use nonreturning `void __stasha_panic(u32)` reason
  6. Hosted default: weak trap without I/O. Freestanding final images: required
  external hook. Scalar reasons 1–5 remain stable; 7 (null) and 8 (allocation)
  remain reserved. There is no unwind/defer-on-panic promise. Preserve focused
  hook checks and freestanding required-hook/object-import evidence separately.

The source-backed archive marker is **ABI 4**: rebuild older source-backed
archives despite the unchanged three-field descriptor layout. The marker alone
does not change native C ABI shape.

A1 excludes initialization-produced writes/ranges/spreads, `foreach`-generated
indexing, `copy`, `append`, allocation, general raw-pointer access,
initialization, ownership/lifetimes, concurrency, and atomic/volatile semantics.
`append`/`rem` ownership remains unqualified: aliases, fields, and returned
interior views can reach realloc/free; neither `heap` nor `cap` establishes a
live owning allocation base. Raw `new`/`mov` policies, K0 restrictions, and
indexed `++`/`--` rejection remain unchanged.

## Historical P3 access runner and evidence

The following baseline and initial checked-access results record the previous
contract. In particular, old unsafe bounds-suppression controls, native trap
matching, and any capacity-expanding view assumptions are not A1 acceptance
criteria. Keep their counts/hashes as history; use fresh exact-binary A1 evidence
for the current contract. The initial review was followed by shared
access-lowering repairs.

Run:

```sh
python3 tests/memory/run_access_contracts.py --compiler ./bin/stasha \
  --opt 0,2 --artifacts /tmp/stasha-memory-audit
python3 -m unittest discover -s tests/memory -p 'test_access_runner.py'
```

`--case NAME` is repeatable. Use `--list` for the current explicit inventory.
`--scope access` selects bounded access checks, while the default `--scope all`
also retains unresolved nullable, lifetime, and receiver-effect requirements.
Diagnostic rejections run once at O0; runtime cases run at each requested level.
Fresh case directories, exact compile exit 1 for expected rejections, diagnostic
headline and source-location checks, absence of an output artifact, bounded
process-tree execution, exact stdout/exit checks, hashes, and IR retention reuse
the scalar suite's fail-closed runner. Accidentally accepted negative programs
are NEVER executed. Five mock-runner tests cover rejection status, signals,
timeouts, artifacts, no execution of negatives, and stale executable removal.

The slice bounds tests deliberately index outside a one-element view while
remaining inside its live two-element backing array. Thus a missing check can
be observed without executing a physical out-of-allocation access. Expected
behavior is the documented bounds trap. Trap evidence requires exact native
trap termination and slice-specific guard/trap pre-optimization IR; this is not
an instrumented panic-hook reason test. Under the previous contract, unsafe slice
accesses were a positive suppression control and remained inside the backing
allocation. A1 instead requires ordinary invalid accesses inside unsafe to fail;
only the explicit unsafe `unchecked` flag skips its own extent guard.

## Historical checked access checkpoint (previous contract)

Compiler `bin/stasha-access-20261010T0608` (SHA256
`898e29222174e20635244d50ad3e63d80a297e61312ebf60afaf3bf4b3faec7f`)
passed 67 focused outcomes at O0–O3. Independent review then found unchecked
assignment parsing, const/final address derivation, non-identifier reslicing,
and field/call pointer-conversion gaps. Regressions and repairs for those cases
are being validated; the earlier 67 passes are not a whole-language claim.
CFG nullable facts, alias lifetimes, receiver effects, and unbounded raw-pointer
view construction remain outside this checkpoint’s qualified scope.

## Historical executed baseline, 10 October 2026 (previous contract)

Compiler: `bin/stasha-parameters-20261010T0531`
SHA256: `a19c57fcb4f92d1c45518aeccef3c01266982a39da9d0fcbf9d9c1f63c5fcdbc`
Version: `stasha 0.2.0-dev (ABI 2)`
Parent build provenance identifies the pinned LLVM 23 toolchain; this runner
records binary identity rather than independently rebuilding that toolchain.

Result: **12 PASS / 27 FAIL**, at O0/O2 for execution. Evidence:
`/tmp/stasha-memory-audit/memory-access-cvjg_5as/summary.json`.
Compiler hashes before/after match. Working-tree fingerprints are review inputs,
not proof of which dirty sources were built into the immutable compiler. Member
lvalue production code was being repaired concurrently and must be rechecked
against the subsequent compiler. Do not turn these failures into expected passes.

Passed controls:

- Reject read-only indexed compound update, known-nil indexed read, direct
  return of a local address, and direct outer-scope storage of an inner address.
- Trap a slice read beyond logical length; execute valid slice read/store/compound
  operations, explicit unsafe accesses, and a guarded nullable parameter.

Failures, prioritized:

1. **Wrong pointee width on valid programs.** `typed_member_deref` and
   `typed_call_deref` load 52 instead of 4660 from a live initialized i32 through
   a pointer-valued field or call. Both compile successfully and exit 1 under
   the exact runtime oracle, at O0 and O2. `gen_unary_prefix` derives types by
   walking to an identifier and otherwise defaults to i8; explicit dereference
   stores and compound updates contain the same fallback.
2. **Slice stores bypass checks.** `slice_oob_store` and `slice_oob_compound`
   compile successfully and exit 0 instead of trapping at both levels.
   `gen_index` emits bounds logic only in its direct slice read path;
   `gen_assign` and `gen_compound_assign` directly form a slice element GEP.
3. **Permission enforcement depends on syntax.** Ten required rejections are
   accepted: read-only dereference/index/member stores, read-only dereference
   compound update, write-only dereference/index/member reads and compound
   updates. The passing read-only indexed compound control demonstrates existing
   checks are incomplete rather than pointer permission syntax being unsupported.
4. **Nullability enforcement is disconnected.** Nullable dereference/index/member
   parameter reads and known-nil dereference/indexed store are accepted.
   `check_nullable_deref` and `prov_narrow_nonnull` have no callers. `gen_if` has
   no nil-fact propagation; `check_null_deref` is used only by direct indexed reads.
5. **Alias lifetime/provenance is not enforced.** Return-of-local-pointer alias,
   outer-scope pointer assignment through an alias, and direct/aliased use after
   `rem` are accepted. None is executed. `check_stack_escape` and
   `check_pointer_lifetime` recognize only selected immediate syntax.
   `prov_record_alloc`, `prov_close`, `prov_check_use`, and `prov_propagate` have
   no callers. Merely connecting them is insufficient: close mutates one
   name-based entry, while aliases copy its closed state instead of sharing
   allocation state. Shadowing, reassignment, and branch joins are unmodeled.

## Historical proposed serialized repair

This proposal records the initial review. Its unsafe suppression wording is
superseded by the A1 contract above; the unresolved nullable/ownership items
remain separate work, not evidence that those repairs have been implemented.

1. Replace identifier-only dereference typing with the resolved immediate
   operand type (`scalar_expr_type`, alias resolution, then `ti_deref_one`).
   Diagnose unresolved/non-pointer dereferences; never substitute i8. Apply
   this to reads, stores, compound updates, and addressable aggregate dereferences.
2. Centralize lvalues into an access descriptor with address, source element
   type, LLVM storage shape, alignment/access flags, and optional bound. Access
   modes are read, write, read+write, and address-only. Load/store/RMW emission
   all consume the same descriptor. Evaluate object/index expressions once.
3. Extract the direct slice read check into a shared slice-element address path
   and reuse it for load/store/compound. Bounds use logical length, source-signed
   index conversion, and a single documented trap mechanism. Keep explicit
   `unsafe`/`unchecked` suppression scoped to that access. Include slice-valued
   member/index/call expressions rather than special-casing identifier syntax.
4. Check permissions at EACH pointer traversal, with read+write for compound
   operations. Reading a pointer slot does not require read access to its pointee.
   Conversely, writing through a writable pointer FIELD of a read-only struct
   view requires reading that field and writing its pointee, not writing the
   containing struct. Blindly propagating a `writing` boolean to every base
   over-rejects this valid case. Preserve const/final and private-field checks.
5. Keep declared nullable types immutable; maintain scope/CFG non-null facts by
   symbol identity. Recognize guarded then/else edges, invalidate facts on
   reassignment and potentially alias-mutating calls, intersect surviving facts
   at joins, and conservatively handle loops. Simply calling the dormant helper
   rejects documented guarded code; simply mutating `stype.nullable` leaks facts.
   Test guard success, the other edge, reassignment inside a guard, post-join
   access, short-circuit conditions, and loop invalidation before claiming safety.
6. Allocation/borrow state must be keyed by allocation and symbol identities,
   with aliases sharing state and scope/branch-aware transfer. Return/assignment
   analyses need expression provenance through members/calls, not just names.
   Implement a bounded explicitly documented conservative analysis first; do not
   claim full hosted memory safety from local helper calls or K0 restrictions.

At that review, the wiki described unsafe as suppressing safety checks and
permitted permission widening; `check_permission_widening` also skipped unsafe.
A1 intentionally changes the bounds contract: ordinary checks now persist and
unchecked indexing requires unsafe. Existing pointer-permission escapes are
separate. This compatibility change requires fresh regression evidence and
ABI-4 source-backed archive rebuilds.


## Pointer qualifier regression suite

`python3 tests/memory/run_pointer_qualifiers.py --compiler ./bin/stasha --artifacts /tmp/stasha-qualifiers`
checks explicit pointer casts, mutable nested pointer-slot invariance, read-only
covariance, aliases, and slice descriptor/backing address separation. The fixed
inventory contains 29 cases: 14 compile-only negative checks and 15 positive
controls, run at O0–O3 by default (74 outcomes). Invalid programs are never
executed, even if accepted. It reuses the scalar runner's diagnostic, artifact,
process-timeout and exact runtime oracles. `test_pointer_qualifiers.py` verifies
the inventory; existing runner unit tests exercise fail-closed rejection behavior.

The suite reconstructs three findings named in the recovery evidence;
the original standalone fixture files were not included, so these are not
byte-identical reruns. Arithmetic permission consistency, pointer depth/type
reinterpretation, integer-pointer conversions, lifetimes and nullable flow are
not qualified by this suite. Fresh-address arithmetic and bare-star declarations
remain compatibility controls. Existing explicit pointer-permission `unsafe`
escapes remain supported; this does not disable ordinary A1 bounds checks.

## Fixed-array len/cap correction

```sh
python3 tests/memory/run_array_queries.py --compiler bin/stasha-array-query-dev1 \
  --opt 0,1,2,3 --artifacts /tmp/array-queries
python3 tests/memory/test_array_queries.py
```

The maintained inventory has 156 fixtures at O0–O3 (624 outcomes): 41 valid
runtime sources, 44 expected source rejections, 48 invalid-runtime bounds IR
controls, 14 legacy malformed-route IR characterizations and 9 boundary IR
controls. The last three categories are compile-only. Acceptance of those
artifacts never authorizes executing them and is not a general safety pass.
Unexpected accepted rejection fixtures also never execute. Runner units verify
classification and fail-closed command, diagnostic, artifact and IR behavior.

The correction applies to newly compiled fixed-array query bodies while keeping
ABI9. Independent archive evidence must retain a genuine old provider that still
returns zero and compare genuinely rebuilt providers at O0–O3. Rebuild every affected provider from source and relink its consumers; an
accepted old marker does not imply a corrected body.

Exact actual storage rank, operand evaluation, access permission, slice behavior,
raw-pointer non-inference and I1b.2 hook preservation are distinct controls.
Within the closed-local interface subset, use a typed `i32` query-derived index;
the equivalent inline query remains a known source-proof exclusion. Historical
len-minus-one invalid-index IR becomes a valid last-element case after correction,
so current out-of-range tests use independent literal and adjusted query boundaries.
