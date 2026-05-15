#!/usr/bin/env bash
# Letölti a Kali Linux ARM64 minimal rootfs-t a hivatalos NetHunter
# forrásból. Ez egy debootstrap-szerű minimal pack — apt-get update,
# apt install nmap, stb. utána egészítheti ki a felhasználó.
#
# Méret: ~250 MB tömörítve (xz), ~700 MB kicsomagolva.
#
# Verzió-pin a release URL-ben: aktuálisan a 2025.4 kép. Ha új release
# jön ki, itt cseréljük az URL-t és a SHA256-ot.
set -euo pipefail

DL_DIR="${1:?usage: fetch-kali-rootfs.sh <download_dir>}"
mkdir -p "${DL_DIR}"

# A Kali NetHunter rootfs a "kalifs-arm64-minimal.tar.xz" — a stable mirror
# a kali.download alatt él. Kis fájl (200-300 MB), hasonlít egy
# debootstrap-eredményre, csak Kali-csomagok aktiválva.
ROOTFS_URL="${ROOTFS_URL:-https://kali.download/nethunter-images/current/rootfs/kalifs-arm64-minimal.tar.xz}"
ROOTFS_SHA256="${ROOTFS_SHA256:-}"   # üres = letöltés után csak kiírjuk

ROOTFS_FILE="${DL_DIR}/kalifs-arm64-minimal.tar.xz"

if [[ ! -f "${ROOTFS_FILE}" ]]; then
  echo "[fetch-kali] letöltés: ${ROOTFS_URL}"
  curl -fL --retry 4 --retry-delay 5 -o "${ROOTFS_FILE}" "${ROOTFS_URL}"
fi

actual_sha=$(sha256sum "${ROOTFS_FILE}" | awk '{print $1}')
echo "[fetch-kali] SHA-256: ${actual_sha}"
if [[ -n "${ROOTFS_SHA256}" && "${actual_sha}" != "${ROOTFS_SHA256}" ]]; then
  echo "[fetch-kali] HIBA: SHA-256 mismatch (várt: ${ROOTFS_SHA256})" >&2
  exit 1
fi

ls -lh "${ROOTFS_FILE}"
echo "[fetch-kali] kész: ${ROOTFS_FILE}"
