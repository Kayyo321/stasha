# Kernel-readiness status

## A1 explicit-access checkpoint (10 October 2026)

The current bounded hardening stage adds checked fixed-array and slice explicit
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
