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
    # CROSS_COMPILE → LKL `tools/lkl/Makefile.autoconf` ebből deríti a triple-t
    #   (patches/0001-... után aarch64-linux-android24 már elfogadott).
    # CLANG_TARGET_FLAGS — a felső szintű kbuild check kedvéért is.
    # Minden llvm-* tool explicit full-path-tal — az NDK bin nincs $PATH-ban,
    # és LKL arch/lkl/scripts/cc-objdump-file-format.sh stb. bare $OBJDUMP-ot
    # hív Kconfig-evalban.
    EXTRA_ARGS+=(
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
      "OBJDUMP=${NDK_BIN}/llvm-objdump"
      "READELF=${NDK_BIN}/llvm-readelf"
      "OBJSIZE=${NDK_BIN}/llvm-size"
      "HOSTLD=${NDK_BIN}/ld.lld"
    )
    echo "[build] android-arm64 NDK toolchain: ${NDK_BIN}"
    ;;
  *)
    echo "ismeretlen SUBARCH: ${SUBARCH}" >&2; exit 2 ;;
esac

# Az LKL build belépési pontja a tools/lkl alatt van. A `make` az
# `arch/lkl/`-be megy be a hoszt-architektúrához, és felépíti a
# `tools/lkl/lib/liblkl-host-lib.so`-t plus a példa-toolokat.
#
# KCONFIG=olddefconfig: A `tools/lkl/Makefile` default-ban `KCONFIG?=defconfig`-ot
# használ a DOT_CONFIG szabályban (`make -C ../.. ARCH=lkl $(KCONFIG)`), ami
# **felülírja** a configure-lkl.sh által előállított .config-ot (a merge-elt
# fragmentünk elveszik — CONFIG_UNIX, CONFIG_USBIP_VHCI_HCD, stb. NEM kerül
# be a végső kernelbe). `KCONFIG=olddefconfig` parancssori override-dal a
# meglévő .config-ot megtartja, és csak az új/hiányzó symbol-okat tölti fel
# default-tal. A rule maradék része (`cat kernel.config >> .config; olddefconfig;
# syncconfig`) zavartalanul fut, a tools/lkl autoconf-generálta beállítások
# (LKL_FUZZING, MMU) tetejére kerülnek a már meglévő network/USB kapcsolóknak.
echo "[build] make -j${JOBS} -C tools/lkl KCONFIG=olddefconfig ${EXTRA_ARGS[*]:-}"
make -j"${JOBS}" -C tools/lkl KCONFIG=olddefconfig "${EXTRA_ARGS[@]}"

# Post-build sanity dump: ha a tools/lkl mégis felülírná a .config-ot, itt
# kiderül — látjuk, hogy a hálózat/USB kapcsolók ott vannak-e a végső
# kernel-build configban.
echo "[build] === post-build .config sanity ==="
grep -E "^(CONFIG_NET|CONFIG_UNIX|CONFIG_INET|CONFIG_NET_NS|CONFIG_USB($|=|_)|CONFIG_USBIP|CONFIG_HID)" .config 2>/dev/null | sort || echo "[build] (.config olvashatatlan)"
echo "[build] ==================================="

mkdir -p "${OUT_DIR}/${SUBARCH}/lib" "${OUT_DIR}/${SUBARCH}/bin"

# Az LKL kimeneti library-k pozíciója verziónként változott — minden
# `liblkl*.so` / `liblkl*.a`-t kimentünk, és külön mappába az example
# tool binárisokat (cptofs/cpfromfs/lklfuse/fs2tar, illetve a bin/* ELF-eket).
copied=0
while IFS= read -r f; do
    cp -v "$f" "${OUT_DIR}/${SUBARCH}/lib/"
    copied=1
done < <(find tools/lkl -maxdepth 4 \( -name 'liblkl*.so' -o -name 'liblkl*.a' \) -type f 2>/dev/null)

for cand in \
    tools/lkl/cptofs \
    tools/lkl/cpfromfs \
    tools/lkl/lklfuse \
    tools/lkl/fs2tar; do
    if [[ -f "${cand}" && -x "${cand}" ]]; then
        cp -v "${cand}" "${OUT_DIR}/${SUBARCH}/bin/"
        copied=1
    fi
done

# A `bin/` mappában is lehet kiegészítő tool (pl. fuzz-target ELF-ek)
shopt -s nullglob
for f in tools/lkl/bin/*; do
    if [[ -f "$f" && -x "$f" && ! "$f" =~ \.(o|so|a)$ ]]; then
        cp -v "$f" "${OUT_DIR}/${SUBARCH}/bin/" 2>/dev/null || true
        copied=1
    fi
done
shopt -u nullglob

if [[ "${copied}" -eq 0 ]]; then
  echo "[build] HIBA: egy LKL kimeneti fájl sem található. tools/lkl tartalom:" >&2
  ls -R tools/lkl >&2 || true
  exit 3
fi

echo "[build] kész — kimenetek:"
find "${OUT_DIR}/${SUBARCH}" -type f -exec ls -lh {} +
