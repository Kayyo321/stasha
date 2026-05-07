# Format Strings

Stasha has two ways to produce formatted text: classic placeholder format strings (`'... {} ...'`) and **comptime interpolated strings** (`@'... {expr} ...'`). Both are zero-cost at runtime — formatting is fully resolved at compile time.

---

## Classic `print.()` Format Strings

The first argument is a string literal. Each `{}` placeholder consumes one trailing argument:

```stasha
print.('hello\n');
print.('x = {}, y = {}\n', x, y);
```

### Format Specifiers

A colon inside `{}` introduces a format spec:

```stasha
print.('{:08x}', 255);          // → 000000ff
print.('{:+}',   42);           // → +42
print.('{:.2}', 3.14159);       // → 3.14
print.('{:<10}', "hi");          // → "hi        "
```

| Spec | Effect |
|------|--------|
| `x` / `X` | Lowercase / uppercase hex |
| `b` | Binary |
| `o` | Octal |
| `.N` | Float precision (N digits after decimal) |
| `<N` | Left-align in width N |
| `N` | Right-align in width N |
| `0N` | Zero-pad to width N |
| `+` | Force sign for positive numbers |
| `#` | Alternate prefix: `0x`, `0b`, `0` |

Specs **compose**: `{:+#08x}` means signed-positive, `0x` prefix, zero-padded, width 8, hex.

### Escaping

A literal `{` is written `\{`:

```stasha
print.('use \{ for a literal brace\n');
```

A literal `}` does not need escaping outside a placeholder.

### Type Inference

The format spec is **derived from the argument type** when no explicit spec is given. `{}` on an `i32` formats as decimal; on an `f64` as a default-precision float; on a slice as `[a, b, c]`. Override with an explicit spec when needed.

```stasha
stack i32 n = 42;
stack f64 f = 3.14;
stack []i32 v = make.{1, 2, 3};

print.('{} {} {}\n', n, f, v);          // 42 3.14 [1, 2, 3]
print.('{:08x}\n', n);                  // 0000002a
```

---

## Comptime Interpolated Strings: `@'...'`

The `@'...'` literal evaluates expressions inline. Each `{expr}` is replaced with the formatted value of `expr` at the call site:

```stasha
stack i32 a = 1, b = 2, c = 3;
print.(@'{a} + {b} * {c} = {a + b * c}\n');
// → 1 + 2 * 3 = 7
```

Unlike a runtime template string, **no parsing happens at runtime**. The compiler:

1. Scans the literal at compile time.
2. Splits it into static chunks and `{expr}` slots.
3. Rewrites the call as `print.('chunk1 {} chunk2 {}\n', expr1, expr2)`.

So `@'...'` desugars cleanly to the placeholder form — same speed, same codegen, same format specs.

### Format Specs Inside `{}`

Format specs work the same as in classic strings:

```stasha
print.(@'value = {value:08x} ({value:+})\n');
print.(@'pi = {3.14159:.4}\n');                  // pi = 3.1416
```

### Field Access and Calls

Any expression that produces a printable value works:

```stasha
type Point: struct { ext i32 x, y; }
Point p = .{ .x = 3, .y = 4 };

print.(@'point = ({p.x}, {p.y})\n');             // point = (3, 4)
print.(@'sum = {p.x + p.y}\n');                  // sum = 7
print.(@'company = {greeter:Builder:company()}\n');
```

The `:` inside an `@'...'` interpolation is the **static-access** operator — see [Submodules](Submodules) for when to use `:` versus `.`.

### Strings That Live On Their Own

Comptime interpolation also works as the value of any string-producing expression:

```stasha
fn ret_it(stack i32 it): heap i8 *rw {
    ret heap @'it = {it}';
}

heap i8 *rw s = ret_it(123);
defer rem.(s);
print.('{}\n', s);                                // it = 123
```

The `heap @'...'` returns a fresh heap-allocated string with ownership transferred to the caller. Use `defer rem.(s)` at the call site, or pass through to another consumer.

For stack-allocated, the storage qualifier is inferred from context:

```stasha
stack i8 *rw banner = @'== {title} ==';
```

(Where the slice is alloca-backed and lives until the end of the enclosing function.)

### Raw Strings

To print a string variable that **already contains** `{}` characters without interpolating them, use the classic form with a single `{}`:

```stasha
heap i8 *rw msg = @'{x} + {y} = {x + y}';        // contents: "1 + 2 = 3"
print.('{}\n', msg);                              // prints once, no re-interpretation
```

Or write a `@'...'` with a literal-brace escape:

```stasha
print.(@'\{not interpolated\}\n');                // → {not interpolated}
```

---

## When to Use Which

| Goal | Use |
|------|-----|
| Single fixed literal with one or two values | `print.('hello {}\n', name);` |
| Many interleaved expressions | `print.(@'x={x} y={y} sum={x+y}\n');` |
| Returning a formatted string | `ret heap @'...{...}...';` |
| Programmatic format string (variable template) | Not supported — both forms require a literal |
| Logging that needs structured fields | Build a logger that takes `{}` placeholders |

`@'...'` shines when the format and the data are written together. Classic `'...'` is better when the format is fixed and the data is a small, mostly-positional list.

---

## Compile-Time Validation

Both forms validate at compile time:

- The **placeholder count** must match the number of trailing arguments (classic form).
- Each `{expr}` in `@'...'` must reference a name in scope.
- Each format spec is parsed; invalid specs produce a compile error pointing at the offending `{...}`.

```
error: format spec ':08q' is not recognised (valid: x, X, b, o, .N, <N, N, 0N, +, #)
```

Runtime panics for format mismatch are impossible — every issue surfaces at compile time.

---

## Edge Cases

**Nested braces in expressions** — Stasha allows expressions inside `{...}` to use `{}` as long as they are balanced. The interpolator counts brace depth:

```stasha
print.(@'set = {struct_t{.a=1, .b=2}.a}\n');   // → set = 1
```

**Multi-line interpolation** — A single `@'...'` may not span multiple lines (no implicit string concatenation). Use the classic form with `\n`:

```stasha
print.('first line\nsecond line: {}\n', val);
```

**Interpolation in single quotes vs double quotes** — Both `@'...'` and `@"..."` work; the choice between single and double quotes is purely about which one your literal contains.

**Aliases in submodules** — `@'{outer:sub:name()}'` resolves the same as `outer:sub:name()`. There is no shorter alias syntax inside an interpolation.

---

## Summary

```stasha
// Classic placeholder
print.('value = {} hex = {:08x}\n', n, n);

// Comptime interpolation
print.(@'value = {n} hex = {n:08x}\n');

// Heap-allocated formatted string
heap i8 *rw msg = heap @'value = {n}';
defer rem.(msg);

// Format specs (work in both forms)
print.(@'{f:.4}, {n:+}, {n:08x}, {s:<10}\n');
```
