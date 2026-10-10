#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/stasha-layout-oracle.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
CC="${CC:-cc}"
LLVM_CONFIG="${LLVM_CONFIG:-$ROOT/build/llvm/bin/llvm-config}"
# Deliberate, visibly labelled secondary oracle path. This never builds or
# qualifies Stasha with a replacement LLVM and is not the pinned-toolchain gate.
if [[ "${1:-}" == --secondary-llvm19 ]]; then
    echo 'SECONDARY ORACLE ONLY: LLVM 19 runtime; not pinned LLVM 23 compiler qualification'
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
      -I"$ROOT/src" -I"$ROOT/extlib/llvm/llvm/include" \
      -I"${LLVM_BUILD_INCLUDE:?set LLVM_BUILD_INCLUDE to pinned generated headers}" \
      "$ROOT/tests/layout/layout_oracle.c" -lLLVM-19 -o "$BUILD/oracle"
else
    [[ -x "$LLVM_CONFIG" ]] || { echo "Missing pinned llvm-config: $LLVM_CONFIG" >&2; exit 2; }
    version="$("$LLVM_CONFIG" --version)"
    [[ "$version" == 23.0.0git ]] || { echo "Expected pinned 23.0.0git, got $version" >&2; exit 2; }
    echo "LLVM $version (revision identity is checked by the project toolchain fingerprint)"
    "$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/src" \
      $("$LLVM_CONFIG" --cflags) "$ROOT/tests/layout/layout_oracle.c" \
      $("$LLVM_CONFIG" --ldflags --libs core native --system-libs) -lstdc++ -o "$BUILD/oracle"
fi
"$BUILD/oracle"
