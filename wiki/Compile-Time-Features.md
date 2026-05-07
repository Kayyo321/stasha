# Compile-Time Features

Stasha has several compile-time facilities that run during parsing or code generation rather than at runtime:

- `@comptime[T]` generic type parameters on structs and functions.
- `@comptime:` struct metadata fields that are removed from runtime layout.
- `@comptime if` platform/architecture conditionals.
- `@comptime assert.(expr, 'message')` invariant checks.
- `sizeof.(T)` and comptime format strings such as `@'{expr}'`.

Generic programming is covered in [Generics](Generics), and compile-time format strings are covered in [Format Strings](Format-Strings). This page focuses on conditionals, assertions, and metadata fields.

---

## Platform Conditionals

`@comptime if` selects a branch at compile time. The inactive branch is not emitted:

```stasha
fn platform_label(void): void {
    @comptime if os == "macos" {
        print.('macOS\n');
    } else {
        @comptime if os == "linux" {
            print.('Linux\n');
        } else {
            print.('other\n');
        }
    }
}
```

Supported target identifiers:

| name | aliases | examples |
|---|---|---|
| operating system | `os`, `platform` | `"macos"`, `"linux"`, `"windows"` |
| CPU architecture | `arch` | `"arm64"`, `"x86_64"` |

`@comptime if` can appear at top level, inside functions, and inside `@comptime:` struct metadata sections.

---

## Compile-Time Assertions

Use `@comptime assert` to stop compilation when an invariant is false:

```stasha
type Vec3: struct {
    ext f32 x, y, z;
}

@comptime assert.(sizeof.(Vec3) == 12, 'Vec3 must be exactly 12 bytes');
@comptime assert.(true);
```

The message argument is optional. Assertions support: constant booleans, integer comparisons (`==`, `!=`, `<`, `>`, `<=`, `>=`), boolean logic (`&&`, `||`, `!`), `sizeof.(T)` checks, and readable compile-time metadata fields — all composable:

```stasha
@comptime assert.(sizeof.(i32) == 4 && sizeof.(i64) == 8, 'size mismatch');
@comptime assert.(sizeof.(i8) == 1 || sizeof.(i8) == 2,   'i8 is 1 or 2 bytes');
@comptime assert.(!(sizeof.(i32) > sizeof.(i64)),          'i32 must not exceed i64');
@comptime assert.(sizeof.(i32) < sizeof.(i64),             'i32 < i64');
```

These expressions are evaluated at compile time via constant folding — no runtime cost.

Assertions can also appear inside functions:

```stasha
fn use_i64(void): void {
    @comptime assert.(sizeof.(i64) == 8, 'i64 must be 8 bytes');
}
```

---

## Compile-Time Struct Fields

A `@comptime:` section inside a struct declares fields that exist only at compile time:

```stasha
type Buffer: struct {
    heap ext u8 *rw data;
    ext i32 len, cap;

@comptime:
    ext i32 align   = 16;
    ext i32 max_cap = 4096;
}

@comptime assert.(Buffer.align == 16);
@comptime assert.(sizeof.(Buffer) == 16);
```

These fields:

- must have constant initializers;
- are readable in compile-time expressions such as `@comptime assert`;
- are stripped from the runtime struct layout;
- can vary by platform when guarded by `@comptime if`.

```stasha
type Handle: struct {
    ext i32 fd;

@comptime:
    @comptime if os == "windows" {
        ext i32 kind = 1;
    } else {
        ext i32 kind = 0;
    }
}
```

See [`examples/ex_comptime.sts`](../examples/ex_comptime.sts) for a complete runnable example.
