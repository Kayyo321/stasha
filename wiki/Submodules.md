# Submodules and the `:` Static Operator

A submodule is a named scope **inside** a single source file. It's a lightweight way to group related declarations without splitting them into separate files. Combined with the `:` static-access operator, submodules make it easy to expose a clean public API while keeping helpers private.

```stasha
mod outer;

int mod helpers {            // submodule "helpers", internal to this file
    ext fn util(void): void { ... }
}

fn main(void): i32 {
    helpers.util();          // dot works
    helpers:util();          // colon works for static access
    ret 0;
}
```

---

## Declaring a Submodule

Submodules use the same `mod` keyword as a top-level module declaration, with an optional `int` / `ext` qualifier and a brace-delimited body:

```stasha
[int|ext] mod <name> {
    // type, fn, var, lifecycle decls
}
```

`int` (default) makes the submodule **file-local** — even if the outer module is imported elsewhere, the submodule and its contents are not visible to the importer.

`ext` exports the submodule as part of the parent module's public surface — importers see `outer.submod.foo`.

```stasha
mod ex_submods;

int mod greeter {
    ext fn greet(i8 *r name) {
        print.('Nice to meet you, {}!\n', name);
    }

    ext type Builder: struct {
        i8 *name;
        i32 age;
    }

    ext fn Builder.new(i8 *_name, i32 _age): Builder {
        ret Builder{.name = _name, .age = _age};
    }

    ext fn Builder.company(void): i8 * {
        ret 'Lumber Sons';
    }
}
```

A submodule is its own visibility domain:

- `ext` items inside the submodule are visible to the **enclosing** module but **only** if the submodule itself is `ext` and the enclosing module is imported.
- `int` items inside the submodule stay submodule-local.

This nests: submodules can contain their own submodules.

---

## Accessing Submodule Members

### Dot `.` — Works for Everything

```stasha
greeter.greet('Sully');                         // call submodule fn
greeter.Builder b = greeter.Builder.('Bob', 30);
print.('{}\n', greeter.Builder.company());      // call static method
print.('{}\n', b.name);                         // access instance field via .
```

The dot operator works uniformly for instance access, static access, and submodule navigation. It is the safe default — when in doubt, use `.`.

### Colon `:` — Static-Only

The colon `:` accesses **static** members of a type or submodule. It's a **scope resolution** operator; it cannot reach instance fields or methods.

```stasha
greeter:greet('Brian');                         // submodule static fn
greeter:Builder:company();                      // static method on a submodule type
```

Inside an interpolated format string, `:` is the only way to write nested static lookups concisely:

```stasha
print.(@'{greeter:Builder:company()}\n');       // → "Lumber Sons"
```

The dot operator would be ambiguous here because `{greeter.Builder.company()}` could be read as instance access on a `Builder` value named `greeter`.

### When to Use Which

| Access | Use `.` | Use `:` |
|--------|---------|---------|
| Instance field/method (`obj.field`) | Yes | No (compile error) |
| Static method (`Type.new(...)`) | Yes | Yes |
| Submodule member (`submod.fn(...)`) | Yes | Yes |
| Inside `@'...'` interpolation | Yes (where unambiguous) | Always works |
| Disambiguating from an instance | Yes (with care) | Yes (clearer intent) |

The safe rule:

> Use `.` everywhere by default. Use `:` only when you specifically want to signal "static / scope resolution" or when interpolation requires it.

The compiler rejects `:` on instance access:

```stasha
Point p = Point.(3, 4);
print.('{}\n', p:x);   // ERROR — `:` cannot read instance fields
print.('{}\n', p.x);   // OK
```

---

## Visibility Rules in Detail

### File-Local Submodule (`int mod`)

```stasha
mod outer;

int mod helpers {
    ext fn priv(void): void { ... }   // visible only inside outer
}
```

In another file:
```stasha
imp outer;
outer.helpers.priv();                  // ERROR — submodule is internal
```

### Exported Submodule (`ext mod`)

```stasha
mod outer;

ext mod ui {
    ext fn show(void): void { ... }
    int fn _internal(void): void { ... }
}
```

In another file:
```stasha
imp outer;
outer.ui.show();                       // OK
outer.ui._internal();                  // ERROR — internal to the submodule
```

### Nested Submodules

Submodules can nest arbitrarily deep:

```stasha
ext mod renderer {
    ext mod gl {
        ext fn init(void): void { ... }
    }

    ext mod vk {
        ext fn init(void): void { ... }
    }
}

renderer.gl.init();
renderer.vk.init();
```

---

## Why Submodules Instead of Separate Files

Submodules trade one tradeoff for another. Use a separate file when:

- The code in question is large.
- Other files want to import it independently of the rest of the parent module.
- The build benefits from finer-grained recompilation.

Use a submodule when:

- The contents are tightly coupled to the rest of the parent module.
- You want a private namespace without the overhead of `mod parent.helper;` and a new file.
- You want to expose a small grouped surface (e.g. `renderer.gl`, `renderer.vk`) without two files for a feature that fits in 30 lines each.

---

## Common Mistakes

**Confusing `:` and `.` on instance values:**
```stasha
Builder b = Builder.('Bob', 30);
b:name;       // ERROR — `:` is static-only
b.name;       // OK
```

**Trying to import an `int mod` from outside:**
```stasha
// in other.sts:
imp parent;
parent.private_sub.thing();   // ERROR — submodule is `int`
```

**Shadowing across submodules:** Two submodules at the same level may declare items with the same name; lookup is always qualified, so `a.foo` and `b.foo` are independent. Don't rely on bare `foo` resolving to one or the other.
