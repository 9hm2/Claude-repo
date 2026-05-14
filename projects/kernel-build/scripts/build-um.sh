#!/usr/bin/env bash
# Lefordítja a UML kernelt és kimásolja a `linux` binárist az out/ alá.
set -euo pipefail

SRC_DIR="${1:?usage: build-um.sh <src_dir> <subarch> <out_dir> <jobs>}"
SUBARCH="${2:?subarch}"
OUT_DIR="${3:?out_dir}"
JOBS="${4:-4}"

cd "${SRC_DIR}"

EXTRA_ARGS=()
if [[ "${SUBARCH}" == "arm64" && -n "${CROSS_COMPILE:-}" ]]; then
  EXTRA_ARGS+=("CROSS_COMPILE=${CROSS_COMPILE}")
  echo "[build] arm64 cross-compile: CROSS_COMPILE=${CROSS_COMPILE}"
fi

echo "[build] make -j${JOBS} ARCH=um SUBARCH=${SUBARCH} ${EXTRA_ARGS[*]:-}"
make -j"${JOBS}" ARCH=um SUBARCH="${SUBARCH}" "${EXTRA_ARGS[@]}"

mkdir -p "${OUT_DIR}/${SUBARCH}"
cp -v linux "${OUT_DIR}/${SUBARCH}/linux-um"
echo "[build] kész — ${OUT_DIR}/${SUBARCH}/linux-um"

file "${OUT_DIR}/${SUBARCH}/linux-um" || true
ls -lh "${OUT_DIR}/${SUBARCH}/linux-um"
