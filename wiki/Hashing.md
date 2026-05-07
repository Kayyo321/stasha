# Hashing

`hash.(expr)` returns a `u64` hash for primitive values and structs.

```stasha
i32 n = 42;
u64 h = hash.(n);
```

Equal input values produce equal hashes within the same implementation. Hash values are intended for hash tables and lookup structures, not stable serialization formats.

---

## Primitive Values

Primitive integers, floats, and booleans can be hashed directly:

```stasha
u64 hi = hash.((i32)42);
u64 hf = hash.(3.14);
u64 ht = hash.(true);
```

---

## Struct Defaults

Structs receive a default structural hash when no custom `hash` method is present:

```stasha
type Point: struct {
    ext i32 x, y;
}

Point a = .{ .x = 3, .y = 4 };
Point b = .{ .x = 3, .y = 4 };

u64 ha = hash.(a);
u64 hb = hash.(b);  // same value as ha
```

The default hash walks the fields in declaration order.

---

## Custom Struct Hashes

Define a `hash(void): u64` method to override the default:

```stasha
type Color: struct {
    ext u8 r, g, b;

    ext fn hash(void): u64 {
        u64 h = (u64) this.r;
        h = h << 8 | (u64) this.g;
        h = h << 8 | (u64) this.b;
        ret h;
    }
}

Color red = .{ .r = 255, .g = 0, .b = 0 };
u64 hr = hash.(red);
```

Use a custom method when a type has a natural key, wants to ignore cache fields, or needs to match an equality method used by a map-like structure.

See [`examples/ex_hash.sts`](../examples/ex_hash.sts).
