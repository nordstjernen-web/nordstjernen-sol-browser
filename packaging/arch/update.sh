#!/bin/sh
# update.sh: point the Arch PKGBUILD and .SRCINFO at a release tag and its wrap pins.
set -eu

ver=${1:?usage: update.sh <release tag>}
here=$(cd "$(dirname "$0")" && pwd)

wrapfield() {
  git -C "$here" show "$ver:subprojects/$1.wrap" | sed -n "s/^$2 *= *//p"
}

pdfjs=$(wrapfield pdfjs-dist source_filename | sed -n 's/^pdfjs-dist-\(.*\)\.tgz$/\1/p')
wuffs=$(wrapfield wuffs source_filename | sed -n 's/^wuffs-\(.*\)\.tar\.gz$/\1/p')
wamr=$(wrapfield wamr source_filename | sed -n 's/^wasm-micro-runtime-WAMR-\(.*\)\.tar\.gz$/\1/p')
if [ -z "$pdfjs" ] || [ -z "$wuffs" ] || [ -z "$wamr" ]; then
  echo "update.sh: could not read the PDF.js, Wuffs and WAMR pins at $ver" >&2
  exit 1
fi

cd "$here"
cur=$(sed -n 's/^pkgver=//p' PKGBUILD)
sed -i \
  -e "s/^pkgver=.*/pkgver=$ver/" \
  -e "s/^_pdfjsver=.*/_pdfjsver=$pdfjs/" \
  -e "s/^_wuffsver=.*/_wuffsver=$wuffs/" \
  -e "s/^_wamrver=.*/_wamrver=$wamr/" \
  PKGBUILD
if [ "$cur" != "$ver" ]; then
  sed -i 's/^pkgrel=.*/pkgrel=1/' PKGBUILD
fi

updpkgsums

for w in pdfjs-dist wuffs wamr; do
  h=$(wrapfield "$w" source_hash)
  if ! grep -q "'$h'" PKGBUILD; then
    echo "update.sh: $w downloaded with a checksum other than subprojects/$w.wrap at $ver" >&2
    exit 1
  fi
done

makepkg --printsrcinfo > .SRCINFO
echo "PKGBUILD and .SRCINFO now build $ver (PDF.js $pdfjs, Wuffs $wuffs, WAMR $wamr)"
