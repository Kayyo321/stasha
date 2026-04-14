# Safety System

Stasha enforces memory and pointer safety at compile time. These checks catch entire classes of bugs before your code runs — without a garbage collector or runtime overhead.

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

You can narrow permissions (give up capabilities), but you can never widen them:

```stasha
heap i32 *rw p = new.(4);
heap i32 *r  q = p;       // OK: narrowing rw → r
// heap i32 *rw r = q;   // ERROR: cannot widen r → rw
```

This means once you hand a read-only pointer to a function, that function cannot acquire write access.

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

This prevents dangling pointers from function returns.

---

## Slice Bounds Checking

Every slice index goes through a bounds check by default:

```stasha
heap []i32 s = make.([]i32, 5);
s[4] = 99;   // OK: 4 < 5
s[5] = 99;   // RUNTIME ERROR: index 5 >= len 5 → llvm.trap (abort)
```

Compile-time constant indices that are provably in-bounds are optimized out automatically.

### Skip Bounds Check

When you can prove the index is valid, use `[unchecked: i]`:

```stasha
for (stack i32 i = 0; i < len.(s); i++) {
    s[unchecked: i] = i;   // no bounds check emitted
}
```

Use this only when you are certain the index is in range.

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

The compiler tracks that inside the `if result != nil { ... }` branch, `result` is narrowed to non-nil.

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

All safety checks can be suppressed inside an `unsafe {}` block. Use this only when you have manually verified correctness:

```stasha
unsafe {
    // Pointer arithmetic without permission
    // Bounds checks disabled
    // Domain rules suspended
    // Permission widening allowed
    for (stack i32 i = 0; i < n; i++) {
        dst[i] = src[i];   // no bounds check
    }
}
```

Unsafe blocks are narrow by design. Safety resumes immediately after the closing `}`.

---

## Summary: What the Compiler Catches

| Bug | Detection |
|-----|-----------|
| Buffer overflow | Bounds check on every index |
| Use-after-free | Conservative provenance tracking |
| Stack escape | Return-of-local-address analysis |
| Permission violation | Pointer permission type system |
| Domain mismatch | Storage domain tracking |
| Null dereference | Nullable pointer type + nil check |
| Missing ownership transfer | `@frees` attribute verification |
| `rem.()` on non-owned ptr | Stack pointer `rem.()` error |
| Const write | `const`/`final` → only `*r` pointers |
| Arithmetic without permission | `+` permission required |

---

## Safety vs Performance

Safety checks have zero runtime cost at the type level (permissions, domains) — those are compile-time only.

Bounds checks do have a small runtime cost. In hot inner loops, you can:
1. Use `[unchecked: i]` when the index is provably safe
2. Use `unsafe {}` for the entire hot loop
3. Use pointer arithmetic with `*rw+` or `*r+` to avoid indexed access

```stasha
// Safe (bounds checked):
for (stack i32 i = 0; i < len.(s); i++) {
    total += s[i];
}

// Faster (no bounds check):
for (stack i32 i = 0; i < len.(s); i++) {
    total += s[unchecked: i];
}

// Fastest (pointer arithmetic, unsafe):
unsafe {
    stack i32 *r+ p = s.ptr;
    stack i32 *r  end = p + len.(s);
    while p < end {
        total += p[0];
        p = p + 1;
    }
}
```
