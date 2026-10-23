#!/usr/bin/env sh
# Build the ThreadSanitizer binary, then run the full unit-test suite and a
# 5-minute pty soak under TSan. Fails (exit 1) if EITHER produces a TSan
# report, or if the suite does not pass.
#
# TSan cannot be combined with ASan, so this uses its own build dir
# (build-tsan, Release + -DGETH_TSAN=ON). When the network is down it reuses
# the dependencies already fetched under build/_deps.
#
# Usage: sh tools/tsan_check.sh [soak_seconds]   (default soak: 300 s)
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build-tsan"
SRC="$REPO/build/_deps"
SOAK_S="${1:-300}"
OUT="$REPO/review/tsan_check"

# 1. Configure + build (Release + TSan). Reuse fetched deps if present.
FC_FLAGS=()
if [ -d "$SRC/ftxui-src" ]; then
  FC_FLAGS=(-DFETCHCONTENT_SOURCE_DIR_FTXUI="$SRC/ftxui-src"
            -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS="$SRC/tomlplusplus-src"
            -DFETCHCONTENT_SOURCE_DIR_DOCTEST="$SRC/doctest-src")
fi
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DGETH_TSAN=ON \
  -DGETH_BUILD_TESTS=ON "${FC_FLAGS[@]}"
cmake --build "$BUILD" -j"$(nproc)"

mkdir -p "$OUT"

# 2. Unit-test suite under TSan: require a passing run AND no TSan report.
GETH_TEST=1 XDG_CONFIG_HOME="$(mktemp -d)" XDG_DATA_HOME="$(mktemp -d)" \
  "$BUILD/geth_tests" > "$OUT/suite.txt" 2>&1 || true
if grep -q "WARNING: ThreadSanitizer" "$OUT/suite.txt"; then
  echo "FAIL: TSan report in the unit-test suite (see $OUT/suite.txt)"
  exit 1
fi
if ! grep -q "Status: SUCCESS!" "$OUT/suite.txt"; then
  echo "FAIL: unit-test suite did not pass (see $OUT/suite.txt)"
  exit 1
fi
echo "OK: unit-test suite passed, clean under TSan"

# 3. 5-minute pty soak under TSan (tsan_soak.py exits 0 iff clean + no report).
if python3 "$REPO/tools/tsan_soak.py" "$BUILD/geth" "$SOAK_S" "$OUT/soak"; then
  echo "OK: ${SOAK_S}s soak clean under TSan"
else
  echo "FAIL: TSan report or non-clean exit in the soak (see $OUT/soak/soak_tsan.txt)"
  exit 1
fi

echo "PASS: no TSan reports in the suite or the ${SOAK_S}s soak"
