# Memory Management

Stasha gives you complete control over memory without a garbage collector. Every allocation is explicit, every free is your responsibility — but the language provides powerful tools to make this safe and ergonomic.

---

## The Three Allocation Strategies

| Strategy | Keyword | Freed by | Use when |
|----------|---------|----------|----------|
| Stack | (default) | Compiler | Small, short-lived data |
| Heap | `new.()` / `rem.()` | You | Dynamically sized, long-lived data |
| Zone | `new.(n) in zone` | `rem.(zone)` | Batch allocations with shared lifetime |

---

## Stack Allocation

Stack variables are automatically freed when their scope exits. No explicit cleanup needed.

```stasha
stack i32 x = 42;
stack f64 arr[8];
stack Point p = .{ .x = 1, .y = 2 };
```

Stack variables are the default for local non-pointer variables.

**Limitation:** You cannot return a pointer to a stack variable — it becomes dangling after the function returns:

```stasha
fn bad(): stack i32 *rw {
    stack i32 local = 42;
    ret &local;   // ERROR: stack escape detected by compiler
}
```

---

## Heap Allocation

Use `new.(size)` to allocate on the heap. Returns a `*rw` pointer.

```stasha
heap i32 *rw p = new.(sizeof.(i32));
*.(p) = 42;
rem.(p);   // must free when done
```

For arrays:
```stasha
stack i32 count = 10;
heap i32 *rw arr = new.(sizeof.(i32) * count);
defer rem.(arr);

for (stack i32 i = 0; i < count; i++) {
    arr[i] = i * i;
}
```

### `sizeof.(Type)`

Returns the byte size of a type at compile time:

```stasha
sizeof.(i32)      // 4
sizeof.(f64)      // 8
sizeof.(Point)    // depends on fields
sizeof.([]i32)    // 24 (ptr + len + cap on 64-bit)
```

### `rem.()` — Free

```stasha
rem.(ptr);   // frees the heap allocation, ptr becomes dangling
```

`rem.(nil)` is safe (no-op).

### `mov.()` — Realloc

Resize a heap allocation:

```stasha
heap i32 *rw buf = new.(sizeof.(i32) * 8);
// ... use buf with 8 elements ...

// Grow to 16 elements:
buf = mov.(buf, sizeof.(i32) * 16);
// old allocation may be freed; buf now points to new (or same) block
```

---

## `defer` — Guaranteed Cleanup

`defer` schedules cleanup to run at scope exit, no matter how the scope exits (normal return, early return, panic):

```stasha
fn process(stack i32 count): i32 {
    heap i32 *rw buf = new.(sizeof.(i32) * count);
    defer rem.(buf);   // registered here, runs at scope exit

    if count == 0 { ret -1; }   // rem.(buf) still runs!

    // ... fill and process buf ...

    ret total;   // rem.(buf) runs here too
}
```

**Best practice:** Put `defer rem.(ptr)` immediately after every `new.()`.

Multiple defers run in LIFO order (last in, first out):

```stasha
heap u8 *rw a = new.(100); defer rem.(a);   // rem.(a) runs 2nd
heap u8 *rw b = new.(200); defer rem.(b);   // rem.(b) runs 1st
```

Defer a block:
```stasha
defer {
    close_file(fd);
    release_lock(mtx);
}
```

---

## Zone Allocation (Arena Allocator)

A zone is a bump-allocating arena backed by linked 64 KiB blocks. All allocations within a zone are freed at once with a single `rem.(zone)` call. This is ideal for:

- Request-scoped memory (web servers)
- Temporary scratch buffers
- Parse trees and intermediate data structures

### Lexical Zone (Auto-Freed)

The zone is created on entry and freed automatically at the closing `}`:

```stasha
zone scratch {
    stack u8 *rw name = new.(32) in scratch;
    stack i32 *rw nums = new.(sizeof.(i32) * 8) in scratch;

    // use name and nums...

}   // scratch freed here — no rem.() needed
```

### Manual Zone

Declare a zone and free it yourself:

```stasha
zone workspace;
stack f64 *rw buf = new.(sizeof.(f64) * 100) in workspace;

// ... use buf ...

rem.(workspace);   // frees all allocations in the zone at once
```

### Zone as Parameter

Zones can be passed to functions:

```stasha
fn make_string(zone z, stack i32 n): stack i8 *rw {
    ret new.(n) in z;   // allocates from the caller's zone
}

zone arena;
stack i8 *rw s = make_string(arena, 64);
// use s...
rem.(arena);   // frees s and everything else in arena
```

### Global Zones

Zones can be declared at module level:

```stasha
zone request_arena;

fn handle_request(void): void {
    stack u8 *rw buf = new.(256) in request_arena;
    // ... use buf ...
    // do NOT call rem.(buf) — zone owns it
}

fn main(void): i32 {
    defer rem.(request_arena);   // frees everything at program exit
    // ... serve requests ...
    ret 0;
}
```

### Zone vs `rem.()` on Zone Pointers

Never call `rem.(ptr)` on a pointer allocated from a zone. The zone owns the memory:

```stasha
zone z;
stack i32 *rw p = new.(4) in z;
rem.(p);    // ERROR: cannot rem a stack pointer (zone-backed)
rem.(z);    // correct: free the whole zone
```

---

## Slices

Slices are fat pointers: `(data*, len, cap)`.

### Owned Slices (Heap)

```stasha
heap []i32 s = make.([]i32, 10);   // len=10, cap=10, zeroed
defer rem.(s);

s[0] = 42;
print.('{}\n', len.(s));   // 10
print.('{}\n', cap.(s));   // 10
```

### Borrowed Slices (Stack)

```stasha
stack i32 arr[8] = .{1,2,3,4,5,6,7,8};
stack []i32 view = arr[:];       // entire array
stack []i32 part = arr[2:6];     // elements [2,6)
stack []i32 tail = arr[5:];      // from index 5 to end
```

Stack slices never own their backing memory — never call `rem.()` on them.

### `append.()` — Dynamic Growth

```stasha
heap []i32 list = make.([]i32, 0, 4);   // len=0, cap=4
defer rem.(list);

for (stack i32 i = 0; i < 8; i++) {
    list = append.(list, i * 10);
}
// If cap is exceeded, append reallocates (2× growth)
// Always use the returned value: list = append.(list, ...)
```

### `copy.()` — Copy Elements

```stasha
heap []i32 src = make.([]i32, 5);
heap []i32 dst = make.([]i32, 8);
defer rem.(src); defer rem.(dst);

// fill src...
stack i32 n = copy.(dst, src);   // copies min(len(dst), len(src)) elements
```

### Nil Slice

A nil slice has no allocation. `len.()` and `cap.()` return 0. `append.()` on nil allocates on first use:

```stasha
heap []i32 out = nil;
defer rem.(out);   // rem.(nil) is a no-op if nothing is appended

for (stack i32 i = 0; i < 5; i++) {
    out = append.(out, i);
}
```

---

## Pointer Ownership Rules

The **storage qualifier** on a pointer variable tracks ownership:

| Qualifier | Meaning | May call `rem.()`? |
|-----------|---------|---------------------|
| `heap` | Owns the pointed-to allocation | Yes — required |
| `stack` | Borrows or points at stack memory | No — error |

```stasha
heap i32 *rw owned = new.(4);
defer rem.(owned);               // you own it, you free it

stack i32 local = 42;
stack i32 *rw borrowed = &local; // borrowed — do NOT rem
```

---

## Summary: Allocation Quick Reference

```stasha
// Single value
heap i32 *rw p = new.(sizeof.(i32));
*.(p) = 42;
rem.(p);

// Array
heap i32 *rw arr = new.(sizeof.(i32) * n);
defer rem.(arr);

// Slice (preferred over raw arrays)
heap []i32 s = make.([]i32, n);
defer rem.(s);

// Zone
zone arena;
stack u8 *rw tmp = new.(256) in arena;
rem.(arena);

// Struct with auto-destructor
{
    Buffer b = Buffer.(1024);
    // ... b.rem() called at }
}

// Slice append (idiomatic nil-start)
heap []Foo items = nil;
defer rem.(items);
items = append.(items, Foo.(1));
items = append.(items, Foo.(2));
```
