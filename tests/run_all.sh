#!/usr/bin/env bash
# Full verification for the PSD codec: round trip, independent parser, fuzz.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SDK="${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk}"
OUT="${OUT:-$ROOT/build/tests}"
SRC=(
  "$ROOT/src/psd/psd_read.cpp"
  "$ROOT/src/psd/psd_write.cpp"
  "$ROOT/src/psd/psd_buf.cpp"
  "$ROOT/src/psd/rle.cpp"
)
mkdir -p "$OUT"

echo "== building round-trip test =="
clang++ -std=c++17 -O2 -Wall -Wextra -isysroot "$SDK" -Xlinker -ld_classic \
  -I"$ROOT/src" -o "$OUT/psd_test" "$HERE/psd_test.cpp" "${SRC[@]}"

echo "== round trip =="
"$OUT/psd_test"

echo "== independent python verifier =="
python3 "$HERE/verify_psd.py" /tmp/psd_rt.psd
python3 "$HERE/verify_psd.py" /tmp/psd_group.psd

echo "== building fuzz test (ASAN + UBSAN) =="
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -isysroot "$SDK" -Xlinker -ld_classic \
  -I"$ROOT/src" -o "$OUT/psd_fuzz" "$HERE/psd_fuzz.cpp" "${SRC[@]}"

echo "== fuzz: 20000 mutated inputs =="
"$OUT/psd_fuzz" /tmp/psd_rt.psd 20000

echo "ALL PSD TESTS PASSED"
