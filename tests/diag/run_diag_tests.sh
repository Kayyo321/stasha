#!/bin/bash
# Usage: ./tests/diag/run_diag_tests.sh [path/to/stasha] [harness options]
# Positive warning cases require exit 0 and fresh output. Negative cases
# require exit 1 plus diagnostics; crashes/timeouts/verifier failures fail.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$DIR/../harness/compiler_suites.py" diag "$@"
