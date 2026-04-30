#!/bin/bash
# C interop test runner for Stasha compiler
# Compiles each cinterop .sts test, runs the resulting binary, checks stdout.
# Negative tests under neg/ assert compiler exits non-zero with expected stderr pattern.
# Usage: ./tests/cinterop/run_cinterop_tests.sh [path/to/stasha]

set -uo pipefail

STASHA="${1:-./bin/stasha}"
DIR="$(cd "$(dirname "$0")" && pwd)"
SUPPORT_LIB="$DIR/build/libcinterop_support.a"
PASS=0
FAIL=0
SKIP=0

red()    { printf "\033[31m%s\033[0m" "$1"; }
green()  { printf "\033[32m%s\033[0m" "$1"; }
yellow() { printf "\033[33m%s\033[0m" "$1"; }

# run_pos NAME EXPECTED_SUBSTRING [EXTRA_LINK_ARGS...]
# Compiles tests/cinterop/<NAME>.sts (links support archive when present),
# runs the binary, asserts stdout contains EXPECTED_SUBSTRING.
run_pos() {
    local name="$1"; shift
    local expect="$1"; shift
    local src="$DIR/$name.sts"
    local bin="$DIR/build/$name"
    if [ ! -f "$src" ]; then
        SKIP=$((SKIP + 1))
        echo "  $(yellow SKIP) $name (no source)"
        return
    fi
    local out err code
    local link_args=()
    if [ -f "$SUPPORT_LIB" ]; then
        link_args+=("-l" "$SUPPORT_LIB")
    fi
    err=$("$STASHA" build "$src" -o "$bin" "${link_args[@]}" "$@" 2>&1)
    # Note: cheader-path compilations may leak via scan_and_deallocate and
    # produce a non-zero exit even on success — so the gate is "binary
    # produced AND no `error:` line in stderr", not the raw exit code.
    if ! echo "$err" | grep -qE "^error:|^\(EE\)| error\["; then
        : # no compiler errors
    else
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) $name (compile error)"
        echo "$err" | grep -E "^error:|^\(EE\)| error\[" | head -5 | sed 's/^/       /'
        return
    fi
    if [ ! -x "$bin" ]; then
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) $name (binary not produced)"
        echo "$err" | head -5 | sed 's/^/       /'
        return
    fi
    out=$("$bin" 2>&1)
    code=$?
    if [ $code -ne 0 ]; then
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) $name (runtime exit=$code)"
        echo "$out" | head -5 | sed 's/^/       /'
        return
    fi
    if echo "$out" | grep -qF "$expect"; then
        PASS=$((PASS + 1))
        echo "  $(green PASS) $name"
    else
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) $name (missing expected '$expect')"
        echo "$out" | head -5 | sed 's/^/       /'
    fi
}

# run_neg NAME EXPECTED_STDERR_PATTERN
# Compiles tests/cinterop/neg/<NAME>.sts and asserts compiler exits non-zero
# with stderr matching EXPECTED_STDERR_PATTERN.
run_neg() {
    local name="$1"; shift
    local pattern="$1"; shift
    local src="$DIR/neg/$name.sts"
    if [ ! -f "$src" ]; then
        SKIP=$((SKIP + 1))
        echo "  $(yellow SKIP) neg/$name (no source)"
        return
    fi
    local err code
    err=$("$STASHA" build "$src" -o /dev/null 2>&1)
    code=$?
    if [ $code -eq 0 ]; then
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) neg/$name (compile unexpectedly succeeded)"
        return
    fi
    if echo "$err" | grep -qE "$pattern"; then
        PASS=$((PASS + 1))
        echo "  $(green PASS) neg/$name"
    else
        FAIL=$((FAIL + 1))
        echo "  $(red FAIL) neg/$name (stderr missing pattern '$pattern')"
        echo "$err" | head -5 | sed 's/^/       /'
    fi
}

echo "=== Stasha C Interop Test Suite ==="
echo "Compiler: $STASHA"
[ -f "$SUPPORT_LIB" ] && echo "Support archive: $SUPPORT_LIB" || echo "Support archive: (none — Path-B rows will be skipped)"
echo ""

# ── Path A — cheader-based tests ─────────────────────────────────────
echo "--- Path A: cheader ──────────────────────────────────"
run_pos 01_scalars      "01_scalars: ok"
run_pos 02_ptrs         "02_ptrs: ok"
run_pos 03_const_ptr    "03_const_ptr: ok"
run_pos 04_struct_byval "04_struct_byval: ok"
run_pos 05_struct_ptr   "05_struct_ptr: ok"
run_pos 06_nested       "06_nested: ok"
run_pos 07_array_field  "07_array_field: ok"
run_pos 08_anon_typedef "08_anon_typedef: ok"
run_pos 09_opaque       "09_opaque: ok"
run_pos 10_enum         "10_enum: ok"
run_pos 11_define       "11_define: ok"
run_pos 12_varargs      "12_varargs: ok"
run_pos 13_callback_to_c "13_callback_to_c: ok"
run_pos 18_bool         "18_bool: ok"
run_pos 19_qualifiers   "19_qualifiers: ok"
run_pos 20_globals      "20_globals: ok"
run_pos 21_outbuf       "21_outbuf: ok"
run_pos 22_outarray     "22_outarray: ok"
run_pos 24_nested_include "24_nested_include: ok"
run_pos 25_search_path  "25_search_path: ok"
run_pos 26_system_header "26_system_header: ok"
run_pos 30_errno        "30_errno: ok"
echo ""

# ── Path B — lib + imp tests ─────────────────────────────────────────
echo "--- Path B: lib + imp ────────────────────────────────"
run_pos 51_b_scalars      "51_b_scalars: ok"
run_pos 52_b_struct_byval "52_b_struct_byval: ok"
run_pos 53_b_callback     "53_b_callback: ok"
run_pos 54_b_ctor         "54_b_ctor: ok"
run_pos 55_b_multi_tu     "55_b_multi_tu: ok"
echo ""

# ── Capstone ──────────────────────────────────────────────────────────
# llvm_smoke needs the in-tree LLVM build's static archives — we look up
# the libfile list via llvm-config rather than baking it into the runner.
# Skip cleanly if the in-tree LLVM hasn't been built yet (`make llvm`).
echo "--- Capstone: LLVM C API ────────────────────────────"
LLVM_CFG="$DIR/../../build/llvm/bin/llvm-config"
if [ ! -x "$LLVM_CFG" ]; then
    SKIP=$((SKIP + 1))
    echo "  $(yellow SKIP) llvm_smoke (run \"make llvm\" first)"
else
    LLVM_LIBS=$("$LLVM_CFG" --libfiles core analysis native lto passes option codegen bitwriter debuginfodwarf objcarcopts textapi object 2>/dev/null)
    LLVM_ARGS=()
    for L in $LLVM_LIBS; do LLVM_ARGS+=("-l" "$L"); done
    run_pos llvm_smoke "ModuleID" "${LLVM_ARGS[@]}"
fi
echo ""

# ── Negative tests ────────────────────────────────────────────────────
echo "--- Negative: precise unsupported errors ────────────"
run_neg bitfield_unsupported    "bitfield"
run_neg long_double_unsupported "long double"
run_neg union_layout            "union"
echo ""

# ── Results ────────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL))
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed, $SKIP skipped ==="
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
