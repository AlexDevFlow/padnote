#!/usr/bin/env bash
#
# build-appimage.sh — bundle padnote into a self-contained AppImage.
#
# Runs on any glibc 2.31+ Linux (Ubuntu Noble / Pop!_OS 24.04 / Debian 12+
# / Fedora 39+). Uses linuxdeploy + linuxdeploy-plugin-qt to pull in Qt6
# shared libraries and Qt's platform plugins.
#
# Inputs (env vars, with defaults):
#   BUILD_DIR    — CMake build dir to install from. Default: ./build
#   APPDIR       — staging dir for the AppImage payload. Default:
#                  ./build/AppDir
#   OUTPUT       — output path for the .AppImage. Default:
#                  ./build/padnote-x86_64.AppImage
#   QMAKE        — qmake6 binary linuxdeploy-plugin-qt should query.
#                  Default: $(command -v qmake6).
#   LINUXDEPLOY  — path to the linuxdeploy AppImage. If unset, the
#                  script downloads the latest release into ./build/.
#
# Run from the repo root. Requires: cmake, an existing CMake configure,
# linuxdeploy, linuxdeploy-plugin-qt, qmake6 in PATH.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
APPDIR="${APPDIR:-${BUILD_DIR}/AppDir}"
OUTPUT="${OUTPUT:-${BUILD_DIR}/padnote-x86_64.AppImage}"
QMAKE="${QMAKE:-$(command -v qmake6 || true)}"
LINUXDEPLOY="${LINUXDEPLOY:-}"

if [[ -z "${QMAKE}" || ! -x "${QMAKE}" ]]; then
    echo "ERROR: qmake6 not found in PATH. Install qt6-base-dev-tools." >&2
    exit 1
fi

# Auto-fetch linuxdeploy + plugin-qt into BUILD_DIR if not provided.
mkdir -p "${BUILD_DIR}"
if [[ -z "${LINUXDEPLOY}" ]]; then
    LINUXDEPLOY="${BUILD_DIR}/linuxdeploy-x86_64.AppImage"
    if [[ ! -x "${LINUXDEPLOY}" ]]; then
        echo "==> Fetching linuxdeploy..."
        curl -sSL --output "${LINUXDEPLOY}" \
          "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
        chmod +x "${LINUXDEPLOY}"
    fi
fi

PLUGIN_QT="${BUILD_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage"
if [[ ! -x "${PLUGIN_QT}" ]]; then
    echo "==> Fetching linuxdeploy-plugin-qt..."
    curl -sSL --output "${PLUGIN_QT}" \
      "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
    chmod +x "${PLUGIN_QT}"
fi

# Ensure the plugin AppImage is on PATH so linuxdeploy can find it as
# `linuxdeploy-plugin-qt`.
export PATH="${BUILD_DIR}:${PATH}"

echo "==> Staging install tree into ${APPDIR}"
rm -rf "${APPDIR}"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}" --prefix /usr

echo "==> Running linuxdeploy with the Qt plugin"
QMAKE="${QMAKE}" \
"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --plugin qt \
    --desktop-file "${APPDIR}/usr/share/applications/padnote.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/padnote.png" \
    --output appimage

# linuxdeploy writes the AppImage into the current working directory
# with its own naming scheme. Move it to the requested OUTPUT path.
generated=$(ls -t padnote*.AppImage 2>/dev/null | head -1 || true)
if [[ -z "${generated}" ]]; then
    echo "ERROR: linuxdeploy did not produce an .AppImage." >&2
    exit 1
fi
mv "${generated}" "${OUTPUT}"
chmod +x "${OUTPUT}"

echo "==> Built ${OUTPUT}"
echo "Size: $(du -h "${OUTPUT}" | cut -f1)"
