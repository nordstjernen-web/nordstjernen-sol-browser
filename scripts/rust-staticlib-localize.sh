#!/bin/sh
# Nordstjernen: pre-link a Rust staticlib and hide everything but its C API, so its bundled std cannot clash with another Rust library's.
# Usage: rust-staticlib-localize.sh IN OUT SYSTEM CPU LD OBJCOPY AR PATTERN...
set -eu
in=$1
out=$2
system=$3
cpu=$4
ld=$5
objcopy=$6
ar=$7
shift 7
merged="$out.merged.o"
rm -f "$out" "$merged"
if [ "$system" = darwin ]; then
    exports="$out.exports"
    : > "$exports"
    for pattern in "$@"; do
        echo "_$pattern" >> "$exports"
    done
    case "$cpu" in
        aarch64) arch=arm64 ;;
        *)       arch=$cpu ;;
    esac
    "$ld" -r -arch "$arch" -all_load "$in" -exported_symbols_list "$exports" -o "$merged"
    rm -f "$exports"
else
    "$ld" -r --whole-archive "$in" -o "$merged"
    keep=""
    for pattern in "$@"; do
        keep="$keep --keep-global-symbol=$pattern"
    done
    "$objcopy" --redefine-sym DW.ref.rust_eh_personality=DW.ref.ns_private_rust_eh_personality "$merged"
    "$objcopy" --wildcard $keep "$merged"
fi
"$ar" rcs "$out" "$merged"
rm -f "$merged"
