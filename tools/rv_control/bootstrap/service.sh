#!/bin/sh
set -eu
ulimit -c 0
[ "$(sed -n 's/^Serial[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo)" = "$(cat /root/r144_depth/spi_native16.enabled)" ] || exit 2
export TMF8829_I2C_DEV=/dev/i2c-3 TMF8829_SPI_DEV=/dev/spidev0.0 TMF8829_SPI_HZ=5000000
export TMF8829_ENABLE_GPIO=54 TMF8829_CS_GPIO=54
export TMF_HOST_READBACK=/run/dtof_candidates/sensor_readback.json
root=/root/r144_depth
state=/run/r144_depth
lock=/run/r144_fusion_20260923
model=${1:-Native}
rgb_width=${2:-576}
format=${3:-nv12}
export DEPTH_OUTPUT_WIDTH=${4:-144}
export DEPTH_OUTPUT_LOCATION=${5:-none}
export DEPTH_OUTPUT_METHOD=${6:-none}
case "$rgb_width:$format" in 144:nv12|576:nv12|2304:rtsp_h265) ;; *) echo 'Unsupported camera transport' >&2; exit 2;; esac
export DEPTH_RGB_FORMAT="$format"
case "$rgb_width" in 144|576|2304) ;; *) echo "Unsupported RGB width" >&2; exit 2;; esac
export DEPTH_RGB_WIDTH="$rgb_width"
[ "$model" = Native ] || { echo 'Unsupported capture identity' >&2;exit 2; }
[ "$DEPTH_OUTPUT_WIDTH:$DEPTH_OUTPUT_LOCATION:$DEPTH_OUTPUT_METHOD" = 144:none:none ] || exit 2
mkdir -p "$state"
cd "$root"
export CANONICAL_WARNING_POLICY=observe-0x08
live_app=./live
same(){ [ -r "/proc/$1/stat" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat | awk '{print $20}')" = "$2" ]; }
write_state(){ printf '{"state":"%s","model":"%s","generation":"%s","rgb_width":%s,"preview_format":"%s","manager_pid":%s,"exit_code":%s}\n' "$1" "$model" "$generation" "$rgb_width" "$format" "$$" "${2:-0}" > "$state/service.tmp";mv "$state/service.tmp" "$state/service.json"; }
generation=$(date +%s)$$
live_pid='';tof_pid='';live_start='';tof_start='';rtsp_pid='';rtsp_start='';monitor_pid='';monitor_start='';log_specs='';runtime=''
manager_start=$(sed 's/.*) //' /proc/$$/stat | awk '{print $20}')
manager_boot=$(cat /proc/sys/kernel/random/boot_id)
finish(){
 code=$?;trap - EXIT;trap '' TERM INT HUP
 for spec in "$monitor_pid:$monitor_start" "$live_pid:$live_start" "$tof_pid:$tof_start" "$rtsp_pid:$rtsp_start"; do
  pid=${spec%:*};born=${spec#*:}
  if [ -n "$pid" ] && same "$pid" "$born"; then
   kill -TERM "$pid" 2>/dev/null || true
   n=0;while same "$pid" "$born" && [ "$(sed 's/.*) //' /proc/"$pid"/stat | cut -d' ' -f1)" != Z ] && [ "$n" -lt 100 ]; do sleep .1;n=$((n+1));done
   if [ "$n" -eq 100 ]; then write_state cleanup_fault 1;touch "$state/fault";exit 1;fi
   wait "$pid" 2>/dev/null || true
  fi
 done
 # A signal can arrive between starting a FIFO reader and its writer. Bound that
 # wait too; only this manager's verified logger may be signalled.
 for spec in $log_specs; do
  pid=${spec%:*};born=${spec#*:};n=0
  while same "$pid" "$born" && [ "$(sed 's/.*) //' /proc/"$pid"/stat | cut -d' ' -f1)" != Z ] && [ "$n" -lt 20 ]; do sleep .1;n=$((n+1));done
  if same "$pid" "$born"; then kill -TERM "$pid" 2>/dev/null || true;fi
  wait "$pid" 2>/dev/null || true
 done
 if [ "$(cat "$lock/session.pid" 2>/dev/null)" != "$$" ] || [ "$(cat "$lock/session.starttime" 2>/dev/null)" != "$manager_start" ] || [ "$(cat "$lock/session.boot_id" 2>/dev/null)" != "$manager_boot" ]; then
  write_state cleanup_fault 1;touch "$state/fault";exit 1
 fi
 if [ "$code" -eq 0 ] && [ -n "$runtime" ]; then
  # These exact temporary files were created by this generation. Fault sessions
  # retain all evidence. Never recursively remove unexpected storage content.
  [ "$runtime" = "$state/rtsp_$generation" ] || { write_state cleanup_fault 1;touch "$state/fault";exit 1; }
  for directory in "$runtime/storage/video0" "$runtime/storage/video1" "$runtime/storage/video2" "$runtime/storage"; do
   if [ -d "$directory" ] && ! rmdir "$directory"; then write_state cleanup_fault 1;touch "$state/fault";exit 1;fi
  done
  rm -f "$runtime/camera.ini" "$runtime/route_guard.active" "$runtime/encoder.pid" "$runtime/encoder.start" "$runtime/monitor.log"
  rmdir "$runtime" || { write_state cleanup_fault 1;touch "$state/fault";exit 1; }
 fi
 if [ "$code" -eq 0 ]; then write_state stopped;else write_state fault "$code";touch "$state/fault";fi
 rm -f "$lock/session.pid" "$lock/session.starttime" "$lock/session.boot_id" "$lock/session.generation"
 rmdir "$lock"
 exit "$code"
}
[ ! -e "$state/fault" ] || exit 1
mkdir "$lock" || exit 1
trap finish EXIT
trap 'exit 0' TERM INT HUP
echo $$ > "$lock/session.pid"
echo "$manager_start" > "$lock/session.starttime"
echo "$manager_boot" > "$lock/session.boot_id"
echo "$generation" > "$lock/session.generation"
write_state starting
printf 'null\n' > "$state/telemetry.tmp"
mv "$state/telemetry.tmp" "$state/telemetry.json"
for name in rkipc live tmf8829_ext sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline; do
 if pidof "$name" >/dev/null;then write_state conflicting_owner 1;exit 1;fi
done
n=0;while [ ! -c /dev/video11 ] || { [ "$rgb_width" != 2304 ] && [ ! -c /dev/i2c-3 ]; };do n=$((n+1));[ "$n" -lt 45 ] || exit 1;sleep 1;done
if [ "$rgb_width" = 2304 ]; then
 # No model, live process, ToF process or NV12 server exists in this mode.
 [ "$DEPTH_OUTPUT_WIDTH:$DEPTH_OUTPUT_LOCATION:$DEPTH_OUTPUT_METHOD" = 144:none:none ] || exit 2
 runtime="$state/rtsp_$generation"
 umask 077
 mkdir "$runtime"
 mkdir "$runtime/storage"
 awk -v runtime="$runtime" -f "$root/camera_rtsp_config.awk" /userdata/rkipc.ini > "$runtime/camera.ini"
 [ -p "$state/rtsp.pipe" ] || mkfifo "$state/rtsp.pipe"
 awk -v base="$state/rtsp.log" -f "$root/bounded_log.awk" < "$state/rtsp.pipe" &
 log_pid=$!;log_start=$(sed 's/.*) //' /proc/$log_pid/stat | awk '{print $20}');log_specs="$log_specs $log_pid:$log_start"
 DEPTH_RTSP_GUARD_FILE="$runtime/route_guard.active" LD_PRELOAD="$root/no_route_monitor.so" LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib /oem/usr/bin/rkipc -c "$runtime/camera.ini" -a /oem/usr/share/iqfiles -l 1 > "$state/rtsp.pipe" 2>&1 &
 rtsp_pid=$!;rtsp_start=$(sed 's/.*) //' /proc/$rtsp_pid/stat | awk '{print $20}')
 echo "$rtsp_pid" > "$runtime/encoder.pid"
 echo "$rtsp_start" > "$runtime/encoder.start"
 "$root/camera_rtsp_monitor" "$rtsp_pid" "$rtsp_start" "$generation" "$model" "$runtime" "$state" > "$runtime/monitor.log" 2>&1 &
 monitor_pid=$!;monitor_start=$(sed 's/.*) //' /proc/$monitor_pid/stat | awk '{print $20}')
 write_state running
 wait "$monitor_pid"
 # The monitor is persistent; its unsolicited exit must latch a fault, even exit 0.
 exit 1
fi
mkdir -p /run/extrinsic_capture
for name in tof; do
 pipe="$state/$name.pipe"
 [ -p "$pipe" ] || mkfifo "$pipe"
 awk -v base="$state/$name.log" -f "$root/bounded_log.awk" < "$pipe" &
 log_pid=$!;log_start=$(sed 's/.*) //' /proc/$log_pid/stat | awk '{print $20}');log_specs="$log_specs $log_pid:$log_start"
done
# Persistent user-selected 48x32 standard preset (0x47), dual_mode=0.
# Native dimensions stay 48x32.
# Preserve the actual device acquisition settings; no model is loaded.
CANONICAL_RUN_ID="$generation" ./tmf8829_ext -m -g -s 100  -b 1 -d 11 -p 66 -t 0 -o 1 -r 6 -i 600 --socket /run/extrinsic_capture/tof.sock --quiet --poll-us 1000 >"$state/tof.pipe" 2>&1 &
tof_pid=$!;tof_start=$(sed 's/.*) //' /proc/$tof_pid/stat | awk '{print $20}')
sleep 2
same "$tof_pid" "$tof_start" || exit 1
[ -p "$state/live.pipe" ] || mkfifo "$state/live.pipe"
awk -v base="$state/live.log" -f "$root/bounded_log.awk" < "$state/live.pipe" &
log_pid=$!;log_start=$(sed 's/.*) //' /proc/$log_pid/stat | awk '{print $20}');log_specs="$log_specs $log_pid:$log_start"
CANONICAL_RUN_ID="$generation" LD_LIBRARY_PATH="$root:/oem/usr/lib" "$live_app" M48 "$model" 0 "$generation" both >"$state/live.pipe" 2>&1 &
live_pid=$!;live_start=$(sed 's/.*) //' /proc/$live_pid/stat | awk '{print $20}')
write_state running
wait "$live_pid"

