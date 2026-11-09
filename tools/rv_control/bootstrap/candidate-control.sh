#!/bin/sh
# RV1103 candidate coordinator. Normal operations only.
set -eu
root=/root/dtof_candidates_20261009
state=/run/dtof_candidates
lock=/run/dtof_switch.lock
same(){ [ -r "/proc/$1/stat" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat | awk '{print $20}')" = "$2" ]; }
case "${1:-}" in
 candidate) [ "$#" = 4 ]; case "$2" in
   14|24|25) "$root/native_gui.sh" validate "$2" "$3" "$4";;
   *) exit 2;;
  esac;;
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
until mkdir "$lock" 2>/dev/null; do n=$((n+1));[ "$n" -le 200 ] || exit 2;sleep .1;done
born=$(sed 's/.*) //' /proc/$$/stat | awk '{print $20}')
boot=$(cat /proc/sys/kernel/random/boot_id)
echo $$ > "$lock/pid";echo "$born" > "$lock/starttime";echo "$boot" > "$lock/boot_id"
unlock(){
 [ "$(cat "$lock/pid")" = "$$" ] && [ "$(cat "$lock/starttime")" = "$born" ] && [ "$(cat "$lock/boot_id")" = "$boot" ] || exit 2
 rm "$lock/pid" "$lock/starttime" "$lock/boot_id";rmdir "$lock"
}
trap unlock EXIT
trap 'exit 2' TERM INT HUP
if [ -f "$state/manager.pid" ] && [ -f "$state/manager.starttime" ]; then
 pid=$(cat "$state/manager.pid");started=$(cat "$state/manager.starttime")
 if same "$pid" "$started"; then
  command=$(tr '\000' ' ' < "/proc/$pid/cmdline")
  case "$command" in *"sh $root/session.sh "*) ;; *) echo 'Candidate manager identity mismatch' >&2;exit 2;; esac
  kill -TERM "$pid"
  n=0
  while same "$pid" "$started" && [ "$(sed 's/.*) //' /proc/"$pid"/stat | cut -d' ' -f1)" != Z ]; do n=$((n+1));[ "$n" -le 120 ] || exit 2;sleep .1;done
 fi
fi
[ ! -e "$state/fault" ] && [ ! -e /run/r144_depth/fault ] || { echo 'Fault retained' >&2;exit 2; }
if [ "$1" = legacy ]; then
 shift
 /root/r144_depth/control.sh configure "$@"
 exit
fi
/root/r144_depth/control.sh stop >/dev/null
[ ! -e /run/r144_fusion_20260923 ] || { echo 'Sensor ownership retained' >&2;exit 2; }
[ -z "$(pidof live tmf8829_ext rkipc litesr_gui litesr_near_gui native_hd_gui litesr512_gui litesr512_gui2 sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline || true)" ] || exit 2
sh /root/r144_depth/ensure-spi.sh
rm -f "$state/ready.json"
shift
nohup /bin/sh "$root/session.sh" "$@" > "$state/session.log" 2>&1 < /dev/null &
manager=$!
n=0
while [ "$n" -lt 180 ]; do
 if [ -f "$state/ready.json" ] && grep -q "\"generation\":\"$2\"" "$state/ready.json"; then cat "$state/ready.json";exit;fi
 kill -0 "$manager" 2>/dev/null || exit 2
 n=$((n+1));sleep .1
done
echo 'Candidate startup timed out; ownership retained' >&2
exit 2
