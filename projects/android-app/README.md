# android-app

Első **teszt applikáció** — Kotlin + Jetpack Compose, legmagasabb stabil API szint.

> A build és a tesztelés **kizárólag GitHub Actions** alatt fut.
> Lokálisan nincs build futtatás — a kód itt íródik, a CI ellenőriz és APK-t épít.

## Stack

- **Kotlin** 2.0.21
- **Jetpack Compose** (BOM 2024.11.00, Material 3)
- **Android Gradle Plugin** 8.8.0
- **Gradle** 8.10.2 (wrapper bekommitelve)
- **JDK** 17
- **compileSdk / targetSdk** 36 (Android 16) · **minSdk** 24
- **applicationId / namespace**: `dev.hm.app`

## CI

A `.github/workflows/android.yml` minden push/PR esetén — ami a
`projects/android-app/**` alatt változik — automatikusan futtat:

1. `./gradlew lint`
2. `./gradlew test` (unit tesztek)
3. `./gradlew assembleDebug`
4. A debug APK feltöltése **artifact**-ként (`app-debug-apk`).

A workflow `workflow_dispatch`-csel manuálisan is indítható.

## Forrás-struktúra

```
app/src/main/
  java/dev/hm/app/
    MainActivity.kt
    ui/theme/                # Color, Type, Theme (dynamic color)
  res/                       # drawables, strings, themes, adaptive icon
  AndroidManifest.xml
app/build.gradle.kts         # app modul (compileSdk/targetSdk = 36)
build.gradle.kts             # root projekt
settings.gradle.kts
gradle/libs.versions.toml    # version catalog
```

## Új kód hozzáadása

1. Új Kotlin fájl a `app/src/main/java/dev/hm/app/` alá.
2. Új függőség: bővítsd a `gradle/libs.versions.toml`-t, majd hivatkozd
   `libs.<név>` alakkal a `app/build.gradle.kts`-ben.
3. Commit → push → CI lefut → APK az Actions futás artifact-jaiból letölthető.
