#!/bin/bash
# valgrind memory-testing harness for padnote.
#
# Two modes:
#   ./scripts/valgrind-run.sh              — leak check (--leak-check=full)
#   ./scripts/valgrind-run.sh --asan       — rebuild with -fsanitize=address
#                                            and run the smoke; prints
#                                            ASan's report inline.
#
# Tools required:
#   sudo apt install valgrind                # leak-check mode
#   (the --asan path uses GCC's built-in ASan; no extra apt install)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUPP="${REPO_ROOT}/scripts/qt-scintilla.supp"
BIN="${REPO_ROOT}/build/padnote"

if [[ "${1:-}" == "--asan" ]]; then
    echo "==> [ASan] Re-configuring with -fsanitize=address..."
    cmake -B "${REPO_ROOT}/build-asan" -S "${REPO_ROOT}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
        -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" >/dev/null
    cmake --build "${REPO_ROOT}/build-asan" -j"$(nproc)" >/dev/null
    echo "==> [ASan] Running 3-second offscreen smoke..."
    QT_QPA_PLATFORM=offscreen \
        ASAN_OPTIONS=detect_leaks=1:print_suppressions=0 \
        timeout 5 "${REPO_ROOT}/build-asan/padnote" \
        --new-instance "${REPO_ROOT}/README.md" || true
    echo "==> [ASan] Done. Build dir: ${REPO_ROOT}/build-asan"
    exit 0
fi

if ! command -v valgrind >/dev/null 2>&1; then
    cat >&2 <<EOF
valgrind not installed. Install with:
    sudo apt install valgrind
Then re-run this script.

Alternative: AddressSanitizer (built-in to GCC, no apt needed):
    ./scripts/valgrind-run.sh --asan
EOF
    exit 2
fi

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN — run cmake --build build first." >&2
    exit 1
fi

# Qt and Scintilla emit known-clean noise (Qt's internal allocators,
# lexer state caches, etc.). Suppress via the sibling .supp file. Add
# new entries when noise re-emerges.
echo "==> Running valgrind (this is slow — 5-10 minutes for a 3 s session)..."
QT_QPA_PLATFORM=offscreen \
valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --suppressions="${SUPP}" \
    --log-file=/tmp/padnote-valgrind.log \
    timeout 5 "$BIN" --new-instance "${REPO_ROOT}/README.md" \
    || true

echo
echo "Done. Report at /tmp/padnote-valgrind.log:"
tail -40 /tmp/padnote-valgrind.log
