#!/bin/zsh

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
EXT_DIR="$SCRIPT_DIR/tools/vscode/stasha-language"
USER_DATA_DIR="${TMPDIR:-/tmp}/stasha-vscode-user"
EXTENSIONS_DIR="${TMPDIR:-/tmp}/stasha-vscode-exts"

mkdir -p "$USER_DATA_DIR" "$EXTENSIONS_DIR"

open -n -a "Visual Studio Code" --args \
  --new-window \
  --user-data-dir "$USER_DATA_DIR" \
  --extensions-dir "$EXTENSIONS_DIR" \
  --extensionDevelopmentPath "$EXT_DIR" \
  "$SCRIPT_DIR"
