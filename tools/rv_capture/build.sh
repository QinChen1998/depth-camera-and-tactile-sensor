#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
out=${1:?Pass a new output directory}
variant=${2:-native}
prefix=${RV1103_CROSS_PREFIX:-/opt/rv1103-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-}
flags=()
libraries=(-ldl)
case "$variant" in
 native) name=native-fov6.bin;;
 litesr)
  private=${3:?Pass the private LiteSR build input directory}
  sdk=${RKNN_SDK:-/opt/rv1103-sdk/rknn-2.3.2}
  test -f "$private/model_key_material.h" && test -f "$private/libcrypto.so.1.1" && test -f "$private/librknnmrt.so"
  flags=(-DRV_CAPTURE_LITESR=1 -I"$private" -I"$sdk/include")
  libraries+=(-L"$private" -lrknnmrt -l:libcrypto.so.1.1)
  name=litesr-fov6.bin;;
 *) echo 'Expected native or litesr' >&2; exit 2;;
esac
mkdir "$out"
"${prefix}g++" -std=c++17 -O3 -ffp-contract=off -fno-math-errno -fno-trapping-math -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static-libstdc++ -static-libgcc -pthread -DRV_CAPTURE_FOV=6 -I"$root" -I"$root/common" "${flags[@]}" "$root/native/runtime/main.cpp" "${libraries[@]}" -o "$out/$name"
"${prefix}strip" "$out/$name"
