pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "kali-term-app"
include(":app")

// Termux terminal-emulator + terminal-view modulok — `scripts/fetch-termux-terminal.sh`
// downloadolja őket CI build előtt. Ha helyiben dolgozunk és még nincsenek,
// ne hibázzon a Gradle config — feltételesen include-oljuk.
if (file("terminal-emulator").exists()) include(":terminal-emulator")
if (file("terminal-view").exists())     include(":terminal-view")
