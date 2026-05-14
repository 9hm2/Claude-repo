# Claude-repo

Több különálló projektet tartalmazó monorepo.

## Projektek

| Projekt | Stack | Leírás |
| --- | --- | --- |
| [`projects/android-app`](projects/android-app) | Kotlin · Jetpack Compose · AGP 8.7 | Android alkalmazás váz (`dev.hm.app`) |

## Struktúra

```
projects/
  android-app/      # Android (Kotlin + Compose) — elsődleges projekt
  <új-projekt>/     # további projektek ide jönnek
.github/workflows/  # CI munkafolyamatok projektenként (path szűrt triggerek)
```

Új projekt hozzáadásához hozz létre egy új mappát a `projects/` alatt saját
README-vel, build-konfigurációval, és — ha kell — egy hozzá tartozó workflow-val
a `.github/workflows/` alatt.
