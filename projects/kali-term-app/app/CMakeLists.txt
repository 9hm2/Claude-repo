# kaliterm native rész — JNI bridge a Kotlin oldal és a host-oldali
# komponensek között.
#
# Phase 2a: csak stub — egy "hello"-szerű JNI függvény, ami bizonyítja
# hogy az NDK toolchain és a Kotlin↔C híd működik.
#
# Phase 2b+: idekapcsoljuk a projects/usb-bridge/ forrásait (libusb-vel),
# és az LKL .so-t prebuilt formában dlopen-eljük.

cmake_minimum_required(VERSION 3.22.1)
project(kaliterm_native LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(kaliterm_native SHARED
    src/main/cpp/kaliterm_jni.c
)

# Az Android NDK biztosítja a -llog binárisokat.
find_library(log-lib log)

target_link_libraries(kaliterm_native
    ${log-lib}
)

target_compile_options(kaliterm_native PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)
