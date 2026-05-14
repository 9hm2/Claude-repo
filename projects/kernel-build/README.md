# kernel-build — UML kernel build a kaliterm projektnek

Ez a komponens fordítja le a **mainline Linux kernel** forrását egy
**User Mode Linux (UML)** binárissá (`linux-um`), amit később az Android
app natívan, root nélkül futtat.

A drivereket — pl. `drivers/usb/serial/ftdi_sio.c`, `drivers/bluetooth/btusb.c`,
`drivers/net/wireless/realtek/rtl8xxxu/*`, stb. — egyszerűen
**ki-/bekapcsoljuk** a `config/kaliterm_um.config` fragmentben, és a CI
újrafordítja a kernelt a kívánt feature-készlettel.

## Verzió

`KERNEL_VERSION` fájlban van rögzítve. Jelenleg: **6.12.x LTS** (5 éves
hosszú támogatás → 2030 körül EoL).

## Helyi build (opcionális — a fő build a CI-ben fut)

```bash
cd projects/kernel-build
make um-x86_64        # x86_64 host build (gyors, fejlesztéshez)
# vagy:
make um-arm64 CROSS_COMPILE=aarch64-linux-gnu-   # ARM64 host build
```

Eredmény: `out/<subarch>/linux-um` — egy futtatható ELF, ami egy mini Linux.

> **Megjegyzés**: az Androidos build natív ARM64 cél. A CI mindkét architektúrát
> elindítja: `x86_64` smoke teszthez (gyors), `arm64` a tényleges
> deployment-hez.

## Új driver hozzáadása

1. Megkeresed a Linux mainline-ban a megfelelő `CONFIG_*` szimbólumot (pl.
   `CONFIG_USB_SERIAL_FTDI_SIO`).
2. Felveszed a `config/kaliterm_um.config`-ba egy sorként:
   ```
   CONFIG_USB_SERIAL_FTDI_SIO=y
   ```
   (vagy `=m` ha modulként akarod).
3. Commit → push → CI lebuildeli az új kernelt.
4. A kernel.yml artifact-jából letölthető a `linux-um` binary.

Lánc-függőségek (pl. `CONFIG_USB=y`) automatikusan beemelődnek a Kconfig
`select`/`depends` szabályaival, és az `olddefconfig` lépés feltölti őket.

## Felépítés

```
KERNEL_VERSION                 # melyik mainline verzió
Makefile                       # belépési pont (um-x86_64 / um-arm64)
config/
  kaliterm_um.config           # MI kapcsoljuk be a drivereket itt
scripts/
  fetch-kernel.sh              # tarball letöltés kernel.org-ról
  configure-um.sh              # defconfig + saját fragment merge
  build-um.sh                  # make + artifact kimentés
patches/                       # (későbbi: uml-hcd és egyéb saját patch-ek)
build/                         # letöltött + bontott kernel forrás (gitignore)
out/                           # kész binárisok (gitignore)
```

## Roadmap fázisok

- **Fázis 0** (most): minimális UM build x86_64-en, csak hostfs+tty.
- **Fázis 1**: initramfs + busybox boot.
- **Fázis 2**: saját `uml-hcd` USB HCD driver patch-be (a `patches/` alá).
- **Fázis 3+**: érdemi driver-bekapcsolások a `config/kaliterm_um.config`-ban.

Részletek a fő tervben: `/root/.claude/plans/a-termux-termin-l-emul-tor-nifty-parasol.md`.

## ⚠️ Architektúra-blokkoló: ARM64 host UML nincs upstream-ben

A Fázis 0 első CI futása kiderítette, hogy a mainline 6.12 LTS **nem
tartalmaz `arch/arm64/Makefile.um`-et**, tehát ARM64 host UM upstream nincs.
Részletes elemzés és opciók: [`STATUS.md`](STATUS.md).

Rövid összefoglaló: az ajánlott megoldás **pivot LKL-re** (Linux Kernel
Library). Felhasználói döntésre vár.
