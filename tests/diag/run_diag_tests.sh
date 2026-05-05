#!/bin/bash
# Diagnostic test runner for Stasha compiler
# Compiles each .sts test, captures stderr, checks expected patterns
# Usage: ./tests/diag/run_diag_tests.sh [path/to/stasha]

set -euo pipefail

STASHA="${1:-./bin/stasha}"
DIR="$(cd "$(dirname "$0")" && pwd)"
PASS=0
FAIL=0
SKIP=0

red()   { printf "\033[31m%s\033[0m" "$1"; }
green() { printf "\033[32m%s\033[0m" "$1"; }
yellow(){ printf "\033[33m%s\033[0m" "$1"; }

# expect_in STDERR PATTERN DESCRIPTION
expect_in() {
    if echo "$1" | grep -qE "$2f"; then
        PASS=$((PASS + 1))
        echo "  $(green PASS) $3"
    else
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) $3"
        echo "       expected pattern: $2"
    fi
}

# expect_not_in STDERR PATTERN DESCRIPTION
expect_not_in() {
    if echo "$1" | grep -qE "$2"; then
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) $3"
        echo "       unexpected pattern found: $2"
    else
        PASS=$((PASS + 1))
        echo "  $(green PASS) $3"
    fi
}

# expect_count STDERR PATTERN EXPECTED_COUNT DESCRIPTION
expect_count() {
    local count
    count=$(echo "$1" | grep -cE "$2" || true)
    if [ "$count" -eq "$3" ]; then
        PASS=$((PASS + 1))
        echo "  $(green PASS) $4 (count=$count)"
    else
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) $4 (expected $3, got $count)"
    fi
}

# compile FILE [EXTRA_FLAGS...] -> sets $STDERR, $EXIT_CODE
compile() {
    local file="$1"; shift
    STDERR=$("$STASHA" build "$file" -o /dev/null "$@" 2>&1 || true)
    EXIT_CODE=$?
}

echo "=== Stasha Diagnostic Test Suite ==="
echo "Compiler: $STASHA"
echo ""

# ── Deduplication tests ──────────────────────────────────────────────
echo "--- Deduplication ---"

echo "dedup_undefined_var.sts"
compile "$DIR/dedup_undefined_var.sts"
expect_count "$STDERR" "undefined variable" 1 "only 1 undefined-variable error for 'missing' used 3x"

echo "dedup_undefined_fn.sts"
compile "$DIR/dedup_undefined_fn.sts"
expect_count "$STDERR" "undefined function" 1 "only 1 undefined-function error for 'calculate' called 3x"

echo "dedup_undefined_type.sts"
compile "$DIR/dedup_undefined_type.sts"
expect_count "$STDERR" "unknown type" 1 "only 1 unknown-type error for 'Widget' used 2x"
expect_in "$STDERR" "did you mean" "Levenshtein suggestion for Widget -> Widgt"

echo "dedup_undefined_member.sts"
compile "$DIR/dedup_undefined_member.sts"
expect_count "$STDERR" "cannot resolve member" 1 "only 1 member error for 'valeu' accessed 2x"
expect_in "$STDERR" "did you mean" "Levenshtein suggestion for valeu -> value"

echo ""

# ── Early abort ──────────────────────────────────────────────────────
echo "--- Early Abort ---"

echo "early_abort_type_error.sts"
compile "$DIR/early_abort_type_error.sts"
expect_in "$STDERR" "error" "pass 0 error reported for recursive struct"
# Should NOT produce cascade errors about Bad in function body
expect_not_in "$STDERR" "undefined variable 'x'" "no cascade errors in later passes"

echo ""

# ── Parser recovery ──────────────────────────────────────────────────
echo "--- Parser Recovery ---"

echo "parser_recovery_multi_error.sts"
compile "$DIR/parser_recovery_multi_error.sts"
expect_in "$STDERR" "error" "at least one syntax error reported"
# Ideally reports errors in both broken1 and broken2

echo ""

# ── Error categories ─────────────────────────────────────────────────
echo "--- Error Categories ---"

echo "category_undefined.sts"
compile "$DIR/category_undefined.sts"
expect_in "$STDERR" "error\[undefined\]" "error[undefined] category tag present"

echo "category_storage_domain.sts"
compile "$DIR/category_storage_domain.sts"
expect_in "$STDERR" "error\[storage\]" "error[storage] category tag present"

echo ""

# ── Levenshtein suggestions ──────────────────────────────────────────
echo "--- Levenshtein Suggestions ---"

echo "levenshtein_variable.sts"
compile "$DIR/levenshtein_variable.sts"
expect_in "$STDERR" "did you mean" "suggestion offered for mistyped variable"

echo "levenshtein_type.sts"
compile "$DIR/levenshtein_type.sts"
expect_in "$STDERR" "did you mean" "suggestion offered for mistyped type"

echo ""

# ── Warnings (--wall) ───────────────────────────────────────────────
echo "--- Warnings (--wall) ---"

echo "warn_unreachable_code.sts (--wall)"
compile "$DIR/warn_unreachable_code.sts" --wall
expect_in "$STDERR" "unreachable" "unreachable code warning emitted"

echo "warn_shadow_variable.sts (--wall)"
compile "$DIR/warn_shadow_variable.sts" --wall
expect_in "$STDERR" "shadow" "shadow variable warning emitted"

echo "warn_unused_with_wall.sts (--wall)"
compile "$DIR/warn_unused_with_wall.sts" --wall
expect_in "$STDERR" "unused" "unused variable warning emitted with --wall"

echo "warn_unused_with_wall.sts (no flags)"
compile "$DIR/warn_unused_with_wall.sts"
expect_in "$STDERR" "unused" "unused variable warning emitted by default"

echo ""

# ── Strict mode ──────────────────────────────────────────────────────
echo "--- Strict Mode ---"

echo "strict_promotes_warning.sts (--strict)"
compile "$DIR/strict_promotes_warning.sts" --strict
expect_in "$STDERR" "error" "warning promoted to error with --strict"
expect_not_in "$STDERR" "warning.*unused" "no warning-level unused message (promoted)"

echo "strict_promotes_warning.sts (no --strict)"
compile "$DIR/strict_promotes_warning.sts"
expect_in "$STDERR" "warning" "warning stays warning without --strict"

echo ""

# ── Coroutine migration diagnostics ───────────────────────────────────
echo "--- Coroutine Diagnostics ---"

echo "coro_await_outside_async.sts"
compile "$DIR/coro_await_outside_async.sts"
expect_in "$STDERR" "only legal inside 'async fn'" "await outside async is rejected"

echo "coro_yield_outside_async.sts"
compile "$DIR/coro_yield_outside_async.sts"
expect_in "$STDERR" "'yield' is only legal inside 'async fn'" "yield outside async is rejected"

echo "coro_mixed_yield_types.sts"
compile "$DIR/coro_mixed_yield_types.sts"
expect_in "$STDERR" "must share one type" "mixed stream yield types are rejected"

echo "coro_stream_nonvoid_ret.sts"
compile "$DIR/coro_stream_nonvoid_ret.sts"
expect_in "$STDERR" "cannot return a final value" "stream coroutines reject non-void ret"

echo "coro_async_dispatch_stream.sts"
compile "$DIR/coro_async_dispatch_stream.sts"
expect_in "$STDERR" "cannot dispatch yielding async function" "async.() rejects stream coroutines"

echo "coro_await_next_requires_stream.sts"
compile "$DIR/coro_await_next_requires_stream.sts"
expect_in "$STDERR" "expects a stream" "await.next requires a stream handle"

echo ""

# ── Structured summary ──────────────────────────────────────────────
echo "--- Structured Summary ---"

echo "dedup_undefined_var.sts (summary format)"
compile "$DIR/dedup_undefined_var.sts"
expect_in "$STDERR" "aborting due to" "structured summary present"

echo ""

# ── Results ──────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL))
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
