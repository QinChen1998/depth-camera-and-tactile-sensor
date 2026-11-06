#!/bin/sh
# DCTS protected LiteSR + Tiny NV12 v5 depth-only384
set -eu
root=/root/dtof_candidates_20261009
export LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib
[ "$#" = 4 ] || exit 2
action=$1; shift
case "$action" in validate|serve) ;; *) exit 2;; esac
case "$1" in
 16)
   binary="$root/dcts-litesr-384x256-v3-depth-only/litesr_near_gui"
   model="$root/dcts-litesr-384x256-v3-depth-only/model.dctsm"
   digest(){ sha256sum "$1" | cut -d' ' -f1; }
   [ ! -L "$binary" ] && [ ! -L "$model" ] &&
   [ "$(digest "$binary")" = '@RECT_BINARY_SHA@' ] && [ "$(digest "$model")" = '@RECT_MODEL_SHA@' ] ||
      { echo 'Protected LiteSR payload missing or damaged' >&2; exit 2; }
   ;;
 18)
   binary="$root/dcts-tiny-nv12-128x80-v1/tiny_nv12"
   model="$root/dcts-tiny-nv12-128x80-v1/model.dctsm"
   camera="$root/dcts-tiny-nv12-128x80-v1/tiny_nv12_camera"
   digest(){ sha256sum "$1" | cut -d' ' -f1; }
   [ ! -L "$binary" ] && [ ! -L "$model" ] && [ ! -L "$camera" ] &&
   [ "$(digest "$binary")" = '@TINY_BINARY_SHA@' ] && [ "$(digest "$model")" = '@TINY_MODEL_SHA@' ] && [ "$(digest "$camera")" = '@TINY_CAMERA_SHA@' ] ||
      { echo 'Protected Tiny NV12 payload missing or damaged' >&2; exit 2; }
   ;;
 14) binary="$root/native_hd_gui"; model='';;
 *) exit 2;;
esac
"$binary" validate "$@"
if [ "$1" = 14 ]; then
for file in /root/r144_depth/camera_rtsp_config.awk /root/r144_depth/no_route_monitor.so /oem/usr/bin/rkipc /userdata/rkipc.ini; do
 [ -s "$file" ] || { echo 'Missing managed H.265 asset' >&2; exit 2; }
done
fi
[ "$action" = serve ] || exit 0
if [ "$1" = 18 ]; then exec "$binary" "$2" "$root/dcts-tiny-nv12-128x80-v1"; fi
exec "$binary" gui "$model" "$@"
