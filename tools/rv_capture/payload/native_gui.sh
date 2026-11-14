#!/bin/sh
# DCTS native dToF and H.265 capture; all depth inference runs on the host.
set -eu
root=/root/dtof_candidates_20261009
export LD_LIBRARY_PATH=/root/r144_depth:/oem/usr/lib
[ "$#" = 4 ] || exit 2
action=$1; shift
case "$action" in validate|serve) ;; *) exit 2;; esac
case "$1" in 14|24|25) ;; *) exit 2;; esac
binary="$root/native_hd_gui"
"$binary" validate "$@"
for file in /root/r144_depth/camera_rtsp_config.awk /root/r144_depth/no_route_monitor.so /oem/usr/bin/rkipc /userdata/rkipc.ini; do
 [ -s "$file" ] || { echo 'Missing managed H.265 asset' >&2; exit 2; }
done
[ "$action" = serve ] || exit 0
# The capture binary expects a legacy argument before the mode.
exec "$binary" gui '' "$@"
