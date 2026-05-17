plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "dev.hm.kaliterm"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.hm.kaliterm"
        minSdk = 24
        // targetSdk = 28 — Termux-pattern. Android Q+ (API 29+) az `app_data_file:s0`
        // SELinux context-szel jelölt `/data/data/<pkg>/files/`-en NEM enged
        // exec-et, ami a kicsomagolt rootfs bin-jeit (/usr/bin/bash, env, …)
        // is érinti → proot a chrooted execve-n "Permission denied"-et kap.
        // SDK 28-on a Q-előtti policy érvényes → exec engedélyezett.
        targetSdk = 28
        versionCode = 1
        versionName = "0.1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            // Production cél az ARM64. A többi ABI-t később adjuk hozzá
            // ha szükséges (x86_64 emulátorhoz hasznos lehet fejlesztésnél).
            abiFilters += setOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                )
                cppFlags += listOf("-std=c++17")
                cFlags   += listOf("-std=c11", "-Wall", "-Wextra")
            }
        }

        vectorDrawables {
            useSupportLibrary = true
        }
    }

    signingConfigs {
        // Stabil debug-key: minden APK build ezzel íródik alá, hogy az
        // előzőre rátelepülés ne dobjon "INSTALL_FAILED_UPDATE_INCOMPATIBLE"-t
        // (eltérő signature) — sem CI build, sem fejlesztői build között.
        getByName("debug") {
            storeFile = file("kaliterm-debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        // AGP 8.x alapból false; az LklService Binder IPC-jéhez kell
        // (src/main/aidl/.../ILklService.aidl → auto-generált Stub/Proxy).
        aidl = true
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Assets/raw: ne re-compressel-je az AGP az amúgy is tömörített
    // formátumokat. KRITIKUS a kalifs-arm64-minimal.tar.xz-re: a default
    // deflate az XZ tartalmat csonkítja, a Kotlin oldali XZInputStream
    // pedig "EOFException null"-lel elhal kicsomagoláskor.
    androidResources {
        noCompress += listOf("xz", "tar.xz")
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        // KÖTELEZŐ — AGP 8+ alapból `useLegacyPackaging = false`, ami azt
        // jelenti hogy a .so fájlok COMPRESSED-ek maradnak az APK-ban és
        // SOSEM kerülnek ki a fájlrendszerre. Mi a `libproot.so`-t ELF
        // executable-ként akarjuk futtatni (NEM dlopen-nel), tehát szükségünk
        // van a kicsomagolt path-ra a `nativeLibraryDir`-ben.
        // Termux is így csinálja.
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)

    // Termux terminal-emulator + terminal-view — vendor-elve a CI-build
    // előtt fetch-termux-terminal.sh-vel. Feltételesen, hogy ne hibázzon
    // a Gradle config helyiben ha még nincsenek lekérve.
    if (rootProject.file("terminal-emulator").exists()) {
        implementation(project(":terminal-emulator"))
    }
    if (rootProject.file("terminal-view").exists()) {
        implementation(project(":terminal-view"))
    }

    // Kali rootfs tar.xz kicsomagolása pure-Java-ban (Android toybox NEM
    // szállít `xz`-t, ezért ProcessBuilder("xz -dc | tar -xf -") elbukik).
    // commons-compress = TAR olvasás + Unix permission/symlink támogatás.
    // xz = LZMA/XZ stream-dekódolás (kis ~110 KB JAR).
    implementation("org.apache.commons:commons-compress:1.27.1")
    implementation("org.tukaani:xz:1.10")

    testImplementation(libs.junit)

    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.ui.test.junit4)

    debugImplementation(libs.androidx.ui.tooling)
    debugImplementation(libs.androidx.ui.test.manifest)
}
