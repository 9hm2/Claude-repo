#!/usr/bin/env bash
# Letölti az LKL (Linux Kernel Library) forrását.
# Az LKL a mainline Linux egy aktív fork-ja, ami egy `arch/lkl/` host-port-ot
# ad hozzá, és a `tools/lkl/` alatt biztosítja a userspace build-rendszert.
set -euo pipefail

LKL_REPO="${1:?usage: fetch-lkl.sh <repo> <branch> <commit?> <build_dir>}"
LKL_BRANCH="${2:?branch}"
LKL_COMMIT="${3:-}"
BUILD_DIR="${4:?build_dir}"

SRC_DIR="${BUILD_DIR}/linux-lkl"

if [[ -d "${SRC_DIR}/.git" ]]; then
  echo "[fetch] update existing checkout"
  cd "${SRC_DIR}"
  git fetch --depth 50 origin "${LKL_BRANCH}"
else
  mkdir -p "${BUILD_DIR}"
  echo "[fetch] clone ${LKL_REPO} (branch=${LKL_BRANCH})"
  git clone --depth 50 --branch "${LKL_BRANCH}" --single-branch \
    "${LKL_REPO}" "${SRC_DIR}"
  cd "${SRC_DIR}"
fi

if [[ -n "${LKL_COMMIT}" ]]; then
  echo "[fetch] checkout pinned commit: ${LKL_COMMIT}"
  git checkout --detach "${LKL_COMMIT}"
else
  echo "[fetch] using tip of ${LKL_BRANCH}: $(git rev-parse HEAD)"
fi

echo "[fetch] kész: ${SRC_DIR}"
ls "${SRC_DIR}/tools/lkl" >/dev/null  # sanity check: LKL fa

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Out-of-tree driverek beágyazása a kernel-tree-be a patch-fázis ELŐTT,
# hogy a patches/ a parent Makefile/Kconfig hivatkozásait felülírhassa.
# Jelenleg: morrownr/8812au-20210820 — RTL8812AU/8821AU/8811AU/8814AU
# (2357:011e TP-Link Archer T2U Plus, Alfa AWUS036ACH, stb.).
"${SCRIPT_DIR}/fetch-rtl8812au.sh" "${SRC_DIR}"

# Helyi patch-ek alkalmazása (idempotens: `patch -N` ugorja a már alkalmazottakat).
# A patch outputja LÁTHATÓ marad — CI-debugban kulcs hogy melyik hunk hová ment.
PATCHES_DIR="${SCRIPT_DIR}/../patches"
if [[ -d "${PATCHES_DIR}" ]]; then
    shopt -s nullglob
    for p in "${PATCHES_DIR}"/*.patch; do
        pname=$(basename "$p")
        echo "[fetch] === patch: ${pname} ==="
        set +e
        patch -p1 -N --no-backup-if-mismatch -d "${SRC_DIR}" -i "$p"
        rc=$?
        set -e
        case "${rc}" in
          0) echo "[fetch]   ✓ alkalmazva: ${pname}" ;;
          1) echo "[fetch]   → már alkalmazva: ${pname}" ;;
          *) echo "[fetch]   ✗ HIBA: patch parancs rc=${rc}, ${pname}" >&2; exit 1 ;;
        esac
    done
    # Verifikáció: a patch-eink ténylegesen érvényesültek-e a forrásban
    if [[ -f "${SRC_DIR}/tools/lkl/Makefile.autoconf" ]] \
       && ! grep -q "aarch64-linux-android" "${SRC_DIR}/tools/lkl/Makefile.autoconf"; then
        echo "[fetch] ✗ HIBA: a Makefile.autoconf patch NEM látszik a forrásban!" >&2
        echo "[fetch]   tartalom körül:" >&2
        grep -n "llvm_target_to_ld_fmt\|Unsupported LLVM" "${SRC_DIR}/tools/lkl/Makefile.autoconf" || true
        exit 1
    fi
    echo "[fetch] verifikáció: patch-ek a forrásban OK"
fi
