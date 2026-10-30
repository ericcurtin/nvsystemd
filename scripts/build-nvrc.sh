#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Build NVRC (the reference, unmodified) as a static musl release binary using NVRC's own
# [profile.release]. musl-gcc is not required: the pure-Rust dependency tree links with
# plain cc against the toolchain's self-contained musl crt/libc, so the linker is
# overridden through the environment instead of editing NVRC's .cargo/config.toml.
#
#   NVRC_SRC   source tree (default /home/ecurtin/nvsd-work/nvrc)
#   NVSD_OUT   output root (default /home/ecurtin/nvsd-work); cargo target dir is $NVSD_OUT/target
set -eu
export PATH=$HOME/.cargo/bin:/usr/bin:$PATH
NVRC_SRC=${NVRC_SRC:-/home/ecurtin/nvsd-work/nvrc}
ROOT=${NVSD_WORK:-/home/ecurtin/nvsd-work}
cd "$NVRC_SRC"
rustup target add x86_64-unknown-linux-musl >/dev/null 2>&1 || true
CARGO_TARGET_DIR=$ROOT/target \
CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER=cc \
	cargo build --locked --release --target x86_64-unknown-linux-musl
ls -l "$ROOT/target/x86_64-unknown-linux-musl/release/NVRC"
