# Async / Await

Stasha is in the middle of migrating `async` / `await` from thread-pool sugar to a real coroutine model.

Today, the runtime and lowering are still legacy for plain `async.(fn)(args)` task dispatch, but the language surface has started moving toward coroutine rules:

- `await(...)` is only legal inside `async fn`
- `stream.[T]` is the dedicated handle type for yielding async functions
- `yield expr;` produces a stream item from an `async fn`
- `yield;` is reserved for cooperative scheduler yield inside an `async fn`
- `await.next(stream)` is the stream-consume spelling in the new surface

The coroutine lowering/runtime work is not fully finished yet, so stream/yield syntax is currently validated by the compiler but not executable end-to-end.

Historically, Stasha layered an `async` / `await` surface on top of the thread-pool runtime. It was **not** a coroutine system — there was no green-thread scheduler, no stack switching, no event loop. `async fn` dispatched to the same POSIX thread pool that powers `thread.(fn)(args)`, and `await(f)` blocked the calling thread until the future resolved.

The point is ergonomics:

- A typed `future.[T]` carries the result type so call sites don't restate it.
- `async.(fn)(args)` reads naturally and pairs with `await(f)` and `await.(fn)(args)`.
- `await.all(...)` and `await.any(...)` express common fan-in patterns without manual loops.

Use `async`/`await` when you want concise typed futures. Use the lower-level [`thread.()` / `future.get`](Concurrency) API when you need raw thread-pool access (e.g. void dispatch with no return value).

---

## `async fn`

`async fn` marks a function as eligible for use with `async.()` / `await.()`:

```stasha
async fn square(i32 x): i32 {
    ret x * x;
}

async fn worker(void): void {
    do_some_work();
}
```

`async` is a function-level marker only. The body is still a normal function — there is no `await` inside an async function (you await **on** the future, not inside the function). It compiles to a regular function, plus an internal thread-pool wrapper.

You can still call an `async fn` synchronously like any other function:

```stasha
i32 r = square(7);   // synchronous call, no future
```

What `async` enables is the `async.(square)(7)` dispatch syntax.

---

## Dispatch: `async.(fn)(args)`

Returns a typed future:

```stasha
stack future.[i32] f = async.(square)(7);
```

The runtime:

1. Allocates a future handle (refcounted).
2. Submits a job to the thread pool.
3. Returns the handle immediately to the caller.

The job runs on the next free worker.

---

## Await: `await(f)` and `await.(fn)(args)`

### `await(f)` — Await an Existing Future

Block until the future resolves, return its value, and drop the future:

```stasha
stack future.[i32] f = async.(square)(7);
stack i32 v = await(f);    // blocks; loads i32; drops f
print.('{}\n', v);          // 49
```

`await(f)` is equivalent to `future.get.(T)(f); future.drop(f);` in one call.

For `future.[void]`, `await(f)` blocks and drops without a value:

```stasha
stack future.[void] vf = async.(worker)();
await(vf);                  // blocks until worker() returns
```

### `await.(fn)(args)` — Dispatch and Await

Dispatch and immediately await — the future never escapes:

```stasha
stack i32 sum = await.(add)(10, 32);     // 42
```

This is equivalent to `await(async.(add)(10, 32))`. It's still useful — it offloads the call to a worker thread and keeps the call site short — and it lets the runtime potentially fast-path single-shot dispatches.

---

## Typed Futures: `future.[T]`

`future.[T]` carries the element type. The compiler:

- Knows the dispatch wrapper's return type.
- Lets `await(f)` extract the value with no `.(T)` annotation.
- Type-checks `await.all(f1, f2, ...)` so all futures share `T`.

Compare:

```stasha
// Untyped (the legacy thread-pool API)
future f = thread.(square)(7);
i32 v = future.get.(i32)(f);
future.drop(f);

// Typed (the async API)
stack future.[i32] f = async.(square)(7);
i32 v = await(f);
```

### `future.[void]`

For functions that return `void`:

```stasha
async fn worker(void): void { ... }

stack future.[void] f = async.(worker)();
await(f);
```

You cannot extract a value from a void future — only `await(f)` (block + drop) and `future.ready(f)` (poll) are valid.

---

## Fan-In: `await.all` and `await.any`

### `await.all(f1, f2, ...)` — All Done

Block until **every** listed future resolves; return their values **in argument order**:

```stasha
stack future.[i32] fa = async.(square)(3);
stack future.[i32] fb = async.(square)(4);
stack future.[i32] fc = async.(square)(5);

stack let [a, b, c] = await.all(fa, fb, fc);
print.('{} {} {}\n', a, b, c);     // 9 16 25
```

All futures must share the same element type `T`; the result is destructured with `let [...]` like any other multi-return call.

`await.all` drops every future after collection — there's no need to call `future.drop(fa)` afterwards.

### `await.any(f1, f2, ...)` — First Done

Block until the **first** future resolves; return that value; drop the losers:

```stasha
stack future.[i32] ga = async.(square)(6);
stack future.[i32] gb = async.(square)(7);
stack i32 winner = await.any(ga, gb);
print.('winner = {}\n', winner);    // 36 or 49 depending on scheduling
```

Losing futures are dropped — but their wrapped functions **still run to completion** on the worker thread. There is no cancellation in v1. Only the result is discarded.

This makes `await.any` suitable for racing equivalent computations against alternative implementations (e.g. CPU vs SIMD path, multiple network endpoints) — but **not** for cancellation.

---

## Mixing `async`/`await` With `thread`/`future`

Both APIs share the same underlying thread pool and futures. A `future.[T]` and a raw `future` are interconvertible at the runtime level — although the typed handle can't be used with `future.get.(U)(f)` for a different `U`:

```stasha
test 'async + thread interop' {
    stack future.[i32] f1 = async.(square)(3);
    future             f2 = thread.(add)(1, 2);

    expect_eq.(await(f1), 9);
    expect_eq.(future.get.(i32)(f2), 3);
    future.drop(f2);
}
```

You can use whichever side reads better at the call site.

---

## Complete Example

```stasha
mod ex_async;

lib "stdio" = io;

int atomic i32 work_done = 0;

async fn worker(void): void {
    work_done = work_done + 1;
}

async fn square(i32 x): i32 { ret x * x; }
async fn add(i32 a, i32 b): i32 { ret a + b; }

fn main(void): i16 {
    // ── deferred form
    stack future.[i32] fs = async.(square)(7);
    stack i32 sq = await(fs);
    io.printf('7^2 = %d\n', sq);              // 49

    // ── one-shot form
    stack i32 sum = await.(add)(10, 32);
    io.printf('10+32 = %d\n', sum);           // 42

    // ── await.all
    stack future.[i32] fa = async.(square)(3);
    stack future.[i32] fb = async.(square)(4);
    stack future.[i32] fc = async.(square)(5);
    stack let [a, b, c] = await.all(fa, fb, fc);
    io.printf('%d %d %d\n', a, b, c);         // 9 16 25

    // ── await.any
    stack future.[i32] ga = async.(square)(6);
    stack future.[i32] gb = async.(square)(7);
    stack i32 winner = await.any(ga, gb);
    io.printf('winner = %d\n', winner);

    // ── void future
    stack future.[void] vf = async.(worker)();
    await(vf);
    io.printf('work_done = %d\n', work_done); // 1

    ret 0;
}
```

---

## Test Block Idioms

Async patterns map cleanly into `test` blocks:

```stasha
test 'async deferred + await' {
    stack future.[i32] f = async.(square)(5);
    expect_eq.(await(f), 25);
}

test 'await.(fn)(args) one-shot' {
    expect_eq.(await.(add)(6, 7), 13);
    expect_eq.(await.(square)(9), 81);
}

test 'await.all returns values in order' {
    stack future.[i32] fa = async.(square)(2);
    stack future.[i32] fb = async.(square)(3);
    stack let [a, b] = await.all(fa, fb);
    expect_eq.(a, 4);
    expect_eq.(b, 9);
}

test 'await.any returns one of the values' {
    stack future.[i32] fa = async.(square)(4);
    stack future.[i32] fb = async.(square)(5);
    stack i32 w = await.any(fa, fb);
    expect.(w == 16 || w == 25);
}
```

---

## Edge Cases & Restrictions

**Capturing across `async`** — `async fn` follows the same capture rules as plain `fn`. There are no closures: an async function may reference module-scope names but not enclosing locals. Pass everything you need through arguments.

**`await` only inside hosted builds** — `freestanding` modules cannot `await`; the thread runtime is not linked. Use a manual polling loop or skip async entirely.

**Dropping a typed future without awaiting** — Letting a `future.[T]` go out of scope without `await`/`future.drop` is a leak. The destructor pass does not auto-drop async futures — they're heap-allocated handles, not RAII values.

**Awaiting twice** — Awaiting the same future twice is a runtime error (the handle is freed by the first `await`). If you need to fan-out a result, copy the value after the first `await`.

**Cancellation** — There is none. A "lost" future from `await.any` runs to completion in the background; only the result is discarded.

**Recursion** — An `async fn` may dispatch other async work, but recursive `async.(self)(...)` followed by an immediate `await` on the same worker can deadlock if the pool is small enough that the parent and child end up on the same worker. Use a non-async helper or split work between batches.

**Ordering of `await.all`** — Results are returned in **argument order**, regardless of the order in which futures actually completed.

---

## Comparison: `async`/`await` vs `thread`/`future`

| | `async fn` + `await` | `thread.()` + `future` |
|---|---|---|
| Function declaration | `async fn name(...): T` | Plain `fn name(...): T` |
| Dispatch | `async.(name)(args)` | `thread.(name)(args)` |
| Future type | `future.[T]` (typed) | `future` (untyped) |
| Block + load | `await(f)` | `future.get.(T)(f); future.drop(f);` |
| Block, no value | `await(f)` (works for `future.[void]`) | `future.wait(f); future.drop(f);` |
| Poll | `future.ready(f)` | `future.ready(f)` |
| Fan-in (all) | `await.all(...)` | manual loop |
| Fan-in (race) | `await.any(...)` | manual loop |
| Drop | Implicit on `await`, `await.all`, `await.any` | Explicit `future.drop(f)` |

The two surfaces share the runtime. Pick whichever fits the call site.
