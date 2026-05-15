#!/usr/bin/env bash
# Letölt egy statikus ARM64 Android proot binárisat a Termux APT repo-ból.
#
# Stratégia: NEM hardcode-oljuk a verziót (a 'pool/main/p/proot/' útvonal
# verzió-spec, és bármelyik release változhat). Helyette dinamikusan:
#   1) Letöltjük a stable/main/binary-aarch64/Packages.xz indexet
#   2) Kikeressük a "Package: proot" rekordot
#   3) Kiolvassuk a Filename: relatív útvonalat
#   4) Letöltjük a .deb-et (BASE/FILENAME)
#   5) Extract: ar x → data.tar.xz → proot ELF
#
# Mirror fallback: packages.termux.dev → packages-cf.termux.dev (CDN).
set -euo pipefail

DL_DIR="${1:?usage: fetch-proot.sh <download_dir>}"
mkdir -p "${DL_DIR}"

MIRRORS=(
  "https://packages.termux.dev/apt/termux-main"
  "https://packages-cf.termux.dev/apt/termux-main"
)

PKG_INDEX_REL="dists/stable/main/binary-aarch64/Packages.xz"
INDEX_FILE="${DL_DIR}/Packages.xz"
INDEX_TXT="${DL_DIR}/Packages.txt"

# 1) Index download — első mirror-ról amelyik él.
BASE_URL=""
for m in "${MIRRORS[@]}"; do
  echo "[fetch-proot] index próba: ${m}/${PKG_INDEX_REL}"
  if curl -fL --retry 3 --retry-delay 2 -o "${INDEX_FILE}" "${m}/${PKG_INDEX_REL}"; then
    BASE_URL="${m}"
    break
  fi
done
if [[ -z "${BASE_URL}" ]]; then
  echo "[fetch-proot] HIBA: egyetlen Termux-mirror sem érhető el" >&2
  exit 1
fi
echo "[fetch-proot] mirror: ${BASE_URL}"

# 2-3) Index dekódolás + proot rekord keresése.
xz -dc "${INDEX_FILE}" > "${INDEX_TXT}"
PROOT_FILENAME=$(awk '
    /^Package: proot$/ { in_proot = 1; next }
    /^Package: /       { in_proot = 0 }
    in_proot && /^Filename: / { sub(/^Filename: /, ""); print; exit }
' "${INDEX_TXT}")

if [[ -z "${PROOT_FILENAME}" ]]; then
  echo "[fetch-proot] HIBA: 'Package: proot' nincs az indexben." >&2
  echo "[fetch-proot] elérhető 'p'-vel kezdődő package-ek:" >&2
  awk '/^Package: p/{print}' "${INDEX_TXT}" | head -30 >&2
  exit 1
fi
echo "[fetch-proot] talált filename: ${PROOT_FILENAME}"

# 4) .deb letöltés
DEB_URL="${BASE_URL}/${PROOT_FILENAME}"
DEB_FILE="${DL_DIR}/$(basename "${PROOT_FILENAME}")"
echo "[fetch-proot] letöltés: ${DEB_URL}"
curl -fL --retry 4 --retry-delay 2 -o "${DEB_FILE}" "${DEB_URL}"

actual_sha=$(sha256sum "${DEB_FILE}" | awk '{print $1}')
echo "[fetch-proot] SHA-256: ${actual_sha}  ($(basename "${DEB_FILE}"))"

# 5) .deb extraction: ar(1) → data.tar.* → proot ELF
WORK_DIR="${DL_DIR}/proot-extract"
rm -rf "${WORK_DIR}" && mkdir -p "${WORK_DIR}"
( cd "${WORK_DIR}" && ar x "${DEB_FILE}" )
for cand in data.tar.xz data.tar.zst data.tar.gz data.tar.bz2 data.tar; do
  if [[ -f "${WORK_DIR}/${cand}" ]]; then
    echo "[fetch-proot] kicsomagolás: ${cand}"
    case "${cand}" in
      *.zst) ( cd "${WORK_DIR}" && zstd -dc "${cand}" | tar -x ) ;;
      *)     ( cd "${WORK_DIR}" && tar -xf "${cand}" ) ;;
    esac
    break
  fi
done

PROOT_BIN=$(find "${WORK_DIR}" -maxdepth 8 -name proot -type f | head -n1)
if [[ -z "${PROOT_BIN}" ]]; then
  echo "[fetch-proot] HIBA: nem találom a proot binárist a kicsomagolt .deb-ben." >&2
  find "${WORK_DIR}" -maxdepth 8 -name 'proot*' >&2 || true
  exit 1
fi
cp -v "${PROOT_BIN}" "${DL_DIR}/proot"
chmod +x "${DL_DIR}/proot"

# Sanity: ELF aarch64?
file "${DL_DIR}/proot" || true
echo "[fetch-proot] kész: ${DL_DIR}/proot ($(stat -c%s "${DL_DIR}/proot") byte)"
