# DCTS Tiny NV12 camera owner v1
if [ "$1" = 18 ]; then
 [ -z "$(pidof rkipc tiny_nv12 tiny_nv12_camera || true)" ] || exit 2
 tiny_camera="$root/dcts-tiny-nv12-128x80-v1/tiny_nv12_camera"
 [ -x "$tiny_camera" ] && [ ! -L "$tiny_camera" ] && [ "$(sha256sum "$tiny_camera" | cut -d' ' -f1)" = '@TINY_CAMERA_SHA@' ] || exit 2
 LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib "$tiny_camera" > "$state/tiny-camera.log" 2>&1 &
 encoder=$!;encoder_born=$(sed 's/.*) //' /proc/$encoder/stat | awk '{print $20}')
fi
