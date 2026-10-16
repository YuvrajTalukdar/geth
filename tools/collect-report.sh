#!/usr/bin/env bash
# GETH support-report collector.
#
# Gathers everything needed to diagnose a GETH problem on this machine:
# the kernel/OS, the GETH version, every data source and its availability
# (with fix hints), a few live samples, the layout decision, and a full
# rendered frame.
#
# It needs no root, changes nothing, and only reads. The report is written
# to geth-report-<hostname>.txt in the repo root, or to the path given as
# $1.
#
# Usage:
#   tools/collect-report.sh                 # -> geth-report-<host>.txt
#   tools/collect-report.sh /tmp/report.txt # -> /tmp/report.txt
set -euo pipefail

# Repo root = the parent of this script's directory.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="build-rel"
GETH="$BUILD_DIR/geth"

# Build the Release binary if it is missing (never rebuild an existing one).
if [[ ! -x "$GETH" ]]; then
    echo "collect-report: no Release build found; building (this can take a while)..." >&2
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >&2
    cmake --build "$BUILD_DIR" --target geth -j"$(nproc)" >&2
fi

# `hostname` is not always installed; `uname -n` is universal.
HOST="$(uname -n)"
OUT="${1:-geth-report-$HOST.txt}"

# The rendered frame on real hardware fills the 60 s graph window with 60
# samples at the 1 s default interval, so it takes ~60 s. Say so first.
echo "collect-report: writing $OUT (the frame step takes ~60 s)..." >&2

{
    echo "GETH support report"
    echo "generated: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "host:      $HOST"
    echo

    echo "=== uname -a ==="
    uname -a
    echo

    echo "=== geth --version ==="
    "$GETH" --version
    echo

    echo "=== geth --probe ==="
    "$GETH" --probe
    echo

    echo "=== geth --dump (5 samples) ==="
    "$GETH" --dump --count 5
    echo

    echo "=== geth --layout-report 160x48 ==="
    "$GETH" --layout-report 160x48
    echo

    echo "=== geth --render-frame 160x48 ==="
    "$GETH" --render-frame 160x48
    echo
} > "$OUT"

echo "collect-report: wrote $OUT ($(wc -c < "$OUT") bytes)" >&2
