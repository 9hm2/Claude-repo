#!/usr/bin/env bash
# Kali ARM64 minimal rootfs build debootstrap-pal + qemu-aarch64-static
# emuláció a 2nd-stage installhoz.
#
# Stratégia:
#   1) /usr/share/debootstrap/scripts/kali-rolling biztosítása. Ubuntu apt
#      debootstrap default-ban nem szállítja, de a Kali-rolling Debian-sid
#      alapú, így a sid script symlinkelhető rá.
#   2) Kali archive-keyring letöltése + dearmoring -> /tmp/kali-keyring.gpg
#      (a debootstrap GPG-checkjéhez kell).
#   3) debootstrap --foreign --arch=arm64 (1st stage) — ez csak letölti és
#      kicsomagolja a deb-eket az új rootfs-be, NEM futtat semmit.
#   4) qemu-aarch64-static másolása a rootfs/usr/bin/-be — innen tudja
#      majd a chrooted kontextus aarch64 binárisokat futtatni.
#   5) chroot ROOTFS /debootstrap/debootstrap --second-stage — ez most már
#      futtatja a postinst scripteket QEMU-emulált aarch64-en.
#   6) qemu cleanup + cache cleanup + tar.xz.
#
# Kimenet: $DL_DIR/kalifs-arm64-minimal.tar.xz
set -euo pipefail

DL_DIR="${1:?usage: build-kali-rootfs.sh <download_dir>}"
mkdir -p "${DL_DIR}"

# A debootstrap-nak (és a chroot-nak) root-jog kell. CI-ben sudo elérhető.
SUDO="${SUDO:-sudo}"

# 1) kali-rolling debootstrap script biztosítása.
SCRIPT_DIR="/usr/share/debootstrap/scripts"
if [[ ! -e "${SCRIPT_DIR}/kali-rolling" ]]; then
  if [[ -e "${SCRIPT_DIR}/sid" ]]; then
    echo "[kali] debootstrap script symlink: kali-rolling -> sid"
    ${SUDO} ln -sfv sid "${SCRIPT_DIR}/kali-rolling"
  else
    echo "[kali] HIBA: nincs ${SCRIPT_DIR}/sid se" >&2
    ls -la "${SCRIPT_DIR}" >&2 | head
    exit 1
  fi
fi

# 2) Kali archive-keyring (binary GPG kulcs a debootstrap-nak).
KEYRING_FILE="${DL_DIR}/kali-archive-keyring.gpg"
if [[ ! -f "${KEYRING_FILE}" ]]; then
  echo "[kali] download Kali archive key (ASCII-armored)"
  curl -fsSL --retry 4 --retry-delay 2 \
    -o "${DL_DIR}/kali-archive-key.asc" \
    https://archive.kali.org/archive-key.asc
  # `gpg --dearmor` raw-OpenPGP-packet stream-et ad — EZT VÁRJA a debootstrap
  # `--keyring`-je ÉS a modern apt `sqv` verifikátora `/etc/apt/trusted.gpg.d/`-ben.
  # A korábbi `gpg --import` GPG-native keybox-formátumot adott, amit az új
  # apt 'unsupported filetype'-tal elutasít.
  echo "[kali] dearmor → binary keyring"
  gpg --dearmor < "${DL_DIR}/kali-archive-key.asc" > "${KEYRING_FILE}"
  ls -lh "${KEYRING_FILE}"
fi

# 3-5) debootstrap futtatás.
ROOTFS_DIR="${DL_DIR}/rootfs"
${SUDO} rm -rf "${ROOTFS_DIR}"
${SUDO} mkdir -p "${ROOTFS_DIR}"

# qemu-aarch64-static a rendszerből; CI workflow ezt apt-tal telepíti.
QEMU_BIN=""
for cand in /usr/bin/qemu-aarch64-static /usr/local/bin/qemu-aarch64-static; do
  if [[ -x "${cand}" ]]; then QEMU_BIN="${cand}"; break; fi
done
if [[ -z "${QEMU_BIN}" ]]; then
  echo "[kali] HIBA: qemu-aarch64-static nem található. apt install qemu-user-static" >&2
  exit 2
fi
echo "[kali] qemu = ${QEMU_BIN}"

KALI_MIRROR="${KALI_MIRROR:-http://http.kali.org/kali}"
KALI_SUITE="${KALI_SUITE:-kali-rolling}"

# Minimal csomaglista: csak ami az indításhoz kell. A user később
# `apt install kali-linux-headless`-szel fel tudja húzni a teljes
# Kali tooling-ot (de az ~3 GB lenne — nem packoljuk az APK-ba).
INCLUDE_PKGS="apt,bash,coreutils,ca-certificates,gpgv,wget,curl,procps,less,nano"

echo "[kali] debootstrap --foreign (1st stage, arm64, ${KALI_SUITE})"
${SUDO} debootstrap \
    --foreign \
    --arch=arm64 \
    --variant=minbase \
    --include="${INCLUDE_PKGS}" \
    --keyring="${KEYRING_FILE}" \
    "${KALI_SUITE}" \
    "${ROOTFS_DIR}" \
    "${KALI_MIRROR}"

echo "[kali] copy ${QEMU_BIN} → rootfs/usr/bin/"
${SUDO} cp "${QEMU_BIN}" "${ROOTFS_DIR}/usr/bin/qemu-aarch64-static"

echo "[kali] debootstrap --second-stage (QEMU-emulated aarch64 chroot)"
${SUDO} chroot "${ROOTFS_DIR}" /debootstrap/debootstrap --second-stage

# Pre-konfigurálás: Kali apt-source kiírás már bent van a rootfs-ben
# (debootstrap kreálta), érintetlen. A user innentől apt-update + install.

echo "[kali] cleanup (qemu, cache, logs)"
${SUDO} rm -f  "${ROOTFS_DIR}/usr/bin/qemu-aarch64-static"
${SUDO} rm -rf "${ROOTFS_DIR}/var/cache/apt/archives"/*.deb \
              "${ROOTFS_DIR}/var/lib/apt/lists"/* \
              "${ROOTFS_DIR}/var/log"/*

echo "[kali] tar.xz pack (sokáig tarthat — sok kis fájl)"
TARBALL="${DL_DIR}/kalifs-arm64-minimal.tar.xz"
${SUDO} tar -cJf "${TARBALL}" -C "${ROOTFS_DIR}" .
${SUDO} chown "$(id -u):$(id -g)" "${TARBALL}"

ls -lh "${TARBALL}"
echo "[kali] kész: ${TARBALL}"
