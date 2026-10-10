#!/usr/bin/env bash
# Source/target validator tests only. Does not qualify emitted compiler output.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/stasha-review-kernel.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
"${CC:-cc}" -std=c11 -D_GNU_SOURCE -DTesting -O1 -g -fno-omit-frame-pointer \
  -fsanitize="${SANITIZERS:-undefined}" -fno-sanitize-recover=all -I"$ROOT/src" \
  "$ROOT/tests/driver/review_kernel_contracts.c" \
  "$ROOT/src/common/common.c" "$ROOT/src/lexer/lexer.c" "$ROOT/src/ast/ast.c" \
  "$ROOT/src/parser/parser.c" "$ROOT/src/preprocessor/preprocessor.c" \
  "$ROOT/src/analysis/kernel.c" -o "$BUILD/contracts"
python3 - "$BUILD/contracts" <<'PY'
import subprocess, sys
try:
    p = subprocess.run([sys.argv[1]], timeout=30)
except subprocess.TimeoutExpired:
    sys.exit('FAIL: kernel contract validator timed out')
if p.returncode < 0:
    sys.exit(f'FAIL: kernel contract validator terminated by signal {-p.returncode}')
sys.exit(p.returncode)
PY
