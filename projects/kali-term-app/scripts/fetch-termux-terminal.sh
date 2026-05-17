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

  # A Termux saját unit-tesztjei JUnit 3-as `junit.framework.TestCase`-t
  # importálnak, és külön testRunner-konfigot várnak. Mi nem szállunk be
  # a Termux-tesztek maintenance-jébe — a vendor modulokat csak mint
  # library-ket akarjuk linkelni. Eltávolítjuk a test-forrásokat hogy a
  # `:terminal-emulator:test` task üresen átmenjen.
  rm -rf "${DST}/src/test" "${DST}/src/androidTest"
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

# Patch: TerminalView.updateSize() null-guard a mRenderer-re. A
# Compose-os AndroidView interop layout-pass-ben hív onSizeChanged-et
# MIELŐTT a factory-block setTextSize/setTypeface-szel a renderert
# inicializálná. Stock Termux-ban ez NPE: `mFontWidth on null object
# reference`. Defenzív early-return — a hívó (setTextSize/setTypeface/
# attachSession) később úgyis újra-hívja az updateSize-t.
TV_JAVA="${PROJ_DIR}/terminal-view/src/main/java/com/termux/view/TerminalView.java"
if ! grep -q "if (mRenderer == null) return;" "${TV_JAVA}"; then
    echo "[termux] patch TerminalView.updateSize — mRenderer null-guard"
    python3 - "${TV_JAVA}" <<'EOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
needle = "if (viewWidth == 0 || viewHeight == 0 || mTermSession == null) return;"
if needle not in src:
    sys.exit(f"patch anchor nincs meg: {needle}")
patch = needle + "\n        if (mRenderer == null) return;"
p.write_text(src.replace(needle, patch, 1))
EOF
fi

# Patch: TerminalSession external-pty-fd support — Phase 3 arch-refaktor.
# A Termux upstream TerminalSession `final` (nem subclass-elhető), és
# `JNI.createSubprocess` mindig forkol a Service process-ében. Phase 3-ban
# a bash a `:lkl` process-ben fut, a main csak a PTY master fd-t kapja
# Binder/PFD-n át. Ezért alternate-ctor-t adunk a TerminalSession-höz, ami
# fork helyett a pre-existing fd-t használja, és waitFor-szálat sem indít.
TS_JAVA="${PROJ_DIR}/terminal-emulator/src/main/java/com/termux/terminal/TerminalSession.java"
if ! grep -q "mExternalPtyFd" "${TS_JAVA}"; then
    echo "[termux] patch TerminalSession — Phase 3 external-pty-fd support"
    python3 - "${TS_JAVA}" <<'EOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()

# 1) final eltávolítása az osztálydeklarációból
src = src.replace(
    "public final class TerminalSession extends TerminalOutput {",
    "public class TerminalSession extends TerminalOutput {",
    1,
)

# 2) mTerminalFileDescriptor láthatósága private → package-private
src = src.replace(
    "    private int mTerminalFileDescriptor;",
    "    int mTerminalFileDescriptor;",
    1,
)

# 3) Alternate ctor + external-pty-fd field-ek a meglévő ctor UTÁN
old_ctor = (
    "    public TerminalSession(String shellPath, String cwd, String[] args, String[] env, Integer transcriptRows, TerminalSessionClient client) {\n"
    "        this.mShellPath = shellPath;\n"
    "        this.mCwd = cwd;\n"
    "        this.mArgs = args;\n"
    "        this.mEnv = env;\n"
    "        this.mTranscriptRows = transcriptRows;\n"
    "        this.mClient = client;\n"
    "    }"
)
new_ctor = old_ctor + "\n\n" + (
    "    /** External PTY master fd (Phase 3); 0 = a default forkpty path. */\n"
    "    private int mExternalPtyFd = 0;\n"
    "    private int mExternalPid = 0;\n"
    "\n"
    "    /** Alternate ctor: wrap a pre-existing PTY master fd (no fork in this process).\n"
    "     *  The shell process lives elsewhere (e.g. `:lkl` process). */\n"
    "    public TerminalSession(int externalPtyFd, int externalPid, Integer transcriptRows, TerminalSessionClient client) {\n"
    "        this.mShellPath = \"(external)\";\n"
    "        this.mCwd = \"\";\n"
    "        this.mArgs = new String[0];\n"
    "        this.mEnv = new String[0];\n"
    "        this.mTranscriptRows = transcriptRows;\n"
    "        this.mClient = client;\n"
    "        this.mExternalPtyFd = externalPtyFd;\n"
    "        this.mExternalPid = externalPid;\n"
    "    }"
)
if old_ctor not in src:
    sys.exit("TerminalSession patch anchor (orig ctor) nincs meg")
src = src.replace(old_ctor, new_ctor, 1)

# 4) initializeEmulator: external módban fork helyett a pre-made fd-t
old_init = (
    "        int[] processId = new int[1];\n"
    "        mTerminalFileDescriptor = JNI.createSubprocess(mShellPath, mCwd, mArgs, mEnv, processId, rows, columns);\n"
    "        mShellPid = processId[0];"
)
new_init = (
    "        int[] processId = new int[1];\n"
    "        if (mExternalPtyFd > 0) {\n"
    "            mTerminalFileDescriptor = mExternalPtyFd;\n"
    "            mShellPid = mExternalPid;\n"
    "            JNI.setPtyWindowSize(mTerminalFileDescriptor, rows, columns);\n"
    "        } else {\n"
    "            mTerminalFileDescriptor = JNI.createSubprocess(mShellPath, mCwd, mArgs, mEnv, processId, rows, columns);\n"
    "            mShellPid = processId[0];\n"
    "        }"
)
if old_init not in src:
    sys.exit("TerminalSession patch anchor (initializeEmulator) nincs meg")
src = src.replace(old_init, new_init, 1)

# 5) Reader thread EOF-on poszt MSG_PROCESS_EXITED-et external módban
old_reader = (
    "                    while (true) {\n"
    "                        int read = termIn.read(buffer);\n"
    "                        if (read == -1) return;\n"
    "                        if (!mProcessToTerminalIOQueue.write(buffer, 0, read)) return;\n"
    "                        mMainThreadHandler.sendEmptyMessage(MSG_NEW_INPUT);\n"
    "                    }"
)
new_reader = (
    "                    while (true) {\n"
    "                        int read = termIn.read(buffer);\n"
    "                        if (read == -1) {\n"
    "                            if (mExternalPtyFd > 0) {\n"
    "                                mMainThreadHandler.sendMessage(mMainThreadHandler.obtainMessage(MSG_PROCESS_EXITED, 0));\n"
    "                            }\n"
    "                            return;\n"
    "                        }\n"
    "                        if (!mProcessToTerminalIOQueue.write(buffer, 0, read)) return;\n"
    "                        mMainThreadHandler.sendEmptyMessage(MSG_NEW_INPUT);\n"
    "                    }"
)
if old_reader not in src:
    sys.exit("TerminalSession patch anchor (reader EOF) nincs meg")
src = src.replace(old_reader, new_reader, 1)

# 6) Waiter thread NE induljon external módban (nincs waitpid-elhető pid)
old_waiter = (
    "        new Thread(\"TermSessionWaiter[pid=\" + mShellPid + \"]\") {\n"
    "            @Override\n"
    "            public void run() {\n"
    "                int processExitCode = JNI.waitFor(mShellPid);\n"
    "                mMainThreadHandler.sendMessage(mMainThreadHandler.obtainMessage(MSG_PROCESS_EXITED, processExitCode));\n"
    "            }\n"
    "        }.start();\n"
    "\n"
    "    }"
)
new_waiter = (
    "        if (mExternalPtyFd == 0) {\n"
    "            new Thread(\"TermSessionWaiter[pid=\" + mShellPid + \"]\") {\n"
    "                @Override\n"
    "                public void run() {\n"
    "                    int processExitCode = JNI.waitFor(mShellPid);\n"
    "                    mMainThreadHandler.sendMessage(mMainThreadHandler.obtainMessage(MSG_PROCESS_EXITED, processExitCode));\n"
    "                }\n"
    "            }.start();\n"
    "        }\n"
    "    }"
)
if old_waiter not in src:
    sys.exit("TerminalSession patch anchor (waiter thread) nincs meg")
src = src.replace(old_waiter, new_waiter, 1)

# 7) write() external módban is engedélyezett (mShellPid lehet 0 ha :lkl
#    nem updateelte még, ezért az mExternalPtyFd-t is check-eljük)
old_write = "        if (mShellPid > 0) mTerminalToProcessIOQueue.write(data, offset, count);"
new_write = "        if (mShellPid > 0 || mExternalPtyFd > 0) mTerminalToProcessIOQueue.write(data, offset, count);"
if old_write not in src:
    sys.exit("TerminalSession patch anchor (write) nincs meg")
src = src.replace(old_write, new_write, 1)

p.write_text(src)
print(f"[termux] patched: {p}")
EOF
fi

echo "[termux] kész:"
ls -la "${PROJ_DIR}/terminal-emulator"
ls -la "${PROJ_DIR}/terminal-view"
