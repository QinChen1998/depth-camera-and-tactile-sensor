# DCTS automatic dToF preset v3
dcts_tof(){
 preset=$1; mode=$2; shift 2
 case "$preset:$mode" in
  :*) if [ "$1" = launch_tof ]; then "$@"; else exec "$@"; fi; return;;
  16:14) distance=5; period=; signal=0;;
  16:17|16:18) distance=5; period=33; signal=1;;
  48:13|48:16) distance=11; period=; signal=1;;
  *) return 2;;
 esac
 remaining=$#
 while [ "$remaining" -gt 0 ]; do
  argument=$1; shift; remaining=$((remaining-1))
  case "$argument" in
   -d|-o|-t) [ "$remaining" -gt 0 ] || return 2; shift; remaining=$((remaining-1));;
   -g|-n) ;;
   -p) if [ -n "$period" ]; then [ "$remaining" -gt 0 ] || return 2; shift; remaining=$((remaining-1)); else set -- "$@" "$argument"; fi;;
   *) set -- "$@" "$argument";;
  esac
 done
 set -- "$@" -d "$distance" -o 1 -t 0
 if [ "$signal" = 1 ]; then set -- "$@" -g; fi
 if [ -n "$period" ]; then set -- "$@" -p "$period"; fi
 if [ "$1" = launch_tof ]; then "$@"; else exec "$@"; fi
}
