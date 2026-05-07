# Signals: `watch.()` and `send.()`

Stasha's signal system provides a typed, in-process pub/sub channel for one-shot or recurring messages. It is implemented entirely in user space — no OS signals, no `kill(2)` — and it is intended for cross-cutting events like shutdown hooks, panic listeners, and ad-hoc message dispatch.

```stasha
watch.(<type> name) => { ... }    // register a receiver
send.(<expr>);                     // dispatch by value type
```

Receivers are matched on the **payload type**. Sending a value of type `T` invokes every active `watch.(T name) => { ... }` receiver for `T`.

---

## Receivers: `watch.()`

A `watch.()` declaration registers a callback for a specific payload type. The callback may live anywhere — module top level, inside `fn`s, or inside an `@[[init]]` block:

```stasha
type sig_t: struct { i32 code; }

fn setup_rec(void): void {
    watch.(sig_t signal) => {
        print.(@'Got a signal! code is {signal.code}!\n');
    }
}
```

Forms:

```stasha
watch.(<TypeName> <var>) => { stmts }       // statement-list body
watch.(<TypeName> <var>) => stmt;           // single-statement body (with `;`)
```

The receiver is **active** as soon as the `watch.()` statement is reached. Receivers registered inside a function body remain active until either:

- Control flow leaves the function (the watcher is descoped), or
- The watcher hits a `break`, which permanently retires it.

`break` inside a `watch.()` body is special: it removes that specific receiver from the registry. After `break`, the same code path will not be invoked again, even if more signals are sent:

```stasha
watch.(sig_t signal) => {
    print.(@'Got signal {signal.code}\n');
    break;   // this receiver is now permanently retired
}
```

---

## Senders: `send.()`

`send.()` dispatches a value to every active receiver registered for that value's type:

```stasha
send.(sig_t{.code = 404});
```

Dispatch is **synchronous** in v1 — `send.()` does not return until every matching receiver has run. There is no thread fan-out, no queue, no asynchronous delivery. This keeps the order deterministic and avoids surprising race conditions between sender and receiver state.

If no receivers are registered for the type, `send.()` is a no-op.

---

## Built-in: `watch.(i32 exit_code)` in `@[[init]]`

A `watch.(i32 exit_code) => { ... }` placed inside an `@[[init]]` block is automatically wired to the program-exit path. Calling `quit.(code)` (or letting `main` return `code`) invokes the registered watcher with that integer:

```stasha
@[[init]] {
    watch.(i32 exit_code) => {
        quit.(exit_code);
    }
}
```

This pattern is useful for unconditional cleanup that must run even if `main` is bypassed (e.g. by a `quit.()` from inside a deeply nested helper).

---

## End-to-End Example

```stasha
mod ex_signals;

@[[init]] {
    watch.(i32 exit_code) => {
        quit.(exit_code);
    }
}

type sig_t: struct {
    i32 code;
}

fn setup_rec(void): void {
    watch.(sig_t signal) => {
        print.(@'Got a signal! code is {signal.code}!\n');

        send.(0);     // routes to the @[[init]] watcher above
        break;        // retire this watcher
    }
}

fn send_signal(void): void {
    send.(sig_t{.code = 404});
}

fn main(void): i32 {
    setup_rec();
    send_signal();
    ret 0;
}
```

Output:

```
Got a signal! code is 404!
```

(The `send.(0)` re-routes to the `i32` watcher, which calls `quit.(0)` — note the `break` retires the `sig_t` watcher first so no recursion occurs.)

---

## Type Matching Rules

Receivers match by **exact payload type**:

- `watch.(sig_t s) => { ... }` matches `send.(sig_t{...})` but not `send.(other_t{...})`.
- Generic types match per concrete instantiation: `watch.(my_t.[i32] v) => { ... }` and `watch.(my_t.[f32] v) => { ... }` are independent watchers.
- Tagged enums route on the enum type, not the variant. Inspect the variant inside the body.

There is no inheritance, interface widening, or implicit conversion. If you `send.(i64{...})` you cannot match it with `watch.(i32 v)`.

---

## Lifecycle and Lexical Scope

- Receivers in module scope (top-level `watch.()` outside any function) live for the duration of the program.
- Receivers in function scope live until the function returns OR until `break`.
- Receivers inside `@[[init]]` and `@[[exit]]` blocks live as long as the program — they are registered before `main` and never descoped.

If you need explicit removal, use `break` inside the body. There is no "remove watcher by handle" API in v1.

---

## Sender / Receiver Ordering

Within a single `send.()`:

- All receivers run synchronously in **registration order**.
- A receiver may itself call `send.(...)`. The recursive send fully drains its receivers **before** the outer send continues to the next receiver.
- A receiver may register **new** receivers via nested `watch.(...)`. They take effect for subsequent `send.()` calls but **not** for the in-flight send.

This is a depth-first, deterministic dispatch — easy to reason about; not suitable for cyclic listeners that could otherwise infinite-loop.

---

## Watch Captures

A `watch.()` body may capture enclosing locals using the same `.|captures|` syntax as lambda expressions. Place the capture list as the **first thing** inside the body braces:

```stasha
// by-value — handler gets a copy; mutations stay local to handler
stack i32 count = 0;
watch.(sig_t signal) {
    .|count|
    count = count + signal.code;   // modifies handler-local copy, not outer count
}

// by-ref — handler mutates outer variable
stack i32 total = 0;
watch.(sig_t signal) {
    .|&total|
    total = total + signal.code;
}
send.(sig_t{.code = 4});
send.(sig_t{.code = 6});
// total == 10
```

The capture list rules are identical to lambda captures: bare name = by value, `&name` = by reference. By-value captures copy the variable when the `watch.()` statement is reached. The env struct is stored on the stack at registration time and passed to every handler invocation.

---

## Crash Protection

Stasha programs automatically handle fatal OS signals (SIGABRT, SIGSEGV, SIGFPE, SIGILL, SIGBUS) via a crash runtime linked into every executable. Use `@[[crash]]` blocks to register cleanup code that runs before the process exits on a fatal signal:

```stasha
@[[crash]] {
    // runs on SIGABRT/SIGSEGV/SIGFPE/SIGILL/SIGBUS
    // signal name and last-C-call breadcrumb are printed automatically
    print.error.('[crash] flushing log before exit\n');
    // keep it signal-safe: no malloc, no locks
}

@[[exit]] {
    // runs on normal exit AND after @[[crash]]
    io.printf('[exit] goodbye\n');
}
```

The crash runtime also tracks the **last C library call site** (file, function, line) via a TLS breadcrumb updated before every `lib`-backed call. On a crash the handler prints:
```
crash: SIGABRT — last call: abort() — ex_crash.sts:44
```

### Attributes

| Attribute | Effect |
|-----------|--------|
| `@[[crash]] { ... }` | Register a crash cleanup block |
| `@[[crash: disable]];` | Remove all signal handlers; do not link `crash_runtime.a` |
| `@[[crash: no_tracking]]` on a `fn` | Disable TLS breadcrumb writes in that function (useful for hot paths) |

```stasha
@[[crash: no_tracking]]
int fn heavy_math(stack i32 n): i64 {
    // breadcrumb tracking suppressed — no overhead per C call
    ...
}
```

`@[[crash: disable]];` is a file-wide declaration that opts the entire module out of crash protection.

See [File-Headers](File-Headers) for the full `@[[...]]` syntax reference.

---

## Restrictions

**No async dispatch (v1).** `send.()` is synchronous. If you need fan-out across threads, dispatch the body of the receiver yourself with `thread.(fn)(args)` or `async.(fn)(args)` and return immediately.

**No type erasure / `any`.** Receivers must name a concrete type. There is no "match all signals" form.

**No cancellation.** Once a `send.()` starts, every active receiver runs to completion (sequentially). The only way to short-circuit is to call `quit.()` from inside a receiver.

**Receivers can panic.** A panic inside a receiver propagates back through the `send.()` call site. Catch with `recover` if your application requires resilience.

**Cross-thread sends are not safe in v1.** If thread A registers a watcher and thread B sends, the registry is not synchronised — protect with a mutex or only register from the main thread until v2.

---

## Implementation Notes

The signal system is implemented in:

- `src/parser/parse_stmt.c` — `watch.()` and `send.()` statement parsing
- `src/codegen/cg_signals.c` — registry data structures, send-dispatch lowering, `break` retirement
- `src/runtime/` — registry initialisation under `__attribute__((constructor))`

Watchers are stored in a per-type linked list keyed by a hash of the payload type's mangled name. Registration is O(1); dispatch is O(N) over the receiver list.

---

## When to Use Signals vs Other Tools

| Situation | Use |
|-----------|-----|
| Static, well-known caller/callee | Direct function call |
| Async work with a single result | `async.(fn)(args)` + `await(f)` |
| Many-to-many event broadcast | `send.()` / `watch.()` |
| Program-exit hooks | `@[[exit]] { ... }` |
| Init hooks before `main` | `@[[init]] { ... }` |
| OS signal handling (SIGINT, SIGTERM) | C interop via `lib "signal"` (Unix `sigaction`) |
| Fatal crash cleanup (SIGSEGV, SIGABRT, etc.) | `@[[crash]] { ... }` |

Signals are deliberately limited to in-process events. For OS signals, use `cheader "signal.h"` and `sigaction(2)` directly. For fatal crash handling, use `@[[crash]]` blocks — the runtime installs handlers automatically.
