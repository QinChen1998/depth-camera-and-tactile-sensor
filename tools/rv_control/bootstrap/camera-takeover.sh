# Sourced only during authenticated, idle USB setup. Some factory rkipc builds
# leave their RTSP listener and camera DMA buffers inherited by USB udhcpc.
dcts_camera_fail(){ printf 'DTOF_ERROR: %s\n' "$*"; exit 2; }
dcts_camera_birth(){ sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $20}'; }
dcts_camera_shared_fds(){
 dcts_has_rtsp=0;dcts_has_dma=0
 for dcts_fd in /proc/"$1"/fd/*; do
  dcts_target=$(readlink "$dcts_fd" 2>/dev/null || true)
  [ "$dcts_target" != /dmabuf:vmpi ] || dcts_has_dma=1
  for dcts_inode in $dcts_rtsp_inodes; do
   [ "$dcts_target" != "socket:[$dcts_inode]" ] || dcts_has_rtsp=1
  done
 done
 [ "$dcts_has_rtsp:$dcts_has_dma" = 1:1 ]
}
dcts_camera_known_helper(){
 [ "$(readlink "/proc/$1/exe" 2>/dev/null)" = /bin/busybox ] &&
 [ "$(tr '\000' ' ' <"/proc/$1/cmdline")" = 'udhcpc -i usb0 -T 1 -A 0 -b -q ' ] &&
 [ "$(awk '/^PPid:/{print $2}' "/proc/$1/status")" = 1 ]
}
dcts_release_camera_helpers(){
 DCTS_CAMERA_HELPERS_RELEASED=0
 [ -z "$(pidof rkipc || true)" ] || dcts_camera_fail 'Stop the camera before releasing inherited descriptors.'
 for dcts_path in /run/r144_fusion_20260923 /run/r144_depth/fault /run/dtof_candidates/fault /run/dtof_tuner/fault; do
  [ ! -e "$dcts_path" ] || dcts_camera_fail 'Capture ownership or fault retained during camera setup.'
 done
 dcts_camera_boot=$(cat /proc/sys/kernel/random/boot_id)
 dcts_rtsp_inodes=$(awk '$4=="0A" && $2 ~ /:022A$/ {print $10}' /proc/net/tcp)
 [ -n "$dcts_rtsp_inodes" ] || return 0
 for dcts_proc in /proc/[0-9]*; do
  dcts_pid=${dcts_proc##*/}
  [ -r "$dcts_proc/stat" ] && [ -r "$dcts_proc/cmdline" ] || continue
  dcts_camera_shared_fds "$dcts_pid" || continue
  dcts_camera_known_helper "$dcts_pid" || dcts_camera_fail 'An unrecognized process retains factory camera descriptors.'
  dcts_born=$(dcts_camera_birth "$dcts_pid")
  [ -n "$dcts_born" ] && [ "$dcts_camera_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] &&
   dcts_camera_known_helper "$dcts_pid" && dcts_camera_shared_fds "$dcts_pid" &&
   [ "$(dcts_camera_birth "$dcts_pid")" = "$dcts_born" ] || dcts_camera_fail 'The camera descriptor owner changed.'
  kill -TERM "$dcts_pid" || dcts_camera_fail 'Could not stop the verified factory camera helper.'
  dcts_wait=0
  while [ "$(dcts_camera_birth "$dcts_pid")" = "$dcts_born" ] &&
        [ "$(sed 's/.*) //' "$dcts_proc/stat" 2>/dev/null | cut -d' ' -f1)" != Z ]; do
   dcts_wait=$((dcts_wait+1));[ "$dcts_wait" -le 100 ] || dcts_camera_fail 'Factory camera descriptors were not released.'
   sleep .1
  done
  DCTS_CAMERA_HELPERS_RELEASED=$((DCTS_CAMERA_HELPERS_RELEASED+1))
 done
 [ -z "$(awk '$4=="0A" && $2 ~ /:022A$/ {print $10}' /proc/net/tcp)" ] ||
  dcts_camera_fail 'RTSP port 554 is still owned by another process.'
 # udhcpc was a leaked camera descendant, not the static USB address owner.
 # Restore only the already-verified USB address if its exit removed it.
 if ! ip -4 addr show dev usb0 | grep -q 'inet 172\.32\.0\.93/16 '; then
  [ -z "$(ip -4 addr show dev usb0 | awk '$1=="inet"{print $2}')" ] || dcts_camera_fail 'USB addressing changed during camera setup.'
  ip addr add 172.32.0.93/16 dev usb0
 fi
}
