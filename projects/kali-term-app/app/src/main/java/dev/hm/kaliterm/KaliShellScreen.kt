package dev.hm.kaliterm

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.util.Log
import android.view.inputmethod.InputMethodManager
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.termux.terminal.TerminalSession
import com.termux.terminal.TerminalSessionClient
import com.termux.view.TerminalView
import com.termux.view.TerminalViewClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Kali shell képernyő — Termux `TermuxActivity` mintára.
 *
 * Lifecycle:
 *   1) Compose belép → bind-elünk a `KaliShellService`-hez (BIND_AUTO_CREATE).
 *   2) Service connected → `getOrCreateSession(rootfs)` egy idempotens
 *      `TerminalSession`-t ad (a Service tartja). Regisztrálunk egy
 *      `TerminalSessionClient`-et ami az event-eket a `TerminalView`-re
 *      forwardolja — KRITIKUS: `onTextChanged → terminalView.onScreenUpdated()`,
 *      enélkül a shell ír de a View sosem rajzol újra.
 *   3) `TerminalView.attachSession(s)` → `onSizeChanged` → `updateSize` →
 *      `initializeEmulator(cols,rows)` → JNI.createSubprocess → shell forkolva.
 *   4) Compose kilép → unbind, listener=null. A session a Service-ben tovább
 *      él (de senki nem kapja az event-eket — egészen a következő bind-ig).
 *
 * UI réteg:
 *   - status sor felül
 *   - TerminalView középen (tap → soft keyboard popup, kötelező focusable)
 *   - extra-keys row alul (Esc, Tab, Ctrl, ↑↓←→, HOME, END, PgUp, PgDn, …)
 */
@Composable
fun KaliShellScreen(onBack: () -> Unit = {}) {
    val ctx = LocalContext.current
    val rootfs = remember { RootfsManager(ctx) }

    var status by remember { mutableStateOf(
        if (rootfs.isReady()) "rootfs kész — service-bind…" else "rootfs nincs kicsomagolva"
    ) }
    var ready by remember { mutableStateOf(rootfs.isReady()) }
    var session by remember { mutableStateOf<TerminalSession?>(null) }
    var terminalView by remember { mutableStateOf<TerminalView?>(null) }
    var ctrlMod by remember { mutableStateOf(false) }
    var binder by remember { mutableStateOf<KaliShellService.LocalBinder?>(null) }
    // Pinch-zoom font-size state (Termux-mintára: 6..40 sp tartomány).
    var fontSize by remember { mutableStateOf(20) }

    // Soft-keyboard megjelenítő. Termux flow: setFocusable + requestFocus +
    // showSoftInput. A TerminalView magától NEM kéri az IME-t (nem EditText).
    fun showKeyboard() {
        val tv = terminalView ?: return
        tv.isFocusable = true
        tv.isFocusableInTouchMode = true
        tv.requestFocus()
        val imm = ctx.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.showSoftInput(tv, InputMethodManager.SHOW_IMPLICIT)
    }

    // KaliShellService bind a Composable belépésére, unbind a kilépésére.
    DisposableEffect(Unit) {
        val conn = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, ib: IBinder) {
                Log.i("kaliterm-shell", "KaliShellService connected")
                binder = ib as KaliShellService.LocalBinder
            }
            override fun onServiceDisconnected(name: ComponentName?) {
                Log.w("kaliterm-shell", "KaliShellService disconnected")
                binder = null
            }
        }
        ctx.bindService(
            Intent(ctx, KaliShellService::class.java),
            conn,
            Context.BIND_AUTO_CREATE,
        )
        onDispose {
            try {
                binder?.setListener(null)
                ctx.unbindService(conn)
            } catch (t: Throwable) {
                Log.w("kaliterm-shell", "unbindService hiba: ${t.message}")
            }
            binder = null
        }
    }

    // Rootfs előkészítése (egyszeri) + session lekérés (Service-ből).
    LaunchedEffect(binder, ready) {
        if (!ready) {
            status = "rootfs előkészítése…"
            val err = withContext(Dispatchers.IO) {
                rootfs.prepare { msg -> status = msg }
            }
            if (err != null) {
                status = "HIBA: $err"
                return@LaunchedEffect
            }
            ready = true
        }
        val b = binder
        if (b != null && session == null) {
            // Listener REGISZTRÁCIÓJA — KRITIKUS: a Termux session itt fogja
            // dispatchelni az onTextChanged eseményeket, és nekünk az a dolgunk
            // hogy a TerminalView-t újrarajzoltassuk.
            b.setListener(object : TerminalSessionClient {
                private val tag = "kaliterm-shell"
                override fun onTextChanged(s: TerminalSession?) {
                    // EZ a bug-fix: enélkül a shell ír de a View blank marad.
                    terminalView?.onScreenUpdated()
                }
                override fun onTitleChanged(s: TerminalSession?) {}
                override fun onSessionFinished(s: TerminalSession?) {
                    Log.i(tag, "session exited rc=${s?.exitStatus}")
                    terminalView?.onScreenUpdated()
                }
                override fun onCopyTextToClipboard(s: TerminalSession?, text: String?) {}
                override fun onPasteTextFromClipboard(s: TerminalSession?) {}
                override fun onBell(s: TerminalSession?) {}
                override fun onColorsChanged(s: TerminalSession?) {}
                override fun onTerminalCursorStateChange(state: Boolean) {}
                override fun getTerminalCursorStyle(): Int = 0
                override fun logError(t: String?, m: String?) { Log.e(t ?: tag, m ?: "") }
                override fun logWarn(t: String?,  m: String?) { Log.w(t ?: tag, m ?: "") }
                override fun logInfo(t: String?,  m: String?) { Log.i(t ?: tag, m ?: "") }
                override fun logDebug(t: String?, m: String?) {}
                override fun logVerbose(t: String?, m: String?) {}
                override fun logStackTraceWithMessage(t: String?, m: String?, e: Exception?) {
                    Log.e(t ?: tag, m ?: "", e)
                }
                override fun logStackTrace(t: String?, e: Exception?) {
                    Log.e(t ?: tag, "", e)
                }
            })
            session = b.getOrCreateSession(rootfs)
            status = "shell aktív (session=${session?.hashCode()?.toString(16)})"
            Log.i("kaliterm-shell", "session attached: $status")
        }
    }

    // Insets-kezelés: a MainActivity Scaffold-jából NEM kapunk innerPaddinget
    // (azt a KaliShell-Box explicit elhagyja), így itt kell mindent kezelnünk:
    //   .systemBarsPadding() — status/nav-bar
    //   .imePadding()        — soft-keyboard (billentyűzet fölött legyen a
    //                          terminál + extra-keys row, ne mögötte)
    Column(
        modifier = Modifier
            .fillMaxSize()
            .systemBarsPadding()
            .imePadding()
            .padding(8.dp),
    ) {
        Row(
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(onClick = onBack) { Text("← Vissza") }
            Text(
                text = "Kali shell — $status",
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurface,
            )
        }
        if (!ready) {
            Spacer(Modifier.height(8.dp))
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        }
        Spacer(Modifier.height(8.dp))

        val s = session
        if (s != null) {
            AndroidView(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .background(Color.Black),
                factory = { c ->
                    TerminalView(c, null).apply {
                        // Termux TerminalView konstruktora NEM állít focusable-t —
                        // a TermuxActivity csinálja. Mi is itt csináljuk, mert
                        // különben requestFocus() no-op és az IME "view is not
                        // served" warning-gal eldobja a showSoftInput-ot.
                        isFocusable = true
                        isFocusableInTouchMode = true
                        // setTextSize() inicializálja a renderert — KÖTELEZŐ
                        // hogy attachSession előtt fusson, különben NPE az
                        // onSizeChanged-ben.
                        setTextSize(fontSize)
                        // A TerminalViewClient.onScale-ben kapjuk a pinch-gesture
                        // akumulált skálázását. Mi a `fontSize` state-re mappoljuk,
                        // és setTextSize-szal alkalmazzuk a TerminalView-on.
                        setTerminalViewClient(makeViewClient(
                            showKeyboard = ::showKeyboard,
                            onPinchZoom = { acc ->
                                if (acc < 0.9f || acc > 1.1f) {
                                    val newSize = (fontSize * acc).toInt().coerceIn(8, 64)
                                    if (newSize != fontSize) {
                                        fontSize = newSize
                                        terminalView?.setTextSize(newSize)
                                    }
                                    1.0f  // reset accumulator
                                } else acc  // tovább-gyűjt
                            },
                        ))
                        // attachSession() bind-eli a session-t — utána az
                        // első onSizeChanged forkolja a shellt.
                        attachSession(s)
                        requestFocus()
                        terminalView = this
                    }
                },
            )

            ExtraKeysRow(
                ctrlMod = ctrlMod,
                onCtrlToggle = { ctrlMod = !ctrlMod },
                onKey = { bytes ->
                    val buf = if (ctrlMod && bytes.size == 1) {
                        val c = bytes[0].toInt() and 0xFF
                        val mapped = when (c) {
                            in 0x60..0x7F -> c - 0x60
                            in 0x40..0x5F -> c - 0x40
                            else -> c
                        }
                        ctrlMod = false
                        byteArrayOf(mapped.toByte())
                    } else {
                        bytes
                    }
                    s.write(buf, 0, buf.size)
                },
            )
        } else if (ready) {
            Text("Service-bind folyamatban…", style = MaterialTheme.typography.bodyMedium)
        }
    }
}

/**
 * Termux-stílusú extra-keys row: ESC, TAB, CTRL (sticky), nyilak, gyakori
 * shell-karakterek. Ctrl gomb sticky → a következő alfa-key Ctrl-modifierként
 * megy ki.
 */
@Composable
private fun ExtraKeysRow(
    ctrlMod: Boolean,
    onCtrlToggle: () -> Unit,
    onKey: (ByteArray) -> Unit,
) {
    val scrollState = rememberScrollState()
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFF222222))
            .horizontalScroll(scrollState)
            .padding(horizontal = 4.dp, vertical = 2.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        ExtraKey("ESC") { onKey(byteArrayOf(0x1b)) }
        ExtraKey("TAB") { onKey(byteArrayOf(0x09)) }
        ExtraKey(
            label = if (ctrlMod) "CTRL•" else "CTRL",
            highlighted = ctrlMod,
            onClick = onCtrlToggle,
        )
        // VT100/xterm nav-keys — KÖTELEZŐ az ESC (0x1b) prefix.
        ExtraKey("↑")    { onKey("[A".toByteArray()) }
        ExtraKey("↓")    { onKey("[B".toByteArray()) }
        ExtraKey("←")    { onKey("[D".toByteArray()) }
        ExtraKey("→")    { onKey("[C".toByteArray()) }
        ExtraKey("HOME") { onKey("[H".toByteArray()) }
        ExtraKey("END")  { onKey("[F".toByteArray()) }
        ExtraKey("PgUp") { onKey("[5~".toByteArray()) }
        ExtraKey("PgDn") { onKey("[6~".toByteArray()) }
        ExtraKey("-")  { onKey("-".toByteArray()) }
        ExtraKey("/")  { onKey("/".toByteArray()) }
        ExtraKey("|")  { onKey("|".toByteArray()) }
        ExtraKey("~")  { onKey("~".toByteArray()) }
        ExtraKey("\\") { onKey("\\".toByteArray()) }
    }
}

@Composable
private fun ExtraKey(
    label: String,
    highlighted: Boolean = false,
    onClick: () -> Unit,
) {
    OutlinedButton(
        onClick = onClick,
        modifier = Modifier.height(40.dp),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 10.dp, vertical = 0.dp),
        colors = if (highlighted) {
            ButtonDefaults.outlinedButtonColors(
                containerColor = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
            )
        } else {
            ButtonDefaults.outlinedButtonColors(
                containerColor = Color.Transparent,
                contentColor = Color.White,
            )
        },
    ) {
        Text(label, fontSize = 14.sp)
    }
}

/**
 * Termux-pattern TerminalViewClient. A legtöbb metódus default-tal/false-szal
 * tér vissza, hogy a `TerminalView` saját kezelő-logikája dolgozzon
 * (text-input → emulator → PTY stdin).
 *
 * Az egyetlen kritikus override: `onSingleTapUp` → soft-keyboard popup.
 */
private fun makeViewClient(
    showKeyboard: () -> Unit,
    onPinchZoom: (Float) -> Float = { it },
) = object : TerminalViewClient {
    private val tag = "kaliterm-view"
    override fun onScale(scale: Float): Float = onPinchZoom(scale)
    override fun onSingleTapUp(e: android.view.MotionEvent?) {
        showKeyboard()
    }
    override fun shouldBackButtonBeMappedToEscape(): Boolean = true
    override fun shouldEnforceCharBasedInput(): Boolean = false
    override fun shouldUseCtrlSpaceWorkaround(): Boolean = false
    override fun isTerminalViewSelected(): Boolean = true
    override fun copyModeChanged(copyMode: Boolean) {}
    override fun onKeyDown(keyCode: Int, e: android.view.KeyEvent?, session: TerminalSession?): Boolean = false
    override fun onKeyUp(keyCode: Int, e: android.view.KeyEvent?): Boolean = false
    override fun onLongPress(event: android.view.MotionEvent?): Boolean = false
    override fun readControlKey(): Boolean = false
    override fun readAltKey(): Boolean = false
    override fun readShiftKey(): Boolean = false
    override fun readFnKey(): Boolean = false
    override fun onCodePoint(codePoint: Int, ctrlDown: Boolean, session: TerminalSession?): Boolean = false
    override fun onEmulatorSet() {}
    override fun logError(t: String?, m: String?) { Log.e(t ?: tag, m ?: "") }
    override fun logWarn(t: String?,  m: String?) { Log.w(t ?: tag, m ?: "") }
    override fun logInfo(t: String?,  m: String?) { Log.i(t ?: tag, m ?: "") }
    override fun logDebug(t: String?, m: String?) {}
    override fun logVerbose(t: String?, m: String?) {}
    override fun logStackTraceWithMessage(t: String?, m: String?, e: Exception?) {
        Log.e(t ?: tag, m ?: "", e)
    }
    override fun logStackTrace(t: String?, e: Exception?) {
        Log.e(t ?: tag, "", e)
    }
}
