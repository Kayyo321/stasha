# Testing

Stasha has a built-in test system. Tests live alongside your code in `test 'name' { ... }` blocks and are run with `stasha test`.

---

## Writing Tests

```stasha
test 'addition works' {
    expect_eq.(2 + 2, 4);
}

test 'pointer is not nil' {
    heap i32 *rw p = new.(sizeof.(i32));
    defer rem.(p);
    expect.(p != nil);
}
```

Test blocks can appear anywhere in your source file, at module level.

---

## Test Assertions

### `expect.(condition)`

Asserts that the condition is true:

```stasha
expect.(x > 0);
expect.(name != nil);
expect.(buf[0] == 42);
```

### `expect_eq.(actual, expected)`

Asserts that two values are equal:

```stasha
expect_eq.(result, 42);
expect_eq.(len.(s), 5);
expect_eq.(p.x, 10);
```

### `expect_neq.(a, b)`

Asserts that two values are not equal:

```stasha
expect_neq.(err, nil);
expect_neq.(a, b);
```

### `test_fail.('message')`

Unconditionally fails the test with a message:

```stasha
test 'branch that should not be reached' {
    if impossible_condition {
        test_fail.('should never reach here');
    }
}
```

---

## Running Tests

```bash
stasha test myfile.sts
```

Output:
```
[PASS] addition works
[PASS] pointer is not nil
[FAIL] string equality
  error: expected "hello" got "world"
```

Tests that pass print `[PASS]`, failures print `[FAIL]` with the failing assertion.

---

## Full Example: Testing a Stack

```stasha
mod stack_test;

type Stack: @comptime[T] struct {
    int heap T *rw data;
    int i32 top;
    int i32 cap;

    ext fn push(T val): void {
        if this.top >= this.cap {
            this.cap = this.cap * 2;
            this.data = mov.(this.data, sizeof.(T) * this.cap);
        }
        this.data[this.top] = val;
        this.top = this.top + 1;
    }

    ext fn pop(void): T {
        this.top = this.top - 1;
        ret this.data[this.top];
    }

    ext fn empty(void): bool => this.top == 0;
    ext fn size(void): i32 => this.top;

    ext fn rem(void): void { rem.(this.data); }
}

fn @comptime[T] Stack.new(i32 cap): Stack.[T] {
    heap T *rw d = new.(sizeof.(T) * cap);
    ret Stack.[T] { .data = d, .top = 0, .cap = cap };
}

test 'stack is initially empty' {
    Stack.[i32] s = Stack.[i32].new(4);
    expect.(s.empty());
    expect_eq.(s.size(), 0);
}

test 'push and pop single element' {
    Stack.[i32] s = Stack.[i32].new(4);
    s.push(42);
    expect.(!s.empty());
    expect_eq.(s.size(), 1);
    expect_eq.(s.pop(), 42);
    expect.(s.empty());
}

test 'LIFO ordering' {
    Stack.[i32] s = Stack.[i32].new(8);
    s.push(1);
    s.push(2);
    s.push(3);
    expect_eq.(s.pop(), 3);
    expect_eq.(s.pop(), 2);
    expect_eq.(s.pop(), 1);
}

test 'growth beyond initial capacity' {
    Stack.[i32] s = Stack.[i32].new(2);
    s.push(10);
    s.push(20);
    s.push(30);   // triggers realloc
    s.push(40);
    expect_eq.(s.size(), 4);
    expect_eq.(s.pop(), 40);
    expect_eq.(s.pop(), 30);
}
```

Run:
```bash
stasha test stack_test.sts
```

---

## Tests in the Standard Library

The stdlib ships with its own tests. Run them with:

```bash
make stdlib-test
```

This compiles and runs the test suite for every stdlib module.

---

## Testing Thread Code

Thread tests work just like regular tests:

```stasha
test 'future returns correct value' {
    future f = thread.(square)(5);
    expect_eq.(future.get.(i32)(f), 25);
    future.drop(f);
}

test 'concurrent futures are independent' {
    future fa = thread.(square)(4);
    future fb = thread.(add)(6, 7);
    expect_eq.(future.get.(i32)(fa), 16);
    expect_eq.(future.get.(i32)(fb), 13);
    future.drop(fa);
    future.drop(fb);
}
```

---

## Test Isolation

Each test block runs independently. Variables declared inside a test are scoped to that test. There's no shared state between tests (unless you use module-level globals, which you should avoid in tests).

---

## Best Practices

- Name tests descriptively: `test 'add returns sum of two ints'`
- Test edge cases: empty input, zero, max values, nil pointers
- Use `defer rem.()` inside tests to avoid leaks
- Keep tests close to the code they test (same file or companion test file)
- Test one thing per block — smaller tests are easier to debug
