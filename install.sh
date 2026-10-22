#!/usr/bin/env bash
# Installs the built plugin into a user-writable plugin directory.
#
# Usage:  ./install.sh [target-dir]
#   default target: <repo>/dist/plugins
# Pass the OBS plugin folder you can write to, e.g.
#   ./install.sh "$HOME/Library/Application Support/obs-studio/plugins"
# (that folder may be protected by macOS TCC; grant Terminal "Full Disk Access"
#  or copy it manually with Finder.)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-$HERE/dist/plugins}"
SRC="$HERE/build/obs-psd.plugin"

if [ ! -d "$SRC" ]; then echo "run ./build.sh first" >&2; exit 1; fi
mkdir -p "$TARGET"
rm -rf "$TARGET/obs-psd.plugin"
cp -R "$SRC" "$TARGET/obs-psd.plugin"
echo "installed obs-psd.plugin -> $TARGET"
echo
echo "OBS reads user plugins from:"
echo "  ~/Library/Application Support/obs-studio/plugins/"
echo "Copy obs-psd.plugin there (Finder or a terminal with Full Disk Access)."
