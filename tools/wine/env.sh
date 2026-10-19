# Sourced by setup_prefix.sh / run_original.sh. Selects the Wine runner for the reference oracle.
#   RUNNER=ge   (default) Proton-GE from Steam compatibilitytools.d via `proton run` (prefix $W/ge/pfx)
#   RUNNER=wine system Wine (prefix $W/prefix)
# RUN_CMD runs an exe; WINE_BIN / WINESERVER_BIN are the runner's plain binaries (registry edits, waits).
W=/mnt/1st4TB/BugsBunny/local/wine
RUNNER=${RUNNER:-ge}
unset WINEARCH
export WINEDEBUG=${WINEDEBUG:--all}
if [ "$RUNNER" = ge ]; then
  PROTON_DIR=${PROTON_DIR:-$HOME/.local/share/Steam/compatibilitytools.d/Proton-GE Latest}
  export STEAM_COMPAT_DATA_PATH=$W/ge STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
  export SteamAppId=0 SteamGameId=0 PROTONFIXES_DISABLE=1
  export WINEPREFIX=$W/ge/pfx
  WINE_BIN=$PROTON_DIR/files/bin/wine
  WINESERVER_BIN=$PROTON_DIR/files/bin/wineserver
  RUN_CMD=("$PROTON_DIR/proton" run)
else
  export WINEPREFIX=$W/prefix
  WINE_BIN=wine
  WINESERVER_BIN=wineserver
  RUN_CMD=(wine)
fi
