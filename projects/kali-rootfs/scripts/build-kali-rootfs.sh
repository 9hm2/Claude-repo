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
  # FONTOS: a modern apt sqv verifikátora raw OpenPGP-packet streamet vár
  # a /etc/apt/trusted.gpg.d/ fájlokban. A `gpg --dearmor` gnupg 2.4+-on
  # KEYBOX (KBXf) formátumot ad — ezt az sqv elutasítja "Missing key"-vel.
  # Megoldás: import → temp-keyring → export (raw OpenPGP packets).
  echo "[kali] gpg import + export → raw OpenPGP packets keyring"
  TEMP_KEYRING="$(mktemp -d)/temp-keyring.gpg"
  gpg --no-default-keyring --keyring "${TEMP_KEYRING}" \
      --import < "${DL_DIR}/kali-archive-key.asc"
  gpg --no-default-keyring --keyring "${TEMP_KEYRING}" \
      --export > "${KEYRING_FILE}"
  rm -rf "$(dirname "${TEMP_KEYRING}")"
  # Verify raw OpenPGP packets format (NEM keybox):
  head -c 4 "${KEYRING_FILE}" | od -An -c | grep -q "KBXf" && {
    echo "[kali] HIBA: keyring KBXf-formátum (keybox), apt sqv NEM fogadja el" >&2
    exit 3
  } || true
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

# 6) Sources-list bővítés: alapból CSAK 'main' van; a felhasználói kérésre
# 'contrib non-free non-free-firmware' is kerüljön bele, hogy firmware-realtek
# és hasonló non-free csomagok telepíthetők legyenek.
echo "[kali] sources.list bővítés: main + contrib + non-free + non-free-firmware"
${SUDO} tee "${ROOTFS_DIR}/etc/apt/sources.list" > /dev/null <<EOF
# Kali rolling — teljes komponens-lista
deb http://kali.download/kali kali-rolling main contrib non-free non-free-firmware
deb-src http://kali.download/kali kali-rolling main contrib non-free non-free-firmware
EOF

# 7) Pre-installolás: az alaprootfs-be belerakjuk a USB-debug + hálózat +
# Wi-Fi csomagokat, hogy a felhasználónak ne kelljen utólag apt install-olnia.
#   - usbutils: lsusb (+ libusb-1.0-0 dependency)
#   - hwdata: usb.ids adatbázis (eltünteti az 'unable to initialize usb spec' warning-ot)
#   - firmware-realtek: Realtek Wi-Fi/Ethernet chipek firmware-ei
#   - firmware-atheros: Atheros Wi-Fi firmware (ath9k_htc, stb.)
#   - firmware-misc-nonfree: egyéb non-free firmware (Broadcom, Mediatek, ...)
#   - net-tools: ifconfig, netstat, route (legacy de gyakori)
#   - iproute2: ip, ss (modern)
#   - kmod: lsmod, modprobe, insmod, rmmod
#   - iw: iw dev wlan0 ... (Wi-Fi config)
#   - wireless-tools: iwconfig, iwlist (legacy Wi-Fi config)
#   - wpasupplicant: Wi-Fi WPA/WPA2/WPA3 association
#   - pciutils: lspci (+ pci.ids adatbázis)
#   - util-linux: dmesg + sok más (csak ha még nincs)
#   - rfkill: Wi-Fi/Bluetooth radio-engedély kezelés
# A `--no-install-recommends` az APK-méret féken tartására, csak ami szükséges.
echo "[kali] copy Kali archive keyring to chroot /etc/apt/trusted.gpg.d/"
# Az apt sqv verifikátora a chroot belsejében saját trusted.gpg.d-jét nézi;
# a debootstrap --keyring csak az alap-csomagoknak adott GPG-jelet, NEM
# telepítette a keyringet a rootfs-be. Másoljuk most.
${SUDO} mkdir -p "${ROOTFS_DIR}/etc/apt/trusted.gpg.d"
${SUDO} cp "${KEYRING_FILE}" "${ROOTFS_DIR}/etc/apt/trusted.gpg.d/kali-archive-keyring.gpg"
${SUDO} chmod 644 "${ROOTFS_DIR}/etc/apt/trusted.gpg.d/kali-archive-keyring.gpg"

echo "[kali] apt update + install (usbutils, hwdata, firmware-realtek)"
${SUDO} chroot "${ROOTFS_DIR}" /usr/bin/env -i \
    DEBIAN_FRONTEND=noninteractive \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    HOME=/root \
    apt-get update
${SUDO} chroot "${ROOTFS_DIR}" /usr/bin/env -i \
    DEBIAN_FRONTEND=noninteractive \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    HOME=/root \
    apt-get install -y --no-install-recommends \
        usbutils \
        hwdata \
        pciutils \
        firmware-realtek \
        net-tools \
        iproute2 \
        kmod \
        iw \
        wireless-tools \
        wpasupplicant \
        util-linux \
        rfkill
        # firmware-atheros (34MB) és firmware-misc-nonfree (4.5MB) KIHAGYVA —
        # APK 100MB GitHub-limit. A user-nek Realtek 2357:011e device-a van;
        # ha más chipset firmware kell, apt install firmware-atheros / firmware-misc-nonfree.

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
