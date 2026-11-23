#!/bin/bash
# Makes the shareable disk image of Thief RT (Thief Gold and Thief II, path traced):
#   cd ~/Games/Thief-RT-Mac && bash package/make-dmg.sh
# -> dist/Thief-RT-<date>.dmg, with "Install Thief RT", the Read Me, and the licences and
# the source. Run it on this Mac after a build (bash setup.sh, or start the game once).
#
# What goes in: the engine as built here (its libraries are inside it already, so it runs
# on any Apple silicon Mac), the path tracer and its files, the launcher (built here from
# package/launcher), and the installer. What does not: the games, the fan missions, and
# the materials made from the textures here (rt/mat_dev: each player's Mac makes its own).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
PKG="$ROOT/package"
# The version: today's date and the next release number of the day (v2026.11.23.3 on GitHub
# -> 2026.11.23.4), the release's tag to give it; the launcher's update check compares the
# two. Given: bash package/make-dmg.sh 2026.11.24.1
DAY="$(date +%Y.%m.%d)"
if [ -n "${1:-}" ]; then
  VERSION="$1"
else
  N=0
  TAGS="$(curl -fsS --max-time 15 "https://api.github.com/repos/loui1989/thief-rt-mac/tags?per_page=100" 2>/dev/null || true)"
  [ -n "$TAGS" ] || echo "  (GitHub could not be reached: the version may be one already released -- give it: bash package/make-dmg.sh $DAY.N)"
  for t in $(printf '%s' "$TAGS" | grep -o "\"v$DAY\.[0-9][0-9]*\"" | tr -d '"v'); do
    n="${t##*.}"
    [ "$n" -gt "$N" ] && N="$n"
  done
  VERSION="$DAY.$((N + 1))"
fi
say() { printf '  %s\n' "$*"; }

APP="$ROOT/Thief RT.app"
[ -x "$APP/Contents/MacOS/thief-runner" ] || { echo "No build here yet (bash setup.sh)"; exit 1; }
[ -f rt/bin/libRTGL1.dylib ] || { echo "No path tracer in rt/bin (bash setup.sh)"; exit 1; }
for t in otool install_name_tool codesign hdiutil ditto clang; do
  command -v "$t" >/dev/null || { echo "$t is missing (Xcode's command line tools: xcode-select --install)"; exit 1; }
done

STAGE="$(mktemp -d /tmp/thiefrt-dmg.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
VOL="$STAGE/Thief RT"
INST="$VOL/Install Thief RT.app"
P="$INST/Contents/Resources/Payload"
mkdir -p "$INST/Contents/MacOS" "$P/Engine"

# --- the engine ---------------------------------------------------------------------
say "Engine"
E="$P/Engine/Thief RT.app"
ditto "$APP" "$E"
R="$E/Contents/MacOS/thief-runner"
FW="$E/Contents/Frameworks"
# rpaths into this Mac's folders (the build's) out; the bundle's own Frameworks in
for rp in $(otool -l "$R" | awk '/cmd LC_RPATH/{getline; getline; print $2}'); do
  case "$rp" in /Users/*|/opt/*|/usr/local/*) install_name_tool -delete_rpath "$rp" "$R" ;; esac
done
RPATHS="$(otool -l "$R")"
case "$RPATHS" in *'@executable_path/../Frameworks'*) ;; *) install_name_tool -add_rpath "@executable_path/../Frameworks" "$R" ;; esac
for l in "$FW"/*.dylib; do
  chmod u+w "$l"
  install_name_tool -id "@rpath/$(basename "$l")" "$l" 2>/dev/null || true
done
# anything still pointing into this Mac's folders would not load elsewhere
# (each one's own name, the first line, aside: the path tracer's is its build's, and it is loaded by path)
if { otool -L "$R" | tail -n +2; for f in "$FW"/*.dylib rt/bin/libRTGL1.dylib; do otool -L "$f" | tail -n +3; done; } |
   grep -E "$HOME|/opt/homebrew|/usr/local/lib"; then
  echo "Libraries still point into this Mac's folders (above)"; exit 1
fi
say "  libraries: $(cd "$FW" && ls | tr '\n' ' ')"

# the path tracer, its shaders and the files it reads (not the materials made here)
mkdir -p "$P/Engine/rt"
ditto rt/bin "$P/Engine/rt/bin"
cp rt/*.ktx2 rt/RTGL1.json rt/materials.txt "$P/Engine/rt/"
[ -d rt/glass ] && ditto rt/glass "$P/Engine/rt/glass"
echo "$VERSION" > "$P/Engine/VERSION"

# ad hoc signatures (the libraries were changed; Apple silicon runs nothing unsigned)
codesign --force -s - "$FW"/*.dylib "$P/Engine/rt/bin/libRTGL1.dylib"
codesign --force --deep -s - "$E"

# --- the launcher -------------------------------------------------------------------
say "Launcher"
L="$P/Thief RT.app"
mkdir -p "$L/Contents/MacOS" "$L/Contents/Resources"
clang -fobjc-arc -O2 -arch arm64 -mmacosx-version-min=13.0 "$PKG/launcher/ThiefLauncher.m" \
  -framework AppKit -o "$L/Contents/MacOS/Thief RT"
cp "$PKG/installer/launch.sh" "$L/Contents/Resources/launch.sh"
# (the installer's finding part, run by the launcher when the installer could not look)
cp "$PKG/installer/install.sh" "$L/Contents/Resources/setup.sh"
chmod +x "$L/Contents/Resources/launch.sh" "$L/Contents/Resources/setup.sh"
# (7-Zip and innoextract, for the HD Mods' Windows installers -- NSIS and Inno Setup ones:
# the launcher's finding part and the installer each have their own)
for t in "7zip/7zz" "innoextract/innoextract"; do
  cp "$PKG/$t" "$L/Contents/Resources/${t#*/}"
  chmod +x "$L/Contents/Resources/${t#*/}"
  codesign --force -s - "$L/Contents/Resources/${t#*/}"
done
cp "$APP/Contents/Resources/thief.icns" "$L/Contents/Resources/icon.icns"
cat > "$L/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Thief RT</string>
  <key>CFBundleDisplayName</key><string>Thief RT</string>
  <key>CFBundleIdentifier</key><string>local.thief-rt.launcher</string>
  <key>CFBundleExecutable</key><string>Thief RT</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>The Dark Engine, restored; path tracing by RTGL1. Thief is a trademark of its owners; the games are not included.</string>
</dict>
</plist>
PLIST
codesign --force -s - "$L"

# --- the installer ------------------------------------------------------------------
say "Installer"
cp "$PKG/installer/install.sh" "$INST/Contents/MacOS/install"
chmod +x "$INST/Contents/MacOS/install"
for t in "7zip/7zz" "innoextract/innoextract"; do
  cp "$PKG/$t" "$INST/Contents/Resources/${t#*/}"
  chmod +x "$INST/Contents/Resources/${t#*/}"
  codesign --force -s - "$INST/Contents/Resources/${t#*/}"
done
cp "$APP/Contents/Resources/thief.icns" "$INST/Contents/Resources/icon.icns"
cat > "$INST/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Install Thief RT</string>
  <key>CFBundleIdentifier</key><string>local.thief-rt.installer</string>
  <key>CFBundleExecutable</key><string>install</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSRequiresNativeExecution</key><true/>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force -s - "$INST" 2>/dev/null || true

# --- the Read Me, licences and source ------------------------------------------------
say "Read Me, licences, source"
cp "$PKG/Read Me.html" "$VOL/Read Me.html"
LIC="$VOL/Licences and Source"
mkdir -p "$LIC/Licences" "$LIC/Source"
cp "$PKG/CREDITS.txt" "$LIC/"
lic() { [ -f "$2" ] && cp "$2" "$LIC/Licences/$1" || echo "   (no licence file for $1: $2)"; }
lic "Thief RT.txt" LICENSE.md
lic "Dark Engine Restored.txt" src/LICENSE.md
lic "RTGL1 (MIT).txt" "$PKG/RTGL1-LICENSE.txt"
lic "SDL2 (zlib).txt" deps/src/SDL2/LICENSE.txt
lic "OpenAL Soft (LGPL 2).txt" "$PKG/licences/OpenAL-Soft-COPYING.txt"
lic "FFmpeg (LGPL 2.1).txt" deps/src/FFmpeg/COPYING.LGPLv2.1
lic "libpng.txt" deps/src/libpng/LICENSE
lic "Ogg (BSD).txt" deps/src/ogg/COPYING
lic "Vorbis (BSD).txt" deps/src/vorbis/COPYING
lic "Squirrel (MIT).txt" deps/src/squirrel/COPYRIGHT
lic "Steam Audio (Apache 2).txt" deps/src/steamaudio/LICENSE-steam-audio.md
lic "7-Zip (LGPL, BSD, unRAR).txt" "$PKG/licences/7-Zip-License.txt"
lic "innoextract (zlib).txt" "$PKG/licences/innoextract-LICENSE.txt"
lic "Boost (BSL 1.0).txt" "$PKG/licences/Boost-LICENSE_1_0.txt"
lic "xz liblzma (0BSD).txt" "$PKG/licences/xz-liblzma-COPYING.0BSD.txt"
# the source: the engine as built here, the changes to the path tracer, and how it is all
# built (setup.sh: which versions of the libraries; FFmpeg and OpenAL Soft can be rebuilt
# and the engine linked again with them)
tar czf "$LIC/Source/thief-rt-engine-src.tar.gz" --exclude .git --exclude .DS_Store src
ditto patches "$LIC/Source/patches"
cp setup.sh "$LIC/Source/setup.sh"
ditto package "$LIC/Source/package"

# --- the disk image -----------------------------------------------------------------
mkdir -p dist
DMG="dist/Thief-RT-$VERSION.dmg"
say "Disk image"
hdiutil create -volname "Thief RT" -srcfolder "$VOL" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$DMG" >/dev/null
say "Done: $ROOT/$DMG ($(du -h "$DMG" | cut -f1))"
open -R "$DMG"
