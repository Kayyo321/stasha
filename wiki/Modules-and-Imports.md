# Modules and Imports

Every Stasha file is a module. Modules control visibility and let you compose programs from multiple files.

---

## Module Declaration

Every file must start with a `mod` declaration:

```stasha
mod my_module;
```

For nested modules (files in subdirectories), use dots:

```stasha
mod math.vector;        // lives at math/vector.sts
mod parser.expr;        // lives at parser/expr.sts
mod ui.widgets.button;  // lives at ui/widgets/button.sts
```

The dotted name mirrors the directory path relative to the entry file.

---

## Importing Stasha Modules

Use `imp` to import another module. This splices its exported types and functions into the current scope:

```stasha
imp math.vector;
imp parser.expr;
```

With an alias:
```stasha
imp math.vector = vec;
// Use as: vec.Vector3, vec.cross_product(), etc.
```

---

## Importing C Libraries

Use `lib` to link a C library and expose its symbols:

```stasha
lib "stdio";            // link libc stdio, use printf directly
lib "stdio" = io;       // link with alias: io.printf(...)
lib "math";             // link libm
lib "mylib" from "path/libmylib.a";   // custom path
```

### `libimp` — Combined lib + imp

For Stasha stdlib modules built with `make stdlib`:

```stasha
libimp "array"    from std;   // loads bin/stdlib/libarray.a + array.sts interface
libimp "dstring"  from std;
libimp "map"      from std;
```

Custom path:
```stasha
libimp "mylib" from "libs/libmylib.a";
```

---

## Importing C Headers with `cheader`

Parse a C header file and import its types and function signatures directly:

```stasha
cheader "stdio.h";

ext fn main(void): void {
    printf("hello from cheader stdio\n");
}
```

With a search path:
```stasha
cheader "mylib.h" search "./include";
lib "mylib" from "libmylib.a";

ext fn main(void): void {
    mylib_point_t pt = .{2, 5};
    print.("sum: {}\n", mylib_sum(&pt));
}
```

`cheader` imports the C types and functions into the current module's scope. Struct types from the header become usable Stasha types.

---

## Module System Rules

### `ext` vs `int`

- `ext` declarations are exported and accessible by other modules that import this one
- `int` declarations are private and cannot be seen from outside

```stasha
mod math.utils;

int const f64 INTERNAL_SCALE = 0.01;   // not visible outside

ext fn scale(f64 x): f64 {            // visible when imported
    ret x * INTERNAL_SCALE;
}
```

### Resolving Names

When you import a module, its `ext` symbols become available using the module name (or alias) as a prefix:

```stasha
imp math.utils;

f64 result = math.utils.scale(100.0);
// or with alias:
imp math.utils = mu;
f64 result = mu.scale(100.0);
```

For C libraries:
```stasha
lib "stdio" = io;
io.printf("hello\n");
```

---

## The Module System and File Structure

Suppose your project has this layout:

```
src/
  main.sts          mod main;
  math/
    vector.sts      mod math.vector;
    matrix.sts      mod math.matrix;
  parser/
    lexer.sts       mod parser.lexer;
    expr.sts        mod parser.expr;
```

In `main.sts`:
```stasha
mod main;

imp math.vector;
imp math.matrix;
imp parser.lexer;
imp parser.expr;

ext fn main(void): i32 {
    math.vector.Vec3 v = math.vector.Vec3.(1.0, 2.0, 3.0);
    // ...
}
```

---

## Library Workflow

### Creating a Library

```bash
stasha lib mymodule.sts -o libmymodule.a
```

### Using a Library

In your source file:
```stasha
lib "mymodule" from "libmymodule.a";
imp mymodule;

ext fn main(void): i32 {
    mymodule.some_function();
    ret 0;
}
```

### `from std` — Standard Library

After running `make stdlib`, standard library modules are in `bin/stdlib/`:

```stasha
libimp "array"   from std;
libimp "dstring" from std;
libimp "map"     from std;
libimp "math"    from std;
```

---

## Project-Level Imports with `ext_libs`

In `sts.sproj`, declare external precompiled libraries:

```
main     = "src/main.sts"
binary   = "bin/myapp"
ext_libs = [
    ("libs/engine.a" : "libs/engine.sts"),
    ("libs/net.a"    : "libs/net.sts"),
]
```

Then import in your source:
```stasha
imp engine;
imp net;
```

---

## Example: Multi-File Project

`src/math/vector.sts`:
```stasha
mod math.vector;

ext type Vec2: struct {
    ext f32 x, y;
}

ext fn Vec2.new(f32 x, f32 y): Vec2 {
    ret Vec2 { .x = x, .y = y };
}

ext fn dot(Vec2 a, Vec2 b): f32 {
    ret a.x * b.x + a.y * b.y;
}
```

`src/main.sts`:
```stasha
mod main;
imp math.vector;

ext fn main(void): i32 {
    math.vector.Vec2 a = math.vector.Vec2.(1.0, 0.0);
    math.vector.Vec2 b = math.vector.Vec2.(0.0, 1.0);
    f32 d = math.vector.dot(a, b);
    print.('dot = {}\n', d);   // 0.0 (perpendicular)
    ret 0;
}
```

`sts.sproj`:
```
main   = "src/main.sts"
binary = "bin/demo"
```

Build and run:
```bash
stasha
./bin/demo
```
