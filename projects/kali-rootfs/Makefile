# kaliterm — Kali rootfs + proot bundle.
#
# A kaliterm Android-app két komponenst igényel a userspace Linux-élményhez:
#   1) `proot` (statikus ARM64 Bionic bináris) — root nélküli chroot wrapper,
#      ptrace-szel intercepts a chroot/uid/path-rezolveléseket. Forrásból
#      buildeljük (NDK aarch64-linux-android24-clang) — proot-me/proot
#      v5.4.0 + samba talloc 2.4.2; külső binary-source dependency NULLA,
#      reprodukálható.
#   2) `kalifs.tar.xz` — Kali ARM64 minimal rootfs, a Kali NetHunter projekt
#      friss képe (~250 MB tömörítve, ~700 MB kicsomagolva).
#
# A két fájlt az APK assets/-ébe csomagoljuk; első indításkor a kaliterm app
# kibontja az adata-mappába és proot-tal beléindul.
#
# Build target-ek:
#   make build-proot         — proot statikus ARM64 binary FORRÁSBÓL (NDK)
#                              ANDROID_NDK_HOME env változó kötelező
#   make build-kali-rootfs   — Kali ARM64 rootfs debootstrap+qemu-static-tal
#                              (sudo-jog kell — CI-ben elérhető)
#   make bundle              — mindkettő, kimenet az out/-ba
#   make clean               — letöltött artifactok törlése
#
# A CI workflow (.github/workflows/kali-term-release.yml) ezt futtatja és a
# végeredményt artifactként feltölti, amit a kali-term-app build elvesz.

OUT_DIR ?= $(CURDIR)/out
DL_DIR  ?= $(CURDIR)/downloads

.PHONY: all bundle build-proot build-kali-rootfs clean help

all: bundle help

help:
	@echo "Targets:"
	@echo "  make build-proot         — proot ARM64 Bionic FORRÁSBÓL (NDK)"
	@echo "  make build-kali-rootfs   — Kali ARM64 rootfs debootstrap-pal"
	@echo "  make bundle              — mindkettőt ÖSSZESZEDI az out/-ba"
	@echo "  make clean               — letöltöttek + bundle törlése"

build-proot:
	@mkdir -p $(DL_DIR)
	@$(CURDIR)/scripts/build-proot.sh "$(DL_DIR)"

build-kali-rootfs:
	@mkdir -p $(DL_DIR)
	@$(CURDIR)/scripts/build-kali-rootfs.sh "$(DL_DIR)"

bundle: build-proot build-kali-rootfs
	@mkdir -p $(OUT_DIR)
	@$(CURDIR)/scripts/build-bundle.sh "$(DL_DIR)" "$(OUT_DIR)"

clean:
	rm -rf $(OUT_DIR) $(DL_DIR)
