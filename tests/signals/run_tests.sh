#!/usr/bin/env bash
# Integration tests for watch/send/quit signal dispatch.
# Builds and runs each .sts test, compares stdout + exit code.
set -u

STASHA="${STASHA:-./bin/stasha}"
DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="${DIR}/bin"
mkdir -p "${BIN_DIR}"

pass=0
fail=0

red()   { printf '\033[31m%s\033[0m' "$1"; }
green() { printf '\033[32m%s\033[0m' "$1"; }

# run_case NAME SRC EXPECTED_EXIT EXPECTED_STDOUT
run_case() {
    local name="$1" src="$2" want_exit="$3" want_stdout="$4"
    local bin="${BIN_DIR}/${name}"

    if ! "${STASHA}" build "${src}" -o "${bin}" >/dev/null 2>&1; then
        echo "  $(red FAIL) ${name} — compile failed"
        fail=$((fail+1))
        return
    fi

    local got_stdout
    got_stdout="$("${bin}" 2>/dev/null)"
    local got_exit=$?

    if [[ "${got_exit}" -ne "${want_exit}" || "${got_stdout}" != "${want_stdout}" ]]; then
        echo "  $(red FAIL) ${name}"
        echo "       want exit=${want_exit} stdout=$(printf %q "${want_stdout}")"
        echo "       got  exit=${got_exit} stdout=$(printf %q "${got_stdout}")"
        fail=$((fail+1))
    else
        echo "  $(green PASS) ${name}"
        pass=$((pass+1))
    fi
}

echo "== signal tests =="

run_case basic "${DIR}/basic.sts" 0 \
"basic: code=42"

run_case multi_watcher "${DIR}/multi_watcher.sts" 0 \
"A:7
B:7"

# break.sts: watcher X breaks on first send, Y stays.
# send(1) -> X fires (prints X:1, then self-deregs), Y fires (prints Y:1)
# send(2) -> X slot is null, Y fires (prints Y:2)
run_case break "${DIR}/break.sts" 0 \
"X:1
Y:1
Y:2"

# recursive.sts: outer dispatch's snapshot len=1 when send(5) starts.
# Handler registers a new watcher and issues send(0). Inner dispatch snapshot
# also only sees existing watchers at the time it starts. The outer handler
# does d=1 c=5, then sends(0): inner fires same (now 1) outer handler again
# (depth becomes 2) because registration happened AFTER inner's snapshot —
# but actually the new watcher was registered BEFORE the inner send so
# snapshot catches it... Let's spell the expected order.
# Order:
#   outer send(5) begins; snapshot N=1; iter 0: call outer handler
#     outer handler runs: depth=1, print d=1 c=5
#       s.code==5 so register NEW (append to watchers), then send(0)
#       inner send(0) begins; snapshot N=2; iter 0: call outer handler
#         outer handler: depth=2, print d=2 c=0
#         s.code==0 so if-branch skipped.
#       inner iter 1: call NEW -> print NEW:0
#   outer iter ends. Done.
run_case recursive "${DIR}/recursive.sts" 0 \
"d=1 c=5
d=2 c=0
NEW:0"

# nested_loop_break.sts: for inside watch body; break inside for must break
# the for (not the watcher). Second send must still invoke the watcher.
run_case nested_loop_break "${DIR}/nested_loop_break.sts" 0 \
"enter:1
i=0
after
enter:2
i=0
after"

# quit.(0) -> exit(0): @[[exit]] block runs first. Exit code 0.
run_case quit_runs_exit "${DIR}/quit_runs_exit.sts" 0 \
"before_quit
exit_block_ran"

# quit.(0) from main -> exit_block prints, then quit.(1) from inside exit_block
# demotes to _Exit(1) which returns exit code 1 without further atexit chain.
run_case quit_from_exit "${DIR}/quit_from_exit.sts" 1 \
"exit_block_ran"

echo
echo "signal tests: $(green "${pass} passed"), $(red "${fail} failed")"
exit "${fail}"
