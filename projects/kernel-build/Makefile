# Userspace Kali Terminál — LKL (Linux Kernel Library) build belépési pont.
#
# Az LKL a mainline Linux kernelt egy userspace .so library-vé fordítja.
# A `linux/drivers/` mappa moduljai (USB serial, BT, Wi-Fi, HID, LED, ...)
# ugyanúgy a forrásból fordulnak — csak az ARCH=lkl host-architektúra
# alatt, és nincs külön emulációs réteg: natív x86_64 / ARM64 utasítások
# futnak közvetlenül.
#
# Build target-ek:
#   make lkl-x86_64        — host smoke teszt (Ubuntu glibc)
#   make lkl-arm64         — Linux ARM64 glibc (referencia/CI sanity)
#   make lkl-android-arm64 — **Android NDK / Bionic ARM64** — ez kell a
#                            kali-term-app jniLibs alá. ANDROID_NDK_HOME
#                            kötelező env változó.

include LKL_REVISION

BUILD_DIR ?= $(CURDIR)/build
SRC_DIR   ?= $(BUILD_DIR)/linux-lkl
OUT_DIR   ?= $(CURDIR)/out
JOBS      ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

CONFIG_FRAGMENT := $(CURDIR)/config/kaliterm_lkl.config

.PHONY: all lkl-x86_64 lkl-arm64 lkl-android-arm64 fetch configure-% build-% clean distclean help

all: lkl-x86_64

help:
	@echo "Targets:"
	@echo "  lkl-x86_64         LKL build az x86_64 host-ra (CI / fejlesztés)"
	@echo "  lkl-arm64          LKL build Linux ARM64-re (glibc, referencia)"
	@echo "  lkl-android-arm64  LKL build Android NDK / Bionic ARM64-re"
	@echo "                     (ANDROID_NDK_HOME env változó kötelező)"
	@echo "  fetch              Csak letölti az LKL forrást"
	@echo "  clean              Build artifact-ok törlése"
	@echo "  distclean          Minden letöltött forrás + build törlése"

lkl-x86_64:        configure-x86_64        build-x86_64
lkl-arm64:         configure-arm64         build-arm64
lkl-android-arm64: configure-android-arm64 build-android-arm64

fetch: $(SRC_DIR)/Makefile

$(SRC_DIR)/Makefile:
	@$(CURDIR)/scripts/fetch-lkl.sh "$(LKL_REPO)" "$(LKL_BRANCH)" "$(LKL_REVISION_COMMIT)" "$(BUILD_DIR)"

configure-%: $(SRC_DIR)/Makefile
	@$(CURDIR)/scripts/configure-lkl.sh "$(SRC_DIR)" "$*" "$(CONFIG_FRAGMENT)"

build-%: configure-%
	@$(CURDIR)/scripts/build-lkl.sh "$(SRC_DIR)" "$*" "$(OUT_DIR)" "$(JOBS)"

clean:
	rm -rf "$(SRC_DIR)/tools/lkl/lib" "$(SRC_DIR)/.config" "$(OUT_DIR)"
	$(MAKE) -C "$(SRC_DIR)/tools/lkl" clean 2>/dev/null || true

distclean:
	rm -rf "$(BUILD_DIR)" "$(OUT_DIR)"
