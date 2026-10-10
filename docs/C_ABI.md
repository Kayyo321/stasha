# C call ABI: x86-64 GNU/Linux LP64

Object representation and calling convention are independent contracts.
`cg_layout.c` supplies sized LLVM types, field offsets, size and alignment from
LLVM's selected target DataLayout. `cg_abi.c` separately classifies those source
records for the C calling convention. `@c_layout` remains naturally aligned;
it does not mean `@packed` and does not, by itself, select a function ABI.

## Selecting the ABI

- Functions imported by `cheader` use their declared C ABI.
- A source declaration or definition uses `@[[abi: c]]` to select C linkage and
  C calling convention. This applies to both calls and exported definitions.
- Ordinary Stasha functions retain the native language convention. `ext` alone
  controls visibility, not aggregate C argument passing.
- `stack let pointer = c_function;` (also `&c_function`) preserves the source
  signature and C calling convention for indirect calls. Converting an aggregate, narrow-scalar or variadic
  C function to an unqualified native `fn*` is rejected, including explicit
  casts: an LLVM opaque pointer does not prove calling-convention compatibility.
- C library calls inferred from argument LLVM types cannot establish an
  aggregate signature. Declare a C prototype instead of guessing one.

The qualified aggregate target selector is exactly x86-64 ELF GNU/Linux LP64,
including normalized vendor variants and an explicit ELF object-format suffix.
Other OS/environment/architecture combinations and x32 reject aggregate C
signatures. Scalar/pointer-only signatures retain their existing behavior.
This is not an AAPCS, Windows, Darwin, musl, BSD or freestanding C-ABI claim.

## Classification and lowering

The implementation follows System V AMD64 psABI section 3.2.3, cross-checked
against the repository's pinned LLVM/Clang 23 source
(`348061d58dfe5e59027a0d6d51f916197cdd8d68`,
`clang/lib/CodeGen/Targets/X86.cpp`, `X86_64ABIInfo`).

Supported record members are integer types through 64 bits, pointers, f32/f64,
nested structs/unions and fixed arrays of supported members. Classes merge over
source members, including every union alternative; the LLVM union storage
anchor is never used to infer a union's C class.

- Aggregates at most 16 bytes classify into up to two INTEGER/SSE eightbytes.
  INTEGER dominates SSE when fields overlap an eightbyte.
- Integer pieces use width-bounded integer coercions; SSE pieces use float,
  double or a two-float vector. Padding never creates an extra register class.
- An unaligned member or an aggregate larger than 16 bytes uses MEMORY.
  Packed aggregates are tested independently from natural layout.
- Arguments expand into coerced register operands. If any register bank cannot
  hold the entire aggregate, the whole argument rolls back to stack `byval`;
  subsequent scalar arguments may use the unconsumed registers.
- MEMORY results use a hidden, naturally aligned `sret`/`noalias` pointer. That
  pointer consumes the first argument GPR. MEMORY arguments have typed `byval`
  and the SysV minimum eight-byte stack alignment.
- Every declaration and call carries the required attributes. Promotable signed
  and unsigned scalar integers use `signext` and `zeroext` respectively.
- Source parameters are reconstructed before the body. Source return expressions
  are evaluated before cleanup, then marshalled to the C result representation.
  ABI spills are local temporaries and do not change the source object layout.

## Explicit boundaries

Bitfields, empty/zero-length extension records, vectors, long double, complex
numbers, language tagged enums, closures/interfaces and C++ records are not
qualified. Aggregate C varargs and aggregate C thread-dispatch wrappers reject
until separately implemented. Ordinary C scalar varargs still use default
argument promotions. Header parsing has separate tests for prototyped typed
callback typedefs, inline callback parameters and callback factories, including
small/large aggregate callbacks. One-dimensional fixed/unsized array parameters
adjust to pointers; array/pointer typedef aliases preserve qualifiers. Unsupported
multidimensional/complex bounds and bare function-type parameter forms reject
explicitly. These tests do not imply a complete C parser. The broad aggregate
matrix also retains its independent explicit-declaration/C-bridge interface.

No ownership, pointer-provenance, destructor, broad memory-safety or kernel ABI
qualification follows from these tests.

## Reproduction

`python3 tests/abi/run_sysv_aggregates.py PATH_TO_COMPILER --cc PATH_TO_CLANG`

The suite creates fresh source, object, IR and executable artifacts and records
the tested compiler SHA-256 and C compiler version. It runs at O0 and O2. The
first phase verifies object memory layout only via scalar/pointer interfaces.
The second phase tests aggregate calls in both directions, C callbacks, inferred
C function pointers, register-bank exhaustion and hidden-result-pointer
register consumption. It checks narrow integer attributes in emitted LLVM IR.
Existing `tests/layout/` and `tests/cinterop/` remain independent regressions.
Actual runtime results, optimization levels and hashes must accompany a
qualification claim; this document alone is not evidence of a passing run.
