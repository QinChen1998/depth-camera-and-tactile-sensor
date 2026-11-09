#!/bin/sh
set -eu
action=$1
serial=$2
boot=$3
stage=$4
mode=${5:-fresh}
fov=${6:-6}
case "$mode:$fov" in fresh:6|repair:6) ;; *) exit 2;; esac
depth=/root/r144_depth
candidate=/root/dtof_candidates_20261009
tuner=/root/dtof_tuner
init=/etc/init.d/S99zdepth
oem=/oem/usr/bin/RkLunch.sh
backup=/oem/usr/bin/RkLunch.sh.dcts-before-bootstrap
oem_new=/oem/usr/bin/.RkLunch.dcts-bootstrap
driver=/root/rv1103_mw_spi/bin/tof_driver
fail(){ printf 'DTOF_ERROR: %s\n' "$*"; exit 2; }
exists(){ [ -e "$1" ] || [ -L "$1" ]; }
digest(){ sha256sum "$1" | cut -d' ' -f1; }
starttime(){ sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $20}'; }
factory_camera(){
 [ "$(readlink "/proc/$1/exe")" = /oem/usr/bin/rkipc ] || return 1
 case "$(tr '\000' ' ' < "/proc/$1/cmdline")" in
  '/oem/usr/bin/rkipc -a /oem/usr/share/iqfiles '|'rkipc -a /oem/usr/share/iqfiles ') return 0;;
  *) return 1;;
 esac
}


if [ "$action" = activate ]; then
mkdir /run/dcts-board-install.lock || fail 'Another USB setup is in progress.'
installed_depth=0 installed_candidate=0 installed_init=0 installed_tuner=0 saved_oem=0 stopped_camera=0 committed=0 prepared_oem=0 driver_updated=0
old_depth=0 old_candidate=0 old_init=0 old_tuner=0 stopped_status=0 locked_switch=0 locked_control=0 oem_updated=0
camera_config_pending=''
cleanup(){
 status=$?
 trap - EXIT
 trap '' TERM INT HUP
 if [ "$committed" = 0 ]; then
  if [ "$driver_updated" = 1 ]; then cp -p "$driver_backup" "$stage/driver-restore" && mv -f "$stage/driver-restore" "$driver" || status=2; fi
  if [ "$saved_oem" = 1 ]; then mv -f "$backup" "$oem" || status=2; fi
  if [ "$saved_oem" = 0 ] && [ "$oem_updated" = 1 ]; then cp -p "$stage/oem-before" "$oem_new" && mv -f "$oem_new" "$oem" || status=2; fi
  if [ "$installed_init" = 1 ]; then mv "$init" "$stage/S99zdepth" || status=2; fi
  if [ "$installed_tuner" = 1 ]; then mv "$tuner" "$stage/tuner" || status=2; fi
  if [ "$installed_candidate" = 1 ]; then mv "$candidate" "$stage/candidate" || status=2; fi
  if [ "$installed_depth" = 1 ]; then mv "$depth" "$stage/depth" || status=2; fi
  if [ "$old_depth" = 1 ]; then mv "$stage/previous/depth" "$depth" || status=2; fi
  if [ "$old_candidate" = 1 ]; then mv "$stage/previous/candidate" "$candidate" || status=2; fi
  if [ "$old_tuner" = 1 ]; then mv "$stage/previous/tuner" "$tuner" || status=2; fi
  if [ "$old_init" = 1 ]; then mv "$stage/previous/S99zdepth" "$init" || status=2; fi
  if [ -e "$stage/previous/depth" ] || [ -e "$stage/previous/candidate" ] || [ -e "$stage/previous/tuner" ] || [ -e "$stage/previous/S99zdepth" ] ||
     { [ "$driver_updated" = 1 ] && [ "$(digest "$driver")" != "$driver_previous" ]; }; then
   mkdir -p /run/r144_depth;touch /run/r144_depth/fault
   printf '%s\n' 'DTOF_ERROR: rollback incomplete; original files retained in staging.'
   status=2
  fi
  if [ "$stopped_status" = 1 ]; then nohup "$depth/depth_status" </dev/null >/dev/null 2>&1 & fi
  if [ "$stopped_camera" = 1 ] && [ -z "$(pidof rkipc || true)" ]; then
   nohup /oem/usr/bin/rkipc -a /oem/usr/share/iqfiles </dev/null >/dev/null 2>&1 &
  fi
 fi
 if [ "$prepared_oem" = 1 ]; then rm -f "$oem_new"; fi
 if [ -n "$camera_config_pending" ]; then rm -f "$camera_config_pending"; fi
 if [ "$locked_control" = 1 ]; then rmdir /run/r144_depth/control.lock || status=2; fi
 if [ "$locked_switch" = 1 ]; then
  rm /run/dtof_switch.lock/pid /run/dtof_switch.lock/starttime /run/dtof_switch.lock/boot_id || status=2
  rmdir /run/dtof_switch.lock || status=2
 fi
 rmdir /run/dcts-board-install.lock || status=2
 exit "$status"
}
trap cleanup EXIT
trap 'exit 2' TERM INT HUP

fi

[ "$(sed -n 's/^Serial[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo)" = "$serial" ] &&
 [ "$(cat /proc/sys/kernel/random/boot_id)" = "$boot" ] ||
 fail 'USB board identity or boot changed during setup.'
tr '\000' '\n' </proc/device-tree/compatible | grep -qx 'rockchip,rv1103' ||
 fail 'Unsupported board firmware.'
if [ "$mode" = fresh ]; then
 for path in "$depth" "$candidate" "$init" "$backup" "$oem_new"; do
  ! exists "$path" || fail 'A previous or partial board deployment exists. Review it before setup.'
 done
else
 for path in "$depth" "$candidate" "$tuner"; do
  ! exists "$path" || { [ -d "$path" ] && [ ! -L "$path" ]; } || fail 'Managed runtime directory is not a regular directory.'
 done
 for path in "$init" "$backup"; do
  ! exists "$path" || { [ -f "$path" ] && [ ! -L "$path" ]; } || fail 'Managed startup file is not a regular file.'
 done
 ! exists "$oem_new" || fail 'Factory startup staging file already exists.'
fi
for path in /run/r144_depth /run/dtof_candidates /run/dtof_tuner; do
 ! exists "$path" || { [ -d "$path" ] && [ ! -L "$path" ]; } || fail 'Runtime state directory is not a regular directory.'
done
for path in /run/dcts-v120-install.lock /run/r144_fusion_20260923 /run/dtof_switch.lock /run/r144_depth/control.lock /run/r144_depth/fault /run/dtof_candidates/fault /run/dtof_tuner/fault /run/rgbtof_calibration_v2/fault; do
 ! exists "$path" || fail 'Existing capture state or fault must be inspected before setup.'
done
if [ "$mode" = fresh ]; then
 ! exists /run/r144_depth && ! exists /run/dtof_candidates || fail 'Existing capture state must be inspected before setup.'
fi
# The application supplies its own authenticated capture runtime. An earlier
# delivery driver is optional and may have any contents, but must be a regular
# file so it can be backed up and restored without following a symlink.
driver_previous=''
for path in /root/rv1103_mw_spi /root/rv1103_mw_spi/bin; do
 [ ! -L "$path" ] || fail 'The delivery driver directory is a symbolic link.'
done
if exists "$driver"; then
 [ -f "$driver" ] && [ ! -L "$driver" ] || fail 'The existing ToF driver is not a regular file.'
 driver_previous=$(digest "$driver")
fi
[ -x /oem/usr/bin/rkipc ] && [ -d /oem/usr/share/iqfiles ] && [ -s /userdata/rkipc.ini ] &&
 [ -L /sys/bus/i2c/devices/4-0030/driver ] &&
 [ -f "$oem" ] && [ ! -L "$oem" ] ||
 fail 'The required camera firmware, SC3336 binding, is missing.'
[ -z "$(pidof tof_driver algorithm_consumer tmf8829_ext tmf8829_host_driver live native_hd_gui litesr_near_gui tmf8829_tuner sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline || true)" ] ||
 fail 'Stop the active ToF or imaging process before setup.'
if [ "$mode" = fresh ]; then [ -z "$(pidof depth_status || true)" ] || fail 'An existing status service is active.'; fi
camera=$(pidof rkipc || true)
case "$camera" in
 '') ;;
 *[!0-9]*) fail 'More than one camera owner is active.';;
 *) factory_camera "$camera" || fail 'An unrecognized camera owner is active.';;
esac
[ "$action" = activate ] || { echo ready; exit 0; }

mkdir /run/dtof_switch.lock || fail 'Another imaging transition is active.'
locked_switch=1
echo $$ > /run/dtof_switch.lock/pid
starttime $$ > /run/dtof_switch.lock/starttime
echo "$boot" > /run/dtof_switch.lock/boot_id
mkdir -p /run/r144_depth
mkdir /run/r144_depth/control.lock || fail 'Another lifecycle operation is active.'
locked_control=1
mkdir "$stage/previous"
[ ! -e /run/r144_fusion_20260923 ] && [ ! -e /run/r144_depth/fault ] && [ ! -e /run/dtof_candidates/fault ] || fail 'Capture state changed before installation.'

[ "$(digest "$stage/depth/tmf8829_ext")" = '@SPI_DRIVER_SHA@' ] ||
 fail 'The delivered SPI driver changed during setup.'
if [ -f "$stage/files.sha256" ]; then
 (cd "$stage"; sha256sum -c files.sha256) > "$stage/upload-check.log" 2>&1 || fail 'Uploaded runtime checksum mismatch.'
fi
chmod 755 "$stage/depth/tmf8829_ext"
printf '%s\n' "$serial" > "$stage/depth/spi_native16.enabled"
for file in "$stage/depth/"*.sh "$stage/candidate/"*.sh "$stage/S99zdepth"; do sh -n "$file"; done
oem_sha=$(digest "$oem")
(set -C; : > "$oem_new") || fail 'The factory startup staging file already exists.'
prepared_oem=1
if grep -q '^[[:space:]]*\[ ! -f /root/r144_depth/spi_native16.enabled \] || return 0$' "$oem"; then
 cp -p "$oem" "$oem_new"
else
 awk '
 $0 == "\tif [ -d \"/oem/usr/share/iqfiles\" ]; then" {
  print "\t[ ! -f /root/r144_depth/spi_native16.enabled ] || return 0"
  count++
 }
 { print }
 END { if (count != 1) exit 2 }
' "$oem" > "$oem_new" || fail 'The factory camera startup script is unsupported.'
fi
chmod 755 "$oem_new"
sh -n "$oem_new"
# Factory rkipc may write defaults back to its input ini on normal shutdown.
# Snapshot by copying, then restore the original bytes before owned imaging.
cp -p /userdata/rkipc.ini "$stage/depth/rkipc_before_setup.ini"
if [ "$mode" = repair ] && [ -f "$depth/rkipc_before_setup.ini" ] && [ ! -L "$depth/rkipc_before_setup.ini" ]; then
 cp -p "$depth/rkipc_before_setup.ini" "$stage/depth/rkipc_before_setup.ini"
fi
cp -p /userdata/rkipc.ini "$stage/camera-before.ini"
camera_config_sha=$(digest "$stage/camera-before.ini")
if [ -n "$camera" ]; then
 born=$(starttime "$camera")
 [ -n "$born" ] && [ "$(pidof rkipc)" = "$camera" ] &&
  factory_camera "$camera" &&
  [ "$(starttime "$camera")" = "$born" ] ||
  fail 'The factory camera owner changed.'
 kill -TERM "$camera"
 stopped_camera=1
 n=0
 while [ "$(starttime "$camera")" = "$born" ] && [ "$(sed 's/.*) //' "/proc/$camera/stat" 2>/dev/null | cut -d' ' -f1)" != Z ]; do
  n=$((n+1))
  [ "$n" -le 300 ] || fail 'The factory camera did not release capture.'
  sleep .1
 done
fi
[ -z "$(pidof rkipc tof_driver algorithm_consumer tmf8829_ext tmf8829_host_driver || true)" ] ||
 fail 'Capture is still in use.'
. "$stage/depth/camera-takeover.sh"
dcts_release_camera_helpers
if [ "$(digest /userdata/rkipc.ini)" != "$camera_config_sha" ]; then
 camera_config_pending=$(mktemp /userdata/.dcts-camera.XXXXXX)
 cp -p "$stage/camera-before.ini" "$camera_config_pending"
 [ "$(digest "$camera_config_pending")" = "$camera_config_sha" ] || fail 'Factory camera configuration backup differs.'
 mv -f "$camera_config_pending" /userdata/rkipc.ini
 camera_config_pending=''
fi
sh "$stage/depth/ensure-spi.sh"
export LD_LIBRARY_PATH="$stage/depth:/oem/usr/lib:/usr/lib"
"$stage/candidate/native_hd_gui" --native-capabilities > "$stage/native.json" ||
 fail 'The Native16 capture program could not start on this firmware.'
grep -qx "{\"native_capture_protocol\":\"native-m48-m16-m8-v2\",\"fov\":$fov,\"model_inference\":false,\"photon_enabled\":false,\"m8\":false,\"m16\":true,\"m48\":true}" "$stage/native.json" ||
 fail 'The Native16 capture program did not report the expected calibration capabilities.'
[ "$(digest "$oem")" = "$oem_sha" ] || fail 'The factory startup script changed during setup.'
if ! exists "$backup"; then ln "$oem" "$backup";saved_oem=1; fi
cp -p "$oem" "$stage/oem-before"
# Keep the earlier delivery entry point consistent when it exists. A board
# without that optional package uses depth/tmf8829_ext directly.
if [ -n "$driver_previous" ]; then
 [ -f "$driver" ] && [ ! -L "$driver" ] && [ "$(digest "$driver")" = "$driver_previous" ] || fail 'The existing ToF driver changed during setup.'
 driver_backup="$driver.dcts-before-$driver_previous"
 if ! exists "$driver_backup"; then ln "$driver" "$driver_backup"; fi
 [ -f "$driver_backup" ] && [ ! -L "$driver_backup" ] && [ "$(digest "$driver_backup")" = "$driver_previous" ] || fail 'Factory driver backup differs.'
 cp -p "$stage/depth/tmf8829_ext" "$stage/tof-driver-new"
 mv -f "$stage/tof-driver-new" "$driver"
 driver_updated=1
else
 ! exists "$driver" || fail 'A ToF driver appeared during setup.'
fi
status_pid=$(pidof depth_status || true)
if [ -n "$status_pid" ]; then
 case "$status_pid" in *[!0-9]*) fail 'More than one status service is active.';; esac
 [ "$(readlink /proc/"$status_pid"/exe)" = "$depth/depth_status" ] || fail 'Unrecognized status service owner.'
 status_born=$(starttime "$status_pid")
 kill -TERM "$status_pid"
 stopped_status=1
 n=0
 while [ "$(starttime "$status_pid")" = "$status_born" ] && [ "$(sed 's/.*) //' /proc/"$status_pid"/stat 2>/dev/null | cut -d' ' -f1)" != Z ]; do
  n=$((n+1));[ "$n" -le 100 ] || fail 'Status service did not exit.';sleep .1
 done
fi
if exists "$depth"; then mv "$depth" "$stage/previous/depth";old_depth=1; fi
if exists "$candidate"; then mv "$candidate" "$stage/previous/candidate";old_candidate=1; fi
if exists "$init"; then mv "$init" "$stage/previous/S99zdepth";old_init=1; fi
if [ -d "$stage/tuner" ]; then
 if [ -d "$tuner/persistence" ] && [ ! -L "$tuner/persistence" ]; then cp -a "$tuner/persistence" "$stage/tuner/persistence"; fi
 if exists "$tuner"; then mv "$tuner" "$stage/previous/tuner";old_tuner=1; fi
 mv "$stage/tuner" "$tuner";installed_tuner=1
fi
mv "$stage/depth" "$depth"
installed_depth=1
mv "$stage/candidate" "$candidate"
installed_candidate=1
mv "$stage/S99zdepth" "$init"
installed_init=1
mv -f "$oem_new" "$oem"
oem_updated=1
sync
unset LD_LIBRARY_PATH
if [ "$mode" = repair ]; then mv "$stage/previous" "$stage.before"; fi
committed=1
"$init" start
cat "$stage/native.json"

