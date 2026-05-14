package dev.hm.kaliterm

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat

private const val ACTION_USB_PERMISSION = "dev.hm.kaliterm.USB_PERMISSION"

/**
 * Egy konkrét USB eszköz UI-állapota.
 *
 * A `device` az Android `UsbDevice`, `granted` jelzi hogy van-e már
 * permission a felhasználótól. `attached` a natív bridge-hez adás
 * lokális állapota — Phase 2b.1-ben csak a JNI log-ja után igazra állítjuk.
 */
data class UsbDeviceState(
    val device: UsbDevice,
    val granted: Boolean,
    val attached: Boolean = false,
)

/**
 * Az UsbManager-rel beszél: eszközöket sorol fel, permission-t kér,
 * megnyitott file descriptor-t átad a natív rétegnek.
 *
 * Compose-natív állapotot vezet (`mutableStateListOf`), így a UI
 * recompose-ja automatikusan követi a változásokat.
 *
 * Életciklus: `start()`-ot kell hívni mielőtt a UI használja, és
 * `stop()`-ot kilépéskor a BroadcastReceiver leszedéséhez.
 * A Composable `rememberUsbController()` ezt automatikusan elvégzi
 * DisposableEffect-tel.
 */
class UsbController(private val context: Context) {

    private val usbManager: UsbManager =
        context.getSystemService(Context.USB_SERVICE) as UsbManager

    val devices = mutableStateListOf<UsbDeviceState>()
    val lastAttachLog = mutableStateOf<String?>(null)
    /** Az utolsó natív rétegtől visszakapott descriptor-diagnosztika, UI-ban mutatva. */
    val lastDescription = mutableStateOf<String?>(null)

    /** Ha fut a bridge, ennek az eszköznek a deviceId-je. Null = nem fut. */
    val activeBridgeDeviceId = mutableStateOf<Int?>(null)

    /** Friss `nativeBridgeStatus()` érték — Compose recompose-ra is használjuk. */
    val bridgeStatus = mutableStateOf("STOPPED")

    /**
     * A `UsbDeviceConnection` referenciát fenn kell tartanunk, amíg a bridge
     * fut — különben a GC bezárná, és az Android USB stack elengedné az
     * eszközt a libusb dup'd fd-je alól.
     */
    private var activeConnection: UsbDeviceConnection? = null

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            // Bármilyen USB esemény után újraszámoljuk a teljes listát —
            // a permission state és az eszközhalmaz egyaránt változhatott.
            refresh()
        }
    }

    fun start() {
        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        // Android 14+: minden runtime receiver-nek explicit exported flag
        ContextCompat.registerReceiver(
            context, receiver, filter,
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        refresh()
    }

    fun stop() {
        try { context.unregisterReceiver(receiver) } catch (_: Throwable) { /* idempotent */ }
    }

    fun refresh() {
        val now = usbManager.deviceList.values.map { d ->
            UsbDeviceState(device = d, granted = usbManager.hasPermission(d))
        }.sortedBy { it.device.deviceName }
        devices.clear()
        devices.addAll(now)
    }

    fun requestPermission(state: UsbDeviceState) {
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(context.packageName)
        val pi = PendingIntent.getBroadcast(
            context,
            state.device.deviceId,
            intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        usbManager.requestPermission(state.device, pi)
    }

    /**
     * Diagnosztikai probe — `nativeAcceptUsbDevice` libusb_wrap_sys_device-szal
     * leolvassa a descriptor-okat, majd elenged mindent. A bridge NEM marad
     * fent. Az eredmény logcat-ben és az UI `lastDescription` mezőjében.
     */
    fun attachToBridge(state: UsbDeviceState): Int {
        if (!state.granted) return -1
        val conn: UsbDeviceConnection = usbManager.openDevice(state.device) ?: return -2
        val fd = conn.fileDescriptor
        if (fd < 0) {
            conn.close()
            return -3
        }
        val rc = NativeBridge.nativeAcceptUsbDevice(
            fd = fd,
            vid = state.device.vendorId,
            pid = state.device.productId,
            busnum = state.device.deviceId ushr 16,
            devnum = state.device.deviceId and 0xFFFF,
        )
        lastAttachLog.value = "fd=$fd VID=%04x PID=%04x rc=$rc".format(
            state.device.vendorId, state.device.productId,
        )
        lastDescription.value = runCatching { NativeBridge.nativeLastDescription() }
            .getOrElse { "(natív rétegtől nem jött descriptor: ${it.message})" }
        conn.close()
        val idx = devices.indexOfFirst { it.device.deviceId == state.device.deviceId }
        if (idx >= 0) devices[idx] = state.copy(attached = (rc >= 0))
        return rc
    }

    /**
     * Beindítja a usb-bridge URB-dispatch worker-szálát erre az eszközre.
     * A `UsbDeviceConnection`-t megőrizzük amíg a bridge fut — így az
     * Android USB stack nem engedi el az eszközt.
     */
    fun startBridge(state: UsbDeviceState): Int {
        if (!state.granted) return -1
        if (activeBridgeDeviceId.value != null) return -16  // EBUSY
        val conn = usbManager.openDevice(state.device) ?: return -2
        val fd = conn.fileDescriptor
        if (fd < 0) { conn.close(); return -3 }

        val rc = NativeBridge.nativeStartBridge(
            fd = fd,
            vid = state.device.vendorId,
            pid = state.device.productId,
            busnum = state.device.deviceId ushr 16,
            devnum = state.device.deviceId and 0xFFFF,
        )
        if (rc < 0) {
            conn.close()
        } else {
            activeConnection = conn
            activeBridgeDeviceId.value = state.device.deviceId
        }
        bridgeStatus.value = runCatching { NativeBridge.nativeBridgeStatus() }
            .getOrDefault("UNKNOWN")
        lastDescription.value = runCatching { NativeBridge.nativeLastDescription() }
            .getOrElse { "(natív descriptor: ${it.message})" }
        return rc
    }

    fun stopBridge(): Int {
        val rc = runCatching { NativeBridge.nativeStopBridge() }.getOrDefault(-1)
        activeConnection?.close()
        activeConnection = null
        activeBridgeDeviceId.value = null
        bridgeStatus.value = runCatching { NativeBridge.nativeBridgeStatus() }
            .getOrDefault("UNKNOWN")
        return rc
    }
}

@Composable
fun rememberUsbController(): UsbController {
    val context = LocalContext.current
    val controller = remember(context) { UsbController(context.applicationContext) }
    DisposableEffect(controller) {
        controller.start()
        onDispose { controller.stop() }
    }
    return controller
}
