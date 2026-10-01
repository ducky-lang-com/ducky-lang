#!/bin/sh
# install.sh - installs the Duck language extension into VS Code-compatible
# editors by copying it into their extension directory. Works offline and
# needs no marketplace account.
#
# Usage:
#   sh install.sh                install into every editor found
#   sh install.sh --optional     do not fail when no editor is installed
#   sh install.sh --uninstall    remove it again
#   sh install.sh --uninstall --optional
set -eu

SRC_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
VERSION="$(grep -m1 '"version"' "$SRC_DIR/package.json" | sed 's/.*: *"\([^"]*\)".*/\1/')"
EXT_NAME="duck-lang-$VERSION"

UNINSTALL=0
OPTIONAL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --uninstall) UNINSTALL=1; shift ;;
        --optional)  OPTIONAL=1;  shift ;;
        -h|--help)
            echo "usage: sh install.sh [--uninstall] [--optional]"
            exit 0
            ;;
        *)
            echo "install.sh: error: unknown option '$1'" >&2
            exit 1
            ;;
    esac
done

# When run through sudo, target the invoking user's editor directories.
EXT_HOME="$HOME"
if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
    su_home="$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6)"
    if [ -n "$su_home" ]; then EXT_HOME="$su_home"; fi
fi

DIRS=""
for d in \
    "$EXT_HOME/.vscode/extensions" \
    "$EXT_HOME/.vscode-oss/extensions" \
    "$EXT_HOME/.cursor/extensions" \
    "$EXT_HOME/.vscode-server/extensions" \
    "$EXT_HOME/.vscode-server-insiders/extensions"
do
    if [ -d "$d" ]; then
        DIRS="$DIRS $d"
    fi
done

if [ -z "$DIRS" ]; then
    if [ "$OPTIONAL" -eq 1 ]; then
        echo "==> no VS Code-compatible editor found, extension not installed"
        echo "    install it later with: make install-vscode"
        exit 0
    fi
    echo "install.sh: error: no VS Code-compatible extensions directory found" >&2
    echo "install.sh: expected one of ~/.vscode/extensions, ~/.vscode-oss/extensions, ~/.cursor/extensions" >&2
    exit 1
fi

for dir in $DIRS; do
    target="$dir/$EXT_NAME"
    if [ "$UNINSTALL" -eq 1 ]; then
        found=0
        for old in "$dir"/duck-lang-*; do
            if [ -d "$old" ]; then
                rm -rf "$old"
                echo "removed $old"
                found=1
            fi
        done
        if [ "$found" -eq 0 ]; then
            echo "not installed in $dir"
        fi
    else
        # Drop any previously installed version so only one copy is loaded.
        for old in "$dir"/duck-lang-*; do
            if [ -d "$old" ] && [ "$old" != "$target" ]; then
                rm -rf "$old"
                echo "removed old $old"
            fi
        done
        rm -rf "$target"
        mkdir -p "$target/syntaxes" "$target/assets"
        cp "$SRC_DIR/package.json" "$target/"
        cp "$SRC_DIR/language-configuration.json" "$target/"
        cp "$SRC_DIR/README.md" "$target/"
        cp "$SRC_DIR/syntaxes/duck.tmLanguage.json" "$target/syntaxes/"
        if [ -d "$SRC_DIR/assets" ]; then
            cp "$SRC_DIR"/assets/* "$target/assets/"
        fi
        echo "installed $EXT_NAME -> $target"
    fi
done

if [ "$UNINSTALL" -eq 0 ]; then
    echo
    echo "Run 'Developer: Reload Window' in the editor (or restart it) to load the extension."
fi
