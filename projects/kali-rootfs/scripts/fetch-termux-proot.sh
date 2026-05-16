#!/usr/bin/env bash
# Termux pre-built proot ARM64 binary letöltése a Termux apt repo-jából,
# + dependency libtalloc.so.2.
#
# MIÉRT NEM saját build:
#   A `proot-me/proot` upstream v5.4.0-ből saját NDK-build-elt binárisunk
#   az `--link2symlink` (Termux extension) NEM ismerete + a `-0` flag silent
#   255-tel halt el chrootban (seccomp/ptrace incompat Android-specifikus
#   kernel-hook patch nélkül). A Termux fork ezeket évek óta működteti,
#   ezért közvetlenül a Termux .deb-jét húzzuk le.
#
# Bemenet: $1 = download_dir
# Kimenet:
#   $1/proot          — Termux proot ARM64 binary (dinamikus, libtalloc.so.2-re)
#   $1/libtalloc.so.2 — Termux talloc library (proot dependencia)

set -euo pipefail

DL_DIR="${1:?usage: fetch-termux-proot.sh <download_dir>}"
mkdir -p "${DL_DIR}"

PROOT_VER="${PROOT_VER:-5.1.107-71}"
TALLOC_VER="${TALLOC_VER:-2.4.3}"
REPO_BASE="https://packages.termux.dev/apt/termux-main/pool/main"

extract_from_deb() {
    local deb_url="$1" deb_file="$2" target_filename="$3" out_path="$4"
    if [[ ! -f "${deb_file}" ]]; then
        echo "[fetch-termux-proot] download ${deb_url}"
        curl -fL --retry 4 --retry-delay 2 -o "${deb_file}" "${deb_url}"
    fi
    local work="${deb_file}.unpack"
    rm -rf "${work}"; mkdir -p "${work}"
    # absolute path — `cd ${work}` után a relatív útvonal elveszne
    local deb_abs; deb_abs="$(realpath "${deb_file}")"
    ( cd "${work}" && ar x "${deb_abs}" )
    local data_tar; data_tar=$(find "${work}" -maxdepth 1 -name 'data.tar.*' | head -1)
    [[ -n "${data_tar}" ]] || { echo "[fetch-termux-proot] HIBA: data.tar.* nincs ${deb_file}-ben" >&2; return 2; }
    mkdir -p "${work}/data"; tar -xf "${data_tar}" -C "${work}/data"
    local found; found=$(find "${work}/data" -type f -name "${target_filename}" | head -1)
    [[ -n "${found}" ]] || { echo "[fetch-termux-proot] HIBA: ${target_filename} nincs ${deb_file}-ben" >&2; find "${work}/data" -type f | head -20 >&2; return 3; }
    cp -v "${found}" "${out_path}"
    rm -rf "${work}"
}

# 1) libtalloc.so.2 — proot futtatásához kötelező. A .deb-ben a `libtalloc.so.2`
#    egy symlink a `libtalloc.so.2.x.y` real-fájlra; mi a real-fájlt
#    húzzuk ki és libtalloc.so.2 néven mentjük.
extract_from_deb \
    "${REPO_BASE}/libt/libtalloc/libtalloc_${TALLOC_VER}_aarch64.deb" \
    "${DL_DIR}/libtalloc_${TALLOC_VER}_aarch64.deb" \
    "libtalloc.so.${TALLOC_VER}" \
    "${DL_DIR}/libtalloc.so.2"

# 2) proot binary
extract_from_deb \
    "${REPO_BASE}/p/proot/proot_${PROOT_VER}_aarch64.deb" \
    "${DL_DIR}/proot_${PROOT_VER}_aarch64.deb" \
    "proot" \
    "${DL_DIR}/proot"
chmod +x "${DL_DIR}/proot"

# 3) ELF-patch: Android jniLibs csak `lib*.so` mintát fogad el. A Termux
#    proot DT_NEEDED-je `libtalloc.so.2` (Linux-szokvány), és a libtalloc
#    SONAME-ja is `libtalloc.so.2`. Mindkettőt `libtalloc.so`-ra patcheljük,
#    így az APK jniLibs-be `libproot.so` + `libtalloc.so` néven kerülnek,
#    a runtime-linker megtalálja.
if ! command -v patchelf >/dev/null; then
    echo "[fetch-termux-proot] HIBA: patchelf nincs telepítve (apt install patchelf)" >&2
    exit 4
fi
echo
echo "─── ELF DT_NEEDED + SONAME patchelf ───"
patchelf --replace-needed libtalloc.so.2 libtalloc.so "${DL_DIR}/proot"
patchelf --set-soname     libtalloc.so                 "${DL_DIR}/libtalloc.so"
echo "proot DT_NEEDED:    $(patchelf --print-needed "${DL_DIR}/proot" | tr '\n' ' ')"
echo "libtalloc.so SONAME: $(patchelf --print-soname "${DL_DIR}/libtalloc.so")"

# Sanity
echo
echo "=== resultados ==="
file "${DL_DIR}/proot" || true
file "${DL_DIR}/libtalloc.so" || true
ls -lh "${DL_DIR}/proot" "${DL_DIR}/libtalloc.so"
echo "[fetch-termux-proot] kész"
