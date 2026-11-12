#!/bin/sh
set -eu
ulimit -c 0
[ "$(sed -n 's/^Serial[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo)" = "$(cat /root/r144_depth/spi_native16.enabled)" ] || exit 2
export TMF8829_I2C_DEV=/dev/i2c-3 TMF8829_SPI_DEV=/dev/spidev0.0 TMF8829_SPI_HZ=5000000
export TMF8829_ENABLE_GPIO=54 TMF8829_CS_GPIO=54
export TMF_HOST_READBACK=/run/dtof_candidates/sensor_readback.json
case "${1:-}" in 14|24|25) ;; *) exit 2;; esac
root=/root/dtof_candidates_20261009
state=/run/dtof_candidates
owner=/run/r144_fusion_20260923
producer='';server='';logger='';sensor_logger='';encoder='';encoder_logger='';hd_runtime='';owned=0
generation=$2
same(){ [ -r "/proc/$1/stat" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat | awk '{print $20}')" = "$2" ]; }
sensor_fault(){ printf '%s\n' sensor-readback > "$state/fault"; }
terminate(){
 if [ -n "$1" ] && same "$1" "$2"; then
  kill -TERM "$1" 2>/dev/null || true
  n=0
  while same "$1" "$2" && [ "$(sed 's/.*) //' /proc/"$1"/stat | cut -d' ' -f1)" != Z ]; do n=$((n+1));[ "$n" -le 70 ] || { touch "$state/fault";return 2; };sleep .1;done
  wait "$1" 2>/dev/null || true
 fi
}
born=$(sed 's/.*) //' /proc/$$/stat | awk '{print $20}')
boot=$(cat /proc/sys/kernel/random/boot_id)
cleanup(){
 code=$?;incomplete=0;trap - EXIT;trap '' TERM INT HUP
 if [ "$code" = 0 ] && [ -n "$producer" ] &&
    { ! same "$producer" "$producer_born" ||
      [ "$(sed 's/.*) //' "/proc/$producer/stat" 2>/dev/null | cut -d' ' -f1)" = Z ]; }; then
  sensor_fault;code=2
 fi
 terminate "$server" "${server_born:-}" || { code=2;incomplete=1; }
 terminate "$producer" "${producer_born:-}" || { code=2;incomplete=1; }
 terminate "$encoder" "${encoder_born:-}" || { code=2;incomplete=1; }
 terminate "$encoder_logger" "${encoder_logger_born:-}" || { code=2;incomplete=1; }
 terminate "$logger" "${logger_born:-}" || { code=2;incomplete=1; }
 terminate "$sensor_logger" "${sensor_logger_born:-}" || { code=2;incomplete=1; }
 [ "$incomplete" = 0 ] || exit 2
 if [ -n "$hd_runtime" ] && [ -d "$hd_runtime" ] && [ "$code" = 0 ]; then
  for directory in "$hd_runtime/storage/video0" "$hd_runtime/storage/video1" "$hd_runtime/storage/video2" "$hd_runtime/storage";do [ ! -d "$directory" ] || rmdir "$directory" || exit 2;done
  for file in camera.ini encoder.pid encoder.start route_guard.active encoder.pipe sensor.pipe server.pipe;do
   [ ! -e "$hd_runtime/$file" ] || rm "$hd_runtime/$file"
  done
  rmdir "$hd_runtime" || exit 2
 fi
 if [ "$owned" = 1 ]; then
  [ "$(cat "$owner/session.pid")" = "$$" ] && [ "$(cat "$owner/session.starttime")" = "$born" ] && [ "$(cat "$owner/session.boot_id")" = "$boot" ] || exit 2
  rm "$owner/session.pid" "$owner/session.starttime" "$owner/session.boot_id" "$owner/session.generation"
  rmdir "$owner"
 fi
 rm -f "$state/ready.json"
 if [ "$owned" = 1 ] && [ "$code" = 0 ] && [ ! -e "$state/fault" ]; then
  receipt=$(mktemp "$state/released.XXXXXX")
  printf '%s %s\n' "$boot" "$generation" > "$receipt"
  mv -f "$receipt" "$state/released"
 fi
 exit "$code"
}
trap cleanup EXIT
trap 'exit 0' TERM INT HUP
echo $$ > "$state/manager.pid"
echo "$born" > "$state/manager.starttime"
mkdir "$owner"
echo $$ > "$owner/session.pid"
echo "$born" > "$owner/session.starttime"
echo "$boot" > "$owner/session.boot_id"
echo "$2" > "$owner/session.generation"
owned=1
mkdir -p /run/extrinsic_capture
cd /root/r144_depth
available=$(awk '/MemAvailable:/ {print $2}' /proc/meminfo)
[ "$available" -ge 8192 ] || { echo 'Imaging startup needs at least 8192 KiB before capture' >&2;exit 2; }
{
 # One camera owner: only rkipc opens VI/AIQ; the capture server emits native measurements.
 hd_runtime="$state/hd_$2"
 umask 077
 mkdir "$hd_runtime" "$hd_runtime/storage"
 awk -v runtime="$hd_runtime" -f /root/r144_depth/camera_rtsp_config.awk /userdata/rkipc.ini > "$hd_runtime/camera.ini"
 # A half-frame line buffer gives the encoder DDR scheduling headroom during native capture.
 # Change only this generation's private video.source section, never the OEM ini.
 awk '/^\[/ {source=($0=="[video.source]")} source && /^buffer_line = / {$0="buffer_line = 648";count++} {print} END {if(count!=1)exit 2}' "$hd_runtime/camera.ini" > "$hd_runtime/camera.hd.ini"
 mv "$hd_runtime/camera.hd.ini" "$hd_runtime/camera.ini"
 mkfifo "$hd_runtime/encoder.pipe"
 awk -v base="$state/encoder.log" -f /root/r144_depth/bounded_log.awk < "$hd_runtime/encoder.pipe" &
 encoder_logger=$!;encoder_logger_born=$(sed 's/.*) //' /proc/$encoder_logger/stat | awk '{print $20}')
 DEPTH_RTSP_GUARD_FILE="$hd_runtime/route_guard.active" LD_PRELOAD=/root/r144_depth/no_route_monitor.so LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib /oem/usr/bin/rkipc -c "$hd_runtime/camera.ini" -a /oem/usr/share/iqfiles -l 1 > "$hd_runtime/encoder.pipe" 2>&1 &
 encoder=$!;encoder_born=$(sed 's/.*) //' /proc/$encoder/stat | awk '{print $20}')
 echo "$encoder" > "$hd_runtime/encoder.pid";echo "$encoder_born" > "$hd_runtime/encoder.start"
}
 mkfifo "$hd_runtime/sensor.pipe"
 awk -v base="$state/sensor.log" -f /root/r144_depth/bounded_log.awk < "$hd_runtime/sensor.pipe" &
 sensor_logger=$!;sensor_logger_born=$(sed 's/.*) //' /proc/$sensor_logger/stat | awk '{print $20}')
sensor_output="$hd_runtime/sensor.pipe"
# Preserve prompt FIFO reads while encoding and native transport overlap. This is CFS
# nice, not a realtime policy; only the shared HD capture and its threads inherit it.
launch_tof(){
 shift
 exec nice -n -5 "$@"
}
CANONICAL_RUN_ID="dtof_$2" CANONICAL_WARNING_POLICY=observe-0x08 launch_tof "$1" ./tmf8829_ext -m -s 100 -g -b 1 -d 11 -p 66 -t 0 -o 1 -r 6 -i 600 --socket /run/extrinsic_capture/tof.sock --quiet --poll-us 1000 > "$sensor_output" 2>&1 &
producer=$!;producer_born=$(sed 's/.*) //' /proc/$producer/stat | awk '{print $20}')
sleep 5
same "$producer" "$producer_born" || { sensor_fault; exit 2; }
rm -f "$state/server.log.0" "$state/server.log.1"
mkfifo "$hd_runtime/server.pipe"
awk -v base="$state/server.log" -f /root/r144_depth/bounded_log.awk < "$hd_runtime/server.pipe" &
logger=$!;logger_born=$(sed 's/.*) //' /proc/$logger/stat | awk '{print $20}')
"$root/native_gui.sh" serve "$@" > "$hd_runtime/server.pipe" 2>&1 &
server=$!;server_born=$(sed 's/.*) //' /proc/$server/stat | awk '{print $20}')
code=0;wait "$server" || code=$?
if [ "$code" != 0 ]; then
 wait "$logger" || true
 if grep -Eq '^(Native ToF fault:|Native IPC connection|Native sensor grid changed|Native grid differs)' \
    "$state/server.log.0" "$state/server.log.1" 2>/dev/null; then sensor_fault; fi
 exit "$code"
fi
