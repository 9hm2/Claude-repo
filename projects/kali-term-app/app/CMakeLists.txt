# kaliterm native build.
#
# A `libkaliterm_native.so` JNI library a Kotlin oldalról hívható natív
# rétegünk. Tartalmazza:
#   - JNI belépési pontokat (kaliterm_jni.c)
#   - libusb 1.0.27-et statikusan beépítve (FetchContent-tel letöltve)
#   - Phase 2b.3-ban: a projects/usb-bridge/ dispatch loop-ját

cmake_minimum_required(VERSION 3.22.1)
project(kaliterm_native LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── libusb 1.0.27 ─────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    libusb_src
    URL      https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
    URL_HASH SHA256=ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575
)
FetchContent_MakeAvailable(libusb_src)

set(LIBUSB_DIR         "${libusb_src_SOURCE_DIR}/libusb")
# Az upstream libusb karbantartja a saját Android config.h-ját — közvetlen
# használjuk, nem írunk sajátot. Tartalmazza pl. PRINTF_FORMAT, ENABLE_LOGGING,
# HAVE_PIPE2, HAVE_CLOCK_GETTIME stb. — mind upstream-validált Android-ra.
set(LIBUSB_ANDROID_DIR "${libusb_src_SOURCE_DIR}/android")

# libusb-nek nincs upstream CMake build-je v1.0.27-ben — magunknak
# rakjuk össze a forrásfájlokat egy statikus library-be. A lista pontos
# tükre az `android/jni/libusb.mk`-nak (ndk-build script a libusb tree-ben).
add_library(usb1_static STATIC
    "${LIBUSB_DIR}/core.c"
    "${LIBUSB_DIR}/descriptor.c"
    "${LIBUSB_DIR}/hotplug.c"
    "${LIBUSB_DIR}/io.c"
    "${LIBUSB_DIR}/strerror.c"
    "${LIBUSB_DIR}/sync.c"
    "${LIBUSB_DIR}/os/events_posix.c"
    "${LIBUSB_DIR}/os/threads_posix.c"
    "${LIBUSB_DIR}/os/linux_usbfs.c"
    "${LIBUSB_DIR}/os/linux_netlink.c"
)
target_include_directories(usb1_static
    PUBLIC  "${LIBUSB_DIR}"
    PRIVATE "${LIBUSB_DIR}/os"
    PRIVATE "${LIBUSB_ANDROID_DIR}"  # upstream config.h-t innen veszi
)
target_compile_definitions(usb1_static PRIVATE
    HAVE_CONFIG_H
)
# libusb-belső warningokat elnyeljük — nem akarjuk upstream forrásban
# pace-rendszerelő javítgatni.
target_compile_options(usb1_static PRIVATE
    -Wno-pointer-arith
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-pedantic
    -fPIC
)
set_property(TARGET usb1_static PROPERTY POSITION_INDEPENDENT_CODE ON)

# ── libkaliterm_native.so ─────────────────────────────────────────────
#
# Tartalmazza a JNI rétegünket + a `projects/usb-bridge/` URB-dispatch
# kódját. Az utóbbi források egy ARM64 ELF-be kerülnek be — pontosan az,
# ami a vhci_hcd-vel kommunikálva URB-eket lebonyolít majd.

set(USB_BRIDGE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../usb-bridge/src")

add_library(kaliterm_native SHARED
    src/main/cpp/kaliterm_jni.c
    src/main/cpp/lkl_runtime.c
    # usb-bridge — embedded:
    ${USB_BRIDGE_SRC}/usb_dispatch.c
    ${USB_BRIDGE_SRC}/usbip_proto.c
)

target_include_directories(kaliterm_native PRIVATE
    ${USB_BRIDGE_SRC}
)

find_library(log-lib log)

target_link_libraries(kaliterm_native
    ${log-lib}
    usb1_static
)

target_compile_options(kaliterm_native PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)
