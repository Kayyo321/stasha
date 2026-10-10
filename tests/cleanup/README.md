# Hosted cleanup regression contract

Run with a freshly built compiler:

```sh
python3 tests/cleanup/run.py --compiler ./bin/stasha \
  --opt 0,1,2,3 --zone-probe --artifacts /tmp/stasha-cleanup-results
python3 tests/cleanup/run.py --list
python3 -m unittest discover -s tests/cleanup -p 'test_runner.py'
```

The default matrix runs all 26 manifest cases at optimization levels 0, 1, 2, and 3.
Use `--case NAME` (repeatable) and `--opt 0,2` for a focused run. Each compiler
invocation and executable run is isolated in a fresh directory with a timeout.
The runner preserves the command, exit status, stdout, stderr, source and
binary hashes, available IR dumps, and a JSON result in its artifact directory.
A passing parser or mock-runner test is not a passing compiler/runtime test.
These are hosted regressions, not evidence of a freestanding/kernel profile.

## Oracle

`cases.json` is the runnable manifest. Source paths are relative to this
directory. Every success entry contains the complete expected stdout,
including its final newline; the adjacent `.stdout` file is an identical,
human-reviewable golden trace. Do not regenerate goldens from compiler output.
Each successful program must exit zero, produce exactly its trace, and write
nothing to stderr. The final `NAME: ok` line occurs after the tested cleanup
and small scalar checks, so neither an early exit nor a correct trace prefix
is success. Tracked struct destructors print unique IDs to expose duplicate,
missing, premature, or incorrectly ordered cleanup.

Rejection cases must produce a source-located compiler error that matches the
specified diagnostic and leave no executable. Signals, timeouts, verifier or
sanitizer failures, missing diagnostics, and unrelated failures are not valid
rejections. All rejection fixtures are deliberately syntactically valid;
their expected failures belong to semantic lowering.

## Required semantics

- Leaving scopes normally or through return, break, or continue cleans the
  exited scopes from innermost to outermost, exactly once each.
- Within each scope, all defers execute last-in-first-out, followed by all
  registered destructors last-in-first-out. This preserves the established
  two-phase per-scope order rather than interleaving by declaration time.
- Return expressions, including function calls and multiple returned values,
  are evaluated before cleanup. Deferred mutation or release of their inputs
  cannot alter the values already selected for return.
- A directly returned named struct local transfers its destructor obligation
  to the receiving caller on that return edge only. Compiling a return in one
  branch must not remove cleanup from another branch or a later join. This is
  a narrow named-local rule, not a claim of general move/alias analysis.
- A `for` initializer has its own scope. It is cleaned after normal exhaustion
  (including zero iterations), break, and return, and survives continue. The
  initializer's names are no longer visible after the loop.
- Break from a switch exits that switch, retaining enclosing loop scopes.
  Continue inside a switch nested in a loop continues that enclosing loop,
  cleaning the switch and loop-body scopes but retaining any for initializer.
- A defer resolves names in its declaration-point lexical environment. Values
  are still read at cleanup time. Later declarations are unavailable, and a
  later same-name declaration must not replace the earlier binding.
- Each defer body has its own temporary lexical scope. Its locals, nested
  defers, and destructors cannot leak into another defer or another CFG edge.
  Return from a defer and break/continue targeting an enclosing construct
  outside it are rejected. Loops and switches created inside a defer may use
  their own break/continue normally.
- Lexical zones are released on every exit after the cleanup of their body.
  Zone-backed values must remain readable during body defers, and return
  values derived from them must be selected before those defers execute.

## Coverage

- `normal_order`: nested fallthrough, mixed declaration order, per-scope
  reverse defer order followed by reverse destructor order.
- `return_values`: scalar and multi-value returns with observable calls and
  defers that mutate the source variables.
- `early_return`: branch-local, block-local, and function-local unwinding on
  early return, plus the non-returning branch and later return.
- `return_heap_scalar`: a scalar loaded from a manually owned heap pointer;
  its defer overwrites the pointee and frees it.
- `aggregate_branch_join`: a named struct returned from one conditional branch
  still receives cleanup when execution follows the other branch.
- `aggregate_multi_return`: two named struct results transfer cleanup to the
  caller while a third local is destroyed in the callee.
- `for_init_lifetime`: normal, continue, zero-iteration, break, and return
  paths, with visible initializer and body destructors.
- `nested_loops`: inner while-loop continue/break retain the enclosing for
  body's resources until the outer iteration ends.
- `loop_families`: foreach, do-while, and infinite-loop cleanup, including
  normal foreach body completion and break/continue paths.
- `nested_switch`: nested switch breaks and a switch-to-outer-loop continue.
- `defer_shadow_lookup`: declaration-point binding with cleanup-time reads
  before and after a same-scope shadowing declaration.
- `defer_branch_locals`: deferred declarations and nested cleanup stay isolated
  while both an early-return edge and a later branch join are compiled.
- `defer_nested_control`: legal inner loop/switch control flow inside a defer.
- `defer_deep_stack`: more than four deferred entries and destructor entries,
  plus forty nested scopes inside cleanup, force cleanup-stack reallocation.
- `lexical_zones`: normal, continue, break, and nested return exits, with live
  zone-backed reads during cleanup and mutation after return-value selection.
- `negative/*`: escaping defer return/break/continue, out-of-scope for init
  and defer locals, and a defer that refers to a later declaration.

## Supplemental generated-function context regressions

The following five fixtures are narrowly scoped hosted regressions. They pass
the recorded O0–O3 native Linux execution matrix below, which does not qualify
coroutine cancellation or general signal support. They guard ordinary generated
functions against accidentally inheriting the enclosing async task's coroutine
state, while requiring the outer task's own cleanup to remain intact.

- `task_return_values`: an async task evaluates an observable return-value
  function call before a defer mutates its input.
- `task_nested_lambda`: an ordinary noncapturing lambda defined inside an
  async task uses its own return/defer semantics; the enclosing task then
  executes its own defer and returns the expected independently selected value.
- `task_lazy_generic`: a generic ordinary function is first instantiated from
  an async task, with independent generic-function and task cleanup.
- `task_watch_handler`: an ordinary watch handler is generated inside an async
  task and invoked synchronously by `send`. Its local defer executes before
  break unregisters it; a second send must not invoke it again, and the task
  subsequently performs its own cleanup and return.

- `task_watch_return`: the generated watch handler returns normally on its
  first invocation, then breaks/unregisters on its second. Neither return may
  target the enclosing task's coroutine blocks.

## Instrumented zone release probe

`--zone-probe` adds one extra case, `lexical_zones_instrumented`, at every
optimization level. It builds `zone_probe.c` with the host C compiler (`--cc`)
and links that object only into the instrumented case. The probe includes the
actual zone runtime under private names, traces each compiler-generated zone
release, and fails on missing, duplicate, empty, or untracked releases. Its
separate golden requires body defers before release, and inner-zone release
before outer-zone release. The regular `lexical_zones` case still uses the
ordinary runtime. Thus the comprehensive command runs 108 compile cases.

The native C probe self-test exercises the actual wrapped runtime: correct
release passes, injected leak and duplicate release exit 91. This is C-only
evidence, not Stasha execution. It does not qualify size-overflow checks,
alignment, OOM behavior, or general allocation safety.

The uninstrumented zone trace checks cleanup ordering and live reads; an ordinary successful
execution alone does not measure allocator release counts or prove absence
of leaks. Stronger allocation instrumentation or sanitizer evidence must be
reported separately. Likewise this corpus does not qualify coroutine
cancellation, arbitrary aggregate expressions, pointer escape analysis,
exceptions, general signal behavior, or OS-level teardown.

## Current evidence

- Current-source GCC+UBSan parser accepts all 26 fixtures (including the
  semantic-rejection fixtures). This checks syntax only.
- 10 mock runner tests pass, covering stale outputs, bad traces, warnings,
  crashes, timeouts, missing diagnostics, and retained provenance.
- 1 native C wrapper test passes, including both injected failure modes.
- The codegen translation unit passes GCC syntax checking against pinned
  LLVM source/build headers. Existing unrelated `-Wall` warnings remain.
- Actual pinned-LLVM-23 compiler execution on native x86-64 Linux: **108/108
  pass**, including the instrumented zone case, at `-o=0,1,2,3` on 2026-10-10.
  Compiler SHA-256:
  `0b55cd0dca14347ed6208c9a5f1a8ffb86fa1ef9ee77ab7b28c339dae35befd6`.
  Each positive compiled without warnings, passed LLVM verification, exited
  zero, and matched the exact trace; every negative met the diagnostic contract.
  External retained report: `implementation-execution/cleanup-etytjgf3/report.json`
  in the implementation audit evidence directory. Later compiler edits require
  a new run; this result belongs to the exact compiler hash above.
- Initial real runs correctly failed on fixture unused-variable warnings and
  unsupported member postfix increment syntax. Destructor-only locals now use
  intentional-unused names, header member increments use assignment, and the
  later-declaration negative fixture uses its local on the ordinary path. No
  expected cleanup trace or diagnostic assertion was weakened.
- Sanitizer-backed generated-program execution remains unqualified. The UBSan
  parser and native C probe are distinct evidence, not substitutes for it.

This patch centralizes exit-depth emission but emits cleanup inline on each
CFG exit; it does not yet share reusable cleanup basic blocks. General
definite-initialization, ownership/borrowing, alias, lifetime, bounds, and
permission-flow analyses remain separate P3 work.
