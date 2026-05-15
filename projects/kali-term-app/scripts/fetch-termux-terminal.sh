#!/usr/bin/env bash
# Letöltjük a Termux terminal-emulator + terminal-view Gradle modulokat
# az upstream termux/termux-app projektből, és bemásoljuk a saját
# projektünkbe Gradle sub-modulokként. Innentől a kali-term-app/app modul
# rájuk hivatkozhat `implementation(project(":terminal-emulator"))` stb.
#
# Az upstream Termux GPL-3.0 — a vendor-elt source-t a saját repó-nkban
# NEM tartjuk (nem szennyezzük a tárat). Minden CI futás letölti frissen.
#
# Verzió-pin: TERMUX_VERSION env-en át felülírható.
set -euo pipefail

PROJ_DIR="${1:?usage: fetch-termux-terminal.sh <kali-term-app dir>}"
TERMUX_VERSION="${TERMUX_VERSION:-v0.118.1}"

TARBALL_URL="https://github.com/termux/termux-app/archive/refs/tags/${TERMUX_VERSION}.tar.gz"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

echo "[termux] download ${TARBALL_URL}"
curl -fL --retry 4 --retry-delay 2 -o "${WORK_DIR}/termux.tar.gz" "${TARBALL_URL}"
tar -xzf "${WORK_DIR}/termux.tar.gz" -C "${WORK_DIR}"

SRC_ROOT=$(find "${WORK_DIR}" -maxdepth 2 -name 'termux-app-*' -type d | head -n1)
if [[ -z "${SRC_ROOT}" ]]; then
  echo "[termux] HIBA: nem találom a kicsomagolt termux-app forrást" >&2
  ls "${WORK_DIR}" >&2
  exit 2
fi
echo "[termux] src root: ${SRC_ROOT}"

for mod in terminal-emulator terminal-view; do
  SRC="${SRC_ROOT}/${mod}"
  DST="${PROJ_DIR}/${mod}"
  if [[ ! -d "${SRC}" ]]; then
    echo "[termux] HIBA: nincs ${SRC}" >&2; exit 3
  fi
  echo "[termux] copy ${mod}"
  rm -rf "${DST}"
  cp -r "${SRC}" "${DST}"

  # Régi build.gradle eltávolítása — saját build.gradle.kts-t teszünk be,
  # ami modern AGP-t és AndroidX-et használ (Termux upstream még groovy
  # Gradle-t használ a saját verziójával).
  rm -f "${DST}/build.gradle"
done

# Saját build.gradle.kts a terminal-emulator-hez (pure Java + JNI).
cat > "${PROJ_DIR}/terminal-emulator/build.gradle.kts" <<'EOF'
plugins {
    id("com.android.library")
}

android {
    namespace = "com.termux.terminal"
    compileSdk = 36

    defaultConfig {
        minSdk = 24
        ndk {
            //noinspection ChromeOsAbiSupport
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            ndkBuild {
                cFlags("-std=c11", "-Wall", "-Wextra", "-Werror=implicit-function-declaration")
            }
        }
    }

    externalNativeBuild {
        ndkBuild {
            path = file("src/main/jni/Android.mk")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.annotation:annotation:1.8.0")
}
EOF

# Saját build.gradle.kts a terminal-view-hez (pure Java/Kotlin View).
cat > "${PROJ_DIR}/terminal-view/build.gradle.kts" <<'EOF'
plugins {
    id("com.android.library")
}

android {
    namespace = "com.termux.view"
    compileSdk = 36

    defaultConfig {
        minSdk = 24
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.annotation:annotation:1.8.0")
    implementation(project(":terminal-emulator"))
}
EOF

# Mindkét modul-hoz `src/main/AndroidManifest.xml` kell, ha még nincs.
for mod in terminal-emulator terminal-view; do
    MAN="${PROJ_DIR}/${mod}/src/main/AndroidManifest.xml"
    if [[ ! -f "${MAN}" ]]; then
        echo "[termux] dummy AndroidManifest.xml for ${mod}"
        mkdir -p "$(dirname "${MAN}")"
        cat > "${MAN}" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" />
EOF
    fi
done

echo "[termux] kész:"
ls -la "${PROJ_DIR}/terminal-emulator"
ls -la "${PROJ_DIR}/terminal-view"
