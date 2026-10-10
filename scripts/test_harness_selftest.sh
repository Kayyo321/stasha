#!/bin/bash
# These tests require Python 3 and Bash, not LLVM or a built Stasha compiler.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "${PYTHON:-python3}" "$ROOT/tests/harness/test_oracles.py" "$@"
