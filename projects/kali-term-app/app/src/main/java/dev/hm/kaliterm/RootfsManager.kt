package dev.hm.kaliterm

import android.content.Context
import android.util.Log
import java.io.File
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
     * tar.xz kicsomagolása. Bionic-on nincs `tar` parancs, és libarchive-ot
     * sem akarunk JNI-zni. Helyette: külső `proot` parancs SOSEM kell az
     * első kicsomagoláshoz, az Android-on van `xz` (toybox-os busybox) ÉS
     * `tar` (toybox-os). Toybox tar+xz együttese pont jó.
     *
     * Stratégia: `xz -dc` (decode to stdout) + `tar -x` egy ProcessBuilderrel.
     * Mindkettő toybox-szal van Android-on.
     */
    private fun extractTarXz(tarball: File, dst: File, progressCb: ((String) -> Unit)?) {
        progressCb?.invoke("xz dekódolás + tar extrakció…")
        // toybox xz: `xz -dc <file>` → stdout
        // toybox tar: `tar -xf -` stdin-ből olvas; `-C <dir>` waar kicsomagol
        val pb = ProcessBuilder(
            "sh", "-c",
            "xz -dc ${tarball.absolutePath} | tar -xf - -C ${dst.absolutePath}"
        )
        pb.redirectErrorStream(true)
        val p = pb.start()
        val output = p.inputStream.bufferedReader().readText()
        val rc = p.waitFor()
        if (rc != 0) {
            throw RuntimeException("xz|tar exit=$rc — $output")
        }
        Log.i(tag, "extracted: ${dst.absolutePath}, files=${dst.walkTopDown().count()}")
    }
}
