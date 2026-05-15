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

    /** A bundle gyökere (proot, launch.sh, tar.xz másolat). */
    val bundleDir: File = File(ctx.filesDir, "rootfs-bundle")
    /** A kibontott Kali fa gyökere (chroot-target). */
    val rootfsDir: File = File(ctx.filesDir, "rootfs")
    /** Marker amit a kicsomagolás végén írunk. */
    private val readyMarker: File = File(rootfsDir, ".kaliterm-ready")

    val prootBin: File   get() = File(bundleDir, "proot")
    val launchSh: File   get() = File(bundleDir, "launch.sh")

    fun isReady(): Boolean = readyMarker.exists() && prootBin.canExecute()

    /**
     * Előkészíti a rootfs-t. UI-thread-en NE hívd — ez lassú (tar+xz).
     * @return null ha sikeres, vagy hibaüzenet.
     */
    fun prepare(progressCb: ((String) -> Unit)? = null): String? {
        try {
            bundleDir.mkdirs()
            rootfsDir.mkdirs()

            // 1) proot + launch.sh másolása assets-ből
            progressCb?.invoke("proot kicsomagolása…")
            copyAsset("rootfs/proot",     File(bundleDir, "proot"))
            copyAsset("rootfs/launch.sh", File(bundleDir, "launch.sh"))
            File(bundleDir, "proot").setExecutable(true, false)
            File(bundleDir, "launch.sh").setExecutable(true, false)

            if (readyMarker.exists()) {
                Log.i(tag, "rootfs already extracted")
                return null
            }

            // 2) tar.xz kicsomagolása
            progressCb?.invoke("Kali rootfs kicsomagolása (38 MB)…")
            val tarball = File(bundleDir, "kalifs-arm64-minimal.tar.xz")
            copyAsset("rootfs/kalifs-arm64-minimal.tar.xz", tarball)

            // Eltávolítjuk a régi részleges fa-t ha volt
            if (rootfsDir.exists()) rootfsDir.deleteRecursively()
            rootfsDir.mkdirs()

            extractTarXz(tarball, rootfsDir, progressCb)
            tarball.delete()  // ~38 MB már nem kell

            readyMarker.writeText("ready ${System.currentTimeMillis()}\n")
            progressCb?.invoke("kész")
            return null
        } catch (t: Throwable) {
            Log.e(tag, "prepare failed", t)
            return "RootfsManager hiba: ${t.javaClass.simpleName}: ${t.message}"
        }
    }

    private fun copyAsset(assetPath: String, dst: File) {
        ctx.assets.open(assetPath).use { input ->
            FileOutputStream(dst).use { output ->
                input.copyTo(output)
            }
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
        progressCb?.invoke("xz dekódolás + tar extrakció (pure-Java)…")
        val total = tarball.length()
        var lastProgressBytes = 0L
        var entries = 0

        FileInputStream(tarball).use { fis ->
            BufferedInputStream(fis).use { bis ->
                XZInputStream(bis).use { xz ->
                    TarArchiveInputStream(xz).use { tar ->
                        while (true) {
                            val entry = tar.nextEntry ?: break
                            extractEntry(entry, tar, dst)
                            entries++
                            // Progress beolvasott byte-okból (XZ-tömörítettből).
                            // Az XZInputStream nem ad progress-API-t, ezért
                            // hozzávetőlegesen az `entries` alapján mutatjuk.
                            if (entries % 500 == 0) {
                                progressCb?.invoke("kicsomagolás: $entries fájl…")
                            }
                        }
                    }
                }
            }
        }
        Log.i(tag, "extracted: ${dst.absolutePath}, entries=$entries")
        progressCb?.invoke("kicsomagolva: $entries fájl")
    }

    private fun extractEntry(entry: TarArchiveEntry, tar: TarArchiveInputStream, dst: File) {
        // Path-traversal védelem: `..` semmilyen formában ne kerülhessen ki
        // a célmappából.
        val out = File(dst, entry.name).canonicalFile
        if (!out.path.startsWith(dst.canonicalPath + File.separator) && out.path != dst.canonicalPath) {
            Log.w(tag, "skip path-traversal: ${entry.name}")
            return
        }

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
                    Log.w(tag, "hardlink failed ${entry.name} → ${entry.linkName}: ${e.message}")
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
