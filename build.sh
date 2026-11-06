#!/usr/bin/env bash
# Builds obs-psd.plugin (a macOS OBS bundle) from source.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# Third-party headers are not committed (see README). Fetch them on demand.
if [ ! -d "$HERE/third_party/obs-headers/libobs" ]; then
  echo "third_party/ is missing; downloading dependencies..." >&2
  "$HERE/fetch-deps.sh"
fi

OBS_APP="${OBS_APP:-/Applications/OBS.app}"
FW="$OBS_APP/Contents/Frameworks"
SDK="${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk}"
OUT="${OUT:-$HERE/build}"
NAME=obs-psd

CXX_FLAGS=(
  -std=c++17 -O2 -fPIC -fvisibility=hidden -fno-rtti
  -isysroot "$SDK"
  -I"$HERE/src" -I"$HERE/third_party"
  -I"$HERE/third_party/obs-headers/libobs"
  -I"$HERE/third_party/obs-headers/frontend-api"
  -I"$HERE/third_party/simde"
  -I"$HERE/third_party/miniz"
  -I"$HERE/third_party/qt-headers/include"
  -I"$HERE/third_party/qt-headers/src/corelib"
  -I"$HERE/third_party/qt-headers/src/gui"
  -I"$HERE/third_party/qt-headers/src/widgets"
  -F"$FW"
  -Wall -Wextra -Wno-unused-parameter -Wno-vexing-parse -Wno-missing-braces
)

SOURCES=(
  src/plugin_main.cpp
  src/ui/psd_ui.cpp
  src/psd/psd_read.cpp
  src/psd/psd_write.cpp
  src/psd/psd_buf.cpp
  src/psd/rle.cpp
  src/psd/psd_text.cpp
  src/psd/psd_descriptor.cpp
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
  src/obs/source_render.cpp
  src/obs/scene_build.cpp
  src/obs/scene_export.cpp
  src/obs/psd_ignore.cpp
  src/obs/log_redact.cpp
  src/obs/psd_text_source.cpp
  src/obs/text_render_macos.mm
)

mkdir -p "$OUT"
BUNDLE="$OUT/$NAME.plugin"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources/locale" "$BUNDLE/Contents/Resources/data/locale"

echo "== compiling =="
# miniz is C, and compiling it as C++ breaks on a const array declaration
# (miniz_tdef.c declares s_tdefl_num_probes twice: once bare, once initialised).
# Build the C files with the C driver and link everything together at the end.
C_SOURCES=()
CXX_SOURCES=()
for src in "${SOURCES[@]}"; do
  case "$src" in
    *.c) C_SOURCES+=("$src") ;;
    *) CXX_SOURCES+=("$src") ;;
  esac
done

OBJS=()
for src in "${C_SOURCES[@]}"; do
  obj="$OUT/$(echo "$src" | tr '/' '_').o"
  clang -std=c11 -O2 -fPIC -fvisibility=hidden \
    -isysroot "$SDK" -I"$HERE/third_party/miniz" \
    -c "$src" -o "$obj"
  OBJS+=("$obj")
done
for src in "${CXX_SOURCES[@]}"; do
  obj="$OUT/$(echo "$src" | tr '/' '_').o"
  if [ "${src##*.}" = "mm" ]; then
    clang++ "${CXX_FLAGS[@]}" -x objective-c++ -c "$src" -o "$obj"
  else
    clang++ "${CXX_FLAGS[@]}" -c "$src" -o "$obj"
  fi
  OBJS+=("$obj")
done

clang++ "${OBJS[@]}" \
  -o "$BUNDLE/Contents/MacOS/$NAME" \
  -bundle -undefined dynamic_lookup \
  -isysroot "$SDK" \
  -F"$FW" \
  -framework libobs \
  -framework CoreText \
  -framework CoreGraphics \
  -framework CoreFoundation \
  "$FW/obs-frontend-api.dylib" \
  -Xlinker -ld_classic

echo "== fixing install names / rpaths =="
install_name_tool -add_rpath @executable_path/../Frameworks "$BUNDLE/Contents/MacOS/$NAME" 2>/dev/null || true
install_name_tool -add_rpath @loader_path/../../Frameworks "$BUNDLE/Contents/MacOS/$NAME" 2>/dev/null || true

cat > "$BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>com.obsproject.obs-psd</string>
  <key>CFBundleName</key><string>obs-psd</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundleVersion</key><string>1</string>
</dict>
</plist>
PLIST

cp data/locale/*.ini "$BUNDLE/Contents/Resources/locale/"

echo "== done: $BUNDLE =="
ls -la "$BUNDLE/Contents/MacOS/"
