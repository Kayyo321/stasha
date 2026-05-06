# Plan: Remove All v1 Restrictions + Add Watch Captures

## Context

The language has several explicitly half-baked features tagged as "v1" or "not supported yet". Goal: fully implement every incomplete feature so the language has no version-gated restrictions. Also adding captures to `watch` handlers per user request.

---

## Incomplete Features Found

| # | Location | Message | Severity |
|---|----------|---------|----------|
| 1 | `cg_expr.c:630` | "lambda may not capture local … from enclosing scope" | Core |
| 2 | `parse_expr.c:2170` | "lambda on the right of '\|>' is not supported in v1" | Medium |
| 3 | `analysis/coroutines.c:379` | "stream coroutine … cannot return a final value in v1" | Medium |
| 4 | `cg_expr.c:2579-2582` | "await.all/any() futures must share one element type" | Complex |
| 5 | `cg_expr.c:451` | "unsupported spread expression in compound initializer" | Medium |
| 6 | `cg_stmt.c:1902` | comptime_assert: "unsupported expression" | Low |

Plus the new feature: **watch captures** (`src/codegen/cg_signals.c`).

---

## Feature 1: Lambda Captures (Core)

### Design

Non-capturing lambdas stay as `fn*` (unchanged). Capturing lambdas produce a new `closure` type — a fat `{fn_ptr, env_ptr}` pair at the LLVM IR level.

**Unified capture syntax — `.|captures|` is always the first thing inside `{ }`:**
```
// Explicit lambda — params declared in (), capture list at body start
stack i32 base = 10;
closure.(stack i32): i32 f = lam.(stack i32 x): i32 { .|base| ret x + base; };
closure.(stack i32): i32 g = lam.(stack i32 x): i32 { .|&base| ret x + base; };  // by-ref

// Trailing closure — first .|...| = captures, second |...| = params (both optional)
reduce(&arr[0], len, 0) { .|&total| |acc, n| total = acc + n };

// Watch — param already declared in (), first .|...| in body = captures
watch.(sig_t s) { .|&count| count = count + 1; }
watch.(sig_t s) { .|x, &y| do_something(s, x, y); }
```

**Rule:** If the body starts with `.|...|`, it is a capture list. If a `|...|` follows immediately after, it is the param list (trailing closure only). No capture list = existing non-capturing behavior.

**Calling a closure:**
```
f(5)   // codegen sees closure type, extracts fn_ptr + env_ptr, calls fn_ptr(5, env_ptr)
```

### AST Changes (`src/ast/ast.h`)

Add capture entry type and capture list to `NodeLambda`:
```c
typedef struct {
    char     *name;
    boolean_t by_ref;   /* True → env holds &name, False → env holds copy */
} capture_entry_t;

/* In lambda_expr struct: */
capture_entry_t *captures;
usize_t          capture_count;
```

Add `TypeClosure` to `type_base_t` enum. Add `closure_params` / `closure_ret` fields to `type_info_t` (mirrors existing fn-pointer fields).

### Parser Changes (`src/parser/parse_expr.c`)

**`parse_lambda_expr()` (line ~178):** After parsing `lam.(params): ret {`, peek at body start. If `.|` seen, parse capture list (comma-separated: bare name = by-value, `&name` = by-ref). Store into `NodeLambda.captures`. The params are already in `()` as before — body `.|...|` is captures only for explicit lambdas.

**`parse_trailing_closure()` (line ~1218):** Currently parses `{ |params| body }`. Extend: after `{`, if `.|` seen, parse first group as captures. If a `|` immediately follows, it = params. If only one group (no `.|`), it remains params (existing non-capturing behavior, backward-compatible).

**`parse_type()` (`parser.c`):** Add `closure.(params): ret` type syntax alongside existing `fn*(params): ret`.

### Codegen Changes (`src/codegen/cg_expr.c`)

**`gen_lambda()` (line 1313):**

When `capture_count > 0`:
1. Build LLVM struct type `{ field0_ty, field1_ty, ... }` for the env (by-value fields are their type; by-ref fields are `ptr`).
2. Allocate env struct at current insertion point (`LLVMBuildAlloca` for stack env; `LLVMBuildMalloc` for heap env — driven by outer variable's storage qualifier, defaulting to stack).
3. Store each captured value (or address) into env struct fields.
4. Append `ptr env_param` as the **last** LLVM parameter of the synthesized function.
5. Inside lambda body: when `gen_ident` encounters a captured name (list was previously blocked), load from env struct instead of erroring.
6. Return a `{ptr fn, ptr env}` struct value (via `LLVMBuildInsertValue` twice into `LLVMGetUndef(closure_llvm_type)`).

**Call sites (`gen_call`, `gen_method_call`):**
- When the callee resolves to a `closure` type: `extractvalue` fn_ptr and env_ptr, add env_ptr as extra last arg to `LLVMBuildCall2`.
- When callee is `fn*` type: unchanged (no env arg).

**`gen_ident()` (line ~621):** Remove the hard error on `lambda_blocked_names` hit when there is a capture list — instead return a load from the env struct field.

**`cg_t` struct (`codegen.c` line ~451):** Add:
```c
LLVMValueRef lambda_env_alloca;  /* current lambda's env alloca (or Null) */
capture_entry_t *lambda_captures;
usize_t          lambda_capture_count;
```

**`cg_types.c`:** Add `get_llvm_closure_type()` — returns `LLVMStructType({ptr, ptr})`.

### Type System (`cg_types.c`, `cg_expr.c`)

- `get_llvm_type()` case for `TypeClosure`: return the `{ptr, ptr}` struct type.
- `build_fn_ptr_llvm_type()` extended for closures (adds env ptr as last param).
- Type inference for trailing closures: when callee param is `TypeClosure`, backfill closure param/ret types.

---

## Feature 2: Watch Captures

### Design

```
stack i32 count = 0;
watch.(sig_t s) { .|count| count = count + 1; }   // by value — copy, outer unaffected
watch.(sig_t s) { .|&count| count = count + 1; }  // by ref — outer count updated
```

Same unified rule: `|...|` as first thing in `{ }` = capture list. The watch param is already declared in `watch.(T name)`, so any `|...|` in the body is captures only — identical parse path as explicit lambda bodies.

### AST Changes (`src/ast/ast.h`)

Extend `NodeWatchStmt`:
```c
struct {
    type_info_t      type;
    char            *param_name;
    node_t          *body;
    capture_entry_t *captures;    /* new */
    usize_t          capture_count; /* new */
} watch_stmt;
```

### Parser Changes (`src/parser/parse_stmt.c`)

**`parse_watch_stmt()` (line 341):** After `watch.(T name)`, call `parse_body()`. In `parse_body()` (or immediately after entering `{`), peek: if `.|` is the first token, parse the capture list using the same helper as `parse_trailing_closure`. Store captures in `watch_stmt.captures`. Continue parsing the rest of the body statements normally. Unified: same parse path as explicit lambda body capture.

### Codegen Changes (`src/codegen/cg_signals.c`)

**Handler ABI change:** Add a 5th parameter `ptr user_data` to every handler function:
```c
void(ptr arg, ptr data_gv, ptr lock_gv, i64 index, ptr user_data)
```

**`ensure_watch_register_fn()`:** Update handler type and registration function to accept and store an additional `user_data` pointer alongside each handler pointer.

**Signal storage (`signal_storage_t`):** Add parallel `user_data` array alongside handler array.

**`gen_watch_stmt()`:**
1. If `capture_count > 0`: build env struct (same as lambda captures), alloca + populate at watch site.
2. Pass env alloca as 5th `reg_args` element.
3. Inside synthesized handler fn: parameter 4 (`user_data`) is cast and used as env_ptr.
4. Captured names are loaded from env fields (by-value) or dereffed (by-ref).

---

## Feature 3: Lambda on RHS of `|>`

**File:** `src/parser/parse_expr.c:2158-2177`

After lambda captures are implemented, a lambda expression produces an LLVM function value. The blocker was that there was no clean "call a lambda" path when the lambda was the RHS.

**Fix:** Remove the `NodeLambda` case error. Instead: when `parse_pipeline` encounters a `NodeLambda` on the RHS, wrap it in a call node. The lambda is gen'd to a fn (or closure), and the LHS value is passed as its first argument.

If it's non-capturing: emit `lam_fn_ptr(lhs_value)`.
If it's capturing (closure): extract fn_ptr, call `fn_ptr(lhs_value, env_ptr)`.

Implementation: synthesize a `NodeCall` wrapping the lambda as callee (similar to how other RHS forms are desugared).

---

## Feature 4: Stream Coroutine Return Values

**File:** `src/analysis/coroutines.c:375-384`

Currently `ret expr;` in a stream coro is rejected. The promise header (`__sts_coro_prom_hdr`) has `complete` and `eos` flags but no slot for a final typed value.

**Design:** Add a `final_val` slot (a `T`-sized payload, same approach as the inline `T` slot already in the promise) to the coro promise. On `ret expr;`, store into `final_val` and set `eos`. On `await.next.(s)` after `stream.done.(s)` is True, allow `stream.final.(s)` to retrieve the value.

**New syntax:**
```
async fn gen(): stream.[i32] {
    yield 1; yield 2;
    ret 99;    // final return value
}
stream.[i32] s = gen();
// ... consume yields ...
if (stream.done.(s)) {
    stack i32 last = stream.final.(s);
}
```

**Changes:**
- `src/analysis/coroutines.c:379`: Remove restriction (allow `ret expr;` in stream coro).
- `src/codegen/cg_coro.c`: In `sts_emit_stream_ret()`, store value into `final_val` field of promise header instead of emitting error.
- `src/codegen/cg_expr.c` or `cg_stmt.c`: Add `stream.final.(s)` op that loads from `final_val` field.
- `src/ast/ast.h`: Extend `FutureOp` enum with `StreamFinal`.
- `src/parser/parse_expr.c`: Parse `stream.final.(expr)`.

---

## Feature 5: Heterogeneous `await.all` / `await.any`

**File:** `src/codegen/cg_expr.c:2553-2586`

Currently all futures in `await.all/any` must share one `T`. True heterogeneous support requires a tuple return type.

**Design:**
```
stack [i32, str] [n, s] = await.all.(compute_int(), compute_str());
```

The return type of `await.all.(f1, f2, ...)` with element types `[T1, T2, ...]` is a multi-assign destructured tuple. Codegen already handles multi-return via `stack T [a, b] = expr;`.

**Changes:**
- Remove homogeneity check at `cg_expr.c:2570-2586`.
- In `gen_await_combinator()`: for `await.all`, allocate per-future typed slots, drive each to completion in order (or round-robin), store results. Return as a struct value (or use the existing multi-assign path).
- For `await.any`: drive all concurrently, return value of first completed (already void since winner is unknown type — possibly return as `void*` tagged with index).
- Parser: `await.all.(f1, f2)` with heterogeneous types maps to multi-assign LHS `stack [T1, T2] [a, b] = ...`.

This is the most complex change. Scope can be limited initially to `await.all` with heterogeneous types (multi-assign return), leaving `await.any` heterogeneous for a follow-up since "winner" semantics are harder.

---

## Feature 6: Spread Expressions in Compound Initializers

**File:** `src/codegen/cg_expr.c:449-454` — `emit_spread_values_const()`

Currently `..expr` in `.{...}` works for: arrays, strings, ranges. Fails for structs and slices.

**Fix:** Identify exactly which spread cases reach the "unsupported" fallthrough by tracing what `emit_spread_values_const` handles. Add cases for:
- Struct spread already handled via `..` spread syntax (`..p` in struct init) — may just need routing fix.
- Runtime-length slice spread in const context: must be split to a runtime-emit path.

Audit `emit_spread_values` (line 457) vs `emit_spread_values_const` (line ~390) to understand which paths exist and plug gaps.

---

## Feature 7: `comptime_assert` Expression Breadth

**File:** `src/codegen/cg_stmt.c:1902`

Currently only: constants, `sizeof`, struct `@comptime` fields, arithmetic.

**Fix:** Run through `LLVMConstFoldInstruction` or a lightweight constant-eval pass on the expression. At minimum, add support for: comparisons (`==`, `!=`, `<`, `>`), boolean logic (`&&`, `||`, `!`), and nested `sizeof` and `comptime` field accesses.

---

## Critical Files

| File | Changes |
|------|---------|
| `src/ast/ast.h` | Add `TypeClosure`, `capture_entry_t`, captures to `NodeLambda` + `watch_stmt` |
| `src/parser/parse_expr.c` | Capture list parse for `lam.[]()`, trailing closure two-`|...|` form, remove `|>` lambda error |
| `src/parser/parse_stmt.c` | Capture list parse in `parse_watch_stmt`, `parse_type` for `closure.()` |
| `src/codegen/cg_expr.c` | `gen_lambda` env struct, `gen_ident` env load, call-site closure dispatch |
| `src/codegen/cg_signals.c` | 5-arg handler ABI, env pass/load in `gen_watch_stmt` |
| `src/codegen/cg_types.c` | `TypeClosure` LLVM type, `get_llvm_closure_type()` |
| `src/codegen/codegen.c` | Add `lambda_env_alloca`, `lambda_captures`, `lambda_capture_count` to `cg_t` |
| `src/analysis/coroutines.c` | Remove stream-return restriction |
| `src/codegen/cg_coro.c` | `sts_emit_stream_ret` stores final value |

---

## Implementation Order

1. `TypeClosure` + `capture_entry_t` in AST (no-op until used)
2. Lambda capture parse (`lam.[x, &y](...)`)
3. Lambda capture codegen (env struct, env load in gen_ident, closure return value)
4. Closure call-site codegen
5. `|>` lambda RHS (remove restriction)
6. Watch capture parse + AST
7. Watch handler ABI update + capture codegen
8. Stream coro `ret expr;`
9. Heterogeneous `await.all`
10. Spread expression gaps
11. `comptime_assert` breadth

---

## Verification

- `stasha test src/tests/test_sugar.sts` — existing lambda/pipeline/trailing tests pass
- New test: `tests/test_captures.sts` — lambda captures by value + by ref
- New test: `tests/signals/captures.sts` — watch with `|&x|` capture modifying outer var
- `stasha test src/tests/test_coro.sts` — stream coro with `ret expr;` final value
- Existing negative test `tests/neg/capture.sts` — **must be updated** (capture is now legal)
