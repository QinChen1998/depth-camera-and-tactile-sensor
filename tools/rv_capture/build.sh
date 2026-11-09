#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
out=${1:?Pass a new output directory}
prefix=${RV1103_CROSS_PREFIX:-/opt/rv1103-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-}
mkdir "$out"
for fov in 6; do
 "${prefix}g++" -std=c++17 -O3 -ffp-contract=off -fno-math-errno -fno-trapping-math -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static-libstdc++ -static-libgcc -pthread -DRV_CAPTURE_FOV="$fov" -I"$root" -I"$root/common" "$root/native/runtime/main.cpp" -ldl -o "$out/native-fov$fov.bin"
 "${prefix}strip" "$out/native-fov$fov.bin"
done
