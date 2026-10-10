# P2 representation checkpoint

This checkpoint changes object representation and requires rebuilding every
Stasha archive, including the archived standard library. Old/new objects must
not be mixed. It does **not** qualify the C ABI for aggregate arguments or
returns. The full-compiler runtime oracle uses only scalar/pointer interfaces.

## Representation rules

- Ordinary and `@c_layout` structs use target LLVM DataLayout: natural member
  alignment, padding and tail padding. `@c_layout` no longer means packed.
- `@packed` requests byte-aligned layout. Ordinary loads/stores are conservatively
  emitted with `align 1` before optimization, including raw-pointer and nested
  packed paths. LLVM may infer stronger alignment from actual allocations.
  This deliberately trades possible performance for soundness until provenance
  and guaranteed alignment are tracked on every lvalue. Atomics are unchanged;
  packed atomic fields are rejected. This is not an atomic/MMIO qualification.
- Union storage keeps both the maximum actual member size and alignment, including
  all array dimensions and nested aggregates. The LLVM representation uses an
  alignment anchor plus bytes; its rounded size is the array stride.
- Tagged enums and `any.[...]` put that same aligned storage after an i32 tag.
  Large payloads no longer use an assumed eight-byte fallback.
- Struct/union/enum by-value dependencies are completed before querying layout.
  Forward references and pointer recursion are supported. By-value cycles,
  unknown/unsized fields and objects exceeding target addressability are errors.
- Generic structs/unions use the same layout path, including arrays and packing.
- `@align(N)` must be positive and a power of two. It is a minimum alignment
  assertion: requests at or below natural ABI alignment are already satisfied.
  Greater alignment is **explicitly unsupported**, including normal integer
  structs requesting 16/32/64 bytes. Merely aligning allocations would leave
  nested fields, array stride and ABI wrong. Full over-alignment remains open.
- Source bitfields are rejected with an explicit diagnostic. Previous grouping
  and direct-access code did not implement nested/self/compound/address-of paths
  correctly. Use ordinary unsigned integer fields and explicit masks. Imported
  C bitfields remain rejected. No target-specific C bitfield ABI is claimed.
- Fixed-size field arrays require positive literal extents. Named/unsized/zero
  extents are diagnosed rather than silently becoming zero-sized fields.
- Representation attributes on aliases, interfaces and enums are rejected rather
  than ignored.
- Debug composite size/member offsets use the same target layout. Array metadata
  includes complete dimensions; tagged enum payloads are a real debug union;
  pointer widths/alignments are target-derived; recursive composites are fixed up.

`cg_layout_query` is the common size/alignment/direct-member-offset service.
Existing `sizeof` uses it. Source `alignof`/`offsetof` syntax is still absent and
is intentionally deferred to a separately validated extension. P2 acceptance is
therefore **incomplete**, even if every test in this directory passes.

## Tests

Run with the pinned toolchain produced by the project's build manifest:

```sh
LLVM_CONFIG=/path/to/pinned/build/bin/llvm-config tests/layout/run_oracle.sh
python3 tests/layout/run_layout_tests.py --compiler bin/stasha
```

The independent C/DataLayout oracle builds the **production** union-storage
primitive. It compares size, alignment, field offsets and three-element strides
for all 49 pairs of u8/u16/u32/u64/f32/f64/pointer under natural, packed and union
representations, then large nested arrays/unions/tagged storage. C additionally
verifies 16/32/64-byte over-aligned object strides. These are 721 assertions.

The full-compiler runner generates 72 natural/packed scalar-pair cases and
nested, forward, recursive-pointer, generic-array, union, large tagged payload,
self/compound/nested packed access cases. It emits fresh object and debug IR
artifacts at O0/O2, verifies unoptimized access alignment and debug member
offsets, links a C scalar/pointer oracle and executes it. Fourteen negative
layouts must produce clean diagnostic exits and remove deliberately seeded stale
objects. Crashes, timeouts, missing artifacts and linker errors fail the test.
The default runtime target is x86-64 Linux; foreign-object execution is not
supported by this runner.

Generate sources without a compiler for syntax-only inspection:

```sh
python3 tests/layout/run_layout_tests.py --generate-only /tmp/layout-sources
```

A labelled **secondary** oracle can be run while LLVM 23 builds:

```sh
LLVM_BUILD_INCLUDE=/path/to/pinned/build/include \
  tests/layout/run_oracle.sh --secondary-llvm19
```

That option links only the small representation oracle against the installed
LLVM 19 C API. It does not build Stasha, substitute a supported compiler toolchain,
or count as pinned LLVM 23/full-compiler qualification.

## Evidence at this checkpoint

- GCC syntax-only check of the complete codegen translation unit against the
  pinned LLVM 23 source and generated headers: passed.
- Secondary LLVM 19/C representation oracle, UBSan: **721 assertions passed** on
  x86_64-pc-linux-gnu.
- Generated Stasha matrix: actual repository parser accepted it without errors.
- Generated C runtime oracle: `-std=c11 -Wall -Wextra -Werror` syntax check passed.
- Pinned LLVM 23 oracle, emitted-IR/debug checks and full-compiler runtime/negative
  tests: **pending the pinned toolchain build**, not reported as passing.
