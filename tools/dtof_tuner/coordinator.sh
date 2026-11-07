#!/bin/sh
# Executed by the pinned native worker, not installed as a service.
# No model deployment, fault recovery, automatic resume, or unverified kills.
set -eu
owner=/run/r144_fusion_20260923
state=/run/dtof_tuner
formal=/root/r144_depth
candidate=/root/dtof_candidates_20261009
fail(){ echo "DTOF_ERROR:$*"; exit 2; }
same(){ [ -r "/proc/$1/stat" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat | awk '{print $20}')" = "$2" ] && [ "$(sed 's/.*) //' /proc/"$1"/stat | cut -d' ' -f1)" != Z ]; }
health(){ [ ! -e /run/r144_depth/fault ] && [ ! -e /run/dtof_candidates/fault ] && [ ! -e /run/rgbtof_calibration_v2/fault ] && [ ! -e "$state/fault" ] || fail 'Active fault retained; inspect device logs'; }
quiescent(){
 [ ! -e "$owner" ] || fail 'Capture owner has not released resources'
 for name in live tmf8829_ext tmf8829_tuner rkipc litesr_gui litesr_near_gui native_hd_gui sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline tiny_nv12 tiny_nv12_camera; do
  ! pidof "$name" >/dev/null || fail "Capture process remains: $name"
 done
}
protected_snapshot(){
 material=$(
  cat /proc/sys/kernel/random/boot_id
  for f in "$formal/control.sh" "$candidate/control.sh"; do sha256sum "$f" || return 2; done
  for name in selected_model selected_comparison selected_rgb_width selected_format selected_depth_width selected_depth_location selected_depth_method; do
   if [ -f "/run/r144_depth/$name" ]; then sha256sum "/run/r144_depth/$name";else echo "absent:$name";fi
  done
 ) || fail 'Cannot read complete configuration snapshot'
 printf '%s\n' "$material" | sha256sum | cut -d' ' -f1
}
snapshot(){
 protected=$(protected_snapshot) || return 2
 native_sha=$(sha256sum "$formal/service.sh") || return 2
 candidate_sha=$(sha256sum "$candidate/session.sh") || return 2
 printf '%s\n%s\n%s\n' "$protected" "$native_sha" "$candidate_sha" | sha256sum | cut -d' ' -f1
}
validate_owner(){
 # The GUI supplies only an identity returned to THIS coordinator's session.
 want=$1;want_boot=$2
 if [ ! -e "$owner" ];then quiescent;kind=idle;generation=-;return;fi
 [ "$want" != - ] || fail 'External capture owner; stop it in its own application'
 [ "$(cat "$owner/session.boot_id")" = "$want_boot" ] && [ "$want_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] || fail 'Owner boot changed'
 generation=$(cat "$owner/session.generation");[ "$generation" = "$want" ] || fail 'External capture generation'
 pid=$(cat "$owner/session.pid");born=$(cat "$owner/session.starttime")
 case "$pid:$born" in *[!0-9:]*) fail 'Malformed owner identity';; esac
 same "$pid" "$born" || fail 'Stale owner retained; maintenance required'
 cmd=$(tr '\000' ' ' < "/proc/$pid/cmdline")
 case "$cmd" in
  *"sh $formal/service.sh "*) kind=native;;
  *"sh $candidate/session.sh "*)
   [ "$(cat /run/dtof_candidates/manager.pid)" = "$pid" ] && [ "$(cat /run/dtof_candidates/manager.starttime)" = "$born" ] || fail 'LiteSR manager identity mismatch'
   kind=litesr;;
  *"sh /root/dtof_tuner/session.sh "*) kind=tuner;;
  *) fail 'Unknown capture owner; no process was signalled';;
 esac
}
owned(){
 [ "$(cat "$state/token" 2>/dev/null)" = "$1" ] || fail 'Experiment token differs'
 validate_owner "dtof-$1" "$(cat /proc/sys/kernel/random/boot_id)"
 [ "$kind" = tuner ] || fail 'Experiment no longer owns capture'
 case "$cmd" in *"sh /root/dtof_tuner/session.sh $1 "*) ;; *) fail 'Experiment command identity mismatch';; esac
}
release_locks(){
 code=$?;trap - EXIT
 if [ "$locked_control" = 1 ];then rmdir /run/r144_depth/control.lock || code=2;fi
 if [ "$locked_switch" = 1 ];then
  [ "$(cat /run/dtof_switch.lock/pid)" = "$$" ] && [ "$(cat /run/dtof_switch.lock/starttime)" = "$lock_born" ] && [ "$(cat /run/dtof_switch.lock/boot_id)" = "$lock_boot" ] || exit 2
  rm /run/dtof_switch.lock/pid /run/dtof_switch.lock/starttime /run/dtof_switch.lock/boot_id
  rmdir /run/dtof_switch.lock || code=2
 fi
 exit "$code"
}
locks(){
 locked_control=0;locked_switch=0
 mkdir /run/dtof_switch.lock 2>/dev/null || fail 'Another imaging transition is active'
 locked_switch=1;lock_born=$(sed 's/.*) //' /proc/$$/stat | awk '{print $20}');lock_boot=$(cat /proc/sys/kernel/random/boot_id)
 echo $$ > /run/dtof_switch.lock/pid;echo "$lock_born" > /run/dtof_switch.lock/starttime;echo "$lock_boot" > /run/dtof_switch.lock/boot_id
 trap release_locks EXIT;trap 'exit 2' TERM INT HUP
 mkdir /run/r144_depth/control.lock 2>/dev/null || fail 'Formal lifecycle control is active'
 locked_control=1
}
stop_owner(){
 [ "$kind" != idle ] || return 0
 old_kind=$kind
 had_tof=0;if pidof tmf8829_ext tmf8829_tuner >/dev/null;then had_tof=1;fi
 same "$pid" "$born" || fail 'Owner exited before STOP'
 kill -TERM "$pid" || fail 'Normal STOP could not be delivered'
 n=0
 while same "$pid" "$born";do n=$((n+1));[ "$n" -le 180 ] || fail 'Normal STOP timed out; ownership preserved';sleep .1;done
 quiescent
 if [ "$had_tof" = 1 ];then
  case "$old_kind" in native) logs='/run/r144_depth/tof.log';;litesr) logs='/run/dtof_candidates/sensor.log';;tuner) logs="$state/tof.log";;esac
  if ! grep -q stop_command_acknowledged "$logs.0" "$logs.1" 2>/dev/null || grep -q 'stop_command_failed\|TOF_WARNING' "$logs.0" "$logs.1" 2>/dev/null;then
   mkdir -p "$state";touch "$state/fault";fail 'STOP acknowledgement missing; fault evidence retained'
  fi
 fi
 health
}
receivers_gone(){
 # Established server-side CAN8 / DTN1(PHC1) / RTSP / LSG ports.
 n=0
 while awk '$4=="01" && $2 ~ /:(227F|2280|2281|022A|2283|2288|228E)$/ {found=1} END {exit !found}' /proc/net/tcp /proc/net/tcp6;do
  n=$((n+1));[ "$n" -lt 30 ] || fail 'Another receiver is still connected';sleep .1
 done
}
validate_settings(){
 [ "$#" = 9 ] || fail 'Incomplete experiment settings'
 case "$1" in ''|*[!a-f0-9]*) fail 'Invalid experiment token';;esac
 [ "${#1}" = 32 ] || fail 'Invalid experiment token length'
 shift
 for value in "$@";do case "$value" in ''|*[!0-9]*) fail 'Non-integer experiment setting';;esac;done
 [ "$1" -le 13 ] && [ "$2" -ge 100 ] && [ "$2" -le 10000 ] && [ "$3" -ge 1 ] && [ "$3" -le 10000 ] && [ "$4" -ge 4 ] && [ "$4" -le 255 ] && [ "$5" -ge 20 ] && [ "$5" -le 2000 ] && [ "$6" -ge 1 ] && [ "$6" -le 4 ] && [ "$7" -le 1 ] && [ "$8" -le 1 ] || fail 'Experiment setting outside supported range'
 [ "$1:$6" != 13:4 ] || fail '48x32 dual four-target result capacity exceeded'
 if [ "$1" -ge 11 ] && [ "$6:$7:$8" = 4:1:1 ];then fail '48x32 four-target Signal and Noise capacity exceeded';fi
}
start_tuner(){
 validate_settings "$@"
 token=$1;shift
 health;quiescent
 mkdir -p "$state"
 echo "$token" > "$state/token";echo starting > "$state/status"
 cut -d. -f1 /proc/uptime > "$state/heartbeat"
 nohup sh /root/dtof_tuner/session.sh "$token" "$@" > "$state/manager.log" 2>&1 < /dev/null &
 manager=$!;n=0
 while [ "$(cat "$state/status")" != running ];do
  health;kill -0 "$manager" 2>/dev/null || fail 'Experiment manager exited; inspect /run/dtof_tuner'
  n=$((n+1));[ "$n" -lt 240 ] || fail 'Experiment startup timed out; lease and evidence retained';sleep .1
 done
 owned "$token"
 cat "$state/ready.$token"
 p=$(cut -d' ' -f1 "$state/tof.identity");b=$(cut -d' ' -f2 "$state/tof.identity")
 same "$p" "$b" || fail 'Sensor PID identity changed'
 tr '\000' ' ' < "/proc/$p/cmdline";echo
}
action=$1;shift
case "$action" in
 inspect)
  [ "$#" = 2 ];health
  [ ! -e /run/dtof_switch.lock ] && [ ! -e /run/r144_depth/control.lock ] || fail 'Device transition in progress'
  validate_owner "$1" "$2"
  [ "$kind" != tuner ] || fail 'Independent tuner already owns capture'
  cat /proc/sys/kernel/random/boot_id;snapshot;echo "$kind";echo "$generation"
  ;;
 start)
  [ "$#" = 12 ];expected=$1;want=$2;want_boot=$3;shift 3
  validate_settings "$@"
  locks;health
  [ "$(snapshot)" = "$expected" ] || fail 'Configuration drift before handover'
  validate_owner "$want" "$want_boot";[ "$kind" != tuner ] || fail 'External tuner owner'
  receivers_gone;stop_owner
  [ "$(snapshot)" = "$expected" ] || fail 'Configuration drift during STOP'
  start_tuner "$@"
  ;;
 switch)
  [ "$#" = 11 ];expected=$1;old=$2;shift 2
  validate_settings "$@"
  locks;health;[ "$(snapshot)" = "$expected" ] || fail 'Configuration drift during experiment'
  owned "$old";stop_owner;start_tuner "$@"
  ;;
 stop)
  [ "$#" = 1 ];token=$1;locks
  if [ ! -e "$owner" ];then
   quiescent;health
   # A failed preflight/start may not yet have written its token. No owner is safe.
  else owned "$token";stop_owner;fi
  echo stopped
  ;;
 heartbeat)
  [ "$#" = 1 ];owned "$1";health
  cut -d. -f1 /proc/uptime > "$state/heartbeat.$1.tmp"
  mv "$state/heartbeat.$1.tmp" "$state/heartbeat"
  echo running
  ;;
 snapshot) [ "$#" = 0 ];snapshot;;
 *) fail 'Unsupported tuner operation';;
esac
