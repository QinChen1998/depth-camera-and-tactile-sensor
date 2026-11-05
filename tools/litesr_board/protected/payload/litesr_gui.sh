#!/bin/sh
# DCTS protected LiteSR v3-info10
set -eu
root=/root/dtof_candidates_20261009
export LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib
[ "$#" = 4 ] || exit 2
action=$1; shift
case "$action" in validate|serve) ;; *) exit 2;; esac
case "$1" in
 13)
   binary="$root/dcts-litesr-protected-v3-info10/litesr_near_gui"
   model="$root/dcts-litesr-protected-v3-info10/model.dctsm"
   digest(){ sha256sum "$1" | cut -d' ' -f1; }
   [ ! -L "$binary" ] && [ ! -L "$model" ] &&
   [ "$(digest "$binary")" = '@BINARY_SHA@' ] && [ "$(digest "$model")" = '@MODEL_SHA@' ] ||
      { echo 'Protected LiteSR payload missing or damaged' >&2; exit 2; }
   ;;
 16)
   binary="$root/dcts-litesr-384x256-v2-info10/litesr_near_gui"
   model="$root/dcts-litesr-384x256-v2-info10/model.dctsm"
   digest(){ sha256sum "$1" | cut -d' ' -f1; }
   [ ! -L "$binary" ] && [ ! -L "$model" ] &&
   [ "$(digest "$binary")" = '@RECT_BINARY_SHA@' ] && [ "$(digest "$model")" = '@RECT_MODEL_SHA@' ] ||
      { echo 'Protected LiteSR payload missing or damaged' >&2; exit 2; }
   ;;
 17)
   binary="$root/dcts-litesr-128x128-v1/litesr_near_gui"
   model="$root/dcts-litesr-128x128-v1/model.dctsm"
   digest(){ sha256sum "$1" | cut -d' ' -f1; }
   [ ! -L "$binary" ] && [ ! -L "$model" ] &&
   [ "$(digest "$binary")" = '@NATIVE16_BINARY_SHA@' ] && [ "$(digest "$model")" = '@NATIVE16_MODEL_SHA@' ] ||
      { echo 'Protected Native16 LiteSR payload missing or damaged' >&2; exit 2; }
   ;;
 14) binary="$root/native_hd_gui"; model='';;
 *) exit 2;;
esac
"$binary" validate "$@"
if [ "$1" != 17 ]; then
for file in /root/r144_depth/camera_rtsp_config.awk /root/r144_depth/no_route_monitor.so /oem/usr/bin/rkipc /userdata/rkipc.ini; do
 [ -s "$file" ] || { echo 'Missing managed H.265 asset' >&2; exit 2; }
done
fi
[ "$action" = serve ] || exit 0
exec "$binary" gui "$model" "$@"
