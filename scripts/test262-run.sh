#!/usr/bin/env bash
# Fetches tc39/test262 into src/quickjs-bellard/test262 (gitignored) and runs it through the engine's run-test262 harness.
#
# Usage: scripts/test262-run.sh [run-test262 args...]
#   No args: full suite.
#   -u: regenerate src/quickjs-bellard/test262_errors.txt from the current
#       pass/fail state (run after fixing a known failure, or after a sync).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
QJS_DIR="$ROOT/src/quickjs-bellard"
BUILDDIR=${BUILDDIR:-$ROOT/builddir}

if [ ! -d "$QJS_DIR/test262/harness" ]; then
    echo "fetching tc39/test262 (shallow, main) into src/quickjs-bellard/test262 ..." >&2
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    curl -sS -L -o "$TMP/test262.tar.gz" \
        https://codeload.github.com/tc39/test262/tar.gz/refs/heads/main
    mkdir -p "$QJS_DIR/test262"
    tar xzf "$TMP/test262.tar.gz" -C "$QJS_DIR/test262" --strip-components=1
fi

[ -d "$BUILDDIR" ] || meson setup "$BUILDDIR" -Dlto=false >&2
meson compile -C "$BUILDDIR" run-test262-bellard >&2

cd "$QJS_DIR"
exec "$BUILDDIR/src/quickjs-bellard/run-test262-bellard" -m -c test262.conf -a "$@"
