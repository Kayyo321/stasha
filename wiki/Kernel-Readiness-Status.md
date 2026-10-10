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
| P2: target and representation | Pending | Target normalization, layout/initialization oracle, ABI identity |
| P3: memory and cleanup | Pending | CFG exits, lifetime/permission/bounds negatives and runtime traces |
| P4: ABI and object emission | Pending | Narrow ABI, register/stack/red-zone policy, object/IR modes |
| P5: freestanding contract | Pending | Forbidden-effect diagnostics, runtime-import manifest |
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
