#!/bin/bash
# Thief RT: starts the game the launcher window chose. "Thief RT.app" (made by the
# installer) runs this, from its Resources, with:
#   game=tg|t2  game_dir=<the game's folder>  fm=<a folder in its FMs, or nothing>
#   renderer=rt|gl  preset=ultra|high|medium|low|<nothing: as the game has it>
#   assert_ask=0|1
#
# Everything is in ~/Library/Application Support/Thief RT: the engine (Engine/), the path
# tracer's folder for this player (rt/: the engine's files, and the materials made from
# the games' textures, mat_dev/), the logs (logs/). The games' own files stay where they
# are; the game keeps its saves there, as it always has.

BASE="$HOME/Library/Application Support/Thief RT"
ENGINE="$BASE/Engine"
BIN="$ENGINE/Thief RT.app/Contents/MacOS/thief-runner"
OSASCRIPT="${OSASCRIPT:-/usr/bin/osascript}"

GAME=tg GAME_DIR="" FM="" RENDERER=rt PRESET="" ASSERT_ASK=0
for a in "$@"; do
  case "$a" in
    game=*) GAME="${a#game=}" ;;
    game_dir=*) GAME_DIR="${a#game_dir=}" ;;
    fm=*) FM="${a#fm=}" ;;
    renderer=*) RENDERER="${a#renderer=}" ;;
    preset=*) PRESET="${a#preset=}" ;;
    assert_ask=*) ASSERT_ASK="${a#assert_ask=}" ;;
  esac
done
case "$GAME" in t2) TITLE="Thief II" ;; *) TITLE="Thief Gold" ;; esac

alert() {
  "$OSASCRIPT" - "Thief RT" "$1" >/dev/null 2>&1 <<'OSA'
on run argv
  activate
  display dialog (item 2 of argv) with title (item 1 of argv) buttons {"OK"} default button 1 with icon caution
end run
OSA
}

if [ ! -x "$BIN" ]; then
  alert "Thief RT is not installed completely. Run \"Install Thief RT\" again."
  exit 1
fi
if [ -z "$GAME_DIR" ] || [ ! -d "$GAME_DIR" ]; then
  alert "$TITLE's files are not where they were ($GAME_DIR). Choose its folder in the launcher: File > Choose a Game Folder."
  exit 1
fi
if [ -n "$FM" ] && [ ! -d "$GAME_DIR/FMs/$FM" ]; then
  alert "The fan mission \"$FM\" is no longer in $TITLE's FMs folder."
  exit 1
fi

mkdir -p "$BASE/logs" "$BASE/rt/mat_dev"
cd "$BASE" || exit 1
# the path tracer's folder: the engine's files (linked: an update brings new ones) and
# this player's materials
for f in "$ENGINE/rt/"*; do
  n="$(basename "$f")"
  [ "$n" = mat_dev ] && continue
  [ "$(readlink "$BASE/rt/$n" 2>/dev/null)" = "$f" ] || ln -sfn "$f" "$BASE/rt/$n"
done

export THIEF_RT_DIR="$BASE/rt"
if [ "$RENDERER" = gl ]; then export THIEF_RT=0; else export THIEF_RT=1; fi
[ -n "$PRESET" ] && export THIEF_RT_PRESET="$PRESET"
# the build as shipped: the frame counter shows the frame rate alone, no performance log
export THIEF_RT_SHIPPING=1
# the engine's debug checks: written to the log and the game carries on (or stops and asks)
if [ "$ASSERT_ASK" = 1 ]; then export DARK_ASSERT_ACTION=ask; else export DARK_ASSERT_ACTION=ignore; fi
# OpenAL Soft's warnings (sound device trouble)
export ALSOFT_LOGLEVEL=2 ALSOFT_LOGFILE="$BASE/logs/openal.log"

ARGS=("game_path=$GAME_DIR")
# a fan mission from the game's FMs folder, or the game itself with its mods (as its cam_mod.ini has them)
if [ -n "$FM" ]; then ARGS+=("fm=$FM"); else ARGS+=("content=configured"); fi

LOG="$BASE/logs/thief.log"
echo "$(date '+%Y-%m-%d %H:%M:%S') Thief RT $(cat "$ENGINE/VERSION" 2>/dev/null): $TITLE${FM:+, $FM}, ${RENDERER}${PRESET:+, $PRESET}" > "$LOG"
echo "  ${ARGS[*]}" >> "$LOG"
"$BIN" "${ARGS[@]}" >> "$LOG" 2>&1 < /dev/null
rc=$?
echo "$(date '+%Y-%m-%d %H:%M:%S') exited with $rc" >> "$LOG"
# the engine's own logs, written in the game's folder
for f in darkengine-diagnostics.log mission_compatibility.log thief-console.log; do
  [ -f "$GAME_DIR/$f" ] && cp -f "$GAME_DIR/$f" "$BASE/logs/$f"
done
if [ $rc -ne 0 ] && [ $rc -ne 130 ] && [ $rc -ne 143 ]; then
  alert "$TITLE closed with an error ($rc). What happened is in the logs: $BASE/logs (thief.log). The launcher's File menu shows them."
fi
exit $rc
