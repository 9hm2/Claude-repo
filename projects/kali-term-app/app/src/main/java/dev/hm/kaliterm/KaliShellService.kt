package dev.hm.kaliterm

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.util.Log
import com.termux.terminal.TerminalSession
import com.termux.terminal.TerminalSessionClient
import java.io.File

/**
 * Termux `TermuxService` mintára: a `TerminalSession` lifecycle-ja Service-ből
 * megy, NEM közvetlenül a Composable-ben. Indok:
 *
 *  - A session a fő-process App lifecycle-jához kötve marad (Composable-rotációk,
 *    Activity recreation túléli). A user érzi: belép → Kali shell, kilép → vissza
 *    → és a shell még él, history-val.
 *  - Egy helyen történik a TerminalSessionClient-dispatch. A bind-elt UI
 *    regisztrál egy listener-t (lásd `setListener`), és minden client-callback
 *    arra dispatchel. Unbind-on a listener null-ra megy → a Service tovább él
 *    de senki nem kapja az event-eket; a session a kernelben fut tovább.
 *  - Egyetlen process — NEM külön `:shell` process, ellentétben az LKL-lel.
 *    A TerminalSession JNI fork + ptrace-szel mehetne másik process-ből is, de
 *    a Termux-mintát követve a fő process-ben tartjuk.
 *
 * **Kritikus** különbség a korábbi (direct) implementációhoz képest:
 *   onTextChanged → forward → UI listener → `terminalView.onScreenUpdated()`.
 *   Enélkül a shell ír a PTY-be, az emulator buffert frissít, de a View
 *   sosem repaint-el → blank terminál. (Ez volt az eredeti bug.)
 */
class KaliShellService : Service() {

    private val tag = "kaliterm-shell-svc"

    @Volatile private var session: TerminalSession? = null
    @Volatile private var listener: TerminalSessionClient? = null

    /** Local binder — bind-elt kliens direkten éri el az API-t (nem AIDL). */
    inner class LocalBinder : Binder() {
        fun getSession(): TerminalSession? = session

        /**
         * Idempotens: ha már van session, visszaadjuk. Egyébként új-at indítunk
         * a megadott rootfs-szel.
         *
         * MEGJEGYZÉS — a `TerminalSession` konstruktor még NEM forkol; a fork
         * a `TerminalView.attachSession()` + `onSizeChanged()` → `updateSize()`
         * → `initializeEmulator(cols, rows)` láncban történik. Itt csak az
         * objektum jön létre.
         */
        fun getOrCreateSession(rootfs: RootfsManager): TerminalSession {
            session?.let { return it }
            // PROOT az APK nativeLibraryDir-ből (libproot.so), NEM a filesDir-ből.
            // Lásd RootfsManager.nativeProot — Android W^X policy miatt.
            //
            // LKL_KERNEL_RELEASE: a `prepareLklMirror()` előre IO-szálon
            // hívott, és a $filesDir/proot-tmp/lkl-proc/ mappát feltöltötte.
            // A `lklProcOsrelease` field-en tárolt érték az osrelease (vagy
            // "6.1.0-kali" fallback ha az LKL nem ready volt).
            val lklRelease = lklProcOsrelease ?: "6.1.0-kali"
            Log.i(tag, "LKL_KERNEL_RELEASE = $lklRelease")

            val lklProcDir = File(rootfs.prootTmpDir, "lkl-proc")
            val env = arrayOf(
                "HOME=${rootfs.bundleDir.absolutePath}",
                "PREFIX=${rootfs.bundleDir.absolutePath}",
                "ROOTFS_DIR=${rootfs.rootfsDir.absolutePath}",
                "PROOT=${rootfs.nativeProot.absolutePath}",
                "LKL_KERNEL_RELEASE=$lklRelease",
                // LKL_PROC_DIR — a fent populate-elt mirror-mappa. A launch.sh
                // kötelezően ellenőrzi (`[ -d $LKL_PROC_DIR ]`); ha a fájlok
                // léteznek, bind-mountolja a chrooted /proc-ra.
                "LKL_PROC_DIR=${lklProcDir.absolutePath}",
                // PROOT_TMP_DIR + TMPDIR — proot kötelezően kér egy writable
                // temp-mappát a mountpoint-emulation cache-jéhez. Android-on
                // nincs /tmp, ezért a filesDir alá tesszük (writable+exec).
                "PROOT_TMP_DIR=${rootfs.prootTmpDir.absolutePath}",
                "TMPDIR=${rootfs.prootTmpDir.absolutePath}",
                "USER_HOME=/root",
                "TERM=xterm-256color",
                "LANG=C.UTF-8",
                "PATH=/system/bin:/system/xbin",
            )
            Log.i(tag, "TerminalSession létrehozása: launch=${rootfs.launchSh.absolutePath}")
            // KÖZVETLENÜL a launch.sh-t indítjuk shell-ként; a `#!/system/bin/sh`
            // shebang a kernelnek tudtul adja hogy sh-val futassa. Korábban
            // `shellPath=sh, args=[launch.sh]`-szel hívtunk, de az argv[0]-t
            // a launch.sh-ra állította, és sh interactive módban indult
            // ahelyett hogy a scriptet futtatta volna.
            val s = TerminalSession(
                /* shellPath      = */ rootfs.launchSh.absolutePath,
                /* cwd            = */ rootfs.bundleDir.absolutePath,
                /* args           = */ arrayOf(rootfs.launchSh.absolutePath),
                /* env            = */ env,
                /* transcriptRows = */ 5000,
                /* client         = */ dispatcher,
            )
            session = s
            return s
        }

        /**
         * Feltölti az LKL /proc mirror-t IO-szálról KÖTELEZŐ hívni —
         * Binder-bind-elés-szel és Thread.sleep-pel jár, ami main-szálon
         * deadlock-ot okozna. A `getOrCreateSession()` az itt feltöltött
         * `lklProcOsrelease`-t használja az env-be.
         */
        fun prepareLklMirror(rootfs: RootfsManager) {
            lklProcOsrelease = populateLklProcMirror(rootfs)
        }

        /**
         * UI-oldali kliens regisztrálása. A Service-en belül egy dispatcher
         * fogadja a Termux session-event-eket, és innen továbbítja a bind-elt
         * Activity-nek. Unbind-on `setListener(null)`-t hívunk.
         */
        fun setListener(c: TerminalSessionClient?) {
            listener = c
        }

        /** Kemény session-stop. Csak akkor hívd, ha biztos hogy nem kell tovább. */
        fun terminate() {
            session?.finishIfRunning()
            session = null
        }
    }

    /**
     * Service-szintű session-client. Minden Termux callback-et továbbít a
     * bind-elt UI listener-jére. Ha nincs UI bind-elve (pl. user a Home
     * képernyőn), az event-ek elvesznek — de a shell process tovább fut, és
     * a következő bind a transcript-et megkapja.
     */
    private val dispatcher = object : TerminalSessionClient {
        override fun onTextChanged(s: TerminalSession?) {
            listener?.onTextChanged(s)
        }
        override fun onTitleChanged(s: TerminalSession?) {
            listener?.onTitleChanged(s)
        }
        override fun onSessionFinished(s: TerminalSession?) {
            Log.i(tag, "session exited rc=${s?.exitStatus}")
            listener?.onSessionFinished(s)
            // session marker tisztítása — legközelebbi getOrCreateSession()
            // friss session-t fog adni.
            if (s === session) session = null
        }
        override fun onCopyTextToClipboard(s: TerminalSession?, text: String?) {
            listener?.onCopyTextToClipboard(s, text)
        }
        override fun onPasteTextFromClipboard(s: TerminalSession?) {
            listener?.onPasteTextFromClipboard(s)
        }
        override fun onBell(s: TerminalSession?) {
            listener?.onBell(s)
        }
        override fun onColorsChanged(s: TerminalSession?) {
            listener?.onColorsChanged(s)
        }
        override fun onTerminalCursorStateChange(state: Boolean) {
            listener?.onTerminalCursorStateChange(state)
        }
        override fun getTerminalCursorStyle(): Int =
            listener?.terminalCursorStyle ?: 0

        override fun logError(t: String?, m: String?)  { Log.e(t ?: tag, m ?: "") }
        override fun logWarn(t: String?,  m: String?)  { Log.w(t ?: tag, m ?: "") }
        override fun logInfo(t: String?,  m: String?)  { Log.i(t ?: tag, m ?: "") }
        override fun logDebug(t: String?, m: String?)  {}
        override fun logVerbose(t: String?, m: String?) {}
        override fun logStackTraceWithMessage(t: String?, m: String?, e: Exception?) {
            Log.e(t ?: tag, m ?: "", e)
        }
        override fun logStackTrace(t: String?, e: Exception?) {
            Log.e(t ?: tag, "", e)
        }
    }

    /** Az utolsó `prepareLklMirror()` által kiolvasott LKL osrelease string,
     *  vagy null ha a kernel nem volt elérhető. A `getOrCreateSession` ezt
     *  használja a `LKL_KERNEL_RELEASE` env-be. */
    @Volatile private var lklProcOsrelease: String? = null

    /** A LklService-Binder cache — a Service-szintű bind-elésen át. */
    @Volatile private var lklIface: ILklService? = null

    private val lklConn = object : android.content.ServiceConnection {
        override fun onServiceConnected(name: android.content.ComponentName?, b: IBinder?) {
            Log.i(tag, "LklService bind-elve (proc-mirror-hez)")
            lklIface = ILklService.Stub.asInterface(b)
        }
        override fun onServiceDisconnected(name: android.content.ComponentName?) {
            Log.i(tag, "LklService disconnect")
            lklIface = null
        }
    }

    /**
     * Az LKL kernel `/proc` fájljait kiírjuk a `rootfs.prootTmpDir/lkl-proc/`-ba,
     * hogy a launch.sh bind-mountolhassa a chroot /proc helyettesítőjeként.
     *
     * Bind-szinkronizáció: max 3 sec-ig várakozik a `:lkl` Service-bind-re,
     * majd lekérdezi a fájlokat. Ha nem sikerül, `null`-t ad vissza, és a
     * launch.sh fallback-el a host-/proc-ra (LKL_PROC_DIR üres lesz).
     *
     * @return LKL kernel `osrelease` stringe (pl. "5.18.0"), vagy `null`
     *   ha az LKL nem elérhető.
     */
    private fun populateLklProcMirror(rootfs: RootfsManager): String? {
        // Bind-el ha még nincs
        if (lklIface == null) {
            try {
                bindService(
                    Intent(this, LklService::class.java),
                    lklConn,
                    Context.BIND_AUTO_CREATE,
                )
            } catch (t: Throwable) {
                Log.w(tag, "LklService bind failed: ${t.message}")
                return null
            }
        }
        // Max 3 sec várakozás bind-ra
        val deadline = System.currentTimeMillis() + 3000
        while (lklIface == null && System.currentTimeMillis() < deadline) {
            Thread.sleep(100)
        }
        val iface = lklIface ?: run {
            Log.w(tag, "LKL bind timeout — fallback host-/proc-ra")
            return null
        }
        // Indítsd a kernelt ha még nem fut. A startKernel idempotens
        // (-EALREADY-t ad ha már fut).
        runCatching { iface.startKernel() }.onSuccess { rc ->
            if (rc == 0) {
                Log.i(tag, "LKL kernel boot — várok 500ms a sysfs init-re")
                Thread.sleep(500)
            } else if (rc != -114 /*EALREADY*/) {
                Log.w(tag, "lkl startKernel rc=$rc")
            }
        }

        val osrelease = runCatching { iface.readLklFile("/proc/sys/kernel/osrelease") }
            .getOrDefault("").trim()
        if (osrelease.isEmpty()) {
            Log.w(tag, "LKL osrelease üres — kernel nem ad /proc-fájlokat")
            return null
        }

        // Mirror-mappa: töröljük és újra-építjük
        val mirror = File(rootfs.prootTmpDir, "lkl-proc")
        mirror.deleteRecursively()
        mirror.mkdirs()

        // Standard /proc fájlok az LKL kernelből. Bővebb listával a chrooted
        // `ls /proc` az LKL-fa tartalmát mutatja, nem a host-Android-fájlokat.
        val procFiles = listOf(
            "version", "cpuinfo", "meminfo", "uptime", "stat",
            "loadavg", "filesystems", "mounts", "modules",
            "partitions", "swaps", "vmstat", "diskstats",
            "interrupts", "softirqs", "buddyinfo", "zoneinfo",
            "slabinfo", "iomem", "ioports", "cmdline", "consoles",
            "devices", "execdomains", "fb", "kallsyms", "key-users",
            "keys", "locks", "misc", "self/maps", "self/status",
            "self/stat", "self/cmdline", "self/comm", "self/cwd",
            "self/environ", "self/exe", "self/limits", "self/mountinfo",
            "self/mounts", "self/mountstats",
            "sys/kernel/osrelease",
            "sys/kernel/ostype",
            "sys/kernel/hostname",
            "sys/kernel/version",
            "sys/kernel/domainname",
            "sys/kernel/random/boot_id",
            "sys/kernel/random/uuid",
        )
        var hit = 0
        for (rel in procFiles) {
            val content = runCatching { iface.readLklFile("/proc/$rel") }.getOrDefault("")
            if (content.isNotEmpty()) {
                val out = File(mirror, rel)
                out.parentFile?.mkdirs()
                out.writeText(content)
                hit++
            }
        }
        // Egy üres `sys`, `bus`, `tty` mappa is — a /proc/sys, /proc/bus stb.
        // standard layout-jelei. (A chrooted bash sokszor csak directory-
        // létezést ellenőriz, nem a tartalmat.)
        listOf("sys/fs", "sys/net", "sys/vm", "bus", "tty", "fs", "net").forEach {
            File(mirror, it).mkdirs()
        }
        Log.i(tag, "LKL proc-mirror: $hit fájl kiírva, osrelease=$osrelease")
        return osrelease
    }

    override fun onBind(intent: Intent?): IBinder {
        Log.i(tag, "onBind")
        return LocalBinder()
    }

    override fun onDestroy() {
        Log.i(tag, "onDestroy — session terminate")
        try { session?.finishIfRunning() } catch (_: Throwable) {}
        session = null
        super.onDestroy()
    }
}
