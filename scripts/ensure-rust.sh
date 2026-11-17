#!/bin/sh
# Make sure rustc meets the floor meson.build sets: when the system one is older, install the stable toolchain with rustup into ~/.cargo/bin, or on Alpine the rust package from edge.
set -eu

FLOOR_MINOR=100
PATH="$HOME/.cargo/bin:$PATH"
export PATH

new_enough() {
    command -v rustc >/dev/null 2>&1 || return 1
    have=$(rustc --version | awk '{ split($2, v, "."); print v[1] * 1000 + v[2] }')
    [ "$have" -ge $((1000 + FLOOR_MINOR)) ]
}

if ! new_enough; then
    echo "rustc is missing or older than 1.$FLOOR_MINOR; installing a newer one" >&2
    if command -v apk >/dev/null 2>&1; then
        edge=https://dl-cdn.alpinelinux.org/alpine/edge
        apk add --no-cache --repository="$edge/main" --repository="$edge/community" \
            "rust>=1.$FLOOR_MINOR"
    elif command -v rustup >/dev/null 2>&1; then
        rustup toolchain install stable --profile minimal
        rustup default stable
    else
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs |
            RUSTUP_INIT_SKIP_PATH_CHECK=yes sh -s -- -y --profile minimal --default-toolchain stable
    fi
    hash -r
    new_enough
fi
rustc --version
