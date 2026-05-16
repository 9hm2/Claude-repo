package dev.hm.kaliterm

import android.content.Context
import android.system.Os
import android.util.Log
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.tukaani.xz.XZInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

/**
 * A kali-rootfs bundle (proot binary + kalifs-arm64-minimal.tar.xz + launch.sh)
 * az APK `assets/rootfs/`-ében jön. Első indítás:
 *   1) assets/rootfs/{proot, launch.sh} → $filesDir/rootfs-bundle/
 *   2) assets/rootfs/kalifs-arm64-minimal.tar.xz → kicsomagolás $filesDir/rootfs/
 *      (egyszeri, kb. 30-60 sec — Java oldali xz dekódolás + tar extract)
 *   3) chmod +x proot, launch.sh
 *
 * Idempotens: a "ready" markerfájl ($filesDir/rootfs/.kaliterm-ready)
 * jelzi hogy a kicsomagolás befejeződött. Ha létezik, az újraindításnál
 * skippeljük.
 *
 * Az osztály egy `prepare(): Result` szuszpend-mentes függvényt ad amit a
 * UI háttér-szálon hívhat (coroutine vagy Thread).
 */
class RootfsManager(private val ctx: Context) {

    private val tag = "kaliterm-rootfs"

    /** A bundle gyökere (launch.sh + esetleg tar.xz másolat). */
    val bundleDir: File = File(ctx.filesDir, "rootfs-bundle")
    /** A kibontott Kali fa gyökere (chroot-target). */
    val rootfsDir: File = File(ctx.filesDir, "rootfs")
    /** Proot temp-mappa — `PROOT_TMP_DIR` és `TMPDIR` env-változókhoz.
     *  Android-on nincs `/tmp`, proot enélkül `proot_tmp_dir not set`
     *  hibával hal el. A filesDir alatt writable+executable terület. */
    val prootTmpDir: File = File(ctx.filesDir, "proot-tmp")
    /** Marker amit a kicsomagolás végén írunk. */
    private val readyMarker: File = File(rootfsDir, ".kaliterm-ready")

    /**
     * A proot binary helye — a `nativeLibraryDir`, NEM a `filesDir`.
     *
     * Indok (Android W^X policy, target SDK 29+): a `/data/data/<pkg>/files/`
     * mountpoint nem-executable. Ha innen próbálnánk forkkal indítani a
     * proot-ot, "Permission denied"-et kapnánk akkor is, ha az `rwx` bit
     * minden szinten be van állítva.
     *
     * Megoldás (Termux-pattern): a proot-ot `libproot.so` néven az APK
     * jniLibs/arm64-v8a/ mappájába rakjuk. A package manager kicsomagolja
     * `applicationInfo.nativeLibraryDir`-be, ami read-only DE executable.
     */
    val nativeProot: File = File(ctx.applicationInfo.nativeLibraryDir, "libproot.so")
    val launchSh: File   get() = File(bundleDir, "launch.sh")

    fun isReady(): Boolean = readyMarker.exists() && nativeProot.canExecute()

    /**
     * Előkészíti a rootfs-t. UI-thread-en NE hívd — ez lassú (tar+xz).
     * @return null ha sikeres, vagy hibaüzenet.
     */
    @Synchronized
    fun prepare(progressCb: ((String) -> Unit)? = null): String? {
        try {
            bundleDir.mkdirs()
            rootfsDir.mkdirs()
            prootTmpDir.mkdirs()

            // 1) Sanity: a proot binárisnak léteznie + futtathatónak kell
            //    lennie a nativeLibraryDir-ben. Ha nincs ott, az APK packelése
            //    rossz volt (jniLibs/arm64-v8a/libproot.so hiányzik).
            if (!nativeProot.exists()) {
                return "HIBA: proot binary nincs a nativeLibraryDir-ben " +
                    "(${nativeProot.absolutePath}). APK build hibás?"
            }
            if (!nativeProot.canExecute()) {
                return "HIBA: ${nativeProot.absolutePath} nem futtatható (chmod?)"
            }

            // 2) Launch.sh INLINE generálása — NEM az asset-bundle-ből másoljuk.
            //    Indok: a bundled launch.sh egy hardcoded `PROOT="$PREFIX/proot"`
            //    sorral jön, ami a régi (rossz) layout-ot feltételezi. Mi most
            //    a nativeLibraryDir-ből futtatjuk a proot-ot, és az env-ben
            //    átadott `$PROOT`-ot kell tisztelnünk.
            progressCb?.invoke("launch.sh generálása…")
            File(bundleDir, "launch.sh").writeText(launchScriptTemplate())
            File(bundleDir, "launch.sh").setExecutable(true, false)

            if (readyMarker.exists()) {
                Log.i(tag, "rootfs already extracted")
                // resolv.conf frissítése akkor is, ha a fa már ki van bontva
                // — különben a régi (üres) resolv.conf marad, és apt fail.
                writeResolvConf()
                return null
            }

            // 3) tar.xz kicsomagolása
            progressCb?.invoke("Kali rootfs kicsomagolása (38 MB)…")
            val tarball = File(bundleDir, "kalifs-arm64-minimal.tar.xz")
            copyAsset("rootfs/kalifs-arm64-minimal.tar.xz", tarball)

            // Eltávolítjuk a régi részleges fa-t ha volt
            if (rootfsDir.exists()) rootfsDir.deleteRecursively()
            rootfsDir.mkdirs()

            extractTarXz(tarball, rootfsDir, progressCb)
            tarball.delete()  // ~38 MB már nem kell

            readyMarker.writeText("ready ${System.currentTimeMillis()}\n")

            // DNS-resolver: a Kali rootfs `/etc/resolv.conf` üresen jön a
            // debootstrap-ből; chrooted apt-update DNS-fail-el (Temporary
            // failure resolving 'kali.download'). Bind-mount-tal a host
            // resolv.conf-ot nem tudjuk át-pull-ozni (Android-permission),
            // ezért közvetlenül public-DNS-eket írunk be. Idempotens —
            // minden indításkor frissül, hogy a clear-data nélkül is hatós.
            writeResolvConf()

            progressCb?.invoke("kész")
            return null
        } catch (t: Throwable) {
            Log.e(tag, "prepare failed", t)
            return "RootfsManager hiba: ${t.javaClass.simpleName}: ${t.message}"
        }
    }

    /**
     * Launch.sh inline template — a TerminalSession `sh launch.sh`-szel hívja.
     *
     * Bemenet env-ből (a KaliShellService állítja be):
     *   PREFIX        a bundleDir absolute path-ja
     *   ROOTFS_DIR    a kicsomagolt Kali fa absolute path-ja
     *   PROOT         a proot binary teljes path-ja (nativeLibraryDir-ből)
     *   USER_HOME     default /root
     *
     * Kimenet: vagy belépés a Kali-bash-ba (siker), vagy diagnosztika +
     * fallback Android-sh-ba (hiba). SOSEM tér vissza üres state-tel a hívóhoz.
     */
    private fun launchScriptTemplate(): String = """#!/system/bin/sh
# kaliterm rootfs launcher — proot wrapper diagnosztikával.
# A PROOT env-változót az Android-oldal állítja be (nativeLibraryDir/libproot.so),
# mert a `/data/data/<pkg>/files/` nem-executable (Android W^X policy).

: "${'$'}{PREFIX:?PREFIX env változó kell}"
: "${'$'}{ROOTFS_DIR:=${'$'}PREFIX/../rootfs}"
: "${'$'}{USER_HOME:=/root}"
: "${'$'}{PROOT:=${'$'}PREFIX/proot}"  # fallback ha az Android-oldal nem állítja be

echo "════════════════════════════════════════════════"
echo " kaliterm — proot launcher"
echo "════════════════════════════════════════════════"
echo "PREFIX     = ${'$'}PREFIX"
echo "ROOTFS_DIR = ${'$'}ROOTFS_DIR"
echo "PROOT      = ${'$'}PROOT"
echo "USER_HOME  = ${'$'}USER_HOME"
echo

# 1) proot bin sanity
if [ ! -f "${'$'}PROOT" ]; then
    echo "✗ HIBA: ${'$'}PROOT nem létezik"
    echo "Drop Android sh-ba a vizsgálathoz."
    exec /system/bin/sh
fi
if [ ! -x "${'$'}PROOT" ]; then
    echo "✗ HIBA: ${'$'}PROOT nem futtatható (W^X policy?)"
    ls -la "${'$'}PROOT"
    exec /system/bin/sh
fi

# 2) proot --version (gyors sanity, hogy linkel-e a libc)
echo "─── proot --version ───"
"${'$'}PROOT" --version 2>&1 || {
    echo "✗ HIBA: proot --version sikertelen (linkelési/ABI hiba?)"
    exec /system/bin/sh
}
echo

# 3) Rootfs sanity
if [ ! -d "${'$'}ROOTFS_DIR" ]; then
    echo "✗ HIBA: ${'$'}ROOTFS_DIR nincs (rootfs nem lett kicsomagolva?)"
    exec /system/bin/sh
fi
if [ ! -x "${'$'}ROOTFS_DIR/bin/bash" ]; then
    echo "✗ HIBA: ${'$'}ROOTFS_DIR/bin/bash nincs vagy nem futtatható"
    ls -la "${'$'}ROOTFS_DIR/bin/" 2>&1 | head -20
    exec /system/bin/sh
fi
echo "✓ rootfs OK (${'$'}(ls "${'$'}ROOTFS_DIR" | wc -l) toplevel-bejegyzés)"
echo

# 4) Kali bash indítása proot chroot-on át — Termux PRoot-Distro receptje.
#
# A binárisunk most a TERMUX FORK (build-proot.sh-szal letöltve a
# termux/proot master-ből, NDK-szal statikusan build-elt libtalloc-cal
# és inline tracee-loader-rel). Tartalmazza az Android-szpecifikus
# kernel-hook patcheket — különben a SECCOMP_MODE_FILTER az upstream
# proot tracee-jét SIGSYS-szel (signal 31) megöli.
echo "─── proot indítása → /bin/bash ───"
exec "${'$'}PROOT" \
    --kill-on-exit \
    --link2symlink \
    -0 \
    --kernel-release="${'$'}{LKL_KERNEL_RELEASE:-6.1.0-kali}" \
    -r "${'$'}ROOTFS_DIR" \
    -w "${'$'}USER_HOME" \
    -b /dev \
    -b /proc \
    -b /sys \
    -b /proc/self/fd/0:/dev/stdin \
    -b /proc/self/fd/1:/dev/stdout \
    -b /proc/self/fd/2:/dev/stderr \
    -b /proc/self/fd:/dev/fd \
    -b /dev/urandom:/dev/random \
    -b /dev/null:/proc/sys/kernel/cap_last_cap \
    /usr/bin/env -i \
        HOME="${'$'}USER_HOME" \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        TERM="${'$'}{TERM:-xterm-256color}" \
        TMPDIR=/tmp \
        LANG=C.UTF-8 \
        /bin/bash --login

# Ide csak akkor jutunk, ha az exec proot SIKERTELEN volt.
echo "✗ HIBA: exec proot sikertelen (${'$'}?)"
echo "Drop to Android sh."
exec /system/bin/sh
"""

    /** Public DNS resolver-bejegyzések a chrooted Kali /etc/resolv.conf-ba. */
    private fun writeResolvConf() {
        try {
            val resolvConf = File(rootfsDir, "etc/resolv.conf")
            resolvConf.parentFile?.mkdirs()
            // resolv.conf gyakran egy symlink (systemd-resolved-stúb)
            // → előbb töröljük, hogy ne a target-et írjuk át véletlenül.
            if (resolvConf.exists() || java.nio.file.Files.isSymbolicLink(resolvConf.toPath())) {
                resolvConf.delete()
            }
            resolvConf.writeText(
                "# kaliterm — auto-generated\n" +
                "nameserver 1.1.1.1\n" +
                "nameserver 8.8.8.8\n" +
                "nameserver 9.9.9.9\n"
            )
        } catch (t: Throwable) {
            Log.w(tag, "writeResolvConf failed: ${t.message}")
        }
    }

    private fun copyAsset(assetPath: String, dst: File) {
        // ctx.assets.openFd() méretet ad ha az asset uncompressed-en van az
        // APK-ban (noCompress lista). Compressed asset-eknél a length=-1
        // jön, akkor csak best-effort másolunk.
        val expectedSize: Long = try {
            ctx.assets.openFd(assetPath).use { it.length }
        } catch (_: Throwable) { -1L }

        ctx.assets.open(assetPath).use { input ->
            FileOutputStream(dst).use { output ->
                input.copyTo(output)
            }
        }

        val actualSize = dst.length()
        Log.i(tag, "copyAsset $assetPath: expected=$expectedSize, actual=$actualSize")
        if (expectedSize > 0 && actualSize != expectedSize) {
            throw java.io.IOException(
                "copyAsset csonka: $assetPath — várt $expectedSize byte, " +
                "ténylegesen $actualSize. AGP noCompress beállítás hiányzik?"
            )
        }
    }

    /**
     * tar.xz kicsomagolás pure-Java-ban (Android toybox NEM szállít `xz`-t).
     *
     *  - XZInputStream: az XZ stream-et LZMA2 + integrity-check (CRC64)
     *    dekódolva tar bájtokká visszafejti.
     *  - TarArchiveInputStream: végigmegy a tar headereken, eddig 512-byte
     *    blokk-egységekben.
     *  - Minden TarArchiveEntry:
     *      type=Dir         → mkdir
     *      type=File        → write bytes
     *      type=Symlink     → Os.symlink(target, linkpath)
     *      type=Hardlink    → Os.link(target, linkpath)
     *  - Permission bit-eket Os.chmod-dal állítjuk vissza.
     *
     * Symlink-kezelés FONTOS: a Kali rootfs sok bin/lib relatív symlinket
     * tartalmaz (pl. /usr/sbin → /usr/bin); enélkül a /bin/bash sem indul.
     */
    private fun extractTarXz(tarball: File, dst: File, progressCb: ((String) -> Unit)?) {
        val total = tarball.length()
        progressCb?.invoke("xz dekódolás + tar extrakció (${total / 1024}KB)…")
        if (total < 1024) {
            throw java.io.IOException(
                "tar.xz gyanúsan kicsi (${total} byte). copyAsset hibás? " +
                "AGP noCompress beállítás hiányzik?"
            )
        }
        var entries = 0

        try {
            FileInputStream(tarball).use { fis ->
                BufferedInputStream(fis).use { bis ->
                    XZInputStream(bis).use { xz ->
                        TarArchiveInputStream(xz).use { tar ->
                            while (true) {
                                val entry = tar.nextEntry ?: break
                                extractEntry(entry, tar, dst)
                                entries++
                                if (entries % 500 == 0) {
                                    progressCb?.invoke("kicsomagolás: $entries fájl…")
                                }
                            }
                        }
                    }
                }
            }
        } catch (e: java.io.EOFException) {
            // Tipikus szignatúra: AGP duplán-tömörítette az asset-et és
            // a getAssets().open() inflate-elt streamje csonka, vagy a
            // tar.xz fájl csonka volt másoláskor. A noCompress fix.
            throw java.io.IOException(
                "tar.xz csonka — entries=$entries, tarball-size=$total. " +
                "Lehet AGP noCompress beállítás hiányzik a build.gradle.kts-ben " +
                "(`androidResources.noCompress += \"xz\"`).",
                e
            )
        }
        Log.i(tag, "extracted: ${dst.absolutePath}, entries=$entries")
        progressCb?.invoke("kicsomagolva: $entries fájl")
    }

    private fun extractEntry(entry: TarArchiveEntry, tar: TarArchiveInputStream, dst: File) {
        // Path-traversal védelem string-szintű normalizációval. NEM
        // canonicalFile-lal — az követné a symlinkeket, és a Kali
        // usr-merged layout (/bin → usr/bin, /sbin → usr/sbin, /lib → usr/lib)
        // "Too many symbolic links" ELOOP-pal halna el.
        val normalized = java.nio.file.Paths.get(entry.name).normalize()
        if (normalized.isAbsolute || normalized.toString().startsWith("..")) {
            Log.w(tag, "skip path-traversal: ${entry.name}")
            return
        }
        val out = File(dst, normalized.toString())

        when {
            entry.isDirectory -> {
                out.mkdirs()
            }
            entry.isSymbolicLink -> {
                out.parentFile?.mkdirs()
                if (out.exists()) out.delete()
                try {
                    Os.symlink(entry.linkName, out.absolutePath)
                } catch (e: Throwable) {
                    Log.w(tag, "symlink failed ${entry.name} → ${entry.linkName}: ${e.message}")
                }
            }
            entry.isLink -> {
                out.parentFile?.mkdirs()
                if (out.exists()) out.delete()
                val src = File(dst, entry.linkName)
                try {
                    Os.link(src.absolutePath, out.absolutePath)
                } catch (e: Throwable) {
                    // Hardlink EACCES Android-on: a `app_data_file:s0` SELinux
                    // context tiltja a hardlink-et. Termux-pattern: fallback
                    // symlink-re. (A proot --link2symlink flagje már átveszi
                    // szemantikailag a chrooted oldalon.)
                    try {
                        Os.symlink(entry.linkName, out.absolutePath)
                    } catch (e2: Throwable) {
                        Log.w(tag, "hardlink+symlink failed ${entry.name} → ${entry.linkName}: ${e.message}; sym: ${e2.message}")
                    }
                }
            }
            entry.isFile -> {
                out.parentFile?.mkdirs()
                FileOutputStream(out).use { fos -> tar.copyTo(fos) }
            }
            else -> {
                // FIFO/char-dev/block-dev: kihagyjuk. A rootfs-szel nem
                // értünk semmit ezekkel az android-userspace szempontjából.
                Log.d(tag, "skip special entry: ${entry.name}")
                return
            }
        }

        // Unix permission bit-ek (TarArchiveEntry.mode-ban) — Os.chmod-dal.
        if (!entry.isSymbolicLink) {
            try {
                val mode = entry.mode and 0xFFF  // alsó 12 bit (suid/sgid/sticky + rwx)
                if (mode != 0) Os.chmod(out.absolutePath, mode)
            } catch (e: Throwable) {
                Log.d(tag, "chmod ${entry.name} m=${entry.mode}: ${e.message}")
            }
        }
    }
}
