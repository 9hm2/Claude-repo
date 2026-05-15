#!/usr/bin/env bash
# Letölt egy statikus ARM64 Android proot binárisat a Termux package-ből.
#
# A Termux-Packages projekt forrásból fordít NDK-val; az APT repón a
# `proot` csomag tartalmazza a `usr/bin/proot` ELF-et — pontosan amit
# nekünk kell (statikus Bionic ARM64).
#
# Verzió-pin: aktuálisan 5.4.0-1 (Termux-stable). Frissítéshez:
#   curl -sL https://packages.termux.dev/apt/termux-main/dists/stable/main/binary-aarch64/Packages | grep -A4 '^Package: proot$'
# és vedd ki a Filename: és SHA256: mezőket.
set -euo pipefail

DL_DIR="${1:?usage: fetch-proot.sh <download_dir>}"
mkdir -p "${DL_DIR}"

PKG_VERSION="5.4.0-1"
PKG_URL="https://packages.termux.dev/apt/termux-main/pool/main/p/proot/proot_${PKG_VERSION}_aarch64.deb"
PKG_SHA256="" # üres = letöltés után csak ellenőrző sorral kiírjuk a SHA-t

DEB_FILE="${DL_DIR}/proot_${PKG_VERSION}_aarch64.deb"

if [[ ! -f "${DEB_FILE}" ]]; then
  echo "[fetch-proot] letöltés: ${PKG_URL}"
  curl -fL --retry 4 --retry-delay 2 -o "${DEB_FILE}" "${PKG_URL}"
fi

actual_sha=$(sha256sum "${DEB_FILE}" | awk '{print $1}')
echo "[fetch-proot] SHA-256: ${actual_sha}"
if [[ -n "${PKG_SHA256}" && "${actual_sha}" != "${PKG_SHA256}" ]]; then
  echo "[fetch-proot] HIBA: SHA-256 mismatch (várt: ${PKG_SHA256})" >&2
  exit 1
fi

# A .deb tárfájl: ar arc → control.tar.* + data.tar.*. Mi a data-ban
# levő `usr/bin/proot` ELF-et akarjuk.
WORK_DIR="${DL_DIR}/proot-extract"
rm -rf "${WORK_DIR}" && mkdir -p "${WORK_DIR}"
cd "${WORK_DIR}"
ar x "${DEB_FILE}"
# data.tar lehet .xz, .zst, .gz, .bz2 — próbáljuk
for cand in data.tar.xz data.tar.zst data.tar.gz data.tar.bz2 data.tar; do
  if [[ -f "${cand}" ]]; then
    echo "[fetch-proot] kicsomagolás: ${cand}"
    tar -xf "${cand}"
    break
  fi
done

if [[ ! -f data/data/com.termux/files/usr/bin/proot && ! -f usr/bin/proot ]]; then
  echo "[fetch-proot] HIBA: nem találom a proot binárist a kicsomagolt fában." >&2
  find . -maxdepth 6 -name proot >&2 || true
  exit 1
fi

PROOT_BIN=$(find . -maxdepth 6 -name proot -type f | head -n1)
cp -v "${PROOT_BIN}" "${DL_DIR}/proot"
chmod +x "${DL_DIR}/proot"

# Sanity-check: ELF aarch64?
file "${DL_DIR}/proot" || true
echo "[fetch-proot] kész: ${DL_DIR}/proot"
