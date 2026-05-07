# C Interoperability

C interop is a first-class feature in Stasha. You can call any C function with a single declaration, import C headers directly, and export Stasha code to C with no friction.

---

## Calling C Standard Libraries

Use `lib` to link a C library:

```stasha
lib "stdio";            // link -lc, expose stdio symbols
lib "stdio" = io;       // alias as 'io'
lib "math";             // link libm
lib "string" = str;     // link libc string.h, alias as 'str'
```

Then call:
```stasha
lib "stdio" = io;

io.printf("hello %s %d\n", "world", 42);
io.fprintf(io.stderr, "error: %s\n", msg);
```

---

## Importing C Headers with `cheader`

`cheader` parses a C header and imports all its types, enums, and function declarations directly into the current module:

```stasha
cheader "stdio.h";

ext fn main(void): void {
    printf("hello from cheader\n");   // called directly, no prefix
}
```

With a search path for custom headers:
```stasha
cheader "mylib.h" search "./include";
lib "mylib" from "libmylib.a";
```

The `cheader` approach is ideal when:
- You have a complex C struct hierarchy
- You don't want to manually re-declare every type
- You're doing heavy C interop

---

## Manually Declaring C Functions

For simpler cases, declare C functions yourself:

```stasha
lib "stdio" = io;

// Stasha doesn't need a declaration for libc functions exposed via `lib`,
// but if you need to declare a function explicitly:
ext fn malloc(u64 size): heap void *rw;
ext fn free(heap void *rw ptr): void;
```

---

## C Type Mapping

| C type | Stasha type |
|--------|-------------|
| `void` | `void` |
| `char`, `int8_t` | `i8` |
| `unsigned char`, `uint8_t` | `u8` |
| `short`, `int16_t` | `i16` |
| `unsigned short`, `uint16_t` | `u16` |
| `int`, `int32_t` | `i32` |
| `unsigned int`, `uint32_t` | `u32` |
| `long`, `int64_t` | `i64` |
| `unsigned long`, `uint64_t` | `u64` |
| `float` | `f32` |
| `double` | `f64` |
| `size_t`, `uintptr_t` | `u64` |
| `char *` | `i8 *rw` or `i8 *r` |
| `void *` | `void *rw` |

---

## C-Compatible Structs

Use `@c_layout` to guarantee the same memory layout as a C struct:

```stasha
type @c_layout Point: struct {
    ext i32 x;
    ext i32 y;
}
```

Use `@packed` to match packed C structs:
```stasha
type @packed WireHeader: struct {
    ext u8  version;
    ext u16 length;
    ext u32 checksum;
}
```

---

## Passing Structs to C

```stasha
cheader "mylib.h" search "./examples";
lib "mylib" from "libmylib.a";

ext fn main(void): void {
    mylib_point_t pt = .{ .x = 2, .y = 5 };
    i32 sum = mylib_sum(&pt);
    print.("sum: {}\n", sum);
}
```

Where `mylib.h` defines:
```c
typedef struct { int x, y; } mylib_point_t;
int mylib_sum(mylib_point_t *pt);
```

---

## Exporting Stasha Functions to C

Any `ext fn` in a Stasha library is exported with C linkage. C code can call it directly:

```stasha
// In stasha_lib.sts
mod stasha_lib;

ext type @c_layout Vec2: struct {
    ext f32 x, y;
}

ext fn vec2_dot(Vec2 a, Vec2 b): f32 {
    ret a.x * b.x + a.y * b.y;
}
```

Compile:
```bash
stasha lib stasha_lib.sts -o libstasha_lib.a
```

In C:
```c
typedef struct { float x, y; } Vec2;
float vec2_dot(Vec2 a, Vec2 b);

int main() {
    Vec2 a = {1.0f, 0.0f};
    Vec2 b = {0.0f, 1.0f};
    float d = vec2_dot(a, b);  // 0.0
}
```

---

## Memory Ownership Between C and Stasha

This is the most important aspect of C interop:

### Stasha allocates, Stasha frees

```stasha
heap u8 *rw buf = new.(256);
some_c_function(buf, 256);   // C reads/writes buf
rem.(buf);                    // Stasha frees
```

### C allocates, C frees

```stasha
lib "stdlib";

heap void *rw raw = malloc(256);
use_buffer((u8 *rw)raw, 256);
free(raw);   // C function frees it
```

### C allocates, Stasha frees (ownership transfer)

When C gives you ownership (like `strdup`):
```stasha
lib "string" = str;
lib "stdlib";

heap i8 *rw copy = str.strdup("hello");
// ... use copy ...
free(copy);   // must use C's free() since it was malloc'd by C
```

**Rule of thumb:** Free with the same allocator that allocated. Don't call `rem.()` on memory that C allocated with `malloc()` (they may use different allocators).

---

## String Interop

C strings are null-terminated `char *`. Stasha strings are null-terminated `i8 *`:

```stasha
lib "stdio" = io;
lib "string" = str;

stack i8 *r greeting = "hello";   // string literal
io.printf("%s, world\n", greeting);

// String length:
u64 len = str.strlen(greeting);
print.('{}\n', len);   // 5

// String comparison:
bool eq = str.strcmp(greeting, "hello") == 0;
```

For mutable C strings:
```stasha
heap i8 *rw buf = new.(64);
defer rem.(buf);
str.strcpy(buf, "hello");
str.strcat(buf, " world");
io.printf("%s\n", buf);
```

---

## Inline Assembly

For low-level hardware access:

```stasha
fn nop(void): void {
    asm { "nop" }
}

fn get_tsc(void): u64 {
    u64 lo, hi;
    asm {
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    }
    ret (hi << 32) | lo;
}
```

---

## Real Example: Using stdio + string

```stasha
mod stdio_demo;

lib "stdio"  = io;
lib "string" = str;
lib "stdlib";

ext fn main(void): i32 {
    heap i8 *rw buf = malloc(128);
    defer free(buf);

    str.snprintf(buf, 128, "Hello from C! Value = %d\n", 42);
    io.printf("%s", buf);

    i32 len = (i32)str.strlen(buf);
    print.('string length = {}\n', len);

    ret 0;
}
```

---

## Variadic C Functions

Variadic C functions (like `printf`) work naturally through `lib`:

```stasha
lib "stdio" = io;

io.printf("name=%s age=%d\n", name, age);
io.fprintf(io.stderr, "error code %d\n", code);
```
