#!/usr/bin/env bash
# proot-me/proot cross-compile Android NDK ARM64 (Bionic) toolchain-nel.
#
# Két komponenst kell forrásból fordítani:
#   1) talloc — Samba memory allocator, proot dependency. A waf build-jét
#      kihagyjuk, csak a `lib/talloc/talloc.c`-t fordítjuk le közvetlenül
#      NDK clanggel és libtalloc.a-vé archiveolunk.
#   2) proot  — saját src/Makefile-t használ; CC/LD/CFLAGS env-en át.
#      A loader rész (loader.elf, ptrace-tracee induláshoz) ugyanazzal
#      az NDK CC-vel épül; xxd-vel header-fájlbe inline-olódik.
#
# Bemenet (env):
#   ANDROID_NDK_HOME — NDK install path
#   TARGET           — clang triple (default aarch64-linux-android24)
#
# Verzió-pin:
#   PROOT_VERSION=5.4.0
#   TALLOC_VERSION=2.4.2
#
# Kimenet: $DL_DIR/proot — statikus aarch64 Android ELF (kb. 1-2 MB).
set -euo pipefail

DL_DIR="${1:?usage: build-proot.sh <download_dir>}"
mkdir -p "${DL_DIR}"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME nincs beállítva (NDK install path)}"
NDK_BIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin"
TARGET="${TARGET:-aarch64-linux-android24}"

PROOT_VERSION="${PROOT_VERSION:-5.4.0}"
TALLOC_VERSION="${TALLOC_VERSION:-2.4.2}"

CC="${NDK_BIN}/${TARGET}-clang"
LD="${CC}"
AR="${NDK_BIN}/llvm-ar"
OBJCOPY="${NDK_BIN}/llvm-objcopy"

if [[ ! -x "${CC}" ]]; then
  echo "[build-proot] HIBA: nincs ${CC}" >&2
  ls "${NDK_BIN}" >&2 | head -20 || true
  exit 3
fi

# ─── 1) Tarball download ──────────────────────────────────────────────────
PROOT_TARBALL="${DL_DIR}/proot-${PROOT_VERSION}.tar.gz"
TALLOC_TARBALL="${DL_DIR}/talloc-${TALLOC_VERSION}.tar.gz"

if [[ ! -f "${PROOT_TARBALL}" ]]; then
  PROOT_URL="https://github.com/proot-me/proot/archive/refs/tags/v${PROOT_VERSION}.tar.gz"
  echo "[build-proot] download proot src: ${PROOT_URL}"
  curl -fL --retry 4 --retry-delay 2 -o "${PROOT_TARBALL}" "${PROOT_URL}"
fi
if [[ ! -f "${TALLOC_TARBALL}" ]]; then
  TALLOC_URL="https://www.samba.org/ftp/talloc/talloc-${TALLOC_VERSION}.tar.gz"
  echo "[build-proot] download talloc src: ${TALLOC_URL}"
  curl -fL --retry 4 --retry-delay 2 -o "${TALLOC_TARBALL}" "${TALLOC_URL}"
fi

WORK="${DL_DIR}/build"
rm -rf "${WORK}"
mkdir -p "${WORK}"
( cd "${WORK}" && tar -xzf "${PROOT_TARBALL}" && tar -xzf "${TALLOC_TARBALL}" )

PROOT_DIR=$(find "${WORK}" -maxdepth 2 -type d -name "proot-*" | head -n1)
TALLOC_DIR=$(find "${WORK}" -maxdepth 2 -type d -name "talloc-*" | head -n1)
echo "[build-proot] proot src:  ${PROOT_DIR}"
echo "[build-proot] talloc src: ${TALLOC_DIR}"

# ─── 2) Build talloc statikusan (csak talloc.c, waf nélkül) ──────────────
TALLOC_OUT="${WORK}/talloc-out"
mkdir -p "${TALLOC_OUT}"

# A talloc.c néhány HAVE_* makrót vár az autoconf-config.h-ból; mi NDK-val
# (Bionic glibc-szerű) cross-compile-olunk, és ezeket explicit megadjuk —
# minden modern POSIX feature elérhető NDK 24+-on.
TALLOC_DEFS=(
    -DHAVE_VA_COPY=1
    -DHAVE_INTPTR_T=1
    -DHAVE_VOID_PTR=1
    -DHAVE_STDLIB_H=1
    -DHAVE_STDIO_H=1
    -DHAVE_STDARG_H=1
    -DHAVE_STDBOOL_H=1
    -DHAVE_STDINT_H=1
    -DHAVE_STRING_H=1
    -DHAVE_UNISTD_H=1
    -DHAVE_TIME_H=1
    -DHAVE_MEMSET=1
    -DHAVE_GETPAGESIZE=1
    -D_GNU_SOURCE=1
)

echo "[build-proot] compile talloc.c"
"${CC}" -c -O2 -fPIC \
    "${TALLOC_DEFS[@]}" \
    -I"${TALLOC_DIR}" \
    -I"${TALLOC_DIR}/lib/talloc" \
    "${TALLOC_DIR}/lib/talloc/talloc.c" \
    -o "${TALLOC_OUT}/talloc.o"

"${AR}" rcs "${TALLOC_OUT}/libtalloc.a" "${TALLOC_OUT}/talloc.o"
ls -lh "${TALLOC_OUT}/libtalloc.a"

# ─── 3) Build proot (saját src/Makefile-jével) ────────────────────────────
PROOT_SRC="${PROOT_DIR}/src"
if [[ ! -f "${PROOT_SRC}/Makefile" ]]; then
  echo "[build-proot] HIBA: ${PROOT_SRC}/Makefile nem létezik." >&2
  find "${PROOT_DIR}" -maxdepth 3 -name Makefile >&2 || true
  exit 4
fi

# proot a `xxd` parancsot várja a loader header-ének előállításához.
if ! command -v xxd >/dev/null; then
  echo "[build-proot] HIBA: 'xxd' nincs telepítve (apt install xxd / vim-common)" >&2
  exit 5
fi

# A proot Makefile a CC/LD/CFLAGS/LDFLAGS env-változókat figyelembe veszi.
# Cross-compile esetén minden eszközt explicit átadunk; CFLAGS-ban a talloc.h
# kereshetősége + az NDK API szint definíciói.
echo "[build-proot] make -C ${PROOT_SRC} (NDK cross-compile)"
make -C "${PROOT_SRC}" -j"$(nproc)" \
    CC="${CC}" \
    LD="${LD}" \
    AR="${AR}" \
    OBJCOPY="${OBJCOPY}" \
    HOST_CC="cc" \
    CFLAGS="-O2 -I${TALLOC_DIR}/lib/talloc -DGIT_VERSION=\"v${PROOT_VERSION}\"" \
    LDFLAGS="-L${TALLOC_OUT} -ltalloc -static-libgcc" \
    proot

PROOT_BIN="${PROOT_SRC}/proot"
if [[ ! -f "${PROOT_BIN}" ]]; then
  echo "[build-proot] HIBA: nincs proot bin a build végén." >&2
  ls -la "${PROOT_SRC}" >&2 || true
  exit 6
fi

cp -v "${PROOT_BIN}" "${DL_DIR}/proot"
"${NDK_BIN}/llvm-strip" "${DL_DIR}/proot" 2>/dev/null || true
chmod +x "${DL_DIR}/proot"

# Sanity
file "${DL_DIR}/proot" || true
ls -lh "${DL_DIR}/proot"
echo "[build-proot] kész: ${DL_DIR}/proot"
