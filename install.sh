#!/bin/sh
# install.sh - installer for the Duck language compiler (duckc).
#
# Usage:
#   sh install.sh                build and install compiler + editor extension
#   sh install.sh --prefix DIR   install into DIR/bin
#   sh install.sh --user         install into $HOME/.local (no root needed)
#   sh install.sh --no-editor    skip the VS Code/VSCodium/Cursor extension
#   sh install.sh --uninstall    remove the compiler and the editor extension
#   sh install.sh --help         show this help
#
# One-liner (clones the repository, builds and installs):
#   curl -fsSL https://raw.githubusercontent.com/didacg/duck-lang/main/install.sh | sh
set -eu

REPO_URL="https://github.com/didacg/duck-lang.git"
BIN_NAME="duckc"
DEFAULT_PREFIX="/usr/local"
USER_PREFIX="${HOME}/.local"

usage() {
    cat <<'EOF'
install.sh - installs the Duck language compiler (duckc)

Options:
  --prefix DIR   install into DIR/bin (default: /usr/local)
  --user         install into $HOME/.local (no root required)
  --no-editor    do not install the VS Code/VSCodium/Cursor extension
  --uninstall    remove the compiler and the editor extension
  -h, --help     show this help

Along with the compiler, the script installs the Duck editor extension
(syntax highlighting for .duck files) into every VS Code-compatible editor
found in your home directory. Use --no-editor to skip it. It also installs
update.sh as 'duck-update', the command used to update Duck later.

Without a checkout of the repository, the script clones it into a temporary
directory, builds it and installs the resulting binary.
EOF
}

PREFIX=""
UNINSTALL=0
EDITOR_EXT=1

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ $# -ge 2 ] || { echo "install.sh: error: --prefix needs an argument" >&2; exit 1; }
            PREFIX="$2"
            shift 2
            ;;
        --user)
            PREFIX="$USER_PREFIX"
            shift
            ;;
        --no-editor)
            EDITOR_EXT=0
            shift
            ;;
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "install.sh: error: unknown option '$1'" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------- uninstall
if [ "$UNINSTALL" -eq 1 ]; then
    # When run through sudo, target the invoking user's editor directories.
    EXT_HOME="$HOME"
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
        su_home="$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6)"
        if [ -n "$su_home" ]; then EXT_HOME="$su_home"; fi
    fi

    removed=0
    remove_file() {
        if [ -e "$1" ]; then
            rm -f "$1"
            echo "removed $1"
            removed=1
        fi
    }
    remove_tree() {
        if [ -d "$1" ]; then
            rm -rf "$1"
            echo "removed $1"
            removed=1
        fi
    }
    if [ -n "$PREFIX" ]; then
        remove_file "$PREFIX/bin/$BIN_NAME"
        remove_file "$PREFIX/bin/duck-update"
        remove_tree "$PREFIX/share/duck-lang"
    else
        for dir in "$DEFAULT_PREFIX" "$USER_PREFIX"; do
            remove_file "$dir/bin/$BIN_NAME"
            remove_file "$dir/bin/duck-update"
            remove_tree "$dir/share/duck-lang"
        done
    fi
    [ "$removed" -eq 1 ] || echo "$BIN_NAME is not installed"

    if [ "$EDITOR_EXT" -eq 1 ]; then
        for ed in \
            "$EXT_HOME/.vscode/extensions" \
            "$EXT_HOME/.vscode-oss/extensions" \
            "$EXT_HOME/.cursor/extensions" \
            "$EXT_HOME/.vscode-server/extensions" \
            "$EXT_HOME/.vscode-server-insiders/extensions"
        do
            for ext in "$ed"/duck-lang-*; do
                if [ -d "$ext" ]; then
                    rm -rf "$ext"
                    echo "removed $ext"
                fi
            done
        done
    fi
    exit 0
fi

# ------------------------------------------------------------- prerequisites
missing=""
for tool in make as ld git; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1 &&
   ! command -v clang >/dev/null 2>&1; then
    missing="$missing cc"
fi

if [ -n "$missing" ]; then
    echo "install.sh: error: missing required tools:$missing" >&2
    echo "install.sh: on Debian/Ubuntu run: sudo apt install build-essential binutils git" >&2
    echo "install.sh: on Fedora run: sudo dnf install gcc make binutils git" >&2
    exit 1
fi

# ------------------------------------------------------------- source tree
SOURCE_DIR=""
if [ -f Makefile ] && [ -f src/main.c ] && [ -f SPEC.md ]; then
    SOURCE_DIR="$(pwd)"
    echo "==> building from the current directory: $SOURCE_DIR"
else
    WORK="$(mktemp -d)"
    trap 'rm -rf "$WORK"' EXIT
    echo "==> cloning $REPO_URL"
    git clone --quiet --depth 1 "$REPO_URL" "$WORK/duck-lang"
    SOURCE_DIR="$WORK/duck-lang"
fi

# ------------------------------------------------------------------- build
echo "==> building $BIN_NAME"
make --quiet -C "$SOURCE_DIR"

# ----------------------------------------------------------------- install
if [ -z "$PREFIX" ]; then
    if [ -w "$DEFAULT_PREFIX" ] || [ -w "$DEFAULT_PREFIX/bin" ]; then
        PREFIX="$DEFAULT_PREFIX"
    else
        PREFIX="$USER_PREFIX"
        echo "==> $DEFAULT_PREFIX is not writable, installing to $PREFIX instead"
    fi
fi

mkdir -p "$PREFIX/bin" 2>/dev/null || true
if [ ! -d "$PREFIX/bin" ] || { [ ! -w "$PREFIX/bin" ] && [ ! -w "$PREFIX" ]; }; then
    echo "install.sh: error: cannot write to $PREFIX/bin" >&2
    echo "install.sh: retry with: sh install.sh --user   (installs into $USER_PREFIX)" >&2
    exit 1
fi

if command -v install >/dev/null 2>&1; then
    install -m 0755 "$SOURCE_DIR/$BIN_NAME" "$PREFIX/bin/$BIN_NAME"
else
    cp "$SOURCE_DIR/$BIN_NAME" "$PREFIX/bin/$BIN_NAME"
    chmod 0755 "$PREFIX/bin/$BIN_NAME"
fi

# Ship the updater as a command so Duck can be updated later from anywhere.
if [ -f "$SOURCE_DIR/update.sh" ]; then
    if command -v install >/dev/null 2>&1; then
        install -m 0755 "$SOURCE_DIR/update.sh" "$PREFIX/bin/duck-update"
    else
        cp "$SOURCE_DIR/update.sh" "$PREFIX/bin/duck-update"
        chmod 0755 "$PREFIX/bin/duck-update"
    fi
    echo "installed $PREFIX/bin/duck-update (run 'duck-update' to update Duck)"
fi

# Stamp the commit that was built so 'duck-update --check' can compare it
# with the tip of origin/main.
DUCK_COMMIT="unknown"
if [ -d "$SOURCE_DIR/.git" ] && command -v git >/dev/null 2>&1; then
    DUCK_COMMIT="$(git -C "$SOURCE_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
fi
mkdir -p "$PREFIX/share/duck-lang"
echo "$DUCK_COMMIT" > "$PREFIX/share/duck-lang/commit"

echo
echo "==> $BIN_NAME installed:"
"$PREFIX/bin/$BIN_NAME" --version

# --------------------------------------------------------- editor extension
if [ "$EDITOR_EXT" -eq 1 ]; then
    if [ -f "$SOURCE_DIR/editors/vscode/install.sh" ]; then
        echo "==> installing the editor extension (.duck syntax highlighting)"
        if ! sh "$SOURCE_DIR/editors/vscode/install.sh" --optional; then
            echo "install.sh: warning: the editor extension was not installed" >&2
        fi
    fi
else
    echo "==> editor extension skipped (--no-editor)"
fi

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *)
        echo
        echo "note: $PREFIX/bin is not in your PATH, add this to your shell profile:"
        echo "      export PATH=\"$PREFIX/bin:\$PATH\""
        ;;
esac

cat <<EOF

Quick start:
  cat > hello.duck <<'DUCK'
  fn main() -> int {
      serve("Hello, Duck!");
      return 0;
  }
  DUCK
  duckc hello.duck -o hello && ./hello

Documentation: https://github.com/didacg/duck-lang
Uninstall with: sh install.sh --uninstall
EOF
