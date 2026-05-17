package dev.hm.kaliterm

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.Process
import android.util.Log
import java.io.File

/**
 * Az LKL kernelt egy SAJÁT OS process-ben (`android:process=":lkl"`) futtatjuk.
 *
 * Indok: az LKL upstream egy-shot kernel: `lkl_sys_halt` + `lkl_cleanup` után
 * a globális kernel-state (cmd_line, percpu, init_sem, …) irreverzibilisen
 * shutdown-ben. Új `lkl_init` ugyanabban a processzben crash-eli az appot.
 *
 * Megoldás: a Service külön process-ben él. **Stop = process death** →
 * az OS minden state-et felszabadít. **Start = új process spawn-ja** →
 * Android friss memóriaképpel hozza vissza a Service-t → fresh kernel.
 *
 * **Foreground service** — az Android csak bound service-t pillanatok alatt
 * kilövi (memory pressure, lifecycle, stb.). A :lkl process futnia kell
 * AMÍG a chrooted Kali shell aktív, mert a shim socket-en át hív LKL-syscallt.
 * A `startForeground()` persistent notification-nel életben tartja.
 *
 * Onnan, hogy a process indul, **autocont**: `onCreate` kontextusban
 *   1) `lkl_start_kernel()` (idempotens; -EALREADY ha már fut),
 *   2) `nativeLklStartControlSocket()` a hardcoded path-on.
 * Így minden ÚJ bind kész infrastructure-ral fogadja a klienst — nincs race
 * az UI-thread és a :lkl-init között.
 *
 * IPC: AIDL (`ILklService`) — szinkron metódushívások Binder-en át.
 */
class LklService : Service() {

    private val tag = "kaliterm-lkl-svc"

    /**
     * Control-socket path — a chrooted shim (`libkali_fuse_shim.so`) ezen át
     * kapcsolódik az LKL-hez. A path BOTH process-ben azonos (filesDir az
     * UID-szintű, közös az app minden process-ének).
     */
    private val controlSockPath: String by lazy {
        File(File(filesDir, "proot-tmp"), "lkl-control.sock").absolutePath
    }

    companion object {
        private const val NOTIF_CHANNEL_ID = "kaliterm-lkl"
        private const val NOTIF_ID = 4711
    }

    private val binder = object : ILklService.Stub() {

        override fun getStatus(): String =
            try {
                NativeBridge.nativeLklStatus()
            } catch (t: Throwable) {
                "ERROR getStatus: ${t.message}"
            }

        override fun startKernel(): Int =
            try {
                NativeBridge.nativeLklStart()
            } catch (t: Throwable) {
                Log.e(tag, "startKernel hiba", t)
                -1
            }

        /**
         * Halt + self-destruct — deadlock-rezisztens.
         *
         * Probléma: a `lkl_sys_halt` MEG TUD AKADNI, ha egy LKL kernel-thread
         * elakadt (pl. vhci_hcd egy hibás sockfd-re vár). Ha közvetlenül
         * hívnánk és blokkolunk rá, a `Process.killProcess` ütemezésig sem
         * jutunk → az egész :lkl process holtmaradna, a UI-n a Stop befagy.
         *
         * Megoldás:
         *   1) ELŐSZÖR ütemezünk egy feltétlen `killProcess`-t 300 ms múlva
         *      — ez akkor is megöli a process-t ha minden más bedöglik.
         *   2) Egy háttérszálon próbáljuk a `nativeLklStop()`-ot — ha
         *      sikerül a kill előtt, tiszta shutdown; ha nem, a kill úgy is
         *      megérkezik.
         *   3) A Binder hívás azonnal visszatér 0-val a kliensnek — nem
         *      blokkoljuk a fő process UI-szálát.
         *
         * A fő process az `onServiceDisconnected`-en érzékeli a process-halált.
         */
        override fun stopKernel(): Int {
            // Foreground state lecsatolva, hogy a notification eltűnjön a
            // kill előtt; ezzel egyúttal a STARTED-state is felszabadul.
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                    stopForeground(STOP_FOREGROUND_REMOVE)
                } else {
                    @Suppress("DEPRECATION")
                    stopForeground(true)
                }
            } catch (_: Throwable) {}
            // 1) Feltétlen self-destruct ütemezés — ez ÉL akkor is ha minden
            //    más bedöglik (halt deadlock, JNI crash, stb.).
            Handler(Looper.getMainLooper()).postDelayed({
                Log.w(tag, "Process.killProcess(myPid) — stop deadline")
                Process.killProcess(Process.myPid())
            }, 300)
            // 2) Háttér-halt próbálkozás. Ha végez < 300 ms-ben, tiszta
            //    shutdown; ha túllóg, a fenti timer úgyis megöli a procit.
            Thread({
                try {
                    val rc = NativeBridge.nativeLklStop()
                    Log.i(tag, "nativeLklStop rc=$rc (háttér)")
                } catch (t: Throwable) {
                    Log.e(tag, "nativeLklStop hiba (háttér)", t)
                }
            }, "lkl-halt").start()
            stopSelf()
            return 0
        }

        override fun attachUsbDevice(
            pfd: ParcelFileDescriptor,
            vid: Int, pid: Int, busnum: Int, devnum: Int,
        ): String {
            // ParcelFileDescriptor: a Binder marshalling automatikusan dup-ot
            // csinált a sender oldali fd-ből, és itt a `:lkl` process saját
            // fd-jét kapjuk. A natív még egy dup-ot csinál, így a libusb
            // saját fd-t kap, a PFD-ben tartottat lezárja a PFD `use`-ja.
            return try {
                pfd.use { p ->
                    NativeBridge.nativeLklAttachUsbDevice(
                        p.fd, vid, pid, busnum, devnum)
                }
            } catch (t: Throwable) {
                Log.e(tag, "attachUsbDevice hiba", t)
                "ERROR attachUsbDevice: ${t.message}"
            }
        }

        /**
         * Read-only fájl az LKL kernel fájlrendszeréből (lkl_sys_openat/read).
         * Empty stringgel tér vissza ha a kernel nem fut, vagy a fájl nem
         * létezik. A Binder-átvitel nagyfájlokra nem alkalmas (1MB-os
         * Parcel-limit), de `/proc` és `/sys` fájlok mind <64KB.
         */
        override fun readLklFile(path: String): String =
            try {
                NativeBridge.nativeLklReadFile(path)
            } catch (t: Throwable) {
                Log.e(tag, "readLklFile($path) hiba", t)
                ""
            }

        override fun listLklDir(path: String): String =
            try {
                NativeBridge.nativeLklListDir(path)
            } catch (t: Throwable) {
                Log.e(tag, "listLklDir($path) hiba", t)
                ""
            }

        override fun startLklControlSocket(path: String): Int =
            try {
                NativeBridge.nativeLklStartControlSocket(path)
            } catch (t: Throwable) {
                Log.e(tag, "startLklControlSocket($path) hiba", t)
                -1
            }
    }

    override fun onCreate() {
        super.onCreate()
        Log.i(tag, "onCreate (pid=${Process.myPid()})")
        // Foreground promote — ez az egyetlen mód, hogy az Android NE lője
        // ki a :lkl process-t bind-release-re vagy memory pressure-re. Az
        // app első Service-cycle-jánál kell hogy ott legyen, MIELŐTT az
        // ANR-watchdog 5 sec-en belül megöl.
        ensureNotificationChannel()
        try {
            startForeground(NOTIF_ID, buildNotification())
        } catch (t: Throwable) {
            Log.w(tag, "startForeground hiba: ${t.message}")
        }
        // Auto-init: kernel + control socket háttér-szálon (lkl_start_kernel
        // ~170ms, a control socket bind+listen pár ms). A Binder kliensek
        // ezzel kész infrastructure-t kapnak — nincs race kondíció.
        Thread({
            try {
                File(filesDir, "proot-tmp").mkdirs()
                val rcKernel = NativeBridge.nativeLklStart()
                Log.i(tag, "auto-init: kernel rc=$rcKernel")
                val rcSock = NativeBridge.nativeLklStartControlSocket(controlSockPath)
                Log.i(tag, "auto-init: control socket rc=$rcSock at $controlSockPath")
            } catch (t: Throwable) {
                Log.e(tag, "auto-init hiba", t)
            }
        }, "lkl-autoinit").start()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // START_STICKY: ha rendszer kilövi (pl. OOM-killer mégis), automatikusan
        // újraspawn-olja a Service-t. A bound-only path ezt NEM adja, csak a
        // started+foreground.
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder {
        Log.i(tag, "onBind a :lkl process-ben (pid=${Process.myPid()})")
        return binder
    }

    override fun onDestroy() {
        Log.i(tag, "onDestroy (pid=${Process.myPid()})")
        super.onDestroy()
    }

    private fun ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            if (nm.getNotificationChannel(NOTIF_CHANNEL_ID) == null) {
                val ch = NotificationChannel(
                    NOTIF_CHANNEL_ID,
                    "Kali shell háttérprocess",
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = "Az LKL kernel életben tartása a Kali shell alatt."
                    setShowBadge(false)
                }
                nm.createNotificationChannel(ch)
            }
        }
    }

    private fun buildNotification(): Notification {
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val piFlags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        else
            PendingIntent.FLAG_UPDATE_CURRENT
        val pi = PendingIntent.getActivity(this, 0, openIntent, piFlags)

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, NOTIF_CHANNEL_ID)
                .setContentTitle("Kali shell aktív")
                .setContentText("LKL kernel fut a háttérben (kernel-szintű USB/proc/sys)")
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentIntent(pi)
                .setOngoing(true)
                .build()
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
                .setContentTitle("Kali shell aktív")
                .setContentText("LKL kernel fut a háttérben")
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentIntent(pi)
                .setOngoing(true)
                .setPriority(Notification.PRIORITY_LOW)
                .build()
        }
    }
}
