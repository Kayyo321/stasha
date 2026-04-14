# Error Handling

Stasha uses explicit, lightweight errors. There are no exceptions and no unwinding. Errors are values.

---

## The `error` Type

The `error` type is either `nil` (no error) or a string message. It maps cleanly to "optional string" semantics:

```stasha
error e = nil;           // no error
error e = error.('something went wrong');
error e = error.('value {} is out of range', value);
```

Check for errors with `== nil` / `!= nil`:

```stasha
if e == nil {
    print.('success\n');
} else {
    print.('error: {}\n', e);
}
```

---

## Returning Errors

Functions that can fail return an `error` as their last return value:

```stasha
fn open_file(stack i8 *r path): [i32, error] {
    if path == nil { ret -1, error.('null path'); }
    // ... try to open ...
    ret fd, nil;   // nil = success
}
```

Single error return (no other value):
```stasha
fn validate(stack i32 n): error {
    if n < 0 { ret error.('negative value: {}', n); }
    if n > 100 { ret error.('value {} exceeds max', n); }
    ret nil;
}
```

---

## Handling Errors: `let` + `if`

The most explicit way:

```stasha
let [fd, err] = open_file("data.txt");
if err != nil {
    print.('failed to open: {}\n', err);
    ret 1;
}
// fd is valid here
```

---

## Handling Errors: `with`

The `with` statement is purpose-built for this pattern:

```stasha
with let [fd, err] = open_file("data.txt"); err == nil {
    // only entered if err == nil
    let [data, err2] = read_all(fd);
    // use data...
} else {
    print.('error: {}\n', err);
}
```

`with` scopes the variables — you can reuse names without shadowing issues:

```stasha
with let [a, err] = first_op(); err == nil {
    with let [b, err] = second_op(a); err == nil {   // new 'err'
        print.('{} {}\n', a, b);
    }
}
```

---

## Error Propagation: `.?` Operator

Use `fn.?(args)` to call a function and propagate its error upward if it fails. This is like Rust's `?` operator:

```stasha
fn load_config(stack i8 *r path): [Config, error] {
    let [fd, _]   = open_file.?(path);       // propagate error
    let [text, _] = read_all.?(fd);          // propagate error
    let [cfg, _]  = parse_config.?(text);    // propagate error
    ret cfg, nil;
}
```

If `open_file()` returns a non-nil error, `load_config` immediately returns `(zero_value, that_error)`.

The `_` placeholder discards the first return value when you only care about propagating the error.

Postfix `?` on any expression:
```stasha
fn pipeline(): error {
    validate_input(data)?;    // propagate error
    process(data)?;           // propagate error
    ret nil;
}
```

---

## `error.()` — Creating Errors

```stasha
ret error.('simple message');
ret error.('value {} is invalid', x);
ret error.('expected range [{}, {}], got {}', lo, hi, val);
```

Error messages support the same format specifiers as `print.()`.

---

## The `with` Statement in Depth

`with` can also be used without error handling, just for scoped bindings:

```stasha
// Single value:
with let result = compute(); true {
    use(result);
}

// The condition after ';' is the "enter block" guard:
with let x = maybe_nil_fn(); x != nil {
    use(x);
}
```

The general form:
```stasha
with let binding = expr; condition {
    // entered if condition is true
} else {
    // entered if condition is false
}
```

---

## Error Helpers — from the Standard Library

The stdlib provides `error_helpers`:

```stasha
libimp "error_helpers" from std;

// Wrap a C errno into a Stasha error:
error e = error_from_errno(errno);

// Check and propagate:
check_or_ret(some_cond, error.('check failed'));
```

---

## Practical Patterns

### Try/Otherwise

```stasha
fn get_user(i32 id): [User, error] {
    let [user, err] = db_lookup(id);
    if err != nil { ret .{}, err; }
    ret user, nil;
}
```

### Pipeline with Early Exit

```stasha
fn process_file(stack i8 *r path): error {
    let [fd, _]     = open.?(path);
    let [data, _]   = read.?(fd);
    let [parsed, _] = parse.?(data);
    let [_]         = write_output.?(parsed);
    close(fd);
    ret nil;
}
```

### Error Accumulation

```stasha
heap []error errors = nil;
defer rem.(errors);

let [_, e1] = op1();
if e1 != nil { errors = append.(errors, e1); }

let [_, e2] = op2();
if e2 != nil { errors = append.(errors, e2); }

if len.(errors) > 0 {
    foreach e in errors {
        print.error.('{}\n', e);
    }
    ret 1;
}
```

---

## Common Mistakes

**Ignoring errors:**
```stasha
open_file("data.txt");   // ERROR VALUE IGNORED
```

Better:
```stasha
let [_, err] = open_file("data.txt");
if err != nil { handle(err); }
```

**Using `.?` in a void function:**
```stasha
fn helper(): void {
    open_file.?("data.txt");   // can't propagate — function returns void
}
```

If you need propagation, change the return type to `error`:
```stasha
fn helper(): error {
    let [_, _] = open_file.?("data.txt");
    ret nil;
}
```

**Forgetting `nil` on success:**
```stasha
fn get(): [i32, error] {
    ret 42;   // ERROR: missing second return value
    ret 42, nil;  // correct
}
```
