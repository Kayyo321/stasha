# Control Flow

Stasha has a full set of control flow constructs — familiar to C programmers but with powerful additions like pattern matching and comparison chains.

---

## If / Else

```stasha
if condition { ... }
if condition { ... } else { ... }
if condition { ... } else if other { ... } else { ... }
```

```stasha
stack i32 x = 42;

if x > 100 {
    print.('big\n');
} else if x > 10 {
    print.('medium\n');
} else {
    print.('small\n');
}
```

No parentheses required around the condition, but they're allowed.

---

## For Loop

C-style for loop:

```stasha
for (init; condition; update) { body }
```

```stasha
for (stack i32 i = 0; i < 10; i++) {
    print.('{} ', i);
}
```

You can omit the `stack` qualifier on the loop variable — the compiler defaults it:

```stasha
for (i32 i = 0; i < 10; i++) { ... }
```

---

## While Loop

```stasha
while (condition) { body }
```

```stasha
stack i32 n = 1;
while (n < 100) {
    n = n * 2;
}
```

---

## Do-While Loop

```stasha
do { body } while (condition);
```

```stasha
stack i32 attempts = 0;
do {
    attempts++;
} while (attempts < 3 && !try_connect());
```

---

## Infinite Loop

```stasha
inf { body }
```

The `inf` loop runs until a `break` (or return/panic):

```stasha
inf {
    stack i8 c = read_char();
    if c == '\n' { break; }
    process(c);
}
```

---

## `foreach` — Slice Iteration

Iterate over every element in a slice without managing an index:

```stasha
foreach element in slice { body }
```

```stasha
heap []i32 nums = make.([]i32, 5);
defer rem.(nums);
nums[0] = 1; nums[1] = 2; nums[2] = 3; nums[3] = 4; nums[4] = 5;

foreach n in nums {
    print.('{} ', n);   // 1 2 3 4 5
}
```

Works on stack slices too:
```stasha
stack i32 arr[6] = .{10, 20, 30, 40, 50, 60};
stack []i32 view = arr[:];

foreach v in view {
    print.('{} ', v);
}
```

`break` and `continue` work inside `foreach`:
```stasha
foreach n in nums {
    if n < 0 { break; }
    if n % 2 != 0 { continue; }
    print.('{} ', n);   // evens only
}
```

---

## `break` and `continue`

```stasha
break;      // exit innermost for/while/do-while/inf/switch
continue;   // next iteration of innermost loop
```

`continue` is not valid inside `switch`.

---

## Switch Statement

C-style switch with fall-through prevented by default (each case needs `break`):

```stasha
switch (expression) {
    case 0: { ... break; }
    case 1: { ... break; }
    case 2, 3: { ... break; }   // multiple values per case
    default: { ... }
}
```

```stasha
switch (error_code) {
    case 0: { print.('OK\n');      break; }
    case 1: { print.('Error\n');   break; }
    case 2: { print.('Timeout\n'); break; }
    default: { print.('Unknown\n'); }
}
```

---

## Match — Pattern Matching

`match` is Stasha's pattern matching statement. It's exhaustive and type-checked.

### Matching Integers

```stasha
match n {
    0 => { print.('zero\n'); }
    1 => { print.('one\n'); }
    2 => { print.('two\n'); }
    _ => { print.('other\n'); }   // wildcard
}
```

Negative literals work too:
```stasha
match n {
    -1 => { print.('minus one\n'); }
    0  => { print.('zero\n'); }
    1  => { print.('one\n'); }
    _  => { print.('other: {}\n', n); }
}
```

### Matching Enum Variants

```stasha
type Status: enum { Ok, Warn, Err }

match status {
    Status.Ok   => { print.('all good\n'); }
    Status.Warn => { print.('warning\n'); }
    Status.Err  => { print.('error!\n'); }
}
```

### Matching Tagged Enums (with Payloads)

```stasha
type Shape: enum {
    Circle(i32),     // radius
    Rect(i32),       // width
    Point,
}

match shape {
    Shape.Circle(r) => { print.('circle, r={}\n', r); }
    Shape.Rect(w)   => { print.('rect, w={}\n',   w); }
    Shape.Point     => { print.('point\n'); }
}
```

### Guard Clauses

Add a condition after the pattern with `if`:

```stasha
match shape {
    Shape.Circle(r) if r > 50 => { print.('large circle\n'); }
    Shape.Circle(r) if r > 10 => { print.('medium circle\n'); }
    Shape.Circle(r)            => { print.('small circle\n'); }
    _                          => { print.('not a circle\n'); }
}
```

If the guard fails, matching continues to the next arm.

### Wildcard Binding

A bare identifier (not `Type.Variant`) binds the discriminant value:

```stasha
match status {
    Status.Ok => { print.('ok\n'); }
    other     => { print.('non-ok code: {}\n', other); }
}
```

### Matching `any` Types

```stasha
fn describe(any.[i32, f64, bool] val): void {
    match any.(val) {
        i32(i32 n)  => { print.('int: {}\n', n); }
        f64(f64 f)  => { print.('float: {}\n', f); }
        bool(bool b) => { print.('bool: {}\n', b); }
    }
}
```

---

## Comparison Chains

Stasha has a unique comparison chain syntax using the `and` / `or` keywords. This is syntactic sugar that evaluates the left-hand side **exactly once** and chains comparisons.

### Range Check (`and`)

```stasha
// x > 10 and < 20  →  x > 10 && x < 20  (x evaluated once)
if x > 10 and < 20 {
    print.('{} is between 10 and 20\n', x);
}
```

Inclusive range:
```stasha
if pct >= 0 and <= 100 {
    print.('valid percentage\n');
}
```

Out-of-range:
```stasha
if flag < 0 or > 100 {
    print.('out of bounds\n');
}
```

### Equality Chain (`or`)

```stasha
// cmd == 1 or 2 or 3  →  cmd == 1 || cmd == 2 || cmd == 3
if cmd == 1 or 2 or 3 {
    handle_command(cmd);
}

// HTTP error codes:
if status == 400 or 401 or 403 or 404 or 500 {
    print.('HTTP error: {}\n', status);
}
```

### Not-Equal Chain (`and`)

```stasha
// mode != 1 and != 2 and != 3
if mode != 1 and != 2 and != 3 {
    print.('mode is none of 1, 2, 3\n');
}
```

### Mixed Operators

```stasha
if val != 5 and > 0 {
    print.('nonzero and not five\n');
}
```

### Function Calls

The left-hand expression is evaluated **exactly once**, even if it's a function call:

```stasha
if get_score() > 60 and < 90 {
    print.('passing grade\n');   // get_score() called once
}
```

### Precedence

`and` binds tighter than `or`:
```stasha
if x == 1 or 15 and != 99 {
    // expands to: x == 1 || (x == 15 && x != 99)
}
```

---

## `defer` — Guaranteed Cleanup

`defer` schedules a statement or block to run at the end of the current scope, regardless of how it exits:

```stasha
heap u8 *rw buf = new.(256);
defer rem.(buf);   // guaranteed to run when this scope exits

// ... code that might return early, panic, etc. ...
// buf is always freed
```

Multiple defers run in LIFO order (last deferred runs first):
```stasha
heap u8 *rw a = new.(100); defer rem.(a);
heap u8 *rw b = new.(200); defer rem.(b);
// At scope exit: rem.(b) runs first, then rem.(a)
```

Defer a block:
```stasha
defer {
    cleanup_step1();
    cleanup_step2();
}
```

---

## `with` — Scoped Binding with Guard

`with` is designed for the common pattern of calling a multi-return function and branching on the error:

```stasha
with let [result, err] = some_fn(); err == nil {
    // only enters here if err == nil
    use(result);
} else {
    handle(err);
}
```

The variables are scoped to the `with` block, so you can reuse names:

```stasha
with let [x, err] = first_fn(); err == nil {
    with let [y, err] = second_fn(); err == nil {   // 'err' is a new variable here
        print.('{} {}\n', x, y);
    }
}
```

Single-variable form:
```stasha
with let value = compute(); true {
    use(value);
}
```

---

## Compile-Time Conditionals

```stasha
comptime_if platform == "macos" {
    // macOS-specific code
} else comptime_if platform == "linux" {
    // Linux-specific code
} else {
    // other platforms
}

comptime_if arch == "aarch64" {
    // ARM64 code
}
```

Available constants: `platform` (`"macos"`, `"linux"`, ...), `arch` (`"aarch64"`, `"x86_64"`, ...).

---

## `comptime_assert`

Check invariants at compile time:

```stasha
comptime_assert.(sizeof.(Header) == 16);
comptime_assert.(sizeof.(i32) == 4);
```

If the assertion fails, the compile fails with an error.
