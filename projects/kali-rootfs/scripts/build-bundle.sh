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
# kaliterm rootfs launcher — proot wrapper diagnosztikával.
#
# Bemenet (env-ből):
#   PREFIX        a kaliterm appdata absolute path-ja (rootfs-bundle alatt)
#   ROOTFS_DIR    a kicsomagolt Kali fa absolute path-ja
#   USER_HOME     a Kali user home-ja a rootfs-en (default /root)
#
# Kimenet: vagy a Kali shell-be belépés (sikeres esetben), vagy egy
# Android-shell amiben látszanak a hibák. SOSEM tér vissza üres state-tel
# a hívóhoz — ha minden bedől, /system/bin/sh dropoljuk hogy a user
# manuálisan tudjon vizsgálódni.

: "${PREFIX:?PREFIX env változó kell}"
: "${ROOTFS_DIR:=$PREFIX/../rootfs}"
: "${USER_HOME:=/root}"

PROOT="$PREFIX/proot"

echo "════════════════════════════════════════════════"
echo " kaliterm — proot launcher"
echo "════════════════════════════════════════════════"
echo "PREFIX     = $PREFIX"
echo "ROOTFS_DIR = $ROOTFS_DIR"
echo "PROOT      = $PROOT"
echo "USER_HOME  = $USER_HOME"
echo

# 1) proot bin sanity
if [ ! -f "$PROOT" ]; then
    echo "✗ HIBA: $PROOT nem létezik"
    ls -la "$PREFIX" 2>&1 | head -10
    echo
    echo "Drop Android sh-ba a vizsgálathoz."
    exec /system/bin/sh
fi
chmod +x "$PROOT" 2>/dev/null || true
if [ ! -x "$PROOT" ]; then
    echo "✗ HIBA: $PROOT nem futtatható"
    ls -la "$PROOT"
    exec /system/bin/sh
fi

# 2) proot --version (gyors sanity, hogy linkel-e a libc)
echo "─── proot --version ───"
"$PROOT" --version 2>&1 || {
    echo "✗ HIBA: proot --version sikertelen (linkelési vagy ABI hiba?)"
    exec /system/bin/sh
}
echo

# 3) Rootfs sanity
if [ ! -d "$ROOTFS_DIR" ]; then
    echo "✗ HIBA: $ROOTFS_DIR nincs (rootfs nem lett kicsomagolva?)"
    exec /system/bin/sh
fi
if [ ! -x "$ROOTFS_DIR/bin/bash" ]; then
    echo "✗ HIBA: $ROOTFS_DIR/bin/bash nincs vagy nem futtatható"
    ls -la "$ROOTFS_DIR/bin/" 2>&1 | head -20
    exec /system/bin/sh
fi
echo "✓ rootfs OK ($(ls "$ROOTFS_DIR" | wc -l) toplevel-bejegyzés)"
echo

# 4) Indítás — bash -l a Kali rootfs-ben
echo "─── proot indítása → /bin/bash ───"
exec "$PROOT" \
    --link2symlink \
    --kill-on-exit \
    -0 \
    -r "$ROOTFS_DIR" \
    -b /dev \
    -b /proc \
    -b /sys \
    -w "$USER_HOME" \
    /usr/bin/env -i \
        HOME="$USER_HOME" \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        TERM="${TERM:-xterm-256color}" \
        LANG=C.UTF-8 \
        /bin/bash -l

# Ide csak akkor jutunk, ha az exec proot SIKERTELEN volt (tipikusan
# fork failure vagy ptrace permission denied). Drop sh-ba.
echo "✗ HIBA: exec proot sikertelen ($?)"
exec /system/bin/sh
EOF
chmod +x "${OUT_DIR}/launch.sh"

echo "[bundle] kész — kimenetek:"
ls -lh "${OUT_DIR}/"
