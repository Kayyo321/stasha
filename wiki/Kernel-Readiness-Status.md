# Kernel-readiness status

## Descriptor defaults and null-dispatch checkpoint (10 October 2026)

Supported ordinary local interface leaves now receive targeted empty descriptor
stores through exact struct/fixed-array layouts, preserving unrelated siblings.
Existing dynamic calls reject null object/table before table access and a null
selected function pointer before invocation. Readable live descriptor storage and
defined pointer words, plus a readable selected table slot yielding a defined
pointer, remain prerequisites. This adds no table-membership, CFI, object-pairing,
lifetime, ownership or concurrency proof. Supported contextual bare nil-to-I
transfers now receive precise source-owned diagnostics. See
[the complete contract and exclusions](Interfaces.md#default-descriptors-and-null-dispatch-semantic-abi11).

**Rebuild all source-backed providers and consumers for semantic ABI11.** Genuine
ABI10 retained bodies lack these guarantees; old archives reject before import,
code generation or linking. Descriptor layout, slot order and native signatures
are unchanged. Never relabel sidecars as a substitute for rebuilding.

Frozen compiler SHA-256:
`495d1dfdb60176ef7e5ac24d465e0966d53b265c59d3db57b5753119c3d0edfe`.
The complete hosted pipeline including the new descriptor gate, all36 rebuilt
standard libraries and qualified memory regressions pass. Focused evidence has
116 strict rows (28 conventional valid executions),14 imported diagnostic rows,
68 independently gated canonical-empty executions, and393 compiler-unit checks
across28 complete compilations. A trusted relocated same-source build with a
distinct binary identity passes the portable reviewed source/IR/hook gate.

Local genuine archive qualification separately covers40 old-provider refusals,
four rebuilt-provider inspections, eight valid source/archive executions and
precise nil source spans. Historical invalid bodies never execute. Hosted CI does
not claim the historical archive matrix. Earlier failed/canceled attempts and
known historical B1 cleanup failure remain attributed in the
[checkpoint receipt](../tests/qualification/checkpoint-interface-descriptor-defaults-2026-10-10.json).

Malformed explicit transfers (including wrapped nil/capture paths), excluded
carriers/effects, general helper history, generated spread/foreach/copy/append/raw
memory and full native/kernel readiness remain open. The new targeted default
loops have separate bounded evidence; K0 interface restrictions are unchanged.
No fresh non-Linux/kernel boot/hardware qualification is claimed.

## Historical source-local leaf factory result checkpoint (10 October 2026)

Selected factory-to-local-copy-to-field-projection paths can now feed the existing
identifier-valued interface use when their exact source-local producers and
emitted call/return evidence are verified. The first subset is an internal,
primitive-input leaf factory with one final initialized-local return and a
complete cleanup-free body classification. Branches, nominal helper chains,
retained bodies and unsupported storage remain outside new admission. See
[the factory contract](Interfaces.md#source-local-leaf-factory-results).

ABI10 is retained. No accepted old archived result receives a stronger guarantee:
both old and fresh archived Holder projections still stop at the declared-body
boundary. Source compiled in the same translation unit can qualify through its
own exact internal named-call route. No metadata relabeling substitutes for that
body. Dead source uses and unused calls do not activate new rejection.

Frozen compiler SHA-256:
`60858af10b17937a4181551a0698f7fc91bcc2789db4305c419052542f93ec2e`.
The full hosted gate, all 36 rebuilt standard libraries and qualified memory
regressions pass. The maintained corpus contains 59 sources and 332 Gate B checks:
96 valid executions, 96 separate valid IR checks, and 140 compile-only rejection
or deferred outcomes. Thirty strict runner sentinels pass. Independent review
also preserves exact Gate A compatibility and lifecycle evidence, source imports,
real native copy-out and genuine ABI10 archive boundaries. See the
[factory checkpoint receipt](../tests/qualification/checkpoint-leaf-factory-2026-10-10.json).

This adds no general helper-history, descriptor initialization/authenticity,
formal-dependent projection, lifetime, ownership, concurrency or CFI theorem.
Existing matching-annotation histories and unsupported routes remain explicit
boundaries. Earlier generated-access/growth and complete native/kernel readiness
work remains open.


## Historical I1b.3a immediate nominal boundary checkpoint (10 October 2026)

Ordinary named-direct arguments and explicit single-result returns now reject
provable immediate nominal mismatches when the destination is an ordinary
concrete interface implementer. The check applies without a downstream interface
sink and does not confuse equal native representation with source identity.
See [the exact routes and exclusions](Interfaces.md#i1b3a-immediate-nominal-boundary-checks).

**Rebuild all source-backed Stasha providers and consumers for semantic ABI10.**
Two genuine ABI9 provider bodies retain the newly rejected argument/return
mismatch. They reject before import/code generation/link under the new marker;
corrected providers and consumers execute O0–O3. Historical invalid bodies never
execute. Layout, signatures and descriptor slots are unchanged; no complete
helper-history or universal correctness claim follows.

Frozen compiler SHA-256:
`3365adff9a547b3e8731b6f1c1d17027fc2bc0f5405acbc74203233dd88ab925`.
Full hosted qualification, all 36 rebuilt standard libraries and qualified memory
regressions pass. The 528-outcome maintained matrix contains 26 valid sources,
58 expected rejections and 48 deferred compile-only characterizations at O0–O3.
Deferred acceptance is not a safety rejection pass. Independent controls include
40 valid executions, 108 clean rejections, 100 deferred compile-only outcomes,
12 local no-promotion controls, 12 genuine archive gates and eight retained
archive declaration-boundary controls. See the
[I1b.3a receipt](../tests/qualification/checkpoint-interface-i1b3a-2026-10-10.json).

Matching annotations still do not prove prior stores, helper bodies or descriptor
validity. Factory/parameter-fed projection eligibility is unchanged. Unsupported
adapters, generated/multi-result/bare-return paths and nonimplementing Holder
carriers remain explicit boundaries. Earlier ownership, lifetime, CFI, default
initialization and generated-access/growth work remains open. No new native
non-Linux or kernel/hardware qualification follows.


## Historical fixed-array query correction (10 October 2026)

Supported newly compiled `len.(array)` and `cap.(array)` expressions now return
the actual selected element extent, preserving operand evaluation, access checks,
array rank and interface-local proof hooks. Slice descriptor behavior is unchanged.
See [query semantics and migration](Slices.md#fixed-array-and-slice-length-queries).

**ABI remains 9. Rebuild every affected provider from source and relink its
consumers.** Existing ABI9 archives stay accepted and can retain the old wrong
zero-return body. Genuine old/new provider evidence demonstrates that boundary;
metadata is not universal behavioral attestation. There is no physical layout,
function signature or interface safety-contract marker change.

Frozen compiler SHA-256:
`e6c28df52b705364b266c47635b2b9856d7fb08208001fa021ffc8fedd8d6db6`.
The complete hosted gate, all 36 rebuilt standard libraries and qualified memory
regressions pass. Dedicated query controls pass 624/624 at O0–O3, with 28
runner unit tests and ten mocked hosted fail-closed controls. Independent review
adds 80 safe runtime observations, 168 normal source rejections, 208 compile-only
IR controls and eight genuine old/rebuilt-provider consumers. Invalid runtime
and malformed-route IR controls never execute and are not broad safety passes.
See the [query checkpoint receipt](../tests/qualification/checkpoint-array-queries-2026-10-10.json).

The correction does not infer raw-pointer extent or repair general nullable,
owner/lifetime or unsupported array-owner routes. Within the I1b.2 closed-local
subset, use an explicit typed `i32` query-derived index; the inline query form
remains an existing source-proof exclusion. Earlier descriptor/helper/return,
generated-access and growth gaps remain open. No fresh kernel/hardware or native
non-Linux readiness claim follows.


## Historical I1b.2 local transfer and projection checkpoint (10 October 2026)

Bounded ordinary concrete local copies/reassignments and field/fixed-array
copy-then-use projections now retain checked producer and write identity at
admitted interface uses. The immutable local inventory follows original lexical
bindings, effects and use frontiers. Exact aliases are supported; a typed
annotation or matching storage size is not sufficient evidence. See the
[local transfer contract](Interfaces.md#i1b2-checked-local-copies-and-projections).

Rebuild **all source-backed Stasha archives and consumers for semantic ABI 9**.
Descriptor layout and slot ordering are unchanged. Genuine ABI8 retained-body
mismatches reject before import/codegen/link; corrected ABI9 consumers execute
provider-owned indirect dispatch at O0–O3. Historical invalid programs remain
compile/link-only and are never run; sidecars are not relabeled.

Frozen compiler SHA-256:
`c656109409bd2aa7935d2315f7cdb20107d6e79f4e19224968fd4a33d14eabf5`.
The full hosted gate, all 36 rebuilt standard libraries and qualified memory
regressions pass. Maintained checks: local transfers 268, source calls 568,
binding 274 and graph 82. Independent review covers 192 valid executions,
284 normal source rejections, 55 retained/inventory controls, 996 ordinary
functional assertions and 24 genuine archive controls. The independent sanitizer run
was unavailable because of LeakSanitizer's ptrace restriction; writer 603
ASan/UBSan-only local checks are separate evidence. See the
[I1b.2 receipt](../tests/qualification/checkpoint-interface-i1b2-2026-10-10.json).

Seven retained accepted gap characterizations are compile-only and remain
unqualified. Ordinary helper parameters/results, broad descriptor stores/returns,
default initialization and aggregate descriptor contents remain open. This stage
does not prove object liveness, ownership, CFI/table membership or concurrency.
Generated access/growth gaps and full native/kernel qualification remain open.

The existing fixed-array len/cap numerical query issue is explicitly deferred:
it can return zero, although checked indexing independently retains the actual
storage bound. Original failed evidence is preserved, and the reproduction,
risk and bounded follow-up checklist are in [Interfaces](Interfaces.md#separate-known-extent-query-limitation).


## Historical I1b.1 interface source-call checkpoint (10 October 2026)

Supported interface-slot calls now compare every actual's source contract before
lowering. Supported native adapters check interface-valued formals, including
same-native-type paths that formerly bypassed boxing checks. Raw nominal identity,
alias layers, permissions, nullability, domain evidence and selected callable
identity remain distinct from LLVM representation. Unsupported source queries
return quiet reasons; unrelated calls do not acquire diagnostics simply because
an interface declaration is present. See [the checked call contract](Interfaces.md#i1b1-source-contracts-at-interface-calls).

Rebuild **all source-backed Stasha archives and consumers for ABI 8**. The marker
is semantic: descriptor layout and slot ordering are unchanged. Genuine ABI 7
bodies with unchecked enum arguments are rejected before import/codegen/link;
corrected ABI 8 provider/consumer calls execute through retained indirect slots at
O0–O3. Old invalid programs are never executed, and sidecars are never relabeled.

Frozen compiler SHA-256:
`de76a5f27892e1889f0e8413433a8fce46230f83ab4efa414bb15dc0b50efa1c`.
The final hosted gate, all 36 rebuilt standard libraries and qualified memory
regressions pass. Maintained suites pass 568 source-call, 274 binding and 82 graph
checks. Source-fact sanitizer checks pass 176; retained graph/metadata/diagnostic
units pass 241, binding units 110, and frontend checks 1,541. Independent review
passes 336 valid runtimes, 1,236 source rejections, 8 genuine archive gates,
12 internal-binding diagnostics,140 + 84 source/native sanitizer checks and 32
compiler-only sanitizer controls. See the
[I1b.1 receipt](../tests/qualification/checkpoint-interface-i1b1-2026-10-10.json).

Eight accepted deferred gaps are characterized compile-only and are not passing
safety rejections. Prior stores, helper parameters/results, default descriptors,
and named aggregate carriers can still carry unchecked values. Ordinary named
aggregates containing interfaces remain usable but unqualified, including
variadic/thread routes. This milestone does not close general descriptor flow,
object pairing, CFI, lifetime, ownership or concurrency. Concrete projection and
assignment usability restoration requires a separate checked-write stage.
Existing A1/B1 generated-access, initialization and growth gaps remain open.
No fresh kernel boot/hardware or native non-Linux release claim is made.

## Historical I1a declared interface binding checkpoint (10 October 2026)

I1a verifies supported declared signatures, complete method binding and native
entry-point agreement before publishing interface tables. Conflicting inherited
requirements and missing or incompatible implementations now diagnose. Dynamic
calls use the formal cached ABI rather than synthesizing it from actual arguments.
See [the supported interface contract](Interfaces.md#i1a-declared-signatures-and-table-binding).

Rebuild **all Stasha archives and consumers for ABI 7**. This is a semantic
compatibility boundary: the ABI6 slot ordering and two-word descriptor layout
remain unchanged. A genuine old provider archive can carry unchecked table
construction inside its body even when imported declarations look acceptable.
Tests reject that archive before code generation and exercise new provider /
consumer dispatch at O0–O3. Historical invalid programs were never executed.

The frozen compiler SHA-256 is
`732ac8a2b21921f87f50a1e7bf0bd36daf374459eece002be3efec91320d60d1`.
The maintained I1a suite passes 274 checks, including 37 compile-only origin
controls. Nine genuine archive controls and 110 binding ASan/UBSan checks pass.
Retained metadata/graph/diagnostic units pass 241 checks, and frontend checks pass
1,541. Independent review includes 88 source contracts, O0–O3 cross-provider
native calls, 32 supplementary controls, 84 synthetic native ABI sanitizer
checks, and 35 initialization-origin controls. The final hosted suite, all 36
rebuilt standard libraries and qualified memory regressions pass. See the
[I1a receipt](../tests/qualification/checkpoint-interface-i1a-2026-10-10.json).

This is bounded declaration and initial-eligibility checking. Concrete values
with retained generic/dependent or unresolved initialization origins cannot enter
the admitted boxing/qualified-call seams. Field/index-derived initializers are
an explicit interim restriction, even when copied to a typed destination. Direct
ordinary values and calls declaring ordinary return types remain supported,
including inside unrelated generic functions. Restoring valid projection and
assignment cases with complete source provenance is a priority for I1b.

Complete actual source-argument checking and descriptor flow through assignments,
returns, casts, conditional merges, fields and containers remain pending. No
runtime table-membership check, indirect-call CFI, object liveness, ownership or
general generic safety is established. Owned buffers remain design-only. A1
implicit generated accesses and B1 append/copy, initializer/spread and broader
nullable/concurrency gaps remain open. No fresh I1a kernel boot, hardware or
native non-Linux release qualification is claimed.

## Historical I0 interface graph and metadata checkpoint (10 October 2026)

I0 validates inheritance graphs before lowering, retains source contract metadata,
and rejects unsupported or over-limit declarations instead of silently discarding
information. Diamond inheritance now deduplicates repeated paths to the same
method declaration while preserving distinct same-name origins for later signature
comparison. Such unresolved groups cannot produce a table in this checkpoint.
See [interface scope and migration](Interfaces.md#i0-graph-and-metadata-checkpoint).

Rebuild **all Stasha archives and consumers for ABI 6**. This is a real interface
slot-order change: a duplicated ancestor slot in an old diamond table is removed.
The object/table descriptor remains two words. Genuine ABI5 and ABI6 archive
fixtures compare actual table entries and cross-archive inherited calls; no
sidecar relabeling is used. The frozen B1 provider writes the old archive but
returns a known cleanup-only failure, retained separately from the successful
old-archive rejection control.

The imported diagnostic source registry now owns its source text, so diagnostics
can safely render an imported declaration after the driver releases its input
buffer. This is separate from the earlier stable diagnostic filename repair.

The frozen compiler SHA-256 is
`da886d196626f05786492ef9ae52e71f4a98335bc9de2b1842078177644de253`.
The final hosted gate passes, including 80 mandatory I0 checks; the local focused
run passes 81 with the genuine legacy-archive control. All 36 ABI6-rebuilt
standard-library modules and qualified memory regressions pass. There are 24
oracle-unit checks and 237 ASan/UBSan metadata/graph/source-ownership checks.
Independent review adds 1,541 frontend and 554 diagnostic ownership checks.
Full LeakSanitizer is unavailable in this executor; direct allocation/cleanup
assertions are separate evidence. See the
[I0 receipt](../tests/qualification/checkpoint-interface-i0-2026-10-10.json).

I0 does not certify full method signatures or missing implementations, all boxing
routes, dispatch membership/CFI, object lifetime, ownership, or concrete object /
table pairing. I1 binding and call agreement is the next milestone. Owned buffers
remain design-only. A1 implicit generated accesses and B1 append/copy, initializer,
spread, lifetime and nullable/concurrency gaps remain open. No fresh I0 kernel
boot, hardware, or native non-Linux release qualification is claimed.

## Historical B1 explicit-count construction checkpoint (10 October 2026)

B1 adds checked scalar `make.([]T, len[, cap])`: full-width counts, selected-target
allocation-size guards, canonical empty construction, and reason-8 failure before
using or publishing a retained failed allocation. Normal allocation elision
remains permitted. Implicit slice conversions now compare source element identity
instead of relying on erased LLVM descriptors. See the
[B1 receipt](../tests/qualification/checkpoint-make-b1-2026-10-10.json) and
[construction contract](Slices.md#b1-checked-explicit-count-construction).

The frozen compiler is SHA-256
`c1fb7d98ae03a1d4b783f7b8c0a03b6df22346ac471f46f7190947049ab832dc`.
Rebuild source-backed archives for **ABI 5**: old ABI 4 objects can contain
unchecked constructor bodies and are rejected. This semantic compatibility
marker does not change the physical slice descriptor or native C layout.

Focused B1 qualification passes 313 outcomes, including O0–O3 retained-allocation
and fault-injection checks, source diagnostics, IR/layout invariants, and genuine
old/new archive controls. Nine oracle-unit checks, the local hosted gate and all
36 ABI5-rebuilt standard-library modules pass. Receipt source hashes identify
the exact tested compiler. Historical evidence below remains snapshot-specific.

B1 does not establish ownership, alias lifetime, complete nested nullability,
concurrency or all initialization paths. General append/copy, initializer/spread
construction, and raw new/mov remain open. B1 has no fresh kernel boot/hardware or
native non-Linux release claim. Interface signatures are the next correctness
milestone; the separate owned-buffer/borrowed-slice proposal remains design-only.

## Historical A1 explicit-access checkpoint (10 October 2026)

The A1 bounded hardening stage adds checked fixed-array and slice explicit
access, logical-length view bounds, unsafe-only local unchecked indexing, and a
shared reason-6 panic hook. Source-backed archives now require **ABI 4** rebuilds;
the descriptor physical layout is unchanged. See the
[A1 evidence receipt](../tests/qualification/checkpoint-bounds-a1-2026-10-10.json)
and [bounds contract](Safety-System.md#a1-explicit-array-and-slice-bounds).

The frozen A1 compiler is identified by SHA-256
`574bcc7e60cf4001967c94e5a6dc082fd97456e272e8057420c6bb2d27a512bf`.
Its exact source manifest and results are in the receipt. It builds on published
restoration commit `79ea057ee5c0f4bd4d791bc99d2806a06a349ab3`.
The hosted gate and all 36 rebuilt standard-library modules pass locally. Focused
qualification includes 305 new bounds outcomes, 299 slice-view outcomes, 77
shared-access outcomes, pointer regressions and 23 freestanding hook/layout
checks. Six K0 host/object/archive configurations pass; current boot and hardware
execution remain **NOT_RUN**.

This does not prove initialized values, backing-allocation validity, lifetime,
ownership, concurrency, or whole-language memory safety. Initializer-generated
writes, foreach-generated indexing, general copy/append and allocation remain
outside A1. Raw views record an explicit unsafe caller assertion, not ownership.
The next stages are checked explicit-count construction, verified interface
signatures/dispatch, and the approved separate owned-buffer/borrowed-slice design
with actual move/borrow enforcement. Those stages are not implemented by A1.
Native platform release, interrupts, SMP, hardware and complete OS readiness
remain open. Historical results below apply only to their identified snapshot.

## Historical restored development checkpoint (10 October 2026)

This is a tested **development checkpoint**, not a release, a memory-safety
certificate, or a completed OS-development qualification. Its historical receipt is
[restored checkpoint evidence](../tests/qualification/checkpoint-restored-2026-10-10.json).
It identifies the exact compiler, source hashes, commands, toolchain, test scope,
and remaining boundaries. Older evidence below does not automatically apply to
this source snapshot.

The current compiler is built with the repository's exact LLVM/Clang/LLD revision
`348061d58dfe5e59027a0d6d51f916197cdd8d68` (`23.0.0git`). The restored local base
`24fdf438f045303ea633c09e0d640a7657b9a17a` has the same tree as the previously
published checkpoint `df11428e28c383eec715193e0d319abad347cdda`. The receipt's
source manifest, rather than the unchanged base commit alone, identifies the
uncommitted integrated source tested here. Existing branch history is retained;
no branch deletion, force update or protection change is part of this checkpoint.

### Repairs in the integrated source

- Slice reads, stores, compound operations and indexed backing addresses use
  shared typed address calculation. Fixed-array members retain their element
  stride. This does not establish bounds on every raw-pointer or fixed-array path.
- Explicit pointer casts enforce read/write permissions, including nested writable
  pointer slots. Readonly slice descriptors remain shallow: backing-element
  permissions and descriptor permissions are distinct. Existing documented
  `unsafe` behavior is retained at this checkpoint.
- Pointer addition/subtraction uses immediate-pointee stride, including nested
  pointers. Existing void/legacy byte-stride behavior and permission rules remain.
- C-header imports derive readonly permission from the immediate pointee,
  including typedef chains and opaque handles. `const char *` differs from
  `char *const`; variadic argument checks are not bypassed.
- Diagnostic fixtures retain their required rejections while removing incidental
  unused-variable warnings and matching the documented address diagnostic.
  Stale closure/character syntax in the comprehensive positive fixture is corrected.
- PCG reseeding now restarts the unchanged constructor sequence. Known-seed
  vectors and repeated reseed controls pass; cryptographic API bodies are unchanged.
- Earlier restored changes include scalar/constant semantics, C aggregate and
  callback ABI, member/static calls, static initialization, hosted output
  publication, frontend diagnostics and standard-library repairs.

### Current evidence and its limits

The final receipt distinguishes fresh integrated results from intermediate
snapshots. The bounded Linux qualification command covers diagnostics, signals,
C interoperability, scalar O0–O3 execution, layout, aggregate/callback/member ABI,
cleanup, driver and static-initializer regressions. Additional current checks
cover slices, pointer permissions/arithmetic, C pointee qualifiers, frontend and
header-parser sanitizers, stdlib execution, thread tests and the core fixtures.

Fresh K0 evidence covers six O0/O2/O3 debug/nondebug variants, host differential
execution, object/archive validation and low/high-address image checks. **QEMU
boot and physical hardware are NOT RUN on this restored snapshot.** Previous
240-boot evidence is retained as historical evidence in
[the older memory-component receipt](../tests/kernel/k0/qualification-memory-2026-10-10.json);
it is not transferred to the current binary. K1 interrupts and K2 SMP remain unrun.

The member ABI negative runner retains its older allowance for warnings. Its
constructor-arity fixtures emit unused-local warnings; independent receipt checks
verify exact error exit, source location and no object. Invalid programs are never
executed. Some positive core fixtures also retain unused-capture warnings. These
are disclosed limitations, not silently suppressed diagnostics.

### Open work recorded at the restored checkpoint

- Complete fixed-array/slice bounds and allocation-size/growth/OOM checking.
- Complete interface signatures and implementation checks, then separately
  qualified dispatch-table and indirect-call hardening.
- Alias-aware lifetime/ownership, nullable control flow, initialization and
  concurrency enforcement. General pointer provenance is not established.
- Deliberate, documented unsafe-boundary changes with compatibility tests;
  the new hardening roadmap is not yet implemented by this checkpoint.
- K1 interrupts/MMIO, K2 SMP, selected hardware and real page protections.
- Native macOS, Windows and AArch64 ABI/release gates, reproducible packaging,
  broader sanitizer/performance evidence and CI on the exact published commit.
- Chained Stasha pointer-type aliases have independently reproduced declaration
  resolution failures on both the pre-repair and current arithmetic snapshots.

## Contract and milestones

Retain one shared hosted/kernel compiler and high-level language features. First
qualify a bounded, allocation-free freestanding component behind a scalar/pointer
C ABI. The provisional qualification target is x86-64 ELF; it does not select the
architecture or bootloader for a separate OS project. Unsupported kernel effects
must be rejected explicitly rather than silently using hosted facilities.

P0 build/oracle, P1 scalars, P2 layout, P3 memory/cleanup, P4 ABI, P5 freestanding,
P6 image policy and P10 reliability have bounded evidence and remaining work.
P7 machine memory/assembly, P8 interrupts/hardware, P9 SMP and P11 release remain
unqualified. A green hosted test run does not waive those gates.

Experimental `--kernel` remains limited to x86-64 ELF obj/IR/lib with scalar/pointer
ABI and explicit unsafe raw-memory access. It rejects unqualified heap, async,
atomic/volatile/TLS, FP/SIMD, generic and aggregate interfaces. Its runtime boundary
is `__stasha_panic(u32)` and audited backend memory helpers. Object imports, not
unused IR declarations alone, determine the freestanding boundary.

## Evidence policy

Each result identifies source/toolchain, target, flags, command, exit category and
actual outcome. Blocked, unrun or skipped work is not a pass. Parser checks do not
qualify generated code; mock harness checks do not qualify the language. A boot
message does not qualify interrupts or SMP. Compiler sanitizers do not prove that
all generated Stasha programs are instrumented or safe.

Linux uses isolated sibling output publication and invalidates stale output on
failure. Native macOS/Windows sidecar bundles are not transactionally or
release-qualified. The original planning baseline and earlier checkpoint evidence
remain in the implementation plan and historical receipts; their old blockers and
counts are not the current status summary above.
