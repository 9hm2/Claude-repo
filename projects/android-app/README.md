# android-app

Kotlin + Jetpack Compose Android projekt váz.

## Stack

- **Kotlin** 2.0.21
- **Jetpack Compose** (BOM 2024.11.00, Material 3)
- **Android Gradle Plugin** 8.7.2
- **Gradle** 8.9
- **JDK** 17 (compile target)
- **minSdk** 24 · **targetSdk/compileSdk** 35
- **applicationId**: `dev.hm.app`

## Felépítés

```
app/
  src/main/
    java/dev/hm/app/        # Kotlin források
      MainActivity.kt
      ui/theme/             # Compose téma (Color, Type, Theme)
    res/                    # Drawables, strings, themes, adaptive icon
    AndroidManifest.xml
  build.gradle.kts          # app modul
build.gradle.kts            # root projekt
settings.gradle.kts
gradle/libs.versions.toml   # version catalog
```

## Build

Android Studio Hedgehog (vagy újabb) ajánlott. Parancssorból:

```bash
./gradlew assembleDebug          # debug APK
./gradlew test                   # unit tesztek
./gradlew connectedAndroidTest   # instrumented tesztek (kell emulátor/eszköz)
./gradlew lint                   # statikus elemzés
```

Az első build a teljes Android SDK-t kéri — Android Studio-ban érdemes először
megnyitni, hogy az SDK letöltődjön (`local.properties` automatikusan generálódik).

## CI

A `.github/workflows/android.yml` minden push/PR-en buildel és tesztel.
