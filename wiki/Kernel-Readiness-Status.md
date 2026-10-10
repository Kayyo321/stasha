# Kernel-readiness implementation status

The implementation follows [the reviewed plan](Kernel-Readiness-Implementation-Plan.md).
This page records engineering status, not a release qualification claim.

## Contract

Retain one shared hosted/kernel compiler and high-level language features. First
qualify a bounded, allocation-free freestanding component behind a scalar/pointer
C ABI. The provisional qualification target is x86-64 ELF; this does not select
an architecture or bootloader for the owner's separate OS project. K1 interrupts
and K2 SMP require their own evidence. Unsupported kernel capabilities must be
rejected explicitly rather than silently falling back to hosted behavior.

## Source and toolchain

- Consolidation candidate starts at `7c0e6359a44d7d348e64623db5685d608a94453a`.
- `devel` (`87d7e443caea217572c799e5270ff275a69d6591`) is an ancestor of `main`
  (`d60731a7d22ef7ba9ec59ebe631f4828af28af75`), itself an ancestor of the candidate.
- No existing history is discarded. Remote branch cleanup follows verification
  and explicit recoverability review; branch count is not a readiness test.
- Required LLVM/LLD revision: `348061d58dfe5e59027a0d6d51f916197cdd8d68`,
  version `23.0.0git`. A different installed LLVM library is not equivalent.

## Milestones

| Package | Status | Remaining acceptance gate |
| --- | --- | --- |
| P0: build and trustworthy test oracle | In progress | Exact toolchain build, clean baseline, selftests, complete suite inventory |
| P1: typed scalar semantics | In progress | Width/sign/range semantics, boundary/differential O0–O3 execution |
| P2: target and representation | In progress | Target normalization, layout/initialization oracle, ABI identity |
| P3: memory and cleanup | In progress | CFG exits, lifetime/permission/bounds negatives and runtime traces |
| P4: ABI and object emission | In progress | Narrow ABI, register/stack/red-zone policy, object/IR modes |
| P5: freestanding contract | In progress | Forbidden-effect diagnostics, runtime-import manifest |
| P6: image linking | Pending | Target-driven link, segment/relocation/map audits |
| P7: machine memory and assembly | Pending | Volatile/atomic access correctness and assembly boundary tests |
| P8: K0/K1 harness | Pending | Reference component and emulator tests; K1 interrupts/hardware |
| P9: K2 concurrency | Pending | SMP/memory-model/selected-hardware qualification |
| P10: reliability/performance | In progress | Sanitizer regressions first; fuzz/differential/performance campaigns |
| P11: qualified release | Pending | Reproducible artifacts and documented supported matrix |

## Evidence policy

Each test result must identify source revision, toolchain, target, flags, command,
exit category, and actual outcome. A blocked or unrun test is never a pass.
Parser-only checks are not compiler correctness evidence. Mock harness tests test
the oracle, not the language. A boot message does not qualify interrupts or SMP.
Known hosted or experimental defects remain visible even when outside K0.

### P0 host publication boundary

Linux output publication uses an isolated sibling directory and only renames a
successful artifact into the requested destination. Failed builds invalidate
stale primary output. macOS and Windows retain native output/sidecar paths;
primary output is invalidated on failure, but the full dSYM/PDB/import-library
bundle is not yet transactional or release-qualified. Darwin symbol extraction
must run before its original object is removed. Native host CI is required;
Linux tests do not establish Windows or macOS correctness.

## P0 test-oracle implementation

Diagnostic, C-interop, and signals runners now share a fail-closed subprocess
harness: fresh output directories, bounded compiler/runtime processes, exact
expected exit categories, retained failure artifacts, and distinct stdout/stderr.
The LLVM capstone Make target also uses this harness. The previously unlisted
C-interop variadic rows are explicit positives. The macOS/AArch64 aggregate-argument
xfail accepts only its documented wrong-value signature; XPASS requires review.
See [harness contracts and commands](../tests/harness/README.md) and
[qualification inventory](../tests/qualification/suites.json).

The mock-compiler selftests pass on the current Linux host, including intentional
fixture failure, compilation failure despite output, stale binary, timeout,
crash/verifier/sanitizer rejection, and process-tree cleanup. This establishes
oracle behavior only. Real diagnostics/C-interop/signals and other host platforms
remain unqualified until run with the selected compiler/toolchain. Existing
unclassified diagnostic fixtures and the contradictory await diagnostic remain
visible qualification gaps; they are not waived as successes.

### First integrated checkpoint (10 October 2026)

This is a development backup, **not a release-ready compiler**. The exact pinned
LLVM23/LLD essential toolchain builds and links both the pristine baseline and
the changed compiler. The full Clang build continues separately. See
[checkpoint evidence](../tests/qualification/checkpoint-2026-10-10.json) for
commands, observed results, binary identities and explicit remaining failures.

Checked results include:

- Frontend: 1,529 UBSan assertions and 256 deterministic syntax mutations;
  earlier 1,463-assertion revision also passed ASan+UBSan. LeakSanitizer is
  unavailable under process tracing. Parser checks do not qualify codegen.
- P1 integers: 2,304 actual compiler/execution checks at O0–O3, including exact
  panic reasons; 56 floating conversion checks. Constant engine: 2,752,600
  Python-reference differential operations under UBSan.
- Cleanup: 108/108 O0–O3 cases including instrumented zone release counts/order.
- Static initialization: 183 O0/O2/O3 checks, including native C-linked data,
  relocations, nested aggregates, callbacks and module-name collisions.
- Layout: 721 checks against pinned LLVM23, plus actual C representation
  comparison for 72 field-pair cases and nested/generic/tagged/packed objects at
  O0/O2, and 14 rejected layouts. Aggregate function ABI remains a separate gate.
- Driver: 31 cases; independent contract review: 18 cases; lifecycle: 16 exact
  O0–O3 traces; all 7 existing signal cases. 64 concurrent same-directory
  compilation cases pass on Linux after serialized log rotation/opening.
- C-header parser: 107 UBSan and ASan+UBSan checks (LSan disabled).
- Test oracles: 42 mock/unit checks. Standard-library and thread targets now
  actually execute their generated test programs and require a nonempty passing
  summary; the former targets only compiled them.

The original Linux compiler linked hosted executables without CRT startup,
producing entry-point-zero executables that crashed. Native GNU/Linux linking
now checks target/CC/object ABI and uses explicit startup/support files with the
pinned embedded LLD. ELF lifecycle slots use retained init/fini arrays and their
dependency graph is explicitly zero initialized.

#### Diagnostic expectation reconciliation

Two old negative fixtures contradicted documented v2 behavior in
[Async-Await](Async-Await.md): ordinary functions may synchronously await, and
streams may return a typed final value. They are now **executed positive tests**
checking returned values, not waived negatives. Genuine wrong-stream dispatch
and wrong-handle negatives remain; their outdated builtin call spelling was
updated to the documented dotted form. All 29 diagnostic source fixtures are
classified (32 invocations including warning-option variants).

#### Current blockers and unsupported boundaries

- C interop: 28/37 pass. Four aggregate ABI runtime failures, two outdated
  variadic fixture spellings, errno header traversal, an explicit signed-canary
  cast, and LLVM C++ support linkage remain measured blockers.
- First real non-bundled stdlib execution: 21/29 pass. Eight failures comprise
  list/set/map test conversion diagnostics; an empty console test suite;
  file-stream and mutex runtime crashes; complex/vector equality failures.
  Bundled-runtime/OpenSSL and full thread suites remain pending.
- A namespaced global write in library mode fails lookup; object/IR component
  tests proceed, but kernel library qualification is blocked until repaired.
- Experimental `--kernel` is limited to x86-64 ELF obj/IR/lib with scalar/pointer
  ABI and explicit unsafe raw-memory access. It rejects unqualified effects,
  atomic/volatile/TLS, FP/SIMD, async, heap, generic and aggregate interfaces.
  Runtime hooks are `__stasha_panic(u32)` and backend-generated C memory helpers.
  A kernel object audit checks format, imports and forbidden lifecycle/TLS/EH
  sections. IR output alone cannot prove post-lowering imports.
- C-layout corrections require rebuilding ABI2 Stasha archives. Source-backed
  archives require matching version/target metadata. Bitfields and over-aligned
  source objects are explicitly rejected pending correct access/ABI support.
- Static union/tagged payload, spread/range/nonconstant initializers, named or
  inferred global extents and TLS addresses are explicitly unsupported.
- General pointer provenance, bounds/permissions and ownership are not fully
  qualified. Unsafe K0 access is an explicit caller boundary, not a lifetime proof.
- Kernel component/boot work is in progress; K1 interrupts, K2 SMP and real
  hardware, native Windows/macOS and reproducible release artifacts are unrun.

No remote branch has been pushed, merged or removed by this checkpoint. Existing
main/devel/portability history remains intact. Publication and eventual one-main
cleanup require an explicit reviewed fast-forward and recoverability plan.
