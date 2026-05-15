# kaliterm — Kali rootfs + proot bundle.
#
# A kaliterm Android-app két komponenst igényel a userspace Linux-élményhez:
#   1) `proot` (statikus ARM64 Bionic bináris) — root nélküli chroot wrapper,
#      ptrace-szel intercepts a chroot/uid/path-rezolveléseket. Termux-os
#      proot ugyanaz az upstream PRoot-Me/PRoot, NDK-val fordítva.
#   2) `kalifs.tar.xz` — Kali ARM64 minimal rootfs, a Kali NetHunter projekt
#      friss képe (~250 MB tömörítve, ~700 MB kicsomagolva).
#
# A két fájlt az APK assets/-ébe csomagoljuk; első indításkor a kaliterm app
# kibontja az adata-mappába és proot-tal beléindul.
#
# Build target-ek:
#   make fetch-proot         — proot static ARM64 binary letöltése
#   make fetch-kali-rootfs   — Kali minimal rootfs tarball letöltése
#   make bundle              — mindkettő, kimenet az out/-ba
#   make clean               — letöltött artifactok törlése
#
# A CI workflow (.github/workflows/kali-rootfs.yml) ezt futtatja és a
# végeredményt artifactként feltölti, amit a kali-term-app build elvesz.

OUT_DIR ?= $(CURDIR)/out
DL_DIR  ?= $(CURDIR)/downloads

.PHONY: all bundle fetch-proot fetch-kali-rootfs clean help

all: bundle help

help:
	@echo "Targets:"
	@echo "  make fetch-proot         — proot ARM64 Bionic static binary"
	@echo "  make fetch-kali-rootfs   — Kali ARM64 minimal rootfs (~250 MB)"
	@echo "  make bundle              — mindkettőt ÖSSZESZEDI az out/-ba"
	@echo "  make clean               — letöltöttek + bundle törlése"

fetch-proot:
	@mkdir -p $(DL_DIR)
	@$(CURDIR)/scripts/fetch-proot.sh "$(DL_DIR)"

fetch-kali-rootfs:
	@mkdir -p $(DL_DIR)
	@$(CURDIR)/scripts/fetch-kali-rootfs.sh "$(DL_DIR)"

bundle: fetch-proot fetch-kali-rootfs
	@mkdir -p $(OUT_DIR)
	@$(CURDIR)/scripts/build-bundle.sh "$(DL_DIR)" "$(OUT_DIR)"

clean:
	rm -rf $(OUT_DIR) $(DL_DIR)
