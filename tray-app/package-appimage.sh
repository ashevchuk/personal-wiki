#!/usr/bin/env bash
# Packages the already-built wiki-tray binary into a standalone AppImage
# via linuxdeploy. Run after `cmake --build tray-app/build`.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BINARY="${BUILD_DIR}/wiki-tray"
APPDIR="${BUILD_DIR}/AppDir"
LINUXDEPLOY_DIR="${BUILD_DIR}/.linuxdeploy"
LINUXDEPLOY="${LINUXDEPLOY_DIR}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
# Pinned 2026-11-05 — a fresh "continuous" build only if this checksum
# ever needs bumping on purpose, not a moving target re-verified per run.
LINUXDEPLOY_SHA256="df908b19f1ed1cdafa79508d415aa12d6c56ce4d56d4149aa8c09286718706e5"

if [[ ! -x "${BINARY}" ]]; then
  echo "error: ${BINARY} not found — build tray-app/ first" >&2
  exit 1
fi

mkdir -p "${LINUXDEPLOY_DIR}"
if [[ ! -x "${LINUXDEPLOY}" ]]; then
  echo "Downloading linuxdeploy..."
  curl -fL -o "${LINUXDEPLOY}" "${LINUXDEPLOY_URL}"
  chmod +x "${LINUXDEPLOY}"
fi

ACTUAL_SHA256="$(sha256sum "${LINUXDEPLOY}" | cut -d' ' -f1)"
if [[ "${ACTUAL_SHA256}" != "${LINUXDEPLOY_SHA256}" ]]; then
  echo "error: linuxdeploy checksum mismatch (got ${ACTUAL_SHA256}, expected ${LINUXDEPLOY_SHA256}) — refusing to use it" >&2
  exit 1
fi

rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/share/applications" \
         "${APPDIR}/usr/share/icons/hicolor/256x256/apps" \
         "${APPDIR}/usr/bin/resources"

cp "${BINARY}" "${APPDIR}/usr/bin/wiki-tray"
cp "${SCRIPT_DIR}/resources/DejaVuSansMono.ttf" "${APPDIR}/usr/bin/resources/"
cp "${SCRIPT_DIR}/resources/wiki-tray.desktop" "${APPDIR}/usr/share/applications/"
cp "${SCRIPT_DIR}/resources/wiki-tray.png" "${APPDIR}/usr/share/icons/hicolor/256x256/apps/"

cd "${BUILD_DIR}"
"${LINUXDEPLOY}" \
  --appdir "${APPDIR}" \
  --executable "${APPDIR}/usr/bin/wiki-tray" \
  --desktop-file "${SCRIPT_DIR}/resources/wiki-tray.desktop" \
  --icon-file "${SCRIPT_DIR}/resources/wiki-tray.png" \
  --output appimage

PRODUCED="$(find "${BUILD_DIR}" -maxdepth 1 -iname 'Wiki_Tray*.AppImage' -newer "${BINARY}" | head -1)"
if [[ -z "${PRODUCED}" ]]; then
  echo "error: linuxdeploy ran but no .AppImage was found in ${BUILD_DIR}" >&2
  exit 1
fi
mv "${PRODUCED}" "${BUILD_DIR}/wiki-tray-x86_64.AppImage"
chmod +x "${BUILD_DIR}/wiki-tray-x86_64.AppImage"
echo "Built ${BUILD_DIR}/wiki-tray-x86_64.AppImage"
