#!/bin/sh
# install.sh - installs the Duck language extension into VS Code-compatible
# editors by copying it into their extension directory. Works offline and
# needs no marketplace account.
#
# Usage:
#   sh install.sh                install into every editor found
#   sh install.sh --uninstall    remove it again
set -eu

SRC_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
VERSION="$(grep -m1 '"version"' "$SRC_DIR/package.json" | sed 's/.*: *"\([^"]*\)".*/\1/')"
EXT_NAME="duck-lang-$VERSION"

UNINSTALL=0
case "${1:-}" in
    --uninstall) UNINSTALL=1 ;;
    -h|--help)
        echo "usage: sh install.sh [--uninstall]"
        exit 0
        ;;
    "") ;;
    *)
        echo "install.sh: error: unknown option '$1'" >&2
        exit 1
        ;;
esac

DIRS=""
for d in \
    "$HOME/.vscode/extensions" \
    "$HOME/.vscode-oss/extensions" \
    "$HOME/.cursor/extensions" \
    "$HOME/.vscode-server/extensions" \
    "$HOME/.vscode-server-insiders/extensions"
do
    if [ -d "$d" ]; then
        DIRS="$DIRS $d"
    fi
done

if [ -z "$DIRS" ]; then
    echo "install.sh: error: no VS Code-compatible extensions directory found" >&2
    echo "install.sh: expected one of ~/.vscode/extensions, ~/.vscode-oss/extensions, ~/.cursor/extensions" >&2
    exit 1
fi

for dir in $DIRS; do
    target="$dir/$EXT_NAME"
    if [ "$UNINSTALL" -eq 1 ]; then
        if [ -d "$target" ]; then
            rm -rf "$target"
            echo "removed $target"
        else
            echo "not installed in $dir"
        fi
    else
        rm -rf "$target"
        mkdir -p "$target/syntaxes"
        cp "$SRC_DIR/package.json" "$target/"
        cp "$SRC_DIR/language-configuration.json" "$target/"
        cp "$SRC_DIR/README.md" "$target/"
        cp "$SRC_DIR/syntaxes/duck.tmLanguage.json" "$target/syntaxes/"
        echo "installed $EXT_NAME -> $target"
    fi
done

if [ "$UNINSTALL" -eq 0 ]; then
    echo
    echo "Run 'Developer: Reload Window' in the editor (or restart it) to load the extension."
fi
