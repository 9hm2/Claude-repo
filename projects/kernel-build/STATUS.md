# Build státusz és nyitott architektúra-döntések

## Fázis 0 első CI futás — eredmények

| Target | Build | Megjegyzés |
| --- | --- | --- |
| `make um-x86_64` | ✅ (futás folyamatban / sikeres) | UM x86_64 host build a stock kernel 6.12.30-ban működik |
| `make um-arm64`  | ❌ **upstream blokk** | Az ARM64 host UML **nem létezik** a mainline 6.12 LTS-ben |

### Az ARM64 hiba pontos oka

```
arch/um/Makefile:40: arch/arm64/Makefile.um: No such file or directory
make[2]: *** No rule to make target 'arch/arm64/Makefile.um'.  Stop.
```

A `arch/um/Makefile` 40. sora `include arch/$(SUBARCH)/Makefile.um`-mal próbál
beágyazni egy host-specifikus szabálykészletet. A 6.12 mainline csak `i386` és
`x86_64` változatokat hoz; **`arch/arm64/Makefile.um` nincs a fában**.

A UML ARM64 host-port patch-ek a LKML-en évek óta keringenek (pl. Karim
Manaouil sorozata 2022–2024), de **nem mergelt** a 6.12 LTS-be. Friss
mainline `next/master` ágakban sincs egységes konszenzus.

→ **A jelen build-rendszerrel ARM64 host UML-t nem tudunk készíteni**.

## Mi a tét?

A projekt célja: **valódi mainline Linux drivereket** futtatni **natívan
ARM64 Androidon, root nélkül, emuláció nélkül**. UML volt az első jelölt erre,
de upstream nincs hozzá ARM64 host. Egy döntés kell, hogyan lépünk tovább.

## Opciók

### Opció A — Pivot LKL-re *(ajánlott)*

[LKL — Linux Kernel Library](https://github.com/lkl/linux): a mainline kernelt
egy `.so` library-vé fordítja userspace API-val. **Természetszerűen
cross-arch** — ARM64 első osztályú célpont, nem külön port.

- **Előny**: stabil, karbantartott, kifejezetten erre lett tervezve;
  ugyanazok a mainline driverek (`drivers/*`), ugyanaz a build-rendszer
  (kconfig + Make); a `tools/lkl/` saját user-mode glue-t ad
- **Hátrány**: nem "full Linux processz" mint a UML — nincs PID 1, nincs
  init; Kali tools nem lehetnek "csak úgy bent". A glue rétegben kell
  syscall-routerelni: az Android-oldali `kali-term-app` Bionic, a chrooted
  Kali pedig saját LKL-példányhoz beszél. Bonyolultabb a "Kali fut benne"
  illúziója.
- **Munka**: build-rendszer nagyrészt áthúzható (Makefile / configure /
  fetch továbbra is jó), de a célpont `lkl.so` lesz, és kell egy
  `liblkl-android.so` JNI wrapper a Bionic oldalon.

### Opció B — Out-of-tree ARM64 UM patch-sorozat

Az LKML-en keringő ARM64 UM patch-eket bevesszük a `patches/` alá, és minden
build előtt rápatcheljük a kernelt.

- **Előny**: megtartjuk a UML "full Linux processz" élményt; Kali natívan
  init-elhet
- **Hátrány**: nagyon labilis — kernel verzió frissítéskor a patch-eket
  rebase-elni kell; a patch-szerző nem garantál stabilitást;
  driver-kompatibilitás bizonytalan
- **Munka**: patch-sorozat beszerzése (pl. a [arm64-um GitHub fork](https://github.com/karim-manaouil/linux/tree/uml-arm64)),
  patches/ alá rendezés, applikáló script

### Opció C — Newer kernel pin (rc / linux-next)

Reménykedjünk hogy egy újabb `rc` vagy `linux-next` snapshot már tartalmaz
ARM64 UM-et.

- **Előny**: ha bejön, gyors
- **Hátrány**: stabilitás kérdéses; ha egy `rc` éppen nem buildelhető, ott
  ülünk; LTS-pin elveszik
- **Realisztikusan**: kevéssé valószínű hogy 2026 közepére már stabil

### Opció D — Marad UML, csak x86_64 cél

A natív ARM64 célt elengedjük, és Termux:USB + emulált x86_64 UML-en
futtatunk. **A felhasználó által explicit kizárt** (emulációs overhead),
csak teljesség kedvéért listázva.

## Javaslat

**Opció A — LKL pivot.** Indokok:
1. Az egyetlen olyan út, ami **valóban natív ARM64-en** fut, **upstream
   mainline** alapokon, **patch-karbantartás nélkül**.
2. A `linux/drivers/*` kód ugyanúgy a forrásból fordul be — a felhasználó
   eredeti követelménye ("valódi mainline driverek, nem userspace
   utánjátszás") teljes mértékben megmarad.
3. Az LKL `tools/lkl/` projektje már megoldja a fő integrációs problémákat
   (syscall routing, FD átadás, USB host shim hookot is támogat).

A "Kali init benne" élményt vagy egy kis init-emulátorral (LKL fork, a glue
réteg fakeli a PID 1-et), vagy egy mellé tett `proot`-tal lehet megoldani.

## Mit kell tenni, ha LKL-re pivotalunk

1. `projects/kernel-build/` kibővítve egy `lkl` taget kap a Makefile-ben.
2. Az LKL upstream-et a fő linux.git oldalán nem találod — saját fork
   (`github.com/lkl/linux`). Tarball-szerű letöltés helyett egy `lkl` source
   tag-tarball.
3. A `kaliterm_um.config` átnevezése `kaliterm_lkl.config`-ra, és a
   `CONFIG_LKL_*` szimbólumokkal kibővítve (USB-host shim, FUSE-helyi
   hostfs, stb.).
4. Egy `liblkl-android.so` JNI wrapper, ami a `kali-term-app`-ban exponálja
   a kernel syscall felületet.
5. A Kali rootfs egy thin init-tel indul, ami az LKL syscall hívások
   megszelídítésére kompilált.

## Mit ne tegyünk

- Ne fordítsuk vissza a felhasználó kívánságát "userspace utánjátszás"-ra.
- Ne nyúljunk a teszt apphoz (`projects/android-app/`).
- Ne adjunk a CI-be ARM64 UM-et, amíg upstream nincs — eddig csak zaj.
