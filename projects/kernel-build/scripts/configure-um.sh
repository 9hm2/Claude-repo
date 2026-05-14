#!/usr/bin/env bash
# Generálja a UML kernel .config-ját a célhoszt architektúrához és ráhúzza a
# saját config-fragmentünket.
set -euo pipefail

SRC_DIR="${1:?usage: configure-um.sh <src_dir> <subarch> <fragment>}"
SUBARCH="${2:?subarch}"   # x86_64 | arm64
FRAGMENT="${3:?fragment}"

case "${SUBARCH}" in
  x86_64) ;;
  arm64)  ;;
  *) echo "ismeretlen SUBARCH: ${SUBARCH}" >&2; exit 2 ;;
esac

cd "${SRC_DIR}"

echo "[configure] alap defconfig ARCH=um SUBARCH=${SUBARCH}"
make ARCH=um SUBARCH="${SUBARCH}" defconfig

if [[ -f "${FRAGMENT}" ]]; then
  echo "[configure] saját fragment merge-elése: ${FRAGMENT}"
  ./scripts/kconfig/merge_config.sh -m -O . .config "${FRAGMENT}"
  make ARCH=um SUBARCH="${SUBARCH}" olddefconfig
else
  echo "[configure] nincs config fragment, csak az alap defconfig"
fi

echo "[configure] kész — .config előállítva"
