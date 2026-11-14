#!/bin/sh
# Cross-build SwitchPowerTray.exe (ARM64, plus optional x64 for Wine self-test).
set -eu

src=$(CDPATH= cd -- "$(dirname "$0")" && pwd -P)
out=${1:?output directory}
mingw_bin=${MINGW_BIN:-/Users/rujian/.local/share/mesa-switch-woa-build/toolchain/llvm-mingw-20261104-ucrt-macos-universal/bin}

mkdir -p "$out"

"$mingw_bin/aarch64-w64-mingw32-clang" \
    -std=c11 -O2 -Wall -Wextra -Werror -Wno-format-truncation \
    -municode -mwindows \
    "$src/SwitchPowerTray.c" \
    -luser32 -lshell32 -ladvapi32 -lgdi32 \
    -o "$out/SwitchPowerTray.exe"
"$mingw_bin/aarch64-w64-mingw32-strip" "$out/SwitchPowerTray.exe"

if [ "${BUILD_X64:-0}" = "1" ]; then
    "$mingw_bin/x86_64-w64-mingw32-clang" \
        -std=c11 -O2 -Wall -Wextra -Werror -Wno-format-truncation \
        -municode -mwindows \
        "$src/SwitchPowerTray.c" \
        -luser32 -lshell32 -ladvapi32 -lgdi32 \
        -o "$out/SwitchPowerTray-x64.exe"
fi

cp "$src/Install-SwitchPowerTray.cmd" "$out/Install-SwitchPowerTray.cmd"
cp "$src/Restore-SwitchPowerTray.cmd" "$out/Restore-SwitchPowerTray.cmd"
