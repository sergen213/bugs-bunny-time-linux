#!/usr/bin/env bash
# Screenshot the running original's client area:  shot.sh lit|tb [out.png]
# Default out: local/wine/shots/<game>-<time>.png. Crops the Xvfb root to the visible game window (client area;
# without a WM the 640x480 client sits at 3,29 (system Wine) / 3,22 (Proton) inside the frame Wine draws).
set -euo pipefail
W=/mnt/1st4TB/BugsBunny/local/wine
game=${1:?lit|tb}
# shellcheck disable=SC1090
. "$W/run/$game.env"
out=${2:-$W/shots/$game-$(date +%H%M%S-%N | cut -c1-10).png}
export DISPLAY
wid=$(xdotool search --onlyvisible --class "steam_app_0|$EXE" | head -1)
eval "$(xdotool getwindowgeometry --shell "$wid")"
import -silent -window root -crop "${WIDTH}x${HEIGHT}+${X}+${Y}" +repage "$out"
echo "$out"
