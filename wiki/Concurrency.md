# Concurrency

Stasha has built-in thread parallelism using a managed thread pool and futures. The runtime is automatically initialized — you don't need to configure or start it.

---

## Overview

Stasha's concurrency model:

- **Thread pool**: A pool of worker threads (min(CPU count, 64)) is created automatically on program startup.
- **`thread.(fn)(args)`**: Dispatch a function to the pool. Returns a `future`.
- **`future`**: A handle to the async result. Collect the result with `future.get.(T)(f)` or just wait with `future.wait(f)`.
- **No shared mutable state by default**: Use `atomic` globals for shared counters/flags, or use message-passing patterns.

---

## Dispatching Work

```stasha
fn square(i32 x): i32 {
    ret x * x;
}

future f = thread.(square)(7);
```

The function runs concurrently on a thread pool worker. `f` is your handle to the result.

Multiple dispatches run in parallel:
```stasha
future fa = thread.(square)(3);
future fb = thread.(square)(4);
future fc = thread.(square)(5);
// All three run concurrently
```

---

## Collecting Results

### `future.get.(T)(f)` — Block and Return

```stasha
future f = thread.(square)(7);
i32 result = future.get.(i32)(f);   // blocks until done, returns i32
print.('7^2 = {}\n', result);        // 49
future.drop(f);
```

### `future.wait(f)` — Block, Discard Result

```stasha
future f = thread.(worker)();
future.wait(f);     // blocks until worker() finishes
future.drop(f);
```

### `future.ready(f)` — Non-Blocking Check

```stasha
future f = thread.(heavy_task)();

// Do other work while waiting:
while !future.ready(f) {
    do_other_work();
}
i32 result = future.get.(i32)(f);
future.drop(f);
```

### `future.drop(f)` — Wait + Free

Always call `future.drop(f)` when you're done with a future. It waits for completion (if not already done) and frees the handle:

```stasha
future f = thread.(task)();
future.drop(f);   // waits if needed, then frees
```

---

## Complete Example: Parallel Computation

```stasha
mod parallel_demo;

lib "stdio" = io;

int fn square(i32 x): i32 { ret x * x; }
int fn add(i32 a, i32 b): i32 { ret a + b; }
int fn double_it(i32 x): i32 { ret x * 2; }

ext fn main(void): i32 {
    // Fire off 4 jobs concurrently:
    future fa = thread.(square)(3);
    future fb = thread.(add)(10, 32);
    future fc = thread.(double_it)(21);
    future fd = thread.(square)(9);

    // Collect results (each .get blocks until that future is ready):
    i32 ra = future.get.(i32)(fa);   //  9
    i32 rb = future.get.(i32)(fb);   // 42
    i32 rc = future.get.(i32)(fc);   // 42
    i32 rd = future.get.(i32)(fd);   // 81

    io.printf('%d %d %d %d\n', ra, rb, rc, rd);

    future.drop(fa); future.drop(fb);
    future.drop(fc); future.drop(fd);
    ret 0;
}
```

---

## Void-Returning Functions

For functions that don't return a value, use `future.wait()` + `future.drop()`:

```stasha
int fn log_event(void): void {
    // ... do work ...
}

future f = thread.(log_event)();
future.wait(f);
future.drop(f);
```

Or just `future.drop(f)` — it waits automatically:
```stasha
future f = thread.(log_event)();
future.drop(f);   // waits + frees
```

---

## Atomic Variables

For shared counters across threads, use `atomic`:

```stasha
int atomic i32 work_done = 0;

int fn worker(void): void {
    work_done = work_done + 1;
}

future f1 = thread.(worker)();
future f2 = thread.(worker)();
future.drop(f1);
future.drop(f2);

print.('work_done = {}\n', work_done);   // 2
```

Atomic operations are single-word load/store (sequentially consistent). For complex multi-field synchronization, use a mutex from the standard library.

---

## Mutex (from stdlib)

```stasha
libimp "mutex" from std;

threading.mutex_t m = threading.mutex_t.new();

// From any thread:
m.lock();
// ... critical section ...
m.unlock();

// Or with defer:
m.lock();
defer m.unlock();
// ... critical section ...
```

---

## Thread Pool Details

- Worker count: `min(nCPU, 64)` threads
- Auto-initialized on the first `thread.()` call (or via `__attribute__((constructor))`)
- Auto-shutdown on program exit via `__attribute__((destructor))`
- Job queue: lock-free ring buffer
- Each future is reference-counted

You don't need to initialize or shut down the thread pool manually.

---

## Patterns

### Fan-Out / Fan-In

```stasha
const i32 N = 8;
future futures[N];

for (i32 i = 0; i < N; i++) {
    futures[i] = thread.(compute)(i);
}

i32 total = 0;
for (i32 i = 0; i < N; i++) {
    total += future.get.(i32)(futures[i]);
    future.drop(futures[i]);
}

print.('total = {}\n', total);
```

### Work Queue (Thread-Safe Counter)

```stasha
int atomic i32 next_item = 0;
int const i32 NUM_ITEMS = 100;

int fn process_next(void): void {
    // Atomically claim the next item:
    i32 idx = next_item;
    next_item = next_item + 1;
    if idx < NUM_ITEMS {
        process_item(idx);
    }
}
```

### Background Task

```stasha
// Fire and forget — result is not needed:
future bg = thread.(save_log_async)();
future.drop(bg);   // waits for completion before proceeding
// (If truly fire-and-forget, keep the future and drop at program end)
```

---

## Common Mistakes

**Forgetting to `future.drop()`:**
```stasha
future f = thread.(task)();
i32 r = future.get.(i32)(f);
// forgot future.drop(f) — memory leak!
```

**Sharing non-atomic mutable state:**
```stasha
int i32 counter = 0;   // NOT atomic!

int fn worker(): void {
    counter++;   // data race — undefined behavior
}
```

Fix: use `atomic`:
```stasha
int atomic i32 counter = 0;   // safe
```

**Getting a void future:**
```stasha
future f = thread.(void_fn)();
i32 r = future.get.(i32)(f);   // ERROR: void_fn returns void
// Use future.wait(f) for void functions
future.wait(f);
future.drop(f);
```
