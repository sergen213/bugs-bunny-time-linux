#!/usr/bin/env bash
# Scripted keyboard input to the running original (XTEST via xdotool, game window focused first).
#   send_keys.sh lit|tb STEP...
#   STEP = KEYS@DUR   hold KEYS (xdotool keysyms joined by '+', e.g. Up+d) for DUR, then release
#          _@DUR      wait
#   DUR  = N (ms) or Nt (N ticks of 1/30 s)
# LiT keys (config.pc 0x46, RESEARCH.md §6): arrows move, d jump(X), f kick, s crouch, a look, e roll, r action,
#   q/w camera R/L, Return start, Tab select.  TB: see RUNTIME_REFERENCE.md (config.pc 0x4C.. defaults).
# Example: send_keys.sh lit _@500 Up@45t d@10t _@1000
set -euo pipefail
W=/mnt/1st4TB/BugsBunny/local/wine
game=${1:?lit|tb}; shift
# shellcheck disable=SC1090
. "$W/run/$game.env"
export DISPLAY
wid=$(xdotool search --onlyvisible --class "steam_app_0|$EXE" | head -1)
xdotool windowfocus --sync "$wid" 2>/dev/null || xdotool windowfocus "$wid"
ms() { case $1 in *t) echo $(( ${1%t} * 1000 / 30 )) ;; *) echo "$1" ;; esac; }
for step in "$@"; do
  keys=${step%@*}; d=$(ms "${step#*@}")
  sec=$(printf '%d.%03d' $((d / 1000)) $((d % 1000)))
  if [ "$keys" = _ ]; then sleep "$sec"; continue; fi
  IFS=+ read -ra ks <<< "$keys"
  xdotool keydown "${ks[@]}"
  sleep "$sec"
  xdotool keyup "${ks[@]}"
done
