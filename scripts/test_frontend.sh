#!/usr/bin/env bash
# Hermetic front-end regression build. Requires only a C compiler and Python 3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/stasha-frontend.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
CC="${CC:-cc}"
SANITIZERS="${SANITIZERS:-undefined}"
flags=(-std=c11 -D_GNU_SOURCE -DTesting -g -O1 -fno-omit-frame-pointer)
if [[ "$SANITIZERS" != none ]]; then
    flags+=("-fsanitize=$SANITIZERS" -fno-sanitize-recover=all)
fi
"$CC" "${flags[@]}" -I"$ROOT/src" \
    "$ROOT/tests/frontend/frontend_tests.c" \
    "$ROOT/src/common/common.c" "$ROOT/src/lexer/lexer.c" \
    "$ROOT/src/ast/ast.c" "$ROOT/src/parser/parser.c" \
    "$ROOT/src/preprocessor/preprocessor.c" -o "$BUILD/frontend-tests"
# Keep crashes and hangs distinct from rejected language inputs. Never reuse an
# old executable, suppress a compiler failure, or retry a sanitizer failure.
python3 - "$BUILD/frontend-tests" <<'PY'
import subprocess
import sys
try:
    result = subprocess.run([sys.argv[1]], timeout=30)
except subprocess.TimeoutExpired:
    sys.exit("FAIL: frontend regression process timed out after 30 seconds")
if result.returncode < 0:
    sys.exit(f"FAIL: frontend regression process terminated by signal {-result.returncode}")
sys.exit(result.returncode)
PY
