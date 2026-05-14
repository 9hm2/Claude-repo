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
