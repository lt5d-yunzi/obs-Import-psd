#!/usr/bin/env bash
# Downloads the third-party headers/libraries obs-psd builds against.
#
# Nothing in third_party/ is committed to this repository: everything there is
# someone else's code with its own licence (OBS is GPL-2.0, Qt is LGPL-3.0,
# simde/miniz/stb are public domain or MIT). This script fetches exactly the
# files the build needs and records where each one came from.
#
#   ./fetch-deps.sh
#
# Requires: curl, tar, unzip. macOS only for the Qt headers (they come from the
# Qt framework that ships inside OBS.app on macOS); elsewhere point QT_DIR at a
# Qt installation with the same headers.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

OBS_APP="${OBS_APP:-/Applications/OBS.app}"
QT_DIR="${QT_DIR:-$OBS_APP/Contents/Frameworks}"
TP="$HERE/third_party"
mkdir -p "$TP"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- OBS headers
# libobs + frontend-api public headers, from the pinned OBS release.
if [ ! -d "$TP/obs-headers/libobs" ]; then
  say "OBS headers (32.2.2)"
  OBS_TAG=32.2.2
  curl -fsSL "https://github.com/obsproject/obs-studio/archive/refs/tags/${OBS_TAG}.tar.gz" \
    -o "$WORK/obs.tar.gz"
  tar -xzf "$WORK/obs.tar.gz" -C "$WORK"
  SRC="$WORK/obs-studio-${OBS_TAG}"
  mkdir -p "$TP/obs-headers/libobs" "$TP/obs-headers/frontend-api"
  # Copy the public headers only; the build includes them as <obs.h>, <obs-module.h>
  # and <graphics/graphics.h>, so keep the include roots intact.
  (cd "$SRC/libobs" && find . -name '*.h' -print0 | while IFS= read -r -d '' f; do
      mkdir -p "$TP/obs-headers/libobs/$(dirname "$f")"
      cp "$f" "$TP/obs-headers/libobs/$f"
    done)
  cp "$SRC/UI/obs-frontend-api/obs-frontend-api.h" "$TP/obs-headers/frontend-api/" 2>/dev/null || \
    cp "$SRC/frontend/api/obs-frontend-api/obs-frontend-api.h" "$TP/obs-headers/frontend-api/" 2>/dev/null || true
  cat > "$TP/obs-headers/SOURCE.txt" <<EOF2
Source : https://github.com/obsproject/obs-studio
Tag    : ${OBS_TAG}
Licence: GPL-2.0-or-later (OBS Studio)
Files  : public libobs headers + obs-frontend-api.h
EOF2
else
  say "OBS headers already present, skipping"
fi

# ------------------------------------------------------------- Qt headers
# Only the Qt headers reachable from QDialog/QPushButton/QColorDialog are needed.
# On macOS they ship inside OBS.app, so copy them from there.
if [ ! -d "$TP/qt-headers/include" ]; then
  say "Qt headers from $QT_DIR"
  if [ ! -d "$QT_DIR/QtWidgets.framework" ]; then
    echo "error: QtWidgets.framework not found under $QT_DIR" >&2
    echo "       set QT_DIR to a Qt installation with the same headers" >&2
    exit 1
  fi
  mkdir -p "$TP/qt-headers/include"
  for fw in QtCore QtGui QtWidgets; do
    if [ -d "$QT_DIR/$fw.framework/Headers" ]; then
      cp -R "$QT_DIR/$fw.framework/Headers/." "$TP/qt-headers/include/"
    fi
  done
  # The build also includes a few private data headers from the source tree;
  # they are only needed for compile-time constants, so any Qt 6.x source works.
  cat > "$TP/qt-headers/SOURCE.txt" <<EOF2
Source : ${QT_DIR} (Qt framework bundled with OBS.app)
Licence: LGPL-3.0 (Qt)
Files  : QtCore/QtGui/QtWidgets public headers
EOF2
else
  say "Qt headers already present, skipping"
fi

# ------------------------------------------------------------------- simde
# libobs' util/sse-intrin.h pulls in <simde/x86/sse2.h> on non-x86 hosts.
if [ ! -d "$TP/simde/simde" ]; then
  say "simde"
  SIMDE_VERSION=0.8.2
  curl -fsSL "https://github.com/simd-everywhere/simde/archive/refs/tags/v${SIMDE_VERSION}.tar.gz" \
    -o "$WORK/simde.tar.gz"
  tar -xzf "$WORK/simde.tar.gz" -C "$WORK"
  cp -R "$WORK/simde-${SIMDE_VERSION}/simde" "$TP/simde/"
  cat > "$TP/simde/SOURCE.txt" <<EOF2
Source : https://github.com/simd-everywhere/simde
Version: ${SIMDE_VERSION}
Licence: MIT
EOF2
else
  say "simde already present, skipping"
fi

# ------------------------------------------------------------------- miniz
# Deflate for PSD channels stored with ZIP compression (compression 2/3).
if [ ! -f "$TP/miniz/miniz.c" ]; then
  say "miniz"
  MINIZ_TAG=3.0.2
  curl -fsSL "https://github.com/richgel999/miniz/archive/refs/tags/${MINIZ_TAG}.tar.gz" \
    -o "$WORK/miniz.tar.gz"
  tar -xzf "$WORK/miniz.tar.gz" -C "$WORK"
  mkdir -p "$TP/miniz"
  cp "$WORK"/miniz-*/miniz.c "$WORK"/miniz-*/miniz.h \
     "$WORK"/miniz-*/miniz_tdef.c "$WORK"/miniz-*/miniz_tdef.h \
     "$WORK"/miniz-*/miniz_tinfl.c "$WORK"/miniz-*/miniz_tinfl.h \
     "$WORK"/miniz-*/miniz_common.h "$WORK"/miniz-*/miniz_export.h \
     "$TP/miniz/" 2>/dev/null || {
       # Older/newer layouts keep the headers in a subdirectory.
       find "$WORK/miniz-${MINIZ_TAG}" -maxdepth 2 -name 'miniz*.c' -o -maxdepth 2 -name 'miniz*.h' | \
         xargs -I{} cp {} "$TP/miniz/"
     }
  cat > "$TP/miniz/SOURCE.txt" <<EOF2
Source : https://github.com/richgel999/miniz
Version: ${MINIZ_TAG}
Licence: MIT / public domain (see the header of miniz.c)
EOF2
else
  say "miniz already present, skipping"
fi

# --------------------------------------------------------------------- stb
if [ ! -f "$TP/stb_image_write.h" ]; then
  say "stb_image_write"
  curl -fsSL "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h" \
    -o "$TP/stb_image_write.h"
  cat > "$TP/stb/SOURCE.txt" 2>/dev/null || true
  cat > "$TP/SOURCE.txt" <<'EOF2'
stb_image_write.h : https://github.com/nothings/stb (public domain / MIT)
EOF2
else
  say "stb already present, skipping"
fi

say "done"
printf '\nthird_party/ is ready. Now run:  ./build.sh\n'
