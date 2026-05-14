# kaliterm-usb-bridge — host-oldali USB/IP server, ami a vhci_hcd-nek
# továbbítja az Android UsbManager-től kapott USB eszközöket libusb-vel.
#
# Build:
#   make              # natív host (x86_64 / arm64 — a system CC dönt)
#   make verbose=1    # részletes parancsok
#
# Az Android (NDK) cross-compile-t a kali-term-app oldaláról fogjuk hívni
# majd egy külön külső.mk fragmentben.

CC      ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS  ?= -O2 -g
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-unused-parameter
STD      = -std=c11
DEFINES  =

LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell $(PKG_CONFIG) --libs   libusb-1.0)

ALL_CFLAGS  = $(STD) $(WARNINGS) $(DEFINES) $(CFLAGS) $(LIBUSB_CFLAGS) -pthread
ALL_LDFLAGS = $(LDFLAGS) -pthread
ALL_LDLIBS  = $(LIBUSB_LIBS) $(LDLIBS)

OUT_DIR ?= out
BIN := $(OUT_DIR)/kaliterm-usb-bridge

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=$(OUT_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

ifeq ($(verbose),1)
  Q :=
else
  Q := @
endif

.PHONY: all clean help

all: $(BIN)

help:
	@echo "Targets:"
	@echo "  all     bridge build (alapértelmezett)"
	@echo "  clean   build artifact-ok törlése"
	@echo ""
	@echo "Variables:"
	@echo "  CC=...        toolchain (default: cc)"
	@echo "  CFLAGS=...    extra compile flag-ek"
	@echo "  verbose=1     parancsok megjelenítése"

$(BIN): $(OBJS)
	@mkdir -p $(@D)
	@echo "  LD    $@"
	$(Q)$(CC) $(OBJS) $(ALL_LDFLAGS) $(ALL_LDLIBS) -o $@

$(OUT_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	@echo "  CC    $<"
	$(Q)$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT_DIR)

-include $(DEPS)
