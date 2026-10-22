#!/usr/bin/env bash
# Run the PS1 original (second reference oracle) headless in PCSX-Redux (built-in OpenBIOS), then dump state.
#   run.sh lit|tb [FRAMES]       -> $PSX_OUT/{ram.bin (2 MiB main RAM, KSEG0 0x80000000), scratch.bin, shot.ppm}
# Env: PSX_KEYS="ST*1,_*90,X*2"  pad 1 plan, BUTTONS*VSYNCS (U D L R X O S T ST SE L1 L2 R1 R2, '+' joins, '_' none)
#      PSX_LOAD=file / PSX_SAVE=file  load a save state first / write one at the end (skip the boot sequence)
#      PSX_SHOT_EVERY=N  also write shot_<vsync>.ppm every N vsyncs (menu navigation, timelines)
#      PSX_TRACE="name=0xADDR:s32*3,..."  per-vsync CSV -> $PSX_OUT/trace.csv (addresses: tools/psx/syms_<game>.txt)
#      PSX_OUT=dir (default local/psx/out/<game>), PCSX=AppImage path, PSX_DISC=<cue> (override the disc)
# Exes: LiT SLUS_008.38 (load 0x80014a00), TB SLUS_011.44; code differs from the PC exes (MIPS, PsyQ), level
# data is the same BZZ container and level-script format (bzedump parses both).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)  # /mnt/1st4TB/BugsBunny
game=${1:?usage: run.sh lit|tb [FRAMES]}
case $game in
  lit) disc="$ROOT/Bugs Bunny - Lost in Time (USA) (EnFrEs)/Bugs Bunny - Lost in Time (USA) (En,Fr,Es).cue" ;;
  tb) disc="$ROOT/Bugs Bunny  Taz - Time Busters (USA) (EnFrEs)/Bugs Bunny & Taz - Time Busters (USA) (En,Fr,Es).cue" ;;
  *) echo "unknown game $game" >&2; exit 2 ;;
esac
export PSX_FRAMES=${2:-${PSX_FRAMES:-600}}
export PSX_OUT=${PSX_OUT:-$ROOT/local/psx/out/$game}
mkdir -p "$PSX_OUT" && rm -f "$PSX_OUT"/{ram.bin,scratch.bin,shot.ppm,trace.csv} "$PSX_OUT"/shot_*.ppm
args=(-cli -stdout -lua_stdout -run -dofile "$HERE/driver.lua")
[ -z "${PSX_LEVEL:-}" ] || args=(-interpreter "${args[@]}")  # breakpoints need the interpreter
env -u WAYLAND_DISPLAY xvfb-run -a -s '-screen 0 1024x768x24' "${PCSX:-$HOME/.local/bin/PCSX-Redux-HEAD-x86_64.AppImage}" \
  -iso "${PSX_DISC:-$disc}" "${args[@]}" 2>&1 | grep --line-buffered '^psx:' || true
[ -s "$PSX_OUT/ram.bin" ] && echo "out: $PSX_OUT"
