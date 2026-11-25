#!/usr/bin/env bash
# Fetch the pinned third-party component that is not available from the
# ESP-IDF component registry at the version validated on this hardware.
#
#   M5GFX 0.2.19  (git tag; MIT)
#
# LVGL 9.5.0 is fetched automatically by the ESP-IDF component manager from
# main/idf_component.yml.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
M5GFX_TAG="0.2.19"
M5GFX_DIR="${ROOT}/components/M5GFX"

if [ ! -d "${M5GFX_DIR}" ]; then
    mkdir -p "${ROOT}/components"
    git clone --depth 1 --branch "${M5GFX_TAG}" \
        https://github.com/m5stack/M5GFX.git "${M5GFX_DIR}"
else
    echo "components/M5GFX already present"
fi

# Not needed to build and large on disk.
rm -rf "${M5GFX_DIR}/docs" "${M5GFX_DIR}/examples"

echo "M5GFX ${M5GFX_TAG} ready at ${M5GFX_DIR}"
