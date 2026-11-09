#!/usr/bin/env bash
# Move subprojects/quickjs.wrap to the newest commit on bellard/quickjs master, refetch it and check every local patch still applies.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WRAP="$ROOT/subprojects/quickjs.wrap"
URL=https://github.com/bellard/quickjs.git

rev=${1:-$(git ls-remote "$URL" refs/heads/master | cut -f1)}
case "$rev" in
    *[!0-9a-f]* | "") echo "bump-bellard-quickjs: not a commit id: $rev" >&2; exit 1 ;;
esac
old=$(sed -n 's/^revision = //p' "$WRAP")
if [ "$old" = "$rev" ]; then
    echo "subprojects/quickjs.wrap already pins $rev"
    exit 0
fi

tmp=$(mktemp "$WRAP.XXXXXX")
sed "s/^revision = .*/revision = $rev/" "$WRAP" > "$tmp"
mv "$tmp" "$WRAP"

rm -rf "$ROOT/subprojects/quickjs"
if ! (cd "$ROOT" && meson subprojects download quickjs); then
    echo "bump-bellard-quickjs: a patch in subprojects/packagefiles/quickjs-patches no longer applies;" >&2
    echo "drop the ones upstream now carries from diff_files in subprojects/quickjs.wrap and rerun." >&2
    exit 1
fi
echo "subprojects/quickjs.wrap: $old -> $rev"
echo "upstream commits since the old pin: https://github.com/bellard/quickjs/compare/$old...$rev"
echo "next: meson setup --reconfigure <builddir> with -Dquickjs=quickjs, build, and run the checks in docs/quickjs.md"
