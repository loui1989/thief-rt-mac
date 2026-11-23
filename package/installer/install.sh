#!/bin/bash
# Thief RT installer: what "Install Thief RT.app" runs.
#
# Finds the games' files on this Mac -- Thief Gold (or Thief: The Dark Project) and
# Thief II -- and The Black Parade (a Thief Gold fan mission: in Thief Gold's FMs
# folder, or downloaded), asks which to take, then installs:
#   ~/Library/Application Support/Thief RT/Engine   the engine and the path tracer
#   ~/Library/Application Support/Thief RT          games.conf (where the games are), logs/
#   /Applications/Thief RT.app                      the launcher (~/Applications if
#                                                   /Applications is not writable)
# Running it again updates the engine and keeps the settings, the saves and the materials.
#
# install.sh --update (the launcher's Install Update, from the new version's disk image):
# the engine and the launcher replaced, nothing asked and nothing else changed (the games,
# the mods, the settings kept); THIEF_RT_APPS: the folder the launcher is in.
#
# The launcher runs this too (install.sh --setup, from its Resources): the finding part
# alone -- the games, The Black Parade, the HD Mod -- when the installer could not look
# (macOS keeps an app from a disk image, not yet installed, out of Downloads, Documents
# and Desktop: the installed launcher is asked, and listed in the privacy settings), and
# from its File menu (Find Games Again).
#
# Nothing of the games is copied: the launcher plays them where they are (and the game
# keeps its saves there, as it always has). The Black Parade, when it was found outside
# Thief Gold's FMs folder, is put in it.

# Under Rosetta (this script's bash started as an Intel process): again, natively
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ] && [ -z "${THIEF_RT_NATIVE:-}" ] && [ -x /usr/bin/arch ]; then
  THIEF_RT_NATIVE=1 exec /usr/bin/arch -arm64 /bin/bash "$0" "$@"
fi

SETUP=""; [ "${1:-}" = --setup ] && SETUP=1
UPDATE=""; [ "${1:-}" = --update ] && UPDATE=1
ME="Install Thief RT"; [ -n "$SETUP" ] && ME="Thief RT"
RES="$(cd "$(dirname "$0")/../Resources" 2>/dev/null && pwd)"
PAYLOAD="$RES/Payload"
DEST="${THIEF_RT_DEST:-$HOME/Library/Application Support/Thief RT}"
APPS="${THIEF_RT_APPS:-/Applications}"
OSASCRIPT="${OSASCRIPT:-/usr/bin/osascript}"
DITTO="${DITTO:-/usr/bin/ditto}"
TITLE="Thief RT"
TILDE="~"   # (for shortening paths: a ~ written in ${x/#$HOME/~} is expanded by bash 3.2)
LOG="$(mktemp "${TMPDIR:-/tmp}/thiefrt-install.XXXXXX")"
exec 3>>"$LOG"

# --- dialogs (AppleScript, its text given as arguments: no quoting trouble) -----------

osa() { "$OSASCRIPT" - "$@"; }

# message <text> [button...]: the button pressed (the last one is the default)
message() {
  osa "$TITLE" "$@" <<'OSA'
on run argv
  set b to {"OK"}
  if (count of argv) > 2 then set b to items 3 thru -1 of argv
  activate
  set r to display dialog (item 2 of argv) with title (item 1 of argv) buttons b default button (count of b)
  return button returned of r
end run
OSA
}

# pick <prompt> <multiple 0|1> <preselected...> -- <items...>: the chosen items, one per
# line (nothing if none), or __quit__
pick() {
  local out
  out="$(pick_osa "$@" 2>>"$LOG")" || { echo "__quit__"; return; }
  printf '%s\n' "$out"
}
pick_osa() {
  osa "$TITLE" "$@" <<'OSA'
on run argv
  set p to item 2 of argv
  set m to (item 3 of argv) is "1"
  set pre to {}
  set choices to {}
  set seenSep to false
  repeat with i from 4 to count of argv
    set a to item i of argv
    if a is "--" then
      set seenSep to true
    else if seenSep then
      set end of choices to a
    else
      set end of pre to a
    end if
  end repeat
  activate
  set c to choose from list choices with title (item 1 of argv) with prompt p default items pre multiple selections allowed m empty selection allowed true OK button name "Continue" cancel button name "Quit"
  if c is false then return "__quit__"
  set AppleScript's text item delimiters to linefeed
  return c as text
end run
OSA
}

choose_folder() {
  osa "$1" <<'OSA'
on run argv
  activate
  try
    return POSIX path of (choose folder with prompt (item 1 of argv))
  on error
    return ""
  end try
end run
OSA
}

notify() {
  osa "$TITLE" "$1" <<'OSA' >/dev/null 2>&1
on run argv
  display notification (item 2 of argv) with title (item 1 of argv)
end run
OSA
}

fail() {
  message "$1

(The details are in $LOG.)" "OK" >/dev/null
  exit 1
}

# --- where things are ----------------------------------------------------------------

# a folder's file by its name in any case: its path
file_in() { find "$1" -maxdepth 1 -iname "$2" 2>/dev/null | head -1; }

# tg (Thief Gold / The Dark Project), t2 (Thief II), or nothing: what game a folder has
game_kind() {
  local d="$1"
  [ -n "$(file_in "$d" cam.cfg)" ] || return 0
  if [ -n "$(file_in "$d" thief2.exe)$(file_in "$d" t2.exe)" ]; then echo t2; return; fi
  if [ -n "$(file_in "$d" thief.exe)" ]; then echo tg; return; fi
  # by the missions: Thief II has a miss8 and no miss3; Thief has a miss3
  if [ -n "$(file_in "$d" miss8.mis)" ] && [ -z "$(file_in "$d" miss3.mis)" ]; then echo t2; return; fi
  if [ -n "$(file_in "$d" miss3.mis)$(file_in "$d" miss18.mis)$(file_in "$d" miss1.mis)" ]; then echo tg; return; fi
}

# the folders under <roots> with a cam.cfg, nearest first
game_dirs() {
  local r
  for r in "$@"; do
    [ -d "$r" ] || continue
    find "$r" -maxdepth 6 \( -name '.Trash' -o -name 'Thief RT' -o -name 'Xcode*.app' -o -name '*.photoslibrary' \
      -o -name 'FMs' -o -name 'node_modules' -o -name '.git' \) -prune -o -type f -iname cam.cfg -print 2>>"$LOG"
  done | while IFS= read -r f; do dirname "$f"; done | awk '!seen[$0]++'
}

DOWNLOADS="$HOME/Downloads"
# how to get innoextract (for the HD Mods' Windows installers): Homebrew; an Intel
# Homebrew (in /usr/local, run under Rosetta) refuses a plain "brew install" on Apple silicon
BREW_INNO="brew install sevenzip innoextract"
if [ ! -x /opt/homebrew/bin/brew ] && [ -x /usr/local/bin/brew ]; then BREW_INNO="arch -x86_64 /usr/local/bin/brew install sevenzip innoextract"; fi
GAME_ROOTS=("$HOME/Games" "$HOME/Library/Application Support/Steam/steamapps/common"
            "$HOME/Library/Application Support/Steam/SteamApps/common" "$HOME/Applications" "/Applications"
            "$DOWNLOADS" "$HOME/Documents" "$HOME/Desktop"
            "$HOME/Library/Application Support/CrossOver/Bottles" "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Bottles")

# --- the engine and the launcher ------------------------------------------------------

# the engine (replaced whole: an update), and the path tracer's folder for this player: the
# engine's files linked in, and the materials made from the textures (kept: they take a
# while to make)
install_engine() {
  rm -rf "$DEST/Engine.new"
  "$DITTO" "$PAYLOAD/Engine" "$DEST/Engine.new" >&3 2>&1 || return 1
  rm -rf "$DEST/Engine" && mv "$DEST/Engine.new" "$DEST/Engine" || return 1
  mkdir -p "$DEST/rt/mat_dev" "$DEST/logs"
  for f in "$DEST/Engine/rt/"*; do ln -sfn "$f" "$DEST/rt/$(basename "$f")"; done
  [ -f "$DEST/renderer.txt" ] || echo rt > "$DEST/renderer.txt"
  return 0
}

# the launcher, in $APPS (~/Applications if that is not writable): $A
install_launcher() {
  if [ ! -w "$APPS" ]; then APPS="$HOME/Applications"; mkdir -p "$APPS"; fi
  A="$APPS/Thief RT.app"
  rm -rf "$A.new"
  "$DITTO" "$PAYLOAD/Thief RT.app" "$A.new" >&3 2>&1 || return 1
  rm -rf "$A" && mv "$A.new" "$A" || return 1
  touch "$A"
  # copied from a downloaded disk image: not to be held up by Gatekeeper again
  xattr -dr com.apple.quarantine "$DEST" "$A" >&3 2>&1
  return 0
}

if [ -n "$UPDATE" ]; then
  echo "Thief RT update: $(cat "$PAYLOAD/Engine/VERSION" 2>/dev/null) (log: $LOG)"
  [ -x "$PAYLOAD/Engine/Thief RT.app/Contents/MacOS/thief-runner" ] || { echo "This disk image is incomplete (its Payload is missing)."; exit 1; }
  [ -d "$DEST/Engine" ] || { echo "Thief RT is not installed here ($DEST): install it with Install Thief RT."; exit 1; }
  install_engine || { echo "Could not copy the engine (the details are in $LOG)."; cat "$LOG"; exit 1; }
  install_launcher || { echo "Could not replace the launcher in $APPS (the details are in $LOG)."; cat "$LOG"; exit 1; }
  echo "Updated: $A"
  exit 0
fi

# --- start ---------------------------------------------------------------------------

if [ -z "$SETUP" ]; then
[ -x "$PAYLOAD/Engine/Thief RT.app/Contents/MacOS/thief-runner" ] || fail "This installer is incomplete (its Payload is missing)."

# (Apple silicon by the hardware: uname -m says x86_64 when this script runs under
# Rosetta -- as an app made of a script can be started, on a Mac that has Rosetta --
# and an M4 was told it had an Intel processor)
if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" != 1 ] && [ "$(uname -m)" != arm64 ] && [ -z "$THIEF_RT_TEST" ]; then
  message "Thief RT needs a Mac with Apple silicon (M1 or later). This Mac has an Intel processor." "Quit" >/dev/null
  exit 1
fi
MACOS="$(sw_vers -productVersion 2>/dev/null)"
if [ -n "$MACOS" ] && [ "${MACOS%%.*}" -lt 14 ] 2>/dev/null; then
  [ "$(message "Thief RT was made for macOS 14 or newer; this Mac has macOS $MACOS. It may not run." "Quit" "Install anyway")" = "Install anyway" ] || exit 0
fi

r="$(message "Thief RT: Thief Gold and Thief II -- and their fan missions, The Black Parade among them -- with path-traced lighting, for Apple silicon Macs.

The installer looks for your copies of the games on this Mac (from Steam or GOG: the game's folder, copied from a PC or a Windows install), and for The Black Parade and the Thief Gold and Thief II HD Mods in your Downloads, then puts a launcher in Applications.

macOS may ask whether the installer can read your Downloads and Documents folders: that is where it looks." "Quit" "Continue")"
[ "$r" = "Continue" ] || exit 0
else
  r="$(message "Thief RT looks for your games now: Thief Gold and Thief II (the game's folder, from Steam or GOG), and The Black Parade and the HD Mods (Thief Gold, Thief II) in your Downloads.

macOS asks whether Thief RT may read your Downloads, Documents and Desktop folders: allow it, so it can look there (and the games can be played from there)." "Quit" "Continue")"
  [ "$r" = "Continue" ] || exit 0
fi

# macOS's privacy protection: Downloads, Documents and Desktop are read only once the
# player allows it (the first look asks). Refused (or asked before and refused), the
# search there finds nothing at all -- so it is checked, and said.
blocked=""
for r in "$DOWNLOADS" "$HOME/Documents" "$HOME/Desktop"; do
  [ -d "$r" ] || continue
  ls "$r" >/dev/null 2>>"$LOG" || blocked="$blocked
   ${r/#$HOME/$TILDE}"
done
echo "not readable:$blocked" >&3
# (the installer, kept out: Thief RT looks when it is first opened -- it can be allowed)
DEFER=""
if [ -n "$blocked" ] && [ -z "$SETUP" ]; then DEFER=1; fi
if [ -n "$blocked" ] && [ -n "$SETUP" ]; then
  r="$(message "macOS did not let Thief RT look in:$blocked

That is where it looks for the games, The Black Parade and the HD Mods. To allow it: System Settings > Privacy & Security > Files & Folders > Thief RT: turn those folders on, then choose File > Find Games Again in Thief RT.

Or carry on and show it the games' folders yourself." "Quit" "Open Settings" "Carry on")"
  case "$r" in
    "Open Settings") open "x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders"; exit 0 ;;
    "Carry on") ;;
    *) exit 0 ;;
  esac
fi

TG_DIR="" T2_DIR="" TAKE_BP="" TAKE_HD="" BP_HAVE="" HD_HAVE="" HD_NOTE=""
TAKE_T2HD="" T2HD_HAVE="" T2HD_SRC="" T2HD_NOTE=""
if [ -z "$DEFER" ]; then
notify "Looking for the games..."

# --- the games -----------------------------------------------------------------------

# what was found before (an update): first
if [ -f "$DEST/games.conf" ]; then
  old_tg="$(sed -n 's/^TG_DIR=//p' "$DEST/games.conf")"
  old_t2="$(sed -n 's/^T2_DIR=//p' "$DEST/games.conf")"
  [ -n "$old_tg" ] && [ "$(game_kind "$old_tg")" = tg ] && TG_DIR="$old_tg"
  [ -n "$old_t2" ] && [ "$(game_kind "$old_t2")" = t2 ] && T2_DIR="$old_t2"
fi
# (a folder with mission files and a Thief binary: the one; else the first)
while IFS= read -r d; do
  case "$(game_kind "$d")" in
    tg) [ -z "$TG_DIR" ] && TG_DIR="$d" ;;
    t2) [ -z "$T2_DIR" ] && T2_DIR="$d" ;;
  esac
  [ -n "$TG_DIR" ] && [ -n "$T2_DIR" ] && break
done < <(game_dirs "${GAME_ROOTS[@]}")
echo "found: TG=$TG_DIR T2=$T2_DIR" >&3

TG_FOUND="Thief Gold  (found: ${TG_DIR/#$HOME/$TILDE})"
TG_LOCATE="Thief Gold  (I'll show you where its files are)"
T2_FOUND="Thief II  (found: ${T2_DIR/#$HOME/$TILDE})"
T2_LOCATE="Thief II  (I'll show you where its files are)"
items=(); pre=()
if [ -n "$TG_DIR" ]; then items+=("$TG_FOUND"); pre+=("$TG_FOUND"); else items+=("$TG_LOCATE"); fi
if [ -n "$T2_DIR" ]; then items+=("$T2_FOUND"); pre+=("$T2_FOUND"); else items+=("$T2_LOCATE"); fi

chosen="$(pick "Which games do you want to play with Thief RT?

A game not found can be located by hand: its folder is the one with cam.cfg and the missions (miss1.mis ...) in it. (Thief: The Dark Project counts as Thief Gold here.)" 1 "${pre[@]}" -- "${items[@]}")"
[ "$chosen" = "__quit__" ] && exit 0

# locate <tg|t2> <name>: a folder chosen by hand, checked
locate() {
  local want="$1" name="$2" d k
  while :; do
    d="$(choose_folder "Choose the $name folder (the one with cam.cfg and the missions in it):")"
    [ -n "$d" ] || return 1
    d="${d%/}"
    if ! ls "$d" >/dev/null 2>>"$LOG"; then
      [ "$(message "macOS did not let $ME read that folder. Allow it in System Settings > Privacy & Security > Files & Folders > $ME (or move the game's folder to a folder in your home folder, such as Games)." "Skip $name" "Choose again")" = "Choose again" ] || return 1
      continue
    fi
    k="$(game_kind "$d")"
    if [ "$k" = "$want" ]; then echo "$d"; return 0; fi
    if [ -n "$k" ]; then
      [ "$(message "That folder has the other game in it, not $name." "Skip $name" "Choose again")" = "Choose again" ] || return 1
    else
      [ "$(message "There is no Thief game in that folder (no cam.cfg and missions)." "Skip $name" "Choose again")" = "Choose again" ] || return 1
    fi
  done
}

WANT_TG="" WANT_T2=""
while IFS= read -r c; do
  case "$c" in
    "$TG_FOUND") WANT_TG=1 ;;
    "$TG_LOCATE") d="$(locate tg "Thief Gold")" && { TG_DIR="$d"; WANT_TG=1; } ;;
    "$T2_FOUND") WANT_T2=1 ;;
    "$T2_LOCATE") d="$(locate t2 "Thief II")" && { T2_DIR="$d"; WANT_T2=1; } ;;
  esac
done <<< "$chosen"
if [ -z "$WANT_TG$WANT_T2" ] && [ -n "$SETUP" ]; then message "No game was chosen." >/dev/null; exit 0; fi
[ -n "$WANT_TG" ] || TG_DIR=""
[ -n "$WANT_T2" ] || T2_DIR=""

# --- The Black Parade -----------------------------------------------------------------

# in Thief Gold's FMs: a folder with its first mission's game file (TBPGAM1.gam)
bp_in_fms() {
  local d
  for d in "$1/FMs"/*/; do
    [ -d "$d" ] || continue
    [ -n "$(file_in "$d" TBPGAM1.gam)" ] && { echo "${d%/}"; return; }
  done
}
BP_HAVE="" BP_SRC=""
if [ -n "$TG_DIR" ]; then
  BP_HAVE="$(bp_in_fms "$TG_DIR")"
  if [ -z "$BP_HAVE" ]; then
    notify "Looking for The Black Parade..."
    # a folder (unpacked) under Downloads or Games, or its archive in Downloads
    d="$(for r in "$DOWNLOADS" "$HOME/Games" "$HOME/Documents" "$HOME/Desktop"; do
           [ -d "$r" ] && find "$r" -maxdepth 5 -path "$TG_DIR" -prune -o -type f -iname TBPGAM1.gam -print 2>/dev/null
         done | head -1)"
    if [ -n "$d" ]; then BP_SRC="dir:$(dirname "$d")"
    else
      for a in "$DOWNLOADS"/*[Bb]lack*[Pp]arade*.zip "$DOWNLOADS"/*[Bb]lack*[Pp]arade*.7z "$DOWNLOADS"/*[Bb][Pp]*.7z "$DOWNLOADS"/*TBP*.zip "$DOWNLOADS"/*TBP*.7z; do
        [ -f "$a" ] || continue
        if tar -tf "$a" 2>>"$LOG" | grep -qi 'tbpgam1\.gam'; then BP_SRC="archive:$a"; break; fi
      done
    fi
  fi
fi
echo "black parade: have=$BP_HAVE src=$BP_SRC" >&3

TAKE_BP=""
if [ -n "$BP_SRC" ]; then
  src="${BP_SRC#*:}"
  r="$(message "The Black Parade was found: ${src/#$HOME/$TILDE}

Put it in Thief Gold's FMs folder, so the launcher can play it?" "Skip" "Install it")"
  [ "$r" = "Install it" ] && TAKE_BP=1
fi

# --- the Thief Gold HD Mod --------------------------------------------------------------
# Its HDMOD folder in Thief Gold's folder, searched by the game as a mod (cam_mod.ini
# mod_path). Found as: the folder already there; an HDMOD folder elsewhere (copied from a
# Windows install); an archive holding one; or its Windows installer (Thief1HDMod1.2.exe).

# The HD Mods' Windows installers are NSIS installers (Thief 2 HD Mod 1.0: unpacked with
# 7-Zip) or Inno Setup ones (unpacked with innoextract). Both are Thief RT's own (in this
# app's Resources; or in the kit, next to this script's folder), copied out and their
# download quarantine taken off, so macOS runs them; else Homebrew's.
own_tool() {   # $1: its name in Resources, $2: its path in the kit, $3: a test argument
  local x d
  for x in "$RES/$1" "$(dirname "$0")/../$2"; do
    [ -f "$x" ] || continue
    d="$(mktemp -d "${TMPDIR:-/tmp}/thiefrt-tool.XXXXXX")"
    if cp "$x" "$d/$1" 2>>"$LOG"; then
      xattr -c "$d/$1" 2>/dev/null
      chmod +x "$d/$1"
      "$d/$1" $3 >&3 2>&1 && { echo "$d/$1"; return; }
    fi
  done
}
INNOEXTRACT="$(own_tool innoextract innoextract/innoextract --version)"
[ -z "$INNOEXTRACT" ] && INNOEXTRACT="$(command -v innoextract 2>/dev/null)"
for x in /opt/homebrew/bin/innoextract /usr/local/bin/innoextract; do
  [ -z "$INNOEXTRACT" ] && [ -x "$x" ] && INNOEXTRACT="$x"
done
SEVENZIP="$(own_tool 7zz 7zip/7zz i)"
[ -z "$SEVENZIP" ] && SEVENZIP="$(command -v 7zz 2>/dev/null)"
for x in /opt/homebrew/bin/7zz /usr/local/bin/7zz; do
  [ -z "$SEVENZIP" ] && [ -x "$x" ] && SEVENZIP="$x"
done

# a Windows installer's files, one per line (each from a /), after the line "nsis" or
# "inno" (how it is unpacked); nothing if it is neither
exe_files() {
  local l
  if [ -n "$SEVENZIP" ] && l="$("$SEVENZIP" l -slt "$1" 2>>"$LOG")" && printf '%s\n' "$l" | grep -qx 'Type = Nsis'; then
    echo nsis
    printf '%s\n' "$l" | sed -n 's|^Path = |/|p'
  elif [ -n "$INNOEXTRACT" ] && l="$("$INNOEXTRACT" -l "$1" 2>>"$LOG")"; then
    echo inno
    printf '%s\n' "$l"
  fi
}

# an HDMOD folder: one with the mod's objects or textures in it
is_hdmod() { [ -d "$1" ] && [ -n "$(file_in "$1" obj)$(file_in "$1" fam)" ]; }
# which game's HD Mod an HDMOD folder is: Thief II's has its own texture families (the
# mansions' marble floors, the Mechanists'), Thief Gold's not
hdmod_game() {
  local f; f="$(file_in "$1" fam)"
  if [ -n "$f" ] && [ -n "$(file_in "$f" mbfloor)$(file_in "$f" vicm01)$(file_in "$f" newmech)$(file_in "$f" civicbui)" ]; then
    echo t2
  else
    echo tg
  fi
}
# the same, from the list of an archive's or an installer's files (on stdin)
listed_game() { if grep -qiE '/fam/(mbfloor|vicm01|newmech|civicbui)/'; then echo t2; else echo tg; fi; }

HD_HAVE="" HD_SRC="" HD_NOTE=""
if [ -n "$TG_DIR" ]; then
  d="$(file_in "$TG_DIR" HDMOD)"
  if [ -n "$d" ] && is_hdmod "$d"; then HD_HAVE="$d"
  else
    notify "Looking for the Thief Gold HD Mod..."
    while IFS= read -r d; do
      is_hdmod "$d" && [ "$(hdmod_game "$d")" = tg ] && { HD_SRC="dir:$d"; break; }
    done < <(for r in "$DOWNLOADS" "$HOME/Games" "$HOME/Documents" "$HOME/Desktop"; do
               [ -d "$r" ] && find "$r" -maxdepth 5 \( -path "$TG_DIR" -o -path "${T2_DIR:-/nonexistent}" -o -name 'Thief RT' -o -name '.Trash' \) -prune \
                 -o -type d -iname HDMOD -print 2>/dev/null
             done)
    if [ -z "$HD_SRC" ]; then
      for a in "$DOWNLOADS"/*.zip "$DOWNLOADS"/*.7z "$DOWNLOADS"/*.rar "$DOWNLOADS"/*.exe; do
        [ -f "$a" ] || continue
        case "$(basename "$a" | tr '[:upper:]' '[:lower:]')" in
          *thief2*|*thief?2*|*thief?ii*|t2*) continue ;;   # (Thief II's: below)
          *hd*mod*|*hdmod*|thief1hd*) ;;
          *) continue ;;
        esac
        lst="$(tar -tf "$a" 2>/dev/null)"
        if printf '%s\n' "$lst" | grep -qiE '(^|/)hdmod/(obj|fam)/' && [ "$(printf '%s\n' "$lst" | listed_game)" = tg ]; then HD_SRC="archive:$a"; break; fi
        case "$a" in
          *.exe|*.EXE)
            if [ -n "$SEVENZIP$INNOEXTRACT" ]; then
              lst="$(exe_files "$a")"
              kind="$(printf '%s\n' "$lst" | head -1)"
              if [ -n "$kind" ] && printf '%s\n' "$lst" | grep -qiE '[/"]hdmod/(obj|fam)/' && [ "$(printf '%s\n' "$lst" | listed_game)" = tg ]; then HD_SRC="$kind:$a"; break; fi
            else
              HD_NOTE="

The Thief Gold HD Mod's installer is in your Downloads ($(basename "$a")), but it is a Windows installer, and the tools to unpack it are missing from this copy of Thief RT: download Thief RT again, or install them (in Terminal: $BREW_INNO), then choose File > Find Games Again in Thief RT."
            fi ;;
        esac
      done
    fi
  fi
fi
echo "hd mod: have=$HD_HAVE src=$HD_SRC 7-zip=$SEVENZIP innoextract=$INNOEXTRACT" >&3

TAKE_HD=""
if [ -n "$HD_SRC" ]; then
  src="${HD_SRC#*:}"
  r="$(message "The Thief Gold HD Mod was found: ${src/#$HOME/$TILDE}

Install it into Thief Gold's folder? Its hand-made HD textures, models and skies are then used in the original missions and in fan missions (about 3 GB; unpacking takes a few minutes)." "Skip" "Install it")"
  [ "$r" = "Install it" ] && TAKE_HD=1
fi

# --- the Thief II HD Mod ----------------------------------------------------------------
# The same for Thief II: its HD Mod (Thief2HDMod, Bentraxx's) in Thief II's folder, found
# as its HDMOD folder, an archive holding one or its Windows installer (innoextract).

T2HD_HAVE="" T2HD_SRC="" T2HD_NOTE=""
if [ -n "$T2_DIR" ]; then
  d="$(file_in "$T2_DIR" HDMOD)"
  if [ -n "$d" ] && is_hdmod "$d"; then T2HD_HAVE="$d"
  else
    notify "Looking for the Thief II HD Mod..."
    while IFS= read -r d; do
      is_hdmod "$d" && [ "$(hdmod_game "$d")" = t2 ] && { T2HD_SRC="dir:$d"; break; }
    done < <(for r in "$DOWNLOADS" "$HOME/Games" "$HOME/Documents" "$HOME/Desktop"; do
               [ -d "$r" ] && find "$r" -maxdepth 5 \( -path "$T2_DIR" -o -path "${TG_DIR:-/nonexistent}" -o -name 'Thief RT' -o -name '.Trash' \) -prune \
                 -o -type d -iname HDMOD -print 2>/dev/null
             done)
    if [ -z "$T2HD_SRC" ]; then
      for a in "$DOWNLOADS"/*.zip "$DOWNLOADS"/*.7z "$DOWNLOADS"/*.rar "$DOWNLOADS"/*.exe; do
        [ -f "$a" ] || continue
        case "$(basename "$a" | tr '[:upper:]' '[:lower:]')" in *hd*) ;; *) continue ;; esac
        lst="$(tar -tf "$a" 2>/dev/null)"
        if printf '%s\n' "$lst" | grep -qiE '(^|/)hdmod/(obj|fam)/' && [ "$(printf '%s\n' "$lst" | listed_game)" = t2 ]; then T2HD_SRC="archive:$a"; break; fi
        case "$a" in
          *.exe|*.EXE)
            if [ -n "$SEVENZIP$INNOEXTRACT" ]; then
              lst="$(exe_files "$a")"
              kind="$(printf '%s\n' "$lst" | head -1)"
              if [ -n "$kind" ] && printf '%s\n' "$lst" | grep -qiE '[/"]hdmod/(obj|fam)/' && [ "$(printf '%s\n' "$lst" | listed_game)" = t2 ]; then T2HD_SRC="$kind:$a"; break; fi
            else
              case "$(basename "$a" | tr '[:upper:]' '[:lower:]')" in
                *thief2*|*thief?2*|*thief?ii*|t2*) T2HD_NOTE="

The Thief II HD Mod's installer is in your Downloads ($(basename "$a")), but it is a Windows installer, and the tools to unpack it are missing from this copy of Thief RT: download Thief RT again, or install them (in Terminal: $BREW_INNO), then choose File > Find Games Again in Thief RT." ;;
              esac
            fi ;;
        esac
      done
    fi
  fi
fi
echo "t2 hd mod: have=$T2HD_HAVE src=$T2HD_SRC" >&3

TAKE_T2HD=""
if [ -n "$T2HD_SRC" ]; then
  src="${T2HD_SRC#*:}"
  r="$(message "The Thief II HD Mod was found: ${src/#$HOME/$TILDE}

Install it into Thief II's folder? Its HD textures, objects, skies and water are then used in the original missions and in fan missions (about 5 GB; unpacking takes several minutes)." "Skip" "Install it")"
  [ "$r" = "Install it" ] && TAKE_T2HD=1
fi

fi   # (DEFER)

# --- install -------------------------------------------------------------------------

mkdir -p "$DEST" || fail "Could not make $DEST."
if [ -z "$SETUP" ]; then
notify "Installing... (this takes a few seconds)"

install_engine || fail "Could not copy the engine."
fi

if [ -z "$DEFER" ]; then
  {
    [ -n "$TG_DIR" ] && echo "TG_DIR=$TG_DIR"
    [ -n "$T2_DIR" ] && echo "T2_DIR=$T2_DIR"
  } > "$DEST/games.conf"
fi

# The Black Parade into Thief Gold's FMs
BP_NOTE=""
if [ -n "$TAKE_BP" ]; then
  notify "Putting The Black Parade in Thief Gold's FMs..."
  src="${BP_SRC#*:}"
  FMS="$TG_DIR/FMs"
  mkdir -p "$FMS"
  if [ "${BP_SRC%%:*}" = archive ]; then
    t="$(mktemp -d "${TMPDIR:-/tmp}/thiefrt-bp.XXXXXX")"
    if tar -xf "$src" -C "$t" >&3 2>&1; then
      g="$(find "$t" -type f -iname TBPGAM1.gam | head -1)"
      if [ -n "$g" ]; then
        top="$(dirname "$g")"
        name="$(basename "$src")"; name="${name%.*}"
        [ "$top" != "$t" ] && name="$(basename "$top")"
        rm -rf "$FMS/$name.new"
        "$DITTO" "$top" "$FMS/$name.new" >&3 2>&1 && mv "$FMS/$name.new" "$FMS/$name" && BP_HAVE="$FMS/$name"
      fi
    fi
    rm -rf "$t"
  else
    name="$(basename "$src")"
    "$DITTO" "$src" "$FMS/$name" >&3 2>&1 && BP_HAVE="$FMS/$name"
  fi
  [ -n "$BP_HAVE" ] || BP_NOTE="

The Black Parade could not be put in Thief Gold's FMs folder (the details are in $LOG): unpack it into a folder of its own there."
fi

# The HD Mod into Thief Gold's folder (HDMOD), its fan-mission variants (HDMOD_FM), and
# cam_mod.ini to use them
if [ -n "$TAKE_HD" ]; then
  notify "Installing the Thief Gold HD Mod... (a few minutes)"
  src="${HD_SRC#*:}"
  # (unpacked next to the game: on the same disk, so it is moved, not copied again)
  t="$(mktemp -d "$TG_DIR/.thiefrt-hdmod.XXXXXX")"
  ok=""
  case "${HD_SRC%%:*}" in
    dir) "$DITTO" "$src" "$t/HDMOD" >&3 2>&1 && ok=1 ;;
    archive) tar -xf "$src" -C "$t" >&3 2>&1 && ok=1 ;;
    inno) "$INNOEXTRACT" -s -d "$t" "$src" >&3 2>&1 && ok=1 ;;
    # (an installer's choices are all in it: the first of each file is kept, the
    # installer's default)
    nsis) "$SEVENZIP" x -aos -y -bd -bsp0 -o"$t" "$src" >&3 2>&1; [ $? -le 1 ] && ok=1 ;;
  esac
  hd=""
  if [ -n "$ok" ]; then
    while IFS= read -r d; do is_hdmod "$d" && { hd="$d"; break; }; done < <(find "$t" -maxdepth 4 -type d -iname HDMOD)
  fi
  if [ -n "$hd" ]; then
    old="$(file_in "$TG_DIR" HDMOD)"
    [ -n "$old" ] && mv "$old" "$TG_DIR/HDMOD.before-thief-rt.$$"
    mv "$hd" "$TG_DIR/HDMOD" && HD_HAVE="$TG_DIR/HDMOD"
  else
    HD_NOTE="

The HD Mod could not be installed (no HDMOD folder in what was unpacked; the details are in $LOG)."
  fi
  rm -rf "$t"
fi

HD_ON=""
if [ -n "$HD_HAVE" ]; then
  # its fan-mission variants, as its obj/fm_mode.bat would switch to: the fan-mission
  # torches, the game's own blood splats (from its obj.crf), the fan-mission water
  FMV="$TG_DIR/HDMOD_FM"
  if [ ! -d "$FMV" ]; then
    mkdir -p "$FMV/obj" "$FMV/fam/waterhw"
    for n in 1 2 3 4 5; do
      f="$(file_in "$HD_HAVE/obj" "NewT0${n}_fm.bin")"
      [ -n "$f" ] && cp "$f" "$FMV/obj/NEWT0$n.BIN"
    done
    crf="$(find "$TG_DIR" -maxdepth 2 -iname obj.crf 2>/dev/null | head -1)"
    if [ -n "$crf" ]; then
      for n in 1 2 3; do
        unzip -C -p "$crf" "blodsp$n.bin" > "$FMV/obj/blodsp$n.bin" 2>>"$LOG"
        [ -s "$FMV/obj/blodsp$n.bin" ] || rm -f "$FMV/obj/blodsp$n.bin"
      done
    fi
    w="$(file_in "$HD_HAVE/fam" waterhw)"
    if [ -n "$w" ] && [ -n "$(file_in "$w" l2in_fm.dds)" ]; then
      cp "$(file_in "$w" l2in_fm.dds)" "$FMV/fam/waterhw/l2in.dds"
      [ -n "$(file_in "$w" l2in.mtl)" ] && cp "$(file_in "$w" l2in.mtl)" "$FMV/fam/waterhw/l2in.mtl"
    fi
    printf '%s\n' "Thief RT: the Thief Gold HD Mod's fan-mission variants (what its fm_mode.bat" \
      "would switch to), searched before HDMOD in fan missions (cam_mod.ini fm_mod_path):" \
      "its fan-mission torch models, the original blood splats, its fan-mission water." > "$FMV/README.txt"
  fi
  # cam_mod.ini: the mod on (the lines it had that are not mod paths, kept below)
  CM="$(file_in "$TG_DIR" cam_mod.ini)"
  if [ -z "$CM" ] || ! grep -qiE '^[[:space:]]*mod_path[[:space:]].*hdmod' "$CM"; then
    sky=""; [ -n "$(file_in "$HD_HAVE" t2skies)" ] && sky="+hdmod/t2skies"
    kept=""
    if [ -n "$CM" ]; then
      cp "$CM" "$TG_DIR/cam_mod.ini.before-thief-rt"
      kept="$(tr -d '\r' < "$CM" | grep -viE '^[[:space:]]*(;|$)|^[[:space:]]*(fm_)?mod_path|^[[:space:]]*(ObjTextures16|MeshTextures16|character_detail)')"
      rm -f "$CM"
    fi
    {
      echo "; Thief RT: mods, NewDark style (searched after a fan mission, before the game's own files)"
      echo ";"
      echo "; Thief Gold HD Mod: hand-made HD textures, models and skies"
      echo "mod_path hdmod$sky"
      echo
      echo "; in fan missions: the HD mod too, with its fan-mission variants first (HDMOD_FM)."
      echo "; A fan mission's own textures and models still come first. Replace this line"
      echo "; with mod_path_skip_fm to keep fan missions as they were made."
      echo "fm_mod_path hdmod_fm+hdmod$sky"
      echo
      echo "; the HD mod's model textures are in obj/txt16 and mesh/txt16 (the game's own"
      echo "; txt folders are still searched after them)"
      echo "ObjTextures16"
      echo "MeshTextures16"
      echo
      echo "; the HD mod's characters are its high-poly models (mesh/hipoly), made for its"
      echo "; textures: the low-poly ones wear them wrongly (a face on the back of the head)"
      echo "character_detail 1"
      if [ -n "$kept" ]; then
        echo
        echo "; (from the cam_mod.ini before)"
        echo "$kept"
      fi
    } > "$TG_DIR/cam_mod.ini"
  fi
  HD_ON=1
fi

# The Thief II HD Mod into Thief II's folder (HDMOD), and cam_mod.ini to use it
if [ -n "$TAKE_T2HD" ]; then
  notify "Installing the Thief II HD Mod... (several minutes)"
  src="${T2HD_SRC#*:}"
  t="$(mktemp -d "$T2_DIR/.thiefrt-hdmod.XXXXXX")"
  ok=""
  case "${T2HD_SRC%%:*}" in
    dir) "$DITTO" "$src" "$t/HDMOD" >&3 2>&1 && ok=1 ;;
    archive) tar -xf "$src" -C "$t" >&3 2>&1 && ok=1 ;;
    inno) "$INNOEXTRACT" -s -d "$t" "$src" >&3 2>&1 && ok=1 ;;
    # (an NSIS installer: Thief 2 HD Mod 1.0's. Its three vegetation choices -- new grass
    # animated, new grass still, the original -- are all in it, animated first: the first
    # of each file is kept, which is its installer's default, the animated grass)
    nsis) "$SEVENZIP" x -aos -y -bd -bsp0 -o"$t" "$src" >&3 2>&1; [ $? -le 1 ] && ok=1 ;;
  esac
  hd=""
  if [ -n "$ok" ]; then
    while IFS= read -r d; do is_hdmod "$d" && { hd="$d"; break; }; done < <(find "$t" -maxdepth 4 -type d -iname HDMOD)
  fi
  if [ -n "$hd" ]; then
    old="$(file_in "$T2_DIR" HDMOD)"
    [ -n "$old" ] && mv "$old" "$T2_DIR/HDMOD.before-thief-rt.$$"
    mv "$hd" "$T2_DIR/HDMOD" && T2HD_HAVE="$T2_DIR/HDMOD"
  else
    T2HD_NOTE="

The Thief II HD Mod could not be installed (no HDMOD folder in what was unpacked; the details are in $LOG)."
  fi
  rm -rf "$t"
fi

T2HD_ON=""
if [ -n "$T2HD_HAVE" ]; then
  CM="$(file_in "$T2_DIR" cam_mod.ini)"
  if [ -z "$CM" ] || ! grep -qiE '^[[:space:]]*mod_path[[:space:]].*hdmod' "$CM"; then
    sky=""; [ -n "$(file_in "$T2HD_HAVE" t2skies)" ] && sky="+hdmod/t2skies"
    kept=""
    if [ -n "$CM" ]; then
      cp "$CM" "$T2_DIR/cam_mod.ini.before-thief-rt"
      kept="$(tr -d '\r' < "$CM" | grep -viE '^[[:space:]]*(;|$)|^[[:space:]]*(fm_)?mod_path|^[[:space:]]*(ObjTextures16|MeshTextures16|character_detail)')"
      rm -f "$CM"
    fi
    {
      echo "; Thief RT: mods, NewDark style (searched after a fan mission, before the game's own files)"
      echo ";"
      echo "; Thief II HD Mod: HD textures, objects, skies and water"
      echo "mod_path hdmod$sky"
      echo
      echo "; in fan missions too (a fan mission's own textures and models still come first)."
      echo "; Replace this line with mod_path_skip_fm to keep fan missions as they were made."
      echo "fm_mod_path hdmod$sky"
      if [ -n "$(file_in "$T2HD_HAVE/obj" txt16)$(file_in "$T2HD_HAVE/mesh" txt16)" ]; then
        echo
        echo "; the HD mod's model textures are in obj/txt16 and mesh/txt16 (the game's own"
        echo "; txt folders are still searched after them)"
        echo "ObjTextures16"
        echo "MeshTextures16"
      fi
      if [ -n "$(file_in "$T2HD_HAVE/mesh" hipoly)" ]; then
        echo
        echo "; the HD mod's characters: its high-poly models (mesh/hipoly), made for its textures"
        echo "character_detail 1"
      fi
      if [ -n "$kept" ]; then
        echo
        echo "; (from the cam_mod.ini before)"
        echo "$kept"
      fi
    } > "$T2_DIR/cam_mod.ini"
  fi
  T2HD_ON=1
fi

# --- the launcher --------------------------------------------------------------------

if [ -z "$SETUP" ]; then
install_launcher || fail "Could not make the launcher in $APPS."
fi

# --- done ----------------------------------------------------------------------------

games=""
[ -n "$TG_DIR" ] && games="$games
   Thief Gold: ${TG_DIR/#$HOME/$TILDE}"
[ -n "$T2_DIR" ] && games="$games
   Thief II: ${T2_DIR/#$HOME/$TILDE}"
[ -n "$BP_HAVE" ] && games="$games
   The Black Parade: in Thief Gold's fan missions"
[ -n "$HD_ON" ] && games="$games
   The Thief Gold HD Mod: on"
[ -n "$T2HD_ON" ] && games="$games
   The Thief II HD Mod: on"
if [ -n "$TG_DIR" ] && [ -z "$HD_ON" ] && [ -z "$HD_NOTE" ]; then
  HD_NOTE="

Optional: the Thief Gold HD Mod (HD textures, models and skies). Download its installer (Thief1HDMod1.2.exe, from ModDB) to your Downloads, then choose File > Find Games Again in Thief RT."
fi
if [ -n "$T2_DIR" ] && [ -z "$T2HD_ON" ] && [ -z "$T2HD_NOTE" ]; then
  T2HD_NOTE="

Optional: the Thief II HD Mod (HD textures, objects, skies and water). Download its installer (Thief 2 HD Mod 1.0, from ModDB or Nexus Mods) to your Downloads, then choose File > Find Games Again in Thief RT."
fi

if [ -n "$SETUP" ]; then
  [ -n "$games" ] || games="
   (none)"
  message "Your games for Thief RT:$games$BP_NOTE$HD_NOTE$T2HD_NOTE" >/dev/null
  exit 0
fi
if [ -n "$DEFER" ]; then
  r="$(message "Thief RT is installed, in your ${APPS/#$HOME/$TILDE} folder.

macOS did not let the installer look in your Downloads, Documents or Desktop folders, so Thief RT looks for your games when it is first opened. macOS then asks whether Thief RT may read those folders: allow it." "Show in Finder" "Open Thief RT")"
  case "$r" in
    "Open Thief RT") open "$A" ;;
    "Show in Finder") open -R "$A" ;;
  esac
  exit 0
fi

r="$(message "Done. Thief RT is in your ${APPS/#$HOME/$TILDE} folder.$games

Open it to choose the game (or a fan mission from its FMs folder), ray tracing or OpenGL, and the quality: Ultra, High, Medium or Low. In the game, Cmd+, (or F10) shows the ray tracing settings.

The first time a level is played, the materials of its textures are made and kept: it can look plain for a few seconds.$BP_NOTE$HD_NOTE$T2HD_NOTE" "Show in Finder" "Open Thief RT")"
case "$r" in
  "Open Thief RT") open "$A" ;;
  "Show in Finder") open -R "$A" ;;
esac
exit 0
