#!/usr/bin/env bash
# Full verification for the PSD codec: round trip, independent parser, fuzz.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SDK="${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk}"
OUT="${OUT:-$ROOT/build/tests}"
# Scratch directory shared by the round-trip test, the Python verifier and the
# fuzzer.  Windows uses %TEMP%; POSIX defaults to /tmp.
TEST_TMP="${PSD_TEST_TMPDIR:-/tmp}"
export PSD_TEST_TMPDIR="$TEST_TMP"
mkdir -p "$TEST_TMP"
SRC=(
  "$ROOT/src/psd/psd_read.cpp"
  "$ROOT/src/psd/psd_write.cpp"
  "$ROOT/src/psd/psd_buf.cpp"
  "$ROOT/src/psd/rle.cpp"
  "$ROOT/src/psd/psd_text.cpp"
  "$ROOT/src/psd/psd_descriptor.cpp"
  "$ROOT/third_party/miniz/miniz.c"
  "$ROOT/third_party/miniz/miniz_tdef.c"
  "$ROOT/third_party/miniz/miniz_tinfl.c"
  "$ROOT/third_party/miniz/miniz_zip.c"
)
mkdir -p "$OUT"

echo "== building round-trip test =="
clang++ -std=c++17 -O2 -Wall -Wextra -isysroot "$SDK" -Xlinker -ld_classic \
  -I"$ROOT/src" -I"$ROOT/third_party/miniz" -o "$OUT/psd_test" "$HERE/psd_test.cpp" "${SRC[@]}"

echo "== round trip =="
"$OUT/psd_test"

echo "== independent python verifier =="
python3 "$HERE/verify_psd.py" "$TEST_TMP/psd_rt.psd"
python3 "$HERE/verify_psd.py" "$TEST_TMP/psd_group.psd"

echo "== building fuzz test (ASAN + UBSAN) =="
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -isysroot "$SDK" -Xlinker -ld_classic \
  -I"$ROOT/src" -I"$ROOT/third_party/miniz" -o "$OUT/psd_fuzz" "$HERE/psd_fuzz.cpp" "${SRC[@]}"

echo "== fuzz: 20000 mutated inputs =="
"$OUT/psd_fuzz" "$TEST_TMP/psd_rt.psd" 20000

echo "ALL PSD TESTS PASSED"
