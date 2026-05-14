# Userspace Kali Terminál — UML kernel build belépési pont.
#
# Cél: a `linux/drivers/` mainline driverek lefordítása egy User Mode Linux
# binárisba (`linux-um.elf`), ami natívan, root nélkül fut Android-on.
#
# Két fő target:
#   make um-x86_64   — gyorsabb fejlesztői build, CI smoke teszthez
#   make um-arm64    — production target Android-ra (NDK cross)

include KERNEL_VERSION

BUILD_DIR ?= $(CURDIR)/build
SRC_DIR   ?= $(BUILD_DIR)/linux-$(KERNEL_VERSION)
OUT_DIR   ?= $(CURDIR)/out
JOBS      ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

CONFIG_FRAGMENT := $(CURDIR)/config/kaliterm_um.config

.PHONY: all um-x86_64 um-arm64 fetch configure-% build-% clean distclean help

all: um-x86_64

help:
	@echo "Targets:"
	@echo "  um-x86_64     UM build az x86_64 host-ra (CI / fejlesztés)"
	@echo "  um-arm64      UM build ARM64 host-ra (Android cross-compile)"
	@echo "  fetch         Csak letölti és kibontja a kernel forrást"
	@echo "  clean         Build artifact-ok törlése"
	@echo "  distclean     Minden letöltött forrás + build törlése"

um-x86_64: configure-x86_64 build-x86_64
um-arm64:  configure-arm64  build-arm64

fetch: $(SRC_DIR)/Makefile

$(SRC_DIR)/Makefile:
	@$(CURDIR)/scripts/fetch-kernel.sh "$(KERNEL_VERSION)" "$(BUILD_DIR)"

configure-%: $(SRC_DIR)/Makefile
	@$(CURDIR)/scripts/configure-um.sh "$(SRC_DIR)" "$*" "$(CONFIG_FRAGMENT)"

build-%: configure-%
	@$(CURDIR)/scripts/build-um.sh "$(SRC_DIR)" "$*" "$(OUT_DIR)" "$(JOBS)"

clean:
	rm -rf "$(SRC_DIR)/.config" "$(SRC_DIR)/include/config" "$(OUT_DIR)"
	$(MAKE) -C "$(SRC_DIR)" ARCH=um clean 2>/dev/null || true

distclean:
	rm -rf "$(BUILD_DIR)" "$(OUT_DIR)"
