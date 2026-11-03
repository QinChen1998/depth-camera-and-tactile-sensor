# DCTS automatic dToF preset v1
dcts_tof(){
 preset=$1; mode=$2; shift 2
 case "$preset:$mode" in
  :*) if [ "$1" = launch_tof ]; then "$@"; else exec "$@"; fi; return;;
  16:14) distance=5;;
  48:13) distance=11;;
  *) return 2;;
 esac
 remaining=$#
 while [ "$remaining" -gt 0 ]; do
  argument=$1; shift; remaining=$((remaining-1))
  case "$argument" in
   -d|-o|-t) [ "$remaining" -gt 0 ] || return 2; shift; remaining=$((remaining-1));;
   -g|-n) ;;
   *) set -- "$@" "$argument";;
  esac
 done
 set -- "$@" -d "$distance" -o 1 -t 0
 if [ "$preset" = 48 ]; then set -- "$@" -g; fi
 if [ "$1" = launch_tof ]; then "$@"; else exec "$@"; fi
}
