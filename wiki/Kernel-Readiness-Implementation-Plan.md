# Stasha: kernel-readiness implementation plan

Research snapshot: 10 October 2026. Scope: audit and implementation design only; no compiler fixes, commits, pushes, or kernel implementation were performed.

## Executive decision

Stasha has a substantial LLVM compiler, explicit storage and permission syntax, modules, C interoperability, tests, and an existing freestanding path. It is **not yet a dependable kernel implementation language**. The critical path is to make a smaller supported subset correct and observable, then prove that subset in a real freestanding component and boot harness. More surface-language features are not the immediate bottleneck.

Three priorities:

1. Establish a trustworthy build/test baseline and fix integer semantics, object layout/ABI, cleanup control flow, and incomplete safety enforcement.
2. Turn the existing freestanding support into an explicit, target-driven contract: no accidental hosted runtime, correct object/linker settings, auditable memory operations, predictable failure behavior.
3. Demonstrate useful kernel components under a boot harness, followed by interrupt/MMIO/SMP gates. Keep advanced hosted facilities unavailable in the kernel profile until separately qualified.

A boot message is a smoke test, not a kernel-readiness certificate. No boot or complete compiler execution was validated during this audit; the verification limits below are material.

## What “implementation description” means

It is the engineering contract for the work: the intended behavior, boundaries, architecture, changes, dependency order, tests, and exact evidence required to call it finished. It gives a human or a fresh coding assistant enough context to implement a bounded change without inventing missing requirements.

**Proposed implementation description:**

> Harden Stasha into a reproducible, testable LLVM-based language capable of implementing freestanding kernel components and, subsequently, a supported OS kernel target. Define and enforce a documented kernel language profile; repair scalar, memory, cleanup, layout, and ABI correctness; separate target configuration and freestanding runtime contracts from compiler-host behavior; add verified atomics, volatile/MMIO operations, assembly integration, and target-aware linking; then qualify the result through differential compiler tests, sanitizer/fuzz campaigns, object inspection, emulator integration, and selected real-hardware tests. Preserve hosted Stasha through regression tests and explicit compatibility/version boundaries.

No repository-specific `impl/IMPL_DESC` convention was found. Proposed repository home after authorization: `wiki/Kernel-Readiness-Implementation-Plan.md`, with bounded issue/PR descriptions referencing its task IDs; this fits the existing wiki documentation pattern. This standalone engineering Markdown is a proposed planning artifact, not an existing implementation specification. `CLAUDE.md`, `wiki/`, and `INCOMPLETE_FEATURES.md` exist but some descriptions are ahead of, or behind, the code.

## 1. Baseline and confidence

### 1.1 Source selection

Repository: https://github.com/Kayyo321/stasha

- Default `main`: `d60731a7d22ef7ba9ec59ebe631f4828af28af75`, 11 May 2026.
- `devel`: `87d7e443caea217572c799e5270ff275a69d6591`, 7 May 2026; verified ancestor of default main.
- Newest visible branch `stdlib-windows-portability`: **`7c0e6359a44d7d348e64623db5685d608a94453a`**, 29 May 2026. Main is its ancestor; it adds 12 commits, including driver, stdlib, and CI portability work.

The audit uses the latest descendant, pinned by SHA. This does not imply that this branch should be merged without review. Before implementing, the owner must select the landing branch; port the relevant existing work or use this descendant rather than silently assuming the old devel branch is current. All source links below use the audited SHA. The clone was kept separate from other projects and tracked files remain unchanged.

LLVM submodule: `348061d58dfe5e59027a0d6d51f916197cdd8d68`, whose version file reports **23.0.0git**. This is a pinned development revision, not a released LLVM 23 toolchain. [Version source](https://github.com/llvm/llvm-project/blob/348061d58dfe5e59027a0d6d51f916197cdd8d68/cmake/Modules/LLVMVersion.cmake). LLVM runtime libraries numbered 17/19 present in the audit environment are not a substitute for this revision's headers, APIs, LLD, and passes.

### 1.2 What was actually run

- Clean clone, branch ancestry/history, source/document/test/CI inspection; no `AGENTS.md` or project `.agents/skills` found.
- `make -j2`: **blocked**, exit 2, because `clang` is absent. The pinned LLVM submodule was not populated/built; `llvm-config`, development headers, CMake/Ninja, and QEMU were unavailable. A suggested historical tool directory was checked and absent. The full compiler, generated objects, existing semantic/ABI tests, performance, and boot path therefore remain **unexecuted**.
- A separate GCC harness compiled the repository's unmodified common, lexer, AST, preprocessor, and parser sources with AddressSanitizer and UndefinedBehaviorSanitizer. It invokes `pp_process`, `parse_from_stream`, and cleanup, without LLVM/codegen or driver import resolution.
- 147 existing `.sts` files from `tests/` and `examples/`: 134 accepted by parsing; 13 rejected; no sanitizer diagnostic in that corpus. These counts describe syntax-only execution and are not a pass/fail classification of the language's test suite. Some files intentionally contain errors or need driver behavior.
- Minimal valid-module repro: an unterminated `/*` comment returned an AST with zero errors (exit 0).
- Minimal valid-module repro: `stack u64 x = 18446744073709551615;` triggered UBSan signed integer overflow in `parse_expr.c:70`, while parsing returned an AST with zero errors. UBSan was in reporting mode, so exit 0 is not success for this case.
- Unmodified zone runtime with a small C harness: two 16-byte requests returned addresses congruent to 8 modulo 16 on this x86-64 environment. The API only aligns to 8 and does not accept requested alignment. This proves insufficient alignment for 16-byte objects in this run; it does not imply every current allocation requires 16-byte alignment.
- GCC native C-layout oracle independently measured ordinary `{u8,u64}` layout as size 16, alignment 8, second-field offset 8, versus packed size 9/alignment 1/offset 1; `va_list` size 24/alignment 8. It validates the reference ABI on this host, not Stasha emission.

LeakSanitizer could not operate under the execution environment's process tracing. Focused ASan/UBSan runs set `ASAN_OPTIONS=detect_leaks=0`; **no leak-free claim** is made.

Evidence includes harness source, logs, source revision, corpus results, and official references. Compiler-output reproductions are explicitly proposed until a pinned full toolchain runs them.

### 1.3 Evidence labels

- **RUN**: reproduced by the described executable harness, at the pinned source.
- **SOURCE**: the code path or missing call is directly visible; resulting machine-code behavior has not been executed here.
- **INFERENCE**: likely consequence requiring a full compiler regression test.
- **DESIGN**: recommendation or future acceptance criterion, not a current feature.
- **BLOCKED**: not verified because necessary tooling or a decision is unavailable.

## 2. Definition of done: graduated kernel support

### K0: qualified freestanding component subset

One selected 64-bit target and object format. Integer/bool operations, explicit casts, raw pointers with documented unsafe boundary, arrays, fixed-layout structs, direct functions, deterministic loops/control flow, explicit initialization, and a narrow scalar/pointer C ABI. Mandatory target machine policy already applies at K0: no red-zone where the kernel requires it, valid stack alignment, explicit CPU/features and code/relocation model, and no unsaved FP/SIMD state. No heap, TLS, unwinding, coroutines, thread pool, closures, dynamic slice growth, signals, implicit I/O, or dynamic initialization. Compiler must reject unavailable facilities with actionable diagnostics, including facilities imported transitively.

Qualification: build a nontrivial allocation-free component such as bitmap/physical-frame bookkeeping or a single-threaded/external-synchronization bounded ring buffer; concurrent mutation is forbidden unless separately qualified; compare a host test adapter against a reference model; link the same implementation into a trusted C/assembly boot harness. Its object must have only an explicitly allowed list of imports and no accidental runtime dependencies. Require data-layout, numeric, memory-access, and failure tests. If a safety feature cannot be enforced, restrict or label the unsafe interface; do not market syntax as a proof of memory safety.

### K1: single-core kernel-capable profile

K0 plus target-specific startup contract, static data/BSS initialization, panic/trap hook, aligned allocator interface if enabled, section placement/linker scripts, validated volatile/MMIO, inline assembly or external assembly boundary, and interrupts through reviewed assembly/C wrappers. Test timer interrupts while ordinary Stasha executes, stack/register preservation, failure paths, and repeated cold boots. Startup may remain small external assembly; self-hosting or rewriting the bootloader is unnecessary.

### K2: practical concurrent OS profile

K1 plus a specified memory model, lock-free atomic capability queries, all legal orders/fences/CAS, interrupt-safe allocation/locking policy, SMP startup and synchronization, per-CPU state or deliberately supported TLS model, concurrency stress/litmus evidence, and real-hardware qualification. Address-space and DMA/cache-coherency policies belong partly to the OS/hardware layer and must be explicit.

### Later scope

Additional architectures, aggregate FFI breadth, native interrupt functions, advanced generics/interfaces/closures, async schedulers, richer standard library, package registry, self-hosting, JIT, GPU support, and production security certification are independent projects. They do not block K0 if rejected or excluded clearly. They must not inherit the kernel-ready claim automatically.

## 3. Target and boot strategy

No OS architecture has been selected by the user. Recommend **x86-64 ELF as the first qualification target**, subject to that decision: X86 is already included in the configured LLVM backend set, there is a clear C ABI reference, and emulator/debugger tooling is mature. It still requires explicit no-red-zone, CPU feature/FP-SIMD policy, stack alignment, code model/relocations, and interrupt entry handling. Do not enable wide vector registers before the OS saves/restores their state. Test canonical high-half addresses without assuming all deployments use one mapping.

AArch64 ELF is an equally reasonable choice if the intended OS/hardware is ARM: require AAPCS64, exception-vector wrappers, stack alignment, explicit SIMD/FP handling, memory attributes/barriers, and no unplanned outline-atomic helpers. Existing by-value ABI xfail makes that area a known qualification obstacle.

RISC-V is a later candidate unless it is the actual OS target: add the LLVM backend (current Makefile builds X86 and AArch64), specify ISA extensions/ABI/relaxation/privilege environment, and validate that exact combination. Avoid simultaneous first-platform bring-up on three architectures.

Boot initially with a pinned external loader and a minimal separately tested assembly/C shim that supplies a stack, initializes memory, passes a versioned handoff structure, and calls an exported Stasha function. Select one documented protocol (e.g. a pinned Limine protocol release or an existing owner's loader) after target choice. QEMU `-kernel` is not a universal raw-ELF loader; use the machine and boot image appropriate to the chosen protocol. Do not claim `org: 0x7c00` makes the current 64-bit compiler a BIOS boot-sector compiler.

## 4. Findings driving the critical path

The backend appendix contains deeper exact source links. This list is prioritized by consequence, not feature count.

| ID | Evidence | Finding and implementation consequence |
|---|---|---|
| F01 | RUN | Integer token accumulation uses host signed `long`; maximum u64 invokes host overflow. Parse into width-independent checked/APInt representation, then range-check against contextual type. [parse_expr.c 56–71](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/parser/parse_expr.c#L56-L71) |
| F02 | SOURCE | Local integer literal emission starts at i32; generic widening sign-extends. Values beyond 32 bits and unsigned widening need typed lowering. Unsigned division/remainder/comparison/right shift use signed operations; checked arithmetic hardcodes signed i32. [cg_expr.c](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_expr.c#L588-L590), [operators](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_expr.c#L904-L985), [coercion](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_types.c#L239-L255) |
| F03 | SOURCE | `@c_layout` forces packed struct layout; type-level `@align` processing is empty. C layout, explicit packing and alignment must be distinct; changing layout is an ABI migration. [codegen.c 978–1091](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/codegen.c#L978-L1091) |
| F04 | SOURCE / existing xfail | Aggregate ABI is incomplete, with an explicit AArch64 by-value argument xfail. Use a target ABI classifier or a deliberately narrow first ABI; LLVM struct types alone are not a C ABI implementation. [xfail](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/tests/cinterop/xfail/04b_struct_byval_arg.sts) |
| F05 | SOURCE | `gen_ret` emits cleanup before evaluating ordinary/coroutine return values; break/continue branch directly without cleanup emission at those exits. Centralize CFG cleanup edges and preserve returned values before releasing ownership. [return](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_stmt.c#L585-L652), [loop exits](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_stmt.c#L1994-L2015) |
| F06 | SOURCE | Provenance, nullable dereference, narrowing, and @frees helpers are defined but have no call sites found across `src`; existing direct nil check does have a call site. Do not conflate these two observations. Safety requires typed CFG analysis, not merely wiring a few functions. [cg_safety.c](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_safety.c#L181-L331) |
| F07 | SOURCE | Slice read path has bounds checking; ordinary slice indexed assignment does not. One shared typed lvalue/access layer must govern loads, stores, compound ops and address-taking. [slice store](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_expr.c#L3837-L3847) |
| F08 | SOURCE | Freestanding skips linked runtime archives, but default crash constructor emission ignores that flag. Existing freestanding linker is host-selected and on macOS/Windows tolerates unresolved symbols. Missing runtime hooks must fail closed at final image link. [driver](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/main.c#L1807-L1835), [constructor](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/codegen.c#L2100-L2108), [linker](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/linker/linker.cpp#L190-L256) |
| F09 | SOURCE | TargetMachine/DataLayout already initialize early: preserve this. Other paths use raw nullable CLI triple or compiler-host preprocessor macros, so native/default and explicit cross-target behavior can differ. Normalize one TargetSpec and propagate it everywhere. [comptime selection](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/cg_stmt.c#L1714-L1734) |
| F10 | SOURCE | Atomics/volatile are only applied in selected access paths; dereference/member/indexed/assignment coverage, order validation, and hardware ordering need explicit contracts and IR tests. A volatile marker is not a synchronization primitive. See backend appendix. |
| F11 | RUN / SOURCE | Unterminated block comment is silently accepted. Parser/lexer resource limits and checked literals need sanitizer regression coverage. [lexer.c](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/lexer/lexer.c#L87-L107) |
| F12 | RUN / SOURCE | Zone allocator only rounds to 8; 16-byte requested objects were 8 modulo 16. Add requested alignment, checked size arithmetic, failure semantics and allocator contracts before kernel allocation. [zone_runtime.c](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/runtime/zone_runtime.c#L25-L65) |
| F13 | SOURCE | Diagnostic runner appends literal `f` to expected regex and hides compiler exit status with `|| true`. Positive C-interop runner can trust a stale existing executable and tolerates nonzero compiler status. Repair the oracle before treating green CI as correctness. [diagnostics](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/tests/diag/run_diag_tests.sh#L19-L59), [interop runner](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/tests/cinterop/run_cinterop_tests.sh#L23-L72) |
| F14 | SOURCE | Release workflow runs stdlib/interop/thread/LLVM smoke, but not every diagnostic/fileheader/signals/language suite; static LLVM cache key requires manual invalidation. Expand hermetic qualification; pin all build inputs. [workflow](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/.github/workflows/release.yml) |
| F15 | SOURCE | `org` is parsed/stored but no codegen/linker consumer found, despite documentation saying it controls image base. Reject unsupported settings or implement target/link-script semantics; never silently ignore kernel settings. [fileheader documentation](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/wiki/File-Headers.md) |
| F16 | SOURCE | Standard `memory` wrappers import C string/stdlib, and calloc multiplies sizes without an explicit overflow/failure check. Split freestanding `core`, optional allocator-backed layer, and hosted library. [memory.sts](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/stsstdlib/memory/memory.sts#L1-L52) |

| F17 | SOURCE | Global initializer handling defaults to null and only recognizes selected literal forms; unsupported expressions can silently retain zero initialization. Implement relocatable constants or reject unsupported initializers. Test global descriptor tables, symbol addresses, strings and aggregates. [codegen.c 1175–1193](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/codegen.c#L1175-L1193) |
| F18 | SOURCE | LLVM verifier failure records an error but can fall through to object emission; later optimization guards do not guard the emit call. Abort emission immediately and invalidate stale output on every failed compilation. [verification](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/codegen.c#L2123-L2130), [emission](https://github.com/Kayyo321/stasha/blob/7c0e6359a44d7d348e64623db5685d608a94453a/src/codegen/codegen.c#L2187-L2196) |

Useful existing foundations: early target DataLayout; object/archive pipeline; LLVM verification before optimization; debug information; freestanding metadata; linkage/section annotations; negative tests and C fixtures; macros/modules and extensive documentation. Preserve working behavior and make every claimed capability testable. `-o=1` currently selects an O2 middle-end pipeline, while machine optimization uses its own mapping; either implement true O1 or document/rename this behavior.

## 5. Architecture to converge on

### Front end and semantic contract

Use stages with explicit invariants: tokens with spans → syntax AST → resolved typed representation → typed CFG with ownership/effects → target-aware lowering → verified LLVM IR → optimized verified IR → object → audited final image. This can be introduced incrementally; a wholesale rewrite is not prerequisite. Stop semantic failures before invoking LLVM.

A resolved value must retain signedness, integer width, qualifiers, pointer depth/pointee, provenance/ownership mode, nullability, address space, alignment and ABI identity. A resolved lvalue must retain the same information through every access path. Existing `LLVMValueRef` plus LLVM integer type cannot distinguish signed and unsigned semantics.

Specify integer literal contextual typing, overflow modes, narrowing casts, shifts outside bit width, divide by zero, signed-minimum divided by -1, evaluation order/short circuiting, uninitialized reads, pointer/integer conversion, pointer arithmetic, enum validity, aggregate initialization and padding. Represent compile-time integers independently of compiler-host `long`; check resource use and deterministic evaluation. Do not allow undefined host C behavior to define the language.

### Memory and cleanup

Choose a tractable ownership model for the kernel subset: explicit borrowed spans/raw pointers at reviewed unsafe boundaries; no automatic broad memory-safety claim until CFG analysis supports branches, aliases, calls, loops, stores, captures and returns. Define which transfers copy, borrow or move and exactly who destroys each value. Analyze definite initialization and moves with control-flow joins. Use stable region identities rather than variable-name-only provenance.

Lower scope exits through reusable cleanup blocks with destination/depth information. Evaluate and protect return values first; transfer ownership; then unwind only exited scopes once in reverse order. Cover return/break/continue/switch/nested scopes and failures. Restrict destructor/cleanup effects in interrupt context. Keep unwinding disabled in the initial kernel profile; traps/panics terminate through a hook, never hosted `exit`/signal machinery.

### Target, layout and ABI

A single immutable TargetSpec should own effective normalized triple, architecture, OS/environment, object format, endianness, pointer/index widths, C type widths, CPU/features, ABI, relocation/code model, stack alignment, red-zone policy, FP/vector policy, atomics capabilities and sysroot/tool paths. Use it for preprocessing/comptime, header import, type layout, debug info, codegen, runtime selection and linker selection. Host execution properties belong in a separate HostTools object.

Compute layout through DataLayout, with explicit source-language representation rules. Treat C representation, packed representation, and alignment separately. Track tail padding, array stride, union maximum size/alignment, tag payload placement, zero-sized/empty representation if supported, and over-alignment. Access packed fields with correct alignment; reject misaligned atomics. Implement `sizeof`/`alignof`/`offsetof` with the same source of truth and compile-time tests.

Implement ABI classification shared by declarations, definitions, direct/indirect calls and C header imports, including sret/byval/inreg/extensions, attributes and stack/register passing. Initially reject unqualified aggregates/variadics at kernel ABI boundaries; scalar/pointer bridges keep the first component feasible. Later test both C→Stasha and Stasha→C for every supported aggregate class and `va_list`. Treat debug layout as part of this contract, not a separate guessed representation.

### Runtime and core library

Create a documented runtime dependency manifest per feature. Proposed tiers are `core` (no OS/allocator), `alloc` (explicit allocator interface), and hosted `std`. Names are design proposals, not current modules. `core` includes integer helpers, spans, fixed-capacity containers and memory primitives, with a no-allocation contract. Provide replaceable panic and allocation-failure hooks with fixed ABI; no default constructors, dynamic TLS, pthreads, exception unwinding, signals, or C I/O in kernel profile.

Some lowered operations can introduce `memcpy`, `memmove`, `memset`, division helpers, stack probes, atomics helpers, or other compiler-runtime calls. Decide each allowed helper deliberately; supply a verified freestanding implementation or reject the triggering operation. Test optimized output because helper calls can appear only after optimization. Statically link only needed pieces; an archive filename appearing on a command line alone does not prove all its objects are present.

### Machine boundary

Separate volatile access, language atomics, device ordering, and compiler barriers. Add typed width/alignment-correct MMIO load/store operations whose access count and lack of tearing are tested for supported targets; target-specific barriers/cache maintenance remain explicit. DMA requires memory attributes/coherency design, not just volatile pointers. No claim of C/C++ sequential consistency from isolated instruction selection.

Inline assembly needs validated target constraints, input/output/tied operands, clobbers including memory/flags, side-effect and stack rules, symbol references, and useful source diagnostics. Keep boot/interrupt entry in reviewed external assembly until this is reliable. Native naked/interrupt annotations are later work and must reject ordinary prologues/unsafe operations. Per-CPU storage starts explicit; TLS support follows a selected model and startup/relocation implementation, not a hosted default.

## 6. Executable roadmap and dependency order

Task IDs below are proposed work packages. Every package should be a reviewable change with regression tests, not a single giant branch. Complexity indicates relative risk, not a promised delivery date. Estimate time only after P0 establishes a working baseline.

### P0 — Pin baseline and make the test oracle trustworthy (first; medium)

Inputs: selected landing branch and toolchain policy. Files: Makefile, `.github/workflows/`, test scripts, compiler build metadata, documentation.

- Record source/submodule SHAs; build pinned LLVM+LLD in a clean documented image, or intentionally migrate to a supported release in a separate compatibility change. Pin host compiler, linker, CMake/Ninja, OS image and emulator versions/digests.
- Repair diagnostic regex/exit checks; run positive cases in fresh temporary directories; delete prior outputs before compilation; fail on nonzero compilation unless a narrowly identified temporary xfail explicitly covers it. Negative cases assert exit category and diagnostic location/text; distinguish rejection, crash, timeout and verifier failure.
- One test manifest enumerates every suite and supported target/profile, skip reason/owner/expiry, and expected outcome. Include diagnostics, syntax, fileheaders, signals, coroutines, memory, generics, core language, interop, stdlib, thread tests, LLVM smoke. Do not indiscriminately execute every `.sts` as a positive test.
- Fail closed immediately on LLVM verification errors; never attempt object emission after invalid IR. Write outputs transactionally and remove/invalidate stale expected outputs on compilation failure. Add compiler sanitizer configuration and emitted IR/object retention on failure. Never run both an unsafe stale binary and report fresh success.
- Key LLVM caches on gitlink, build options, host toolchain, target set and OS; pin workflow actions to audited immutable revisions. Separate read-only PR CI permissions from release publication.

Acceptance: clean build twice from manifest; logs report exact tool versions and SHA. Intentionally break a fixture, force compiler failure, leave a stale executable, and introduce a timeout; harness must detect all four. Existing suites have explicit pass/fail/xfail inventory. No warning is silently converted into a pass. This baseline may be red initially; publish the failures honestly.

### P1 — Specify and preserve typed scalar semantics (depends P0; high)

Files: lexer/parser AST, new semantic/type layer, cg_types/cg_expr/cg_stmt, compile-time evaluator, diagnostics.

- Fix open-comment diagnostics and integer parsing first; add width-independent integer constant representation and contextual literal typing.
- Implement typed arithmetic/coercion, signed/unsigned compare/divide/remainder/shift, checked/wrapping modes for each supported width; define cast and mixed-type rules rather than guessing from LLVM types.
- Define all exceptional arithmetic behavior before lowering; prevent poison/undefined operations on defined source programs. Do not introduce `nsw`, `nuw`, `inbounds`, `nonnull`, `noundef`, `noalias`, dereferenceability, or alignment promises without a proven source guarantee.
- Make constant evaluation and runtime evaluation agree; implement short-circuit semantics and deterministic target-aware compile-time selection. Validate optimization option behavior.

Acceptance: boundary matrix for i/u8/16/32/64, zero/sign/min/max, unsigned top bit, local/global literals, argument/return/cast paths, overflow and invalid shift/division. Compare reference arbitrary-precision semantics against emitted execution at `-o=0`, `-o=1`, `-o=2`, `-o=3`; trap tests use subprocesses and check the intended hook/reason. No sanitizer diagnostics parsing maximum valid literals. Valid high-half address constants round-trip without truncation. Include a case that previously used signed comparison for `u64`.

### P2 — Centralize target and representation (depends P0; parallel with P1; high)

Files: compiler driver configuration, fileheader/comptime evaluation, cg_types/registry/debug, C header importer, linker.

- Normalize TargetSpec before parsing target-conditional constructs; use it in every stage. No silent fallback to host C widths or platform macros for target behavior.
- Fix C layout vs packed, honor legal alignment, compute union/tagged payload size and stride from actual types. Diagnose unsupported representation.
- Add source-level size/alignment/offset query support if absent, with one authoritative implementation. Ensure native invocation equals explicit native triple.
- Implement target-correct relocatable constant initialization (symbol addresses, offsets, strings, descriptor/aggregate tables) or reject unsupported global initializers. Never substitute zero silently. Verify exact data bytes and relocation entries before boot.
- Establish ABI version/hash and rebuild all archived stdlib/dependent code after layout changes.

Acceptance: generated layout descriptors compare with Clang/GCC reference layouts for selected ABI; exhaustive small field combinations plus nested arrays/unions/tagged payload tests. Include packed and 16/32/64-byte alignment. Compare host→foreign target predefined conditions without executing foreign code. Reject unsupported triples, invalid alignments and unavailable backends. Debugger field offsets match actual bytes.

### P3 — CFG memory and cleanup correctness (depends P1 typed values; high)

Files: new semantic/CFG analysis, cg_safety, cg_dtors, cg_stmt, lvalue/access lowering, captured/lambda/zone paths if enabled.

- Specify definite initialization, ownership/borrowing, aliasing, move and lifetime rules for the supported profile; conservatively reject cases not proven safe.
- Repair return evaluation and every cleanup exit. Centralize lvalue load/store/index logic so permissions, nullability, bounds, alignment and access modes cannot disappear on another syntax path.
- Replace disconnected safety helpers with end-to-end analyses; test propagation/join behavior rather than merely counting calls.
- Until qualified, exclude closures/coroutines/signals and implicit heap effects from kernel profile. Audit aggregate copies/destructors and heap-return ownership in hosted mode as regressions.

Acceptance: deterministic trace of constructors/defer/destructors on normal exit, early return, nested break/continue, switch, return-expression call and branch joins; each cleanup exactly once. Returned heap-derived value evaluated before release. Compile-fail tests for bad lifetime/move/nullability/permissions/initialization, including aliases, members, calls and loops. Bounds tests for reads AND writes AND compound assignments; sanitizer-backed hosted execution for permitted programs. Unsafe blocks suppress only documented safety rules, not type/layout/IR validity.

### P4 — Qualified ABI and object emission (depends P1+P2; high)

Files: ABI lowering, fn declarations/calls/function pointers/header imports, driver emission modes, debug info.

- Establish the K0 machine policy here: kernel red-zone setting, stack alignment, selected CPU/features, code/relocation model and allowed register/FP/SIMD usage; verify disassembly before any kernel component executes. P7 extends machine-facing facilities, not these baseline prerequisites.

- Establish narrow scalar/pointer kernel C ABI first; explicitly reject unsupported aggregate/variadic interfaces in the profile.
- Add stable object-only and IR-only emission with specified outputs; preserve original archive workflow. Current `-o=N` is the optimization syntax; any new `-O` alias is a separate compatibility decision.
- Implement remaining selected-target C ABI classes using authoritative ABI rules and compiler oracle; unify all call forms. Close AArch64 xfail only with actual bidirectional execution on that target.
- Make `va_list` representation target-driven; validate default promotions and `va_copy`/`va_end`, or reject variadic definitions initially.

Acceptance: bidirectional C/Stasha ABI matrix at all optimization levels, including integer extension, floating point if allowed, mixed/nested aggregates, large/small returns, callbacks, alignment, variadics, and external assembly probes of registers/stack. K0 gate needs only its declared narrow matrix, with unsupported cases rejected. `nm`/`readelf`/`objdump` metadata and symbols are reproducible; source paths can be remapped for stable debug output.

### P5 — Enforce freestanding profile and runtime contract (depends P1+P2+P4; medium-high)

Files: driver imports/link selection, codegen builtins/lifecycle, runtime headers, new core modules, stdlib categorization.

- Harden the existing freestanding flag: disable implicit crash installation, all hosted constructors/runtime effects and inappropriate builtins; transitive imports must respect profile capability requirements.
- Add no-allocation/no-TLS/no-unwind/no-FP-SIMD options/effects as selected profile requires; reject unsupported behavior rather than falling back to host services.
- Separate core/alloc/hosted modules and define panic/failure/memory helper ABI. Check generated undefined symbols after optimization; selected permitted helper set is a versioned manifest.
- Allocation remains disabled for K0. Before enabling alloc: size-overflow checks, requested alignment, zero-size semantics, OOM behavior, realloc failure/ownership semantics, allocator context, and interrupt/reentrancy restrictions. Qualify zone allocator separately.

Acceptance: compile a fixed-capacity data structure with zero allocations and link it into a freestanding harness; no libc/pthread/crash/TLS/unwind/dynamic-loader imports. Importing a forbidden module produces a clear source diagnostic. Deliberately omit panic/memory helper and final link fails. OOM/over-alignment tests for optional alloc tier. Object inspection must catch optimization-introduced libcalls, not merely source-level calls.

### P6 — Target-driven kernel image linking (depends P2+P4+P5; medium)

Files: linker API/driver, target profile, build/project configuration, linker scripts and image audit tools.

- Link based on target object format, never compile-time host `#ifdef` selection. Accept explicit entry, linker script, map output, relocation/code model, CPU/features, sections/retention and BSS policy through structured arguments.
- Implement documented image-address option semantics or reject `org`; define virtual vs physical load addresses. Keep relocatable object emission separate from final-image linking and raw-binary conversion.
- Final images fail on unresolved symbols, unexpected dynamic segments/dependencies/relocations, unsupported TLS or unwind metadata. Define whether constructors are forbidden or explicitly invoked; initial kernel profile forbids them.
- Support garbage collection/retention of critical boot sections deliberately; test symbol visibility, weak overrides, exported names and dead stripping.

Acceptance: ELF headers/program segments/entry/section alignment/permissions/load mappings match manifest; map file accounts for all retained code/data. No PT_INTERP/DT_NEEDED in static freestanding image. All relocations fit chosen model. Test low-address and selected high-half layouts. Cross-host emission produces target-format output or a clear unsupported diagnostic, never an accidental host binary.

### P7 — Validate machine-facing memory and assembly (depends P1+P2+P3+P5; high)

Files: typed access lowering, atomic builtins, inline assembly parser/lowering, target runtime shim.

- Specify the base data-race and atomic memory model before implementing it, including interaction with interrupts and externally shared memory. Implement atomic load/store/RMW/CAS/fence orders with legality/alignment checks and lock-free capability query; prohibit hidden locking/helper fallback in contexts that require lock-free operations.
- Cover volatile/member/index/deref/store paths consistently. Add explicit MMIO width/alignment/access APIs and architecture barrier hooks. Specify device vs normal-memory assumptions; preserve access side effects under optimization.
- Validate assembly constraints/clobbers/outputs and target dialect; external assembly remains the initial privileged boundary. Explicitly set/verify red-zone, stack, FP/vector and code-model policies.

Acceptance: FileCheck-like IR tests for exact order/volatile/alignment and disassembly checks for target instructions; fake-device access-log tests for count/order/width; negative invalid-order/alignment tests. Atomic host stress plus target litmus tests; a test passing on strongly ordered x86 does not qualify ARM barriers. Interrupt wrappers preserve all state that interrupted qualified code can use, including caller-saved registers: an ordinary function-call ABI is insufficient for asynchronous entry. Validate flags/direction state, stack alignment/switching, architectural exception return state, and any permitted vector/FP state; never use an uninitialized stack or unsaved vector state. No lock-free claim without inspecting actual lowered code.

### P8 — Prove K0 and K1 in a boot harness (depends P3+P4+P5+P6; P7 for K1; high integration)

- Pin selected boot protocol/loader, QEMU machine/CPU/firmware, assembly shim and linker script. Keep harness small and independently tested.
- Run the same bounded component under host reference tests and target boot tests. Verify BSS/data, stack alignment, call ABI, arithmetic edge cases, section retention, panic path and clean termination report.
- K1 adds timer interrupts during computation, volatile device access, stack/register canaries, allocation if enabled, and failure injection. Do not start with disk drivers, filesystems or a scheduler; those obscure compiler failures.
- Use structured serial records with test ID/build SHA/checksum and explicit completion sentinel. A missing sentinel, unexpected reset, triple fault, timeout, wrong status or extra panic is failure. Do not accept a substring from an earlier boot.

Acceptance: reproducible image passes all selected-target boot tests at O0/O2/O3 and debug/nondebug; retain serial logs, linker map, IR/object and QEMU config. QEMU exit conventions are machine-specific (e.g. x86 isa-debug-exit mapping) and must be decoded, not assumed to be normal exit 0. Repeat reset/startup runs and interrupt load; GDB source stepping and backtrace for ordinary frames are usable. Then perform selected real-hardware smoke/stress tests because emulation cannot validate all device ordering/cache behavior.

### P9 — K2 concurrency and per-CPU qualification (depends P7+P8; high)

- Extend the P7 memory model to OS scheduling, ownership across threads/interrupts and lock ordering. Bring up a small SMP harness with explicit per-CPU data before generic TLS.
- Implement TLS only with chosen ABI/model, storage initialization and relocations; reject all other models. Audit hidden runtime helpers and scheduler assumptions.
- Test contention, acquire/release publication, CAS success/failure, fences, interrupts during lock ownership, CPU startup/shutdown and allocation policy. Never sleep/block in forbidden interrupt contexts.

Acceptance: model/litmus analysis establishes allowed outcomes; repeated bounded emulator and selected-hardware stress must not observe forbidden outcomes. Failure to observe an outcome is not proof of impossibility. Require sustained lock-free/lock stress with invariant checks and timeout diagnostics. TSan can help hosted runtime tests but does not validate device accesses or all freestanding behavior. Kernel-level concurrency safety needs actual target evidence.

### P10 — Continuous reliability/performance qualification (begins P0; gates all releases)

- Add property/differential/metamorphic generators for the defined subset; compare with a reference interpreter and restricted, UB-free C oracle where semantics coincide. Compare O0/O2/O3 and backend/host combinations. Do not interpret agreement between optimizations as independent proof.
- Fuzz lexer/preprocessor/parser/semantic analysis/IR verification/module graph and supported ABI lowering; enforce timeout/memory/input-depth limits, minimize findings, save seeds and regression cases. Run compiler ASan/UBSan; add leak testing in a compatible environment; separately sanitizer-test generated hosted programs and C runtime.
- Verify IR before and after transformation stages; use verify-each in CI independent of debug-info mode. Preserve failing pre/post IR and exact pipeline/tool version. Check optimization/error cleanup paths.
- Benchmark compile wall time/peak RSS, object/text size, runtime throughput/latency and stack use on representative kernel algorithms against same-target C built with the same LLVM generation. Control CPU/features/options, warmup, variance and semantic equivalence; measure allocation counts and worst-case behavior where relevant.
- Establish baseline distributions first. Proposed initial review triggers: statistically credible >10% runtime or text-size regression, >15% compile-time/RSS regression, or any new allocation/runtime helper/stack policy violation; owner may tune numeric thresholds after measurement. Correctness and fixed resource limits are hard gates regardless of speed.

Acceptance: no unresolved wrong-code issue in the qualified subset; all sanitizer findings triaged; every crash/miscompile has a minimized regression; budgets documented by target/workload. Scheduled long campaigns supplement fast per-PR tests. Coverage percentages, fuzz hours and a boot demo are evidence, never a proof of absence of bugs.

### P11 — Version, document and release the qualified contract (depends completed selected K-level)

- Publish exact supported language/profile/target matrix and excluded features; update README/wiki claims to verified evidence.
- Assign compiler/runtime/core/ABI versions; invalidate incompatible caches and artifacts. Never silently link old packed-layout archives against corrected layout.
- Release reproducible binaries with checksums/provenance/dependency inventory and supported bootstrap source/toolchain. Keep the C bootstrap compiler; self-hosting can later add trust-chain verification, not replace qualification.
- User-facing diagnostics explain unsupported kernel features and how to move code behind approved hooks. Examples contain executable tests and match actual command syntax.

Acceptance: a fresh contributor can build, run the full qualified test matrix, produce and inspect a boot image from documentation, and reproduce the reference result. Two independent clean builds match stable outputs or have explained nondeterministic debug metadata. The release explicitly states K0/K1/K2 scope rather than claiming every feature is kernel-safe.

### Dependency DAG and earliest useful milestone

P0 → P1 and P2 in parallel.
P1 → P3; P1+P2 → P4; P4+P2 → P5 → P6.
P3+P4+P5+P6 → P8(K0).
P1+P2+P3+P5 → P7; P7+P8(K0) → P8(K1) → P9(K2).
P10 runs throughout; P11 follows the selected qualification level.

The **earliest useful OS contribution** is K0, not “all language features complete”: a tested, allocation-free component behind scalar/pointer ABI, called by a small C/assembly kernel harness. Do not defer numeric/layout/memory correctness to achieve an earlier boot screenshot. For each work package, land tests plus one semantic change and keep hosted regressions running.

## 7. Single-main consolidation and high-level language preservation

The owner's latest requirement is one authoritative **main** branch at the end, retaining Stasha's higher-level ergonomics and low-level control while making it excel at OS development. This plan is not a permanent kernel fork or a replacement of hosted Stasha with a tiny language.

Before implementation, inventory all three remote branches by ancestry and content. Current evidence: devel is historical, and the portability branch contains main plus 12 commits. Review that delta as the first consolidation candidate; identify behavior/API changes, dependency changes, tests and any unmerged commits rather than assuming the newest branch is entirely correct. Build/test the consolidated candidate and keep recoverable commit references. Land reviewed work into main in bounded changes. Temporary review branches are compatible with one final authoritative main; no enduring hosted/kernel branch split is proposed.

Only after integration and verification should obsolete remote branches be removed, under explicit authorization. No merge, push, default-branch change or deletion happened during this planning task. Tag known baselines and document ABI-breaking releases so users can reproduce older artifacts. Branch removal must not be used as a substitute for resolving tests or preserving useful history.

Hosted and kernel profiles share parser, type system, semantics, optimizer, debug information, core libraries and test infrastructure. Keep ergonomic features (methods, generics, modules, slices, tests, pattern matching, RAII/defer, and the existing higher-level facilities) and qualify their behavior systematically. Dynamic allocation, closures, async, thread pools, I/O and signals can remain useful hosted features while being rejected or explicitly adapted in freestanding contexts. A profile restriction is a stated runtime/effect boundary, not removal of the feature from the language.

The owner asked to fix parser and logic issues. Convert that into measurable work: every known defect in the qualified scope receives a regression test and fix; all parser/semantic/codegen paths receive structured tests and fuzzing; remaining issues are tracked, scoped and disclosed. It is not credible to promise that any nontrivial compiler has zero undiscovered bugs. The release gate is no unresolved correctness defect in the declared qualified subset plus strong continuous evidence across the whole language.

## 8. Test commands, artifacts and concrete regression checklist

### Existing commands to reproduce after provisioning the pinned toolchain

These commands already correspond to repository behavior. Run from a fresh checkout, with dependencies available and adequate build resources. Building the LLVM submodule is substantial and was not completed here.

```sh
git checkout 7c0e6359a44d7d348e64623db5685d608a94453a
git submodule update --init --recursive
make llvm
make -j2
make stdlib
make test-cinterop
make test-threads
make test-llvm-smoke
bash tests/diag/run_diag_tests.sh ./bin/stasha
```

The diagnostic runner and positive interop oracle need P0 fixes before their results can be trusted. `make stdlib` includes hosted tests and bundled external libraries; it is not the minimal kernel-core build. Do not run install scripts as a substitute for a provenance-verified development toolchain.

Existing IR inspection controls:

```sh
STS_DUMP_IR_PRE=/tmp/case.pre.ll STS_DUMP_IR=/tmp/case.post.ll \
  ./bin/stasha build case.sts -o=0 -o /tmp/case
```

Repeat in independent temporary directories at `-o=1`, `-o=2`, `-o=3`; distinguish compiler exit, linker exit and generated-program exit. For deliberately freestanding cases, do not run the resulting image as a host application. The planned object-only flag and target-profile commands below do not exist merely because they are specified here.

### Commands proposed for implementation

Names are interface proposals; P0/P4/P6 should either implement these scripts or document equivalent commands. They are **not runnable acceptance evidence today**.

```sh
python3 tools/test.py --suite all --manifest tests/manifest.json --clean
python3 tools/test.py --suite semantics --opt 0,1,2,3 --sanitize-compiler
python3 tools/abi_check.py --profile targets/kernel-x86_64.json --both-directions
python3 tools/audit_object.py --manifest targets/kernel-x86_64.imports.json component.o
python3 tools/build_kernel_harness.py --profile targets/kernel-x86_64.json
python3 tools/run_kernel_tests.py --profile targets/kernel-x86_64.json --timeout 60
python3 tools/benchmark.py --baseline reference.json --output candidate.json
```

A profile filename here is illustrative until the OS architecture is chosen. Each runner must emit machine-readable results and exact commands, environment/tool hashes, elapsed time, compiler/target exit codes, expected/actual values, and retained failure artifacts. CI exit must reflect failure even when log capture uses pipelines.

### Minimum issue-linked regression corpus

- Parse: open block comment; invalid escapes/numeric bases; maximum valid/one-too-large literals; deeply nested syntax and macros; malformed UTF-8 if accepted input is defined as UTF-8; error recovery with correct spans; imports/cycles/duplicate definitions; deterministic macro expansion/resource limits.
- Numerics: u8 255 widened to u64 remains 255; u64 top bit compares above 1; local `0x100000000` remains 4294967296; unsigned right shift fills with zero; each signed/unsigned checked operation uses its width; zero/minimum division and remainder trigger specified outcomes; branch short-circuit prevents evaluation of the inactive expression.
- Flow/lifetime: return expression observes pre-cleanup value; each heap owner released once; break/continue exit only their scopes; moves through aggregate/call/return and branch joins; borrowed data cannot outlive owner; guarded nullability joins are sound; uninitialized reads rejected.
- Memory access: slice boundary read/write/compound update; nested member/index paths; read-only/write-only permissions; packed/unaligned access; known-size pointer bounds where promised; unsafe access retains documented semantics.
- Layout/ABI: `{u8,u64}`, nested struct/array, packed and over-aligned types, tagged payload larger than 16 bytes, recursive/pointer-containing types, scalar extensions, callbacks, aggregate return/argument and `va_list`; compare layouts and call results against target C oracle.
- Freestanding: simplest function has no crash constructor; no unexpected imports at O0/O3; forbidden high-level runtime effect diagnosed; memory intrinsic helpers supplied deliberately; missing hook fails final link; selected target dictates branches/object/linker.
- Machine boundary: volatile store and read on pointer/member/index, exact device access widths/counts, atomic order/CAS failure-order rejection, lock-free lowering and helper detection, assembly clobber/register preservation, no red-zone use, no unsaved FP/vector use, stack alignment and high-half relocations.
- Harness: expected serial sequence, controlled panic, timer interrupt, counter integrity, reset loop, stack canaries, no unexpected reboot, defined timeout/exit policy; SMP added only at K2.

### Reproduction sources from this audit

Frontend probe build (from repository root, probe source supplied in evidence):

```sh
gcc -std=gnu2x -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc frontend_probe.c src/common/common.c src/lexer/lexer.c src/ast/ast.c \
  src/parser/parser.c src/preprocessor/preprocessor.c -o frontend_probe
ASAN_OPTIONS=detect_leaks=0 ./frontend_probe open-comment.sts
ASAN_OPTIONS=detect_leaks=0 ./frontend_probe u64-max.sts
```

`open-comment.sts` contains:

```stasha
mod audit;
fn main(void): i32 { ret 0; } /* unterminated
```

`u64-max.sts` contains:

```stasha
mod audit;
fn main(void): i32 { stack u64 x = 18446744073709551615; ret 0; }
```

These are parser/preprocessor tests only. Evidence harness paths are relative to the extracted evidence package in the commands above; adjust the probe source location accordingly. They are not proposed production compiler changes.

## 9. Risks, migrations and stopping rules

- **ABI breaks:** corrected packing/alignment/tagged unions/argument lowering can break every dependent archive. Version the ABI; rebuild all artifacts; compare symbol/layout manifests; reject incompatible cached modules.
- **Safety design scope:** full interprocedural borrowing plus closures/concurrency is large. A conservative subset with explicit unsafe interfaces is safer than optimistic incomplete enforcement. Keep high-level features and expand qualification in stages.
- **LLVM changes:** project uses development LLVM. Pin semantics/docs and test IR under every upgrade; never infer target-independent safety from current web docs. Upgrade in its own change with bisectable corpus.
- **Performance pressure:** fix wrong code first. Do not disable checks globally to win benchmarks; document checked versus unsafe profile results separately.
- **Host/target leakage:** native builds can hide target bugs. Native-vs-explicit-native equivalence and cross-host object tests are release requirements.
- **Hidden allocation and runtime dependencies:** audit both source effects and optimized objects; no blanket “no hidden allocations” claim from syntax alone.
- **Test oracle weakness:** green results from stale binaries or ignored exit statuses are invalid; P0 must precede feature confidence.
- **Boot debugging cost:** use stable trusted external startup and a narrow ABI boundary, then add one kernel capability at a time. Avoid simultaneous compiler+bootloader+MMU+scheduler invention.
- **Operational claims:** no date estimate or “all bugs fixed” promise before baseline. Stop/ask when target/ABI/boot protocol choices would lock in incompatible user intent, or when publication/merge/deletion is needed.

Decisions needed before implementation, bundled rather than blocking this research: (1) first OS architecture and intended hardware; (2) boot protocol/current loader if any; (3) pinned LLVM development baseline versus a tested release migration; (4) acceptance of ABI-breaking corrections and rebuild; (5) authorization for the consolidation/implementation work and eventual branch cleanup. Recommend x86-64 ELF, a pinned external bootloader, stable explicit kernel ABI, and staged mainline changes unless the OS goals point elsewhere.

## 10. Source policy and primary references

Repository claims above are pinned to Stasha SHA `7c0e6359a44d7d348e64623db5685d608a94453a`. The detailed backend audit and source manifest are in the accompanying evidence. External sources were checked on 10 October 2026. Current web documentation is orientation; pinned LLVM source defines this compiler dependency's contract.

- [Pinned LLVM Language Reference](https://github.com/llvm/llvm-project/blob/348061d58dfe5e59027a0d6d51f916197cdd8d68/llvm/docs/LangRef.rst): poison/undef, integer operations, volatile/atomics, calling conventions, attributes, inline assembly and data layout. Pinned LangRef SHA256: `805edda90fd88d958098b38b7d6d58f43457ff04a4cb0c3fe50ea3bad5e89262`.
- [Current LLVM LangRef](https://llvm.org/docs/LangRef.html) and [undefined-behavior manual](https://llvm.org/docs/UndefinedBehavior.html): useful context, but not interchangeable with the pinned revision.
- [Clang toolchain documentation](https://clang.llvm.org/docs/Toolchain.html): compiler runtime and library dependencies.
- [LLD ELF linker](https://lld.llvm.org/ELF/index.html) and [linker scripts](https://lld.llvm.org/ELF/linker_script.html): final linking/section and script behavior; verify against the bundled LLD revision.
- [x86-64 psABI source](https://gitlab.com/x86-psABIs/x86-64-ABI): ABI classification, registers, stack and red-zone contract; pin a revision for implementation.
- [Arm ABI specifications](https://github.com/ARM-software/abi-aa) and [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst): target ABI rules, with immutable revision selected during implementation.
- [RISC-V psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/): target alternative; the inspected current page identifies a development pre-release, so pin a revision if selected.
- [QEMU system invocation](https://www.qemu.org/docs/master/system/invocation.html), [GDB usage](https://www.qemu.org/docs/master/system/gdb.html), and [QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html): emulator execution/control/debugging. Pin executable version and machine/CPU separately.
- [LLVM testing infrastructure](https://llvm.org/docs/TestingGuide.html), [libFuzzer](https://llvm.org/docs/LibFuzzer.html), [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html), and [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html): compiler validation tooling, with the caveat that dynamic tests do not prove semantic correctness.

A concrete documentation drift was checked: the pinned LangRef permits assuming execution continues after a volatile load (with a specific volatile-store exception), whereas current LangRef has different volatile trapping/continuation wording. This is why faulting-MMIO behavior must follow the selected LLVM version and target contract, not folklore.

## Final completion standard

The planning task is complete when this roadmap and evidence are reviewed. Implementation is complete only for a specifically declared K-level/target/profile after its gates pass. The intended end state is one authoritative main branch, a pleasant and capable hosted language, a rigorously specified low-level kernel profile, and tests that make compiler correctness and performance visible. Nothing in this audit certifies the current compiler or any generated kernel as production-ready.
