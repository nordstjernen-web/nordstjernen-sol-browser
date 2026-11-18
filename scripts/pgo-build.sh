#!/usr/bin/env bash
# pgo-build.sh — build with profile-guided optimisation: instrumented build, training run over data/render-tests/ and the QuickJS microbench, then the optimised rebuild.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILDDIR=${BUILDDIR:-$ROOT/builddir-pgo}
VIEWPORT=${NS_PGO_VIEWPORT:-1280}

log() { printf 'pgo-build: %s\n' "$*"; }

cd "$ROOT"
if [ -f "$BUILDDIR/build.ninja" ]; then
    meson configure "$BUILDDIR" -Dpgo=generate "$@"
else
    meson setup "$BUILDDIR" -Dpgo=generate "$@"
fi
find "$BUILDDIR" \( -name '*.gcda' -o -name '*.profraw' -o -name '*.profdata' \) -delete
meson compile -C "$BUILDDIR" ${NS_BUILD_JOBS:+-j "$NS_BUILD_JOBS"}

BIN=$BUILDDIR/src/gtk/nordstjernen
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

log "training on data/render-tests"
for page in "$ROOT"/data/render-tests/*.html "$ROOT"/docs/demos/*.html; do
    name=$(basename "$page" .html)
    "$BIN" --headless --viewport="$VIEWPORT" --dump="png:$OUT/$name.png" \
        "file://$page" >/dev/null 2>&1 || log "  $name did not render"
    "$BIN" --headless --dump=text "file://$page" >/dev/null 2>&1 || true
done

for qjs in "$BUILDDIR/src/quickjs-bellard/qjs-bellard" "$BUILDDIR/src/quickjs/qjs"; do
    if [ -x "$qjs" ]; then
        log "training on the QuickJS microbench"
        "$qjs" "$ROOT/src/quickjs-bellard/tests/microbench.js" >/dev/null
    fi
done

if ls "$BUILDDIR"/pgo/*.profraw >/dev/null 2>&1; then
    profdata=$(command -v llvm-profdata || command -v llvm-profdata-"$(clang -dumpversion | cut -d. -f1)")
    "$profdata" merge -o "$BUILDDIR/pgo/default.profdata" "$BUILDDIR"/pgo/*.profraw
elif [ -z "$(find "$BUILDDIR" -name '*.gcda' -print -quit)" ]; then
    log "the training run wrote no profile"
    exit 1
fi

meson configure "$BUILDDIR" -Dpgo=use
meson compile -C "$BUILDDIR" ${NS_BUILD_JOBS:+-j "$NS_BUILD_JOBS"}
log "optimised build is in $BUILDDIR"
