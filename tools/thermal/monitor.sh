#!/bin/sh
# Read-only telemetry. No sysfs writes, governor changes, debugfs mounts or logs
# on the board. The host passes this as a quoted, non-interactive shell command.
set -eu
root=${DCTS_THERMAL_ROOT:-}
limit=${DCTS_THERMAL_SAMPLES:-0}
case "$limit" in ''|*[!0-9]*) exit 2;; esac
emit(){
 [ -r "$root$1" ] || return 0
 value=
 IFS= read -r value < "$root$1" || [ -n "$value" ] || return 0
 printf '%s\t%s\t%s\n' "$kind" "$1" "$value"
}
lines(){
 [ -r "$root$1" ] || return 0
 while IFS= read -r value || [ -n "$value" ]; do
  printf '%s\t%s\t%s\n' "$kind" "$1" "$value"
 done < "$root$1"
}
kind=M
emit /proc/sys/kernel/random/boot_id
emit /proc/version
emit /sys/devices/system/cpu/online
emit /sys/devices/system/cpu/present
lines /sys/kernel/debug/clk/clk_summary
printf 'M\tobserver_pid\t%s\n' "$$"
policies=
for d in "$root"/sys/devices/system/cpu/cpufreq/policy*; do
 [ -d "$d" ] || continue
 policies="$policies ${d#"$root"}"
done
if [ -z "$policies" ] && [ -d "$root/sys/devices/system/cpu/cpu0/cpufreq" ]; then
 policies=/sys/devices/system/cpu/cpu0/cpufreq
fi
for d in $policies; do
 for f in cpuinfo_min_freq cpuinfo_max_freq scaling_available_frequencies scaling_driver scaling_governor affected_cpus; do emit "$d/$f"; done
done
for d in "$root"/sys/class/thermal/thermal_zone*; do
 [ -d "$d" ] || continue
 d=${d#"$root"}
 for f in type mode policy; do emit "$d/$f"; done
 for f in "$root$d"/trip_point_*; do [ ! -f "$f" ] || emit "${f#"$root"}"; done
done
for d in "$root"/sys/class/thermal/cooling_device*; do
 [ -d "$d" ] || continue
 d=${d#"$root"};emit "$d/type";emit "$d/max_state"
done
for d in "$root"/sys/class/devfreq/*; do
 [ -d "$d" ] || continue
 d=${d#"$root"};emit "$d/name";emit "$d/governor";emit "$d/available_frequencies"
done
index=0
processes=
while :; do
 read -r uptime rest < "$root/proc/uptime"
 printf 'BEGIN\t%s\t%s\n' "$index" "$uptime"
 kind=V
 emit /proc/sys/kernel/random/boot_id
 emit /proc/stat
 emit /proc/loadavg
 lines /proc/meminfo
 for d in $policies; do
  for f in scaling_cur_freq cpuinfo_cur_freq scaling_min_freq scaling_max_freq; do emit "$d/$f"; done
  lines "$d/stats/time_in_state"
 done
 for d in "$root"/sys/class/thermal/thermal_zone*; do [ ! -d "$d" ] || emit "${d#"$root"}/temp"; done
 for d in "$root"/sys/class/thermal/cooling_device*; do [ ! -d "$d" ] || emit "${d#"$root"}/cur_state"; done
 for d in "$root"/sys/class/devfreq/*; do
  [ -d "$d" ] || continue
  d=${d#"$root"}
  for f in cur_freq min_freq max_freq load; do emit "$d/$f"; done
 done
 emit /sys/kernel/debug/rknpu/load
 emit /sys/kernel/debug/rknpu/power
 if [ $((index % 10)) -eq 0 ] && [ -r "$root/sys/kernel/debug/clk/clk_summary" ]; then
  while IFS= read -r value; do
   case "$value" in *armclk*|*clk_core*|*npu*|*rknpu*|*clk_ddr*) printf 'V\t/sys/kernel/debug/clk/clk_summary\t%s\n' "$value";; esac
  done < "$root/sys/kernel/debug/clk/clk_summary"
 fi
 if [ -z "$root" ] && [ $((index % 5)) -eq 0 ]; then processes=$(pidof litesr_near_gui tmf8829_ext tiny_nv12 tiny_nv12_camera native_hd_gui 2>/dev/null || true); fi
 for pid in $processes; do emit "/proc/$pid/stat";emit "/proc/$pid/comm"; done
 emit "/proc/$$/stat"
 emit /run/dtof_candidates/sensor_readback.json
 emit /run/dtof_candidates/sensor_readback.json.calibration.json
 read -r uptime rest < "$root/proc/uptime"
 printf 'END\t%s\t%s\n' "$index" "$uptime"
 index=$((index + 1))
 [ "$limit" -eq 0 ] || [ "$index" -lt "$limit" ] || break
 sleep 1
done
