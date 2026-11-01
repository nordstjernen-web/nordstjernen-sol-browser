#!/usr/bin/env bash
# Build a portable Nordstjernen Alpine package (.apk) by repackaging the
# bundle that pack-linux.sh produces. The binary is statically linked
# against the in-tree engine (lexbor, quickjs). tracedeps is disabled
# (the APKBUILD has no source=), so depends= is derived below by mapping
# every library the staged binaries load to its owning Alpine package.
set -euo pipefail

log() { printf '[pack-apk] %s\n' "$*" >&2; }
trap 'rc=$?; printf "[pack-apk] FAILED (exit %s) at line %s: %s\n" \
    "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR
[ -n "${NS_DEBUG:-}" ] && set -x

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${VERSION:-$(awk -F"'" \
    '/^[[:space:]]*version[[:space:]]*:/ { print $2; exit }' "$ROOT/meson.build")}
ARCH=$(uname -m)
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
SLUG="nordstjernen-${FSVERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"

if ! command -v abuild >/dev/null 2>&1; then
    echo "abuild not found. Install it first:" >&2
    echo "    sudo apk add alpine-sdk       # Alpine" >&2
    exit 1
fi

if [ ! -x "$STAGE/nordstjernen" ]; then
    log "bundle not staged yet — running pack-linux.sh first"
    "$ROOT/scripts/pack-linux.sh"
fi

APKVER=$(printf '%s' "$VERSION" | tr 'A-Z' 'a-z')
APKBASE=$(printf '%s' "$APKVER" | grep -oE '^[0-9]+(\.[0-9]+)*' || true)
APKBASE=${APKBASE:-0}
APKREST=${APKVER#"$APKBASE"}
APKDIGITS=$(printf '%s' "$APKREST" | tr -cd '0-9')
if [ -n "$APKDIGITS" ]; then
    APKVER="${APKBASE}_git${APKDIGITS:0:12}"
else
    APKVER="$APKBASE"
fi
log "version=$VERSION apkver=$APKVER arch=$ARCH"

export PACKAGER="${PACKAGER:-Andreas Røsdal <andreas.rosdal@gmail.com>}"
ABUILD_HOME="$ROOT/dist/abuild-home"
rm -rf "$ABUILD_HOME"
mkdir -p "$ABUILD_HOME"
export HOME="$ABUILD_HOME"
abuild-keygen -a -i -n >/dev/null 2>&1 || abuild-keygen -a -n >/dev/null 2>&1

BUILDTOP="$ROOT/dist/apkbuild"
rm -rf "$BUILDTOP"
mkdir -p "$BUILDTOP"
export REPODEST="$ROOT/dist/apkrepo"
rm -rf "$REPODEST"

# tracedeps is off, so resolve every shared library the staged binaries load to
# the Alpine package that owns it; auto-detected features (SDL2 audio, libjxl,
# GStreamer, enchant, …) then appear in depends= exactly when they were built.
LIB_DEPS=$(for bin in nordstjernen nordstjernen-renderer nordstjernen-audio; do
        if [ -x "$STAGE/$bin" ]; then ldd "$STAGE/$bin" 2>/dev/null || true; fi
    done | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' | sort -u \
    | while read -r lib; do
        { apk info -W "$lib" 2>/dev/null || true; } | sed -n 's/.* is owned by //p'
    done | sed -E 's/-[0-9][^-]*-r[0-9]+$//' | sort -u | tr '\n' ' ')
[ -n "$LIB_DEPS" ] || { log "ERROR: could not resolve runtime packages"; exit 1; }
log "runtime packages: $LIB_DEPS"

cat > "$BUILDTOP/nordstjernen.post-install" <<'EOF'
#!/bin/sh
if [ "$(readlink /usr/bin/internet-browser)" = nordstjernen ]; then
    rm -f /usr/bin/internet-browser
fi
[ -e /usr/bin/open-browser-nordstjernen ] || \
    ln -sf nordstjernen /usr/bin/open-browser-nordstjernen
exit 0
EOF
cat > "$BUILDTOP/nordstjernen.pre-deinstall" <<'EOF'
#!/bin/sh
if [ "$(readlink /usr/bin/open-browser-nordstjernen)" = nordstjernen ]; then
    rm -f /usr/bin/open-browser-nordstjernen
fi
exit 0
EOF

cat > "$BUILDTOP/APKBUILD" <<APKBUILD_EOF
# Maintainer: Andreas Røsdal <andreas.rosdal@gmail.com>
pkgname=nordstjernen
pkgver=${APKVER}
pkgrel=0
pkgdesc="Nordstjernen Sol Explorer — a small, hand-written web browser"
url="https://nordstjernen.org"
arch="${ARCH}"
license="LicenseRef-NSL-1.0 OR GPL-3.0-or-later"
depends="${LIB_DEPS}ca-certificates fontconfig font-dejavu"
options="!check !tracedeps !strip"
source=""
install="nordstjernen.post-install nordstjernen.pre-deinstall"

build() {
	return 0
}

package() {
	install -Dm755 "${STAGE}/nordstjernen" "\$pkgdir/usr/bin/nordstjernen"
	install -Dm755 "${STAGE}/nordstjernen-renderer" "\$pkgdir/usr/bin/nordstjernen-renderer"
	if [ -e "${STAGE}/nordstjernen-audio" ]; then
		install -Dm755 "${STAGE}/nordstjernen-audio" "\$pkgdir/usr/bin/nordstjernen-audio"
	fi
	for icon in "${STAGE}"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \\
	            "${STAGE}"/data/icons/hicolor/scalable/apps/nordstjernen.gif; do
		[ -e "\$icon" ] && install -Dm644 "\$icon" \\
			"\$pkgdir/usr/share/icons/hicolor/scalable/apps/\$(basename "\$icon")"
	done
	install -Dm644 "${STAGE}/data/nordstjernen.desktop" \\
		"\$pkgdir/usr/share/applications/org.nordstjernen.WebBrowser.desktop"
	install -Dm644 "${STAGE}/License.md" \\
		"\$pkgdir/usr/share/nordstjernen/License.md"
	install -Dm644 "${STAGE}/COPYING" \\
		"\$pkgdir/usr/share/nordstjernen/COPYING"
	install -Dm644 "${STAGE}/README.md" \\
		"\$pkgdir/usr/share/doc/nordstjernen/README.md"
	install -Dm644 "${STAGE}/THIRD-PARTY-LICENSES.md" \\
		"\$pkgdir/usr/share/doc/nordstjernen/THIRD-PARTY-LICENSES.md"
}
APKBUILD_EOF

( cd "$BUILDTOP" && abuild -F -d rootpkg )

APKFILE=$(find "$REPODEST" -name "nordstjernen-${APKVER}-r*.apk" | head -n1 || true)
if [ -z "$APKFILE" ]; then
    echo "abuild produced no .apk — see $BUILDTOP for details." >&2
    exit 1
fi

DEST="$ROOT/dist/nordstjernen-${FSVERSION}-${ARCH}.apk"
cp "$APKFILE" "$DEST"
[ -s "$DEST" ] || { log "ERROR: $DEST missing or empty"; exit 1; }

apk_size=$(du -h "$DEST" 2>/dev/null | cut -f1 || echo '?')
echo
echo "Built: $DEST ($apk_size)"
echo
echo "Inspect: tar -tzf $DEST"
echo "Install: sudo apk add --allow-untrusted $DEST"
