#!/usr/bin/env bash
# Összeszedi az APK-asset-eket: proot ARM64 binary + kalifs.tar.xz +
# egy kis launcher script, ami az első indításnál kibontja a rootfs-t és
# proot-tal beléindul.
set -euo pipefail

DL_DIR="${1:?usage: build-bundle.sh <dl_dir> <out_dir>}"
OUT_DIR="${2:?out_dir}"

mkdir -p "${OUT_DIR}"

if [[ ! -f "${DL_DIR}/proot" ]]; then
  echo "[bundle] HIBA: nincs ${DL_DIR}/proot — futtasd a fetch-proot-ot" >&2
  exit 1
fi
if [[ ! -f "${DL_DIR}/kalifs-arm64-minimal.tar.xz" ]]; then
  echo "[bundle] HIBA: nincs kalifs tarball — futtasd a fetch-kali-rootfs-t" >&2
  exit 1
fi

# Az APK assets/-be ezek a fájlok kerülnek (a kali-term-app workflow
# kifolyatja a saját asset-mappájába):
#   assets/rootfs/proot                       — bin
#   assets/rootfs/kalifs-arm64-minimal.tar.xz — fs
#   assets/rootfs/launch.sh                   — első indítás script
cp -v "${DL_DIR}/proot"                              "${OUT_DIR}/proot"
cp -v "${DL_DIR}/kalifs-arm64-minimal.tar.xz"        "${OUT_DIR}/kalifs-arm64-minimal.tar.xz"

# Egy minimal launcher amit a kaliterm Java/Kotlin oldal hív, miután
# a fájlok az app private storage-ban vannak. A `$PREFIX` az appdata
# absolute path-ja (lásd kaliterm/RootFsManager.kt).
cat > "${OUT_DIR}/launch.sh" <<'EOF'
#!/system/bin/sh
# kaliterm rootfs launcher — proot wrapper.
#
# Bemenet (env-ből):
#   PREFIX        a kaliterm appdata absolute path-ja (rootfs alatt)
#   ROOTFS_DIR    PREFIX/rootfs — a kicsomagolt Kali fa
#   USER_HOME     a "kali" user home-ja a rootfs-en (default /root)
#
# Kimenet: a Kali shell-be belépés interactive `bash -l`-lel.

set -e

: "${PREFIX:?PREFIX env változó kell}"
: "${ROOTFS_DIR:=$PREFIX/rootfs}"
: "${USER_HOME:=/root}"

PROOT="$PREFIX/proot"
chmod +x "$PROOT" 2>/dev/null || true

if [ ! -d "$ROOTFS_DIR" ]; then
  echo "[launch] HIBA: nincs kicsomagolt rootfs $ROOTFS_DIR-ben" >&2
  exit 1
fi

# proot opciók:
#   -0          : minden uid 0-nak látszik a chrootban (kali tooling root-ot vár)
#   -r          : rootfs path
#   -b src:dst  : bind-mount a host fájlrendszerből
#   -w          : working directory induláskor
#   --link2symlink : '/dev/...' és hard-linkek workaround (Termux best practice)
#   --kill-on-exit : ha a kaliterm bezárul, a proot-ben futó folyamatok is mennek
exec "$PROOT" \
    --link2symlink \
    --kill-on-exit \
    -0 \
    -r "$ROOTFS_DIR" \
    -b /dev \
    -b /proc \
    -b /sys \
    -b /sdcard \
    -w "$USER_HOME" \
    /usr/bin/env -i \
        HOME="$USER_HOME" \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        TERM="${TERM:-xterm-256color}" \
        LANG=C.UTF-8 \
        /bin/bash -l
EOF
chmod +x "${OUT_DIR}/launch.sh"

echo "[bundle] kész — kimenetek:"
ls -lh "${OUT_DIR}/"
