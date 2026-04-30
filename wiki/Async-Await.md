# Async / Await

Stasha has two flavors of `async fn`, both lowered via **LLVM coroutines** (`llvm.coro.*`):

| flavor | declared as | call returns | semantics |
|---|---|---|---|
| **task** | `async fn f(...): T` | `future.[T]` (coro frame ptr) | cooperative task — caller drives it with `await`, `future.*`, or fan-in combinators |
| **stream** | `async fn g(...): stream.[T]` | `stream.[T]` (coro frame ptr) | suspends on every `yield`; consumer drives via `await.next(s)` |

Both flavors compile to LLVM split-resume/destroy coroutine objects. `async fn` does **not** dispatch to the POSIX thread pool; it uses coroutine frames plus a small thread-local executor for cooperative `yield;` rescheduling. Use `thread.(fn)(args)` when you want real parallel execution.

---

## Quick Reference

```stasha
// task coroutine
async fn sum(i32 n): i32 { ... ret result; }
stack i32 v = await(sum(10));              // drives to completion, returns i32

// stream coroutine
async fn count(i32 n): stream.[i32] { ... yield i; ... ret; }
stream.[i32] s = count(5);
inf {
    stack i32 v = await.next(s);
    if (stream.done(s)) { break; }
}
stream.drop(s);

// cancellation
stream.cancel(s);   // sets cancelled flag; producer sees it at next yield

// fan-in
stack i32 [a, b] = await.all(sum(3), sum(4));   // round-robin both, return in order
stack i32 w      = await.any(slow(), fast());   // first completed task wins

// async methods
type Foo: struct {
    ext i32 val;
    ext async fn compute(i32 x): i32 { ret this.val + x; }
    ext async fn stream_vals(i32 n): stream.[i32] { ... }
}

// generic async fn
async fn @comptime[T] wrap(stack T x): T { ret x; }
async fn @comptime[T] gen_stream(stack T x, stack i32 n): stream.[T] { ... }
stack i32 r = await(wrap.[i32](42));
```

Key rules:
- `await(f)` is legal **anywhere** (not just inside `async fn`). It drives the task coroutine on the calling thread.
- `future.ready`, `future.wait`, `future.get.(T)`, and `future.drop` also work on typed `future.[T]` coroutine task handles.
- `await.next(s)` is legal anywhere — drives one step and returns the next item.
- `stream.done(s)` — non-zero once the producer hits `ret;`. Check after every `await.next(s)`.
- `stream.drop(s)` — destroy frame; always call for every constructed stream.
- `yield expr;` — produce an item and suspend. Legal only in `async fn` returning `stream.[T]`.
- `yield;` — cooperative suspend, no item. The coroutine is queued on the thread-local executor and can be resumed by waits/awaits that pump pending work.
- `ret;` — end a stream. `ret expr;` is rejected in stream coroutines.

---

## Task coroutines

A task `async fn` returning `T` compiles to an LLVM coroutine that runs to completion when driven. Calling the function allocates a coroutine frame, runs the body to the initial suspend point (after setting up the promise), and returns the frame pointer as a `future.[T]`.

```stasha
async fn sum_to(i32 n): i32 {
    stack i32 acc = 0;
    stack i32 i = 0;
    while (i <= n) {
        acc = acc + i;
        i = i + 1;
    }
    ret acc;
}

stack i32 v = await(sum_to(100));   // 5050
```

`await(f)` drives the coroutine with `llvm.coro.resume` until `complete == 1`, loads the result from the promise slot, destroys the frame, and returns `T`. While waiting, it also lets the thread-local coroutine executor run any pending `yield;` continuations.

A task may `await` another task inside its body — the inner await drives the child synchronously on the same thread:

```stasha
async fn fib(i32 n): i32 {
    if (n <= 1) { ret n; }
    stack i32 a = await(fib(n - 1));
    stack i32 b = await(fib(n - 2));
    ret a + b;
}
```

### Starting Tasks: Direct Calls and `async.(fn)(args)`

Calling an `async fn` directly creates and returns its `future.[T]` coroutine handle:

```stasha
stack future.[i32] f = sum_to(10);
// ... other work ...
stack i32 result = await(f);
```

`async.(fn)(args)` is kept as intent-revealing sugar for the same operation. It is a synonym for a direct call to an `async fn`; it does **not** move the task to the thread pool:

```stasha
stack future.[i32] f = async.(sum_to)(10);
stack i32 result = future.get.(i32)(f);
future.drop(f);
```

### `await.(fn)(args)`

Drive immediately with no intermediate handle:

```stasha
stack i32 r = await.(sum_to)(10);
```

### Fan-in: `await.all` and `await.any`

`await.all` drives all live tasks cooperatively in round-robin order and returns their results in argument order:

```stasha
stack i32 [a, b, c] = await.all(sum_to(3), sum_to(4), sum_to(5));
// a=6, b=10, c=15
```

`await.any` drives all live tasks cooperatively in round-robin order until one completes. The first task to report `complete` wins; the remaining live tasks are cancelled and dropped. It returns only the winner's result:

```stasha
stack i32 winner = await.any(slow_fn(99), fast_fn(1));
```

This is still single-threaded cooperative scheduling. CPU-bound tasks only yield to their peers at explicit coroutine suspension points such as `yield;`, nested awaits, or completion. Parallel race semantics require explicit thread dispatch (`thread.(fn)(args)`).

### Future Operations on Coroutine Tasks

Typed coroutine task handles use `future.[T]` and support the same surface spelling as thread-pool futures:

```stasha
stack future.[i32] f = async.(square)(9);
if (!future.ready(f)) {
    // do local work
}
stack i32 n = future.get.(i32)(f);
future.drop(f);

stack future.[void] vf = async.(worker)();
future.wait(vf);
future.drop(vf);
```

For a typed coroutine future, `future.wait` and `future.get.(T)` drive the coroutine to completion on the calling thread. `future.drop` waits if needed, then destroys the coroutine frame. Untyped `future` values from `thread.(fn)` still use the thread-pool runtime.

---

## Stream coroutines

An `async fn` returning `stream.[T]` is a real LLVM coroutine with one suspend point per `yield`. The frame persists between calls; the consumer drives it step by step.

```stasha
async fn range(i32 lo, i32 hi): stream.[i32] {
    stack i32 i = lo;
    while (i < hi) {
        yield i;
        i = i + 1;
    }
    ret;
}
```

### Consumer pattern

```stasha
stream.[i32] s = range(0, 10);
inf {
    stack i32 v = await.next(s);
    if (stream.done(s)) { break; }
    print.('{}\n', v);
}
stream.drop(s);
```

`await.next(s)` synchronously drives the producer until it yields the next item or reaches `ret;`. `stream.done(s)` returns non-zero once eos is set; the value returned on eos is zero-initialised. Always call `stream.drop(s)` even if the stream wasn't fully consumed.

### Eager start

Stream coroutines have **no initial suspend**. The body runs to the first `yield` when the function is called. The first `await.next(s)` reads the item already waiting in the frame without an extra resume round-trip.

### Cancellation

`stream.cancel(s)` sets a `cancelled` flag in the coroutine frame. The producer checks the flag at each resume edge (immediately after every `yield`). When cancellation is detected, the coroutine executes its cleanup path and sets eos.

```stasha
stream.[i32] s = nats();
// consume a few items...
stream.cancel(s);
// drain: run until eos so the cleanup path executes
inf {
    stack i32 _v = await.next(s);
    if (stream.done(s)) { break; }
}
stream.drop(s);
```

A stream cancelled before being driven at all will still finalize cleanly — the first resume triggers the cancellation check.

### Infinite producers

Infinite streams are safe. `stream.drop(s)` calls `llvm.coro.destroy`, which unwinds through the cleanup path and frees the frame without resuming the body:

```stasha
async fn nats(void): stream.[i32] {
    stack i32 i = 0;
    inf { yield i; i = i + 1; }
    ret;
}

fn main(void): i32 {
    stream.[i32] s = nats();
    stack i32 taken = 0;
    while (taken < 5) {
        stack i32 v = await.next(s);
        if (stream.done(s)) { break; }
        print.('{}\n', v);
        taken = taken + 1;
    }
    stream.drop(s);    // frame freed, body never resumes
    ret 0;
}
```

---

## Async methods

`async fn` defined inside a struct body is an instance method. It has access to `this`:

```stasha
type Counter: struct {
    ext i32 base;
    ext async fn next(i32 step): i32 {
        ret this.base + step;
    }
    ext async fn values(i32 n): stream.[i32] {
        stack i32 i = 0;
        while (i < n) {
            yield this.base + i;
            i = i + 1;
        }
        ret;
    }
}

stack Counter c = Counter.new(100);
stack i32 v = await(c.next(5));          // 105
stream.[i32] s = c.values(3);            // yields 100, 101, 102
```

Both task and stream variants are supported. The method receives `this` as a regular pointer argument alongside the declared parameters.

---

## Generic async functions

`@comptime[T] async fn` is supported for both tasks and streams:

```stasha
async fn @comptime[T] identity(stack T x): T { ret x; }

async fn @comptime[T] repeat(stack T x, stack i32 n): stream.[T] {
    stack i32 i = 0;
    while (i < n) { yield x; i = i + 1; }
    ret;
}

stack i32  r  = await(identity.[i32](42));
stack i64  r2 = await(identity.[i64](999));
stream.[i32] s = repeat.[i32](7, 3);   // yields 7, 7, 7
```

Type parameters are instantiated at each call site. The coroutine prologue is applied to each instantiation independently.

---

## `thread.(fn)` vs `async fn`

`async fn` and `thread.(fn)` are separate dispatch models:

| | `async fn` + `await` | `thread.(fn)` + `future` |
|---|---|---|
| Execution | cooperative, caller-driven, same thread | parallel (POSIX thread pool) |
| Returns | `future.[T]` (typed coro frame) | `future` (untyped pool handle) |
| Await | `await(f)` or `await.(fn)(args)` | `future.get.(T)(f); future.drop(f);` |
| Poll | `future.ready(f)` | `future.ready(f)` |
| Fan-in all | `await.all(...)` round-robin | manual loop |
| Fan-in race | `await.any(...)` cooperative first-complete | manual loop |
| Cancellation | `stream.cancel(s)` (streams only) | none |

Use `async fn` for cooperative coroutine-style logic and typed coroutine handles. Use `thread.(fn)` when you need real parallelism.

---

## Frame layout (informational)

Every coroutine (task or stream) has a promise header that the compiler reads and writes through `llvm.coro.promise(handle, 8, false)`:

```text
%__sts_coro_prom_hdr = {
    i32 complete,      // 1 once task body has returned
    i32 eos,           // 1 once stream body hits ret;
    i32 item_ready,    // 1 between yield expr; and next consumer drive
    i32 has_error,     // reserved
    i32 is_stream,     // 1 for streams, 0 for tasks
    i32 cancelled,     // 1 after stream.cancel(s)
    ptr continuation,  // awaiting coroutine to resume
    ptr child          // current child coroutine handle
}
// Stream promise: { header, T item }
// Task promise:   { header, T result }
```

`cg_coro.c` in the compiler emits all reads/writes through `sts_call_coro_promise` which expands to a GEP + load/store via the promise intrinsic.

### Pass pipeline

Every module containing any `async fn` runs:

```
coro-early , cgscc(coro-split) , coro-cleanup , globaldce
```

after IR generation. Every `async fn` receives the `presplitcoroutine` attribute so CoroEarly adopts it as a Switch-Resumed coroutine. CoroSplit produces the resume/destroy clones. GlobalDCE removes the pre-split entry stub.

---

## Limitations (v1)

| limitation | notes |
|---|---|
| No parallel tasks | `await.all`/`await.any` drive cooperatively on the caller thread. Use `thread.(fn)` for parallelism. |
| Cooperative race only | `await.any` observes coroutine completion, not wall-clock thread completion. CPU-bound tasks must suspend explicitly to share progress. |
| Cancellation propagation | `stream.cancel`/loser cancellation flags a coroutine and is observed at resume edges such as `yield`, `yield;`, and `await`. Deep cancellation policy is still minimal. |
| Executor is thread-local | `yield;` queues the coroutine on a fixed-size thread-local executor; there is no IO reactor or cross-thread scheduler yet. |
| `ret expr;` in stream | rejected. Use `yield expr; ret;` for a last-value-then-end pattern. |
| `await` inside `freestanding` | not supported; the coroutine frame uses heap allocation. |

---

## Complete examples

[`examples/ex_coroutine_surface.sts`](../examples/ex_coroutine_surface.sts) — stream coroutines: counter, fibonacci, infinite early-drop, and `yield;`.

[`examples/ex_coro_tasks.sts`](../examples/ex_coro_tasks.sts) — task coroutines + streams + cancellation + async methods + generic async fns + cooperative `await.all`/`await.any`.

[`examples/ex_async.sts`](../examples/ex_async.sts) — typed coroutine future operations and interop with untyped `thread.(fn)` futures.

Build any of them:
```sh
bin/stasha build examples/ex_coro_tasks.sts -o /tmp/demo
bin/stasha test  examples/ex_coro_tasks.sts && ./a.test
```
