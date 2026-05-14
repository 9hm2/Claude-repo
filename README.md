# Claude-repo

Több különálló projektet tartalmazó monorepo. **Minden build a GitHub Actions
alatt fut** — lokálisan nincs futtatás.

## Projektek

| Projekt | Stack | Státusz |
| --- | --- | --- |
| [`projects/android-app`](projects/android-app) | Kotlin · Jetpack Compose · AGP 8.8 · target SDK 36 | Teszt app (`dev.hm.app`) — referencia scaffold |
| [`projects/kernel-build`](projects/kernel-build) | Linux mainline · UML · GCC/cross-compile | Userspace Kali terminál — UML kernel build (Fázis 0) |

## Struktúra

```
projects/
  android-app/       # Android (Kotlin + Compose) — teszt app, referencia
  kernel-build/      # Linux UML kernel build a kaliterm projekthez
  <új-projekt>/      # további projektek ide jönnek
.github/workflows/   # projekt-szintű CI munkafolyamatok (path-szűrt triggerek)
```

## Build és tesztelés

Minden ellenőrzés a `.github/workflows/` alatti munkafolyamatokon fut:
push/PR esetén automatikusan, vagy `workflow_dispatch`-csel manuálisan
indítható. APK-k a futás artifact-jaiból tölthetők le.
