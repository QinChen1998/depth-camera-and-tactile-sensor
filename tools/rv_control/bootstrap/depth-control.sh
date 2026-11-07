#!/bin/sh
# Authenticated SSH/init entry point. All lifecycle mutations share this lock.
set -eu
root=/root/r144_depth
state=/run/r144_depth
owner=/run/r144_fusion_20260923
action=${1:-status}
case "$action" in status|stop) ;; *) echo "Use the candidate coordinator for imaging" >&2;exit 2;;esac
fail(){ echo "$*" >&2; exit 1; }
validate(){ [ "$1" = Native ] || fail 'Unsupported capture identity'; }
if [ "$action" = status ]; then
 cat "$state/service.json"; cat "$state/telemetry.json"; exit 0
fi
case "$action" in start|stop) [ "$#" -le 1 ] || fail 'Unexpected argument' ;;
 configure) { [ "$#" -eq 5 ] || [ "$#" -eq 6 ]; } || fail 'Usage: configure PREVIEW DEPTH LOCATION METHOD [MODEL]'; case "$2" in 144|576|2304) ;; *) fail 'Preview';; esac; if [ "$#" -eq 6 ]; then validate "$6"; fi ;;
 select) [ "$#" -eq 2 ] || fail 'Usage: control.sh select Native'; validate "$2" ;;
 format) [ "$#" -eq 2 ] || fail 'Usage: control.sh format nv12'; [ "$2" = nv12 ] || fail 'Unsupported format' ;;
 preview) [ "$#" -eq 2 ] || fail 'Usage: control.sh preview 144|576|2304'; case "$2" in 144|576|2304) ;; *) fail 'Unsupported preview width';; esac ;;
 rgb) [ "$#" -eq 2 ] || fail 'Usage: control.sh rgb 144|576'; case "$2" in 144|576) ;; *) fail 'Unsupported RGB width';; esac ;;
 *) fail 'Usage: control.sh start|stop|status|select MODEL|rgb WIDTH'  ;;
esac
mkdir -p "$state"
mkdir "$state/control.lock" 2>/dev/null || fail 'Control busy; retry after current operation completes'
trap 'rmdir "$state/control.lock"' EXIT
trap 'exit 1' TERM INT HUP
same(){ [ -r "/proc/$1/stat" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat 2>/dev/null | awk '{print $20}')" = "$2" ]; }
active(){
 p=$(cat "$state/manager.pid" 2>/dev/null || true)
 born=$(cat "$state/manager.start" 2>/dev/null || true)
 case "$p:$born" in *[!0-9:]*|:|:*|*:) return 1;; esac
 same "$p" "$born" && [ "$(sed 's/.*) //' /proc/"$p"/stat 2>/dev/null | cut -d' ' -f1)" != Z ]
}
verify_owner(){
 [ "$(cat "$owner/session.pid")" = "$p" ] || fail 'Manager owner mismatch'
 [ "$(cat "$owner/session.starttime")" = "$born" ] || fail 'Manager starttime mismatch'
 [ "$(cat "$owner/session.boot_id")" = "$(cat /proc/sys/kernel/random/boot_id)" ] || fail 'Manager boot mismatch'
 case "$(tr '\000' ' ' < /proc/"$p"/cmdline)" in
  *"sh $root/service.sh"*) ;; *) fail 'Manager command mismatch';;
 esac
}
stop_owned(){
 if active; then
  verify_owner
  kill -TERM "$p"
  n=0
  while same "$p" "$born" && [ "$(sed 's/.*) //' /proc/"$p"/stat 2>/dev/null | cut -d' ' -f1)" != Z ]; do
   n=$((n+1)); [ "$n" -le 150 ] || fail 'Stop timed out; ownership preserved'
   sleep .1
  done
 fi
 [ ! -e "$owner" ] || fail 'Capture lock remains; inspection required'
 [ -z "$(pidof live tmf8829_ext rkipc sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline || true)" ] || fail 'Another capture owner remains'
}
if [ "$action" = stop ]; then stop_owned; exit 0; fi
[ ! -e "$state/fault" ] || fail 'Fault latched; preserve logs and investigate before recovery'
current=$(cat "$state/selected_model" 2>/dev/null || echo Native)
model=Native
[ "$action" != select ] || model=$2
if [ "$action" = configure ] && [ "$#" -eq 6 ]; then model=$6; fi
validate "$model"
rgb_width=$(cat "$state/selected_rgb_width" 2>/dev/null || echo 576)
case "$action" in rgb|preview|configure) rgb_width=$2;; esac
case "$rgb_width" in 144|576|2304) ;; *) fail 'Unsupported RGB width';; esac
if [ "$rgb_width" = 576 ]; then [ -s "$root/preview576.bin" ] || fail 'Missing preview map'; fi
# Check assets before stopping the current healthy stream.
for file in live tmf8829_ext bounded_log.awk; do
 [ -s "$root/$file" ] || fail "Missing capture asset: $file"
done
format=$(cat "$state/selected_format" 2>/dev/null || echo nv12)
[ "$action" != format ] || format=$2
case "$action" in preview|configure) format=nv12;; esac
if [ "$rgb_width" = 2304 ]; then
 [ "$action" != format ] || fail '2304 uses H.265 RTSP; select 144/576 for NV12 or RGB'
 format=rtsp_h265
elif [ "$format" = rtsp_h265 ]; then
 format=nv12
fi
case "$format" in nv12|rgb|rtsp_h265) ;; *) fail 'Unsupported format';; esac
if [ "$format" = rtsp_h265 ]; then
 for file in camera_rtsp_config.awk camera_rtsp_monitor no_route_monitor.so; do
  [ -s "$root/$file" ] || fail "Missing RTSP asset: $file"
 done
 [ -x "$root/camera_rtsp_monitor" ] && [ -x /oem/usr/bin/rkipc ] || fail 'Missing RTSP executable'
 [ -s /userdata/rkipc.ini ] && [ -d /oem/usr/share/iqfiles ] || fail 'Missing RTSP camera configuration'
 [ -s /oem/usr/share/iqfiles/sc3336_CMK-OT2119-PC1_30IRC-F16.json ] || fail 'Missing camera IQ asset'
 [ -r /proc/vcodec/enc/venc_info ] || fail 'Missing real encoder progress interface'
 # Validate the private configuration before stopping a healthy fusion session.
 awk -v runtime="$state/rtsp_validation" -f "$root/camera_rtsp_config.awk" /userdata/rkipc.ini >/dev/null || fail 'Invalid RTSP camera configuration'
fi
depth_width=$(cat "$state/selected_depth_width" 2>/dev/null || echo 144)
depth_location=$(cat "$state/selected_depth_location" 2>/dev/null || echo none)
depth_method=$(cat "$state/selected_depth_method" 2>/dev/null || echo none)
if [ "$action" = configure ]; then depth_width=$3; depth_location=$4; depth_method=$5; fi
[ "$depth_width:$depth_location:$depth_method" = 144:none:none ] || fail 'Capture has no inferred depth'
current_depth="$(cat "$state/selected_depth_width" 2>/dev/null || echo 144):$(cat "$state/selected_depth_location" 2>/dev/null || echo none):$(cat "$state/selected_depth_method" 2>/dev/null || echo none)"
current_format=$(cat "$state/selected_format" 2>/dev/null || echo nv12)
current_rgb=$(cat "$state/selected_rgb_width" 2>/dev/null || echo 576)
if active; then
 verify_owner
 if [ "$current_depth" = "$depth_width:$depth_location:$depth_method" ] && [ "$current" = "$model" ] && [ "$current_rgb" = "$rgb_width" ] && [ "$current_format" = "$format" ]; then cat "$state/service.json"; exit 0; fi
fi
stop_owned
[ ! -e "$state/fault" ] || fail 'Fault occurred during stop; switch refused'
printf '%s\n' "$model" > "$state/selected_model.tmp"
mv "$state/selected_model.tmp" "$state/selected_model"
printf '%s\n' "$rgb_width" > "$state/selected_rgb_width.tmp"
mv "$state/selected_rgb_width.tmp" "$state/selected_rgb_width"
if ! pidof depth_status >/dev/null; then "$root/depth_status" </dev/null >/dev/null 2>&1 & fi
printf '%s\n' "$format" > "$state/selected_format.tmp"
mv "$state/selected_format.tmp" "$state/selected_format"
printf '%s\n' "$depth_width" > "$state/selected_depth_width"
printf '%s\n' "$depth_location" > "$state/selected_depth_location"
printf '%s\n' "$depth_method" > "$state/selected_depth_method"
nohup sh "$root/service.sh" "$model" "$rgb_width" "$format" "$depth_width" "$depth_location" "$depth_method" </dev/null >"$state/manager.log" 2>&1 &
p=$!
echo "$p" > "$state/manager.pid"
born=$(sed 's/.*) //' /proc/"$p"/stat | awk '{print $20}')
echo "$born" > "$state/manager.start"
# Retain the control lock until the service has published its own identity.
n=0
while [ "$(cat "$owner/session.pid" 2>/dev/null || true)" != "$p" ] || ! grep -q "\"manager_pid\":$p," "$state/service.json" 2>/dev/null; do
 same "$p" "$born" || fail 'Manager exited during startup; inspect logs'
 n=$((n+1)); [ "$n" -le 50 ] || fail 'Manager startup timed out'
 sleep .1
done
cat "$state/service.json"

