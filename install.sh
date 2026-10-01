#!/bin/sh
# install.sh - installer for the Duck language compiler (duckc).
#
# Usage:
#   sh install.sh                build and install into the default prefix
#   sh install.sh --prefix DIR   install into DIR/bin
#   sh install.sh --user         install into $HOME/.local (no root needed)
#   sh install.sh --uninstall    remove the installed compiler
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
  --uninstall    remove the compiler installed in the prefix
  -h, --help     show this help

Without a checkout of the repository, the script clones it into a temporary
directory, builds it and installs the resulting binary.
EOF
}

PREFIX=""
UNINSTALL=0

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
    removed=0
    if [ -n "$PREFIX" ]; then
        if rm -f "$PREFIX/bin/$BIN_NAME"; then
            echo "removed $PREFIX/bin/$BIN_NAME"
            removed=1
        fi
    else
        for dir in "$DEFAULT_PREFIX" "$USER_PREFIX"; do
            if [ -e "$dir/bin/$BIN_NAME" ]; then
                rm -f "$dir/bin/$BIN_NAME"
                echo "removed $dir/bin/$BIN_NAME"
                removed=1
            fi
        done
    fi
    [ "$removed" -eq 1 ] || echo "$BIN_NAME is not installed"
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

echo
echo "==> $BIN_NAME installed:"
"$PREFIX/bin/$BIN_NAME" --version

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
