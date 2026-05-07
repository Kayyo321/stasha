# Types

Stasha has a rich, explicit type system. Every value has a known type at compile time.

---

## Primitive Types

### Integer Types

| Type | Width | Range |
|------|-------|-------|
| `i8` | 8-bit signed | -128 to 127 |
| `i16` | 16-bit signed | -32,768 to 32,767 |
| `i32` | 32-bit signed | ±2.1 billion |
| `i64` | 64-bit signed | ±9.2 × 10¹⁸ |
| `u8` | 8-bit unsigned | 0 to 255 |
| `u16` | 16-bit unsigned | 0 to 65,535 |
| `u32` | 32-bit unsigned | 0 to 4.3 billion |
| `u64` | 64-bit unsigned | 0 to 1.8 × 10¹⁹ |

Integer literals:
```stasha
42          // i32
42u         // u32
42i64       // i64 suffix
0xFF        // hex
0b1010      // binary
0o17        // octal
```

### Floating-Point Types

| Type | Width | Precision |
|------|-------|-----------|
| `f32` | 32-bit | ~7 decimal digits |
| `f64` | 64-bit | ~15 decimal digits |

```stasha
3.14        // f64
3.14f       // f32 literal
1.0e10      // scientific notation
```

### Boolean

```stasha
bool flag = true;
bool off  = false;
```

### `void` — No Value

Used as the return type of functions that return nothing, and as the parameter type for zero-argument functions:

```stasha
fn log(void): void { ... }
```

### `error` — Error Value

The `error` type is either `nil` (no error) or a string message. It's Stasha's lightweight error representation:

```stasha
fn parse(stack i8 *r input): [i32, error] {
    if *.(input) == 0 { ret 0, error.('empty input'); }
    ret 42, nil;   // nil means no error
}
```

Check with `!= nil` / `== nil`:
```stasha
let [val, err] = parse(s);
if err != nil { handle_error(err); }
```

---

## Pointer Types

Pointer types encode both the **permission** (what you can do with the pointer) and optionally whether it may be `nil`.

### Pointer Permissions

| Permission | Symbol | Meaning |
|------------|--------|---------|
| Read-only | `*r` | Can only read through the pointer |
| Write-only | `*w` | Can only write through the pointer |
| Read-write | `*rw` or `*` | Can read and write |
| With arithmetic | `+` suffix | Pointer arithmetic allowed |

Combine permissions:
```stasha
i32 *r      // read-only
i32 *w      // write-only
i32 *rw     // read + write
i32 *r+     // read + arithmetic
i32 *w+     // write + arithmetic
i32 *rw+    // read + write + arithmetic
```

Permissions can only **narrow**, never widen:
```stasha
i32 *rw p = &x;
i32 *r  q = p;   // OK — narrowing rw → r
// i32 *rw r = q;  // ERROR — cannot widen r → rw
```

### Nullable Pointers

Prefix the type with `?` to declare a pointer that may be `nil`:
```stasha
?i32 *rw maybe = find_something();
if maybe != nil {
    print.('{}\n', *.(maybe));   // safe to use inside this branch
}
```

Non-nullable pointers (`i32 *rw`) are assumed to never be nil. The compiler will eventually enforce this.

### Dereferencing

Use `*.(ptr)` to dereference:
```stasha
stack i32 x = 10;
stack i32 *rw p = &x;
*.(p) = 42;                    // write through pointer
print.('{}\n', *.(p));         // read: prints 42
print.('{}\n', x);             // same value: 42
```

---

## Slice Types

A slice is a **fat pointer** — a (pointer, length, capacity) triple — giving a dynamic view over contiguous elements.

```stasha
[]i32       // slice of i32
[]u8        // byte slice
```

Create slices:
```stasha
heap []i32 owned = make.([]i32, 10);       // heap-owned (must rem.())
heap []i32 cap   = make.([]i32, 0, 100);   // explicit capacity

stack i32 arr[8] = .{1,2,3,4,5,6,7,8};
stack []i32 view = arr[:];                  // borrowed view (no allocation)
stack []i32 part = arr[2:6];               // sub-view [2,6)
```

Built-in slice operations:
```stasha
len.(s)              // current element count
cap.(s)              // backing capacity
append.(s, val)      // return new slice with val appended
copy.(dst, src)      // copy elements, return count
```

See [Memory Management](Memory-Management) for ownership rules.

---

## User-Defined Types

### Structs

```stasha
type Point: struct {
    ext i32 x;
    ext i32 y;
}
```

See [Structs](Structs) for the full guide.

### Enums

```stasha
type Color: enum { Red, Green, Blue }

type Shape: enum {
    Circle(f64),     // payload
    Square(f64),
    Point,           // no payload
}
```

See [Enums](Enums) for pattern matching and tagged enums.

### Unions

```stasha
type Value: union {
    ext i32 as_int;
    ext f32 as_float;
    ext u8  as_byte;
}
```

All fields share the same memory. Size equals the largest field.

### Interfaces

```stasha
type Drawable: interface {
    draw(void): void;
}
```

See [Interfaces](Interfaces).

### Type Aliases

```stasha
type Byte:   u8;
type Handle: i32;
type Buffer: heap u8 *rw;
```

Aliases give semantic meaning to primitive types. They're fully interchangeable with the underlying type.

---

## Generic Types

Generic types are parameterized with `@comptime[T]`:

```stasha
type Pair: @comptime[T] struct {
    ext T first;
    ext T second;
}

// Instantiate
Pair.[i32] p = Pair.[i32] { .first = 1, .second = 2 };
```

See [Generics](Generics) for the full guide.

---

## The `any` Type — Inline Tagged Union

`any.[T1, T2, ...]` is an inline tagged union that can hold one of the listed types at a time:

```stasha
fn acceptor(any.[i32, i64, f32, f64] num): f64 {
    match any.(num) {
        i32(i32 val) => { ret val * 2; }
        i64(i64 val) => { ret val * 4; }
        f32(f32 val) => { ret val * 6; }
        f64(f64 val) => { ret val * 8; }
    }
}

let result = acceptor(10);           // passes as i32
let result = acceptor((i64)10);      // passes as i64
let result = acceptor(10.51);        // passes as f64
```

This is Stasha's approach to polymorphism without vtables or heap allocation.

---

## Function Pointer Types

```stasha
fn*(i32, i32): i32         // function pointer: takes two i32, returns i32
fn*(heap u8 *rw, i32): void   // function pointer with domain-tagged pointer param
```

Declare and use:
```stasha
stack fn*(i32, i32): i32 math_op = &add;
i32 result = math_op(10, 32);
```

The domain tags (like `heap`) are enforced by the compiler — you cannot pass a stack pointer to a function pointer expecting a heap pointer.

---

## The `future` Type

```stasha
future f = thread.(compute)(arg);
i32 result = future.get.(i32)(f);
future.drop(f);
```

See [Concurrency](Concurrency) for full details.

---

## The `zone` Type

```stasha
zone scratch;   // arena allocator
```

Can be passed to functions:
```stasha
fn fill_from(zone z, i32 n): stack u8 *rw {
    ret new.(n) in z;
}
```

See [Memory Management](Memory-Management).

---

## Type Casting

Use explicit C-style casts:
```stasha
stack i32 n = (i32)3.99;          // f64 → i32 (truncates to 3)
stack f64 f = (f64)count;         // i32 → f64
stack u8  b = (u8)char_val;       // narrowing cast
stack i32 *rw p = (i32 *rw)raw_ptr;  // pointer cast (unsafe)
```

---

## Type Sizes

```stasha
sizeof.(i32)       // 4
sizeof.(f64)       // 8
sizeof.(Point)     // depends on fields and alignment
sizeof.([]i32)     // 24 (ptr + len + cap, 8 bytes each on 64-bit)
```

`sizeof.()` is evaluated at compile time. Use it with `new.()`:
```stasha
heap i32 *rw p = new.(sizeof.(i32) * count);
```

---

## Summary Table

| Category | Types |
|----------|-------|
| Signed integers | `i8` `i16` `i32` `i64` |
| Unsigned integers | `u8` `u16` `u32` `u64` |
| Floats | `f32` `f64` |
| Boolean | `bool` |
| No value | `void` |
| Error | `error` |
| Pointer | `T *perm`, `?T *perm` |
| Slice | `[]T` |
| Array | `T[N]` |
| Struct | `type Foo: struct { ... }` |
| Enum | `type Foo: enum { ... }` |
| Union | `type Foo: union { ... }` |
| Interface | `type Foo: interface { ... }` |
| Alias | `type Foo: T` |
| Generic | `type Foo: @comptime[T] struct { ... }` |
| Inline union | `any.[T1, T2, ...]` |
| Function pointer | `fn*(params): ret` |
| Future | `future` |
| Zone | `zone` |
