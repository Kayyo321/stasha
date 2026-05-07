# Generics

Stasha supports compile-time generics through the `@comptime[T]` annotation. Generic functions and structs are instantiated at compile time for each concrete type they're used with — similar to C++ templates, but with cleaner syntax.

---

## Generic Functions

Declare a generic function by adding `@comptime[T]` before `fn`:

```stasha
fn @comptime[T] identity(stack T val): T {
    ret val;
}
```

Instantiate at the call site with `.[ConcreteType]`:

```stasha
stack i32 n = identity.[i32](99);
stack f64 f = identity.[f64](3.14);
stack bool b = identity.[bool](true);
```

---

## Common Generic Functions

### Swap

```stasha
fn @comptime[T] swap(stack T *rw a, stack T *rw b): void {
    stack T tmp = a[0];
    a[0] = b[0];
    b[0] = tmp;
}

stack i32 x = 10, y = 20;
swap.[i32](&x, &y);
// x == 20, y == 10
```

### Clamp

```stasha
fn @comptime[T] clamp(stack T val, stack T lo, stack T hi): T {
    if val < lo { ret lo; }
    if val > hi { ret hi; }
    ret val;
}

stack i32 safe = clamp.[i32](150, 0, 100);   // 100
stack f64 unit = clamp.[f64](-0.5, 0.0, 1.0); // 0.0
```

### Min / Max

```stasha
fn @comptime[T] gmin(stack T a, stack T b): T {
    if a < b { ret a; }
    ret b;
}

fn @comptime[T] gmax(stack T a, stack T b): T {
    if a > b { ret a; }
    ret b;
}

i32 smaller = gmin.[i32](3, 7);   // 3
f64 larger  = gmax.[f64](1.5, 2.3);  // 2.3
```

### Generic Contains (on slices)

```stasha
fn @comptime[T] contains(stack []T s, stack T val): bool {
    for (stack i32 i = 0; i < len.(s); i++) {
        if s[i] == val { ret true; }
    }
    ret false;
}

heap []i32 nums = make.([]i32, 5);
defer rem.(nums);
// ... fill nums ...
bool found = contains.[i32](nums, 42);
```

---

## Generic Structs

Parameterize a struct with `@comptime[T]`:

```stasha
type Point: @comptime[T] struct {
    ext T x;
    ext T y;
}
```

Define static methods for generic structs:

```stasha
fn @comptime[T] Point.new(stack T x, stack T y): Point.[T] {
    ret Point.[T] { .x = x, .y = y };
}
```

Instantiate:
```stasha
Point.[i32] pi = Point.[i32].new(1, 2);
Point.[f64] pf = Point.[f64].new(1.5, 2.7);

print.('{} {}\n', pi.x, pi.y);   // 1 2
print.('{} {}\n', pf.x, pf.y);   // 1.5 2.7
```

---

## Generic Functions on Generic Structs

Write standalone generic functions that work on generic structs:

```stasha
fn @comptime[T] point_add(stack Point.[T] a, stack Point.[T] b): Point.[T] {
    ret Point.[T] { .x = a.x + b.x, .y = a.y + b.y };
}

Point.[i32] p1 = Point.[i32].new(1, 2);
Point.[i32] p2 = Point.[i32].new(3, 4);
Point.[i32] p3 = point_add.[i32](p1, p2);
// p3.x == 4, p3.y == 6
```

---

## Multiple Type Parameters

```stasha
type Pair: @comptime[K, V] struct {
    ext K key;
    ext V value;
}

fn @comptime[K, V] Pair.new(K k, V v): Pair.[K, V] {
    ret Pair.[K, V] { .key = k, .value = v };
}

Pair.[i32, bool] entry = Pair.[i32, bool].new(1, true);
```

---

## Interface-Constrained Generics

Require that a generic type implements an interface:

```stasha
type Entity: interface {
    id(void): i32;
}

// T must implement Entity:
fn @comptime[T.[Entity]] log_id(stack T val): void {
    print.('id = {}\n', val.id());
}

// Generic struct with constraint:
type EntityHolder: @comptime[T.[Entity]] struct {
    int stack T *r entity;

    ext fn print_id(void): void {
        print.('Entity id: {}\n', this.entity.id());
    }
}
```

If you try to instantiate with a type that doesn't implement `Entity`, the compiler rejects it.

---

## Generic Slices

Use generics to write functions that work on any slice type:

```stasha
fn @comptime[T] reverse(stack []T s): void {
    stack i32 lo = 0;
    stack i32 hi = len.(s) - 1;
    while lo < hi {
        stack T tmp = s[lo];
        s[lo] = s[hi];
        s[hi] = tmp;
        lo++; hi--;
    }
}

heap []i32 nums = make.([]i32, 5);
defer rem.(nums);
// fill nums with 1,2,3,4,5
reverse.[i32](nums);
// nums is now 5,4,3,2,1
```

---

## How Generics Work Internally

Stasha generics are **monomorphized** at compile time. When you write:

```stasha
identity.[i32](99)
identity.[f64](3.14)
```

The compiler generates two separate functions — one for `i32` and one for `f64`. There's no boxing, no runtime type information, and no overhead compared to writing the function manually for each type.

This is the same model as C++ templates and Rust generics, but with the simpler `@comptime[T]` syntax.

---

## Limitations

- Generic parameters must be known at compile time (hence `@comptime`)
- You cannot pass a generic type as a value (only instantiated concrete types)
- Variadic generic type parameters (`@comptime[T...]`) are not yet supported
- Reflection / type inspection on generic parameters is not yet available

---

## Complete Example

```stasha
mod generics_demo;

lib "stdio" = io;

// Generic stack data structure
type Stack: @comptime[T] struct {
    int heap T *rw data;
    int i32 top;
    int i32 cap;
}

fn @comptime[T] Stack.new(stack i32 initial_cap): Stack.[T] {
    heap T *rw d = new.(sizeof.(T) * initial_cap);
    ret Stack.[T] { .data = d, .top = 0, .cap = initial_cap };
}

fn @comptime[T] Stack.push(stack T val): void {
    if this.top >= this.cap {
        this.cap = this.cap * 2;
        this.data = mov.(this.data, sizeof.(T) * this.cap);
    }
    this.data[this.top] = val;
    this.top = this.top + 1;
}

fn @comptime[T] Stack.pop(void): T {
    this.top = this.top - 1;
    ret this.data[this.top];
}

fn @comptime[T] Stack.empty(void): bool => this.top == 0;

fn @comptime[T] Stack.rem(void): void {
    rem.(this.data);
}

ext fn main(void): i32 {
    Stack.[i32] s = Stack.[i32].new(4);

    s.push(10);
    s.push(20);
    s.push(30);

    while !s.empty() {
        io.printf('%d\n', s.pop());   // 30, 20, 10
    }

    // s.rem() called automatically at scope exit

    ret 0;
}
```
