# Coro Plan

## Big Goal

Make real LLVM coroutine system. Replace current thread-pool async sugar.

Need one model with all:

- `async fn` lowered by `llvm.coro.*`
- `await` inside async fn
- `yield expr` inside async fn for async stream/generator
- `yield;` for no-value cooperative resched
- typed final return for task
- typed yielded item for stream
- continuation passing
- executor scheduling
- cancellation
- destroy
- bridge to old thread runtime during migration

## Truth Now

Now system not coro. Now system is typed thread-pool dispatch.

- `async fn` only marker, not body transform
- `async.(fn)(args)` -> `__thread_dispatch(...)`
- `await(f)` blocks thread with `__future_get` / `__future_wait`
- `await.all` / `await.any` are thread-future fan-in, not coro combinators
- `TypeFuture` now opaque runtime ptr ABI in `src/ast/ast.h`, `src/parser/parser.c`, `src/codegen/cg_types.c`
- no `llvm.coro.*` lowering exists
- no coro pass pipeline exists
- `src/runtime/thread_runtime.c` is pthread backend; keep only as compat backend later

Repo pieces already there:

- AST: `TypeFuture`, `NodeAsyncCall`, `NodeAwaitExpr`, `NodeAwaitCombinator`, `NodeFutureOp`
- Parser: `async fn` marker on `NodeFnDecl`; `await(...)`, `await.(fn)(args)`, `await.all/any(...)`
- Codegen: `gen_async_call()` -> `gen_dispatch_to_pool()`; `gen_await()` does blocking wait + auto-drop; `await.all/any` use `__future_get`, `__future_drop`, `__future_wait_any`
- Docs: `wiki/Async-Await.md` says current async/await is not coroutine system and `await` blocks thread

So migration big architecture change, not small cleanup.

## Main Choices

Keep source feel:

- keep `async fn`
- keep `await(f)`
- keep `await.(fn)(args)`
- keep `future.[T]` first, but now means coroutine task handle

Add:

- `stream.[T]` for yielding async fn
- `yield expr`
- `yield;`
- `await.next(stream)` for v1 stream consume

Rules:

- `async fn` with no `yield expr` => task coroutine
- `async fn` with any `yield expr` => stream coroutine
- task coroutine may `ret expr`
- stream coroutine v1 may only `ret;`, not non-void `ret expr`
- stream coroutine ends with eos + optional error, not second typed final value
- `await`, `yield expr`, `yield;` only legal inside `async fn`
- `future.[T]` only for single-result task, never for stream
- calling yielding `async fn` returns `stream.[T]`
- save nicer sugar like `for await` for later

## AST + Types

Change `src/ast/ast.h`.

Add:

- `TypeStream`
- coro flavor enum on fn decl:
  `CoroNone`, `CoroTask`, `CoroStream`, `CoroUnknown`
- `NodeYieldExpr` for `yield expr`
- `NodeYieldNowExpr` for `yield;`
- fn fields:
  `coro_flavor`, `yield_type`, `has_await`, `has_yield_value`, `has_yield_now`

Keep:

- `NodeAwaitExpr`
- `NodeAwaitCombinator`
- `NodeAsyncCall`

Refactor meaning:

- `TypeFuture`: from "pthread future ptr ABI" to "task handle source type"
- `NodeAwaitExpr`: from blocking wait to suspend/resume when inside async fn

## Semantic Pass

Add coroutine-analysis pass before LLVM codegen.

Pass must:

- infer fn flavor from body
- if any `yield expr`, mark `CoroStream`
- if only `await` / `yield;` / `ret`, mark `CoroTask`
- reject `yield expr` outside `async fn`
- reject `yield;` outside `async fn`
- reject `await` outside `async fn`
- infer stream item type from all `yield expr`
- reject mixed yield types
- reject non-void `ret` in stream coroutine v1
- keep current `async.(fn)(args)` marker checks
- also reject dispatch forms that expect task but target stream coroutine

## LLVM Lowering

One shared coro model for task + stream:

1. `llvm.coro.id`
2. `llvm.coro.size`
3. alloc frame
4. `llvm.coro.begin`
5. use `llvm.coro.suspend` for all await/yield/final suspend points
6. `llvm.coro.destroy` on destroy path
7. `llvm.coro.end` at end

### Frame Must Hold

All coroutines:

- resume index / state
- spilled locals crossing suspend
- cleanup flags for partial init
- executor ptr
- continuation handle
- cancellation state
- completion state
- error state

Task also:

- final result slot
- completion waiter / continuation registration

Stream also:

- current yielded item slot
- item-ready flag
- consumer waiting flag
- end-of-stream flag

## Task Lowering

For `async fn foo(...): T` with no `yield expr`:

- lower body as coroutine-producing function
- alloc frame with `llvm.coro.size`
- init runtime task header + result storage
- schedule first resume on executor
- return lightweight task handle

`ret expr`:

- store result in task slot
- mark complete
- wake continuation / awaiter
- jump final suspend

Final suspend:

- dedicated block
- `llvm.coro.suspend` with final-suspend meaning
- destroy path calls `llvm.coro.destroy`
- end with `llvm.coro.end`

## Await Lowering

For `await expr` in task or stream coro:

1. eval awaitable
2. ready-check through runtime await hooks
3. if ready, extract value and continue
4. if not ready:
   register current coro as continuation on child task
   spill live-after-resume values to frame
   `llvm.coro.suspend`
   on resume reload values and extract child result
5. if child errored, propagate through language error model

Need fast path: if already ready, no needless executor round-trip.

## Stream Lowering

For `async fn` with `yield expr`:

- call builds stream coroutine
- call returns `stream.[T]`
- producer resumes by consumer demand or scheduler policy
- each `yield expr` is suspend point

`yield expr`:

1. eval value
2. store in current-item slot
3. set item-ready
4. wake waiting consumer continuation
5. `llvm.coro.suspend`
6. on resume clear item-ready and continue after yield

`yield;`:

1. enqueue self back onto executor ready queue
2. `llvm.coro.suspend`
3. resume next block, no produced item

Stream complete:

- mark eos
- wake waiting consumer continuation
- jump final suspend

## Worked Lowering Sketches

Task example source:

```stasha
async fn add_then_square(i32 a, i32 b): i32 {
    stack i32 sum = await.(add)(a, b);
    stack i32 sq  = await.(square)(sum);
    ret sq;
}
```

Task shape:

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

Stream example source:

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

Stream shape:

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

## LLVM Pass Pipeline

Run coro passes after IR gen, before object emit:

- `coro-early`
- `coro-split`
- `coro-elide`
- `coro-cleanup`

Need:

- verify IR before/after coroutine transform where practical
- optimized builds keep coroutine correctness
- allow stack elide when LLVM proves frame non-escaping

## Runtime

Add new coroutine runtime beside old thread runtime.

### Core Objects

Coroutine header has:

- frame ptr
- resume fn ptr
- destroy fn ptr
- executor ptr
- state flags
- refcount
- cancellation flags
- continuation ptr

Task state has:

- result storage
- error storage
- completion status
- awaiter / continuation registration

Stream state has:

- current yielded item storage
- item-ready flag
- consumer waiting state
- terminal completion flag
- terminal error storage

Executor v1 is single-threaded:

- ready queue
- `schedule(task_or_stream)`
- `wake(coroutine)`
- `run()`

API must let later multithreaded executors reuse same task/stream handles.

### Runtime Behavior

Task await:

- parent installs self as continuation on child task
- child completion enqueues parent
- parent resumes and extracts result

Stream next:

- consumer asks next through runtime `next` API
- producer resumes until yielded / completed / errored
- producer stores yielded item then suspends

Scheduler yield:

- current coro re-enqueued on ready queue
- coro suspends

Completion:

- finishing task or stream wakes all relevant continuation paths

Destroy:

- dropped task/stream calls `llvm.coro.destroy` exactly once when final ownership released

Cancellation:

- cooperative only in v1
- check at await / `yield expr` / `yield;` boundaries and before resume where practical

## Interop

Keep old thread runtime for now, but only for explicit `thread.(fn)` usage + bridge path.

Rules:

- old `thread.(fn)` / raw `future` still available
- bridge awaitables adapt old thread futures into new task-await semantics
- do not use blocking futures as core async implementation
- rebuild `await.all` / `await.any` on coroutine tasks, not pthread cond waits

## Migration Order

1. Add coro flavor metadata + yield nodes to AST.
2. Add `TypeStream`; refactor `TypeFuture` into task-handle type; keep `future.[T]` parse.
3. Add coroutine semantic pass for legality + flavor inference.
4. Extend lexer/parser for `yield expr` and `yield;`.
5. Add coro intrinsic declarations + builder helpers in codegen.
6. Add runtime coroutine header, task state, stream state, executor API beside legacy thread runtime.
7. Lower task-producing `async fn` as LLVM coroutines.
8. Lower generator-producing `async fn` as LLVM coroutines with same frame model.
9. Rework `async.(fn)(args)` and direct async calls to build coroutine tasks/streams, not pthread jobs.
10. Reimplement `await` in async bodies as real suspend/resume.
11. Add stream-consume API + lowering for next item.
12. Add `yield;` lowering + executor support.
13. Rebuild `await.all` and `await.any` as coroutine combinators.
14. Add legacy thread-future adapters.
15. Remove eager async thread-wrapper generation once coroutine dispatch replaces it.
16. Update examples + docs for async tasks, async streams, scheduler-yield.

## Keep / Delete / Refactor

Keep temporary:

- thread pool runtime for explicit `thread.(fn)` compat
- raw future runtime API only for bridge adapters

Delete or demote:

- async reliance on `__thread_dispatch`
- blocking `__future_get` as core meaning of `await`
- blocking `__future_wait` as core meaning of `await`
- `__future_wait_any` as core fan-in impl
- eager async thread wrappers after coroutine lowering complete

Refactor:

- `TypeFuture`
- `NodeAwaitExpr`
- async docs
- async examples
- ownership diagnostics
- fan-in combinators
- codegen runtime fn registration

## Public API

Source keep:

- `async fn`
- `await(f)`
- `await.(fn)(args)`
- `future.[T]` for task coroutines

Source add:

- `stream.[T]`
- `yield expr`
- `yield;`
- `await.next(stream)`

Runtime add new coro API for:

- task create / handle init
- stream create / handle init
- continuation register
- executor schedule
- stream next poll / wait
- task complete
- stream item publish
- cancellation request
- destroy / release

Choose C names that do not collide with old runtime, like `__coro_*`, `__task_*`, `__stream_*`.

## Tests

Parsing + semantics:

- `await` only in async coroutines
- `yield expr` only in async coroutines
- `yield;` only in async coroutines
- mixed yield type mismatch rejected
- task-vs-stream flavor inferred/validated right
- non-void `ret` in stream coroutine rejected in v1

Task IR:

- has `llvm.coro.id`
- has `llvm.coro.begin`
- has suspend site for each await
- has final suspend
- has destroy path
- has `llvm.coro.end`

Stream IR:

- has suspend at every `yield expr`
- has suspend at every `yield;`
- producer resume points preserved right

Runtime task behavior:

- single await
- multiple awaits
- nested async calls
- final result propagation
- void-return tasks
- error propagation through await

Runtime stream behavior:

- yielded values stay in order
- await between yields works
- scheduler-yield fairness
- consumer waiting before producer yield
- producer yielding before consumer wait
- clean completion
- clean early drop

Combinators:

- `await.all` preserves order
- `await.any` resumes one winner path safely
- no use-after-free on loser cleanup

Cancellation + destruction:

- dropped unfinished task destroys safely
- dropped stream destroys safely mid-sequence
- cancellation seen at await/yield boundaries
- no leaks after cancellation

Compatibility:

- legacy `thread.(fn)` jobs still bridge into coroutine await

Optimization:

- coro pass pipeline succeeds
- optimized builds keep task + stream correctness

## Old Code Impact

Source-compatible stays:

- `async fn`
- `async.(fn)(args)` for task coroutines
- `await(f)`
- `await.(fn)(args)`
- `future.[T]` for task coroutines

Meaning changes:

- `await` inside async fn now suspends, not blocks
- `async fn` now changes lowering, not just dispatch eligibility
- yielding async fn returns stream, not task future

New surface needed:

- `stream.[T]`
- `await.next(stream)` in v1

## Assumptions

- file is `wiki/Coroutine-Migration-Plan.md`
- doc must be concrete, implementation-ready, not vague concept note
- `yield` ships in first-class migration, not later
- both value-yielding async generators and no-value scheduler-yield are required
- `future.[T]` stays compatibility spelling for single-result tasks only; streams get distinct type
