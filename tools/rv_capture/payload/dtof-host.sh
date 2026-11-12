# DCTS automatic native dToF presets
dcts_tof(){
 preset=$1; mode=$2; shift 2
 case "$preset:$mode" in
  16:14) distance=5;;
  48:24) distance=11;;
  48:25) distance=12;;
  *) echo 'Unsupported native capture preset' >&2; exit 2;;
 esac
 program=$1; shift
 # Keep the saved timing, integration and SNR.
 remaining=$#
 while [ "$remaining" -gt 0 ]; do
  argument=$1; shift; remaining=$((remaining-1))
  case "$argument" in
   -d|-o|-t) [ "$remaining" -gt 0 ] || exit 2; shift; remaining=$((remaining-1));;
   -g|-n) ;;
   *) set -- "$@" "$argument";;
  esac
 done
 "$program" "$@" -d "$distance" -o 1 -t 0
}
