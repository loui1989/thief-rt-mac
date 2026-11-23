#!/bin/bash
# Thief RT for Mac — the restored Dark Engine (Thief: The Dark Project, Thief Gold, Thief II)
# built natively for Apple silicon.
#
#   cd ~/Games/Thief-RT-Mac && bash setup.sh
#
# Builds the libraries it needs into deps/ (SDL2, libpng, Squirrel, Ogg Vorbis, FFmpeg for the
# cutscenes), then the engine from src/, and puts it together as "Thief RT.app" in this folder.
# Safe to re-run: only what changed is rebuilt. Logs go to logs/.

set -u
cd "$(dirname "$0")"
ROOT="$(pwd)"
mkdir -p logs deps/src deps/install
STATUS="$ROOT/logs/status.txt"
: > "$STATUS"
say()  { echo "==> $*"; echo "$(date '+%H:%M:%S') $*" >> "$STATUS"; }
fail() { echo "FAILED: $*"; echo "$(date '+%H:%M:%S') FAILED: $*" >> "$STATUS"; exit 1; }

JOBS="$(sysctl -n hw.ncpu)"
PREFIX="$ROOT/deps/install"
APP="$ROOT/Thief RT.app"
DUKE_DEPS="$HOME/Games/Duke-RT-Mac/deps"

SDL_TAG="release-2.30.9"
PNG_TAG="v1.6.44"
SQ_TAG="v3.2"
OGG_TAG="v1.3.5"
VORBIS_TAG="v1.3.7"
FFMPEG_TAG="n7.1"
OPENAL_TAG="1.24.1"

say "Checking tools"
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ] && [ -z "${THIEF_RT_NATIVE:-}" ]; then
  THIEF_RT_NATIVE=1 exec /usr/bin/arch -arm64 /bin/bash "$0" "$@"   # (Terminal under Rosetta: natively)
fi
[[ "$(uname -m)" == arm64 ]] || fail "this needs an Apple silicon Mac"
xcode-select -p >/dev/null 2>&1 || fail "Xcode command-line tools missing. Run: xcode-select --install"
TOOLS="$ROOT/deps/tools"
mkdir -p "$TOOLS/bin"
if [ -x "$DUKE_DEPS/tools/cmake/CMake.app/Contents/bin/cmake" ] && [ -x "$DUKE_DEPS/tools/bin/ninja" ]; then
  TOOLS="$DUKE_DEPS/tools"          # the ones Duke-RT-Mac already downloaded
else
  if [ ! -x "$TOOLS/cmake/CMake.app/Contents/bin/cmake" ]; then
    say "Downloading CMake"
    curl -fsSL -o "$TOOLS/cmake.tar.gz" \
      "https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-macos-universal.tar.gz" || fail "CMake download"
    mkdir -p "$TOOLS/cmake" && tar xzf "$TOOLS/cmake.tar.gz" -C "$TOOLS/cmake" --strip-components 1 || fail "CMake unpack"
  fi
  if [ ! -x "$TOOLS/bin/ninja" ]; then
    say "Downloading Ninja"
    curl -fsSL -o "$TOOLS/ninja.zip" "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-mac.zip" || fail "Ninja download"
    unzip -oq "$TOOLS/ninja.zip" -d "$TOOLS/bin" && chmod +x "$TOOLS/bin/ninja" || fail "Ninja unpack"
  fi
  xattr -dr com.apple.quarantine "$TOOLS" 2>/dev/null
fi
export PATH="$TOOLS/cmake/CMake.app/Contents/bin:$TOOLS/bin:$PATH"
cmake --version >/dev/null 2>&1 || fail "cmake won't run"
ninja --version >/dev/null 2>&1 || fail "ninja won't run"
{ echo "cmake: $(cmake --version | head -1)"; echo "ninja: $(ninja --version)"; echo "clang: $(clang --version | head -1)"
  echo "macOS: $(sw_vers -productVersion)  cpu: $(sysctl -n machdep.cpu.brand_string)  jobs: $JOBS"; } > logs/tools.txt
export CC=clang CXX=clang++
# a copy of the SDK's headers, so the port can be compile-checked away from this Mac
SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
if [ ! -f logs/sdk-headers.tar.gz ] || [ "$(cat logs/sdk-headers.txt 2>/dev/null)" != "$SDKROOT v2" ]; then
  ( cd "$SDKROOT" && tar czhf "$ROOT/logs/sdk-headers.tar.gz" usr/include \
      System/Library/Frameworks/OpenGL.framework/Headers System/Library/Frameworks/Metal.framework/Headers \
      System/Library/Frameworks/QuartzCore.framework/Headers System/Library/Frameworks/CoreFoundation.framework/Headers \
      System/Library/Frameworks/IOSurface.framework/Headers System/Library/Frameworks/CoreGraphics.framework/Headers \
      System/Library/Frameworks/AppKit.framework/Headers System/Library/Frameworks/Foundation.framework/Headers \
      2>/dev/null ) ; echo "$SDKROOT v2" > logs/sdk-headers.txt
fi
CM=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_FIND_FRAMEWORK=LAST)

# getsrc <name> <url> <tag>: a shallow clone of one tag into deps/src/<name>
getsrc() {
  local dir="$ROOT/deps/src/$1"
  if [ ! -f "$dir/.tag" ] || [ "$(cat "$dir/.tag")" != "$3" ]; then
    rm -rf "$dir"
    git clone -q --depth 1 --branch "$3" "$2" "$dir" >> logs/deps.log 2>&1 || fail "downloading $1 (see logs/deps.log)"
    echo "$3" > "$dir/.tag"
  fi
}
# built <name> <tag>: true if deps/install already has that version
built() { [ "$(cat "$PREFIX/.$1" 2>/dev/null)" = "$2" ]; }
mark()  { echo "$2" > "$PREFIX/.$1"; }
: > logs/deps.log

if ! built sdl2 "$SDL_TAG"; then
  say "Building SDL2"
  getsrc SDL2 https://github.com/libsdl-org/SDL.git "$SDL_TAG"
  { cmake -S deps/src/SDL2 -B deps/build/SDL2 "${CM[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF &&
    ninja -C deps/build/SDL2 -j "$JOBS" install; } >> logs/deps.log 2>&1 || fail "SDL2 (see logs/deps.log)"
  mark sdl2 "$SDL_TAG"
fi
if ! built png "$PNG_TAG"; then
  say "Building libpng"
  getsrc libpng https://github.com/pnggroup/libpng.git "$PNG_TAG"
  { cmake -S deps/src/libpng -B deps/build/libpng "${CM[@]}" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF \
      -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF &&
    ninja -C deps/build/libpng -j "$JOBS" install; } >> logs/deps.log 2>&1 || fail "libpng (see logs/deps.log)"
  mark png "$PNG_TAG"
fi
getsrc squirrel https://github.com/albertodemichelis/squirrel.git "$SQ_TAG"
getsrc ogg https://github.com/xiph/ogg.git "$OGG_TAG"
getsrc vorbis https://github.com/xiph/vorbis.git "$VORBIS_TAG"

# OpenAL Soft (3D sound with EAX-style reverb): Duke-RT-Mac's build if it's there
OPENAL_LIB="$DUKE_DEPS/install/lib/libopenal.1.dylib"
if [ ! -f "$OPENAL_LIB" ]; then
  OPENAL_LIB="$PREFIX/lib/libopenal.1.dylib"
  if ! built openal "$OPENAL_TAG"; then
    say "Building OpenAL Soft"
    getsrc openal-soft https://github.com/kcat/openal-soft.git "$OPENAL_TAG"
    { cmake -S deps/src/openal-soft -B deps/build/openal-soft "${CM[@]}" -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
        -DALSOFT_TESTS=OFF &&
      ninja -C deps/build/openal-soft -j "$JOBS" install; } >> logs/deps.log 2>&1 || fail "OpenAL Soft (see logs/deps.log)"
    mark openal "$OPENAL_TAG"
  fi
fi

# FFmpeg plays the cutscenes. Optional: without it the game runs, the movies are skipped.
FFMPEG_ARG=()
if ! built ffmpeg "$FFMPEG_TAG"; then
  say "Building FFmpeg for the cutscenes (a few minutes, once)"
  getsrc FFmpeg https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_TAG"
  mkdir -p deps/build/FFmpeg
  if ( cd deps/build/FFmpeg &&
       "$ROOT/deps/src/FFmpeg/configure" --prefix="$PREFIX/ffmpeg" --cc=clang --arch=arm64 --enable-static \
         --disable-shared --disable-programs --disable-doc --disable-network --disable-avdevice \
         --disable-avfilter --disable-encoders --disable-muxers --disable-autodetect --enable-zlib \
         --extra-cflags="-mmacosx-version-min=14.0" --extra-ldflags="-mmacosx-version-min=14.0" &&
       make -j "$JOBS" && make install ) > logs/ffmpeg.log 2>&1; then
    mark ffmpeg "$FFMPEG_TAG"
  else
    echo "   (FFmpeg did not build - see logs/ffmpeg.log; the game will skip its movies)"
  fi
fi
built ffmpeg "$FFMPEG_TAG" && FFMPEG_ARG=(-DDARK_FFMPEG_ROOT="$PREFIX/ffmpeg")

# Steam Audio (traced sound: reverb from the level's own walls, sounds muffled behind them),
# from the copy in deps-vendor. Optional: without it the game uses its own room reverb.
SA_TAR="$ROOT/deps-vendor/steamaudio-4.8.1-thief1.tar.gz"
PHONON_LIB="$PREFIX/lib/libphonon.dylib"
if [ -f "$SA_TAR" ]; then
  SA_TAG="$(shasum < "$SA_TAR" | cut -c1-16)"
  if ! built phonon "$SA_TAG" || [ ! -f "$PHONON_LIB" ]; then
    say "Building Steam Audio for traced sound (a few minutes, once)"
    rm -rf deps/src/steamaudio deps/build/steamaudio
    if tar xzf "$SA_TAR" -C deps/src > logs/steamaudio.log 2>&1 &&
       cmake -S deps/src/steamaudio -B deps/build/steamaudio "${CM[@]}" >> logs/steamaudio.log 2>&1 &&
       ninja -C deps/build/steamaudio -j "$JOBS" phonon >> logs/steamaudio.log 2>&1 &&
       mkdir -p "$PREFIX/lib" && cp -f deps/build/steamaudio/libphonon.dylib "$PHONON_LIB"; then
      mark phonon "$SA_TAG"
    else
      rm -f "$PHONON_LIB"
      echo "   (Steam Audio did not build - see logs/steamaudio.log; the game's own sound is used)"
      echo "$(date '+%H:%M:%S') Steam Audio did not build (logs/steamaudio.log)" >> "$STATUS"
    fi
  fi
fi

# The path tracer: RTGL1 (Metal) -- Thief's own copy, kept apart from the Raze RT kit's
# (which Duke and Blood use): its header in rt/include for the build, its library,
# shaders and data files in rt/ for run time. Copied once from the Raze RT kit; changes
# to the library for Thief go in patches/rtgl1-lib/*.patch, applied to RTGL1's source
# here (rtgl1-kit/src) and built into rt/bin -- again only when a patch changes. If it
# does not build, the library in rt/bin stays as it was.
RT_ARG=()
KIT="$ROOT/rtgl1-kit"
if ls patches/rtgl1-lib/*.patch >/dev/null 2>&1 && [ -d "$KIT/src/RayTracedGL1/Source" ]; then
  RL="$ROOT/build/rtgl1-lib"
  stamp="$(cat patches/rtgl1-lib/*.patch | shasum | cut -c1-40)"
  if [ "$(cat "$RL/stamp" 2>/dev/null)" != "$stamp" ]; then
    say "Building the path tracer library with Thief's changes (the first time takes a few minutes)"
    mkdir -p "$RL"
    rm -f "$RL/libRTGL1.dylib" "$RL/stamp"
    : > logs/rtgl1-lib.log
    ok=0
    if rsync -a --delete --exclude /Build "$KIT/src/RayTracedGL1/" "$RL/src/" >> logs/rtgl1-lib.log 2>&1; then
      ok=1
      for p in patches/rtgl1-lib/*.patch; do
        patch -s -p1 -d "$RL/src" < "$p" >> logs/rtgl1-lib.log 2>&1 || { ok=0; echo "patch $p did not apply" >> logs/rtgl1-lib.log; break; }
      done
    fi
    if [ $ok = 1 ] &&
       cmake -S "$RL/src" -B "$RL/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DRG_WITH_METAL=ON -DRG_WITH_EXAMPLES=OFF \
         -DRG_SPIRV_CROSS_DIR="$KIT/src/SPIRV-Cross" -DRG_GLSLANG="$KIT/bin/glslang" >> logs/rtgl1-lib.log 2>&1 &&
       ninja -C "$RL/build" -j "$JOBS" RayTracedGL1 >> logs/rtgl1-lib.log 2>&1 &&
       [ -f "$RL/src/Build/bin/libRTGL1.dylib" ] && cp -f "$RL/src/Build/bin/libRTGL1.dylib" "$RL/libRTGL1.dylib"; then
      mkdir -p rt/bin rt/include
      cp -f "$RL/libRTGL1.dylib" rt/bin/.libRTGL1.dylib.new && mv -f rt/bin/.libRTGL1.dylib.new rt/bin/libRTGL1.dylib
      rsync -a --delete "$RL/src/Include/" rt/include/
      echo "$stamp" > "$RL/stamp"
      say "Path tracer library OK"
    else
      echo "   (the path tracer library did not build - see logs/rtgl1-lib.log; the one in rt/bin is used as it is)"
      echo "$(date '+%H:%M:%S') path tracer library did not build (logs/rtgl1-lib.log)" >> "$STATUS"
    fi
  fi
fi
if [ -f rt/include/RTGL1/RTGL1.h ] && [ -f rt/bin/libRTGL1.dylib ]; then
  RT_ARG=(-DDARK_RTGL1_INCLUDE="$ROOT/rt/include")
else
  echo "   (RTGL1 not found in rt/ - building without the path tracer)"
fi

[ -f src/CMakeLists.txt ] || fail "src/ is missing (the engine source)"
say "Configuring the engine"
cmake -S src -B build "${CM[@]}" -DDARK_SQUIRREL_SOURCE="$ROOT/deps/src/squirrel" \
  -DDARK_OGG_SOURCE="$ROOT/deps/src/ogg" -DDARK_VORBIS_SOURCE="$ROOT/deps/src/vorbis" "${FFMPEG_ARG[@]}" "${RT_ARG[@]}" \
  > logs/configure.log 2>&1 || fail "CMake configure (see logs/configure.log)"

say "Building the engine (the first build takes a few minutes)"
start=$(date +%s)
ninja -C build -j "$JOBS" -k 0 thief-runner > logs/build.log 2>&1
rc=$?
secs=$(( $(date +%s) - start ))
grep -E "error:|FAILED:|Undefined symbols|ld: " logs/build.log | head -300 > logs/build-errors.txt
[ $rc -eq 0 ] || fail "Build failed after ${secs}s - $(grep -c 'error:' logs/build.log) errors (see logs/build-errors.txt)"
say "Build OK in ${secs}s"

say "Putting together Thief RT.app"
C="$APP/Contents"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources"
# a binary is put as a new file and renamed over the old one: macOS caches code signatures by
# file, and one rewritten in place is killed when it starts
put() { cp -f "$1" "$2.new" && mv -f "$2.new" "$2"; }
put build/thief-runner "$C/MacOS/thief-runner"
put "$PREFIX/lib/libSDL2-2.0.0.dylib" "$C/Frameworks/libSDL2-2.0.0.dylib"
put "$OPENAL_LIB" "$C/Frameworks/libopenal.1.dylib"
install_name_tool -id @rpath/libopenal.1.dylib "$C/Frameworks/libopenal.1.dylib" 2>/dev/null
if [ -f "$PHONON_LIB" ]; then
  put "$PHONON_LIB" "$C/Frameworks/libphonon.dylib"
else
  rm -f "$C/Frameworks/libphonon.dylib"
fi
install_name_tool -add_rpath @executable_path/../Frameworks "$C/MacOS/thief-runner" 2>/dev/null
if [ ! -f "$C/Resources/thief.icns" ]; then
  IS="$ROOT/build/thief.iconset"; rm -rf "$IS"; mkdir -p "$IS"
  ICONS="src/assets/icons/hicolor"
  for s in 16 32 128 256 512; do
    [ -f "$ICONS/${s}x${s}/apps/dark-engine-restored.png" ] && cp "$ICONS/${s}x${s}/apps/dark-engine-restored.png" "$IS/icon_${s}x${s}.png"
    d=$((s*2)); [ -f "$ICONS/${d}x${d}/apps/dark-engine-restored.png" ] && cp "$ICONS/${d}x${d}/apps/dark-engine-restored.png" "$IS/icon_${s}x${s}@2x.png"
  done
  iconutil -c icns "$IS" -o "$C/Resources/thief.icns" 2>/dev/null || echo "   (no app icon)"
fi
cat > "$C/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Thief RT</string>
  <key>CFBundleDisplayName</key><string>Thief RT</string>
  <key>CFBundleIdentifier</key><string>local.thief-rt-mac</string>
  <key>CFBundleExecutable</key><string>thief-runner</string>
  <key>CFBundleIconFile</key><string>thief</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
PLIST
codesign --force --deep -s - "$APP" > logs/codesign.log 2>&1 || fail "codesign (see logs/codesign.log)"
otool -L "$C/MacOS/thief-runner" > logs/linkage.txt 2>&1
grep -q "Ray tracing renderer: RTGL1" logs/configure.log && say "Path tracer: built in (bash run.sh; bash run.sh --gl for OpenGL)"
say "Ready: bash run.sh"
