# Using Libraries

How to use precompiled Stasha libraries and C libraries in your programs.

---

## Using a Stasha Library

### Step 1: Get the library files

You need two files:
- `libmylib.a` — the compiled archive
- `mylib.sts` — the Stasha interface source (for type-checking)

### Step 2: Import in your code

```stasha
mod main;

lib "mylib" from "libs/libmylib.a";
imp mylib;

ext fn main(void): i32 {
    mylib.some_function();
    mylib.SomeType t = mylib.SomeType.(args);
    ret 0;
}
```

The `lib` path is resolved **relative to the source file** that contains the declaration.

### Step 3: Build

```bash
stasha main.sts -o bin/app
```

---

## Using the Standard Library

After `make stdlib`:

```stasha
libimp "array"  from std;
libimp "dstring" from std;
libimp "map"    from std;
libimp "math"   from std;

ext fn main(void): i32 {
    array_t.[i32] arr = array_t.[i32].new(8);
    defer arr.rem();
    arr.push(42);
    ret 0;
}
```

`from std` resolves to `bin/stdlib/lib<name>.a`.

---

## Using a C Library

```stasha
lib "stdio" = io;
lib "math";
lib "pthread";

ext fn main(void): i32 {
    io.printf("hello from C\n");
    f64 root = math.sqrt(2.0);
    ret 0;
}
```

---

## Using a Custom C Library

```stasha
cheader "mylib.h" search "./include";
lib "mylib" from "libs/libmylib.a";

ext fn main(void): void {
    mylib_result_t r = mylib_compute(42);
    print.("result: {}\n", r.value);
}
```

---

## In a `sts.sproj` Project

```
main     = "src/main.sts"
binary   = "bin/app"
ext_libs = [
    ("libs/engine.a"  : "libs/engine.sts"),
    ("libs/physics.a" : "libs/physics.sts"),
]
```

In `src/main.sts`:
```stasha
mod main;

imp engine;
imp physics;

ext fn main(void): i32 {
    engine.init();
    physics.simulate();
    ret 0;
}
```

---

## `libimp` vs `lib + imp`

These are equivalent:
```stasha
libimp "array" from std;
```
```stasha
lib "array" from std;
imp array;
```

`libimp` is the shorthand. Use it for stdlib modules. Use the expanded form when you need more control (e.g., aliasing the lib import separately).

---

## Finding What's Available

Check the stdlib source at `stsstdlib/` for available modules:

```
collections/  array, list, set, opt, buffer
str/          string, dstring, str_view, str_conv, unicode
math/         math, vector, extra
io/           console, file_stream, logging
map/          map
time/         clock, extra
threading/    mutex
sys/          sys, cl_args
net/          network, http
proc/         process
random/       simple_rng, complex_rng
serial/       json
crypto/       crypto
fs/           filesystem
config/       config
error/        error_helpers
resource/     resource
```
