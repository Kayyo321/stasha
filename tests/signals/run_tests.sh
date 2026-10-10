#!/usr/bin/env bash
# Fresh-output, timeout-bounded watch/send/quit tests with exact stdout/exit.
# Retains STASHA environment variable compatibility with the original runner.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$DIR/../harness/compiler_suites.py" signals "${STASHA:-./bin/stasha}" "$@"
