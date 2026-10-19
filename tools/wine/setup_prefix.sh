#!/usr/bin/env bash
# Build the isolated Wine reference environment for the ORIGINAL games (reference oracle only).
# Idempotent. Never writes into original/; never touches ~/.wine.
#   prefix      RUNNER=ge (default): Proton-GE, $W/ge/pfx; RUNNER=wine: system Wine, $W/prefix (see env.sh)
#   installs    $W/lit, $W/tb           Datas/bin copied (writable: config.pc, bugs.ini, saves), data dirs symlinked
#   CD drives   D: = $W/cd_lit "BBLIT", E: = $W/cd_tb "Bugs_Taz" (labels the exes compare, LiT 0x45f360 / TB rsrc str 5)
set -euo pipefail
ORIG=/mnt/1st4TB/BugsBunny/original
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"
export WINEDLLOVERRIDES="winemenubuilder.exe,mscoree,mshtml=d"
unset DISPLAY
mkdir -p "$W"/{logs,shots,run,csv}
if [ ! -f "$WINEPREFIX/system.reg" ]; then
  if [ "$RUNNER" = ge ]; then "${RUN_CMD[@]}" cmd /c exit; else wineboot -i; fi
fi

# install trees: exe dir copied, everything else symlinked read-only
mk_install() { # name srcDatas binName
  local dst=$W/$1/Datas
  mkdir -p "$dst"
  for e in "$2"/*; do
    local b; b=$(basename "$e")
    if [ "$b" = "$3" ]; then
      mkdir -p "$dst/$b"; cp -n "$e"/* "$dst/$b/"
    else ln -sfn "$e" "$dst/$b"; fi
  done
}
mk_install lit "$ORIG/lit/Datas" bin
mk_install tb "$ORIG/tb/Datas" BIN

# LiT config.pc (BugsDecomp CONFIG_PC, RESEARCH.md §6): renderer@4=2 (OpenGL), w@0x10=640, h@0x14=480, fullscreen@0x18=0
python3 - "$W/lit/Datas/bin/config.pc" <<'EOF'
import struct, sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
assert len(b) == 128 and b[0] == 0x10
for off, v in ((4, 2), (0x10, 640), (0x14, 480), (0x18, 0)): struct.pack_into('<I', b, off, v)
open(p, 'wb').write(b)
EOF

# CD trees: label file + Datas symlink
mkdir -p "$W/cd_lit" "$W/cd_tb"
printf 'BBLIT\n' > "$W/cd_lit/.windows-label"
printf 'Bugs_Taz\n' > "$W/cd_tb/.windows-label"
ln -sfn "$ORIG/lit/Datas" "$W/cd_lit/Datas"
ln -sfn "$ORIG/tb/Datas" "$W/cd_tb/Datas"

# drives: drop host auto-mappings, add C:\BBLIT, C:\BBTB and the two CD drives
dd=$WINEPREFIX/dosdevices
find "$dd" -maxdepth 1 -name '[d-y]:*' -exec rm -f {} +
ln -sfn "$W/cd_lit" "$dd/d:"
ln -sfn "$W/cd_tb" "$dd/e:"
ln -sfn "$W/lit" "$WINEPREFIX/drive_c/BBLIT"
ln -sfn "$W/tb" "$WINEPREFIX/drive_c/BBTB"

reg() { "$WINE_BIN" reg add "$@" /f >/dev/null; }
reg 'HKLM\Software\Wine\Drives' /v d: /d cdrom
reg 'HKLM\Software\Wine\Drives' /v e: /d cdrom
reg 'HKCU\Software\Wine\DllOverrides' /v winemenubuilder.exe /d ''
# the exes chdir to "<installation path>\bin" (LiT WinMain 0x405950; TB reads the same value under resource string 7)
reg 'HKCU\Software\Infogrames\Bugs Bunny Lost In Time' /v 'installation path' /d 'C:\BBLIT\Datas'
reg 'HKCU\Software\Infogrames\Bugs Bunny & Taz - Time Busters' /v 'installation path' /d 'C:\BBTB\Datas'
"$WINESERVER_BIN" -w
echo "prefix ready: $WINEPREFIX"
