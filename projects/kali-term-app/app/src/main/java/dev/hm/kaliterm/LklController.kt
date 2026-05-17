package dev.hm.kaliterm

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.Log
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext

/**
 * Az [LklService] (külön `:lkl` process) felé vezető Binder-kliens.
 *
 * Életciklus:
 *   - `bind()` indítja a kötést → Android spawn-ol egy fresh `:lkl` process-t,
 *     onServiceConnected-en visszakapjuk az [ILklService] proxyt.
 *   - `start()` → `iface.startKernel()` (szinkron Binder-hívás).
 *   - `stop()` → `iface.stopKernel()` — a service self-destruct-ol; a
 *     `onServiceDisconnected` callback érzékeli a process-halált, az
 *     `iface = null` lesz.
 *   - A következő `start()` automatikusan re-bind-el (ha disconnect után
 *     vagyunk) — új process, új kernel, friss state. Ez az egész
 *     architektúra lényege.
 */
class LklController(private val context: Context) {

    private val tag = "kaliterm-lkl-ctrl"

    val status = mutableStateOf("(nincs bind)")
    val connected = mutableStateOf(false)

    /**
     * Egy várakozó `start` művelet — ha a bind éppen folyamatban van (a
     * disconnect után indítva), a `start()` ezt true-ra állítja, és az
     * `onServiceConnected` automatikusan elindítja a kernelt.
     */
    private var pendingStart = false

    private var iface: ILklService? = null

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            Log.i(tag, "onServiceConnected: $name")
            KaliInitLog.add("lkl-ctrl", "onServiceConnected — :lkl Binder kész")
            iface = ILklService.Stub.asInterface(service)
            connected.value = (iface != null)
            refresh()
            if (pendingStart) {
                pendingStart = false
                val rc = runCatching { iface?.startKernel() ?: -100 }.getOrDefault(-1)
                Log.i(tag, "pending start lefutott, rc=$rc")
                refresh()
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            Log.i(tag, "onServiceDisconnected: $name")
            KaliInitLog.add("lkl-ctrl", "onServiceDisconnected — :lkl process meghalt!")
            iface = null
            connected.value = false
            status.value = "(:lkl process megszűnt — bind újrahúzva " +
                           "a következő Start-ra)"
            try { context.unbindService(this) } catch (_: Throwable) {}
        }
    }

    fun bind() {
        if (iface != null) return
        Log.i(tag, "bind() — :lkl process spawn-olása")
        KaliInitLog.add("lkl-ctrl", "bind() — startForegroundService + bindService")
        val intent = Intent(context, LklService::class.java)
        try {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        } catch (t: Throwable) {
            Log.w(tag, "startForegroundService failed: ${t.message}")
            KaliInitLog.add("lkl-ctrl", "startForegroundService HIBA: ${t.message}")
        }
        context.bindService(intent, conn, Context.BIND_AUTO_CREATE)
    }

    fun unbind() {
        try { context.unbindService(conn) } catch (_: Throwable) {}
        iface = null
        connected.value = false
    }

    /**
     * Indítja a kernelt. Ha még nincs bind (pl. a service halála után),
     * elindítja a bind-ot és pending Start-ot regisztrál — az
     * onServiceConnected automatikusan elindítja akkor.
     *
     * Visszaad: 0 = sikeres közvetlen start, -EAGAIN (-11) = pending
     * bind-ot indítottunk; a UI-nak frissítenie kell egy kicsit múlva.
     */
    fun start(): Int {
        val live = iface
        if (live != null) {
            val rc = runCatching { live.startKernel() }.getOrDefault(-1)
            refresh()
            return rc
        }
        pendingStart = true
        status.value = "(bind folyamatban — start a connection után automatikusan)"
        bind()
        return -11 // -EAGAIN — pending, várj
    }

    fun stop(): Int {
        val live = iface
        if (live == null) {
            return -107 // -ENOTCONN
        }
        val rc = runCatching { live.stopKernel() }.getOrDefault(-1)
        // A Service most self-destruct-ol; a `onServiceDisconnected` érzékeli
        // a process-halált, és átállítja a status-t. Addig is jelezzük:
        status.value = "(:lkl process kill folyamatban, halt rc=$rc — " +
                       "az onServiceDisconnected átállítja a state-et)"
        return rc
    }

    fun refresh() {
        val live = iface
        status.value = if (live != null) {
            runCatching { live.status }.getOrDefault("(getStatus hiba)")
        } else {
            "(nincs bind)"
        }
    }

    /**
     * USB eszköz attach a `:lkl` process-be.
     *
     * @param hostFd a UsbManager `openDevice().fileDescriptor` értéke.
     *   A ParcelFileDescriptor.fromFd() egy DUP-ot csinál belül, így
     *   a Java-oldali eredeti `UsbDeviceConnection` továbbra is owner.
     * @return diagnosztikai szöveg a service-től, vagy "(nincs bind)"
     */
    fun attachUsb(hostFd: Int, vid: Int, pid: Int, bus: Int, dev: Int): String {
        val live = iface ?: return "(nincs bind — Start után próbáld)"
        return try {
            ParcelFileDescriptor.fromFd(hostFd).use { pfd ->
                runCatching { live.attachUsbDevice(pfd, vid, pid, bus, dev) }
                    .getOrElse { "ERROR Binder hívás: ${it.message}" }
            }
        } catch (t: Throwable) {
            "ERROR PFD wrap: ${t.message}"
        }
    }

    /**
     * USB-bridge leállítása a `:lkl` process kernelében — `stopUsbBridge`
     * Binder-call. URB worker thread, vhci_hcd port detach, libusb cleanup.
     * Idempotens (0 = sikerült vagy már le van állítva).
     */
    fun stopUsbBridge(): Int {
        val live = iface ?: return 0  // nincs bind → semmi sem fut
        return runCatching { live.stopUsbBridge() }.getOrElse {
            Log.w(tag, "stopUsbBridge hiba: ${it.message}")
            -1
        }
    }

    /**
     * Phase 2c.5e — read-only fájl-tartalom az LKL kernel fájlrendszeréből.
     * Üres stringgel tér vissza ha:
     *   - nincs Binder bind a `:lkl` Service-hez (kernel nem boot-olt még)
     *   - a fájl nem létezik az LKL `/proc`-jában
     *   - Binder error (cross-process IPC fail)
     *
     * A KaliShellService hívja a session-create előtt, hogy egy
     * "LKL-proc-mirror" mappát készítsen a chroot bind-mountokhoz.
     */
    fun readLklFile(path: String): String {
        val live = iface ?: return ""
        return try {
            runCatching { live.readLklFile(path) }.getOrDefault("")
        } catch (t: Throwable) {
            Log.w(tag, "readLklFile($path) hiba: ${t.message}")
            ""
        }
    }
}

@Composable
fun rememberLklController(): LklController {
    val context = LocalContext.current
    val controller = remember(context) { LklController(context.applicationContext) }
    DisposableEffect(controller) {
        controller.bind()
        onDispose { controller.unbind() }
    }
    return controller
}
