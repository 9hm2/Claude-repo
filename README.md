# Claude-repo

Több különálló projektet tartalmazó monorepo. **Minden build a GitHub Actions
alatt fut** — lokálisan nincs futtatás.

## Projektek

| Projekt | Stack | Státusz |
| --- | --- | --- |
| [`projects/android-app`](projects/android-app) | Kotlin · Jetpack Compose · AGP 8.8 · target SDK 36 | Teszt app (`dev.hm.app`) — referencia scaffold |
| [`projects/kernel-build`](projects/kernel-build) | Linux mainline · LKL · GCC/cross-compile | Userspace Kali terminál — LKL kernel library build (Fázis 1a — USB stack engedélyezve) |
| [`projects/usb-bridge`](projects/usb-bridge) | C11 · libusb-1.0 · USB/IP wire | Host-oldali USB/IP server az LKL vhci_hcd-nek (Fázis 1b — skeleton) |

## Struktúra

```
projects/
  android-app/       # Android (Kotlin + Compose) — teszt app, referencia
  kernel-build/      # Linux mainline kernel library (LKL) build a kaliterm projekthez
  usb-bridge/        # host-oldali USB/IP server az LKL vhci_hcd-nek (C, libusb)
  <új-projekt>/      # további projektek ide jönnek
.github/workflows/   # projekt-szintű CI munkafolyamatok (path-szűrt triggerek)
```

## Build és tesztelés

Minden ellenőrzés a `.github/workflows/` alatti munkafolyamatokon fut:
push/PR esetén automatikusan, vagy `workflow_dispatch`-csel manuálisan
indítható. APK-k a futás artifact-jaiból tölthetők le.
