# Language Reference

Complete syntax reference for the Stasha programming language.

---

## File Structure

```stasha
mod module_name;              // required — first line

// imports
imp other.module;
imp other.module = alias;
lib "clib" [= alias];
lib "clib" from "path/lib.a" [= alias];
libimp "name" from std;
libimp "name" from "path/lib.a";
cheader "header.h" [search "path"];

// top-level declarations (any order)
type Name: struct { ... }
type Name: enum { ... }
type Name: union { ... }
type Name: interface { ... }
type Alias: existing_type;
type Name: @comptime[T] struct { ... }

ext fn name(params): ret { ... }
int fn name(params): ret { ... }

ext type_name var = value;
int type_name var = value;
const type_name var = value;

zone arena_name;   // global zone
tls type_name var = value;

test 'name' { ... }
comptime_assert.(expr);
comptime_if cond { ... } else { ... }
```

---

## Storage Qualifiers

```
stack    heap    atomic    const    final    volatile    tls    restrict
```

---

## Primitive Types

```
void    bool
i8  i16  i32  i64
u8  u16  u32  u64
f32  f64
error
```

---

## Pointer Types

```
T *r         // read-only
T *w         // write-only
T *rw        // read-write (also T *)
T *r+        // read + arithmetic
T *w+        // write + arithmetic
T *rw+       // full: read + write + arithmetic
?T *perm     // nullable pointer (may be nil)
```

---

## Composite Types

```
[]T                          // slice (fat pointer: ptr, len, cap)
T[N]                         // fixed array, size N
fn*(params): ret             // function pointer
fn*(domain T *perm, ...): ret   // function pointer with domain tags
future                       // async result handle
zone                         // arena allocator
any.[T1, T2, ...]            // inline tagged union
```

---

## Operators

```
// Arithmetic
+   -   *   /   %
+%  -%  *%                   // wrapping (C overflow)
+!  -!  *!                   // trapping (abort on overflow)

// Comparison
<   >   <=  >=  ==  !=
.==                          // universal equality

// Logical
&&  ||  !
and  or                      // comparison chain keywords

// Bitwise
&   |   ^   ~   <<   >>

// Assignment
=   +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=

// Increment / decrement
++  --  (prefix and postfix)

// Pointer
&expr                        // address-of
*.(ptr)                      // dereference
ptr[i]                       // indexed access (bounds-checked)
ptr[unchecked: i]            // indexed access (no bounds check)

// Slice
arr[:]                       // full slice
arr[lo:hi]                   // half-open range [lo, hi)
arr[lo:]                     // from lo to end
arr[:hi]                     // from 0 to hi

// Ternary
cond ? a : b

// Error propagation
expr?                        // propagate error from expression
fn.?(args)                   // call and propagate error
```

---

## Literals

```
42          i32
42u         u32
42i64       i64 (suffix i8/i16/i32/i64/u8/u16/u32/u64)
3.14        f64
3.14f       f32
true  false  bool
'hello'     string literal (also "hello")
`a`         char literal (i8)
0xFF        hex integer
0b1010      binary integer
0o17        octal integer
nil         null pointer
```

---

## Initializers

```stasha
Type { .field = val, ... }           // struct literal
.{ .field = val, ... }               // shorthand struct literal
.{}                                  // zero initializer
.{ ..other, .field = val }           // spread and override

.{1, 2, 3}                           // array literal
.{[1] = 5}                           // designated array index
T arr[] = .{1, 2, 3}                 // inferred size
T arr[N] = .{1, 2}                   // partial (rest zeroed)
.{0..5}                              // range [0,5)
.{0..=5}                             // inclusive range [0,5]
.{0..10:2}                           // range with step
.{..arr1, 4, 5}                      // spread array into literal
.{.."hello"}                         // spread string into i8 array
```

---

## Variable Declarations

```stasha
type name = value;
stack type name = value;
heap type *perm name = value;
const type name = value;
final type name = value;
atomic type name = value;
volatile type name = value;
tls type name = value;
type a = v1, b = v2;            // multiple same-type
let name = expr;                // inferred single
let [a, b] = expr;              // inferred multi-return
let [a, b, c] = expr;
```

---

## Function Declaration

```stasha
[ext|int] fn name(params): ret { body }
[ext|int] fn name(params): ret => expr;     // expression body
fn Type.method(params): ret { body }        // static method
fn @comptime[T] name(params): ret { body }  // generic
fn @comptime[T, U] name(params): ret { body }
fn name(stack type name, ...): ret { }      // variadic
```

**Attributes:** `@weak`, `@hidden`, `@frees` (on pointer params)

---

## Struct Declaration

```stasha
type Name: struct {
    ext type field;
    int type field;
    ext type a, b;              // multiple same type
    type field: N;              // bitfield (N bits)
    ext fn method(params): ret { ... }
    ext fn rem(void): void { ... }   // destructor
}

type Name: @packed struct { ... }
type Name: @align(N) struct { ... }
type Name: @c_layout struct { ... }
type Name: @comptime[T] struct { ... }

type Name: struct.[iface1, iface2] { ... }   // implements interfaces
```

---

## Enum Declaration

```stasha
type Name: enum {
    Variant1,
    Variant2,
    Variant3(payload_type),   // tagged variant
}
```

---

## Union Declaration

```stasha
type Name: union {
    ext type field1;
    ext type field2;
}
```

---

## Interface Declaration

```stasha
type Name: interface {
    method(params): ret;
}

type Name: interface.[ParentInterface] {
    method(): void;
}
```

---

## Control Flow

```stasha
if cond { }
if cond { } else { }
if cond { } else if other { } else { }

for (init; cond; update) { }
while (cond) { }
do { } while (cond);
inf { }
foreach elem in slice { }

break;
continue;

switch (expr) {
    case val: { ... break; }
    case a, b: { ... break; }
    default: { ... }
}

match expr {
    Type.Variant => { }
    Type.Variant(payload) => { }
    Type.Variant(payload) if guard => { }
    int_literal => { }
    -int_literal => { }
    _ => { }
    identifier => { }            // wildcard binding
}

match any.(expr) {
    TypeName(type var) => { }
}
```

---

## Memory

```stasha
new.(bytes)                      // heap alloc, returns *rw
new.(bytes) in zone_name         // zone alloc
rem.(ptr)                        // free heap ptr (or zone)
rem.(nil)                        // safe no-op
mov.(ptr, new_bytes)             // realloc
sizeof.(Type)                    // compile-time size

make.([]T, len)                  // allocate heap slice
make.([]T, len, cap)             // with explicit capacity
append.(slice, val)              // return new/grown slice
copy.(dst, src)                  // copy elements, return count
len.(slice)                      // element count
cap.(slice)                      // backing capacity

zone name { ... }                // lexical zone (auto-freed)
zone name;                       // manual zone

defer stmt;
defer { ... }

unsafe { ... }                   // suppress all safety checks
```

---

## Concurrency

```stasha
future f = thread.(fn)(args);
future.wait(f);
future.drop(f);
i32 r = future.get.(i32)(f);
bool done = future.ready(f);
```

---

## Built-in Operations

```stasha
print.('fmt', args...);
print.error.('fmt', args...);

sizeof.(Type)
hash.(expr)
equ.(a, b)

expect.(cond)                    // test assertion
expect_eq.(a, b)
expect_neq.(a, b)
test_fail.('msg')
```

---

## Compile-Time

```stasha
comptime_assert.(expr);
comptime_if cond { } else { }
#if platform == "macos" { }      // preprocessor conditional
```

Available `comptime_if` identifiers: `platform`, `arch`

Platform values: `"macos"`, `"linux"`, etc.
Arch values: `"aarch64"`, `"x86_64"`, etc.

---

## Modules

```stasha
mod name;
mod dotted.name;

imp dotted.name;
imp dotted.name = alias;

lib "cname";
lib "cname" = alias;
lib "cname" from "path/libname.a";
libimp "name" from std;
libimp "name" from "path/libname.a";
cheader "header.h";
cheader "header.h" search "path";
```

---

## Macros

```stasha
int macro fn name! {
    () => { body };
    (@arg) => { body };
    (@a, @b) => { body };
    (...@args) => {
        @foreach arg : args { ... }
    };
}

ext macro fn name! { ... }         // exported macro

int macro let alias! = token;      // token alias macro
```

---

## Test Blocks

```stasha
test 'description' {
    expect.(cond);
    expect_eq.(a, b);
    expect_neq.(a, b);
    test_fail.('message');
}
```

---

## Special Syntax

```stasha
// Expression body
fn f(): i32 => expr;
fn method(): i32 => this.field;

// Constructor call
Type.(args)                  // calls Type.new(args)

// Error propagation
fn.?(args)                   // propagate error from function call
expr?                        // propagate error from expression

// Comparison chain
x > 10 and < 20              // x > 10 && x < 20
x == 1 or 2 or 3             // x == 1 || x == 2 || x == 3
x != 0 and != -1             // x != 0 && x != -1

// with statement
with let [a, err] = fn(); err == nil { } else { }
with let x = fn(); cond { }

// Inline assembly
asm { "instruction" }
asm {
    "instruction"
    : outputs
    : inputs
    : clobbers
}

// Array length (built-in field)
arr.len

// Interface disambiguation
obj.interface_name.method()

// Deref
*.(ptr)

// Double pointer deref
*.(*(pp))
```
