#!/bin/sh
# Stasha installer — macOS and Linux
# Usage: curl -fsSL https://raw.githubusercontent.com/Kayyo321/stasha/main/install.sh | sh
#        curl -fsSL ... | sh -s -- --from-source

set -e

REPO="Kayyo321/stasha"
INSTALL_DIR="${STASHA_INSTALL_DIR:-$HOME/.stasha}"

FROM_SOURCE=0
for arg in "$@"; do
  case "$arg" in
    --from-source) FROM_SOURCE=1 ;;
  esac
done

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
    echo "Unsupported architecture: $ARCH" >&2
    if [ "$FROM_SOURCE" = "0" ]; then
      printf "Build from source instead? Requires: git, cmake, ninja, cc, c++ [y/N] "
      read -r ans
      case "$ans" in
        [yY]*) FROM_SOURCE=1 ;;
        *) exit 1 ;;
      esac
    fi
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

# ── source-build helper ────────────────────────────────────────
build_from_source() {
  echo "Building Stasha ${TAG} from source..."

  for tool in git cmake ninja cc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      echo "error: '$tool' is required for a source build but not found" >&2
      echo "       Install: git cmake ninja and a C/C++ toolchain (cc/c++)" >&2
      exit 1
    fi
  done

  BUILD_TMP="$(mktemp -d)"
  # shellcheck disable=SC2064
  trap "rm -rf '$BUILD_TMP'" EXIT

  echo "Cloning ${REPO}@${TAG}..."
  git clone --depth 1 --branch "$TAG" --recurse-submodules \
    "https://github.com/${REPO}.git" "$BUILD_TMP/stasha"

  cd "$BUILD_TMP/stasha"
  make llvm
  make openssl
  make
  make stdlib
  make install PREFIX="$INSTALL_DIR"
  cd -

  rm -rf "$BUILD_TMP"
}

if [ "$FROM_SOURCE" = "1" ]; then
  build_from_source
else
  URL="https://github.com/${REPO}/releases/download/${TAG}/${ARCHIVE}"
  SHA_URL="${URL}.sha256"
  echo "Installing Stasha ${TAG} (${OS_NAME}/${ARCH_NAME})..."

  # ── download ────────────────────────────────────────────────
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT

  if command -v curl >/dev/null 2>&1; then
    HTTP_STATUS="$(curl -sSL --progress-bar -o "$TMP/$ARCHIVE" -w '%{http_code}' "$URL")"
  else
    HTTP_STATUS="$(wget -q --show-progress -O "$TMP/$ARCHIVE" --server-response "$URL" 2>&1 \
                   | awk '/HTTP\//{last=$2} END{print last}')"
  fi

  if [ "$HTTP_STATUS" = "404" ]; then
    echo ""
    echo "No prebuilt archive found for ${OS_NAME}/${ARCH_NAME} (tag ${TAG})."
    printf "Build from source instead? Requires: git, cmake, ninja, cc [y/N] "
    read -r ans
    case "$ans" in
      [yY]*) build_from_source ;;
      *) echo "Aborting."; exit 1 ;;
    esac
  elif [ "$HTTP_STATUS" != "200" ]; then
    echo "error: download failed (HTTP ${HTTP_STATUS}): ${URL}" >&2
    exit 1
  fi

  # ── SHA-256 verify ──────────────────────────────────────────
  if command -v curl >/dev/null 2>&1; then
    SHA_REMOTE="$(curl -fsSL "$SHA_URL" 2>/dev/null | awk '{print $1}')" || SHA_REMOTE=""
  else
    SHA_REMOTE="$(wget -qO- "$SHA_URL" 2>/dev/null | awk '{print $1}')" || SHA_REMOTE=""
  fi

  if [ -n "$SHA_REMOTE" ]; then
    if command -v sha256sum >/dev/null 2>&1; then
      SHA_LOCAL="$(sha256sum "$TMP/$ARCHIVE" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
      SHA_LOCAL="$(shasum -a 256 "$TMP/$ARCHIVE" | awk '{print $1}')"
    else
      SHA_LOCAL=""
    fi

    if [ -n "$SHA_LOCAL" ]; then
      if [ "$SHA_LOCAL" != "$SHA_REMOTE" ]; then
        echo "error: SHA-256 mismatch — archive may be corrupt or tampered" >&2
        echo "  expected: $SHA_REMOTE" >&2
        echo "  got:      $SHA_LOCAL" >&2
        exit 1
      fi
      echo "  SHA-256 verified."
    fi
  fi

  # ── extract ─────────────────────────────────────────────────
  mkdir -p "$INSTALL_DIR"
  tar -xzf "$TMP/$ARCHIVE" --strip-components=1 -C "$INSTALL_DIR"
  chmod +x "$INSTALL_DIR/bin/stasha"

  # ── smoke test ──────────────────────────────────────────────
  if ! "$INSTALL_DIR/bin/stasha" --version >/dev/null 2>&1; then
    echo "error: smoke test failed — stasha binary did not run cleanly" >&2
    echo "       Try: $INSTALL_DIR/bin/stasha --version" >&2
    exit 1
  fi
  echo "  smoke test OK: $("$INSTALL_DIR/bin/stasha" --version)"
fi

# ── update PATH ────────────────────────────────────────────────
BIN_DIR="$INSTALL_DIR/bin"
EXPORT_LINE="export PATH=\"\$HOME/.stasha/bin:\$PATH\""

if [ "$INSTALL_DIR" != "$HOME/.stasha" ]; then
  EXPORT_LINE="export PATH=\"${BIN_DIR}:\$PATH\""
fi

path_already_set() {
  case ":$PATH:" in
    *":$BIN_DIR:"*) return 0 ;;
  esac
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
