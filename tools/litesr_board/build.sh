#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
variant=${1:?Use native or protected384}
private=${2:?Pass private Key.h/libcrypto directory; use - for native capture}
out=${3:?Pass a new output directory}
case "$variant" in native|protected384) ;; *) printf 'Unknown runtime: %s\n' "$variant" >&2; exit 2;; esac
sdk=${RKNN_SDK:-/opt/rv1103-sdk/rknn-2.3.2}
prefix=${RV1103_CROSS_PREFIX:-/opt/rv1103-sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-}
flags=()
libraries=(-L"$sdk/lib" -lrknnmrt -ldl)
name=litesr
if [[ "$variant" == native ]]; then
  flags+=(-DLITESR_CAPTURE_ONLY=1)
  name=native
else
  test -f "$private/Key.h" && test -f "$private/libcrypto.so.1.1"
  flags+=(-I"$private")
  libraries+=(-L"$private" -l:libcrypto.so.1.1)
fi
mkdir "$out"
for fov in 1 2; do
  "${prefix}g++" -std=c++17 -O3 -ffp-contract=off -fno-math-errno -fno-trapping-math -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -static-libstdc++ -static-libgcc -pthread -DLITESR_NEAR_MODEL=1 -DLITESR_NATIVE_FOV="$fov" \
    -I"$root" -I"$root/common" -I"$root/$variant/runtime" -I"$sdk/include" "${flags[@]}" \
    "$root/$variant/runtime/main.cpp" "${libraries[@]}" -o "$out/$name-fov$fov.bin"
  "${prefix}strip" "$out/$name-fov$fov.bin"
done
