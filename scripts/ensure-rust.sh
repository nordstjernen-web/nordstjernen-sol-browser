#!/bin/sh
# Make sure rustc meets the floor meson.build sets, installing the stable toolchain with rustup into ~/.cargo/bin when the system one is older.
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
    echo "rustc is missing or older than 1.$FLOOR_MINOR; installing stable with rustup" >&2
    if command -v rustup >/dev/null 2>&1; then
        rustup toolchain install stable --profile minimal
        rustup default stable
    else
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs |
            RUSTUP_INIT_SKIP_PATH_CHECK=yes sh -s -- -y --profile minimal --default-toolchain stable
    fi
    new_enough
fi
rustc --version
