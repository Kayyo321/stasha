# Enums

Enums in Stasha come in two flavors: simple enums (like C enums, integer-backed) and tagged enums (also called variant enums, which carry data per variant). Both are matched with `match`.

---

## Simple Enums

```stasha
type Direction: enum {
    North,
    South,
    East,
    West,
}
```

Each variant is an integer discriminant starting at 0. Use the `Type.Variant` syntax to reference variants:

```stasha
Direction d = Direction.North;

if d == Direction.North {
    print.('heading north\n');
}
```

### Matching Simple Enums

```stasha
match d {
    Direction.North => { move_north(); }
    Direction.South => { move_south(); }
    Direction.East  => { move_east(); }
    Direction.West  => { move_west(); }
}
```

The match is exhaustive — if you miss a variant, the compiler warns (or errors).

### Wildcard Binding

A bare identifier catches the remaining cases and binds the discriminant integer:

```stasha
type Status: enum { Ok, Warn, Err }

match status {
    Status.Ok => { print.('ok\n'); }
    other     => { print.('non-ok code: {}\n', other); }
}
```

---

## Tagged Enums (Variants with Payloads)

Tagged enums let each variant carry its own data:

```stasha
type Shape: enum {
    Circle(i32),       // radius
    Rect(i32),         // width
    Triangle(i32),     // base
    Point,             // no payload
}
```

Create a tagged enum value:
```stasha
Shape s = Shape.Circle(30);
Shape r = Shape.Rect(100);
Shape p = Shape.Point;
```

### Matching with Payload Binding

```stasha
match s {
    Shape.Circle(r)   => { print.('circle, radius={}\n', r); }
    Shape.Rect(w)     => { print.('rect, width={}\n', w); }
    Shape.Triangle(b) => { print.('tri, base={}\n', b); }
    Shape.Point       => { print.('point\n'); }
}
```

The payload variable (`r`, `w`, `b`) is bound and available only within the arm's body.

---

## Guard Clauses

Add conditions after the pattern with `if`:

```stasha
match s {
    Shape.Circle(r) if r > 50 => { print.('large circle\n'); }
    Shape.Circle(r) if r > 10 => { print.('medium circle\n'); }
    Shape.Circle(r)            => { print.('small circle\n'); }
    _                          => { print.('not a circle\n'); }
}
```

If the guard is false, matching falls through to the next arm.

---

## Integer Literal Arms

Match against specific integer values:

```stasha
fn classify(i32 n): void {
    match n {
        0 => { print.('zero\n'); }
        1 => { print.('one\n'); }
        2 => { print.('two\n'); }
        _ => { print.('other\n'); }
    }
}
```

Negative integers work too:
```stasha
match n {
    -1 => { print.('minus one\n'); }
    0  => { print.('zero\n'); }
    1  => { print.('one\n'); }
    _  => { }
}
```

---

## Fat Arrow — Short Arms

Use `=>` for single-expression arms:

```stasha
fn area(Shape s): f64 {
    match s {
        Shape.Circle(r) => 3.14 * r * r;
        Shape.Rect(w)   => w * w;
        Shape.Point     => 0.0;
    }
}
```

*(This syntax may require the match result to be used — check compiler behavior.)*

---

## Enums in Structs

Enums can be embedded in structs:

```stasha
type Entity: struct {
    ext i32 id;
    ext Status status;
    ext Shape shape;
}

Entity e = .{
    .id     = 1,
    .status = Status.Ok,
    .shape  = Shape.Circle(25),
};

match e.status {
    Status.Ok  => { print.('entity {} is ok\n', e.id); }
    Status.Err => { print.('entity {} has error\n', e.id); }
    _          => { }
}
```

---

## C-Compatible Enums

When working with C libraries, use simple enums with `@c_layout` on the containing struct, or match the integer values directly:

```stasha
type ErrCode: enum {
    EOK   = 0,
    EFAIL = 1,
    ENOENT = 2,
}
```

*(Integer-valued enum variants — check if your version supports explicit values.)*

---

## Tagged Enum in `any`

The `any.[T1, T2, ...]` type is essentially an anonymous tagged enum. See [Types](Types) for details.

```stasha
fn process(any.[i32, f64, bool] val): void {
    match any.(val) {
        i32(i32 n)   => { print.('int: {}\n', n); }
        f64(f64 f)   => { print.('float: {}\n', f); }
        bool(bool b) => { print.('bool: {}\n', b); }
    }
}
```

---

## Practical Example: Result Type

The error-returning pattern in Stasha is a multi-return, but you can model a fuller result type with tagged enums:

```stasha
type ParseResult: enum {
    Ok(i32),
    Err(error),
}

fn parse_int(stack i8 *r s): ParseResult {
    if *.(s) == 0 { ret ParseResult.Err(error.('empty string')); }
    ret ParseResult.Ok(42);   // simplified
}

match parse_int(input) {
    ParseResult.Ok(val) => { print.('got {}\n', val); }
    ParseResult.Err(e)  => { print.('failed: {}\n', e); }
}
```

---

## `sizeof` on Enums

The size of a tagged enum equals the discriminant (usually `i32`) plus the largest payload, with alignment padding:

```stasha
sizeof.(Status)   // 4 (just the discriminant, no payload)
sizeof.(Shape)    // 8 (4 for discriminant + 4 for i32 payload)
```

Use `comptime_assert.()` to verify:
```stasha
comptime_assert.(sizeof.(Shape) == 8);
```
