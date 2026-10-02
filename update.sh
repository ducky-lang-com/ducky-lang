#!/bin/sh
# update.sh - updates the Ducky language installation (duckyc, ducky-update and
# the editor extension) to the latest published version.
#
# Usage:
#   sh update.sh              update Ducky (rebuild + reinstall)
#   sh update.sh --check      only report whether a newer version exists
#   sh update.sh --help       show this help
#
# From a checkout of the repository it runs `git pull --ff-only` and then
# reinstalls your local sources, so local work is never overwritten.
# Anywhere else - including `curl | sh` - the latest sources are downloaded
# into a temporary directory and built there.
#
# Options such as --user, --prefix DIR and --no-editor are forwarded to
# install.sh. When no option is given and duckyc lives in ~/.local/bin,
# --user is assumed automatically.
#
# One-liner:
#   curl -fsSL https://raw.githubusercontent.com/ducky-lang-com/ducky-lang/main/update.sh | sh
set -eu

REPO_URL="https://github.com/ducky-lang-com/ducky-lang.git"

usage() {
    cat <<'EOF'
update.sh - updates the Ducky language installation

Options:
  --check        report whether a newer version exists (changes nothing)
  --user         update a --user installation (~/.local)
  --prefix DIR   update the installation in DIR/bin
  --no-editor    do not update the editor extension
  -h, --help     show this help

From a checkout it runs `git pull --ff-only` and reinstalls your local
sources. From anywhere else it downloads the latest sources and reinstalls.
Every other option is forwarded to install.sh.

Installed as `ducky-update` by install.sh; can also be run as:
  curl -fsSL https://raw.githubusercontent.com/ducky-lang-com/ducky-lang/main/update.sh | sh
EOF
}

CHECK_ONLY=0
USER_OPT=0
PREFIX_OPT=""
PREV=""
for arg in "$@"; do
    case "$PREV" in
        --prefix) PREFIX_OPT="$arg" ;;
    esac
    case "$arg" in
        --check)   CHECK_ONLY=1 ;;
        --user)    USER_OPT=1 ;;
        -h|--help) usage; exit 0 ;;
    esac
    PREV="$arg"
done

# ---------------------------------------------------------------------------
# Locate ourselves: a checkout next to this script (or the current directory
# when the script is piped from curl) vs. an installed copy in PREFIX/bin.
HERE="$(pwd)"
case "$0" in
    */*)
        if d="$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd)"; then
            HERE="$d"
        fi
        ;;
esac

IS_CHECKOUT=0
if [ -f "$HERE/Makefile" ] && [ -d "$HERE/.git" ] && [ -f "$HERE/src/main.c" ]; then
    IS_CHECKOUT=1
fi

# An installed copy ($PREFIX/bin/ducky-update) knows its own prefix.
SCRIPT_PREFIX=""
case "$HERE" in
    */bin)
        candidate="$(dirname "$HERE")"
        if [ -f "$candidate/share/ducky-lang/commit" ]; then
            SCRIPT_PREFIX="$candidate"
        fi
        ;;
esac

if [ -n "$PREFIX_OPT" ]; then
    PREFIX_DIR="$PREFIX_OPT"
elif [ "$USER_OPT" -eq 1 ]; then
    PREFIX_DIR="$HOME/.local"
elif [ -n "$SCRIPT_PREFIX" ]; then
    PREFIX_DIR="$SCRIPT_PREFIX"
else
    PREFIX_DIR="/usr/local"
fi

# ---------------------------------------------------------------------------
# --check: compare the installed commit (stamped by install.sh/make install)
# with the tip of origin/main, without touching anything.
if [ "$CHECK_ONLY" -eq 1 ]; then
    installed_commit="$(cat "$PREFIX_DIR/share/ducky-lang/commit" 2>/dev/null || true)"
    if [ -z "$installed_commit" ] && [ -z "$PREFIX_OPT" ] && [ "$USER_OPT" -eq 0 ]; then
        installed_commit="$(cat "$HOME/.local/share/ducky-lang/commit" 2>/dev/null || true)"
    fi
    remote_commit="$(git ls-remote "$REPO_URL" refs/heads/main 2>/dev/null | cut -f1 || true)"
    if [ -z "$remote_commit" ]; then
        echo "update.sh: error: cannot reach $REPO_URL (no network?)" >&2
        exit 1
    fi
    installed_ver="unknown"
    if command -v duckyc >/dev/null 2>&1; then
        installed_ver="$(duckyc --version 2>/dev/null | head -n 1 || echo unknown)"
    fi
    echo "installed: ${installed_commit:-unknown} ($installed_ver)"
    echo "latest:    $remote_commit"
    if [ -z "$installed_commit" ]; then
        echo "=> the installed commit could not be determined; run 'sh update.sh' to update anyway"
    elif [ "$installed_commit" = "$remote_commit" ]; then
        echo "=> Ducky is up to date"
    else
        echo "=> installed commit differs from origin/main; update with: sh update.sh"
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# Obtain the sources.
if [ "$IS_CHECKOUT" -eq 1 ]; then
    echo "==> updating the checkout: $HERE"
    if ! git -C "$HERE" pull --ff-only; then
        echo "update.sh: could not fast-forward." >&2
        echo "  Commit or stash your local changes first, then run this again." >&2
        exit 1
    fi
    SRC="$HERE"
else
    echo "==> downloading the latest sources from $REPO_URL"
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT INT TERM
    git clone --quiet --depth 1 "$REPO_URL" "$TMP/ducky-lang"
    SRC="$TMP/ducky-lang"
fi

# Assume --user when that is clearly how Ducky was installed.
if [ "$#" -eq 0 ] && [ "$USER_OPT" -eq 0 ] && [ -z "$PREFIX_OPT" ]; then
    if command -v duckyc >/dev/null 2>&1; then
        case "$(command -v duckyc)" in
            "$HOME/.local/bin/duckyc") set -- --user ;;
        esac
    fi
fi

echo "==> installing"
if ! sh "$SRC/install.sh" "$@"; then
    echo "update.sh: the update failed" >&2
    echo "  system-wide installation: sudo ducky-update (or sudo sh install.sh)" >&2
    echo "  user installation:        ducky-update --user" >&2
    exit 1
fi

echo "==> Ducky updated"
