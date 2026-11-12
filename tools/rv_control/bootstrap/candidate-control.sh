#!/bin/sh
# RV1103 candidate coordinator and verified clean-session recovery.
set -eu
root=/root/dtof_candidates_20261009
state=/run/dtof_candidates
lock=/run/dtof_switch.lock
owner=/run/r144_fusion_20260923
# The SSH worker reads marked diagnostics from stdout and suppresses stderr.
fail(){ printf 'DTOF_ERROR: %s\n' "$*"; exit 2; }
recovering=0
hex_generation(){ [ "${#1}" = 32 ] && case "$1" in *[!0-9a-f]*) return 1;; *) return 0;; esac; }
same(){ [ -r "/proc/$1/stat" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat | awk '{print $20}')" = "$2" ]; }
case "${1:-}" in
 capabilities) [ "$#" = 1 ] || exit 2; printf '%s\n' '{"candidate_stream_recovery":"clean-candidate-restart-v1"}'; exit 0;;
 candidate|auto-candidate)
  if [ "$1" = auto-candidate ]; then
   [ "$#" = 6 ] && hex_generation "$5" && [ "$5" != "$3" ] || fail 'Invalid previous capture generation'
   recovering=1;previous=$5;expected_boot=$6
  else
   [ "$#" = 4 ] || exit 2
  fi
  case "$2" in 14|24|25) ;; *) fail 'Unsupported native capture mode';; esac
  "$root/native_gui.sh" validate "$2" "$3" "$4";;
 legacy) [ "$#" = 6 ]; case "$2" in 144|576|2304) ;; *) exit 2;; esac
  case "$6" in Native) ;; *) exit 2;; esac
  case "$3:$4:$5" in
   144:none:none) ;;
   *) exit 2;;
  esac;;
 *) exit 2;;
esac
mkdir -p "$state"
n=0
until mkdir "$lock" 2>/dev/null; do
 n=$((n+1));[ "$recovering" = 0 ] && [ "$n" -le 200 ] || fail 'Another board capture transition holds the lifecycle lock'
 sleep .1
done
born=$(sed 's/.*) //' /proc/$$/stat | awk '{print $20}')
boot=$(cat /proc/sys/kernel/random/boot_id)
echo $$ > "$lock/pid";echo "$born" > "$lock/starttime";echo "$boot" > "$lock/boot_id"
unlock(){
 [ "$(cat "$lock/pid")" = "$$" ] && [ "$(cat "$lock/starttime")" = "$born" ] && [ "$(cat "$lock/boot_id")" = "$boot" ] || exit 2
 rm "$lock/pid" "$lock/starttime" "$lock/boot_id";rmdir "$lock"
}
trap unlock EXIT
trap 'exit 2' TERM INT HUP

no_faults(){
 for fault in "$state/fault" /run/r144_depth/fault /run/dtof_tuner/fault /run/rgbtof_calibration_v2/fault; do
  if [ -e "$fault" ] || [ -L "$fault" ]; then
   if [ "$fault" = "$state/fault" ] && [ -f "$fault" ] && [ ! -L "$fault" ] && grep -qx sensor-readback "$fault"; then
    fail 'Native ToF sensor readback failed and the fault is latched'
   fi
   fail 'A capture or sensor fault is latched; inspect the board logs'
  fi
 done
}

if [ "$recovering" = 1 ]; then
 [ "$boot" = "$expected_boot" ] || fail 'Board boot changed before stream recovery'
 # The host closed its old receiver. Only that generation can release the
 # owner, and the coordinator waits for its manager to exit without signalling it.
 n=0
 while :; do
  no_faults
  if [ -e "$owner" ]; then
   previous_boot=$(cat "$owner/session.boot_id" 2>/dev/null || true)
   previous_token=$(cat "$owner/session.generation" 2>/dev/null || true)
   [ -z "$previous_boot" ] || [ "$previous_boot" = "$boot" ] || fail 'A different boot owns capture'
   [ -z "$previous_token" ] || [ "$previous_token" = "$previous" ] || fail 'A different capture generation owns the board'
  else
   pid=$(cat "$state/manager.pid" 2>/dev/null || true)
   started=$(cat "$state/manager.starttime" 2>/dev/null || true)
   if [ -z "$pid" ] || [ -z "$started" ] || ! same "$pid" "$started" ||
      [ "$(sed 's/.*) //' "/proc/$pid/stat" 2>/dev/null | cut -d' ' -f1)" = Z ]; then break; fi
  fi
  n=$((n+1)); [ "$n" -le 150 ] || fail 'Previous capture did not release its resources'
  sleep .1
 done
 [ "$(cat "$state/released" 2>/dev/null || true)" = "$boot $previous" ] ||
  fail 'Previous capture has no clean-release receipt'
 # Include owners with process names outside the known imaging binaries.
 for fd in /proc/[0-9]*/fd/*; do
  case "$(readlink "$fd" 2>/dev/null || true)" in
   /dev/spidev0.0|/dev/i2c-3|/dev/video[0-9]*|/dev/media[0-9]*|/dev/v4l-subdev*|/dev/rkisp*|/dmabuf:vmpi)
    fail 'An unrecognized process retains camera or ToF descriptors';;
  esac
 done
 [ -r /proc/net/tcp ] || fail 'Cannot verify that the stream listeners were released'
 for net in /proc/net/tcp /proc/net/tcp6; do
  [ ! -r "$net" ] || [ -z "$(awk '$4=="0A" && $2 ~ /:(022A|2280)$/ {print $2}' "$net")" ] ||
   fail 'A process still owns the RTSP or ToF stream listener'
 done
elif [ -f "$state/manager.pid" ] && [ -f "$state/manager.starttime" ]; then
 pid=$(cat "$state/manager.pid");started=$(cat "$state/manager.starttime")
 if same "$pid" "$started"; then
  command=$(tr '\000' ' ' < "/proc/$pid/cmdline")
  case "$command" in *"sh $root/session.sh "*) ;; *) echo 'Candidate manager identity mismatch' >&2;exit 2;; esac
  kill -TERM "$pid"
  n=0
  while same "$pid" "$started" && [ "$(sed 's/.*) //' /proc/"$pid"/stat | cut -d' ' -f1)" != Z ]; do n=$((n+1));[ "$n" -le 120 ] || exit 2;sleep .1;done
 fi
fi
no_faults
if [ "$1" = legacy ]; then
 shift
 /root/r144_depth/control.sh configure "$@"
 exit
fi
if [ "$recovering" = 0 ]; then /root/r144_depth/control.sh stop >/dev/null; fi
[ ! -e "$owner" ] || fail 'Sensor ownership retained'
[ -z "$(pidof live tmf8829_ext rkipc litesr_gui litesr_near_gui native_hd_gui litesr512_gui litesr512_gui2 sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline || true)" ] || fail 'A capture process is still active'
sh /root/r144_depth/ensure-spi.sh
rm -f "$state/ready.json" "$state/released"
mode=$2;generation=$3;options=$4
nohup /bin/sh "$root/session.sh" "$mode" "$generation" "$options" > "$state/session.log" 2>&1 < /dev/null &
manager=$!
n=0
while [ "$n" -lt 180 ]; do
 if [ -f "$state/ready.json" ] && grep -q "\"generation\":\"$generation\"" "$state/ready.json"; then cat "$state/ready.json";exit;fi
 if ! kill -0 "$manager" 2>/dev/null || [ "$(sed 's/.*) //' "/proc/$manager/stat" 2>/dev/null | cut -d' ' -f1)" = Z ]; then
  no_faults
  fail 'Candidate startup exited before the first frame; inspect the board capture logs'
 fi
 n=$((n+1));sleep .1
done
no_faults
fail 'Candidate startup timed out; ownership retained'
