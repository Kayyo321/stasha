#!/usr/bin/env bash
# Build the production linker/auditor against the selected pinned LLVM/LLD.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LLVM_BUILD="${LLVM_BUILD:-$ROOT/build/llvm}"
CFG="$LLVM_BUILD/bin/llvm-config"
[[ -x "$CFG" && "$("$CFG" --version)" == 23.0.0git ]] || { echo 'Pinned LLVM23 llvm-config required' >&2; exit 2; }
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/stasha-linker-probe.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
# Function sections let this focused test omit unrelated drivers after linking.
"${CXX:-c++}" -O1 -ffunction-sections -fdata-sections $("$CFG" --cxxflags) \
  -DSTASHA_HAS_LLD -I"$ROOT/src" -I"$ROOT/extlib/llvm/lld/include" \
  "$ROOT/src/linker/linker.cpp" "$ROOT/tests/driver/review_linker_probe.cpp" \
  -Wl,--gc-sections -Wl,--start-group "$LLVM_BUILD"/lib/liblld*.a \
  "$LLVM_BUILD"/lib/libLLVM*.a -Wl,--end-group \
  $("$CFG" --system-libs) -lstdc++ -lzstd -lz -o "$BUILD/probe"
python3 "$ROOT/tests/driver/review_linker_primitives.py" "$BUILD/probe" "$@"
