# Async / Await

Stasha has two coexisting flavors of `async fn`:

| flavor | declared as | call returns | semantics |
|---|---|---|---|
| **task** | `async fn f(...): T` | `future.[T]` | thread-pool dispatch — see [Concurrency](Concurrency) for runtime details |
| **stream coroutine** | `async fn g(...): stream.[T]` | `stream.[T]` | real LLVM coroutine; suspends on every `yield` |

Tasks are the original ergonomic wrapper around the POSIX thread pool — same model since v0. Stream coroutines were added in the coroutine migration: they are lowered through the `llvm.coro.*` intrinsics and the `coro-early` / `coro-split` / `coro-cleanup` pass pipeline, so a yielding `async fn` becomes a state machine with a heap-allocated frame and `coro.suspend` points at every `yield`.

This page covers both. Stream coroutines start at [Stream coroutines](#stream-coroutines).

## Quick Reference

- `async fn name(...): T` → call yields `future.[T]`; consume with `await(f)` / `await.all` / `await.any`.
- `async fn name(...): stream.[T]` → call yields `stream.[T]`; consume with `await.next(s)` / `stream.done(s)` / `stream.drop(s)`.
- `await(...)` is only legal inside `async fn` (or as a top-level driver of a future).
- `await.next(s)` is legal anywhere — it synchronously drives the producer until the next item or EOS.
- `yield expr;` produces a stream item; only legal inside an `async fn` returning `stream.[T]`.
- `yield;` is a cooperative reschedule (no item); same scope rule as `yield expr`.
- `ret;` ends a stream cleanly. `ret expr;` is rejected in stream coroutines (use `yield` for values).
- Library-backed task dispatch and the legacy thread runtime still live alongside the coroutine path — see [Concurrency](Concurrency).

Historically, Stasha layered an `async` / `await` surface on top of the thread-pool runtime. Tasks still ride on that runtime: there's no green-thread scheduler under `await(future)`, no stack switching, no event loop. `async fn` (returning `T`) dispatches to the same POSIX thread pool that powers `thread.(fn)(args)`, and `await(f)` blocks the calling thread until the future resolves.

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

---

## Stream coroutines

A yielding `async fn` is lowered as a real LLVM coroutine. Calling it does **not** dispatch to the thread pool. The function:

1. Allocates a coroutine frame on the heap (via `malloc(coro.size)`) at the top of its body.
2. Runs eagerly to the first `yield` (or to `ret;`, if there is none).
3. Returns the frame pointer as a `stream.[T]` handle.

Each `yield expr;` stores the produced item into the frame's promise slot, marks `item_ready=1`, and `coro.suspend`s back to the caller. The consumer drives the producer through `await.next(s)`, which synchronously calls `coro.resume(s)` until either `item_ready` is set (returns the item) or `eos` is set (the producer hit `ret;`). `yield;` suspends without producing — useful for cooperative scheduling once a real executor lands; in v1 the consumer simply re-resumes immediately.

### Producer side

```stasha
async fn count_up(i32 n): stream.[i32] {
    stack i32 i = 0;
    while (i < n) {
        yield i;        // produces an item, suspends
        i = i + 1;
    }
    ret;                // ends the stream — eos = 1
}
```

Inside a stream coroutine:

- `yield expr` — produce one item of type `T`. The expression must coerce to the declared `stream.[T]` element type.
- `yield;` — cooperative reschedule. No item is produced; on resume execution continues at the statement after the `yield;`.
- `ret;` — end the stream cleanly. Marks `eos = 1` and runs the coroutine to its final suspend.
- `ret expr;` — **rejected by the coroutine analysis pass** in v1. Use `yield` to produce values.
- `await(future)` — still legal (and still blocks on the underlying thread future). Inside a stream coroutine it does not become a coroutine-suspend point in v1.

The yielded item type is inferred from all `yield expr` sites and must match the declared `stream.[T]`. Mixed yield types (e.g. `yield 3; yield "hi";`) are rejected by the coroutine analysis pass.

Stream coroutines may not currently be defined inside a struct (`async fn` methods aren't supported in v1) and may not be generic (`@comptime[T]` parameters on stream coroutines are deferred to v2).

### Consumer side

A stream is consumed with three primitives:

```stasha
stream.[i32] s = count_up(5);

inf {
    stack i32 v = await.next(s);
    if (stream.done(s)) { break; }    // post-call eos check
    print.('{}\n', v);
}
stream.drop(s);
```

| call | meaning |
|---|---|
| `await.next(s)` | drive the producer until it yields the next item, or sets eos. Returns the item (zero-initialised on eos). |
| `stream.done(s)` | `i32` — non-zero once the producer has hit `ret;`. Check after each `await.next(s)`. |
| `stream.drop(s)` | destroy the stream coroutine. Frees its frame whether or not the producer reached eos. Always pair every constructed stream with a `stream.drop`. |

`await.next(s)` is legal anywhere — including in plain (non-async) functions and at module top-level. The drive loop is synchronous: the producer runs on the consumer's thread.

Awaiting a stream after `eos` returns a zero-initialised `T` and leaves `stream.done(s)` true. Calling `stream.drop(s)` on a finished or partially consumed stream is always safe.

### Eager start

Stream coroutines do **not** have an initial suspend. Calling the function runs the body until the first `yield` (or to `ret;`). This means:

- The first `await.next(s)` returns the first yielded item without an extra `coro.resume` round-trip.
- A producer that completes before yielding (e.g. `if (n == 0) { ret; }`) returns a stream whose `eos` is already set.
- Side effects in the producer body before the first `yield` execute when the function is **called**, not when the consumer first drives.

### Early drop and infinite producers

Streams can be dropped before the producer reaches `ret;`. The compiler emits `llvm.coro.destroy(s)` from `stream.drop(s)`, which unwinds through the coroutine's cleanup path, frees the frame, and skips the rest of the producer body. This means infinite generators are safe:

```stasha
async fn nats(void): stream.[i32] {
    stack i32 i = 0;
    inf { yield i; i = i + 1; }
    ret;
}

fn main(void): i32 {
    stream.[i32] s = nats();
    stack i32 i = 0;
    while (i < 5) {
        stack i32 v = await.next(s);
        if (stream.done(s)) { break; }
        print.('{}\n', v);
        i = i + 1;
    }
    stream.drop(s);   // never reaches eos — frame freed via coro.destroy
    ret 0;
}
```

### Frame layout (informational)

Every stream coroutine carries a **promise** — a fixed-layout header plus an inline `T` slot — pinned by the second argument of `llvm.coro.id`. The runtime never reaches into the promise directly; both the producer body and the consumer-side intrinsics access it through `llvm.coro.promise(handle, 8, false)`, which lowers to a constant offset from the handle.

```text
%__sts_coro_prom_hdr = type {
    i32 complete,    // 1 once the producer has run to ret;
    i32 eos,         // 1 once the producer has hit ret;
    i32 item_ready,  // 1 between a yield expr; and the next consumer drive
    i32 has_error,   // reserved for v2
    i32 is_stream,   // 1 — distinguishes stream from task promises
    i32 padding,
    ptr continuation,// reserved for v2 (resume parent on yield)
    ptr error_msg    // reserved for v2
}
%__sts_stream_prom_<T> = type { %__sts_coro_prom_hdr, T }
```

Codegen for `yield expr;` in the producer is:

```text
store T expr, ptr item_slot
store i32 1, ptr item_ready
%sp = call i8 @llvm.coro.suspend(token none, i1 false)
switch i8 %sp, label %coro.suspend [
    i8 0, label %after_yield
    i8 1, label %coro.cleanup
]
after_yield:
    store i32 0, ptr item_ready
    ; ... resume body
```

Codegen for `await.next(s)` in the consumer is:

```text
loop:
    %ir = load i32, ptr item_ready_of(s)
    br_ne ir, 0, %got_item, %check_eos
check_eos:
    %eos = load i32, ptr eos_of(s)
    br_ne eos, 0, %eos_block, %resume
resume:
    call void @llvm.coro.resume(ptr s)
    br loop
got_item:
    %v = load T, ptr item_of(s)
    store i32 0, ptr item_ready_of(s)
    ; ... %v is the await.next result
```

This keeps the consumer single-threaded and synchronous: the producer runs on the calling thread, suspends at `yield`, and is resumed at the next `await.next`. The eager-start path means the **first** `await.next` reads the item the producer left ready when it returned the handle, without an extra round-trip.

### Pass pipeline

When any `async fn` in a module uses `yield`, codegen runs:

```
coro-early , cgscc(coro-split) , coro-cleanup , globaldce
```

after IR generation, before object emission. The frontend marks every stream coroutine with the `presplitcoroutine` attribute so `coro-early` adopts it as a Switch-Resumed coroutine. `coro-split` lowers `coro.id`/`coro.begin`/`coro.suspend` and produces the resume+destroy clones. The `globaldce` tail removes the unused pre-split entry function once the resume path is wired up.

### Limitations (v1)

The coroutine migration is intentionally scoped — these are deliberate omissions, not bugs:

| missing | why |
|---|---|
| `await(stream_handle)` | streams use `await.next`. `await(s)` on a stream handle is rejected. |
| Stream coroutines as struct methods | `is_method && is_async` is unsupported in v1. |
| Generic stream coroutines (`@comptime[T] async fn ...`) | the generic instantiator runs before the coroutine analysis pass; v2 work. |
| Cancellation | `__async_cancel` only affects task futures. To stop a stream early, call `stream.drop(s)`. |
| True parallelism for streams | the coroutine drives synchronously on the consumer's thread. Use a task (`async.(fn)(args)`) when you want a different worker thread. |
| Cross-coroutine `await(stream)` continuations | the promise has a `continuation` slot but no executor wires it up yet. Streams are consumer-driven, not producer-pushed. |
| `ret expr;` from a stream | rejected by analysis. Use `yield` for the last item, then `ret;`. |

### Migration plan status

The coroutine migration tracker is at [`Coroutine-Migration-Plan`](Coroutine-Migration-Plan). The plan's stages 1–10 are complete: stream lowering, the consumer drive loop, `yield;`, the LLVM pass pipeline, and the analysis-pass legality checks all ship in this revision. Stages 11+ (true async tasks built on coroutines, executor queue, fan-in combinators on coroutine futures, cancellation propagation through await) are v2 work.

---

## Complete Stream Example

```stasha
mod stream_demo;

lib "stdio" = io;

async fn fib(i32 n): stream.[i64] {
    stack i64 a = 0;
    stack i64 b = 1;
    stack i32 i = 0;
    while (i < n) {
        yield a;
        stack i64 t = a + b;
        a = b;
        b = t;
        i = i + 1;
    }
    ret;
}

fn main(void): i32 {
    stream.[i64] f = fib(10);
    inf {
        stack i64 v = await.next(f);
        if (stream.done(f)) { break; }
        io.printf('%lld\n', v);
    }
    stream.drop(f);
    ret 0;
}
```

A larger walkthrough — counters, an interleaved `yield;` example, an infinite stream consumed with early drop, and accompanying `test 'name' { ... }` blocks — lives in [`examples/ex_coroutine_surface.sts`](../examples/ex_coroutine_surface.sts). Build it with `bin/stasha build examples/ex_coroutine_surface.sts -o /tmp/coro_demo` or run the tests with `bin/stasha test examples/ex_coroutine_surface.sts && ./a.test`.
