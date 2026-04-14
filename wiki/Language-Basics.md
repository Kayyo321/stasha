# Language Basics

This page gives you a fast overview of Stasha's syntax. If you're coming from C or a C-like language, most of this will look familiar — with some powerful additions.

---

## File Structure

Every `.sts` file follows this pattern:

```stasha
mod module_name;      // must be first line

// imports (optional)
imp other.module;
lib "c_library";

// declarations: types, functions, globals
type Foo: struct { ... }

ext fn main(void): i32 {
    // code
    ret 0;
}
```

---

## Semicolons

Statements end with `;`. This is required.

---

## Comments

```stasha
// single-line comment

/* multi-line
   comment */
```

---

## Identifiers

Standard rules: letters, digits, underscores. Must start with a letter or underscore.

```stasha
my_variable
_private
FooBar
count2
```

---

## Visibility: `ext` and `int`

Stasha uses explicit visibility on every declaration:

- `ext` — **external** (exported, visible outside the module)
- `int` — **internal** (private, only visible within the module)

```stasha
ext fn public_fn(): void { }    // callable from other modules
int fn helper(): void { }       // only used inside this file
```

This applies to functions, variables, struct fields, and type declarations.

---

## Storage Qualifiers

Every variable declaration includes a **storage qualifier** that describes how the variable is allocated:

| Qualifier | Meaning |
|-----------|---------|
| `stack` | Stored on the stack (default for local variables) |
| `heap` | Points to heap-allocated memory |
| `const` | Compile-time constant, immutable |
| `final` | Runtime value, cannot be reassigned |
| `atomic` | Thread-safe atomic variable |
| `volatile` | Prevents compiler optimization (for hardware registers, etc.) |
| `tls` | Thread-local storage |

```stasha
stack i32 x = 42;          // stack variable
const i32 MAX = 100;        // compile-time constant
final f64 pi = 3.14159;     // can't be reassigned after init
atomic i32 counter = 0;     // thread-safe
```

The storage qualifier is **required** in many contexts. When omitted on a declaration, the compiler usually infers `stack`.

---

## Primitive Types

```stasha
bool        // true or false
i8  i16  i32  i64    // signed integers
u8  u16  u32  u64    // unsigned integers
f32  f64             // floating point
void                 // no value
error                // error value (nil or message string)
```

---

## Literals

```stasha
42          // i32 literal
42u         // u32 literal
42i64       // i64 literal
3.14        // f64 literal
3.14f       // f32 literal
true false  // bool
'hello'     // string literal (also "hello")
'\n'        // escape sequences: \n \t \r \\ \' \"
0xFF        // hex integer
0b1010      // binary integer
0o17        // octal integer
```

---

## Variable Declaration

```stasha
i32 x = 10;
f64 y = 3.14;
stack bool flag = true;
```

For multiple variables of the same type:

```stasha
stack i32 [a, b, c] = 1, 2, 3;
```

---

## Operators

### Arithmetic
```stasha
x + y    x - y    x * y    x / y    x % y
```

### Wrapping Arithmetic (C-style overflow)
```stasha
x +% y   x -% y   x *% y   // wrap on overflow
```

### Trapping Arithmetic (abort on overflow)
```stasha
x +! y   x -! y   x *! y   // trap (llvm.trap) on overflow
```

### Comparison
```stasha
x == y   x != y   x < y   x > y   x <= y   x >= y
```

### Logical
```stasha
x && y   x || y   !x
```

### Bitwise
```stasha
x & y   x | y   x ^ y   ~x   x << y   x >> y
```

### Assignment
```stasha
x = y
x += y   x -= y   x *= y   x /= y   x %= y
x &= y   x |= y   x ^= y   x <<= y  x >>= y
```

### Increment / Decrement
```stasha
x++   x--   ++x   --x
```

### Ternary
```stasha
condition ? value_if_true : value_if_false
```

### Address-of
```stasha
&variable    // take address of a variable
```

### Pointer dereference
```stasha
*.(ptr)      // dereference a pointer
```

---

## Print: Built-in Formatted Output

`print.()` is built in — no import needed.

```stasha
print.('Hello!\n');
print.('x = {}\n', x);
print.('x = {:08x}\n', x);    // hex, zero-padded to 8 chars
print.('y = {:.3}\n', y);     // float with 3 decimal places
```

Format specifiers inside `{}`:

| Spec | Effect |
|------|--------|
| `{}` | Default format |
| `{:x}` | Lowercase hex |
| `{:X}` | Uppercase hex |
| `{:b}` | Binary |
| `{:o}` | Octal |
| `{:.N}` | Float precision (N decimal places) |
| `{:N}` | Right-align in width N |
| `{:<N}` | Left-align in width N |
| `{:0N}` | Zero-pad to width N |
| `{:+}` | Force sign (`+` or `-`) |
| `{:#}` | Alt prefix (`0x`, `0b`, `0`) |

Combine flags: `{:+#010x}` — force sign + `0x` prefix + zero-pad to 10.

To print a literal `{`, write `\{`.

---

## Common Mistakes for New Users

**Missing storage qualifier:**
```stasha
// WRONG
i32 x = 5;         // may work but is ambiguous

// RIGHT
stack i32 x = 5;
```

**Using `return` instead of `ret`:**
```stasha
// WRONG
return 0;

// RIGHT
ret 0;
```

**Forgetting semicolons:**
```stasha
// WRONG
stack i32 x = 5
x = x + 1

// RIGHT
stack i32 x = 5;
x = x + 1;
```

---

## Next Steps

- [Variables](Variables) — Deep dive into storage qualifiers
- [Types](Types) — Full type system
- [Functions](Functions) — Declaring and calling functions
- [Control Flow](Control-Flow) — If, loops, match
