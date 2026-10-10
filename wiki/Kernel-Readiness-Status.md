# Kernel-readiness status

## I1b.1 interface source-call checkpoint (10 October 2026)

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
