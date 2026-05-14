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

**Phase 2a — skeleton (jelen állapot):**
- Gradle/Compose projektszerkezet, `dev.hm.kaliterm`
- CMake-alapú natív build (`libkaliterm_native.so`)
- Egyszerű JNI stub (`nativeHello`, `nativeVersion`) — bizonyítja hogy a
  Kotlin↔C híd megvan az ARM64 ELF build-del
- Compose home-screen: app név + natív string + build verzió
- CI workflow APK artifact-tal

**Phase 2b (következő):**
- UsbManager device discovery + permission UI
- A `projects/usb-bridge` C forrásait beépíteni a `libkaliterm_native.so`-ba
- libusb prebuilt Android-ra (vagy build CMake-ből)
- JNI metódus: `attachUsbDevice(fd, busid)` — UsbManager fd-t lemásol és
  továbbít a bridge dispatch loop-jának

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
