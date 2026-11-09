#!/usr/bin/env bash
# Prefetch the GitHub release source of the named wraps into subprojects/packagecache through the REST API; on any failure meson downloads it itself as before.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CACHE="$ROOT/subprojects/packagecache"

log() { printf '[prefetch-wrap] %s\n' "$*" >&2; }

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

wrap_value() { sed -n "s/^$2 *= *//p" "$1" | head -n1; }

github() {
    local accept=$1
    shift
    if [ -n "${GH_TOKEN:-}" ]; then
        set -- -H "Authorization: Bearer $GH_TOKEN" "$@"
    fi
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --connect-timeout 20 \
        -H "User-Agent: nordstjernen-prefetch-wrap" -H "Accept: $accept" "$@"
}

prefetch() {
    local wrap="$ROOT/subprojects/$1.wrap"
    if [ ! -f "$wrap" ]; then
        log "no wrap file for $1"
        return 1
    fi
    local url name want
    url=$(wrap_value "$wrap" source_url)
    name=$(wrap_value "$wrap" source_filename)
    want=$(wrap_value "$wrap" source_hash)
    local out="$CACHE/$name"
    if [ -f "$out" ] && [ "$(sha256 "$out")" = "$want" ]; then
        log "$name is already in the package cache"
        return 0
    fi
    local pattern='^https://github\.com/([^/]+)/([^/]+)/releases/download/([^/]+)/([^/]+)$'
    if ! [[ $url =~ $pattern ]]; then
        log "$1 does not download a GitHub release asset"
        return 1
    fi
    local repo="${BASH_REMATCH[1]}/${BASH_REMATCH[2]}" tag="${BASH_REMATCH[3]}" asset="${BASH_REMATCH[4]}"
    local release asset_url
    if ! release=$(github application/vnd.github+json "https://api.github.com/repos/$repo/releases/tags/$tag"); then
        log "could not look up release $tag of $repo"
        return 1
    fi
    asset_url=$(printf '%s' "$release" | python3 -c 'import json, sys
name = sys.argv[1]
print(next((a["url"] for a in json.load(sys.stdin).get("assets", []) if a["name"] == name), ""))' "$asset")
    if [ -z "$asset_url" ]; then
        log "release $tag of $repo has no asset $asset"
        return 1
    fi
    mkdir -p "$CACHE"
    local part="$out.part"
    if ! github application/octet-stream -o "$part" "$asset_url"; then
        rm -f "$part"
        log "download of $asset from $repo failed"
        return 1
    fi
    local got
    got=$(sha256 "$part")
    if [ "$got" != "$want" ]; then
        rm -f "$part"
        log "$asset hashes to $got, but $1.wrap pins $want"
        return 1
    fi
    mv -f "$part" "$out"
    log "cached $name from release $tag of $repo"
}

for wrap in "$@"; do
    prefetch "$wrap" || log "leaving $wrap to meson's own download"
done
exit 0
