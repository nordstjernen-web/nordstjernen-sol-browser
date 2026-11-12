#!/usr/bin/env bash
# Fetch the pinned pdfjs-dist tarball named by subprojects/pdfjs-dist.wrap into a directory (default debian/vendor) and verify its SHA-256, for builds that cannot download.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WRAP="$ROOT/subprojects/pdfjs-dist.wrap"
DEST=${1:-"$ROOT/debian/vendor"}

wrap_value() { sed -n "s/^$1 *= *//p" "$WRAP" | head -n1; }

URL=$(wrap_value source_url)
NAME=$(wrap_value source_filename)
WANT=$(wrap_value source_hash)

mkdir -p "$DEST"
if [ ! -f "$DEST/$NAME" ]; then
    curl -fsSL --retry 5 --retry-delay 2 --connect-timeout 20 \
        -o "$DEST/$NAME.part" "$URL"
    mv "$DEST/$NAME.part" "$DEST/$NAME"
fi

if command -v sha256sum >/dev/null 2>&1; then
    GOT=$(sha256sum "$DEST/$NAME" | cut -d' ' -f1)
else
    GOT=$(shasum -a 256 "$DEST/$NAME" | cut -d' ' -f1)
fi
if [ "$GOT" != "$WANT" ]; then
    echo "fetch-pdfjs: $NAME has SHA-256 $GOT, expected $WANT" >&2
    rm -f "$DEST/$NAME"
    exit 1
fi
echo "$DEST/$NAME"
