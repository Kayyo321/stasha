#!/bin/sh
# Stasha uninstaller — macOS and Linux

set -e

INSTALL_DIR="${STASHA_INSTALL_DIR:-$HOME/.stasha}"
BIN_DIR="$INSTALL_DIR/bin"

if [ ! -d "$INSTALL_DIR" ]; then
  echo "Stasha does not appear to be installed at $INSTALL_DIR"
  exit 0
fi

echo "Removing $INSTALL_DIR..."
rm -rf "$INSTALL_DIR"

# Strip PATH export lines from shell rc files
strip_from_file() {
  RC="$1"
  if [ ! -f "$RC" ]; then return; fi
  if grep -qF "$BIN_DIR" "$RC" 2>/dev/null; then
    # Remove the comment line and the export line
    TMP="$(mktemp)"
    grep -v "# Stasha" "$RC" | grep -vF "$BIN_DIR" > "$TMP"
    # Also remove trailing blank lines left behind (best-effort)
    mv "$TMP" "$RC"
    echo "  removed PATH entry from $RC"
  fi
}

strip_from_file "$HOME/.zshrc"
strip_from_file "$HOME/.bashrc"
strip_from_file "$HOME/.bash_profile"
strip_from_file "$HOME/.profile"

FISH_CFG="$HOME/.config/fish/config.fish"
if [ -f "$FISH_CFG" ] && grep -qF "$BIN_DIR" "$FISH_CFG" 2>/dev/null; then
  TMP="$(mktemp)"
  grep -v "# Stasha" "$FISH_CFG" | grep -vF "$BIN_DIR" > "$TMP"
  mv "$TMP" "$FISH_CFG"
  echo "  removed PATH entry from $FISH_CFG"
fi

echo ""
echo "Stasha has been uninstalled."
echo "Restart your terminal for the PATH change to take effect."
