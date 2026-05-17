package dev.hm.kaliterm

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.system.Os
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
            val lklSysDir  = File(rootfs.prootTmpDir, "lkl-sys")
            val lklDevDir  = File(rootfs.prootTmpDir, "lkl-dev")
            val env = arrayOf(
                "HOME=${rootfs.bundleDir.absolutePath}",
                "PREFIX=${rootfs.bundleDir.absolutePath}",
                "ROOTFS_DIR=${rootfs.rootfsDir.absolutePath}",
                "PROOT=${rootfs.nativeProot.absolutePath}",
                "LKL_KERNEL_RELEASE=$lklRelease",
                // LKL_*_DIR — a fent populate-elt mirror-mappák. A launch.sh
                // kötelezően ellenőrzi (`[ -d $DIR ]`); ha a tartalom megvan,
                // bind-mountolja a chrooted /proc, /sys, /dev-re. A user-él-
                // mény: `ls /proc`, `ls /sys`, `ls /dev` mind az LKL fáját
                // mutatja, NEM az Android-hostét.
                "LKL_PROC_DIR=${lklProcDir.absolutePath}",
                "LKL_SYS_DIR=${lklSysDir.absolutePath}",
                "LKL_DEV_DIR=${lklDevDir.absolutePath}",
                // LKL_CONTROL_SOCK — a `:lkl` process-en indított unix-socket
                // path-ja (host filesystem); a launch.sh bind-mountolja a
                // chrooted `/run/lkl-control.sock`-ra, és az LD_PRELOAD shim
                // ezen át hív LKL-syscallt.
                "LKL_CONTROL_SOCK=${File(rootfs.prootTmpDir, "lkl-control.sock").absolutePath}",
                // PHASE-pure-rootfs: host-injekciós mappa (shim, keyring,
                // usb.ids, resolv.conf) — a launch.sh bind-mountolja a
                // megfelelő chrootbeli path-okra. A Kali rootfs SZŰZ marad.
                "HOST_INJECTIONS_DIR=${rootfs.hostInjectionsDir.absolutePath}",
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
            Log.i(tag, "TerminalSession létrehozása (PHASE 3: shell a :lkl-ben)")
            KaliInitLog.add("shell-svc", "TerminalSession létrehozás START")
            val iface = lklIface
                ?: run {
                    KaliInitLog.add("shell-svc", "HIBA: lklIface null — bind nem futott le")
                    throw IllegalStateException("LklService nem bind-elt — populateLklProcMirror nem futott le?")
                }

            var pfd: android.os.ParcelFileDescriptor? = runCatching {
                iface.getCurrentShell()
            }.getOrNull()
            var shellPid = runCatching { iface.getCurrentShellPid() }.getOrDefault(0)

            if (pfd == null) {
                Log.i(tag, "új shell spawnolása a :lkl process-ben")
                KaliInitLog.add("shell-svc", "startKaliShell hívás (forkpty+execve a :lkl-ben)")
                val ts = System.currentTimeMillis()
                pfd = iface.startKaliShell(
                    /* shellPath = */ rootfs.launchSh.absolutePath,
                    /* cwd       = */ rootfs.bundleDir.absolutePath,
                    /* args      = */ arrayOf(rootfs.launchSh.absolutePath),
                    /* env       = */ env,
                    /* cols      = */ 80,
                    /* rows      = */ 24,
                ) ?: run {
                    KaliInitLog.add("shell-svc", "HIBA: startKaliShell null — forkpty fail")
                    throw IllegalStateException("startKaliShell visszatért null-lal — :lkl forkpty fail")
                }
                shellPid = iface.getCurrentShellPid()
                KaliInitLog.add("shell-svc", "shell spawn OK: pid=$shellPid (${System.currentTimeMillis()-ts}ms)")
                Log.i(tag, "shell spawn OK: pid=$shellPid (:lkl-ben), PTY-fd dup-olva main-be")
            } else {
                Log.i(tag, "reconnect a meglévő :lkl-shell-hez: pid=$shellPid")
                KaliInitLog.add("shell-svc", "reconnect meglévő :lkl-shell-hez (pid=$shellPid)")
            }

            val masterFd = pfd.detachFd()  // PFD ownership-transfer a TerminalSession-höz
            val s = TerminalSession(
                /* externalPtyFd  = */ masterFd,
                /* externalPid    = */ shellPid,
                /* transcriptRows = */ 5000,
                /* client         = */ dispatcher,
            )
            session = s
            return s
        }

        /**
         * Rekurzív LKL-fájlfa mirror: pl. `/sys/bus/usb/devices/` minden
         * sub-directory-jét és fájlját átmásolja a chrooted-mirror-be.
         * A symlinkeket (subsystem/driver/module/of_node) skipeli a
         * sysfs-circular-loop ellen.
         */
        // -- delegál a Service-szintű private helperre (alább) --

        /**
         * Feltölti az LKL /proc mirror-t IO-szálról KÖTELEZŐ hívni —
         * Binder-bind-elés-szel és Thread.sleep-pel jár, ami main-szálon
         * deadlock-ot okozna. A `getOrCreateSession()` az itt feltöltött
         * `lklProcOsrelease`-t használja az env-be.
         *
         * @Synchronized: a LaunchedEffect-cancel/restart 2x indíthatja
         * párhuzamosan; a synchronized csak az ELSŐ indítást engedi, a
         * második wait-tel az első eredményére.
         */
        @Synchronized
        fun prepareLklMirror(rootfs: RootfsManager) {
            // Cache-fix: a LaunchedEffect(binder, ready) kétszer triggerelődhet
            // (binder és ready változás). Ha már sikerült populálni, NE
            // walkoljuk újra az LKL-t — 600ms-1s felesleges munka lenne.
            // FONTOS: a foreground service is megtarthat egy elavult cache-t
            // egy APK-upgrade UTÁN, ha a service nem destroy-olódott. Ezért
            // a sentinel-fájl content-check ha a kritikus usb1/busnum üres,
            // ÚJRA-RUN-oljuk a populate-et.
            //
            // Cache-check most a populateLklProcMirror-ban van (a bind után),
            // hogy a live LKL-state vs cached materialize-t össze tudjuk
            // hasonlítani — különben a USB-attach-detach változások nem
            // detektálódnának, és a stale cache miatt a chrooted lsusb a
            // régi device-set-et látná.
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
    /**
     * Rekurzív walk az LKL fájlrendszerén — minden file-jét és sub-directory-jét
     * átmásolja a mirror-fába azonos relative-path-szerkezettel.
     *
     * Skip-szabályok (perf-szempontból kritikus, mert 1 Binder-call/file
     * az UI-szálat blokkolná):
     *   - All-digit nevek (e.g. /proc/1, /proc/123) — process-mappák
     *   - "slab", "cache" — /sys/kernel/slab/<NAME>/{NAME} kvázi-explosion
     *   - sysfs circular-link entries
     *   - per-directory cap (max 30 entry) — anomálisan nagy mappákat skipel
     */
    private fun recursivelyMirrorLklTree(
        iface: ILklService,
        lklRoot: String,
        lklPath: String,
        mirrorRoot: File,
        maxDepth: Int,
        skipNames: Set<String> = DEFAULT_SKIP_NAMES,
    ): Int {
        if (maxDepth <= 0) return 0
        val entries = runCatching { iface.listLklDir(lklPath) }.getOrDefault("")
            .lines().filter { it.isNotBlank() }
        if (entries.isEmpty()) return 0
        var count = 0
        // Per-directory entry-cap — szélsőséges esetekben (/sys/kernel/slab,
        // /sys/devices/system/cpu/cpu*) ezrek a entry-k vannak. 30 felett
        // skipelünk: a "fejlécfájlokat" kapja meg a chrooted, a részletes
        // attribútumokat NEM.
        val MAX_ENTRIES_PER_DIR = 30
        val takenEntries = entries.take(MAX_ENTRIES_PER_DIR)
        if (entries.size > MAX_ENTRIES_PER_DIR) {
            Log.d(tag, "skip ${entries.size - MAX_ENTRIES_PER_DIR} extra entries in $lklPath")
        }
        for (entry in takenEntries) {
            val name = entry.trim()
            if (name == "." || name == "..") continue
            if (name in skipNames) continue
            // All-digit nevek: process-mappák /proc-en, slab-cache-számok stb.
            if (name.all { it.isDigit() }) continue
            val childLklPath = "$lklPath/$name"
            val rel = childLklPath.removePrefix("$lklRoot/").removePrefix("/")
            val out = File(mirrorRoot, rel)
            val content = runCatching { iface.readLklFile(childLklPath) }.getOrDefault("")
            if (content.isNotEmpty()) {
                out.parentFile?.mkdirs()
                runCatching { out.writeText(content) }
                count++
            } else {
                val sub = runCatching { iface.listLklDir(childLklPath) }.getOrDefault("")
                if (sub.isNotBlank()) {
                    out.mkdirs()
                    count += recursivelyMirrorLklTree(
                        iface, lklRoot, childLklPath, mirrorRoot, maxDepth - 1, skipNames
                    )
                } else {
                    out.parentFile?.mkdirs()
                    runCatching { out.writeText("") }
                    count++
                }
            }
        }
        return count
    }

    // Companion object alább a fájl végén (merge-elve a NOTIF_* konstansokkal).

    private fun populateLklProcMirror(rootfs: RootfsManager): String? {
        KaliInitLog.add("shell-svc", "populateLklProcMirror START")
        // Bind-el ha még nincs. ELŐSZÖR startForegroundService — ezzel STARTED
        // állapotba kerül a :lkl, és foreground-notification-nel él tovább
        // akkor is, ha minden bind elengedjük. Csak ezután bind-elünk a
        // Binder-cache-hez. (Korábban csak BIND_AUTO_CREATE volt → :lkl 3-5
        // sec után GC-killed, a chrooted shim halott socketre kapcsolódott.)
        if (lklIface == null) {
            try {
                val intent = Intent(this, LklService::class.java)
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                    startForegroundService(intent)
                } else {
                    startService(intent)
                }
                bindService(intent, lklConn, Context.BIND_AUTO_CREATE)
                KaliInitLog.add("shell-svc", "LklService start+bind initiated")
            } catch (t: Throwable) {
                Log.w(tag, "LklService start/bind failed: ${t.message}")
                KaliInitLog.add("shell-svc", "LklService start/bind HIBA: ${t.message}")
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
            KaliInitLog.add("shell-svc", "LKL bind TIMEOUT (3 sec) — fallback host /proc-ra")
            return null
        }
        KaliInitLog.add("shell-svc", "LklService bind OK (${System.currentTimeMillis() - (deadline - 3000)}ms)")
        // Indítsd a kernelt ha még nem fut. A startKernel idempotens
        // (-EALREADY-t ad ha már fut).
        val t0 = System.currentTimeMillis()
        runCatching { iface.startKernel() }.onSuccess { rc ->
            KaliInitLog.add("shell-svc", "startKernel rc=$rc (${System.currentTimeMillis()-t0}ms)")
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
            KaliInitLog.add("shell-svc", "osrelease ÜRES — LKL kernel nem ad /proc-ot")
            return null
        }
        KaliInitLog.add("shell-svc", "LKL osrelease = $osrelease")

        // CACHE-CHECK (bind után, materialize előtt) — ha a cached materialize
        // még érvényes (file-integrity + LIVE USB-list match), SKIP a teljes
        // walk-ot. Ezzel USB-attach detect-en MINDIG re-populate-elünk, anélkül
        // hogy a változatlan-set esetén feleslegesen 300ms+ walkolnánk.
        // KIVÉTEL: ha `mirrorCacheDirty` flag set (pl. USB unplug után az
        // UsbController-ből), kényszerű re-walk.
        if (mirrorCacheDirty) {
            KaliInitLog.add("shell-svc", "mirrorCacheDirty=true → force re-walk")
            mirrorCacheDirty = false
        } else if (lklProcOsrelease != null) {
            val sUsbBusnum  = File(rootfs.prootTmpDir, "lkl-sys/bus/usb/devices/usb1/busnum")
            val sUevent     = File(rootfs.prootTmpDir, "lkl-sys/bus/usb/devices/usb1/uevent")
            val sSubsystem  = File(rootfs.prootTmpDir, "lkl-sys/bus/usb/devices/usb1/subsystem")
            val ssIsLink    = try {
                java.nio.file.Files.isSymbolicLink(sSubsystem.toPath())
            } catch (_: Throwable) { false }

            // Cheap Binder hívás: az LKL aktuális /sys/bus/usb/devices listája
            val liveUsbDevs = runCatching {
                iface.listLklDir("/sys/bus/usb/devices")
            }.getOrDefault("").lines()
                .map { it.trim() }
                .filter { it.isNotEmpty() }
                .sorted()
            val cachedUsbDevs = File(rootfs.prootTmpDir, "lkl-sys/bus/usb/devices")
                .list()?.toList()?.sorted() ?: emptyList()

            val integrityOk = sUsbBusnum.exists() && sUsbBusnum.length() > 0
                          && sUevent.exists()   && sUevent.length()   > 0
                          && ssIsLink
            val usbListSame = liveUsbDevs.isNotEmpty() && cachedUsbDevs == liveUsbDevs

            if (integrityOk && usbListSame) {
                KaliInitLog.add("shell-svc", "cache OK (USB-list match: ${liveUsbDevs.size} entry) — SKIP walk")
                return osrelease  // a cached materialize jó, NEM walkolunk újra
            }
            val why = when {
                !integrityOk -> "integrity-check fail (sentinel hiány)"
                !usbListSame -> "USB device-set változott (cached=$cachedUsbDevs live=$liveUsbDevs)"
                else -> "ismeretlen"
            }
            KaliInitLog.add("shell-svc", "cache INVALID ($why) — full RE-POPULATE")
        }

        // PHASE 4 — BULK MATERIALIZE: a /proc /sys /dev fát EGY Binder-
        // hívásban szerializálva lekérjük az LKL-től, és valódi disk-
        // fájlokként kiírjuk. A proot ezeket bind-mountolja. Minden
        // libc-syscall valódi fd-vel megy → nincs dirfd-assertion-bug,
        // nincs shim-hang, nincs LD_PRELOAD-routing-overhead.
        val procMirror = File(rootfs.prootTmpDir, "lkl-proc")
        val sysMirror  = File(rootfs.prootTmpDir, "lkl-sys")
        val devMirror  = File(rootfs.prootTmpDir, "lkl-dev")
        procMirror.deleteRecursively(); procMirror.mkdirs()
        sysMirror.deleteRecursively();  sysMirror.mkdirs()
        devMirror.deleteRecursively();  devMirror.mkdirs()

        var tt = System.currentTimeMillis()
        val procHit = materializeLklTree(iface, "/proc", procMirror, maxDepth = 4, maxPerDir = 64)
        KaliInitLog.add("shell-svc", "/proc materalizálva: $procHit entry (${System.currentTimeMillis()-tt}ms)")
        tt = System.currentTimeMillis()
        // /sys: maxDepth=6 hogy a USB device-attribútumokat (busnum, devnum,
        // idVendor, idProduct, manufacturer, …) is beolvassuk. Pl. a path
        // /sys/bus/usb/devices/usb1/busnum 5 szinten van, /sys/devices/
        // platform/vhci_hcd.0/usb1/1-1/idVendor pedig 6 szinten.
        val sysHit  = materializeLklTree(iface, "/sys",  sysMirror,  maxDepth = 6, maxPerDir = 80)
        KaliInitLog.add("shell-svc", "/sys materalizálva: $sysHit entry (${System.currentTimeMillis()-tt}ms)")
        tt = System.currentTimeMillis()
        val devHit  = materializeLklTree(iface, "/dev",  devMirror,  maxDepth = 3, maxPerDir = 64)
        KaliInitLog.add("shell-svc", "/dev materalizálva: $devHit entry (${System.currentTimeMillis()-tt}ms)")

        // libusb 'sysfs not mounted' fix — explicit garantáljuk a kritikus
        // /sys/bus/usb/devices és /sys/class/usbmisc directory létezését.
        listOf("bus/usb/devices", "bus/usb/drivers", "bus/hid/devices",
               "class/usb", "class/usbmisc", "class/hidraw", "class/tty",
               "class/net", "class/input", "class/block",
               "devices/virtual").forEach {
            File(sysMirror, it).mkdirs()
        }

        // libudev device discovery — real-sysfs struktúrát kell utánoznunk:
        //
        // Real sysfs:
        //   /sys/bus/usb/devices/usbN       SYMLINK → ../../../devices/platform/vhci_hcd.0/usbN
        //   /sys/devices/platform/vhci_hcd.0/usbN/subsystem   SYMLINK → ../../../../bus/usb
        //   /sys/devices/platform/vhci_hcd.0/usbN/{busnum,devnum,uevent,...}  attribútumok
        //
        // Sorrend: ELŐSZÖR subsystem-link a kanonikus path-ba, AZTÁN
        // konvertáljuk az /sys/bus/usb/devices/usbN-t symlinkre a
        // kanonikusra. (Fordított sorrendben a subsystem-link törlődne
        // a regular-dir delete-rel.)
        try {
            val canonicalParent = File(sysMirror, "devices/platform/vhci_hcd.0")
            // (1) Subsystem symlinkek a kanonikus device-dir-ekben:
            //     /sys/devices/platform/vhci_hcd.0/usbN/subsystem → ../../../../bus/usb
            canonicalParent.listFiles()?.forEach { canDir ->
                if (!canDir.isDirectory) return@forEach
                val name = canDir.name
                if (!name.startsWith("usb")) return@forEach
                val ssLink = File(canDir, "subsystem")
                if (!ssLink.exists()) {
                    try {
                        Os.symlink("../../../../bus/usb", ssLink.absolutePath)
                    } catch (t: Throwable) {
                        Log.w(tag, "canonical subsystem symlink fail ($name): ${t.message}")
                    }
                }
            }

            // (2) Az /sys/bus/usb/devices/usbN entry-eket konvertáljuk
            //     symlinkre, hogy a libudev lstat-ja S_ISLNK == true-t adjon.
            val busDevicesDir = File(sysMirror, "bus/usb/devices")
            busDevicesDir.listFiles()?.forEach { devDir ->
                val name = devDir.name
                if (!name.startsWith("usb") || ':' in name) return@forEach
                val canDir = File(canonicalParent, name)
                if (canDir.isDirectory && devDir.isDirectory &&
                    !java.nio.file.Files.isSymbolicLink(devDir.toPath()))
                {
                    // Töröljük a regular dir-t (a tartalom megvan a kanonikus path-on)
                    devDir.deleteRecursively()
                    try {
                        // /sys/bus/usb/devices/usbN → ../../../devices/platform/vhci_hcd.0/usbN
                        Os.symlink("../../../devices/platform/vhci_hcd.0/$name", devDir.absolutePath)
                        KaliInitLog.add("usb-dev", "$name → symlink → vhci_hcd.0/$name")
                    } catch (t: Throwable) {
                        Log.w(tag, "device symlink fail ($name): ${t.message}")
                    }
                }
            }
        } catch (t: Throwable) {
            Log.w(tag, "sysfs symlink batch error: ${t.message}")
        }

        // DIAG: pontosan megmutatja, hogy az LKL-ben valóban mi van.
        // A user kérése: a root hub-okat (Linux Foundation 2.0/3.0) látnia
        // kell még csatlakoztatott USB nélkül is, ha a vhci_hcd működik.
        fun dumpDiag(name: String, mirror: File, relPath: String) {
            val f = File(mirror, relPath)
            val entries = f.list()?.toList() ?: emptyList()
            val preview = entries.take(10).joinToString(", ")
                .let { if (entries.size > 10) "$it, …" else it }
            KaliInitLog.add("diag", "$name (${entries.size}): $preview")
        }
        dumpDiag("/sys/bus/usb/devices",   sysMirror, "bus/usb/devices")
        dumpDiag("/sys/devices/platform",  sysMirror, "devices/platform")
        dumpDiag("/sys/class/usb",         sysMirror, "class/usb")
        dumpDiag("/sys/class/usbmisc",     sysMirror, "class/usbmisc")
        dumpDiag("/dev (LKL devtmpfs top)",devMirror, "")
        // Ha vhci_hcd betöltődött, ennek a path-nak léteznie kell
        File(sysMirror, "devices/platform/vhci_hcd.0").list()?.let {
            KaliInitLog.add("diag", "vhci_hcd.0 (${it.size}): ${it.take(15).joinToString(", ")}")
        } ?: KaliInitLog.add("diag", "vhci_hcd.0 NINCS — vhci_hcd init nem futott le?")

        // Byte-szintű ellenőrzés: a kritikus sysfs-attribútumokat az lsusb
        // VALÓBAN olvassa. Ha üres a fájl, lsusb skip-eli a device-t.
        fun dumpFile(label: String, rel: String) {
            val f = File(sysMirror, rel)
            val content = if (f.exists()) f.readText().trim().take(60) else "(missing)"
            KaliInitLog.add("diag", "$label = '$content' (${f.length()}B)")
        }
        dumpFile("usb1/busnum",      "bus/usb/devices/usb1/busnum")
        dumpFile("usb1/devnum",      "bus/usb/devices/usb1/devnum")
        dumpFile("usb1/idVendor",    "bus/usb/devices/usb1/idVendor")
        dumpFile("usb1/idProduct",   "bus/usb/devices/usb1/idProduct")
        dumpFile("usb1/descriptors", "bus/usb/devices/usb1/descriptors")
        dumpFile("usb1/uevent",      "bus/usb/devices/usb1/uevent")
        dumpFile("usb2/uevent",      "bus/usb/devices/usb2/uevent")
        dumpFile("1-1/busnum",       "bus/usb/devices/1-1/busnum")
        dumpFile("1-1/idVendor",     "bus/usb/devices/1-1/idVendor")
        // /sys/bus/usb/devices/usb1 stub — proot bind később (csak ha LKL
        // valódi root-hub-bejegyzéssel rendelkezik).

        // libusb 'sysfs not mounted' fix — statfs(/sys) MAGIC-check (SYSFS_MAGIC
         // = 0x62656572) failel a bind-mountolt ext4 dirre. libusb fallback-el
         // usbfs-re, ami a /dev/bus/usb/B/D fájlokból olvas 18 byte USB device
         // descriptort. Ezért MOST KIÍRJUK az igazi descriptor byte-okat
         // a placeholder fájlokba (a /sys/bus/usb/devices/X/descriptors-ből).
        try {
            val usbDevs = File(sysMirror, "bus/usb/devices").listFiles()
            usbDevs?.forEach { devDir ->
                val name = devDir.name
                if (':' in name) return@forEach  // interface (skip)
                val busnum  = File(devDir, "busnum") .takeIf { it.exists() }?.readText()?.trim().orEmpty()
                val devnum  = File(devDir, "devnum") .takeIf { it.exists() }?.readText()?.trim().orEmpty()
                val descFile = File(devDir, "descriptors")
                if (busnum.isNotEmpty() && devnum.isNotEmpty() && descFile.exists()) {
                    val bus3 = busnum.padStart(3, '0')
                    val dev3 = devnum.padStart(3, '0')
                    val node = File(devMirror, "bus/usb/$bus3/$dev3")
                    node.parentFile?.mkdirs()
                    runCatching {
                        // Másoljuk át a descriptors raw byte-okat — libusb
                        // usbfs-fallback exact-18-byte read-jét kielégíti.
                        node.writeBytes(descFile.readBytes())
                        Os.chmod(node.absolutePath, "644".toInt(8))
                    }
                    KaliInitLog.add("usb-dev", "/dev/bus/usb/$bus3/$dev3 ← descriptors (${descFile.length()}B)")
                }
            }
        } catch (t: Throwable) {
            Log.w(tag, "/dev/bus/usb descriptor-copy error: ${t.message}")
            KaliInitLog.add("usb-dev", "HIBA: descriptor copy: ${t.message}")
        }

        // /proc/mounts standard layout — libusb 'sysfs not mounted' fix
        try {
            File(procMirror, "mounts").writeText(
                "rootfs / rootfs rw 0 0\n" +
                "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n" +
                "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n" +
                "devtmpfs /dev devtmpfs rw,nosuid 0 0\n" +
                "devpts /dev/pts devpts rw,nosuid,noexec,relatime 0 0\n"
            )
            File(procMirror, "self").mkdirs()
            File(procMirror, "self/mounts").writeText(File(procMirror, "mounts").readText())
        } catch (t: Throwable) {
            Log.w(tag, "/proc/mounts write error: ${t.message}")
        }

        // A control-socket-et megtartjuk (a netlink-fake-elt libusb miatt
        // még szükséges a shim-réteg, és ahhoz a control-socket be van
        // huzalozva), de a file-op-okra már NEM hagyatkozunk rá.
        val ctrlSockPath = File(rootfs.prootTmpDir, "lkl-control.sock").absolutePath
        try {
            val rc = iface.startLklControlSocket(ctrlSockPath)
            if (rc == 0) Log.i(tag, "LKL control socket at $ctrlSockPath")
            else Log.w(tag, "startLklControlSocket rc=$rc")
            KaliInitLog.add("shell-svc", "control-socket rc=$rc")
        } catch (t: Throwable) {
            Log.w(tag, "startLklControlSocket failed: ${t.message}")
            KaliInitLog.add("shell-svc", "control-socket HIBA: ${t.message}")
        }

        Log.i(tag, "populateLklProcMirror DONE — return osrelease=$osrelease")
        KaliInitLog.add("shell-svc", "populateLklProcMirror DONE")
        return osrelease
    }

    /**
     * PHASE 4 — bulk Binder + parse + materialize.
     * Egyetlen `readLklTree(root)` hívás visszaadja a teljes /proc vagy /sys
     * fát szerializálva, amit itt parse-olunk és valódi diszk-fájlokként
     * kiírunk.
     *
     * Formátum (lásd lkl_runtime.c nativeLklReadTree):
     *   'D\n' <path> '\n'                            — directory
     *   'F\n' <path> '\n' <size> '\n' <bytes> '\n'   — file
     *   'E\n'                                        — end
     */
    private fun materializeLklTree(
        iface: ILklService, root: String, outRoot: File,
        maxDepth: Int, maxPerDir: Int,
    ): Int {
        val buf: ByteArray = runCatching {
            iface.readLklTree(root, maxDepth, 8 * 1024 * 1024, maxPerDir)
        }.getOrNull() ?: return 0

        var pos = 0
        var count = 0
        fun readByte(): Byte = buf[pos++]
        fun readUntilNewline(): String {
            val start = pos
            while (pos < buf.size && buf[pos] != '\n'.code.toByte()) pos++
            val s = String(buf, start, pos - start, Charsets.UTF_8)
            if (pos < buf.size) pos++  // skip '\n'
            return s
        }

        while (pos < buf.size) {
            val t = readByte()
            if (pos >= buf.size || buf[pos] != '\n'.code.toByte()) {
                Log.w(tag, "materializeLklTree($root): malformed at pos=$pos")
                break
            }
            pos++  // skip '\n' after type
            when (t) {
                'D'.code.toByte() -> {
                    val path = readUntilNewline()
                    val rel = path.removePrefix(root).removePrefix("/")
                    val out = if (rel.isEmpty()) outRoot else File(outRoot, rel)
                    out.mkdirs()
                    count++
                }
                'F'.code.toByte() -> {
                    val path = readUntilNewline()
                    val size = readUntilNewline().toIntOrNull() ?: 0
                    if (pos + size > buf.size) {
                        Log.w(tag, "materializeLklTree: truncated at $path size=$size")
                        break
                    }
                    val rel = path.removePrefix(root).removePrefix("/")
                    val out = if (rel.isEmpty()) outRoot else File(outRoot, rel)
                    out.parentFile?.mkdirs()
                    runCatching {
                        out.outputStream().use { it.write(buf, pos, size) }
                    }
                    pos += size
                    if (pos < buf.size && buf[pos] == '\n'.code.toByte()) pos++
                    count++
                }
                'E'.code.toByte() -> return count
                else -> {
                    Log.w(tag, "materializeLklTree: unknown type=${t.toInt().toChar()} at pos=$pos")
                    break
                }
            }
        }
        return count
    }

    override fun onCreate() {
        super.onCreate()
        Log.i(tag, "onCreate")
        // Foreground promote — Samsung BBA (Background Activity Auto-Control)
        // aggresszív killer-rel a main process pár sec alatt meghal a háttérben.
        // KaliShellService nélkül a TerminalSession (bash process) is dies a main
        // halálával, és a user a back-előrelépésnél új shell-t kell hogy lásson
        // (state-vesztés). Ezzel viszont a service és benne a session is él
        // amíg explicit Stop nincs.
        ensureNotificationChannel()
        try {
            startForeground(NOTIF_ID, buildNotification())
        } catch (t: Throwable) {
            Log.w(tag, "startForeground hiba: ${t.message}")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder {
        Log.i(tag, "onBind")
        return LocalBinder()
    }

    override fun onDestroy() {
        Log.i(tag, "onDestroy — PHASE3: shell a :lkl-ben él tovább, NEM killing")
        // KRITIKUS leak-fix: a populateLklProcMirror által bindelt lklConn-t
        // unbind-elni KELL itt, különben ServiceConnectionLeaked exception
        // a logcat-ban + Android tracking-resource holding.
        if (lklIface != null) {
            try { unbindService(lklConn) } catch (t: Throwable) {
                Log.w(tag, "lklConn unbind hiba: ${t.message}")
            }
            lklIface = null
        }
        // PHASE 3: a shell (bash) NEM ebben a process-ben fut, hanem :lkl-ben.
        // A finishIfRunning() Os.kill(mShellPid)-ot hív, ami egy másik process
        // gyermekére EPERM-mel failel — ártalmatlan. A PTY master fd duplikátum-
        // ját itt a kernel autom. zárja a process-halálban. Az igazi shell
        // tovább él a `:lkl`-ben, és a new-main reconnect-helhet rá.
        session = null
        super.onDestroy()
    }

    private fun ensureNotificationChannel() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as android.app.NotificationManager
            if (nm.getNotificationChannel(NOTIF_CHANNEL_ID) == null) {
                val ch = android.app.NotificationChannel(
                    NOTIF_CHANNEL_ID,
                    "Kali shell session",
                    android.app.NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = "Az aktív Kali shell session életben tartása."
                    setShowBadge(false)
                }
                nm.createNotificationChannel(ch)
            }
        }
    }

    private fun buildNotification(): android.app.Notification {
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val piFlags = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M)
            android.app.PendingIntent.FLAG_IMMUTABLE or android.app.PendingIntent.FLAG_UPDATE_CURRENT
        else
            android.app.PendingIntent.FLAG_UPDATE_CURRENT
        val pi = android.app.PendingIntent.getActivity(this, 1, openIntent, piFlags)

        return if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            android.app.Notification.Builder(this, NOTIF_CHANNEL_ID)
                .setContentTitle("Kali shell session")
                .setContentText("Bash session él a háttérben (proot+LKL)")
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentIntent(pi)
                .setOngoing(true)
                .build()
        } else {
            @Suppress("DEPRECATION")
            android.app.Notification.Builder(this)
                .setContentTitle("Kali shell session")
                .setContentText("Bash session él a háttérben")
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentIntent(pi)
                .setOngoing(true)
                .setPriority(android.app.Notification.PRIORITY_LOW)
                .build()
        }
    }

    companion object {
        private const val NOTIF_CHANNEL_ID = "kaliterm-shell"
        private const val NOTIF_ID = 4712

        /** Globális flag a /proc + /sys mirror cache invalidáláshoz.
         *  Az `UsbController.stopBridge()` ezt true-ra állítja amikor egy
         *  USB-eszközt detach-olunk; a következő `populateLklProcMirror`
         *  hívás full re-walkot végez (cache-check skip). */
        @Volatile var mirrorCacheDirty: Boolean = false

        private val DEFAULT_SKIP_NAMES = setOf(
            // sysfs symlink-loop
            "subsystem", "driver", "module", "of_node",
            // /proc/<PID>-szintű link-ek
            "cwd", "exe", "root", "fd", "fdinfo", "task",
            // /sys/kernel/slab — több ezer slab-cache, fölösleges chrootnak
            "slab", "cache",
            // /sys/devices/*/power — runtime-PM állapotok, kvázi-explosion
            "power",
            // /sys/firmware/efi — UEFI-state, nem releváns
            "efi",
            // /proc-szinten thread-self és self host-overlay-jel kezelve
            "self", "thread-self",
        )
    }
}
