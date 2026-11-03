#!/bin/sh
# Exclusive, leased ToF experiment. Faults never auto-resume production.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
state=/run/dtof_tuner
owner=/run/r144_fusion_20260923
[ "$#" = 9 ] || exit 2
token=$1; mode=$2; iterations=$3; short=$4; snr=$5; period=$6; objects=$7
signal=$8; noise=$9
case "$token" in ''|*[!a-f0-9]*) exit 2;; esac
[ "${#token}" = 32 ] || exit 2
for number in "$mode" "$iterations" "$short" "$snr" "$period" "$objects" "$signal" "$noise"; do
 case "$number" in ''|*[!0-9]*) exit 2;; esac
done
[ "$mode" -le 13 ] && [ "$iterations" -ge 100 ] && [ "$iterations" -le 10000 ] &&
 [ "$short" -ge 1 ] && [ "$short" -le 10000 ] && [ "$snr" -ge 4 ] && [ "$snr" -le 255 ] &&
 [ "$period" -ge 20 ] && [ "$period" -le 2000 ] && [ "$objects" -ge 1 ] && [ "$objects" -le 4 ] &&
 [ "$signal" -le 1 ] && [ "$noise" -le 1 ] || exit 2
if [ "$mode" = 13 ] && [ "$objects" = 4 ]; then exit 2; fi
if [ "$mode" -ge 11 ] && [ "$objects" = 4 ] && [ "$signal" = 1 ] && [ "$noise" = 1 ]; then exit 2; fi
mkdir -p "$state"
[ ! -e /run/r144_depth/fault ] && [ ! -e /run/rgbtof_calibration_v2/fault ] && [ ! -e "$state/fault" ] || exit 1
mkdir "$owner" || exit 1
start_of(){ sed 's/.*) //' /proc/"$1"/stat 2>/dev/null | awk '{print $20}'; }
alive(){ [ -r /proc/"$1"/stat ] && [ "$(start_of "$1")" = "$2" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat | cut -d' ' -f1)" != Z ]; }
born=$(start_of $$); boot=$(cat /proc/sys/kernel/random/boot_id)
echo $$ > "$owner/session.pid"; echo "$born" > "$owner/session.starttime"
echo "$boot" > "$owner/session.boot_id"; echo "dtof-$token" > "$owner/session.generation"
echo "$token" > "$state/token"; echo starting > "$state/status"
tof_pid=''; tof_start=''; log_pid=''; log_start=''
finish(){
 code=$?; trap - EXIT; trap '' TERM INT HUP
 if [ -n "$tof_pid" ] && alive "$tof_pid" "$tof_start"; then
  kill -TERM "$tof_pid" || true
  n=0
  while alive "$tof_pid" "$tof_start"; do
   n=$((n+1)); [ "$n" -lt 100 ] || { touch "$state/fault"; echo cleanup_fault > "$state/status"; exit 1; }
   sleep .1
  done
  wait "$tof_pid" 2>/dev/null || true
 fi
 if [ -n "$log_pid" ]; then
  n=0
  while alive "$log_pid" "$log_start"; do
   n=$((n+1)); [ "$n" -lt 30 ] || { touch "$state/fault"; echo cleanup_fault > "$state/status"; exit 1; }
   sleep .1
  done
  wait "$log_pid" 2>/dev/null || true
 fi
 # Require evidence that the sensor STOP was acknowledged, not just PID exit.
 if [ -n "$tof_pid" ]; then
  grep -q stop_command_acknowledged "$state/tof.log.0" "$state/tof.log.1" &&
   ! grep -q 'TOF_WARNING\|stop_command_failed' "$state/tof.log.0" "$state/tof.log.1" || code=1
 fi
 if [ "$(cat "$owner/session.pid")" != "$$" ] || [ "$(cat "$owner/session.starttime")" != "$born" ] ||
    [ "$(cat "$owner/session.boot_id")" != "$boot" ] || [ "$(cat "$owner/session.generation")" != "dtof-$token" ]; then
  touch "$state/fault"; echo cleanup_fault > "$state/status"; exit 1
 fi
 if [ "$code" != 0 ]; then touch "$state/fault"; echo fault > "$state/status"; else echo stopped > "$state/status"; fi
 rm "$owner/session.pid" "$owner/session.starttime" "$owner/session.boot_id" "$owner/session.generation"
 rmdir "$owner"
 if [ "$code" = 0 ]; then rm -f "$state/output.$token.fifo" "$state/ready.$token"; fi
 exit "$code"
}
trap finish EXIT
trap 'exit 0' TERM INT HUP
for name in live tmf8829_ext tmf8829_tuner rkipc sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline; do
 ! pidof "$name" >/dev/null || exit 1
done
export CANONICAL_WARNING_POLICY=observe-0x08 CANONICAL_RUN_ID="$token"
export DTOF_TUNER_TOKEN="$token" DTOF_TUNER_SETTINGS="$mode $iterations $short $snr $period $objects $signal $noise"
# Two bounded 64 KiB RAM slots; no accumulating flash logs or histograms.
printf '' > "$state/tof.log.0"; printf '' > "$state/tof.log.1"
mkfifo "$state/output.$token.fifo"
awk -v base="$state/tof.log" -f "$root/bounded_log.awk" < "$state/output.$token.fifo" &
log_pid=$!; log_start=$(start_of "$log_pid")
set -- -m -b 0 -d "$mode" -p "$period" -t 0 -o "$objects" -r "$snr" -i "$iterations" -s "$short"
if [ "$signal" = 1 ]; then set -- "$@" -g; fi
if [ "$noise" = 1 ]; then set -- "$@" -n; fi
"$root/tmf8829_tuner" "$@" --socket 8833 --quiet --poll-us 1000 > "$state/output.$token.fifo" 2>&1 &
tof_pid=$!; tof_start=$(start_of "$tof_pid")
echo "$tof_pid $tof_start" > "$state/tof.identity"
n=0
while [ ! -f "$state/ready.$token" ]; do
 alive "$tof_pid" "$tof_start" && alive "$log_pid" "$log_start" || exit 1
 ! grep -q TOF_WARNING "$state/tof.log.0" "$state/tof.log.1" || exit 1
 n=$((n+1)); [ "$n" -lt 200 ] || exit 1
 sleep .1
done
echo running > "$state/status"
while alive "$tof_pid" "$tof_start" && alive "$log_pid" "$log_start"; do
 ! grep -q TOF_WARNING "$state/tof.log.0" "$state/tof.log.1" || exit 1
 now=$(cut -d. -f1 /proc/uptime); beat=$(cat "$state/heartbeat")
 case "$beat" in ''|*[!0-9]*) echo 'FAULT: malformed heartbeat' >&2; exit 1;; esac
 [ "$beat" -le "$now" ] && [ $((now-beat)) -le 45 ] || { echo 'FAULT: expired heartbeat' >&2; exit 1; }
 sleep 1
done
echo 'FAULT: sensor or logger exited' >&2
exit 1
