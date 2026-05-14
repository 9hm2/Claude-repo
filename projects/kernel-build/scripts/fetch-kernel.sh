#!/usr/bin/env bash
# Letölti a Linux mainline forrást és kibontja a build dirba.
# Reprodukálható: a verziót a Makefile KERNEL_VERSION-ből kapja.
set -euo pipefail

KERNEL_VERSION="${1:?usage: fetch-kernel.sh <version> <build_dir>}"
BUILD_DIR="${2:?usage: fetch-kernel.sh <version> <build_dir>}"

MAJOR="${KERNEL_VERSION%%.*}"
TARBALL="linux-${KERNEL_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/${TARBALL}"
SRC_DIR="${BUILD_DIR}/linux-${KERNEL_VERSION}"

if [[ -f "${SRC_DIR}/Makefile" ]]; then
  echo "[fetch] már létezik: ${SRC_DIR}"
  exit 0
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [[ ! -f "${TARBALL}" ]]; then
  echo "[fetch] letöltés: ${URL}"
  curl -fsSL --retry 5 --retry-delay 2 -o "${TARBALL}.part" "${URL}"
  mv "${TARBALL}.part" "${TARBALL}"
fi

echo "[fetch] kibontás: ${TARBALL}"
tar -xf "${TARBALL}"
echo "[fetch] kész: ${SRC_DIR}"
