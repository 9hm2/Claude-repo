#!/usr/bin/env bash
# build-fuse-shim.sh — libkali_fuse_shim.so cross-compile ARM64 glibc-ABI-ra.
#
# Két MIÉRT NEM az NDK:
#   1) Az NDK aarch64-linux-android-clang Bionic libc-re fordít — a chrooted
#      Kali bash GLIBC 2.36+-on fut, ABI-inkompatibilis.
#   2) A `LD_PRELOAD` a dinamikus-linker `RTLD_NEXT`-jén át a `glibc` dlsym-
#      jeit várja; Bionic-shim ezt nem teljesíti.
#
# Output: assets/rootfs/libkali_fuse_shim.so

set -euo pipefail

SCRIPT_DIR="$(realpath "$(dirname "$0")")"
APP_DIR="$(realpath "$SCRIPT_DIR/..")"
SRC="$APP_DIR/app/src/main/cpp_shim/libkali_fuse_shim.c"
OUT_DIR="$APP_DIR/app/src/main/assets/rootfs"
OUT="$OUT_DIR/libkali_fuse_shim.so"

CC="${CC:-aarch64-linux-gnu-gcc}"
if ! command -v "$CC" >/dev/null; then
    echo "[fuse-shim] HIBA: nincs $CC. apt install gcc-aarch64-linux-gnu" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

echo "[fuse-shim] $CC -shared -fPIC -O2 ..."
"$CC" -shared -fPIC -O2 -Wall \
    -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
    -o "$OUT" "$SRC" \
    -ldl -lpthread

# strip
"${STRIP:-aarch64-linux-gnu-strip}" "$OUT" 2>/dev/null || true

file "$OUT"
ls -lh "$OUT"
echo "[fuse-shim] kész."
