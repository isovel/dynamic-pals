#!/usr/bin/env bash
# Deploy the offline build into Palworld's UE4SS Mods folder as main.dll.
# Override the destination with UE4SS_MODS_DIR.
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/deps/pal-ui-lib/tools/progress.sh"

MOD_NAME="DynamicPals"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODS_DIR="${UE4SS_MODS_DIR:-/mnt/s/SteamLibrary/steamapps/common/Palworld/Pal/Binaries/Win64/ue4ss/Mods}"
DEST="$MODS_DIR/$MOD_NAME"
DLL="$SRC_DIR/dlls/main_offline.dll"

[[ -f "$DLL" ]] || { say_fail "dlls/main_offline.dll not found - build first"; exit 1; }
[[ -d "$MODS_DIR" ]] || { say_fail "mods dir not found: $MODS_DIR"; exit 1; }

mkdir -p "$DEST/dlls"
touch "$DEST/enabled.txt"

# Copy over the target: renaming onto an existing file fails on WSL drvfs mounts,
# and Windows locks main.dll while the game runs.
if ! cp -f "$DLL" "$DEST/dlls/main.dll" 2>/dev/null; then
    say_fail "could not write $DEST/dlls/main.dll (is Palworld running?)"
    exit 1
fi
cp -f "$SRC_DIR/dlls/DynamicPals.pak" "$DEST/dlls/DynamicPals.pak"

say_ok "Installed $MOD_NAME (offline)" "$(short_path "$DEST" 3)"
