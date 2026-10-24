#!/usr/bin/env bash
# Builds obs-psd.plugin (a macOS OBS bundle) from source.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

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
  src/obs/text_render_macos.mm
)

mkdir -p "$OUT"
BUNDLE="$OUT/$NAME.plugin"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources/locale" "$BUNDLE/Contents/Resources/data/locale"

echo "== compiling =="
clang++ "${CXX_FLAGS[@]}" \
  "${SOURCES[@]}" \
  -o "$BUNDLE/Contents/MacOS/$NAME" \
  -bundle -undefined dynamic_lookup \
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
