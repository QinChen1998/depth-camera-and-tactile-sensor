#!/bin/sh
# Dispatch the imaging generation to the matching board program.
set -eu
root=$(CDPATH='' cd "$(dirname "$0")" && pwd)
action=${1:-}
case "$action" in
 check-litesr) [ "$#" = 1 ] || exit 2;;
 validate|serve) [ "$#" = 4 ] || exit 2;;
 *) exit 2;;
esac
shift
if [ "$action" != check-litesr ]; then
 case "$1" in
  14|24|25) exec "$root/native_gui.sh" "$action" "$@";;
  13) ;;
  *) exit 2;;
 esac
fi
binary="$root/litesr_near_gui"
model="$root/litesr-near-256.dctsm"
[ -s "$model" ] && [ -s "$root/librknnmrt.so" ] && [ -s "$root/libcrypto.so.1.1" ] || {
 echo 'Missing protected LiteSR model or runtime library' >&2; exit 2;
}
export LD_LIBRARY_PATH="$root:/root/r144_depth:/oem/usr/lib:/usr/lib"
if [ "$action" = check-litesr ]; then
 exec "$binary" --check-model "$model"
fi
"$binary" validate "$@"
for file in /root/r144_depth/camera_rtsp_config.awk /root/r144_depth/no_route_monitor.so /oem/usr/bin/rkipc /userdata/rkipc.ini; do
 [ -s "$file" ] || { echo 'Missing managed H.265 asset' >&2; exit 2; }
done
[ "$action" = serve ] || exit 0
exec "$binary" gui "$model" "$@"
