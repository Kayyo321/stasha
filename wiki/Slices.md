# Slices

A slice is a fat pointer — `(data*, len, cap)` — that gives a dynamic view over a contiguous run of elements. Unlike a fixed array (`T[N]`), the length is not part of the type, so functions can accept slices of any size:

```stasha
[]T              // the slice type
heap []T         // heap-domain slice; not proof of allocation-base ownership
stack []T        // borrowed view (no ownership)
```

This page describes the slice APIs and the A1 explicit-access/view contract.
Allocation, initialization-produced writes, `foreach`, `copy`, `append`, and
general ownership/lifetime safety are not qualified by A1. Examples of those
APIs describe intended usage, not proof that their inputs or ownership are safe.

---

## Storage Domains

The storage qualifier on a slice variable describes where the **backing buffer** lives:

| Declaration | Backing buffer | Free with | Notes |
|-------------|---------------|-----------|-------|
| `heap []T s = make.([]T, n);` | malloc'd | `rem.(s)` | Owned, must be freed |
| `heap []T s = make.{...};` | malloc'd | `rem.(s)` | Same — see *make.{...}* below |
| `stack []T v = arr[:];` | array on stack | (nothing) | Borrowed view |
| `stack []T s = make.{...};` | alloca region | (nothing) | Compiler-managed |

`heap []T` is **the one allowed exception** to the rule that disallows `heap` on
non-pointer variables: a slice descriptor includes a data pointer. Neither this
qualifier nor `cap` proves that the pointer is a live owning allocation base.
Aliased, field-held, or returned views can still reach realloc/free; general
`append`/`rem` ownership enforcement remains unqualified.

Stack slices never own their data. `rem.()` on a stack slice is a compile error.

---

## A1: Explicit Access and View Bounds

Explicit slice reads, stores, compound updates, and indexed addresses require
`0 <= index < len`. `&slice[len]` is invalid, even for a live backing allocation
with spare capacity. The descriptor's signed i32 fields must have `len >= 0` and
`cap >= len`. Shape validation is numerical; it proves neither backing storage
nor initialization/lifetime. Ordinary checks remain active inside `unsafe`.

Fixed-array access uses the actual LLVM array extent `N`, including nested
arrays and fields. Reads, stores, updates, and recursive base indexes require
`0 <= index < N`. Only pure terminal `&array[N]` may form a one-past address,
without loading an element or permitting its dereference. For example,
`&matrix[rows][0]` is invalid although `&matrix[rows-1][columns]` may be one-past.

For an array, `source[lo:hi]` requires `0 <= lo <= hi <= N`. Array-to-slice
conversion rejects `N > INT32_MAX` before constructing descriptor fields.
For an existing slice it requires `0 <= lo <= hi <= current len`, never spare
capacity. Omitted `lo` is zero; omitted `hi` is the array extent/current length.
The result has `data = old_data + lo`, `len = hi-lo`, and `cap = old_cap-lo`
(`old_cap = N` for an array). Empty-at-end ranges remain valid. Capacity metadata
does not permit reading or reslicing past the current logical length.

Indexes and range bounds retain their full source integer width and signedness
until validation, so negative signed values and large `u64` values cannot pass
by truncation. Checks precede the indexed/range-offset GEP and narrowing into
descriptor fields. The source element type is retained for supported field,
call, nested-index, and view expressions.

Operands evaluate once: base then index for reads/addresses; source, `lo`, then
`hi` for ranges; RHS before target base/index for plain indexed assignment;
target base/index before RHS for compound assignment. The relevant operands
precede the operation's shape/extent guards. A compound RHS invalidating its
saved address remains an ownership/lifetime issue.

`[unchecked: index]` requires `unsafe` and skips only that index's extent guard.
It retains shape/type validation and does not disable checks within the base,
index expression, or surrounding operations. Ordinary range checks cannot be
disabled by enclosing them in `unsafe`.

### Explicit Unsafe Raw-Pointer Views

A raw data-pointer range requires `unsafe`, an explicit upper bound, and a
resolved non-void sized element type:

```stasha
stack i32 arr[2] = .{7, 8};
stack i32 *rw p = &arr[0];
unsafe {
    stack []i32 view = p[0:2];   // data=p, len=cap=2
    stack []i32 end = p[2:2];    // data=p+2, len=cap=0
}
```

Pointer, `lo`, and `hi` evaluate once in that order; omitted `lo` is zero.
Full-width guards require `0 <= lo <= hi <= INT32_MAX`. Before forming the range
offset, `hi` must also fit `target_max_size / target_ABI_element_size`; zero-sized
elements have no byte offset and need no division. Result data is `ptr + lo`,
with **`len = cap = hi-lo`**. Function values, futures, and opaque handles cannot
implicitly become data views, even when LLVM represents them as pointers. An
explicit typed data-pointer cast is required first and does not prove validity.

By constructing this mutable view, the caller asserts that the selected interval
is live, correctly aligned, initialized, correctly typed, and has the required
mutation permission. An empty interval preserves `ptr + lo`, which must be
valid within an allocation or one-past it. Null is permitted only for `0:0` as
an unsafe caller precondition; there is no new runtime null/lifetime proof.
The descriptor records this assertion, not ownership of the allocation base.
Raw `new`/`mov` policies are unchanged.

### Failure and Compatibility

Invalid bounds or descriptor shape call the nonreturning
`void __stasha_panic(u32)` hook with reason **6**. The hosted weak default traps
without I/O; freestanding final images must provide the hook. Reasons 1–5 are
unchanged, while 7 (null) and 8 (allocation) remain reserved. No unwind or
defer-on-panic behavior is promised.

Rebuild source-backed archives for **ABI 4** after these semantic changes. The
three-field descriptor layout remains unchanged, and the archive marker alone
does not change native C ABI shape. A1 does not qualify initialization-produced
writes/ranges/spreads, `foreach`-generated indexing, `copy`, `append`, allocation,
general raw-pointer access, initialization, ownership/lifetimes, concurrency, or
atomic/volatile semantics. Existing K0 restrictions and indexed `++`/`--`
rejection are unchanged.

## Building Slices

### `make.([]T, len)` — Owned, Zeroed

```stasha
heap []i32 s = make.([]i32, 6);    // [0 0 0 0 0 0]  len=cap=6
defer rem.(s);
```

### `make.([]T, len, cap)` — Owned, Explicit Capacity

```stasha
heap []i32 buf = make.([]i32, 0, 8);   // len=0 cap=8 — no realloc until 9th append
defer rem.(buf);
```

### `make.{...}` — Inline Initializer (v2)

`make.{...}` is the compound-initializer counterpart of `.{...}` for slices. The element type is inferred from the LHS; the storage class on the LHS picks where the backing buffer lives:

```stasha
heap []i32 a = make.{1, 2, 3, 4};            // len=4 cap=4
defer rem.(a);

stack []i32 s = make.{7, 8, 9};              // alloca-backed; never escapes the fn
```

`make.{...}` understands the same grammar as `.{...}` — literals, ranges, designators, spread:

```stasha
heap []i32 b = make.{1..=8};                 // [1 2 3 4 5 6 7 8]
heap []i32 c = make.{0..10:2};               // [0 2 4 6 8]
heap []i32 d = make.{[0] = 1, [4] = 5};      // [1 0 0 0 5]
heap []i32 e = make.{0..3, 99, 100};         // [0 1 2 99 100]

stack i32 src[4] = .{10, 20, 30, 40};
heap []i32 f = make.{..src, 99, 100};         // [10 20 30 40 99 100]
defer rem.(f);

heap []i32 base = make.{1, 2, 3};
heap []i32 g = make.{..base, 99, ..base};     // [1 2 3 99 1 2 3]
defer rem.(base); defer rem.(g);

heap []i8 s = make.{.."hello"};                // string spread
defer rem.(s);
```

#### Range Coercion

Integer ranges coerce to floats when the slice element type is `f32` / `f64`:

```stasha
heap []f32 f = make.{1..=4};                 // [1.0 2.0 3.0 4.0]
defer rem.(f);

heap []f64 g = make.{0.0..=1.0:0.25};        // [0.0 0.25 0.5 0.75 1.0]
defer rem.(g);
```

Float ranges require all bounds and the step to be float literals.

#### Compile-Time vs Runtime Length

`make.{...}` computes the length at compile time when every component is a constant range or a constant array. Otherwise it falls back to runtime length arithmetic (e.g. when spreading another slice whose `len` is dynamic):

```stasha
// length known at compile time → single allocation, no loop
heap []i32 a = make.{0..1000};

// length depends on `base` at runtime → emits a small loop to compute size
heap []i32 base = compute_base();          // returns []i32 of unknown length
heap []i32 b = make.{..base, 99, ..base};
```

For very large constant ranges, the codegen emits a loop instead of unrolling — `make.{0..1000000}` produces one `malloc` and one `for` loop.

#### `final` / `const` Fast Path

When all elements are scalar literals and the slice is `final` or `const`, the compiler lowers it to a private constant global. No allocation, smaller IR, fewer instructions:

```stasha
final []i32 weeks = make.{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
print.('{}\n', weeks[1]);                    // 28
```

This optimisation is opt-in — you have to use `final` or `const` to get it. Mutable slices always allocate.

#### Empty `make.{}`

```stasha
heap []i32 z = make.{};      // len=0 cap=0 — same shape as `nil`
print.('{} {}\n', len.(z), cap.(z));   // 0 0
```

#### Element Destructors

If the element type has a `rem` method, `rem.(s)` walks the elements first, calling each destructor, then frees the backing buffer:

```stasha
heap []Buffer bufs = make.{
    Buffer.(64),
    Buffer.(128),
    Buffer.(256),
};
defer rem.(bufs);   // calls each Buffer.rem(), then frees the slice
```

#### Nested Slices: `heap [][]T`

```stasha
heap [][]i32 outer = make.{
    make.{1, 2, 3},
    make.{4, 5, 6, 7},
};
defer rem.(outer);    // walks inner slices first, then frees outer
```

The destructor pass is recursive — every inner slice is freed before the outer buffer.

---

## Slicing Existing Buffers

### From a Stack Array

```stasha
stack i32 arr[8] = .{10, 20, 30, 40, 50, 60, 70, 80};

stack []i32 full = arr[:];      // len=8 cap=8
stack []i32 mid  = arr[2:6];    // len=4 cap=6  (elements 30..60)
stack []i32 tail = arr[5:];     // len=3 cap=3  (elements 60..80)
```

Mutations through a view affect the original array:

```stasha
mid[0] = 999;
print.('{}\n', arr[2]);          // 999
```

### Reslicing

Reslicing produces another view into the **same** backing store. All stack `[]T` — no allocation:

```stasha
stack []i32 a = arr[:];         // len=8 cap=8
stack []i32 b = a[1:5];         // len=4 cap=7
stack []i32 c = b[1:3];         // len=2 cap=6
```

`cap` shrinks only when the lower bound moves; the upper bound moves the `len`.
For example, `b[0:5]` above is invalid because `len.(b) == 4`, even though
`cap.(b) == 7`. An empty `b[4:4]` view is valid. The retained capacity is metadata,
not permission to expose uninitialized or previously hidden elements.

---

## Functions Take Borrowed Views

Slice parameters are **always** `stack []T`. The fat pointer is copied by value; mutations reach the shared backing store:

```stasha
int fn sum(stack []i32 s): i32 {
    stack i32 total = 0;
    for (stack i32 i = 0; i < len.(s); i++) {
        total += s[i];
    }
    ret total;
}

int fn fill(stack []i32 s, stack i32 v): void {
    for (stack i32 i = 0; i < len.(s); i++) { s[i] = v; }
}
```

You can pass a `heap []T` or `stack []T` to either of these — the cast to `stack []T` happens implicitly.

### Returning a Heap Slice

```stasha
int fn make_squares(stack i32 count): heap []i32 {
    heap []i32 s = make.([]i32, count);
    for (stack i32 i = 0; i < count; i++) {
        s[i] = (i + 1) * (i + 1);
    }
    ret s;   // ownership transfers to the caller
}

heap []i32 sq = make_squares(5);
defer rem.(sq);                      // caller frees
```

Never return a `stack []T` that views a local stack array — that's a dangling view:

```stasha
int fn bad(void): stack []i32 {
    stack i32 arr[4] = .{1, 2, 3, 4};
    ret arr[:];                       // ERROR — dangling view after return
}
```

The compiler rejects this with a *stack escape* diagnostic.

---

## `append.()` and Growth

This API's allocation and ownership behavior is outside A1 qualification.
Only a live owning allocation base may be reallocated or freed. A `heap`
declaration and a numerically valid `cap` do not establish that precondition;
borrowed/interior views can still reach these operations through aliases,
fields, or returned values. Do not infer ownership safety from acceptance.

```stasha
heap []i32 grow = make.([]i32, 0, 2);
defer rem.(grow);
for (stack i32 i = 0; i < 8; i++) {
    grow = append.(grow, i);
}
```

Behaviour:

- If `len < cap`, the same backing store is reused (O(1)).
- If `len == cap`, a new buffer is allocated at **2× capacity** and the old one is freed internally.

For the intended owning-buffer pattern, reassign `grow = append.(grow, val)`
and free only the current valid owner. Growth may invalidate aliases. Reassignment
alone is not proof of a live owning base or safe allocation-failure handling.

### Nil Slices

A nil slice has `len=0`, `cap=0`, and no allocation. `append.()` on nil allocates on first use:

```stasha
heap []i32 out = nil;
defer rem.(out);                       // safe no-op if no appends occur

for (stack i32 i = 1; i <= 5; i++) {
    out = append.(out, i * i);
}
print.('{}\n', len.(out));             // 5
```

`rem.(nil)` is always a no-op.

---

## `copy.()`

```stasha
heap []i32 dst = make.([]i32, 8);
defer rem.(dst);
heap []i32 src = make.{10, 20, 30, 40, 50};
defer rem.(src);

stack i32 n = copy.(dst, src);
// n = 5, dst = [10, 20, 30, 40, 50, 0, 0, 0]
```

`copy.()` returns the number of elements actually copied — `min(len(dst), len(src))`. Source and destination may overlap; the implementation handles it correctly.

---

## Iteration: `foreach`

```stasha
heap []i32 nums = make.{1..=10};
defer rem.(nums);

foreach n in nums {
    print.('{} ', n);          // 1 2 3 4 5 6 7 8 9 10
}
```

`foreach` works on any slice — heap or stack. The element is a by-value copy, scoped to the loop body. `break` and `continue` behave as in any other loop.

See [Control Flow](Control-Flow#foreach--slice-iteration) for the full grammar.

---

## Built-in Operations Summary

```stasha
make.([]T, len)         // allocate, zero, cap = len
make.([]T, len, cap)    // allocate with explicit capacity
make.{...}              // compound initializer (literals, ranges, spread)
append.(s, val)         // grow if needed; ownership safety is not A1-qualified
copy.(dst, src)         // copy min(len) elements; return count
len.(s)                 // current element count
cap.(s)                 // backing capacity
foreach v in s { }      // iterate
s[lo:hi]                // reslice
rem.(s)                 // free heap slice (with element destructor walk)
```

---

## Common Mistakes

**Returning a view of a local array:**
```stasha
int fn dangling(): stack []i32 {
    stack i32 a[4] = .{1,2,3,4};
    ret a[:];              // ERROR
}
```

**Calling `rem.()` on intermediate `append` results:**
```stasha
heap []i32 s = make.([]i32, 0, 4);
heap []i32 t = append.(s, 1);
// rem.(s);               // invalid ownership pattern; not reliably diagnosed
rem.(t);                  // only if t is the sole current live owning base
```

Depending on growth, `s` may alias `t` or hold a stale pointer. The compiler does
not yet qualify general alias/ownership enforcement for `append` or `rem`.

**Forgetting the storage qualifier on `make.{}`:**
```stasha
[]i32 x = make.{1, 2, 3};   // ERROR — `make.{}` requires heap or stack on the LHS
heap []i32 y = make.{1, 2, 3};   // OK
```

**Spreading from a unknown-size source:**
```stasha
fn build(stack []i32 src): heap []i32 {
    ret make.{..src, 99};      // OK — runtime length, emits a loop
}
```
