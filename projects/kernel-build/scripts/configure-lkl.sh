#!/usr/bin/env bash
# Konfigurálja az LKL kernel buildet a saját feature-eink beemelésével.
#
# Az LKL alapból minimális defconfig-gal indul (tools/lkl alatt). A mi
# `config/kaliterm_lkl.config` fragmentünket rámergeljük, hogy a kiválasztott
# driverek beépüljenek a kernel library-be.
set -euo pipefail

SRC_DIR="${1:?usage: configure-lkl.sh <src_dir> <subarch> <fragment>}"
SUBARCH="${2:?subarch}"   # x86_64 | arm64
FRAGMENT="${3:?fragment}"

case "${SUBARCH}" in
  x86_64|arm64) ;;
  *) echo "ismeretlen SUBARCH: ${SUBARCH}" >&2; exit 2 ;;
esac

cd "${SRC_DIR}"

echo "[configure] alap LKL defconfig"
make ARCH=lkl defconfig

if [[ -f "${FRAGMENT}" ]]; then
  echo "[configure] saját fragment merge-elése: ${FRAGMENT}"
  ./scripts/kconfig/merge_config.sh -m -O . .config "${FRAGMENT}"
  make ARCH=lkl olddefconfig
else
  echo "[configure] nincs config fragment, csak az alap LKL defconfig"
fi

echo "[configure] kész — .config előállítva"
