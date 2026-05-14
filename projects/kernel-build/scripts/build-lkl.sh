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
    # scripts/Makefile.clang a SRCARCH alapján egy hard-coded táblából deríti
    # a target triple-t — pl. CLANG_TARGET_FLAGS_arm64 := aarch64-linux-gnu.
    # LKL `SRCARCH=lkl`-t használ, de nincs `CLANG_TARGET_FLAGS_lkl` érték,
    # ezért a kbuild "add '--target=' option" hibával leáll. Make command-line
    # override-dal definiáljuk a változót (felülírja a Makefile `:=` assignmentet).
    # LLVM=1 → llvm-* tool-okat keres prefix nélkül; explicit AR/LD/NM/...
    # megadás minden default fölött győz.
    EXTRA_ARGS+=(
      "LLVM=1"
      "CLANG_TARGET_FLAGS=aarch64-linux-android24"
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
