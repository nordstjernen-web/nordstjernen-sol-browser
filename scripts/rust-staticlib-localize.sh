#!/bin/sh
# Nordstjernen: pre-link a Rust staticlib and hide everything but its C API, so its bundled std cannot clash with another Rust library's.
set -eu
in=$1
out=$2
ld=$3
objcopy=$4
ar=$5
shift 5
merged="$out.merged.o"
"$ld" -r --whole-archive "$in" -o "$merged"
keep=""
for pattern in "$@"; do
    keep="$keep --keep-global-symbol=$pattern"
done
"$objcopy" --redefine-sym DW.ref.rust_eh_personality=DW.ref.ns_private_rust_eh_personality "$merged"
"$objcopy" --wildcard $keep "$merged"
rm -f "$out"
"$ar" rcs "$out" "$merged"
rm -f "$merged"
