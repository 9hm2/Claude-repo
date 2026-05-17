# kali-term-app

A **userspace Kali terminál** Android app — `dev.hm.kaliterm`.

Ez kapcsolja össze a többi komponenst:

- **`projects/kernel-build`** → `liblkl-host-lib.so` (a mainline Linux kernel
  userspace library formában, drivere-kkel együtt — vhci_hcd, ftdi_sio,
  usbhid, btusb, stb.)
- **`projects/usb-bridge`** → `kaliterm-usb-bridge` (USB/IP wire server,
  libusb-vel csomagolja az Android UsbManager FD-jét)
- **`projects/rootfs-build`** *(még jön)* → `kali-rootfs.img` (ext4
  image-be csomagolt Kali ARM64 minimális rootfs)

> Csak GitHub Actions-ben buildelünk — a `.github/workflows/kali-term-app.yml`
> minden push/PR-re lefuttatja az APK build-et, lint-et, unit teszteket.

## Stack

- Kotlin 2.0.21 + Jetpack Compose (BOM 2024.11.00), Material 3
- AGP 8.8.0, Gradle 8.10.2 wrapper, JDK 17
- minSdk 24 · targetSdk/compileSdk 36
- NDK 27.0.12077973, CMake 3.22.1
- ABI: `arm64-v8a` (produkciós cél)

## Fejlesztési állapot

**Phase 2a — skeleton (kész):**
- Gradle/Compose projektszerkezet, `dev.hm.kaliterm`
- CMake-alapú natív build (`libkaliterm_native.so`)
- Egyszerű JNI stub (`nativeHello`, `nativeVersion`) — bizonyítja hogy a
  Kotlin↔C híd megvan az ARM64 ELF build-del
- Compose home-screen: app név + natív string + build verzió
- CI workflow APK artifact-tal

**Phase 2b.1 — UsbManager UI + FD átadás JNI-n (kész):**
- `UsbController` osztály: device discovery (`UsbManager.deviceList`),
  per-eszköz permission (`PendingIntent` + `BroadcastReceiver`),
  `openDevice()` után az `fileDescriptor`-t átadja a natív rétegnek.
- Compose UI: USB eszközök listája, per-card `Engedély` / `Attach` gomb,
  utolsó attach-log szöveg.
- `nativeAcceptUsbDevice(fd, vid, pid, ...)` JNI metódus: `fstat`-tal
  ellenőrzi hogy érvényes karaktereszköz-fd, `__android_log_print`-tel
  logol, majd egyelőre `close`-olja.
- Manifest: `android.hardware.usb.host` deklarálva (`required=false`).

**Phase 2b.2 — libusb 1.0.27 beépítve, descriptor probe (jelen állapot):**
- libusb forrást FetchContent-tel húzzuk be a CMake build-be (1.0.27
  release tarball, SHA256-pin); statikus library-be építve.
- `cmake/libusb-config.h.in` — saját Android NDK config.h, autoconf
  helyett (`HAVE_EVENTFD`, `HAVE_TIMERFD`, `POLL_NFDS_TYPE`, stb.).
- `nativeAcceptUsbDevice` mostantól:
  1. `dup`-olja az fd-t (libusb_close ne zárja le a Kotlin oldal eredetijét),
  2. `libusb_set_option(NO_DEVICE_DISCOVERY)` — root nélkül nem szabad
     a `/dev/bus/usb`-t scannelni,
  3. `libusb_init` + `libusb_wrap_sys_device(dup_fd)`,
  4. `libusb_get_device_descriptor` + `libusb_get_active_config_descriptor`
     → logba kerül VID/PID/class/protocol/bcdUSB/numConfigs +
     interface-enkénti class/protocol/endpoints,
  5. cleanup: `libusb_close` + `libusb_exit`.
- Eredmény: bizonyítva, hogy a libusb az **Android usbfs fd-jén
  keresztül**, **root nélkül** képes a teljes USB descriptor-hierarchiát
  lekérdezni. Innentől már bármilyen URB submission megoldható
  (control / bulk / interrupt) — pontosan amit a usb-bridge Fázis 1c-ben
  is csinál.

**Phase 2b.3 (következő):**
- A `projects/usb-bridge` C forrásait beépíteni a `libkaliterm_native.so`-ba
  (CMakeLists `add_subdirectory` vagy fájl-szintű inkluzió).
- `nativeAcceptUsbDevice` mostantól nem csak descriptor-t probol, hanem
  egy worker-szálban elindítja a teljes dispatch-loop-ot az fd-re.
- Compose UI: per-eszköz live state (RX/TX bájtok, hibák).

**Phase 2c:**
- LKL `liblkl-host-lib.so` prebuilt copy a `kernel-build` artifact-ból
- `dlopen` az appban, `lkl_start_kernel`
- `socketpair()` → egyik vég a `vhci_hcd usbip_sockfd_store`-jára íródik
  (sysfs), másik vég a bridge dispatch loop-jában
- Első `lsusb` az LKL belsejéből

**Phase 2d:**
- Kali rootfs disk-image mount LKL-be (`lkl_disk_add`, `lkl_mount_dev`)
- `lklfuse` Android oldalon az ext4 mappa kitettéséhez
- PRoot chroot a Kali rootfs-be, terminál bekötése PTY-vel a Compose UI-hoz

## Felépítés

```
app/
  CMakeLists.txt                            # natív build entry
  src/main/
    AndroidManifest.xml
    java/dev/hm/kaliterm/
      MainActivity.kt                       # Compose UI
      NativeBridge.kt                       # JNI külső függvények
      ui/theme/                             # Compose téma
    cpp/
      kaliterm_jni.c                        # JNI implementáció
    res/                                    # drawables, strings, themes
  build.gradle.kts                          # app modul
build.gradle.kts                            # root
settings.gradle.kts
gradle/libs.versions.toml                   # version catalog
```

## Új JNI metódus felvétele

1. `cpp/kaliterm_jni.c` — új `Java_dev_hm_kaliterm_NativeBridge_<név>` függvény.
2. `NativeBridge.kt` — `external fun <név>(...)` deklaráció.
3. Commit → push → CI lefordít, ABI-bontásra `arm64-v8a` az artifact.
