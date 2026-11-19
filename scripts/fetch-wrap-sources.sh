#!/usr/bin/env bash
# Fetch the pinned source tarballs of the wraps a build downloads (PDF.js, Wuffs, WAMR) into a directory (default debian/vendor) and verify their SHA-256, for builds that cannot download.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-"$ROOT/debian/vendor"}
WRAPS=${WRAPS:-"pdfjs-dist wuffs wamr"}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

mkdir -p "$DEST"
for wrap in $WRAPS; do
    file="$ROOT/subprojects/$wrap.wrap"
    wrap_value() { sed -n "s/^$1 *= *//p" "$file" | head -n1; }
    url=$(wrap_value source_url)
    name=$(wrap_value source_filename)
    want=$(wrap_value source_hash)
    if [ ! -f "$DEST/$name" ]; then
        curl -fsSL --retry 5 --retry-delay 2 --connect-timeout 20 \
            -o "$DEST/$name.part" "$url"
        mv "$DEST/$name.part" "$DEST/$name"
    fi
    got=$(sha256_of "$DEST/$name")
    if [ "$got" != "$want" ]; then
        echo "fetch-wrap-sources: $name has SHA-256 $got, expected $want" >&2
        rm -f "$DEST/$name"
        exit 1
    fi
    echo "$DEST/$name"
done
