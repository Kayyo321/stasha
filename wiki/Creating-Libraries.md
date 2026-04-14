# Creating Libraries

Stasha can compile your code into static libraries (`.a`), dynamic libraries (`.dylib`/`.so`), or make it importable as a Stasha module by other projects.

---

## Static Library

### 1. Write the Library

`veclib.sts`:
```stasha
mod veclib;

ext type Vec2: struct {
    ext f32 x, y;
}

ext fn Vec2.new(f32 x, f32 y): Vec2 {
    ret Vec2 { .x = x, .y = y };
}

ext fn dot(Vec2 a, Vec2 b): f32 {
    ret a.x * b.x + a.y * b.y;
}

ext fn length(Vec2 v): f32 {
    ret 0.0;   // simplified — use math.sqrt in practice
}
```

### 2. Compile to `.a`

```bash
stasha lib veclib.sts -o libveclib.a
```

This produces `libveclib.a`.

### 3. Use It

In another file:
```stasha
mod main;

lib "veclib" from "libveclib.a";
imp veclib;

ext fn main(void): i32 {
    veclib.Vec2 a = veclib.Vec2.(1.0, 0.0);
    veclib.Vec2 b = veclib.Vec2.(0.0, 1.0);
    f32 d = veclib.dot(a, b);
    print.('dot = {}\n', d);
    ret 0;
}
```

Note: The library path is resolved **relative to the source file** that imports it.

---

## Dynamic Library

```bash
stasha dylib mylib.sts -o libmylib.dylib   # macOS
stasha dylib mylib.sts -o libmylib.so      # Linux
```

Use dynamic libraries when:
- You want to update the library without relinking the application
- You're writing a plugin system
- You're providing a shared library for C consumers

---

## Library Design Tips

### Use `ext` and `int` Carefully

Only export what users need:

```stasha
// These are exported — part of the public API:
ext type Buffer: struct { ... }
ext fn Buffer.new(i32 size): Buffer { ... }
ext fn Buffer.write(u8 val): void { ... }
ext fn Buffer.read(): u8 { ... }

// These are private implementation details:
int fn validate_state(): bool { ... }
int i32 global_count = 0;
```

### Document with Comments

```stasha
// Creates a new buffer with the given capacity.
// The caller must call rem.() or let scope auto-destruct.
ext fn Buffer.new(i32 capacity): Buffer { ... }
```

### Write Tests

Include test blocks in your library files:

```stasha
test 'buffer write then read' {
    Buffer b = Buffer.(16);
    b.write(42);
    expect_eq.(b.read(), 42);
}
```

Run with `stasha test veclib.sts`.

---

## Exporting for C Consumers

If you want C code to call your Stasha library, use `ext` on functions and mark structs with `@c_layout`:

```stasha
ext type @c_layout point_t: struct {
    ext i32 x;
    ext i32 y;
}

ext fn point_sum(point_t a, point_t b): point_t {
    ret .{ .x = a.x + b.x, .y = a.y + b.y };
}
```

Then compile:
```bash
stasha lib mylib.sts -o libmylib.a
```

In C:
```c
typedef struct { int x, y; } point_t;
point_t point_sum(point_t a, point_t b);

int main() {
    point_t a = {1, 2};
    point_t b = {3, 4};
    point_t c = point_sum(a, b);  // {4, 6}
}
```

---

## Standard Library Module Convention

stdlib modules follow this pattern:
1. Source in `stsstdlib/<category>/<name>.sts`
2. Built with `make stdlib` → `bin/stdlib/lib<name>.a`
3. Imported with `libimp "<name>" from std;`

If you want your library to be distributable as a "stdlib-style" module:

1. Put the source in a known location
2. Build it: `stasha lib mymod.sts -o bin/stdlib/libmymod.a`
3. Users import with: `libimp "mymod" from std;`

---

## `sts.sproj` for a Library Project

```
main    = "src/lib.sts"
library = "bin/libmylib.a"
```

Running `stasha` in the project root will build the library.

---

## Versioning and Compatibility

Currently, Stasha libraries don't have a built-in version system. Convention:
- Include the version in the library name: `libmylib-1.0.a`
- Keep the interface `.sts` file next to the `.a` file
- Document breaking changes in comments at the top of the interface file
