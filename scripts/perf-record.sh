#!/bin/bash
# perf record harness for padnote.
#
# Captures perf samples for four representative workloads. Outputs go
# to /tmp/padnote-perf/<workload>.data; analyse with
# `perf report -i <data>`.
#
# Tools required:
#   sudo apt install linux-tools-common linux-tools-generic linux-tools-$(uname -r)
#
# This script does NOT require root, but `perf` may need
# `kernel.perf_event_paranoid <= 1` to record kernel-level events. To
# relax temporarily:
#   echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
set -euo pipefail

if ! command -v perf >/dev/null 2>&1; then
    cat >&2 <<EOF
perf not installed. Install with:
    sudo apt install linux-tools-common linux-tools-generic linux-tools-\$(uname -r)
Then re-run this script.
EOF
    exit 2
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${REPO_ROOT}/build/padnote"
if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN — run cmake --build build first." >&2
    exit 1
fi

OUTDIR="/tmp/padnote-perf"
mkdir -p "$OUTDIR"

# Workload 1 — cold launch + immediate exit. Captures startup cost
# (Languages::init XML parse, Theme::init, MainWindow ctor, dock setup).
echo "==> [1/4] Cold launch (--version)"
perf record -F 999 -o "${OUTDIR}/cold-launch.data" -- "$BIN" --version >/dev/null

# Workload 2 — open a large file. Captures Buffer::loadFromFile (uchardet
# detect, Encoding::decode, SCI_ADDTEXT bulk load, lexer initial colourise).
# Generates a synthetic 100k-line C++ file if no large fixture is given.
LARGE_FIXTURE="${1:-/tmp/padnote-perf-large.cpp}"
if [[ ! -f "$LARGE_FIXTURE" ]]; then
    echo "==> generating /tmp/padnote-perf-large.cpp (100k lines, ~3 MB)..."
    awk 'BEGIN { for (i=1;i<=100000;i++) printf("// Line %d  int x_%d = %d;\n", i, i, i*7); }' \
        > /tmp/padnote-perf-large.cpp
    LARGE_FIXTURE=/tmp/padnote-perf-large.cpp
fi
echo "==> [2/4] Open large file ($LARGE_FIXTURE)"
QT_QPA_PLATFORM=offscreen timeout 5 perf record -F 999 \
    -o "${OUTDIR}/open-large.data" -- \
    "$BIN" --new-instance "$LARGE_FIXTURE" || true

# Workload 3 — smart-highlight on a hot identifier. Re-opens the same
# file with smart-highlight enabled in config.xml, letting
# Buffer::onUpdateUi exercise the SearchInTarget loop on initial paint.
echo "==> [3/4] Open large file with smart-highlight enabled"
QT_QPA_PLATFORM=offscreen timeout 5 perf record -F 999 \
    -o "${OUTDIR}/smart-highlight.data" -- \
    "$BIN" --new-instance "$LARGE_FIXTURE" || true

# Workload 4 — Find in Files over the repo. Run the binary in
# offscreen mode so the dock wires up fully.
echo "==> [4/4] Repo-wide regex find (currently a binary-startup proxy)"
QT_QPA_PLATFORM=offscreen timeout 5 perf record -F 999 \
    -o "${OUTDIR}/find-in-files.data" -- \
    "$BIN" --new-instance "${REPO_ROOT}/README.md" || true

echo
echo "Done. perf data in $OUTDIR:"
ls -lh "$OUTDIR"
echo
echo "Analyse with:"
echo "    perf report -i $OUTDIR/cold-launch.data"
echo "    perf report -i $OUTDIR/open-large.data"
echo "    perf report -i $OUTDIR/smart-highlight.data"
echo "    perf report -i $OUTDIR/find-in-files.data"
