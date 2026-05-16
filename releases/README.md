# kaliterm debug APK release-ek

Ez a mappa lokálisan-buildelt APK-kat tartalmaz, mert a GitHub Actions
CI-tier (minute + artifact storage) kimerült.

## Telepítés

Letöltöd a kívánt `.apk`-t a fenti listából (a fájlnévben a commit SHA
azonosítja a verziót), majd telepíted:

```bash
adb install kaliterm-debug-f8de9a7.apk
```

Vagy közvetlenül a telefonon: `Files / Letöltések` → koppints az APK-ra
→ engedélyezd az ismeretlen forrásból telepítést → Telepít.

## Build-info

- Commit: `f8de9a7` (claude/setup-multi-project-repo-ogjeK branch)
- LKL: liblkl.so 22 MB (stripped, ARM64 Bionic)
- Kali rootfs: kalifs-arm64-minimal.tar.xz 23 MB (debootstrap kali-rolling)
- proot: 180 KB ARM64 Bionic ELF (forrásból, jniLibs/libproot.so)
- Termux modules: terminal-emulator + terminal-view v0.118.1 (vendor)
- APK total: 47 MB (debug, aláírva a kaliterm-debug.keystore-ral)

## Hogyan készült

Lokális build a fejlesztői env-ben (NEM CI-ben).
Részletek: `projects/kali-term-app/scripts/` + workflow YAML.

