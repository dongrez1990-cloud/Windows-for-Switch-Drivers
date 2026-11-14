#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")" && pwd -P)
out=${1:?output directory}
bin=${MINGW_BIN:-/Users/rujian/.local/share/mesa-switch-woa-build/toolchain/llvm-mingw-20261104-ucrt-macos-universal/bin}
mkdir -p "$out"
for arch in aarch64 x86_64; do
    "$bin/$arch-w64-mingw32-clang" -std=c11 -O2 -Wall -Wextra -Werror \
        -Wno-format-truncation -municode -mwindows "$src/SwitchTelemetry.c" \
        -luser32 -lgdi32 -ladvapi32 -o "$out/SwitchTelemetry-$arch.exe"
done
