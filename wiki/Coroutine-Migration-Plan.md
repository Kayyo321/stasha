# Unified Async Coroutine And Yield Migration Plan

## Summary

Create `wiki/Coroutine-Migration-Plan.md` as a concrete implementation plan for replacing the current thread-pool future sugar with a unified LLVM coroutine system that supports all of the following in one model:

- `async fn` lowered with `llvm.coro.*`
- `await` inside async functions
- `yield value` inside async functions for async-stream / async-generator semantics
- a separate no-value cooperative scheduler yield
- typed final return values plus typed yielded element values
- continuation passing, executor scheduling, cancellation, destruction, and interop with the existing thread runtime during migration

Current implementation facts this plan is anchored on:

- Today `async fn` is only a dispatch marker, not a coroutine body transform.
- `async.(fn)(args)` lowers to `__thread_dispatch(...)`.
- `await(f)` blocks the current thread via `__future_get` / `__future_wait`.
- `await.all` and `await.any` are thread-future fan-in helpers, not coroutine combinators.
- `TypeFuture` is currently an opaque runtime pointer ABI in `src/ast/ast.h`, `src/parser/parser.c`, and `src/codegen/cg_types.c`.
- There is no existing `llvm.coro.*` lowering or coroutine pass integration.
- The current pthread runtime in `src/runtime/thread_runtime.c` should become a compatibility backend, not the coroutine substrate.

Chosen defaults for the migration:

- Preserve `async fn`, `await(f)`, and `await.(fn)(args)` as the main surface.
- Preserve `future.[T]` initially as source syntax for coroutine-backed tasks.
- Add value-yielding async generators and a separate scheduler-yield primitive in the same coroutine framework.
- Treat async functions as belonging to two coroutine-capable families: returning tasks and returning async streams.
- Keep one shared frame/runtime model for both async tasks and async generators.

## Current State

The current implementation is a typed thread-pool dispatch layer, not a coroutine system.

- AST:
  `TypeFuture`, `NodeAsyncCall`, `NodeAwaitExpr`, `NodeAwaitCombinator`, and `NodeFutureOp` already exist.
- Parser:
  `async fn` is parsed as a marker on `NodeFnDecl`; `await(...)`, `await.(fn)(args)`, and `await.all/any(...)` are parsed as expressions.
- Codegen:
  `gen_async_call()` lowers to `gen_dispatch_to_pool()`. `gen_await()` lowers to blocking runtime calls and auto-drops the future. `await.all` and `await.any` are lowered around `__future_get`, `__future_drop`, and `__future_wait_any`.
- Runtime:
  `src/runtime/thread_runtime.c` provides a pthread-based worker pool, opaque `__future_t`, condition-variable waits, and queue-based job submission.
- Documentation:
  `wiki/Async-Await.md` explicitly states that current async/await is not a coroutine system and that `await` blocks the current thread.

This means the migration is architectural, not incremental cleanup of a half-lowered coroutine path.

## Language And Type-System Design

Define three coroutine-facing concepts:

- `future.[T]` as the compatibility spelling for a task-like async result that eventually completes with `T`
- `stream.[T]` as a new public type for async generators that `yield T` multiple times
- a no-value scheduler-yield operation spelled `yield;`

### Source Rules

- `async fn` with no `yield value` lowers to a task coroutine producing one final result.
- `async fn` with at least one `yield value` lowers to an async generator / stream coroutine.
- `ret` remains the final completion path for task coroutines.
- For yielding async functions, v1 forbids non-void terminal returns. A yielding `async fn` completes with end-of-stream plus optional error, not a second typed final value.
- `yield expr` is only legal inside `async fn`.
- `yield;` is only legal inside `async fn`.
- `await expr` is only legal inside `async fn`.

### Surface Compatibility

- Keep `async.(fn)(args)` and `await.(fn)(args)` for task-producing async functions.
- Keep current `future.[T]` syntax for task-producing async functions only.
- Do not overload `future.[T]` to mean both single-shot tasks and streams.
- Introduce stream construction through ordinary async-function invocation:
  calling a yielding `async fn` returns `stream.[T]`.
- Introduce stream consumption via a new builtin surface:
  `await.next(stream)` returns the next yielded item or end-of-stream state.
- Reserve ergonomic sugar such as `for await` for a later follow-up after the core runtime and lowering are stable.

### AST Additions

Update `src/ast/ast.h` to add:

- `TypeStream`
- coroutine flavor metadata on function declarations:
  `CoroNone`, `CoroTask`, `CoroStream`, `CoroUnknown`
- `NodeYieldExpr` for `yield value`
- `NodeYieldNowExpr` for `yield;`
- function declaration fields:
  `coro_flavor`, `yield_type`, `has_await`, `has_yield_value`, `has_yield_now`

Keep:

- `NodeAwaitExpr`
- `NodeAwaitCombinator`
- `NodeAsyncCall`

Refactor:

- `TypeFuture` from "opaque pointer to pthread future" into "task handle type in source language"
- `NodeAwaitExpr` semantic meaning from blocking wait to coroutine suspension when inside async code

### Semantic Analysis Requirements

Add a coroutine-analysis pass before LLVM codegen:

- infer coroutine flavor from function body
- if any `yield value` appears, mark function as `CoroStream`
- if only `await` / `yield;` / `ret` appear, mark function as `CoroTask`
- reject `yield value` outside `async fn`
- reject `yield;` outside `async fn`
- reject `await` outside `async fn`
- infer stream element type from all `yield value` sites
- reject mixed `yield` types
- reject non-void `ret` inside a stream coroutine in v1
- preserve current async marker checks for `async.(fn)(args)`, but extend them to reject dispatch forms that target stream coroutines where a task is required

## LLVM Lowering Design

Use one shared coroutine lowering scheme for both tasks and streams:

1. emit `llvm.coro.id`
2. emit `llvm.coro.size`
3. allocate frame memory
4. emit `llvm.coro.begin`
5. emit suspension sites with `llvm.coro.suspend`
6. emit destroy path with `llvm.coro.destroy`
7. terminate with `llvm.coro.end`

### Frame Layout

Every coroutine frame must carry:

- resume index / coroutine state
- spilled locals that live across suspension points
- cleanup flags for partially initialized values
- executor pointer
- continuation handle
- cancellation state
- completion state
- error state

Task coroutine frames additionally carry:

- final result slot
- completion waiters / continuation registration

Stream coroutine frames additionally carry:

- current yielded item slot
- item-ready flag
- consumer waiting flag
- end-of-stream flag

### Task Coroutine Lowering

For `async fn foo(...): T` with no `yield value`:

- lower function body as a coroutine-producing function that returns a task handle
- allocate coroutine frame with `llvm.coro.size`
- initialize runtime task header and promise/result storage
- schedule initial resume on the executor
- return a lightweight handle to the caller

Return lowering:

- `ret expr` stores the value into the task result slot
- mark task completion
- wake registered continuation / awaiter
- branch to final suspend

Final suspend lowering:

- emit a dedicated final-suspend block
- call `llvm.coro.suspend` with final-suspend semantics
- on destroy path call `llvm.coro.destroy`
- close with `llvm.coro.end`

### Await Lowering

For `await expr` inside a task or stream coroutine:

1. evaluate the awaitable expression
2. check readiness through runtime await hooks
3. if ready:
   extract value immediately and continue
4. if not ready:
   register current coroutine as continuation on the child task
   store any live values needed after resumption into the frame
   emit `llvm.coro.suspend`
   on resume, reload live values and extract child result
5. if child completed with error:
   propagate through the language's error model

The fast path must avoid executor round-trips when the awaited value is already complete.

### Stream Coroutine Lowering

For `async fn` with `yield value`:

- function call constructs a stream coroutine and returns `stream.[T]`
- producer is resumed by consumer demand or scheduling policy
- every `yield expr` is a suspension point

`yield expr` lowering:

1. evaluate yield value
2. store value into current-yield slot in the frame
3. set item-ready state
4. wake any waiting consumer continuation
5. emit `llvm.coro.suspend`
6. on resume, clear item-ready and continue after the yield site

`yield;` lowering:

1. enqueue current coroutine back onto executor ready queue
2. emit `llvm.coro.suspend`
3. resume at the next block with no produced item

Stream completion:

- mark end-of-stream
- wake waiting consumer continuation
- branch to final suspend

### Worked Lowering Example: Task Coroutine

Source:

```stasha
async fn add_then_square(i32 a, i32 b): i32 {
    stack i32 sum = await.(add)(a, b);
    stack i32 sq  = await.(square)(sum);
    ret sq;
}
```

Lowering shape:

```text
foo():
  coro.id
  coro.size
  alloc frame
  coro.begin
  schedule initial execution
  return task handle

resume.entry:
  call add coroutine
  if ready -> continue
  else install continuation; coro.suspend

resume.after_add:
  extract add result
  call square coroutine
  if ready -> continue
  else install continuation; coro.suspend

resume.after_square:
  extract square result
  store final result
  mark complete
  wake continuation
  goto final.suspend

final.suspend:
  coro.suspend(final)
  coro.end

destroy:
  coro.destroy
```

### Worked Lowering Example: Stream Coroutine

Source:

```stasha
async fn produce(i32 n): stream.[i32] {
    stack i32 i = 0;
    while (i < n) {
        yield i;
        yield;
        i = i + 1;
        await(timer_tick());
    }
    ret;
}
```

Lowering shape:

```text
produce():
  coro.id
  coro.size
  alloc frame
  coro.begin
  return stream handle

resume.loop:
  check loop cond
  yield value i:
    store current item
    mark item-ready
    wake consumer
    coro.suspend

resume.after_item:
  requeue self
  coro.suspend

resume.after_sched_yield:
  increment i
  await timer_tick():
    if ready -> continue
    else install continuation; coro.suspend

resume.after_await:
  jump loop

final.complete:
  mark eos
  wake consumer
  goto final.suspend

final.suspend:
  coro.suspend(final)
  coro.end
```

### LLVM Pass Pipeline

Extend the LLVM emission pipeline to support coroutine passes before object emission:

- `coro-early`
- `coro-split`
- `coro-elide`
- `coro-cleanup`

Requirements:

- coroutine passes must run after IR generation and before final object emission
- verification must run both before and after coroutine transformation where practical
- optimized builds must preserve coroutine correctness
- stack-allocation elision must be permitted when LLVM can prove safe non-escaping frames

## Runtime Design

Add a new coroutine runtime alongside the existing thread runtime.

### Core Runtime Objects

#### Coroutine Header

Every coroutine frame is associated with a header containing:

- frame pointer
- resume function pointer
- destroy function pointer
- executor pointer
- state flags
- refcount
- cancellation flags
- continuation pointer

#### Task State

- result storage
- error storage
- completion status
- awaiter / continuation registration

#### Stream State

- current yielded item storage
- item-ready flag
- consumer waiting state
- terminal completion flag
- terminal error storage

#### Executor

Start with a single-threaded executor:

- ready queue
- `schedule(task_or_stream)`
- `wake(coroutine)`
- `run()`

Design the API so later multithreaded executors can reuse the same task/stream handles.

### Required Runtime Behavior

Task await:

- parent coroutine installs itself as continuation on child task
- child completion enqueues parent on executor
- parent resumes and extracts result

Stream next:

- consumer requests next item through runtime `next` API
- producer resumes until one of:
  item yielded
  completed
  errored
- producer stores yielded item in stream slot and suspends

Scheduler yield:

- current coroutine is re-enqueued on ready queue
- coroutine suspends

Completion:

- finishing task or stream wakes all relevant continuation paths

Destroy:

- dropped tasks/streams call `llvm.coro.destroy` exactly once when final ownership is released

Cancellation:

- cooperative only in v1
- checked at await/yield/scheduler boundaries and before resumption where practical

### Interop With Existing Thread Runtime

Keep the current thread runtime temporarily for explicit `thread.(fn)` usage and migration bridging.

Interop rules:

- existing `thread.(fn)` / raw `future` runtime remains available
- bridge awaitables adapt legacy thread futures into new task await semantics
- no attempt is made to use blocking futures as the internal implementation model for async coroutines
- `await.all` / `await.any` are rebuilt on top of coroutine tasks, not pthread condition waits

## Compiler Migration Sequence

1. Add coroutine flavor metadata and new yield nodes to AST.
2. Add `TypeStream` and refactor `TypeFuture` into a task-handle type while preserving `future.[T]` parsing.
3. Add coroutine semantic analysis for legality and flavor inference.
4. Extend lexer/parser to support `yield expr` and `yield;`.
5. Add coroutine intrinsic declarations and builder helpers in codegen.
6. Introduce runtime coroutine header, task state, stream state, and executor APIs beside the legacy thread runtime.
7. Lower task-producing `async fn` bodies as LLVM coroutines.
8. Lower generator-producing `async fn` bodies as LLVM coroutines using the same frame model.
9. Rework `async.(fn)(args)` and direct async calls to construct coroutine tasks or streams, not pthread jobs.
10. Reimplement `await` in async bodies as real suspension/resume.
11. Add stream-consumption API and lowering for requesting the next yielded item.
12. Add `yield;` lowering and executor support.
13. Rebuild `await.all` and `await.any` as coroutine combinators.
14. Add adapters for legacy thread futures during migration.
15. Remove eager generation of thread wrappers for async functions once coroutine-backed dispatch fully replaces them.
16. Update examples/docs to demonstrate async tasks, async streams, and scheduler-yield semantics.

## Keep, Delete, Refactor

### Keep Temporarily

- thread pool runtime as compatibility backend for explicit `thread.(fn)` usage
- raw future runtime API for bridge adapters only

### Delete Or Demote

- async-specific reliance on `__thread_dispatch`
- blocking `__future_get` as the core meaning of `await`
- blocking `__future_wait` as the core meaning of `await`
- `__future_wait_any` as the implementation basis for coroutine fan-in
- eager generation of async thread wrappers once coroutine lowering is complete

### Refactor

- `TypeFuture`
- `NodeAwaitExpr`
- async docs
- async examples
- ownership diagnostics
- fan-in combinators
- codegen runtime function registration

## Public API And Interface Changes

### Source Language

- keep:
  `async fn`, `await(f)`, `await.(fn)(args)`, `future.[T]`
- add:
  `stream.[T]`
  `yield expr`
  `yield;`
  `await.next(stream)`

### Runtime API

Add a new coroutine runtime header and implementation with functions conceptually equivalent to:

- task creation / handle init
- stream creation / handle init
- continuation registration
- executor scheduling
- stream next polling / waiting
- task completion
- stream item publication
- cancellation request
- destroy / release

The exact C symbol names should be chosen to avoid collision with the legacy thread runtime and to make the split obvious, for example `__coro_*`, `__task_*`, and `__stream_*`.

## Test Plan

### Parsing And Semantics

- `await` valid only in async coroutines
- `yield value` valid only in async coroutines
- `yield;` valid only in async coroutines
- mixed-yield type mismatches rejected
- task-vs-stream coroutine flavor inferred or validated correctly
- non-void `ret` in stream coroutine rejected in v1

### Task Codegen IR

- emitted IR contains `llvm.coro.id`
- emitted IR contains `llvm.coro.begin`
- emitted IR contains suspend sites for each await
- emitted IR contains final suspend
- emitted IR contains destroy path
- emitted IR contains `llvm.coro.end`

### Stream Codegen IR

- emitted IR contains suspension at every `yield value`
- emitted IR contains suspension at every `yield;`
- producer resume points are preserved correctly

### Runtime Task Behavior

- single await
- multiple awaits
- nested async calls
- final result propagation
- void-return tasks
- error propagation through await

### Runtime Stream Behavior

- multiple yielded values in order
- await between yields
- scheduler-yield fairness
- consumer waiting before producer yield
- producer yielding before consumer wait
- clean completion
- clean early drop

### Combinators

- `await.all` preserves order
- `await.any` resumes one winner path safely
- no use-after-free on loser cleanup

### Cancellation And Destruction

- dropped unfinished task destroys safely
- dropped stream destroys safely mid-sequence
- cancellation observed at await/yield boundaries
- no leaks after cancellation

### Compatibility

- legacy `thread.(fn)` jobs can still be bridged into coroutine awaits during migration

### Optimization

- coroutine pass pipeline succeeds
- optimized builds preserve correctness for task and stream coroutines

## Migration Notes For Existing Code

What stays source-compatible:

- `async fn`
- `async.(fn)(args)` for task coroutines
- `await(f)`
- `await.(fn)(args)`
- `future.[T]` for task coroutines

What changes semantically:

- `await` inside async bodies becomes suspension, not blocking wait
- `async fn` is no longer just a dispatch marker; it changes function lowering
- yielding async functions return streams, not task futures

What needs a new surface:

- stream-producing async functions need `stream.[T]`
- consumers need `await.next(stream)` in v1

## Assumptions

- The file created during execution is `wiki/Coroutine-Migration-Plan.md`.
- The document is concrete and implementation-ready, not a high-level concept note.
- `yield` support is part of the first-class coroutine migration, not a deferred extension.
- Both value-yielding async generators and a separate no-value scheduler-yield are included in the design.
- `future.[T]` remains the compatibility spelling for single-result async tasks only; streams get their own distinct type.
