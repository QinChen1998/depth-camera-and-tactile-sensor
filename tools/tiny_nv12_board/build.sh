#!/bin/bash
set -euo pipefail
[ "$#" = 2 ] || { echo "usage: build.sh PRIVATE NEW_OUTPUT" >&2; exit 2; }
private=$(realpath "$1"); output=$2
[ ! -e "$output" ] || { echo 'Output must be new' >&2; exit 2; }
mkdir "$output"; output=$(realpath "$output")
root=$(cd "$(dirname "$0")/runtime" && pwd)
sdk=${RKNN_SDK:-/opt/rv1103-sdk/rknn-2.3.2}
cross=${RV1103_CROSS_PREFIX:-/opt/rv1103-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-}
flags=(-std=c++17 -O3 -Wno-psabi -ffp-contract=off -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static-libstdc++ -static-libgcc)
"${cross}g++" "${flags[@]}" -DTINY_GEOMETRY_SEEDED=9 -DTINY_GEOMETRY_STRIPE_ROWS=4 -DTINY_HOLES_SMALL_SLOPE=1 -DTINY_PAIR_LIMIT_MS=50 -DTINY_PAIR_WAIT_MS=0 -DTINY_TOF_BUFFER=8 -I"$sdk/include" -I"$private" "$root/pipeline.cpp" -L"$sdk/lib" -L"$private" -lrknnmrt -l:libcrypto.so.1.1 -ldl -pthread -o "$output/tiny_nv12"
"${cross}g++" "${flags[@]}" -DRGBTOF_CAPTURE_BUFFERS=2 "$root/raw_camera.cpp" -ldl -o "$output/tiny_nv12_camera"
"${cross}strip" "$output/tiny_nv12" "$output/tiny_nv12_camera"
