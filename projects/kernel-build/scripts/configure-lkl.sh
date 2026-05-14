#!/usr/bin/env bash
# Konfigurálja az LKL kernel buildet a saját feature-eink beemelésével.
#
# Az LKL alapból minimális defconfig-gal indul (tools/lkl alatt). A mi
# `config/kaliterm_lkl.config` fragmentünket rámergeljük, hogy a kiválasztott
# driverek beépüljenek a kernel library-be.
set -euo pipefail

SRC_DIR="${1:?usage: configure-lkl.sh <src_dir> <subarch> <fragment>}"
SUBARCH="${2:?subarch}"   # x86_64 | arm64 | android-arm64
FRAGMENT="${3:?fragment}"

case "${SUBARCH}" in
  x86_64|arm64|android-arm64) ;;
  *) echo "ismeretlen SUBARCH: ${SUBARCH}" >&2; exit 2 ;;
esac

cd "${SRC_DIR}"

# Android-NDK target esetén a defconfig is a célplatformra fordított
# helper-binárisokat akar — explicit toolchain-felülbírálás kell.
MAKE_EXTRA=()
if [[ "${SUBARCH}" == "android-arm64" ]]; then
  : "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME nincs beállítva (NDK install path)}"
  NDK_BIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin"
  # scripts/Makefile.clang a SRCARCH alapján egy hard-coded táblából deríti
  # a target triple-t (CLANG_TARGET_FLAGS_arm64=aarch64-linux-gnu, stb.).
  # LKL `SRCARCH=lkl`-t használ, de a táblában nincs `CLANG_TARGET_FLAGS_lkl`,
  # ezért a kbuild "add '--target=' option" hibával leáll. Make command-line
  # override-dal felülírjuk a `:=` assignmentet.
  # LLVM=1 → llvm-* tool-ok prefix nélkül; explicit AR/LD/... megadás
  # győz minden default fölött.
  # CROSS_COMPILE — LKL `tools/lkl/Makefile.autoconf` ezt használja a triple
  # deriválására és exportálja `CLANG_TARGET_FLAGS_lkl`-be. A felső szintű
  # kbuild (defconfig step) nem fut tools/lkl autoconfon át, ezért
  # CLANG_TARGET_FLAGS-t is explicit átadunk, hogy a scripts/Makefile.clang
  # `add '--target=' option` check átmenjen.
  MAKE_EXTRA+=(
    "LLVM=1"
    "CROSS_COMPILE=aarch64-linux-android24"
    "CLANG_TARGET_FLAGS=aarch64-linux-android24"
    "CC=${NDK_BIN}/aarch64-linux-android24-clang"
    "HOSTCC=cc"
    "AR=${NDK_BIN}/llvm-ar"
    "LD=${NDK_BIN}/ld.lld"
    "NM=${NDK_BIN}/llvm-nm"
    "STRIP=${NDK_BIN}/llvm-strip"
    "OBJCOPY=${NDK_BIN}/llvm-objcopy"
  )
fi

echo "[configure] alap LKL defconfig (SUBARCH=${SUBARCH})"
make ARCH=lkl "${MAKE_EXTRA[@]}" defconfig

if [[ -f "${FRAGMENT}" ]]; then
  echo "[configure] saját fragment merge-elése: ${FRAGMENT}"
  ./scripts/kconfig/merge_config.sh -m -O . .config "${FRAGMENT}"
  make ARCH=lkl "${MAKE_EXTRA[@]}" olddefconfig
else
  echo "[configure] nincs config fragment, csak az alap LKL defconfig"
fi

echo "[configure] kész — .config előállítva"
