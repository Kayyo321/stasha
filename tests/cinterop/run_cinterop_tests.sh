#!/bin/bash
# Usage: ./tests/cinterop/run_cinterop_tests.sh [path/to/stasha] [harness options]
# Every output lives in a fresh per-case directory; compilation must exit 0.
# --filter '^llvm_smoke$' --require-prerequisites runs only the LLVM capstone.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$DIR/../harness/compiler_suites.py" cinterop "$@"
