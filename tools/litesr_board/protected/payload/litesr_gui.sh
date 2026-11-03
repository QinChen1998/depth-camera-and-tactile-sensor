#!/bin/sh
# DCTS protected LiteSR v2
set -eu
root=/root/dtof_candidates_20261009
export LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib
[ "$#" = 4 ] || exit 2
action=$1; shift
case "$action" in validate|serve) ;; *) exit 2;; esac
case "$1" in
 13)
   binary="$root/dcts-litesr-protected-v2/litesr_near_gui"
   model="$root/dcts-litesr-protected-v2/model.dctsm"
   digest(){ sha256sum "$1" | cut -d' ' -f1; }
   [ ! -L "$binary" ] && [ ! -L "$model" ] &&
   [ "$(digest "$binary")" = '@BINARY_SHA@' ] && [ "$(digest "$model")" = '@MODEL_SHA@' ] ||
      { echo 'Protected LiteSR payload missing or damaged' >&2; exit 2; }
   ;;
 14) binary="$root/native_hd_gui"; model='';;
 *) exit 2;;
esac
"$binary" validate "$@"
for file in /root/r144_depth/camera_rtsp_config.awk /root/r144_depth/no_route_monitor.so /oem/usr/bin/rkipc /userdata/rkipc.ini; do
 [ -s "$file" ] || { echo 'Missing managed H.265 asset' >&2; exit 2; }
done
[ "$action" = serve ] || exit 0
exec "$binary" gui "$model" "$@"
