#!/usr/bin/env bash
# Lefordítja az LKL library-t és kimásolja az out/ alá.
set -euo pipefail

SRC_DIR="${1:?usage: build-lkl.sh <src_dir> <subarch> <out_dir> <jobs>}"
SUBARCH="${2:?subarch}"   # x86_64 | arm64 | android-arm64
OUT_DIR="${3:?out_dir}"
JOBS="${4:-4}"

cd "${SRC_DIR}"

EXTRA_ARGS=()
case "${SUBARCH}" in
  x86_64)
    : "host build, default toolchain"
    ;;
  arm64)
    : "${CROSS_COMPILE:=aarch64-linux-gnu-}"
    EXTRA_ARGS+=("CROSS_COMPILE=${CROSS_COMPILE}")
    echo "[build] arm64 cross-compile (glibc): CROSS_COMPILE=${CROSS_COMPILE}"
    ;;
  android-arm64)
    : "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME nincs beállítva (NDK install path)}"
    NDK_BIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin"
    if [[ ! -x "${NDK_BIN}/aarch64-linux-android24-clang" ]]; then
      echo "[build] HIBA: nincs aarch64-linux-android24-clang itt: ${NDK_BIN}" >&2
      exit 3
    fi
    # LLVM=1 + CROSS_COMPILE=aarch64-linux-android-:
    #   az kbuild `scripts/Makefile.clang` ebből deríti a target triple-t.
    #   Az LLVM=1 jelzi hogy minden tool llvm-* (prefix nélkül); az explicit
    #   AR/LD/NM/STRIP/OBJCOPY megadás felülírja az auto-detect-et.
    #   Az NDK clang wrapper (aarch64-linux-android24-clang) saját --target=
    #   flag-je később jön a parancssorban, így overrideolja a kbuild által
    #   hozzáadott --target=aarch64-linux-android-et (utolsó wins).
    EXTRA_ARGS+=(
      "LLVM=1"
      "CROSS_COMPILE=aarch64-linux-android-"
      "CC=${NDK_BIN}/aarch64-linux-android24-clang"
      "HOSTCC=cc"
      "AR=${NDK_BIN}/llvm-ar"
      "LD=${NDK_BIN}/ld.lld"
      "NM=${NDK_BIN}/llvm-nm"
      "STRIP=${NDK_BIN}/llvm-strip"
      "OBJCOPY=${NDK_BIN}/llvm-objcopy"
    )
    echo "[build] android-arm64 NDK toolchain: ${NDK_BIN}"
    ;;
  *)
    echo "ismeretlen SUBARCH: ${SUBARCH}" >&2; exit 2 ;;
esac

# Az LKL build belépési pontja a tools/lkl alatt van. A `make` az
# `arch/lkl/`-be megy be a hoszt-architektúrához, és felépíti a
# `tools/lkl/lib/liblkl-host-lib.so`-t plus a példa-toolokat.
echo "[build] make -j${JOBS} -C tools/lkl ${EXTRA_ARGS[*]:-}"
make -j"${JOBS}" -C tools/lkl "${EXTRA_ARGS[@]}"

mkdir -p "${OUT_DIR}/${SUBARCH}"

# Az LKL kimenetek pozíciója fejlődéssel változott — minden valószínű
# kandidátust keresünk, és kimentjük az out/ alá.
copied=0
for cand in \
    tools/lkl/lib/liblkl-host-lib.so \
    tools/lkl/liblkl-host-lib.so \
    tools/lkl/lib/liblkl.so \
    tools/lkl/liblkl.so \
    tools/lkl/lib/liblkl.a \
    tools/lkl/liblkl.a; do
  if [[ -f "${cand}" ]]; then
    cp -v "${cand}" "${OUT_DIR}/${SUBARCH}/"
    copied=1
  fi
done

# Példa-toolok (cptofs, cpfromfs, lklfuse, ...) — ha vannak, vigyük.
for cand in tools/lkl/bin/* tools/lkl/cptofs tools/lkl/cpfromfs tools/lkl/lklfuse; do
  if [[ -f "${cand}" && -x "${cand}" ]]; then
    cp -v "${cand}" "${OUT_DIR}/${SUBARCH}/" || true
    copied=1
  fi
done

if [[ "${copied}" -eq 0 ]]; then
  echo "[build] HIBA: egy LKL kimeneti fájl sem található. tools/lkl tartalom:" >&2
  ls -R tools/lkl >&2 || true
  exit 3
fi

echo "[build] kész — kimenetek:"
ls -lh "${OUT_DIR}/${SUBARCH}/"
