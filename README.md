# NeoTerm proot rendszer

Ez a repó tartalmazza a **proot alapú futtatókörnyezet** építő-/kiszolgáló
oldalát, amelyre a [NeoTerm](https://github.com/NeoTerm/NeoTerm) terminál-
emulátor proot-átállása épül. A NeoTerm kliens-oldali integrációja a
`NeoTerm-pr` repó `claude/neoterm-proot-migration-uS0o8` ágán található.

## Miért proot?

A NeoTerm eddig a **Termux-stílusú natív bootstrap** modellt használta:

- a csomagok egyedi `PREFIX`-szel (`/data/data/io.neoterm/files/usr`) fordítva,
- shebangek és `/bin/...` útvonalak futásidőben átírva egy `LD_PRELOAD`
  `execve`-wrapperrel (`libnexec.so`, `app/src/main/cpp/exec.c`),
- minden bináris az app UID-jával, namespace/chroot nélkül fut.

Ennek korlátai: csak speciálisan újrafordított csomagok futnak, a standard
disztró-binárisok nem; a hardcode-olt `/usr`, `/etc`, `/bin` útvonalak törnek.

A **proot** (ptrace alapú, userspace `chroot` + bind-mount emuláció, root
jogosultság nélkül) egy **valódi Linux-disztró rootfs**-t futtat, ahol `/`,
`/usr`, `/etc`, `/bin` a megszokott helyükön vannak. Így:

- nem kell shebang-átírás (`libnexec` proot módban kikapcsol),
- nem kellenek `PREFIX`-patch-elt csomagok — standard `apt`/`apk`/`pacman`,
- futnak a normál Ubuntu/Alpine/Kali/Arch binárisok.

## Komponensek

| Fájl | Szerep |
| --- | --- |
| `proot/build-proot.sh` | A [termux/proot](https://github.com/termux/proot) fork cross-fordítása Android NDK-val (Bionic, aarch64). Statikus ELF-et és beágyazott loadert épít. Kimenet: `<dl>/proot`. |
| `proot/fetch-rootfs.sh` | Az upstream disztró-rootfs-ek (Ubuntu/Alpine/Kali/Arch) letöltése és egységes `.tar.xz`-be csomagolása + sha256. |
| `proot/distros.json` | A támogatott disztrók manifesztje (id, alapértelmezett shell, csomagkezelő, arch-leképezés). A kliens `Distro.kt` ezt tükrözi. |

## Miért a Termux fork?

Az upstream `proot-me/proot` az Android-kernel seccomp-szűrőjén `SIGSYS`-szal
(signal 31) öli a chroot-olt processzeket, mert a kernel blokkolja a
ptrace-syscallt és az upstream nem kerüli meg. A `termux/proot` fork
tartalmazza az ehhez szükséges kernel-hook patcheket
(`syscall/seccomp.c` — `SECCOMP_MODE_FILTER` fallback + Android-kompat).

## Build és publikálás

```bash
# 1) proot bináris (NDK kell)
export ANDROID_NDK_HOME=/opt/android-ndk
./proot/build-proot.sh ./out/proot/aarch64       # → ./out/proot/aarch64/proot

# 2) rootfs-ek
./proot/fetch-rootfs.sh ./out ubuntu alpine kali arch --arch aarch64
```

A `./out` fát ezután bármilyen statikus HTTP-hostra fel lehet tölteni
(GitHub Releases, S3, raw.githubusercontent). A NeoTerm a setupkor ebből a
base-URL-ből tölt, a következő elrendezést várva:

```
<BASE_URL>/proot/<arch>/proot
<BASE_URL>/rootfs/<distro>/<arch>.tar.xz
<BASE_URL>/rootfs/<distro>/<arch>.tar.xz.sha256
```

ahol `<arch> ∈ {aarch64, arm, x86_64}` (a NeoTerm
`SetupHelper.determineArchName()` kimenete), `<distro> ∈ {ubuntu, alpine,
kali, arch}`. A base-URL a kliensben a `NeoTermPath.DEFAULT_PROOT_SOURCE`.

## Kliensoldali futtatás (NeoTerm-pr)

A NeoTerm a letöltött proot binárissal így indítja a bejelentkező shellt
(`ProotManager.buildLaunch`):

```
proot --kill-on-exit --link2symlink -0 -r <rootfs>           \
      -b /dev -b /proc -b /sys -b /dev/pts                   \
      -b /proc/self/fd:/dev/fd                                \
      -b /proc/self/fd/0:/dev/stdin                           \
      -b /proc/self/fd/1:/dev/stdout                          \
      -b /proc/self/fd/2:/dev/stderr                          \
      -b /dev/urandom:/dev/random                             \
      -w /root                                                \
      /usr/bin/env -i HOME=/root TERM=... LANG=C.UTF-8 ...    \
      /bin/bash --login
```

A proot maga az app UID-jával fut (`PROOT_TMP_DIR` egy írható app-könyvtárra
mutat); a beágyazott loader miatt nincs külső `PROOT_LOADER` fájl.
