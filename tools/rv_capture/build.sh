#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
out=${1:?Pass a new output directory}
if [[ $# -gt 2 || ${2:-native} != native ]]; then
 echo 'Expected a native capture build' >&2
 exit 2
fi
prefix=${RV1103_CROSS_PREFIX:-/opt/rv1103-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-}
mkdir "$out"
"${prefix}g++" -std=c++17 -O3 -ffp-contract=off -fno-math-errno -fno-trapping-math -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static-libstdc++ -static-libgcc -pthread -DRV_CAPTURE_FOV=6 -I"$root" -I"$root/common" "$root/native/runtime/main.cpp" -ldl -o "$out/native-fov6.bin"
"${prefix}strip" "$out/native-fov6.bin"
