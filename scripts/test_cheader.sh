#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/stasha-cheader.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
CC="${CC:-cc}"
SANITIZERS="${SANITIZERS:-undefined}"
flags=(-std=c11 -D_GNU_SOURCE -DTesting -g -O1 -fno-omit-frame-pointer)
if [[ "$SANITIZERS" != none ]]; then
    flags+=("-fsanitize=$SANITIZERS" -fno-sanitize-recover=all)
fi
"$CC" "${flags[@]}" -I"$ROOT/src" "$ROOT/tests/cinterop/cheader_unit.c" \
    "$ROOT/src/common/common.c" -o "$BUILD/cheader-unit"
python3 - "$BUILD/cheader-unit" "$ROOT/tests/cinterop/11_define.sts" <<'PY'
import subprocess
import sys
try:
    result = subprocess.run(sys.argv[1:], timeout=30)
except subprocess.TimeoutExpired:
    sys.exit("FAIL: C header parser regression process timed out after 30 seconds")
if result.returncode < 0:
    sys.exit(f"FAIL: C header parser regression process terminated by signal {-result.returncode}")
sys.exit(result.returncode)
PY
