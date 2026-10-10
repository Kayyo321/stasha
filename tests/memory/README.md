# Hosted memory-access contract regressions

This bounded P3 suite tests the existing hosted promises in
`wiki/Safety-System.md`, plus source-typed dereference width. It does not qualify
kernel memory safety: K0 rejects slices/heap and requires explicit unsafe raw
pointer access. The initial review was followed by shared access-lowering repairs.

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
an instrumented panic-hook reason test. Unsafe slice accesses are an explicit
positive control and still remain inside the backing allocation.

## Checked access checkpoint

Compiler `bin/stasha-access-20261010T0608` (SHA256
`898e29222174e20635244d50ad3e63d80a297e61312ebf60afaf3bf4b3faec7f`)
passed 67 focused outcomes at O0–O3. Independent review then found unchecked
assignment parsing, const/final address derivation, non-identifier reslicing,
and field/call pointer-conversion gaps. Regressions and repairs for those cases
are being validated; the earlier 67 passes are not a whole-language claim.
CFG nullable facts, alias lifetimes, receiver effects, and unbounded raw-pointer
view construction remain outside this checkpoint’s qualified scope.

## Executed baseline, 10 October 2026

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

## Proposed serialized repair

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

The existing wiki says unsafe suppresses safety checks and explicitly permits
permission widening there; existing `check_permission_widening` also skips unsafe.
Any intentional narrowing of that contract should be explicit and covered by
compatibility tests, not an accidental consequence of a new lvalue helper.


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
remain compatibility controls. Explicit `unsafe` escapes remain supported.
