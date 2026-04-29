# Syntax Sugar: Lambdas, Pipelines, and Trailing Closures

Stasha includes three pieces of expression-level sugar that compose to make higher-order code less ceremonial:

- **`lam.(...)`** — bare lambda expression that lowers to a module-level function pointer.
- **`|>`** — left-associative pipeline operator.
- **Trailing closure** — short-form lambda after a `.()` call, automatically passed as the last argument.

All three are pure desugaring — there is no runtime lambda object, no closure capture, no GC. Each lambda becomes a regular function at the top level of the module, and call sites are rewritten to plain function-pointer calls.

---

## Lambdas

```stasha
stack fn*(stack i32): i32 sq = lam.(stack i32 x): i32 { ret x * x; };
stack i32 r = sq(5);   // 25
```

The expression `lam.(params): ret { body }` produces a function pointer of type `fn*(params): ret`. The compiler lifts the body to a hidden module-level function and the expression value is the address of that function.

### Anatomy

```
lam.(<params>): <return-type> { <body> }
       │             │             │
       │             │             └── identical to a normal fn body
       │             └── single return type (or `[T1, T2]` multi-return)
       └── parameter list with the same `stack`/`heap`/perm rules as `fn`
```

The parameter syntax is **identical** to a normal function. Storage qualifiers and pointer permissions all work:

```stasha
lam.(stack i8 *r name, stack i32 len): bool { ... }
lam.(heap i32 *rw buf, stack i32 n): void { ... }
```

### As a Function-Pointer Argument

```stasha
ext fn apply(stack fn*(stack i32): i32 f, stack i32 x): i32 {
    ret f(x);
}

stack i32 r = apply(lam.(stack i32 x): i32 { ret x + 100; }, 7);   // 107
```

### Capture Is Not Allowed (v1)

A lambda body may reference module-scope names — globals, functions, types, macros — but **not** enclosing locals:

```stasha
fn outer(void): void {
    stack i32 base = 10;

    stack fn*(stack i32): i32 bad =
        lam.(stack i32 x): i32 { ret x + base; };   // ERROR: base captured

    stack fn*(stack i32): i32 good =
        lam.(stack i32 x): i32 { ret x + 10; };     // OK
}
```

Error:
```
error: lambda body captures local variable 'base'; capturing closures land in v2
```

Capturing closures are planned for v2 with explicit `heap`/`stack` env storage. Until then, pass any context through the parameters or via globals.

---

## Pipeline Operator `|>`

The pipeline operator threads a value as the **first argument** of a function call:

```stasha
a |> f               →  f.(a)
a |> f.(b, c)        →  f.(a, b, c)
```

It is left-associative and very low precedence — sits below `||`/`&&`, above assignment.

```stasha
ext fn double(stack i32 x): i32 { ret x * 2; }
ext fn negate(stack i32 x): i32 { ret -x; }
ext fn add(stack i32 a, stack i32 b): i32 { ret a + b; }

stack i32 r1 = 21 |> double;                    // 42
stack i32 r2 = 5 |> double |> negate;           // -10  (negate(double(5)))
stack i32 r3 = 100 |> add(50);                  // 150  (add(100, 50))
```

### Composing With Lambdas

A lambda is just a function-pointer expression — pipeline accepts it directly:

```stasha
stack fn*(stack i32): i32 inc = lam.(stack i32 x): i32 { ret x + 1; };
stack i32 r = 10 |> inc |> double;              // 22
```

### What Goes On the Right

The right-hand side must be a **callable**:

| RHS shape | Lowering |
|-----------|----------|
| `fn_name` | `fn_name.(lhs)` |
| `fn_name.(args...)` | `fn_name.(lhs, args...)` |
| `obj.method` | `obj.method.(lhs)` |
| `obj.method.(args...)` | `obj.method.(lhs, args...)` |
| function-pointer variable | `(*var)(lhs)` |

Pipelining into anything else (a literal, a struct field that isn't a fn ptr) is a compile error:
```
error: right-hand side of |> must be a callable
```

### Precedence Rules

Pipeline binds **looser** than every binary operator and **tighter** than assignment. So:

```stasha
stack i32 r = a + 1 |> double;                  // (a + 1) |> double  →  double(a + 1)
stack bool b = x > 10 |> ok;                    // (x > 10) |> ok       →  ok(x > 10)
```

Use parentheses if you want different grouping.

---

## Trailing Closures

A short-form lambda may appear in braces **after** a `.()` call. The closure is passed as the **last argument** of that call:

```stasha
ext fn apply(stack i32 x, stack fn*(stack i32): i32 f): i32 { ret f(x); }

stack i32 r = apply(7) { |x| x * 3 };
//             ↑       ↑
//             apply takes (i32, fn*(...))
//             {|x| x*3} becomes the second argument
```

Equivalent to:

```stasha
stack i32 r = apply(7, lam.(stack i32 x): i32 { ret x * 3; });
```

### Three Forms

#### Typed-param form: `{ |p1, p2| body-expr }`

The pipe-delimited identifier list names the parameters. Their **types are inferred** from the matching fn-pointer parameter slot in the callee:

```stasha
ext fn apply2(stack i32 a, stack i32 b,
              stack fn*(stack i32, stack i32): i32 f): i32 {
    ret f(a, b);
}

stack i32 r = apply2(10, 20) { |a, b| a + b };   // 30
```

#### Block form: `{ stmt1; stmt2; ... }`

When the closure has zero parameters, drop the pipes. The block runs as a statement list:

```stasha
ext fn run(stack fn*(void): void f): void { f(); }

run() { print.('hi from trailing closure\n'); };
```

#### Single-expression body

A typed-param closure with one expression body needs no `ret` and no semicolon:

```stasha
filter(&nums[0], 5, &out[0], &out_len) { |n| n % 2 == 0 };
```

The compiler inserts the implicit `ret` when the closure type expects a non-`void` return.

### Why Last-Argument

Trailing closures **only** bind to the final parameter of the call. Write higher-order functions with the fn-pointer param last:

```stasha
fn map(stack []T src, stack []T dst, stack fn*(T): T f): void { ... }
fn reduce(stack []T arr, stack T init, stack fn*(T, T): T f): T { ... }
fn filter(stack T *r src, stack i32 n, stack T *rw dst,
          stack i32 *rw out_len, stack fn*(T): bool pred): void { ... }
```

That layout makes call sites clean:

```stasha
sum    = reduce(arr, 0)            { |acc, n| acc + n };
filter(&nums[0], 5, &out[0], &n)   { |x| x > 0 };
```

### Parsing Hazard: `if` / `while` / `for`

The trailing-closure parser is **suppressed** inside the condition expression of `if`, `while`, `for`, `do-while`, `match`, and `switch`. Otherwise this would be ambiguous:

```stasha
if pred(x) { do_thing(); }
//        ↑ this is the if-body, NOT a trailing closure for pred
```

If you genuinely want to call `pred` with a trailing closure inside an `if` condition, parenthesise the call:

```stasha
if (pred(x) { |y| y > 0 }) { do_thing(); }
```

---

## Putting It Together

```stasha
mod ex_sugar_combo;

int fn double(stack i32 x): i32 { ret x * 2; }

int fn map(stack i32 *r src, stack i32 n,
           stack i32 *rw dst,
           stack fn*(stack i32): i32 f): void {
    for (stack i32 i = 0; i < n; i++) { dst[i] = f(src[i]); }
}

ext fn main(void): i32 {
    stack i32 in[5]  = .{1, 2, 3, 4, 5};
    stack i32 out[5] = .{};

    // pipeline + lambda
    stack i32 r = 10 |> lam.(stack i32 x): i32 { ret x + 1; }
                     |> double;
    print.('{}\n', r);                       // 22

    // map with a trailing closure
    map(&in[0], 5, &out[0]) { |n| n * n };

    for (stack i32 i = 0; i < 5; i++) {
        print.('{} ', out[i]);               // 1 4 9 16 25
    }
    print.('\n');
    ret 0;
}
```

---

## Lowering Summary

| Source | Lowered to |
|--------|------------|
| `lam.(p): R { body }` | Top-level `fn __lam_NN(p): R { body }`; expression value is `&__lam_NN` |
| `a \|> f` | `f.(a)` |
| `a \|> f.(b, c)` | `f.(a, b, c)` |
| `f.(args) { \|p\| body }` | `f.(args, lam.(p): R { ret body; })` |
| `f.(args) { stmts }` | `f.(args, lam.(void): void { stmts })` |

All three desugar at parse time. The codegen sees plain `fn_decl` and `fn_call` nodes — no special runtime, no extra allocations.

---

## Common Mistakes

**Capturing a local in a lambda:**
```stasha
fn outer(): void {
    stack i32 n = 5;
    stack fn*(stack i32): i32 f =
        lam.(stack i32 x): i32 { ret x + n; };   // ERROR: captures n
}
```
Fix: pass `n` as a parameter, or move it to module scope as a `final` global.

**Trailing closure on a non-callable last param:**
```stasha
fn add(stack i32 a, stack i32 b): i32 { ret a + b; }
i32 r = add(1) { |x| x };                       // ERROR: last param is i32, not fn*
```

**Pipelining into something that isn't a call:**
```stasha
stack i32 r = 5 |> 10;                          // ERROR: 10 is not callable
```

**Forgetting to give the lambda a return type:**
```stasha
stack fn*(stack i32): i32 f =
    lam.(stack i32 x) { ret x; };               // ERROR: return type required
```
