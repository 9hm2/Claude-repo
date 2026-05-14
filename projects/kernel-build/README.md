# kernel-build — LKL kernel build a kaliterm projektnek

Ez a komponens fordítja le a **mainline Linux kernel** forrását (LKL fork-on
keresztül) egy **userspace library-vé** (`liblkl-host-lib.so`), amit
később az Android app natívan, root nélkül használ — sem QEMU, sem TCG
emuláció, csak ARM64 / x86_64 utasítások közvetlen futtatása.

A drivereket — pl. `drivers/usb/serial/ftdi_sio.c`, `drivers/bluetooth/btusb.c`,
`drivers/net/wireless/realtek/rtl8xxxu/*`, stb. — egyszerűen
**ki-/bekapcsoljuk** a `config/kaliterm_lkl.config` fragmentben, és a CI
újrafordítja a library-t a kívánt feature-készlettel.

## LKL — Linux Kernel Library

Az [LKL](https://github.com/lkl/linux) a mainline Linux egy fork-ja, ami
egy `arch/lkl/` host-architektúrát ad hozzá. A `tools/lkl/` mappa adja a
userspace build-rendszert és a glue kódot, ami a kernel library-t
felhasználható API-val burkolja.

**Miért LKL és nem UML?** Az UML mainline-ban nem támogat ARM64 host-ot
(részletek: [`STATUS.md`](STATUS.md)). Az LKL ezzel szemben cross-arch
első osztály — ARM64 ugyanúgy működik mint x86_64, mert csak egy
hoszt-toolchain váltás kell.

## Verzió

Az `LKL_REVISION` fájlban van rögzítve. Jelenleg: az LKL `master` ágának
tip-je (Fázis 0 — bootstrap). Amint ismerjük egy működő kombináció commit
hash-ét, `LKL_REVISION_COMMIT`-be írjuk be a pin-t.

## Helyi build (opcionális — a fő build a CI-ben fut)

```bash
cd projects/kernel-build
make lkl-x86_64        # x86_64 host build (gyors, fejlesztéshez)
# vagy:
make lkl-arm64 CROSS_COMPILE=aarch64-linux-gnu-   # ARM64 host build
```

Eredmény: `out/<subarch>/liblkl-host-lib.so` — a host-side LKL library,
plus a példa-toolok (`cptofs`, `cpfromfs`, `lklfuse`, ...).

> A CI mindkét architektúrát futtatja: `x86_64` smoke teszthez,
> `arm64` a tényleges Android deployment-hez.

## Új driver hozzáadása

1. Megkeresed a Linux mainline-ban a megfelelő `CONFIG_*` szimbólumot
   (pl. `CONFIG_USB_SERIAL_FTDI_SIO`).
2. Felveszed a `config/kaliterm_lkl.config`-ba egy sorként:
   ```
   CONFIG_USB_SERIAL_FTDI_SIO=y
   ```
   (vagy `=m` ha modulként akarod).
3. Commit → push → CI lebuildeli az új library-t.
4. Az artifact-ból letölthető az új `liblkl-host-lib.so`.

Lánc-függőségek (pl. `CONFIG_USB=y`) automatikusan beemelődnek a Kconfig
`select`/`depends` szabályaival, és az `olddefconfig` lépés feltölti őket.

## Felépítés

```
LKL_REVISION                   # melyik LKL commit/branch
Makefile                       # belépési pont (lkl-x86_64 / lkl-arm64)
config/
  kaliterm_lkl.config          # MI kapcsoljuk be a drivereket itt
scripts/
  fetch-lkl.sh                 # git clone github.com/lkl/linux
  configure-lkl.sh             # defconfig + saját fragment merge
  build-lkl.sh                 # make -C tools/lkl + artifact kimentés
patches/                       # (későbbi: saját kernel patch-ek)
build/                         # kibontott LKL forrás (gitignore)
out/                           # kész binárisok (gitignore)
```

## Roadmap fázisok

- **Fázis 0** (most): minimális LKL build x86_64 + arm64 host-ra.
- **Fázis 1**: USB host shim a `tools/lkl/`-ben — Android UsbManager FD
  beadása, hogy a kernel USB stack lássa az eszközöket.
- **Fázis 2**: érdemi driver-bekapcsolások a `config/kaliterm_lkl.config`-ban
  (USB serial, HID, BT, Wi-Fi).
- **Fázis 3+**: Kali rootfs az LKL library-be hookolva (a részleteket lásd
  a fő tervben).

Részletek: `/root/.claude/plans/a-termux-termin-l-emul-tor-nifty-parasol.md`.
