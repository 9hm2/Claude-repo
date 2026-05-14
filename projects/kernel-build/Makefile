# Userspace Kali Terminál — LKL (Linux Kernel Library) build belépési pont.
#
# Az LKL a mainline Linux kernelt egy userspace .so library-vé fordítja.
# A `linux/drivers/` mappa moduljai (USB serial, BT, Wi-Fi, HID, LED, ...)
# ugyanúgy a forrásból fordulnak — csak az ARCH=lkl host-architektúra
# alatt, és nincs külön emulációs réteg: natív x86_64 / ARM64 utasítások
# futnak közvetlenül.
#
# Két fő target:
#   make lkl-x86_64   — host build (CI smoke teszt, fejlesztés)
#   make lkl-arm64    — Android cross build (production target)

include LKL_REVISION

BUILD_DIR ?= $(CURDIR)/build
SRC_DIR   ?= $(BUILD_DIR)/linux-lkl
OUT_DIR   ?= $(CURDIR)/out
JOBS      ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

CONFIG_FRAGMENT := $(CURDIR)/config/kaliterm_lkl.config

.PHONY: all lkl-x86_64 lkl-arm64 fetch configure-% build-% clean distclean help

all: lkl-x86_64

help:
	@echo "Targets:"
	@echo "  lkl-x86_64    LKL build az x86_64 host-ra (CI / fejlesztés)"
	@echo "  lkl-arm64     LKL build ARM64 host-ra (Android cross-compile)"
	@echo "  fetch         Csak letölti az LKL forrást"
	@echo "  clean         Build artifact-ok törlése"
	@echo "  distclean     Minden letöltött forrás + build törlése"

lkl-x86_64: configure-x86_64 build-x86_64
lkl-arm64:  configure-arm64  build-arm64

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
