# kaliterm debug APK release-ek

Ez a mappa lokálisan-buildelt APK-kat tartalmaz, mert a GitHub Actions
CI-tier (minute + artifact storage) kimerült.

## Telepítés (split-chunkból)

Az APK > 100MB → GitHub repo file-limitje miatt 90MB-os darabokra van vágva.
A telepítéshez először össze kell raknod:

```bash
cat kaliterm-debug-72f326a.apk.aa kaliterm-debug-72f326a.apk.ab \
    > kaliterm-debug-72f326a.apk
adb install kaliterm-debug-72f326a.apk
```

Vagy `wget`-tel közvetlenül a telefonra (Termux-ban):

```bash
wget https://raw.githubusercontent.com/9hm2/Claude-repo/claude/setup-multi-project-repo-ogjeK/releases/kaliterm-debug-72f326a.apk.aa
wget https://raw.githubusercontent.com/9hm2/Claude-repo/claude/setup-multi-project-repo-ogjeK/releases/kaliterm-debug-72f326a.apk.ab
cat kaliterm-debug-72f326a.apk.aa kaliterm-debug-72f326a.apk.ab > kaliterm-debug.apk
```

## Build-info — kaliterm-debug-72f326a.apk (138 MB)

- Commit: `72f326a` (claude/setup-multi-project-repo-ogjeK branch)
- LKL: liblkl.so 22 MB (stripped, ARM64 Bionic)
- Kali rootfs: kalifs-arm64-minimal.tar.xz 68 MB
  - **debootstrap kali-rolling** alap
  - **usbutils + hwdata + pciutils** (lsusb, lspci)
  - **firmware-realtek** (rtw89 / rtl_bt / rtl_nic / rtlwifi / rtw88)
  - **net-tools + iproute2** (ifconfig, ip, route, ss, netstat)
  - **kmod** (modprobe, depmod, lsmod, insmod)
  - **iw + wireless-tools + wpasupplicant + rfkill** (Wi-Fi userspace)
  - **util-linux** (dmesg, mount, blkid, hwclock, ...)
  - **/lib/modules/6.12.0-kaliterm+/** depmod-generált index fájlokkal
    (modules.builtin.alias.bin 7.6KB, modules.builtin.bin 16KB) → `modprobe
    rtl8xxxu` / `btusb` / `ath9k_htc` exit=0
- shim: libkali_fuse_shim.so 67KB (LD_PRELOAD; open/openat/klogctl
  intercept LKL kernel ring-buffer → dmesg routing)
- proot: 248 KB ARM64 Bionic ELF
- Termux modules: terminal-emulator + terminal-view v0.118.1 (vendor)

## Hogyan készült

Lokális build a fejlesztői env-ben (NEM CI-ben).
Részletek: `projects/kali-term-app/scripts/` + `projects/kali-rootfs/scripts/`.
