# Compound Initializers

Stasha's compound initializer syntax — `.{ }` — is a unified, zero-boilerplate way to create and populate arrays and structs inline. Combined with the spread operator (`..`) and range expressions (`..`), it covers the full spectrum from simple literals to complex nested data without any runtime overhead.

---

## Overview

| Syntax | Meaning |
|--------|---------|
| `.{ v1, v2, v3 }` | Positional array or struct init |
| `.{ .field = v }` | Named struct field init |
| `.{ [i] = v }` | Designated array index init |
| `.{}` | Zero initializer (all bytes zeroed) |
| `.{ ..other }` | Spread: copy from array/struct/string |
| `.{ 0..5 }` | Range: 0, 1, 2, 3, 4 (exclusive end) |
| `.{ 0..=5 }` | Range: 0, 1, 2, 3, 4, 5 (inclusive end) |
| `.{ 0..10:2 }` | Range with step: 0, 2, 4, 6, 8 |
| `.{ ..(0..5) }` | Spread a range expression |

All forms can be freely combined in a single initializer.

---

## Array Initializers

### Basic Positional

Elements are assigned left-to-right starting at index 0.

```stasha
i32 a[] = .{1, 2, 3};           // length inferred: [1, 2, 3]
i32 b[5] = .{1, 2};             // [1, 2, 0, 0, 0] — unspecified slots zeroed
```

When the size is omitted (`[]`), the compiler counts the elements in the initializer (including any ranges or spreads) and infers the array length at compile time.

When the size is explicit, all slots not covered by the initializer are zero-filled.

### Designated Index

Jump to a specific index with `[i] = value`. The cursor advances from that index for any subsequent positional elements.

```stasha
i32 c[5] = .{[2] = 99};              // [0, 0, 99, 0, 0]
i32 d[5] = .{[1] = 10, [3] = 30};   // [0, 10, 0, 30, 0]
i32 e[5] = .{[1] = 10, 11};         // [0, 10, 11, 0, 0] — cursor advances from 1
```

Designators must be non-negative compile-time integer literals.

---

## Struct Initializers

### Named Field Init

Use `.field = value` to set struct fields by name. Fields not mentioned are zero-initialized.

```stasha
type vec2_t: struct { ext f32 x; ext f32 y; }

vec2_t v = .{ .x = 1.0, .y = 2.0 };
```

Order does not matter — field names are matched by name, not position:

```stasha
vec2_t v = .{ .y = 2.0, .x = 1.0 };   // same result
```

### Positional Struct Init

Omit the `.field =` prefix to assign by declaration order:

```stasha
vec2_t v = .{ 1.0, 2.0 };   // x = 1.0, y = 2.0
```

Named and positional forms cannot be mixed in the same initializer.

### Zero Initializer

An empty `.{}` zeroes every field:

```stasha
vec2_t v = .{};        // x = 0.0, y = 0.0
```

This works on both structs and arrays of any type.

### Nested Aggregates

Nested structs and arrays can themselves use `.{ }`:

```stasha
type player_t: struct {
    ext vec2_t pos;
    ext i32 hp;
}

player_t p = .{
    .pos = .{ .x = 10.0, .y = 20.0 },
    .hp  = 100
};
```

The inner `.{ }` adopts the type of the field it is being assigned to — no explicit type annotation is needed.

---

## The Spread Operator (`..`)

The `..` prefix inside `.{ }` expands a value's contents inline at the current cursor position.

### Array Spread

Spread one array into another:

```stasha
i32 a[] = .{1, 2, 3};
i32 b[] = .{..a, 4, 5};        // [1, 2, 3, 4, 5]
i32 c[] = .{0, ..a, 0};        // [0, 1, 2, 3, 0]
```

Elements from `a` are copied one by one. The source array must have a known compile-time size (i.e. it must be a local array variable with an inferred or declared fixed length).

Multiple spreads can appear in the same initializer:

```stasha
i32 x[] = .{1, 2};
i32 y[] = .{3, 4};
i32 z[] = .{..x, ..y};         // [1, 2, 3, 4]
```

### String Spread

Spread a string literal to expand its characters into an `i8` array:

```stasha
i8 s[] = .{.."Hello", .." ", .."World"};
// s = ['H','e','l','l','o',' ','W','o','r','l','d']
print.("{}\n", s);              // Hello World
```

Each character of the string literal becomes one element. No null terminator is automatically appended — if you need one, add it explicitly or rely on the zero-filling of an explicit size:

```stasha
i8 buf[12] = .{.."Hello"};     // ['H','e','l','l','o',0,0,0,0,0,0,0]
```

### Struct Spread (Copy-with-Override)

Spread a struct value inside a struct initializer to copy all its fields, then selectively override individual ones:

```stasha
type player_t: struct {
    ext vec2_t pos;
    ext i32 hp;
    ext i32 mp;
}

player_t base = .{ .pos = .{10.0, 20.0}, .hp = 100, .mp = 50 };

// Copy base entirely, override only hp:
player_t p1 = .{ ..base, .hp = 200 };
// p1.pos = {10.0, 20.0}, p1.hp = 200, p1.mp = 50

// Override multiple fields:
player_t p2 = .{ ..base, .hp = 1, .mp = 1 };
```

The spread must appear **before** any field overrides. It copies the entire source struct, then each subsequent `.field = value` overwrites that field.

> **Note:** The spread source must be the same struct type as the initializer target.

---

## Range Expressions

Ranges generate a sequence of integer values inline inside `.{ }`. They are a purely compile-time feature — all bounds and steps must be integer literals.

### Exclusive Range (`..`)

```stasha
i32 r[] = .{0..5};     // [0, 1, 2, 3, 4]  — end is exclusive
```

### Inclusive Range (`..=`)

```stasha
i32 r[] = .{0..=5};    // [0, 1, 2, 3, 4, 5]  — end is included
```

### Stepped Range (`:step`)

```stasha
i32 r[] = .{0..10:2};  // [0, 2, 4, 6, 8]
i32 r[] = .{1..10:3};  // [1, 4, 7]
```

The step must be a non-zero integer literal. Negative steps are supported for descending sequences:

```stasha
i32 r[] = .{5..0:-1};  // [5, 4, 3, 2, 1]
i32 r[] = .{10..0:-2}; // [10, 8, 6, 4, 2]
```

### Mixing Ranges with Values

Values and ranges can be freely interleaved:

```stasha
i32 a[] = .{-1, 0..5, 99};        // [-1, 0, 1, 2, 3, 4, 99]
i32 b[] = .{0..3, 10, 20..=22};   // [0, 1, 2, 10, 20, 21, 22]
```

### Length Inference with Ranges

When the array size is omitted, the compiler counts the values each range will produce at compile time:

```stasha
i32 r[] = .{0..5};          // length = 5, inferred at compile time
i32 s[] = .{0..10:2};       // length = 5, inferred at compile time
i32 t[] = .{1, 2, 0..3};    // length = 5 (1 + 1 + 3)
```

All range bounds must be compile-time integer constants for length inference to succeed. If they are not, a compile error is issued:

```
error: range bounds and step must be compile-time integer literals
```

---

## Spreading Ranges

Ranges can be spread with `..`, which is equivalent to placing the range directly in the initializer:

```stasha
i32 a[] = .{ ..(0..5) };       // same as .{0..5}
i32 b[] = .{10, ..(0..5), 20}; // [10, 0, 1, 2, 3, 4, 20]
```

The parentheses are required when spreading a range inline to disambiguate from a spread of a variable named `0`.

### Combining Everything

All forms — values, spreads, and ranges — compose freely:

```stasha
i32 base[]  = .{10, 20, 30};
i32 mix[] = .{
    ..(0..5),           // 0 1 2 3 4
    10,                 // 10
    ..(20..=25),        // 20 21 22 23 24 25
    ..base,             // 10 20 30
    ..(0..10:2)         // 0 2 4 6 8
};
// mix = [0,1,2,3,4, 10, 20,21,22,23,24,25, 10,20,30, 0,2,4,6,8]
```

---

## Type Inference and Hint Propagation

The compiler propagates the **target type** of the declaration into the initializer, so the elements inside `.{ }` know what type to produce. This is why you never need to annotate nested compound initializers:

```stasha
// Compiler sees: target type of .{ .pos = ... } is player_t
// So for .pos it knows: field pos is vec2_t
// So for the inner .{ .x = ..., .y = ... } it knows: target is vec2_t
player_t p = .{
    .pos = .{ .x = 1.0, .y = 2.0 },   // no vec2_t annotation needed
    .hp  = 100
};
```

If the compiler cannot determine a target type — for example, when `.{ }` is used in a context without enough type information — it emits:

```
error: compound initializer requires a known target type
```

---

## Error Reference

| Error | Cause |
|-------|-------|
| `compound initializer requires a known target type` | `.{ }` used where the type can't be inferred from context |
| `range bounds and step must be compile-time integer literals` | Range used non-constant bounds |
| `field designators are not valid in array initializers` | `.field = v` used inside an array `.{ }` |
| `array designator index must be a non-negative integer literal` | `[i]` with non-const or negative index |
| `too many values in struct compound initializer 'T'` | More positional values than struct fields |
| `unknown field 'f' in struct 'T'` | `.field = v` where field doesn't exist |
| `unsupported spread expression in compound initializer` | `..expr` where expr is not a known-size local array, string literal, range, or struct |
| `range expressions can only appear in compound initializers` | Range used outside `.{ }` |

---

## Quick Reference

```stasha
// ── Arrays ──────────────────────────────────────────────────────────
i32 a[]   = .{1, 2, 3};              // inferred length
i32 b[5]  = .{1, 2};                 // zero-padded to 5
i32 c[5]  = .{[2] = 99};             // designated index
i32 r1[]  = .{0..5};                 // exclusive range: [0,1,2,3,4]
i32 r2[]  = .{0..=5};                // inclusive range: [0,1,2,3,4,5]
i32 r3[]  = .{0..10:2};              // stepped: [0,2,4,6,8]
i32 r4[]  = .{5..0:-1};              // descending: [5,4,3,2,1]

i32 x[]   = .{1, 2, 3};
i32 y[]   = .{..x, 4, 5};           // spread array: [1,2,3,4,5]
i8  s[]   = .{.."Hi", .."!"};       // spread strings: ['H','i','!']
i32 mix[] = .{0, ..(1..4), ..(5..=7)};  // [0,1,2,3,5,6,7]

// ── Structs ─────────────────────────────────────────────────────────
vec2_t v  = .{};                      // zero init
vec2_t v  = .{ .x = 1.0, .y = 2.0 }; // named fields
vec2_t v  = .{ 1.0, 2.0 };           // positional

player_t base = .{ .hp = 100, .mp = 50, .pos = .{} };
player_t p2   = .{ ..base, .hp = 200 };  // copy + override
```
