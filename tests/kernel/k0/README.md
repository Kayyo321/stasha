# Bounded experimental K0 component qualification

This is a compiler qualification artifact, **not a kernel or an operating-system
project**. It demonstrates one deliberately small freestanding component under a
reviewed external startup/link/I/O boundary. It does not make the entire Stasha
language kernel-ready, complete the P3 ownership analysis or P6 compiler final
linker mode, qualify K1 interrupts, or validate physical hardware.

## Run

Use a versioned compiler copy adjacent to the runtime archives; do not rebuild it
while a run is in progress. The runner verifies its SHA-256 again at completion.

```sh
python3 tests/kernel/k0/test_runner.py

# Host differential tests and static artifact/link checks only.
python3 tests/kernel/k0/run.py --compiler ./bin/stasha-VERSION \
  --llvm-bin /path/to/pinned/llvm-build/bin

# Actual bounded target boots. All paths are explicit; no install/download occurs.
source /workspace/shared/stasha-toolchain/env.sh
python3 tests/kernel/k0/run.py --compiler ./bin/stasha-VERSION \
  --llvm-bin "$LLVM_BUILD/bin" \
  --qemu "$STASHA_TOOLCHAIN_ROOT/bootstrap/usr/bin/qemu-system-x86_64" \
  --qemu-machine pc-i440fx-10.0 \
  --qemu-data "$QEMU_FIRMWARE_DIR" --qemu-bios "$QEMU_BIOS" --require-boot
```

Defaults are O0/O2/O3, debug/nondebug, low/high addresses, two fresh emulator starts
per success/panic case, and a separate independently compiled C implementation.
`--opt 0 --debug no` is a narrower development run and cannot establish the full
matrix. Without `--qemu`, boot is explicitly `NOT_RUN`, never a skipped pass.
Hardware always remains `NOT_RUN`; `full_k0_readiness` is intentionally false.

Every invocation uses a unique directory under `build/k0-results`, snapshots the
sources, and retains exact commands, separate stdout/stderr, exit codes, timing,
compiler/source/tool hashes, IR, relocatable objects, archives, ELF images,
linker scripts/maps, executable disassembly, QEMU configuration, and machine-readable
`summary.json`. No stale output from another run is consulted. A `RUNNING` or
`FAIL` summary is not a passing result. Archive failures are retained and make the
overall run fail while independent object/boot work continues.

## Explicit object/IR-only lane

```sh
python3 tests/kernel/k0/run.py --objects-only \
  --compiler ./bin/stasha-VERSION --llvm-bin /path/to/pinned/llvm-build/bin
```

This additive mode emits only `obj` and `ir` for the unchanged component. It uses
one shared set of the full runner's IR, import, relocatable, DWARF and machine-code
checks. It never falls back from a failed full run. Omitting `--objects-only`
retains the full archive/host/image workflow described above.

The required inspection utilities are `opt`, `llvm-nm`, `llvm-readobj`, and
`llvm-objdump`, all explicitly inside the selected configured build's `bin`.
The unchanged `scripts/verify_llvm_toolchain.py` also requires the selected
`llvm-config`, CMake cache, clean pinned LLVM source and existing configured LLD
libraries. The runner does not provision or rebuild any prerequisite. Utility
restoration is a separate reviewed operation, limited to those four targets and
its approved dependency closure; there is no PATH fallback to another LLVM.

Each utility must be a regular, nonempty executable, resolve inside the selected
`bin`, and report exactly `LLVM version 23.0.0git`, without diagnostics. Its supplied
and resolved paths, symlink identity, mode, size, hash and full version output are
retained. These plain version banners do not attest the exact LLVM commit. For a
checkpoint claim, independently bind their pre/post SHA-256 values to a reviewed
exact-source/configuration/dependency/build receipt. The generic source/build
verifier and runner hashes are not reproducible-build attestation.

Default requested order is O0 nondebug/debug, O2 nondebug/debug, and O3
nondebug/debug. The current driver forces effective O0 for every `-g` command;
receipts retain both requested and effective optimization, including the actual
`-o=2 -g` and `-o=3 -g` arguments. Only nondebug O2/O3 establish the existing
three-helper import expectation. `--opt 2 --debug no` can pass that exact subset,
with `full_default_matrix=false`; it does not establish the default six variants.
Empty, duplicate, malformed and unsupported optimization lists are rejected before
creating output. All explicit QEMU/firmware/machine/boot-repeat options conflict
with this lane, including explicit values equal to full-mode defaults.

Permitted processes in this lane are the selected compiler's `obj`/`ir` emissions,
selected utility versions and inspections, Python's existing source/build verifier,
and disclosed Git/llvm-config identity reads. Inspections include
`opt -passes=verify -disable-output`, `llvm-nm --undefined-only`,
`llvm-readobj --file-headers --sections --relocations --symbols`, and
`llvm-objdump --no-debuginfod -dr`. No library/archive emission, host compiler,
assembler, linker, objcopy, executable, emulator, debugger or network operation is
part of component qualification. Verifier subprocess reads are listed in the
receipt; they read Git identities/status and llvm-config's version, targets,
assertion mode, RTTI and host target.

The optional `--source-tree TREE` resolves a real Git tree for the candidate-file
patch receipt; its default is local HEAD, which is reported separately. The patch
covers only this runner, its self-tests, this README and the readiness-status page.
It is not a reconstruction of unrelated untracked overlays. Selected K0 source and
current compiler-source hashes are recorded separately. A restored publication
with an older local HEAD needs an independently verified published-tree binding.
Current source identities do not claim that a runner-only edit rebuilt the frozen
compiler; its original build provenance remains separate.

A fresh unique run retains exact argv/cwd/timeouts, raw stdout/stderr, exits and
signal/timeout/spawn categories, fresh nonempty outputs, per-stage states, and
partial failure evidence. LLVM inspections must have valid non-vacuous structure,
not just zero exits or format words. The copied snapshot, original inputs, selected
tools, compiler, verifier, cache, source patch and inspected outputs are checked
again before success. A terminal receipt write failure returns failure. Mock
self-tests exercise these failure paths and the full default workflow without
running the actual full workflow.

A terminal `OBJECT/IR PASS` requires every requested variant and shared gate, plus
final identity checks and a retained summary. All excluded stages are explicitly
`NOT_RUN`: archive validation/member checks, prelink-negative fixtures, host
execution/ABI, memory-shim execution and runtime counters, missing-panic/helper
links, code-model negative links, low/high final images, boot, debugger and hardware.
`full_k0_readiness` stays false on success and failure. Debug section presence is
not debugger usability; this lane establishes no execution, ownership/lifetime,
boot or general kernel-readiness result and advances no K-level.

## Component and oracle

`component.sts` implements an allocation-free 64-slot resource bitmap and a
bounded buffer checksum. The bitmap is not a heap allocator: callers own one
properly aligned `u64` and the supplied byte buffer. All memory access has explicit
`unsafe` boundaries. There is no unqualified ownership/bounds-safety claim for raw
pointers. Integers use typed wrapping or checked operations deliberately.

`reference.c` compares 155,689 observations to independent C computations, with
fixed seed `0x1337cafedeadbeef` and final digest `5710ad21404bb36e`. It checks:

- 258 buffer lengths, 1,024 changing-buffer checksums, 64 slots plus exhaustion,
  and 2,048 deterministic take/release operations with invalid/double releases
- all i/u8/16/32/64 scalar C-to-Stasha argument/return boundaries, unsigned top
  bits, signed minimum division, valid checked multiplication, and valid shifts
- pointers round-tripped without dereferencing manufactured integer addresses
- eight arguments across SysV integer registers and stack, all six callee-saved
  registers, stack balance and a stack canary using external assembly
- initialized data and zero BSS, mutation of a global, and a full-width high-half
  address constant
- 64 fixed 1,025-byte typed aggregate copies: same pointer, forward/backward
  overlaps of 1, 17 and 1,024 bytes, and adjacent non-overlapping buffers; every
  surrounding byte is checked against an independent snapshot oracle
- eight aggregate zero-initializations at varied byte offsets, checking the
  complete buffer including guards, and 16 distinct-owned-aggregate copies with
  independently computed changing-input checksums

Host execution first runs `baseline.c` as an independently compiled C substitute,
then the actual Stasha object and the same component archive using its normal
`k0_component__*` module-qualified exports. Separate fresh processes verify the exact panic
ABI `void __stasha_panic(uint32_t)` for overflow (1), division by zero (2), signed
minimum divided by -1 (3), and invalid shift (4). An assembly panic wrapper checks
16-byte SysV call-site alignment (callee RSP mod 16 = 8) before tail-jumping into
C. The callback path is deliberately limited to the profile's panic hook; this
is not a general bidirectional arbitrary-external-function qualification.

The memory cases add 150,008 observations to the earlier 5,681-observation suite.
`memory_expectation.py` independently models their byte transformations and final
checksum. The C baseline uses loops and an explicit temporary byte snapshot,
including overlap cases; it does not rely on overlapping C struct assignment.
Only pointers and scalars cross the Stasha/C ABI. The aggregate is 1,025 bytes
with alignment one, so tested odd byte offsets are deliberate and valid.

## Machine and image policy

- Stasha `--kernel --target x86_64-unknown-none-elf`, static relocation and kernel
  code model; no red zone, implicit FP/SIMD, heap, TLS, unwind, async, atomics or
  volatile language operations
- Exact pinned LLVM 23 `opt`, object inspection, archiver and LLD, plus a verified
  source/build receipt checking the clean source revision against the repository
  gitlink, CMake cache, configured projects/targets and tool hashes; host GCC's exact
  executable hash/version is recorded for the C/assembly oracle and shim
- C uses `-ffreestanding -fno-builtin -fno-pie -mcmodel=kernel -mno-red-zone
  -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables
  -fno-unwind-tables -fno-tree-loop-distribute-patterns` with warnings treated as errors
- Static ELF64 payload at physical `0x200000`; low VMA `0x200000`, high VMA
  `0xffffffff80200000`; separate RX/R/RW load segments, no RWX segment
- Reviewed external LLD scripts with an explicit entry, retained startup/cookie,
  a 512-KiB image ceiling, 64-KiB stack and source-specific sections
- Every controlled relocatable input is audited before linking, including the
  Stasha object, extracted archive member, C/assembly objects, and the separate
  ELF32 bootstrap/payload wrapper. Constructor/destructor/preinit arrays, EH,
  TLS names/flags/symbol types and disguised initializer/unwind section types fail;
  ordinary relocation and debug sections are allowed. Twenty-six negative
  ELF32/ELF64 fixtures include an EH case that the linker would otherwise discard.
- No PT_INTERP, dynamic/TLS segments, runtime constructors/destructors, allocated
  relocations or unresolved final-image symbols
- Machine-code scans reject SIMD/FP registers/instructions, negative-RSP memory
  operands that might use a red zone, and FS/GS-relative TLS accesses
- Missing panic hook must fail final linking. An out-of-range intermediate VMA
  must fail the signed-32-bit relocations of the kernel code model.

The actual nondebug O2/O3 object and archive imports are exactly the panic hook plus
`memcpy`, `memmove` and `memset`. Typed aggregate assignment lowers to `memmove`,
zero initialization to `memset`, and copying between two distinct owned globals
to `memcpy`. O0 and this pinned compiler's `-g` path scalarize these same bounded operations
and import only panic:
**O0/debug are semantic execution evidence, not lowered-memory-hook coverage**.

`memory_helpers.c` supplies the same reviewed freestanding leaf implementations
for host and boot, with counters checked around every operation. Each optimized
success run must execute exactly 64 `memmove`, eight `memset`, and 16 `memcpy`
calls from these cases; scalarized O0/debug must execute none. The independently
compiled C baseline is exempt from the Stasha-specific lowering counts while
making the same semantic observations. The runner rejects shim imports, calls,
helper-directed tail jumps/relocations and machine-policy violations, guarding
against recursive compiler lowering. Removing one helper at a time must fail a
complete nondebug O2/O3 image link solely for that missing symbol. O0/debug have no missing-helper
link claim because those symbols are not imported there.

This is a bounded 1,025-byte operation qualification, not coverage of arbitrary
lengths, arbitrary helper callers, nonzero `memset` values, or every possible
aggregate lowering. The shim's counters are ordinary single-thread test state,
not atomic or volatile language support. O0 IR may retain unused hosted function
declarations: live calls and actual object/image imports define the boundary.

## Boot boundary

`boot32.S` is a small Multiboot-v1 protected-mode entry loaded by QEMU, with its own
GDT, zeroed page tables and long-mode transition. It maps the first GiB at both
identity and `0xffffffff80000000` aliases using 2-MiB pages. `entry64.S` deliberately
poisons then clears BSS, installs an aligned private stack/canary, and calls the C
harness. `boot.c` exclusively owns polling COM1 and the test-only debug-exit port.
These are external privileged operations, not evidence of Stasha volatile or
inline-assembly support. The test page tables are intentionally permissive;
segment permission audits do not claim page-level W^X enforcement.

The pinned provisioned emulator is Debian QEMU 10.0.13 (`1:10.0.13+ds-0+deb13u1`),
`pc-i440fx-10.0`, one `qemu64` CPU, 64 MiB RAM, single-thread TCG, and SeaBIOS
1.16.3-2. Binary and firmware hashes are recorded in each run. Private package
provisioning is outside this runner; no global installation or guest disk is used.

Each target boot emits exact `K0_START` and `K0_COMPLETE` or expected `K0_PANIC`
records with fresh run identity, compiler build SHA and an exact checksum. Extra
output, a wrong reason, missing sentinel, reset/triple fault, timeout, signal, or
wrong exit status fails. `isa-debug-exit` writes are decoded as `(value << 1) | 1`:
success is process status 3; the four panic cases are 67/69/71/73. `-no-reboot`
prevents retrying a reset. Do **not** use `-no-shutdown`: this QEMU version then
prints completion but stays alive, and the timeout correctly fails the run.
Each QEMU process has a 15-second wall-time limit and is group-killed on timeout.

## What remains outside this evidence

No interrupts, SMP, devices beyond the reviewed serial/exit shim, scheduler,
filesystem, dynamic allocator, arbitrary aggregate/variadic ABI, atomics, MMIO
semantics, real-hardware stress, GDB source stepping/backtrace session, or general
memory-safety proof. Repeated boot means independent fresh startup processes,
not hot-reset or interrupt-load testing. Debug variants emit/audit objects and
execute; debugger usability needs its own recorded session. P6 native driver
final-image linking remains separate from this explicit external LLD workflow.
