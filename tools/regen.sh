#!/usr/bin/env bash
# Regenerate src/recomp/gen/ from your own default.xbe.
#   tools/regen.sh <xboxrecomp dir> <game dir containing default.xbe>
set -euo pipefail
XR=$(cd "$1" && pwd); GAME=$(cd "$2" && pwd); HERE=$(cd "$(dirname "$0")/.." && pwd)
PY=${PYTHON:-python3}
mkdir -p "$XR/game_files"
cp "$GAME/default.xbe" "$XR/game_files/default.xbe"
cd "$XR"
$PY -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json > /dev/null
$PY -m tools.disasm game_files/default.xbe
$PY -m tools.func_id game_files/default.xbe
$PY -m tools.abi_analysis game_files/default.xbe
$PY "$HERE/tools/apply_names.py" tools/disasm/output/functions.json "$HERE/names/string_names.json" "$HERE/names/extra_functions.json"
rm -rf "$HERE/src/recomp/gen"; mkdir -p "$HERE/src/recomp/gen"
$PY -m tools.recomp game_files/default.xbe --all --split 250 --game-name "Red Dead Revolver" --gen-dir "$HERE/src/recomp/gen" \
    --exclude-manual "$HERE/src/rdr_hooks.c"   # its sub_X_enter: entry hooks
echo "generated: $(ls "$HERE/src/recomp/gen" | wc -l) files"
