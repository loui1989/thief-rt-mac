#!/bin/bash
# Launch Thief RT from Terminal, with the engine's output in logs/thief.log.
#   cd ~/Games/Thief-RT-Mac && bash run.sh                 (game folder: THIEF_DIR, or the one chosen before)
#   bash run.sh quickstart=MISS1.MIS                        (straight into a mission; other engine options too)
#   bash run.sh --gl                                        (the OpenGL renderer instead of the path tracer)
#   bash run.sh --rt                                        (the path tracer, whatever the game's Options say)
# Path tracer settings (environment): THIEF_RT_UPSCALE 0-4 (MetalFX; 3 = performance),
#   THIEF_RT_LIGHT (light brightness, 1), THIEF_RT_DEBUG=1 (statistics in logs/thief.log)
cd "$(dirname "$0")"
ROOT="$(pwd)"
BIN="$ROOT/Thief RT.app/Contents/MacOS/thief-runner"
[ -x "$BIN" ] || { echo "No build yet - run: bash setup.sh"; exit 1; }
mkdir -p logs
ARGS=()
export THIEF_RT="${THIEF_RT:-1}" THIEF_RT_DIR="${THIEF_RT_DIR:-$ROOT/rt}"
# Engine debug checks ("assertions") are written to logs/thief.log and the game carries on
# (DARK_ASSERT_ACTION=ask brings back the dialog that stops it)
export DARK_ASSERT_ACTION="${DARK_ASSERT_ACTION:-ignore}"
# OpenAL Soft warnings (audio device problems) in logs/openal.log
export ALSOFT_LOGLEVEL="${ALSOFT_LOGLEVEL:-2}" ALSOFT_LOGFILE="${ALSOFT_LOGFILE:-$ROOT/logs/openal.log}"
# The renderer chosen in the game (Options, Video: Renderer), for this start;
# --gl or --rt on the command line instead
if [ -f "$ROOT/renderer.txt" ] && grep -q '^gl' "$ROOT/renderer.txt"; then export THIEF_RT=0; fi
if [ "${1:-}" = "--gl" ]; then export THIEF_RT=0; shift; fi
if [ "${1:-}" = "--rt" ]; then export THIEF_RT=1; shift; fi
REMEMBERED="$HOME/Library/Application Support/Dark Engine Restored/game_path.txt"
GAME="${THIEF_DIR:-$(cat "$REMEMBERED" 2>/dev/null)}"
[ -z "$GAME" ] && [ -f "$HOME/Downloads/ThiefGold_Data/cam.cfg" ] && GAME="$HOME/Downloads/ThiefGold_Data"
[ -n "$GAME" ] && ARGS=("game_path=$GAME")
echo "$(date '+%Y-%m-%d %H:%M:%S') launching: ${ARGS[*]} $*" > logs/thief.log
"$BIN" "${ARGS[@]}" "$@" >> logs/thief.log 2>&1 < /dev/null
rc=$?
echo "$(date '+%Y-%m-%d %H:%M:%S') exited with $rc" >> logs/thief.log
[ -n "$GAME" ] && for f in darkengine-diagnostics.log mission_compatibility.log thief-console.log; do
  [ -f "$GAME/$f" ] && cp -f "$GAME/$f" "logs/$f"
done
exit $rc
