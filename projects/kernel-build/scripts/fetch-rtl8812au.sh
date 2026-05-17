#!/usr/bin/env bash
# Letölti a morrownr/8812au-20210820 out-of-tree drivert és kernel-fába
# integrálja a `drivers/net/wireless/realtek/rtl8812au/` alá.
#
# Indok: a mainline `rtl8xxxu` driver NEM támogatja a RTL8821AU / RTL8811AU /
# RTL8812AU chipset-eket. A TP-Link Archer T2U Plus (2357:011e), Alfa AWUS036ACH,
# és sok más USB Wi-Fi adapter ezt használja → out-of-tree driver kell.
#
# Az upstream Makefile-jét úgy írták meg, hogy ha `KERNELRELEASE` be van
# állítva (= kernel-tree-ből hívva), az in-tree `obj-$(CONFIG_RTL8812AU) :=
# 8812au.o` path-on megy. A CONFIG_RTL8812A / CONFIG_USB_HCI fix-érték a
# Makefile elején — nem kell defconfig-fragment. A patches/-ben a parent
# realtek/Kconfig + Makefile-be be van fűzve a subdirektorija.
set -euo pipefail

SRC_DIR="${1:?usage: fetch-rtl8812au.sh <kernel_src_dir>}"
DRIVER_REPO="${RTL8812AU_REPO:-https://github.com/morrownr/8812au-20210820.git}"
DRIVER_BRANCH="${RTL8812AU_BRANCH:-main}"
# Pin a fetch idején-aktuális commit-ra — reproducible build.
DRIVER_COMMIT="${RTL8812AU_COMMIT:-994a225434bdea6767886f74368b6a8cccf19d26}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_DIR="${SCRIPT_DIR}/../build/rtl8812au-cache"
DEST_DIR="${SRC_DIR}/drivers/net/wireless/realtek/rtl8812au"

if [[ -d "${DEST_DIR}" ]] && [[ -f "${DEST_DIR}/.kaliterm-imported" ]]; then
    echo "[rtl8812au] már be van másolva: ${DEST_DIR}"
    exit 0
fi

if [[ ! -d "${CACHE_DIR}/.git" ]]; then
    echo "[rtl8812au] clone ${DRIVER_REPO} branch=${DRIVER_BRANCH}"
    mkdir -p "$(dirname "${CACHE_DIR}")"
    git clone --depth 50 --branch "${DRIVER_BRANCH}" --single-branch \
        "${DRIVER_REPO}" "${CACHE_DIR}"
fi
cd "${CACHE_DIR}"
git fetch --depth 50 origin "${DRIVER_BRANCH}" || true
git checkout --detach "${DRIVER_COMMIT}"
echo "[rtl8812au] commit pinned: $(git rev-parse HEAD)"
cd - >/dev/null

mkdir -p "${DEST_DIR}"
# Forrás-másolás: .git nélkül, .gitignore-okat kihagyva
rsync -a --delete \
    --exclude='.git/' --exclude='.github/' --exclude='*.ko' --exclude='*.o' \
    --exclude='.tmp_versions/' --exclude='Module.symvers' \
    "${CACHE_DIR}/" "${DEST_DIR}/"

# Toolchain-kompatibilitás: az upstream Makefile GCC-13-specifikus warning-
# disable flag-eket ad hozzá unconditionally, amit a NDK clang `-Werror`
# módja unknown-warning-optionnek vesz → fail. Wrappel-jük cc-option-be,
# ami csak akkor ad hozzá flag-et ha a compiler ismeri.
M="${DEST_DIR}/Makefile"
# Szimbólumütközés-fix: a kernel `lib/crypto/aes.c` exportál `aes_encrypt`-et
# is, és a rtl8812au saját `core/crypto/aes-internal-enc.c`-je is `aes_encrypt`-ként
# definiálja → in-tree-build duplicate-symbol link-error. -D macro-szinten
# globálisan renamel-jük a rtl8812au-belüli hívásokra.
echo 'EXTRA_CFLAGS += -Daes_encrypt=rtl8812au_aes_encrypt' >> "$M"
echo "[rtl8812au] Makefile aes_encrypt → rtl8812au_aes_encrypt rename hozzáadva"

# Egyik régen-existed flag-et nyitva hagyjuk; a GCC-13 specifikus
# csoportot meg cc-option-be wrappel-jük.
sed -i -E \
    -e 's/^EXTRA_CFLAGS \+= -Wno-enum-int-mismatch$/EXTRA_CFLAGS += $(call cc-option,-Wno-enum-int-mismatch)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-stringop-overread$/EXTRA_CFLAGS += $(call cc-option,-Wno-stringop-overread)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-enum-conversion$/EXTRA_CFLAGS += $(call cc-option,-Wno-enum-conversion)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-int-in-bool-context$/EXTRA_CFLAGS += $(call cc-option,-Wno-int-in-bool-context)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-missing-prototypes$/EXTRA_CFLAGS += $(call cc-option,-Wno-missing-prototypes)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-missing-declarations$/EXTRA_CFLAGS += $(call cc-option,-Wno-missing-declarations)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-empty-body$/EXTRA_CFLAGS += $(call cc-option,-Wno-empty-body)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-address$/EXTRA_CFLAGS += $(call cc-option,-Wno-address)/' \
    -e 's/^EXTRA_CFLAGS \+= -Wno-cast-function-type$/EXTRA_CFLAGS += $(call cc-option,-Wno-cast-function-type)/' \
    "$M"
echo "[rtl8812au] Makefile cc-option wrap: $(grep -c 'cc-option' "$M") flag-et patched"

# Idempotens marker — a következő build-script-futás látja hogy már be van másolva.
date -u +'rtl8812au integrated %Y-%m-%dT%H:%M:%SZ' > "${DEST_DIR}/.kaliterm-imported"

echo "[rtl8812au] integráció kész: ${DEST_DIR}"
echo "[rtl8812au] fájlszám: $(find "${DEST_DIR}" -type f | wc -l)"
echo "[rtl8812au] méret:    $(du -sh "${DEST_DIR}" | awk '{print $1}')"
