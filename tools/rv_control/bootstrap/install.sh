#!/bin/sh
set -eu
action=$1
serial=$2
boot=$3
stage=$4
depth=/root/r144_depth
candidate=/root/dtof_candidates_20261009
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
mkdir /run/dcts-v120-install.lock || fail 'Another USB setup is in progress.'
installed_depth=0 installed_candidate=0 installed_init=0 saved_oem=0 stopped_camera=0 committed=0 prepared_oem=0 driver_updated=0
cleanup(){
 status=$?
 trap - EXIT
 trap '' TERM INT HUP
 if [ "$committed" = 0 ]; then
  if [ "$driver_updated" = 1 ]; then cp -p "$driver_backup" "$stage/driver-restore" && mv -f "$stage/driver-restore" "$driver" || status=2; fi
  if [ "$saved_oem" = 1 ]; then mv -f "$backup" "$oem" || status=2; fi
  if [ "$installed_init" = 1 ]; then mv "$init" "$stage/S99zdepth" || status=2; fi
  if [ "$installed_candidate" = 1 ]; then mv "$candidate" "$stage/candidate" || status=2; fi
  if [ "$installed_depth" = 1 ]; then mv "$depth" "$stage/depth" || status=2; fi
  if [ "$stopped_camera" = 1 ] && [ -z "$(pidof rkipc || true)" ]; then
   nohup /oem/usr/bin/rkipc -a /oem/usr/share/iqfiles </dev/null >/dev/null 2>&1 &
  fi
 fi
 if [ "$prepared_oem" = 1 ]; then rm -f "$oem_new"; fi
 rmdir /run/dcts-v120-install.lock || status=2
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
for path in "$depth" "$candidate" "$init" "$backup" "$oem_new"; do
 ! exists "$path" || fail 'A previous or partial board deployment exists. Review it before setup.'
done
for path in /run/r144_fusion_20260923 /run/dtof_switch.lock /run/r144_depth /run/dtof_candidates; do
 ! exists "$path" || fail 'Existing capture state or fault must be inspected before setup.'
done
[ -f "$driver" ] && [ ! -L "$driver" ] &&
 { [ "$(digest "$driver")" = 290bf1a751fbaa3105a2f9caf5e6aeac867131cd2dbab1ee16e40394163f7521 ] || [ "$(digest "$driver")" = 0c593858cfde16cebf7d2ddc503c2b7eec71de7dffc34d290dcf6a87c6744560 ]; } ||
 fail 'The V1.2.0 SPI ToF driver is missing or differs from the delivery.'
[ -x /oem/usr/bin/rkipc ] && [ -d /oem/usr/share/iqfiles ] && [ -s /userdata/rkipc.ini ] &&
 [ -r /usr/lib/libcrypto.so.1.1 ] && [ -L /sys/bus/i2c/devices/4-0030/driver ] &&
 [ -f "$oem" ] && [ ! -L "$oem" ] ||
 fail 'The delivered camera firmware, SC3336 binding, or crypto runtime is missing.'
[ -z "$(pidof tof_driver algorithm_consumer tmf8829_ext tmf8829_host_driver depth_status live native_hd_gui litesr_near_gui sensor_fusion_service capture_bridge rgbtof_bridge dtof48 next_pipeline || true)" ] ||
 fail 'Stop the active ToF or imaging process before setup.'
camera=$(pidof rkipc || true)
case "$camera" in
 '') ;;
 *[!0-9]*) fail 'More than one camera owner is active.';;
 *) factory_camera "$camera" || fail 'An unrecognized camera owner is active.';;
esac
[ "$action" = activate ] || { echo ready; exit 0; }


[ "$(digest "$stage/depth/tmf8829_ext")" = 0c593858cfde16cebf7d2ddc503c2b7eec71de7dffc34d290dcf6a87c6744560 ] ||
 fail 'The delivered SPI driver changed during setup.'
chmod 755 "$stage/depth/tmf8829_ext"
printf '%s\n' "$serial" > "$stage/depth/spi_native16.enabled"
for file in "$stage/depth/"*.sh "$stage/candidate/"*.sh "$stage/S99zdepth"; do sh -n "$file"; done
oem_sha=$(digest "$oem")
(set -C; : > "$oem_new") || fail 'The factory startup staging file already exists.'
prepared_oem=1
awk '
 $0 == "\tif [ -d \"/oem/usr/share/iqfiles\" ]; then" {
  print "\t[ ! -f /root/r144_depth/spi_native16.enabled ] || return 0"
  count++
 }
 { print }
 END { if (count != 1) exit 2 }
' "$oem" > "$oem_new" || fail 'The factory camera startup script is unsupported.'
chmod 755 "$oem_new"
sh -n "$oem_new"
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
sh "$stage/depth/ensure-spi.sh"
export LD_LIBRARY_PATH="$stage/depth:/oem/usr/lib:/usr/lib"
"$stage/candidate/native_hd_gui" --native-capabilities > "$stage/native.json" ||
 fail 'The Native16 capture program could not start on this firmware.'
grep -qx '{"native_capture_protocol":"native-m48-m16-m8-v2","fov":1,"model_inference":false,"photon_enabled":false,"m8":false,"m16":true,"m48":true}' "$stage/native.json" ||
 fail 'The Native16 capture program did not report the expected F1 capabilities.'
"$stage/candidate/dcts-litesr-protected-v3-info10/litesr_near_gui" check-protected \
 "$stage/candidate/dcts-litesr-protected-v3-info10/model.dctsm" > "$stage/model.log" 2>&1 &&
 grep -qx DCTS_PROTECTED_LITESR_READY "$stage/model.log" ||
 fail 'The board could not initialize the protected LiteSR model.'
"$stage/candidate/dcts-litesr-384x256-v2-info10/litesr_near_gui" check-protected \
 "$stage/candidate/dcts-litesr-384x256-v2-info10/model.dctsm" > "$stage/rect-model.log" 2>&1 &&
 grep -qx DCTS_PROTECTED_LITESR_READY "$stage/rect-model.log" ||
 fail 'The board could not initialize rectangular LiteSR.'
"$stage/candidate/dcts-litesr-128x128-v1/litesr_near_gui" check-protected \
 "$stage/candidate/dcts-litesr-128x128-v1/model.dctsm" > "$stage/native16-model.log" 2>&1 &&
 grep -qx DCTS_PROTECTED_LITESR_READY "$stage/native16-model.log" ||
 fail 'The board could not initialize Native16 LiteSR.'
[ "$(digest "$oem")" = "$oem_sha" ] || fail 'The factory startup script changed during setup.'
ln "$oem" "$backup"
saved_oem=1
# Retain the delivered driver before installing the same validated program
# at both application entry points. All capture owners are stopped above.
driver_previous=$(digest "$driver")
driver_backup="$driver.dcts-before-$driver_previous"
if [ ! -e "$driver_backup" ]; then ln "$driver" "$driver_backup"; fi
[ -f "$driver_backup" ] && [ ! -L "$driver_backup" ] && [ "$(digest "$driver_backup")" = "$driver_previous" ] || fail 'Factory driver backup differs.'
cp -p "$stage/depth/tmf8829_ext" "$stage/tof-driver-new"
mv -f "$stage/tof-driver-new" "$driver"
driver_updated=1
mv "$stage/depth" "$depth"
installed_depth=1
mv "$stage/candidate" "$candidate"
installed_candidate=1
mv "$stage/S99zdepth" "$init"
installed_init=1
mv -f "$oem_new" "$oem"
sync
committed=1
unset LD_LIBRARY_PATH
"$init" start
cat "$stage/native.json"

