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

A következő fázisban (Fázis 1) jön az **USB host shim** — a `tools/lkl/`
extensible host-interfészen keresztül az Android UsbManager FD-t bedrótozzuk
a kernel USB stack-jébe, hogy a Linux USB drivere lássa a fizikai eszközt.
