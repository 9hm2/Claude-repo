# Claude-repo

Több különálló projektet tartalmazó monorepo. **Minden build a GitHub Actions
alatt fut** — lokálisan nincs futtatás.

## Projektek

| Projekt | Stack | Státusz |
| --- | --- | --- |
| [`projects/android-app`](projects/android-app) | Kotlin · Jetpack Compose · AGP 8.8 · target SDK 36 | Első teszt app (`dev.hm.app`) |

## Struktúra

```
projects/
  android-app/       # Android (Kotlin + Compose) — első teszt app
  <új-projekt>/      # további projektek ide jönnek
.github/workflows/   # projekt-szintű CI munkafolyamatok (path-szűrt triggerek)
```

## Build és tesztelés

Minden ellenőrzés a `.github/workflows/` alatti munkafolyamatokon fut:
push/PR esetén automatikusan, vagy `workflow_dispatch`-csel manuálisan
indítható. APK-k a futás artifact-jaiból tölthetők le.
