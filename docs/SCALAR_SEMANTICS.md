# Scalar semantics, provisional contract v1

This document describes the P1 implementation, not a completed qualification of
the compiler or its hosted runtime. `tests/scalars/` is the acceptance oracle.
Actual execution results, optimization levels, compiler/toolchain hashes and
remaining failures must accompany any qualification claim.

## Source types and literal domain

Supported integer value types are `i8/i16/i32/i64` and `u8/u16/u32/u64`.
`bool` is a separate truth-value type with an i1 representation. Integer AST
literals store a positive `uint64_t` magnitude; a leading minus is a separate
unary AST node. Parsing a magnitude above `UINT64_MAX` is an error. This bounded
representation is independent of compiler-host `long`; it is not arbitrary
precision. The differential test reference uses Python arbitrary-precision ints.

A direct positive or negative literal adopts its declaration, assignment,
parameter, return or other typed integer operand's type. Literal-only arithmetic
subexpressions inherit that same context. The literal must fit that type exactly;
there is no implicit truncation. Unary negative literal syntax permits signed
minimum values, including `-9223372036854775808`. Negative unsigned literals are
rejected except negative zero. Without context the defaults are i32, then i64,
then u64 for positive literals; negative literals default to i32 or i64.

The expected source type is associated with one AST node. An initializer does
not leak its expected type into conditions, indexing expressions, callees,
unrelated arguments or the operand of an explicit cast. Source type descriptors
are retained for variables, fields, dereferences, indexed elements, function
signatures, casts, arithmetic results and conditional results. LLVM integer
bit widths are never sufficient evidence of source signedness.

## Binary operations and conversions

- A typed integer operand determines an untyped literal operand's type.
- Two typed integer operands of the same signedness use their greater width.
- Typed signed and unsigned arithmetic/comparison operands require an explicit
  cast. There is no C-style implicit signedness selection.
- Assignment/argument/return conversions accept same-signedness widening and
  unsigned-to-strictly-larger-signed widening. Other integer conversions require
  an explicit cast. Typed expressions are not narrowed merely because their
  receiving variable is narrower.
- An explicit integer cast truncates low bits when narrowing and extends using
  the **source** signedness when widening. At equal widths it reinterprets the
  same bit pattern. Thus `(u64)((u8)255)` is 255, while `(u64)((i8)-1)` is
  `UINT64_MAX`.
- A cast operand is typed independently before conversion. `(u8)256` explicitly
  truncates to zero; `u8 value = 256` is a compile-time error.
- Integer-to-bool converts zero to false and all other values to true.
- Integer-to-floating conversion uses source signedness. Floating-to-integer
  conversion traps for NaN or a value outside `[0, 2^w)` for unsigned and
  `[-2^(w-1), 2^(w-1))` for signed, then truncates toward zero. These conservative
  bounds apply before truncation. Floating-to-bool is false for either signed
  zero and true otherwise, including NaN.
- The C variadic ABI retains default promotions: i/u8/16 and bool to i32, and f32
  to f64. This does not make unqualified C signature inference ABI-safe.

Pointer/integer explicit cast paths preserve an available 64-bit address bit
pattern on the initial 64-bit target. This is not a relocation, provenance, safe
dereference or cross-target ABI guarantee. Legacy pointer/call inference paths
remain outside the qualified scalar subset until the ABI/lvalue work packages.

## Arithmetic, exceptions and evaluation

Ordinary `+`, `-`, `*`, unary negation, and explicit `+%`, `-%`, `*%` wrap modulo
the result width, for signed and unsigned types. Signed results interpret the
wrapped bit pattern as two's complement. No `nsw` or `nuw` promise is added.
`+!`, `-!`, `*!` use the signed or unsigned overflow intrinsic at the actual
result width and call the failure hook on overflow. The LLVM constant constructor
canonicalizes unused high bits before invoking LLVM23; source fit checks happen
separately. This matters because malformed in-memory constants can print valid
IR while optimizing incorrectly.

Division and remainder use source signedness. Signed division truncates toward
zero and signed remainder has the dividend's sign. Both division by zero and
signed MIN divided **or remaindered** by -1 call the failure hook before the
LLVM operation can execute.

Shift counts retain their original type until checked. A negative count or
count greater than or equal to the left width calls the failure hook, including
compound assignments. A count such as 256 cannot first narrow to u8 zero. Left
shift drops high bits; signed right shift sign-fills and unsigned right shift
zero-fills. Shift operators do not change the left operand's width.

Binary operands and call arguments are evaluated left to right. `&&` and `||`
short-circuit. Conditional expressions evaluate only the selected branch;
conversions are emitted in their respective predecessor blocks before the phi.
Comparison chains evaluate the base once and short-circuit AND groups before OR
groups. Their right sides use the base's scalar type; a lossy conversion requires
an explicit cast. Cleanup/evaluation ordering at scope exits is a separate P3
contract.

## Failure ABI

`void __stasha_panic(uint32_t reason)` must never return. Its stable reason IDs are:

| ID | Meaning |
| --- | --- |
| 1 | checked integer overflow |
| 2 | integer division/remainder by zero |
| 3 | signed MIN / -1 or MIN % -1 |
| 4 | invalid shift count |
| 5 | floating-to-integer range/NaN |
| 6 | invalid explicit array/slice index, view range, or slice descriptor shape (A1) |
| 7 | reserved: null |
| 8 | reserved: allocation |

Hosted mode lazily emits a weak, cold, noinline, noreturn implementation that
executes `llvm.trap`, with no implicit I/O. A strong object definition can replace
it. Freestanding mode emits only a declaration; the final image must provide the
hook, and a missing hook is a link failure. Returning from an override violates
the ABI. Stasha source definitions of this reserved hook are rejected until the
compiler can prove they do not return; only an exact external `void(u32)`
declaration and a reviewed C/assembly implementation are supported. The differential harness can link a strong C hook and verify the exact
reason plus a fixed exit code; a signal alone does not qualify reason selection.

### A1 bounds integration

A1 uses the same reason-6 hook for explicit indexed reads, stores, compound
updates, and address formation, plus view-range/descriptor-shape checks. Reasons
1–5 remain stable, and 7 (null) and 8 (allocation) remain reserved. These calls
have no implicit I/O or unwinding/defer-on-panic guarantee.

Index and range expressions retain their full source integer width and
signedness until validation. Source-signed widening and unsigned full-width
comparison reject negative signed and large `u64` inputs without first narrowing
to the descriptor's signed i32 fields. Fixed arrays use their actual LLVM extent
`N`: ordinary element and recursive-base access requires `0 <= index < N`.
Only pure terminal `&array[N]` may form an unloaded, non-dereferenceable one-past
address. Slices require `0 <= index < len`, including indexed addresses, so
`&slice[len]` is invalid. Descriptor fields must have `len >= 0` and `cap >= len`.

Array views require `0 <= lo <= hi <= N`; conversion rejects `N > INT32_MAX`.
Slice views require `0 <= lo <= hi <= current len`, never spare capacity. Their
result has `len = hi-lo` and `cap = old_cap-lo`; empty-at-end ranges are valid.
Ordinary bounds, shape, and range checks persist inside `unsafe`. The unsafe-only
`[unchecked: index]` skips only its own extent guard, retaining shape/type checks
and checks in its operands or surrounding operations.

Each relevant operand evaluates once: base then index for reads/addresses;
source, `lo`, then `hi` for ranges; RHS before indexed target for plain assignment;
target before RHS for compound assignment. Shape/extent guards follow the
relevant operands. Bounds checking does not prove that a later compound RHS
leaves its saved target alive.

An explicit unsafe raw data-pointer range requires an explicit `hi` and a
resolved non-void sized element type. Full-width checks require
`0 <= lo <= hi <= INT32_MAX` and, for nonzero target ABI element size,
`hi <= target_max_size / element_size` before the range offset. Zero-sized
elements avoid division. Result data is `ptr + lo`, with `len = cap = hi-lo`.
Function values, futures, and opaque handles need an explicit typed data-pointer
cast before view construction. The caller asserts a live, aligned, initialized,
correctly typed interval and permission for the mutable slice. Empty intervals
still require a valid in-allocation/one-past result; null is permitted only for
`0:0` as a caller precondition. These are not runtime null/lifetime proofs.

A1 excludes initialization-produced writes/ranges/spreads, `foreach`-generated
indexing, `copy`, `append`, allocation, general raw-pointer access,
initialization, ownership/lifetimes, concurrency, and atomic/volatile semantics.
General `append`/`rem` remain ownership-unqualified: neither `heap` nor capacity
establishes a live owning allocation base, including through aliases, fields,
and returned views. K0 restrictions and indexed `++`/`--` rejection are unchanged.
See [Safety System](../wiki/Safety-System.md) and [Slices](../wiki/Slices.md).

These source-compatibility changes require rebuilding source-backed archives
for **ABI 4**, although the three-field slice descriptor layout is unchanged.
This compatibility marker alone does not change native C ABI shape.

## Constant evaluation

Integer constant evaluation uses width-masked uint64 bit patterns and unsigned
magnitude arithmetic. It never delegates signed overflow, signed shifts,
MIN/-1 division or out-of-range shifts to host C. Runtime and compile-time
ordinary/wrapping/checked operations have the same integer rules. Exceptional
operations in evaluated compile-time branches reject compilation. Short-circuited
branches are not evaluated. Integer global constants are checked using this same
evaluator, rather than silently truncating or substituting zero. Scalar constant
references must name an already finalized immutable global. Forward references,
self references, and cycles reject; dependency-ordered evaluation is not yet
implemented. Symbolic pointer relocations use a separate initializer path.

The evaluator bounds recursion to 256 and expression work to 10,000 nodes per
request. Unsupported constant forms reject rather than inventing a value.
Existing aggregate-range metadata uses a separately checked `long` API; values
outside that API are rejected, not truncated. Initializer ranges have a
16,777,216-element resource cap, and their generated negative literals retain
unary-minus structure. This legacy aggregate interface is not a full-domain
integer constant interface.

Target OS/architecture selection uses the normalized target descriptor, not the
compiler host's preprocessor macros. `sizeof` uses the target DataLayout through
the authoritative layout query, with sized-type validation.

## Qualification boundary

The scalar suite covers integer boundaries, local/global literals, arguments,
returns, casts, arithmetic, comparisons, shifts, traps and short circuiting at
`-o=0/1/2/3`. Additional hosted paths (closures, coroutines, type-erased variadics,
complex aggregates, byte/array helpers, inferred external calls) must retain
individual regression results and are not automatically certified by these tests.
No broad memory-safety, layout, ABI, ownership or freestanding claim follows from
passing the scalar matrix.
