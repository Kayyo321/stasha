# Structs

Structs are the primary way to group related data in Stasha. They support methods, destructors, constructors, and interface implementations.

---

## Basic Declaration

```stasha
type Name: struct {
    ext i32 field1;
    ext f64 field2;
    int bool private_field;   // int = private
}
```

- `ext` fields are exported (accessible from other modules)
- `int` fields are internal (module-private)

---

## Creating Instances

### Struct Literal

```stasha
type Point: struct {
    ext i32 x;
    ext i32 y;
}

Point p = Point { .x = 3, .y = 4 };
```

### Zero Initializer

```stasha
Point p = .{};                    // all fields zeroed
Point p = Point {};               // same
```

### Designated Initializer

```stasha
Point p = .{ .x = 10, .y = 20 };
```

### Constructor Pattern

Define a `new` static method and call it with `Type.(args)`:

```stasha
fn Point.new(i32 x, i32 y): Point {
    ret Point { .x = x, .y = y };
}

Point p = Point.(3, 4);   // calls Point.new(3, 4)
```

### Spread / Copy-with-Override

```stasha
type Player: struct { ext i32 hp; ext i32 mp; }

Player base = .{ .hp = 100, .mp = 50 };
Player p2   = .{ ..base, .hp = 200 };   // copies base, overrides hp only
```

---

## Methods

Methods are defined **inside** the struct body. They have access to `this`:

```stasha
type Counter: struct {
    int i32 value;

    ext fn increment(void): void {
        this.value = this.value + 1;
    }

    ext fn add(i32 n): void {
        this.value = this.value + n;
    }

    ext fn get(void): i32 => this.value;

    ext fn reset(void): void {
        this.value = 0;
    }
}
```

Call methods with dot syntax:
```stasha
Counter c = Counter.(0);
c.increment();
c.add(10);
print.('{}\n', c.get());   // 11
```

---

## Static Methods (Constructors)

Static methods are defined **outside** the struct body:

```stasha
fn Counter.new(i32 initial): Counter {
    ret Counter { .value = initial };
}
```

`this` is **not** available in static methods — they're just functions that return the type.

---

## Destructors

A method named `rem` is the destructor. It runs automatically at scope exit:

```stasha
type Buffer: struct {
    int heap u8 *rw data;
    int i32 size;

    ext fn write(i32 idx, u8 val): void {
        this.data[idx] = val;
    }

    ext fn rem(void): void {
        rem.(this.data);   // free heap allocation
        print.('Buffer freed\n');
    }
}

fn Buffer.new(i32 sz): Buffer {
    heap u8 *rw d = new.(sz);
    ret Buffer { .data = d, .size = sz };
}

{
    Buffer b = Buffer.(64);
    b.write(0, 42);
    // ... use b ...
}   // Buffer.rem() called automatically here
```

---

## Multiple Fields of the Same Type

```stasha
type Vec3: struct {
    ext f32 x, y, z;   // shorthand
}
```

Equivalent to:
```stasha
type Vec3: struct {
    ext f32 x;
    ext f32 y;
    ext f32 z;
}
```

---

## Nested Structs

```stasha
type Vec2: struct {
    ext f32 x, y;
}

type Player: struct {
    ext Vec2 pos;
    ext Vec2 vel;
    ext i32  hp;
}

Player p = .{
    .pos = .{ .x = 0.0, .y = 0.0 },
    .vel = .{ .x = 1.0, .y = 0.5 },
    .hp  = 100
};

print.('{}\n', p.pos.x);   // 0.0
```

---

## Struct Attributes

### `@packed` — No Padding

```stasha
type @packed WirePacket: struct {
    ext u8  version;
    ext u16 length;
    ext u32 checksum;
}
// sizeof(WirePacket) == 7 (no padding)
```

### `@align(N)` — Alignment

```stasha
type @align(16) SimdData: struct {
    ext f32 v[4];
}
```

### `@c_layout` — C ABI Compatible

Guarantees the same memory layout as an equivalent C struct. Use when passing structs to C functions:

```stasha
type @c_layout CPoint: struct {
    ext i32 x;
    ext i32 y;
}
```

---

## Bitfields

```stasha
type Flags: struct {
    ext i32 read  : 1;
    ext i32 write : 1;
    ext i32 exec  : 1;
    ext i32 padding : 29;   // fill to 32 bits
}

Flags f = .{};
f.read  = 1;
f.write = 1;
```

---

## Generics

Generic structs use `@comptime[T]`:

```stasha
type Pair: @comptime[T] struct {
    ext T first;
    ext T second;
}

fn @comptime[T] Pair.new(T a, T b): Pair.[T] {
    ret Pair.[T] { .first = a, .second = b };
}

Pair.[i32] p = Pair.[i32].new(1, 2);
print.('{} {}\n', p.first, p.second);   // 1 2
```

Multiple type parameters:
```stasha
type Map_Entry: @comptime[K, V] struct {
    ext K key;
    ext V value;
}

Map_Entry.[i32, bool] e = Map_Entry.[i32, bool] { .key = 1, .value = true };
```

---

## Interface Implementation

Structs can implement interfaces:

```stasha
type Drawable: interface {
    draw(void): void;
}

type Circle: struct.[Drawable] {
    ext f64 radius;

    ext fn draw(void): void {
        print.('Drawing circle with r={}\n', this.radius);
    }
}
```

See [Interfaces](Interfaces) for the full guide.

---

## Custom Hash and Equality

Structs get a default hash based on their fields. Override it by defining `hash`:

```stasha
type Color: struct {
    ext u8 r, g, b;

    ext fn hash(void): u64 {
        u64 h = (u64)this.r;
        h = h << 8 | (u64)this.g;
        h = h << 8 | (u64)this.b;
        ret h;
    }
}

Color c = Color.(255, 0, 0);
u64 h = hash.(c);   // uses the custom hash method
```

---

## Global Struct Zones

Zones can be declared at the module level and shared across functions:

```stasha
zone request_arena;  // module-level zone

fn handle_request(void): void {
    stack u8 *rw buf = new.(256) in request_arena;
    // ... use buf ...
}

fn main(void): i32 {
    defer rem.(request_arena);
    handle_request();
    ret 0;
}
```

---

## `sizeof` on Structs

```stasha
print.('{}\n', sizeof.(Point));        // 8 (two i32s)
print.('{}\n', sizeof.(Vec3));         // 12 (three f32s)

comptime_assert.(sizeof.(WirePacket) == 7);   // verify at compile time
```

---

## Common Mistakes

**Using `this` in a static method:**
```stasha
fn Counter.new(): Counter {
    this.value = 0;   // ERROR: this is not valid here
    ret Counter { .value = 0 };  // correct
}
```

**Forgetting `rem` on a heap struct field:**
```stasha
type Bag: struct {
    heap u8 *rw data;
    // No rem method — data will leak when Bag goes out of scope
}
```

Always define a `rem` destructor if the struct owns heap memory.

**Public field accessing private data through a pointer:**
```stasha
// int fields are only accessible within the module
// External code cannot read or write int fields
```
