#!/usr/bin/env bash
# Small deterministic syntax robustness corpus, separate from semantic tests.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/stasha-frontend-fuzz.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
CC="${CC:-cc}"
SANITIZERS="${SANITIZERS:-undefined}"
flags=(-std=c11 -D_GNU_SOURCE -g -O1 -fno-omit-frame-pointer)
if [[ "$SANITIZERS" != none ]]; then
    flags+=("-fsanitize=$SANITIZERS" -fno-sanitize-recover=all)
fi
"$CC" "${flags[@]}" -I"$ROOT/src" \
    "$ROOT/tests/frontend/frontend_probe.c" \
    "$ROOT/src/common/common.c" "$ROOT/src/lexer/lexer.c" \
    "$ROOT/src/ast/ast.c" "$ROOT/src/parser/parser.c" \
    "$ROOT/src/preprocessor/preprocessor.c" -o "$BUILD/frontend-probe"
python3 "$ROOT/tests/frontend/fuzz_frontend.py" "$BUILD/frontend-probe"
