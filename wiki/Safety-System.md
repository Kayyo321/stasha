# Safety System

This page describes Stasha’s safety contracts. The current development compiler
has qualified subsets, not a complete memory-safety proof: nullable flow, alias
lifetimes, receiver effects, and allocation-failure behavior are still being
repaired and tested. See [implementation status](Kernel-Readiness-Status.md).
Explicit fixed-array and slice accesses have the A1 checks described below.
Ordinary checks remain active inside `unsafe`; the unsafe-only `unchecked` index
flag suppresses just its own extent guard.

---

## Pointer Permissions

Every pointer type declares what operations are allowed through it:

| Permission | Symbol | Reads | Writes | Arithmetic |
|-----------|--------|-------|--------|------------|
| Read-only | `*r` | Yes | No | No |
| Write-only | `*w` | No | Yes | No |
| Read-write | `*rw` | Yes | Yes | No |
| Read + arith | `*r+` | Yes | No | Yes |
| Write + arith | `*w+` | No | Yes | Yes |
| Full | `*rw+` | Yes | Yes | Yes |

```stasha
stack i32 *r  reader  = &x;     // can only read
heap  u8  *w  writer  = buf;    // can only write
heap  i32 *rw both    = data;   // read and write
heap  u8  *r+ walker  = start;  // read and walk forward
```

### Permissions Only Narrow

Outside `unsafe`, you can narrow permissions (give up capabilities), but cannot
widen them through the checked pointer conversions:

```stasha
heap i32 *rw p = new.(4);
heap i32 *r  q = p;       // OK: narrowing rw → r
// heap i32 *rw r = q;   // ERROR: cannot widen r → rw
```

Checked conversions preserve the read-only restriction; unsafe escapes and
arbitrary reinterpretation are not a general proof of immutability.

Explicit pointer-to-pointer casts also check read/write permissions outside
`unsafe`. Nested read/write qualifiers can narrow only through a read-only
pointer path: a writable pointer slot must retain the same nested read/write
qualifiers, because a replacement value would also be visible through the
original alias. For example, `i32 *rw *rw` can become `i32 *r *r`, but cannot
become `i32 *r *rw`. This is a bounded qualifier check, not a proof covering
integer-pointer round trips, arbitrary type reinterpretation, or lifetimes.
Arithmetic qualifier consistency remains a separate implementation issue.

A read-only pointer to a container protects its fields, including a slice's
descriptor. It does not freeze a slice's separately referenced backing buffer:
`&container.view[i]` can still provide a writable element address when the
descriptor can be read. Taking a writable address of the descriptor itself
remains disallowed. The same shallow distinction applies to `const` and `final`
slice descriptors.


### Const and Final

```stasha
const i32 N = 100;
stack i32 *r  ok  = &N;    // read-only pointer from const — OK
// stack i32 *rw bad = &N; // ERROR: writable pointer from const variable
```

---

## Storage Domain Tracking

Every pointer variable carries a **domain**: stack or heap. The domain describes where the pointed-to data lives, not where the pointer variable itself is stored.

### Heap Domain

```stasha
heap i32 *rw p = new.(sizeof.(i32));  // p points to heap memory
defer rem.(p);
```

### Stack Domain

```stasha
stack i32 x = 42;
stack i32 *rw q = &x;   // q points to stack memory
```

### Domain Rules

**Rule 1: No heap allocation into a stack pointer**
```stasha
stack i32 *rw p = new.(4);  // ERROR: heap alloc into stack pointer
```

**Rule 2: No stack address into a heap pointer**
```stasha
stack i32 local = 42;
heap i32 *rw p = &local;    // ERROR: stack address into heap pointer
```

**Rule 3: No `rem.()` on stack pointers**
```stasha
stack i32 x = 42;
stack i32 *rw p = &x;
rem.(p);                    // ERROR: cannot rem a stack pointer
```

**Rule 4: Domain is permanent**

Once a pointer is assigned from one domain, every subsequent assignment must respect the same domain:

```stasha
heap i32 *rw p = new.(4);
p = &some_local;            // ERROR: stack address into heap pointer (on re-assign)
```

---

## Stack Escape Prevention

The compiler tracks when a pointer to a local variable would escape its scope:

```stasha
fn get_ptr(): stack i32 *rw {
    stack i32 local = 42;
    ret &local;   // ERROR: cannot return pointer to local stack variable
}
```

This diagnoses selected direct escapes. Alias-based and general lifetime
enforcement remains unqualified; A1 bounds checks do not repair it.

---

## A1: Explicit Array and Slice Bounds

Explicit reads, stores, compound updates, and indexed addresses share bounds
lowering. For a fixed array, reads/stores/updates require `0 <= index < N`, using
the actual LLVM array extent, including fields and nested array storage. Only a
pure terminal `&array[index]` permits `index == N`: it forms a one-past pointer,
loads no element, and does not permit dereference. Recursive bases still select
an element. Thus `&matrix[rows][0]` fails, but
`&matrix[rows-1][columns]` may form a one-past pointer to the final row.

For slices, all four operations require `0 <= index < len`, including address
formation: `&slice[len]` is invalid. The signed i32 descriptor fields must satisfy
`len >= 0` and `cap >= len`. Shape validation is numerical; it does not prove
backing allocation, initialized values, or lifetime.

```stasha
heap []i32 s = make.([]i32, 5);
s[4] = 99;   // OK: 4 < 5
s[5] = 99;   // reason-6 panic: index 5 >= len 5
```

Indexes and range bounds retain their full source width and signedness before
validation; negative signed values and large `u64` values cannot pass by integer
truncation. Fixed-array extents likewise are not truncated to descriptor width.

Each relevant expression evaluates once, in the existing order:

- Read/address: base, then index.
- Range: source, then `lo`, then `hi`.
- Plain indexed assignment: RHS, then target base and index.
- Compound assignment: target base and index, then RHS.

The operation's shape/extent guards follow its relevant operands; a failed
compound target prevents RHS evaluation. A later RHS that invalidates the saved
target address is an ownership/lifetime issue beyond bounds checking.

Array views require `0 <= lo <= hi <= N`; arrays with `N > INT32_MAX` cannot be
converted to a slice. Slice views require `0 <= lo <= hi <= current len`.
Capacity never authorizes exposing elements beyond that length. Omitted `lo` is
zero and omitted `hi` is the source length. Result length is `hi-lo`, capacity is
`old_cap-lo`; empty-at-end ranges are valid. Ordinary range and descriptor checks
remain active inside `unsafe`.

### Explicit Unsafe Index Escape

`[unchecked: i]` requires an `unsafe` block:

```stasha
unsafe {
    for (stack i32 i = 0; i < len.(s); i++) {
        s[unchecked: i] = i;   // skips only this index's extent guard
    }
}
```

Use it only when you have established access validity. Descriptor shape and
resolved element-type validation remain. Checks in the base, index expression,
or surrounding operations remain: an unchecked outer access cannot hide an
ordinary invalid access used to compute its index. Using `unchecked` outside
`unsafe` is a compile-time error.

### Unsafe Raw-Pointer Views

`ptr[lo:hi]` requires `unsafe`, an explicit `hi`, and a resolved non-void sized
data element type. Function values, futures, and opaque runtime handles cannot
implicitly become views; an explicit typed data-pointer cast is required first.
The pointer and bounds evaluate once, in that order. Full-width guards require
`0 <= lo <= hi <= INT32_MAX` and, for nonzero target ABI element size,
`hi <= target_max_size / element_size` before the range offset. Zero-sized
elements avoid division. Result data is `ptr + lo` and `len = cap = hi-lo`.

The caller asserts that the interval is live, correctly aligned, initialized,
correctly typed, and permits mutation through the resulting slice. An empty
interval still requires a valid in-allocation or one-past result pointer. Null
is permitted only for `0:0` by this unsafe precondition; these numerical guards
do not add runtime null or lifetime proof. Descriptor fields record the caller's
assertion, not physical backing or ownership evidence.

### Failure Hook, Compatibility, and Boundary

Invalid index/range/shape checks call nonreturning
`void __stasha_panic(u32 reason)` with reason **6**. The hosted weak default traps
without I/O. Freestanding final images must supply the hook. Scalar reasons 1–5
are unchanged; 7 (null) remains reserved; B1 uses 8 for retained allocation failure. Returning from the
hook violates its ABI; there is no unwinding or defer-on-panic guarantee.

Rebuild source-backed archives for **ABI 5** after these source-compatibility
changes. The three-field descriptor layout is unchanged; the marker alone does
not change native C ABI shape.

A1 excludes initialization-produced writes/ranges/spreads, `foreach`-generated
indexing, `copy`, `append`, allocation, general raw-pointer access, initialization,
ownership/lifetimes, concurrency, and atomic/volatile semantics. K0 capability
restrictions and indexed `++`/`--` rejection are unchanged. General `append` and
`rem` remain ownership-unqualified: aliases, fields, and returned interior views
can reach realloc/free. Neither `heap` storage nor capacity proves a live owning
allocation base. See [Slices](Slices.md) and the
[regression contract](../tests/memory/README.md) for the bounded scope.

---

## Nullable Pointer Checks

Pointers prefixed with `?` may be nil. Use them to represent optional values:

```stasha
fn find(stack []i32 s, stack i32 target): ?i32 *rw {
    for (stack i32 i = 0; i < len.(s); i++) {
        if s[i] == target {
            heap i32 *rw p = new.(sizeof.(i32));
            *.(p) = target;
            ret p;
        }
    }
    ret nil;
}
```

Always check before dereferencing:
```stasha
heap ?i32 *rw result = find(nums, 42);
if result != nil {
    print.('found: {}\n', *.(result));
    rem.(result);
}
```

This is the intended guarded-use pattern. General CFG non-null tracking remains
unqualified; accepting this example does not prove that unguarded, aliased, or
later-invalidated uses are rejected. A1 does not add nullable-flow analysis.

---

## The `@frees` Attribute

Mark a pointer parameter with `@frees` to declare that the function takes **ownership** and is responsible for calling `rem.()` on all exit paths:

```stasha
ext fn process_and_free(@frees heap u8 *rw buf, stack i32 n): i32 {
    stack i32 sum = 0;
    for (stack i32 i = 0; i < n; i++) { sum += buf[i]; }
    rem.(buf);   // required: compiler verifies this
    ret sum;
}
```

If `rem.(buf)` is missing on any exit path, the compiler errors.

The caller must not use `buf` after passing it to such a function:
```stasha
heap u8 *rw data = new.(4);
data[0] = 10;
stack i32 result = process_and_free(data, 4);
// data is now dangling — do not use
```

---

## `unsafe {}` Blocks

`unsafe` permits existing pointer/domain escapes and explicit unchecked indexing.
It does not remove ordinary array/slice bounds, descriptor-shape, or range checks.
Use it only when you have manually verified the extra obligations:

```stasha
unsafe {
    // Pointer arithmetic without permission
    // Existing domain escapes remain available
    // Permission widening allowed
    for (stack i32 i = 0; i < n; i++) {
        dst[i] = src[i];   // ordinary array/slice checks remain active
    }
}
```

Keep unsafe blocks narrow. Use `[unchecked: i]` inside them only for the specific
extent check you mean to suppress; other operations retain their own checks.

---

## Summary: Bounded Checks and Open Work

| Bug | Detection |
|-----|-----------|
| Explicit array/slice out-of-range access | A1 extent checks, with the explicit unsafe index escape above |
| Malformed slice length/capacity | Numerical shape check; no allocation/lifetime proof |
| Use-after-free | General alias/lifetime analysis remains unqualified |
| Stack escape | Selected direct return/storage diagnostics; not general alias proof |
| Permission violation | Bounded pointer permission checks outside unsafe |
| Domain mismatch | Storage-domain diagnostics; not allocation-base ownership proof |
| Null dereference | General nullable-flow enforcement remains unqualified |
| Missing ownership transfer | `@frees` checks are not general ownership qualification |
| `rem.()` on non-owned ptr | Direct stack-pointer rejection is not proof for other pointers/views |
| Const write | `const`/`final` → only `*r` pointers |
| Arithmetic without permission | `+` permission required |

---

## Safety vs Performance

Permissions/domain diagnostics are compile-time checks. Bounds and descriptor
checks are emitted at runtime unless optimization proves them redundant.
`unsafe` alone does not remove them. The unsafe-only `unchecked` flag removes
one extent guard, not all checks; no speedup follows without measurement of the
actual optimized program. General raw-pointer operations are outside A1's
bounds guarantee and are not a substitute for establishing access validity.
