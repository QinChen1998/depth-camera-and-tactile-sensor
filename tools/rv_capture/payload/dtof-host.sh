# DCTS automatic dToF presets v4
dcts_tof(){
 preset=$1; mode=$2; shift 2
 main_iterations=
 case "$preset:$mode" in
  16:14) distance=5;;
  48:24) distance=11;;
  48-1200:24) distance=11; main_iterations=1200;;
  48:25) distance=12;;
  48-1200:25) distance=12; main_iterations=1200;;
  *) echo 'Unsupported native capture preset' >&2; exit 2;;
 esac
 program=$1; shift
 # Both Native48 1200K presets override only the main integration.
 # Other inputs, including period and SNR, come from the saved settings.
 remaining=$#
 while [ "$remaining" -gt 0 ]; do
  argument=$1; shift; remaining=$((remaining-1))
  case "$argument" in
   -d|-o|-t) [ "$remaining" -gt 0 ] || exit 2; shift; remaining=$((remaining-1));;
   -i)
    [ "$remaining" -gt 0 ] || exit 2
    set -- "$@" -i "${main_iterations:-$1}"
    shift; remaining=$((remaining-1));;
   -g|-n) ;;
   *) set -- "$@" "$argument";;
  esac
 done
 "$program" "$@" -d "$distance" -o 1 -t 0
}
