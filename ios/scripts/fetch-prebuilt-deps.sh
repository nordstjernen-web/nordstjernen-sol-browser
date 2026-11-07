#!/usr/bin/env bash
#
# Download the prebuilt iOS dependency sysroot published by the
# nordstjernen-dependencies-build repo's "build-ios-deps" workflow as a public
# GitHub Release, and lay it out so ios/scripts/build-engine.sh can consume it
# via $NORDSTJERNEN_IOS_SYSROOT.
#
# Release assets (under the rolling tag, default 'ios-sysroot-latest'):
#   nordstjernen-ios-sysroot-<platform>.tar.gz   # each contains a top-level <platform>/
#   SHA256SUMS
#
# Each asset normally carries a GitHub build-provenance attestation. With an
# authenticated `gh` CLI (GH_TOKEN on CI) it is verified; a missing gh, missing
# auth or a missing attestation (Sigstore outage at publish time) only warns
# unless NORDSTJERNEN_REQUIRE_ATTESTATION=1, while an invalid one is fatal. Pass
# --tag ios-sysroot-<date>-<sha> (an immutable per-commit release) to pin.
#
# platform is 'device' (arm64 iphoneos) or 'simulator' (arm64 iphonesimulator).
# These are public, so no authentication is required — just curl, tar and
# sha256sum. After running, the layout is:
#   $SYSROOT/device/{include,lib,lib/pkgconfig}
#   $SYSROOT/simulator/...
#
# Which release is fetched is pinned by ios/sysroot.lock (an immutable
# per-commit tag plus the sha256 of that release's SHA256SUMS). The downloaded
# SHA256SUMS must hash to the locked value, so a tampered release cannot change
# what gets built without a reviewed change to the lock. --tag overrides the
# lock (e.g. --tag ios-sysroot-latest for bleeding edge) and skips that check.
#
# Usage:
#   fetch-prebuilt-deps.sh [--sysroot DIR] [--platform device|simulator]
#                          [--repo OWNER/REPO] [--tag TAG]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCK_FILE="${SCRIPT_DIR}/../sysroot.lock"
lock_get() { sed -n "s/^$1=//p" "${LOCK_FILE}" 2>/dev/null | head -1; }

REPO="nordstjernen-web/nordstjernen-dependencies-build"
TAG=""
LOCKED_SUMS_SHA256=""
SYSROOT_BASE="${NORDSTJERNEN_IOS_SYSROOT:-$HOME/.cache/nordstjernen-ios-sysroot}"
PLATFORMS=(device simulator)

while [ "$#" -gt 0 ]; do
  case "$1" in
    --sysroot)  SYSROOT_BASE="$2"; shift 2 ;;
    --platform) PLATFORMS=("$2"); shift 2 ;;
    --repo)     REPO="$2"; shift 2 ;;
    --tag)      TAG="$2"; shift 2 ;;
    -h|--help)  sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ -z "${TAG}" ]; then
  TAG="$(lock_get tag)"
  LOCKED_SUMS_SHA256="$(lock_get sha256sums)"
  [ -n "${TAG}" ] || { echo "no --tag given and no tag in ${LOCK_FILE}" >&2; exit 2; }
  echo "Using pinned release ${TAG} from ios/sysroot.lock"
else
  echo "Using release ${TAG} from --tag (bypasses ios/sysroot.lock; its SHA256SUMS is not checked against the lock)" >&2
fi

command -v curl      >/dev/null 2>&1 || { echo "curl is required" >&2; exit 2; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum is required" >&2; exit 2; }

BASE_URL="https://github.com/${REPO}/releases/download/${TAG}"
mkdir -p "${SYSROOT_BASE}"
SYSROOT_BASE="$(cd "${SYSROOT_BASE}" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

UA="nordstjernen-ios-deps/1.0 (+https://github.com/${REPO})"
dl() { curl -fsSL --proto '=https' --proto-redir '=https' --retry 4 --retry-delay 2 -A "${UA}" -o "$2" "$1"; }

verify_attestation() {
  local file="$1" name="${1##*/}"
  local required="${NORDSTJERNEN_REQUIRE_ATTESTATION:-0}"
  soft_fail() {
    if [ "${required}" != "0" ]; then echo "$1 (NORDSTJERNEN_REQUIRE_ATTESTATION is set)" >&2; exit 1; fi
    echo "$1; skipping provenance verification of ${name} (set NORDSTJERNEN_REQUIRE_ATTESTATION=1 to require it)" >&2
  }
  if ! command -v gh >/dev/null 2>&1; then soft_fail "gh CLI not found"; return 0; fi
  if ! gh auth status >/dev/null 2>&1; then soft_fail "gh CLI is not authenticated (set GH_TOKEN)"; return 0; fi
  local digest count
  digest="$(sha256sum "${file}" | awk '{print $1}')"
  local api_out
  if api_out="$(gh api "repos/${REPO}/attestations/sha256:${digest}" --jq '.attestations | length' 2>&1)"; then
    count="${api_out}"
  elif printf '%s' "${api_out}" | grep -qiE 'not found|HTTP 404'; then
    count=0
  else
    soft_fail "could not query the attestations API for ${name}"; return 0
  fi
  if [ "${count}" = "0" ]; then soft_fail "no build-provenance attestation is published for ${name}"; return 0; fi
  gh attestation verify "${file}" --repo "${REPO}" >/dev/null 2>&1 \
    || { echo "provenance attestation check FAILED for ${name} (repo ${REPO}); refusing to install" >&2; exit 1; }
  echo "verified build provenance of ${name}"
}

# The published sysroot's pkg-config files bake in the dependency-build repo's
# build-time absolute prefix (…/sysroot/<platform>), which does not exist on the
# consumer runner. Rewrite every baked path ending in /sysroot/<platform> to the
# local unpack path so pkg-config resolves the relocated headers and libraries.
# Matching by that shape (rather than a prefix= line or a fixed repo name) holds
# for CMake-generated .pc files that hardcode includedir/libdir with no prefix=
# line — uchardet is one — and regardless of the workspace the sysroot was built
# in. It is idempotent: the local path has no literal /sysroot/<platform>
# segment, so an already-relocated file never re-matches.
relocate_pkgconfig_prefixes() {
  local prefix_dir="$1"
  local platform
  platform="$(basename "${prefix_dir}")"
  [ -d "${prefix_dir}/lib/pkgconfig" ] || return 0
  local pc
  for pc in "${prefix_dir}"/lib/pkgconfig/*.pc; do
    [ -e "${pc}" ] || continue
    python3 - "${pc}" "${prefix_dir}" "${platform}" <<'PY'
import re, sys
path, local, platform = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
pat = re.compile(r'/[^\s:="\']*?/sysroot/' + re.escape(platform) + r'(?=[/\s:"\']|$)')
new = pat.sub(lambda m: local, text)
if new != text:
    open(path, "w").write(new)
PY
  done
}

echo "Downloading checksum manifest from ${TAG}"
dl "${BASE_URL}/SHA256SUMS" "${tmp}/SHA256SUMS" \
  || { echo "could not fetch SHA256SUMS from release '${TAG}' (does it exist yet?)" >&2; exit 1; }
if [ -n "${LOCKED_SUMS_SHA256}" ]; then
  got="$(sha256sum "${tmp}/SHA256SUMS" | awk '{print $1}')"
  [ "${got}" = "${LOCKED_SUMS_SHA256}" ] \
    || { echo "SHA256SUMS of release ${TAG} hashes to ${got}, but ios/sysroot.lock pins ${LOCKED_SUMS_SHA256}; the release was changed after it was locked, refusing" >&2; exit 1; }
  echo "SHA256SUMS matches ios/sysroot.lock"
fi

for platform in "${PLATFORMS[@]}"; do
  case "${platform}" in device|simulator) ;; *) echo "invalid platform: ${platform}" >&2; exit 2 ;; esac
  asset="nordstjernen-ios-sysroot-${platform}.tar.gz"
  echo "Downloading ${asset}"
  dl "${BASE_URL}/${asset}" "${tmp}/${asset}" || { echo "download failed for ${asset}" >&2; exit 1; }

  want="$(awk -v f="${asset}" '$2 ~ ("(^|/)" f "$") {print $1}' "${tmp}/SHA256SUMS" | head -1)"
  [ -n "${want}" ] || { echo "no checksum for ${asset} in SHA256SUMS" >&2; exit 1; }
  echo "${want}  ${tmp}/${asset}" | sha256sum -c - >/dev/null \
    || { echo "checksum mismatch for ${asset}" >&2; exit 1; }
  verify_attestation "${tmp}/${asset}"

  rm -rf "${SYSROOT_BASE:?}/${platform}"
  tar -xzf "${tmp}/${asset}" -C "${SYSROOT_BASE}"
  [ -d "${SYSROOT_BASE}/${platform}/lib" ] || { echo "unexpected archive layout for ${platform}" >&2; exit 1; }
  relocate_pkgconfig_prefixes "${SYSROOT_BASE}/${platform}"
  echo "Installed ${platform} -> ${SYSROOT_BASE}/${platform}"
done

cat >&2 <<EOF

Prebuilt iOS sysroot ready. Point the engine build at it with:

    export NORDSTJERNEN_IOS_SYSROOT="${SYSROOT_BASE}"

then run ios/scripts/build-engine.sh device (and simulator).
EOF
