#!/usr/bin/env bash
# Run an ORIGINAL game (reference oracle) headless in the isolated prefix, on its own Xvfb display.
#   run_original.sh lit|tb [exe switches...]     start (returns once the game window exists); state in run/<game>.env
#   run_original.sh stop lit|tb                  stop that game, its memsrv and its Xvfb
# Default switches: LiT "/opengl /win /x640 /y480 /b00" (title, as the BugsBunny.exe launcher passes; without /b the
# exe starts level 2 LANGUAGE.BZE, absent on PC), TB "/ogl /win /x640 /y480" (logos -> GDI title menu).
# Level: LiT /bNN = level-table index 0x4ad380 (L01A = /b04); TB /bNN (table 0x4cd998, ARCA01 = /b28) or /bze:arca01.
# Env: RES=WxH (Xvfb screen, default 800x600 so the framed 640x480 client fits), AUDIO=1 keeps sound (default: off),
#      RUNNER=ge|wine (env.sh; default Proton-GE), SYNC=off -> PROTON_NO_NTSYNC/FSYNC/ESYNC=1 (Proton only).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=env.sh
. "$HERE/env.sh"
[ "${SYNC:-on}" = off ] && export PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1

stop() ( # subshell: the sourced state must not leak into a new start
  env=$W/run/$1.env
  if [ -f "$env" ]; then
    # shellcheck disable=SC1090
    . "$env"
    kill "$SRV_PID" 2>/dev/null || true
    pkill -f "$EXE" 2>/dev/null || true
    sleep 1
    pkill -9 -f "$EXE" 2>/dev/null || true
    kill "$XVFB_PID" 2>/dev/null || true
    rm -f "$env"
  fi
  # Last game of the prefix gone: kill its wineserver and every orphan (explorer.exe /desktop etc.). Orphans
  # otherwise pile up across runs (thousands of explorer.exe once exhausted RAM).
  compgen -G "$W/run/*.env" >/dev/null && exit 0
  "$WINESERVER_BIN" -k 2>/dev/null || true
  for p in $(pgrep -u "$(id -u)" -f 'wine|\.exe'); do  # wine binaries only (this script also matches 'wine')
    case $(readlink "/proc/$p/exe" 2>/dev/null) in *wine*) ;; *) continue ;; esac
    grep -qzx "WINEPREFIX=$WINEPREFIX" "/proc/$p/environ" 2>/dev/null && kill -9 "$p" 2>/dev/null
  done
  true
)

if [ "${1:-}" = stop ]; then stop "$2"; exit 0; fi

game=${1:?lit|tb}; shift
case $game in
  lit) EXE=bugs.exe;   WIN='C:\BBLIT\Datas\bin\bugs.exe';  DIR=$W/lit/Datas/bin; DEF=(/opengl /win /x640 /y480 /b00) ;;
  tb)  EXE=bblit2.exe; WIN='C:\BBTB\Datas\BIN\bblit2.exe'; DIR=$W/tb/Datas/BIN;  DEF=(/ogl /win /x640 /y480) ;;
  *) echo "game must be lit or tb" >&2; exit 2 ;;
esac
[ $# -gt 0 ] && ARGS=("$@") || ARGS=("${DEF[@]}")
stop "$game"

RES=${RES:-800x600}
DPY=$W/run/$game.dpy
: > "$DPY"
setsid Xvfb -displayfd 3 -screen 0 "${RES}x24" -nolisten tcp -noreset 3>"$DPY" >/dev/null 2>>"$W/logs/xvfb-$game.log" < /dev/null &
XVFB_PID=$!
for _ in $(seq 50); do [ -s "$DPY" ] && break; sleep 0.1; done
read -r dnum < "$DPY"
export DISPLAY=:$dnum
overrides="winemenubuilder.exe,mscoree,mshtml=d"
[ "${AUDIO:-0}" = 1 ] || overrides="$overrides;winepulse.drv,winealsa.drv=d"
export WINEDLLOVERRIDES=$overrides

LOG=$W/logs/$game-$(date +%Y%m%d-%H%M%S).log
SOCK=$W/run/$game.sock
cd "$DIR"
setsid python3 "$HERE/memsrv.py" "$SOCK" "$EXE" -- "${RUN_CMD[@]}" "$WIN" "${ARGS[@]}" >"$LOG" 2>&1 < /dev/null &
SRV_PID=$!
cat > "$W/run/$game.env" <<EOF
DISPLAY=:$dnum
XVFB_PID=$XVFB_PID
SRV_PID=$SRV_PID
EXE=$EXE
LOG=$LOG
SOCK=$SOCK
RUNNER=$RUNNER
EOF

for _ in $(seq 120); do
  wid=$(xdotool search --onlyvisible --class "steam_app_0|$EXE" 2>/dev/null | head -1 || true)
  [ -n "$wid" ] && break
  sleep 0.5
done
echo "game=$game display=:$dnum window=${wid:-none} log=$LOG args=${ARGS[*]}"
