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
  # MINDEN llvm-* eszközt explicit full-path-tal adunk át. A kbuild LLVM=1
  # módban csak a bare neveket ismeri (llvm-ar, llvm-objdump, stb.) — de
  # az NDK-bin nincs a $PATH-ban, és LKL `arch/lkl/scripts/cc-objdump-file-format.sh`
  # bare `$OBJDUMP`-ot hív, ezért `OBJDUMP` is kell, nem csak OBJCOPY.
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
    "OBJDUMP=${NDK_BIN}/llvm-objdump"
    "READELF=${NDK_BIN}/llvm-readelf"
    "OBJSIZE=${NDK_BIN}/llvm-size"
    "HOSTLD=${NDK_BIN}/ld.lld"
  )
fi

echo "[configure] alap LKL defconfig (SUBARCH=${SUBARCH})"
make ARCH=lkl "${MAKE_EXTRA[@]}" defconfig

if [[ -f "${FRAGMENT}" ]]; then
  # MEGJEGYZÉS: korábban `scripts/kconfig/merge_config.sh -m`-t használtunk.
  # Tapasztalat: a CI Android-NDK build során a CONFIG_UNIX=y NEM ragadt meg
  # a végső .config-ban (a /proc/net/protocols on-device nem mutatja az
  # AF_UNIX-ot, és socket(AF_UNIX,…) -EAFNOSUPPORT-tal hibázik), miközben
  # lokálisan a host-toolchainnel ugyanaz a parancs OK-t adott. Lehet hogy
  # `make olddefconfig` Android-NDK env-ben máshogy dolgozza fel a merge-elt
  # .config-ot (pl. `# CONFIG_X is not set` sor megmarad a `CONFIG_X=y`
  # mellett). Robosztusabb stratégia: a fragment-et DIREKT a .config végéhez
  # írjuk — `olddefconfig` az utolsó értéket veszi. Plusz előtte explicit
  # töröljük a "not set" sorokat, amik a fragment-ben kapcsolnánk be.
  echo "[configure] saját fragment alkalmazása (direct append + olddefconfig)"
  # Először töröljük a "# CONFIG_FOO is not set" sorokat azokra a kulcsokra,
  # amiket a fragment bekapcsol — különben kétszer lenne a változónak értéke
  # és a Kconfig parser viselkedése implementáció-specifikus.
  awk '/^CONFIG_[A-Z0-9_]+=/{
        name=$0; sub(/=.*/,"",name); print name
      }' "${FRAGMENT}" > /tmp/kaliterm-fragment-keys.txt
  while IFS= read -r key; do
    [[ -z "$key" ]] && continue
    sed -i "\|^# ${key} is not set$|d" .config
  done < /tmp/kaliterm-fragment-keys.txt
  printf '\n# ── kaliterm fragment ──\n' >> .config
  cat "${FRAGMENT}" >> .config
  make ARCH=lkl "${MAKE_EXTRA[@]}" olddefconfig
else
  echo "[configure] nincs config fragment, csak az alap LKL defconfig"
fi

echo "[configure] kész — .config előállítva"
echo "[configure] === kulcs CONFIG értékek (sanity) ==="
grep -E "^(CONFIG_NET|CONFIG_UNIX|CONFIG_INET|CONFIG_NET_NS|CONFIG_USB($|=|_)|CONFIG_USBIP|CONFIG_HID)" .config | sort || true
echo "[configure] ============================================"
