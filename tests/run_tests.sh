#!/usr/bin/env bash
# run_tests.sh - test suite for the Duck language compiler.
#
#   tests/cases/*.duck    must compile, run and print exactly the contents of
#                         the matching .expected file (exit status from
#                         .exit, default 0).
#   tests/errors/*.duck   must fail to compile with a message containing the
#                         text of the matching .expected file.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKC="$ROOT/duckc"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

ok() {
    pass=$((pass + 1))
    printf '  ok    %s\n' "$1"
}

bad() {
    fail=$((fail + 1))
    printf '  FAIL  %s\n' "$1"
    shift
    for detail in "$@"; do
        printf '%s\n' "$detail" | sed 's/^/        /'
    done
}

if [ ! -x "$DUCKC" ]; then
    echo "error: $DUCKC not found - run 'make' first" >&2
    exit 1
fi

echo "runtime tests"
for src in "$ROOT"/tests/cases/*.duck; do
    name="$(basename "$src" .duck)"
    exe="$TMP/$name"

    if ! "$DUCKC" "$src" -o "$exe" 2>"$TMP/$name.cc.err"; then
        bad "$name" "compilation failed:" "$(cat "$TMP/$name.cc.err")"
        continue
    fi

    "$exe" >"$TMP/$name.out" 2>&1
    code=$?

    if ! diff -u "$ROOT/tests/cases/$name.expected" "$TMP/$name.out" >"$TMP/$name.diff"; then
        bad "$name" "output differs:" "$(cat "$TMP/$name.diff")"
        continue
    fi

    want=0
    if [ -f "$ROOT/tests/cases/$name.exit" ]; then
        want="$(tr -d '[:space:]' <"$ROOT/tests/cases/$name.exit")"
    fi
    if [ "$code" -ne "$want" ]; then
        bad "$name" "exit status: want $want, got $code"
        continue
    fi

    ok "$name"
done

echo "error tests"
for src in "$ROOT"/tests/errors/*.duck; do
    name="$(basename "$src" .duck)"

    if "$DUCKC" "$src" -o "$TMP/$name" 2>"$TMP/$name.err"; then
        bad "$name" "compilation succeeded but an error was expected"
        continue
    fi

    want="$(cat "$ROOT/tests/errors/$name.expected")"
    if ! grep -qF "$want" "$TMP/$name.err"; then
        bad "$name" "expected message not found" "want: $want" "got:  $(head -n 1 "$TMP/$name.err")"
        continue
    fi

    ok "$name"
done

echo
echo "$pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    exit 1
fi
