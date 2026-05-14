# Build státusz és architektúra-döntés

## Pivot: UML → LKL (elvégezve)

Az első Fázis 0 build kiderítette, hogy a mainline Linux 6.12 LTS-ben
**nincs ARM64 host UML támogatás** — az `arch/arm64/Makefile.um` fájl nem
létezik, és az UML ARM64 patch-ek nem mergeltek upstream-be.

A felhasználói követelmény: **valódi mainline driverek**, **natív ARM64
Androidon**, **root és emuláció nélkül**. A UML útat ez kizárta.

Pivotáltunk az **LKL (Linux Kernel Library)**-re:
[github.com/lkl/linux](https://github.com/lkl/linux). Az LKL ugyanazt a
mainline kernel forrást használja, de a `arch/lkl/` host-architektúra
cross-arch első osztály — ARM64 ugyanúgy célozható mint x86_64, csak egy
toolchain váltás kell.

### A pivot lényegi pontjai

| Szempont | Megmarad? |
| --- | --- |
| Valódi `linux/drivers/*` kód a forrásból fordul | ✅ igen |
| Natív utasítás-végrehajtás (no QEMU/TCG) | ✅ igen |
| ARM64 Android cél | ✅ igen, első osztály |
| Reprodukálható build CI-ben | ✅ igen |
| Mainline upstream alap | ✅ igen (LKL fork-ja a mainline-nak) |
| "Full Linux processz" (PID 1 init, fork, stb.) | ⚠️ glue-réteg kell |

A "Kali init benne" élményt egy thin glue-rétegben oldjuk meg — a kernel
library exponálja a syscall felületet, és egy könnyű init-emulátor vagy
PRoot-szerű wrapper biztosítja a "userspace Kali" érzést. Erről részlet
a fő tervben.

## Korábbi blokkoló (lezárva)

Az UM ARM64 hiba pontos manifesztációja:

```
arch/um/Makefile:40: arch/arm64/Makefile.um: No such file or directory
make[2]: *** No rule to make target 'arch/arm64/Makefile.um'.  Stop.
```

Ez **upstream tény**, nem build-bug. A pivot megkerüli teljesen.

## Mostantól

A Fázis 0 új célja: az LKL library lefordul **mindkét host architektúrára**
(x86_64 + arm64 cross), és az artifact-okat (`liblkl-host-lib.so` + tools)
CI feltölti.

**Fázis 0 lezárva**: ✅ mindkét architektúra zöldül a CI-ban, az artifact-ok
elérhetők. (`liblkl-host-lib.so` + `lklfuse`, `cptofs`, `cpfromfs`.)

## Fázis 1a — USB stack bekapcsolva (✅)

**Megközelítés**: nem írunk saját HCD driver-t. Helyette bekapcsoljuk a már
létező mainline **`vhci_hcd` (USB/IP)** driver-t, ami pontosan arra való,
hogy egy socket-en jövő URB-eket lekezeljen.

Az LKL kernelbe bekerülő driver-készlet ezzel a commit-tal:

| Modul | Mire jó |
| --- | --- |
| `drivers/usb/core/*` | USB core, hub, URB dispatch |
| `drivers/usb/usbip/vhci_hcd.c` | Virtuális USB HCD — socket-FD-ről URB |
| `drivers/usb/serial/{ftdi_sio,ch341,cp210x,pl2303}.c` | USB serial smoke test |
| `drivers/hid/usbhid/*`, `drivers/hid/hid-generic.c` | USB HID smoke test |

A host oldal (`projects/usb-bridge/`) **Fázis 1b + 1c** alatt kész:
- Unix socket szerver, SCM_RIGHTS-szal érkező USB fd, `libusb_wrap_sys_device`.
- Teljes USB/IP wire loop: CMD_SUBMIT/CMD_UNLINK pdu-kat libusb async
  control/bulk/interrupt transfer-ekké fordítunk; completion-ök RET_SUBMIT
  pdu-kban mennek vissza a `vhci_hcd`-nek. Részletek:
  `projects/usb-bridge/README.md`.

## Phase 2c.2 — Android NDK / Bionic LKL build

A `kernel.yml`-ben jelenleg három build job fut párhuzamosan:

| Job | Toolchain | Artifact | Cél |
| --- | --- | --- | --- |
| `LKL build (x86_64 host)` | host gcc | `lkl-x86_64` | Sanity / fejlesztés |
| `LKL build (arm64 cross)` | `aarch64-linux-gnu-gcc` (glibc) | `lkl-arm64` | Referencia, Linux ARM64 |
| **`LKL build (android arm64, Bionic)`** | **`aarch64-linux-android24-clang` (Bionic)** | **`lkl-android-arm64`** | **kali-term-app jniLibs** |

A harmadik job az NDK 27.0.12077973-at telepíti `sdkmanager`-rel, majd
`make lkl-android-arm64`-t futtat — a `scripts/build-lkl.sh` az NDK
clangot + LLVM tools-t (`llvm-ar`, `ld.lld`, `llvm-strip`, `llvm-nm`,
`llvm-objcopy`) használ a kbuild minden szintjén. Eredmény:
`out/android-arm64/liblkl-host-lib.so` — Bionic ABI, ARM64 ELF,
dinamikusan az Android `libc.so`-ra hivatkozik.

A `kali-term-app` Phase 2c.1-ben **API-szinten előkészítve** van:
- `nativeLklStatus / nativeLklStart / nativeLklStop` JNI metódusok
- `dlsym(RTLD_DEFAULT, ...)` alapú symbol resolution — graceful degrade
- UI mutatja: `LKL — UNAVAILABLE / LOADED / RUNNING`.

A Phase 2c.3 (következő, kali-term-app oldal) a `lkl-android-arm64`
artifact-ot pull-olja le a kernel.yml legutóbbi sikeres futásából,
és kimásolja `app/src/main/jniLibs/arm64-v8a/liblkl-host-lib.so`-ba —
ezzel az APK install-után az LKL panel automatikusan `LOADED`-re vált.

## Fázis 2 (következő) — end-to-end összekötés

A `vhci_hcd` és a `kaliterm-usb-bridge` most még külön él. A következő
lépés a két socket-vég összekötése egy első Android NDK build keretében:
1. `kali-term-app` JNI-ben elindítjuk az `liblkl-host-lib.so`-t,
   és `vhci_hcd`-t a `usbip_sockfd_store` sysfs attribútumon keresztül
   attach-oljuk az általunk megnyitott socket-pair egyik végéhez.
2. A másik végét odaadjuk a `kaliterm-usb-bridge`-nek (vagy beágyazva
   ugyanabba a processzbe egy thread-en, vagy spawn-olt daemon-ként).
3. Az UsbManager-től kapott USB fd-t SCM_RIGHTS-szal átadjuk a bridge-nek.
4. Mostantól a Kali shell-ből `lsusb` látja az eszközt, és pl. a `ftdi_sio`
   driver bind-eli, létrejön `/dev/ttyUSB0`.
