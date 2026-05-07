#!/bin/sh
# Stasha installer — macOS and Linux
# Usage: curl -fsSL https://raw.githubusercontent.com/Kayyo321/stasha/main/install.sh | sh

set -e

REPO="Kayyo321/stasha"
INSTALL_DIR="${STASHA_INSTALL_DIR:-$HOME/.stasha}"

# ── detect platform ────────────────────────────────────────────
OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
  Darwin) OS_NAME="macos" ;;
  Linux)  OS_NAME="linux" ;;
  *)
    echo "error: unsupported OS: $OS" >&2
    exit 1
    ;;
esac

case "$ARCH" in
  arm64|aarch64) ARCH_NAME="arm64" ;;
  x86_64|amd64)  ARCH_NAME="x86_64" ;;
  *)
    echo "error: unsupported architecture: $ARCH" >&2
    exit 1
    ;;
esac

ARCHIVE="stasha-${OS_NAME}-${ARCH_NAME}.tar.gz"

# ── find latest release ────────────────────────────────────────
echo "Fetching latest Stasha release..."

if command -v curl >/dev/null 2>&1; then
  TAG="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
        | grep '"tag_name"' | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/')"
elif command -v wget >/dev/null 2>&1; then
  TAG="$(wget -qO- "https://api.github.com/repos/${REPO}/releases/latest" \
        | grep '"tag_name"' | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/')"
else
  echo "error: curl or wget is required to install Stasha" >&2
  exit 1
fi

if [ -z "$TAG" ]; then
  echo "error: could not determine latest release tag" >&2
  exit 1
fi

URL="https://github.com/${REPO}/releases/download/${TAG}/${ARCHIVE}"
echo "Installing Stasha ${TAG} (${OS_NAME}/${ARCH_NAME})..."

# ── download and extract ───────────────────────────────────────
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if command -v curl >/dev/null 2>&1; then
  curl -fsSL --progress-bar "$URL" -o "$TMP/$ARCHIVE"
else
  wget -q --show-progress "$URL" -O "$TMP/$ARCHIVE"
fi

mkdir -p "$INSTALL_DIR"
tar -xzf "$TMP/$ARCHIVE" --strip-components=1 -C "$INSTALL_DIR"
chmod +x "$INSTALL_DIR/bin/stasha"

# ── update PATH ────────────────────────────────────────────────
BIN_DIR="$INSTALL_DIR/bin"
EXPORT_LINE="export PATH=\"\$HOME/.stasha/bin:\$PATH\""

# Use the absolute path variant if INSTALL_DIR is not the default
if [ "$INSTALL_DIR" != "$HOME/.stasha" ]; then
  EXPORT_LINE="export PATH=\"${BIN_DIR}:\$PATH\""
fi

path_already_set() {
  case ":$PATH:" in
    *":$BIN_DIR:"*) return 0 ;;
  esac
  # also check if the rc file already has the line
  [ -f "$1" ] && grep -qF "$BIN_DIR" "$1" 2>/dev/null
}

add_to_file() {
  RC="$1"
  if ! grep -qF "$BIN_DIR" "$RC" 2>/dev/null; then
    printf '\n# Stasha\n%s\n' "$EXPORT_LINE" >> "$RC"
    echo "  added to $RC"
  fi
}

SHELL_NAME="$(basename "${SHELL:-sh}")"
RC_UPDATED=""

case "$SHELL_NAME" in
  zsh)
    add_to_file "$HOME/.zshrc"
    RC_UPDATED="$HOME/.zshrc"
    ;;
  bash)
    if [ "$(uname -s)" = "Darwin" ]; then
      add_to_file "$HOME/.bash_profile"
      RC_UPDATED="$HOME/.bash_profile"
    else
      add_to_file "$HOME/.bashrc"
      RC_UPDATED="$HOME/.bashrc"
    fi
    ;;
  fish)
    FISH_CFG="$HOME/.config/fish/config.fish"
    mkdir -p "$(dirname "$FISH_CFG")"
    if ! grep -qF "$BIN_DIR" "$FISH_CFG" 2>/dev/null; then
      printf '\n# Stasha\nfish_add_path "%s"\n' "$BIN_DIR" >> "$FISH_CFG"
      echo "  added to $FISH_CFG"
      RC_UPDATED="$FISH_CFG"
    fi
    ;;
  *)
    add_to_file "$HOME/.profile"
    RC_UPDATED="$HOME/.profile"
    ;;
esac

# ── done ───────────────────────────────────────────────────────
echo ""
echo "Stasha ${TAG} installed to ${INSTALL_DIR}"
echo ""

if [ -n "$RC_UPDATED" ]; then
  echo "To start using stasha, restart your terminal or run:"
  echo "  source ${RC_UPDATED}"
else
  echo "stasha is already on your PATH."
fi

echo ""
echo "Verify installation:"
echo "  stasha --version"
