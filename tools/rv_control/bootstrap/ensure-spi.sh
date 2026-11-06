#!/bin/sh
set -eu
[ -c /dev/spidev0.0 ] && exit 0
vendor=/usr/bin/luckfox-config
digest=$(sha256sum "$vendor" 2>/dev/null | cut -d' ' -f1)
[ "$digest" = 9bfb0fc882d9a020bc6ae5f2be1e2a842cf3ae7876a626da5e8a52da73a7d107 ] || {
 echo 'DTOF_ERROR: SPI0 is unavailable and the Luckfox configuration tool is unsupported.'
 exit 2
}
work=$(mktemp -d /run/dcts-spi.XXXXXX)
trap 'rm -f "$work"/*; rmdir "$work"' EXIT
# Load the verified vendor functions, stopping before the interactive entry point.
sed '/^echo 0 >\/proc\/sys\/kernel\/printk$/,$d' "$vendor" > "$work/functions"
[ ! -r /etc/luckfox.cfg ] || cp /etc/luckfox.cfg "$work/luckfox.cfg"
if ! /bin/bash -c '
 . "$1/functions"
 LUCKFOX_CFG_FILE=$1/luckfox.cfg
 LUCKFOX_PIN_DIAGRAM_FILE=$1/pins
 LUCKFOX_FDT_DUMP_TXT=$1/fdt.txt
 LUCKFOX_CHANGE_TXT=$1/change.txt
 LUCKFOX_DYNAMIC_DTS=$1/overlay.dts
 LUCKFOX_DYNAMIC_DTBO=$1/overlay.dtbo
 LUCKFOX_FDT_DTB=$1/fdt.dtb
 LUCKFOX_FDT_OVERLAY_DTS=$1/fdt-overlay.dts
 LUCKFOX_FDT_OVERLAY_DTBO=$1/fdt-overlay.dtbo
 LUCKFOX_FDT_HDR_DTB=$1/header.dtb
 LUCKFOX_FDT_HDR_OVERLAY_DTS=$1/header.dts
 LUCKFOX_FDT_HDR_OVERLAY_DTBO=$1/header.dtbo
 luckfox_config_init
 luckfox_spi_app 1 0 0 1 1 10000000
' dcts-spi "$work" >/dev/null 2>&1 || [ ! -c /dev/spidev0.0 ]; then
 echo 'DTOF_ERROR: Could not enable the delivered SPI0_M0 configuration.'
 exit 2
fi

