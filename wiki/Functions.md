# Functions

Functions are the basic unit of code in Stasha. They have explicit visibility, typed parameters, and can return multiple values.

---

## Basic Declaration

```stasha
fn name(parameters): return_type {
    body
}
```

```stasha
fn add(i32 a, i32 b): i32 {
    ret a + b;
}
```

Call it:
```stasha
stack i32 result = add(3, 4);
```

---

## Visibility

Every function must be either `ext` (exported) or `int` (internal):

```stasha
ext fn public_fn(): void { }    // callable from other modules
int fn helper(): void { }       // only callable within this module
```

If you omit the qualifier, the function defaults to `ext`.

---

## Parameters

Parameters always have a type. The storage qualifier on pointer parameters tells the compiler the domain:

```stasha
fn process(stack i32 n, stack f64 scale): f64 { ... }
fn fill(heap u8 *rw buf, stack i32 len): void { ... }
fn read_only(stack i32 *r data, stack i32 n): i32 { ... }
```

Multiple parameters of the same type can be listed without repeating the type:

```stasha
fn add3(i32 a, b, c): i32 {
    ret a + b + c;
}
```

---

## Return Types

```stasha
fn nothing(void): void { }
fn value(void): i32 { ret 42; }
```

### Multiple Return Values

Use `[T1, T2, ...]` to return multiple values:

```stasha
fn min_max(stack i32 a, stack i32 b): [i32, i32] {
    if a < b { ret a, b; }
    ret b, a;
}

// Destructure with let:
let [lo, hi] = min_max(3, 7);
```

### Returning Errors

The canonical pattern:

```stasha
fn parse(stack i8 *r s): [i32, error] {
    if *.(s) == 0 { ret 0, error.('empty string'); }
    ret parse_number(s), nil;
}

let [val, err] = parse(input);
if err != nil { print.('error: {}\n', err); ret 1; }
```

---

## Expression Body (Fat Arrow)

For simple one-expression functions, use `=>` instead of `{ ret ...; }`:

```stasha
fn double(i32 x): i32 => x * 2;
fn add(i32 a, i32 b): i32 => a + b;
fn square(f64 x): f64 => x * x;
```

This works for methods too:

```stasha
type Foo: struct {
    ext i32 x;
    ext fn get_x(void): i32 => this.x;
    ext fn doubled(void): i32 => this.x * 2;
}
```

---

## Static Methods (Constructors)

Define static methods outside the struct body using `fn TypeName.method_name()`. These are the idiomatic way to write constructors:

```stasha
type Point: struct {
    ext i32 x, y;
}

fn Point.new(i32 x, i32 y): Point {
    ret Point { .x = x, .y = y };
}

// Call with Type.(args) syntax:
Point p = Point.(3, 4);
```

The `.(args)` call syntax invokes the `.new()` constructor automatically:
```stasha
Point p = Point.(3, 4);         // calls Point.new(3, 4)
```

---

## Instance Methods (Inside Struct Body)

Methods defined inside the struct body have access to `this`:

```stasha
type Counter: struct {
    int i32 value;

    ext fn increment(void): void {
        this.value = this.value + 1;
    }

    ext fn get(void): i32 => this.value;

    ext fn reset(void): void {
        this.value = 0;
    }
}

fn Counter.new(void): Counter {
    ret Counter { .value = 0 };
}
```

**`this` is only valid inside inline methods.** Using `this` in an external `fn Type.method()` is a compile error.

---

## Destructors

A method named `rem` is automatically called when the value goes out of scope:

```stasha
type Buffer: struct {
    heap u8 *rw data;
    int i32 size;

    ext fn rem(void): void {
        rem.(this.data);   // free the heap allocation
    }
}

fn Buffer.new(i32 size): Buffer {
    heap u8 *rw d = new.(size);
    ret Buffer { .data = d, .size = size };
}

// Usage:
{
    Buffer b = Buffer.(64);
    // ... use b ...
}   // b.rem() called automatically here
```

---

## Generic Functions

Use `@comptime[T]` to write type-parameterized functions:

```stasha
fn @comptime[T] identity(stack T val): T {
    ret val;
}

fn @comptime[T] swap(stack T *rw a, stack T *rw b): void {
    stack T tmp = a[0];
    a[0] = b[0];
    b[0] = tmp;
}
```

Instantiate at the call site with `.[Type]`:
```stasha
stack i32 n = identity.[i32](99);
stack f64 f = identity.[f64](3.14);
swap.[i32](&a, &b);
```

---

## Variadic Functions

```stasha
ext fn my_printf(stack i8 *r fmt, ...): void { ... }
```

Variadic functions are mainly used for C interop.

---

## Function Pointers

```stasha
// Declare
stack fn*(i32, i32): i32 op = &add;

// Call
i32 result = op(10, 20);

// Reassign
op = &multiply;
```

With domain-tagged pointer parameters:
```stasha
stack fn*(heap u8 *rw, i32): void writer = &fill_buffer;
```

---

## Error Propagation

Use `fn.?(args)` to propagate errors automatically:

```stasha
fn open_and_read(stack i8 *r path): [[]u8, error] {
    // .? calls the function; if it returns an error,
    // THIS function immediately returns that error too.
    let [fd, _] = open_file.?(path);
    let [data, _] = read_all.?(fd);
    ret data, nil;
}
```

You can also use the postfix `?` on expressions:
```stasha
fn pipeline(): error {
    risky_op()?;    // propagate if error
    ret nil;
}
```

---

## The `with` Statement

`with` combines a multi-return call with a scope guard and destructuring:

```stasha
fn divide(i32 a, i32 b): [i32, error] {
    if b == 0 { ret 0, error.('division by zero'); }
    ret a / b, nil;
}

// Only executes the body if err == nil:
with let [result, err] = divide(10, 2); err == nil {
    print.('result = {}\n', result);
} else {
    print.('error: {}\n', err);
}
```

The variables `result` and `err` are scoped to the `with` block.

---

## Inline Assembly

```stasha
fn no_op(void): void {
    asm { "nop" }
}

fn rdtsc(void): u64 {
    u64 lo, hi;
    asm {
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :
        :
    }
    ret (hi << 32) | lo;
}
```

---

## Attributes

| Attribute | Effect |
|-----------|--------|
| `@weak` | Function can be overridden at link time |
| `@hidden` | Not exported from shared libraries |
| `@frees` | Parameter attribute: function takes ownership, must call `rem.()` |

```stasha
@weak
ext fn default_handler(void): void { ... }

@hidden
int fn internal_only(void): void { ... }

ext fn take_ownership(@frees heap u8 *rw buf): void {
    rem.(buf);   // required on all paths
}
```

---

## `restrict` on Pointer Parameters

Tell the compiler that two pointer parameters don't alias. Enables auto-vectorization:

```stasha
int fn add_arrays(
    stack restrict i32 *rw dst,
    stack restrict i32 *r  src,
    stack i32 n
): void {
    for (stack i32 i = 0; i < n; i++) {
        dst[i] = dst[i] + src[i];
    }
}
```

---

## Common Mistakes

**Using `return` instead of `ret`:**
```stasha
return 42;   // ERROR — Stasha uses ret
ret 42;      // correct
```

**Using `this` outside a struct method body:**
```stasha
fn Foo.method(): void {
    this.x = 1;   // ERROR — this is only valid inside inline methods
}
```

**Forgetting to propagate errors:**
```stasha
fn risky(): error { ... }

fn caller(): void {
    risky();          // error return ignored!
    let err = risky();   // better — check it
    if err != nil { handle_error(err); }
}
```
