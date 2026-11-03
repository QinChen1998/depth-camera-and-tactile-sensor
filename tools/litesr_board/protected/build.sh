#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")" && pwd)
private=${1:?Pass the private build directory with Key.h and the board libcrypto.so.1.1}
out=${2:?Pass an output directory for the FOV executables}
sdk=${RKNN_SDK:-/opt/rv1103-sdk/rknn-2.3.2}
prefix=${RV1103_CROSS_PREFIX:-/opt/rv1103-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-}
mkdir -p "$out"
for fov in 1 2; do
 "${prefix}g++" -std=c++17 -O3 -ffp-contract=off -fno-math-errno -fno-trapping-math -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
  -static-libstdc++ -static-libgcc -pthread -DLITESR_NEAR_MODEL=1 -DLITESR_NATIVE_FOV="$fov" \
  -I"$root/.." -I"$root/runtime" -I"$sdk/include" -I"$private" "$root/runtime/main.cpp" \
  -L"$sdk/lib" -L"$private" -lrknnmrt -l:libcrypto.so.1.1 -ldl -o "$out/litesr-fov$fov.bin"
 "${prefix}strip" "$out/litesr-fov$fov.bin"
done
