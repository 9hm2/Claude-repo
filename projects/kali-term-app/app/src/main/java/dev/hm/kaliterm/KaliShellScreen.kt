package dev.hm.kaliterm

import android.content.Context
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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
 * Kali shell képernyő. A Termux `TerminalSession`-t indítjuk fel a
 * `launch.sh`-vel — TerminalSession a saját PTY-jét spawnolja (JNI fork+exec)
 * és a `TerminalEmulator`-jával parse-olja az xterm-escape sequence-okat.
 * `TerminalView` rendereli a screen buffert.
 *
 * UI-réteg:
 *   - status sor felül
 *   - TerminalView középen (tap → soft keyboard popup)
 *   - extra-keys row alul (Esc, Tab, Ctrl-C, ↑↓←→, Ctrl, /, |, ~, -)
 */
@Composable
fun KaliShellScreen() {
    val ctx = LocalContext.current
    val rootfs = remember { RootfsManager(ctx) }

    var status by remember { mutableStateOf(
        if (rootfs.isReady()) "rootfs kész — shell indítás" else "rootfs nincs kicsomagolva"
    ) }
    var ready by remember { mutableStateOf(rootfs.isReady()) }
    var session by remember { mutableStateOf<TerminalSession?>(null) }
    var terminalView by remember { mutableStateOf<TerminalView?>(null) }
    var ctrlMod by remember { mutableStateOf(false) }   // következő billentyű Ctrl-modosítóval

    // Soft-keyboard megjelenítő segédfüggvény — TerminalView tap-on hívva.
    // A Termux flow-t másolja: setFocusable(true) → requestFocus() → showSoftInput.
    fun showKeyboard() {
        val tv = terminalView ?: return
        tv.isFocusable = true
        tv.isFocusableInTouchMode = true
        tv.requestFocus()
        val imm = ctx.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.showSoftInput(tv, InputMethodManager.SHOW_IMPLICIT)
    }

    LaunchedEffect(Unit) {
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
        if (session == null) {
            session = startSession(rootfs)
            status = "shell aktív"
        }
    }

    Column(modifier = Modifier.fillMaxSize().padding(8.dp)) {
        Text(
            text = "Kali shell — $status",
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurface,
        )
        if (!ready) {
            Spacer(Modifier.height(8.dp))
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        }
        Spacer(Modifier.height(8.dp))

        val s = session
        if (s != null) {
            // Terminál (megfogja a méret-flexet a Column-ban — weight=1)
            AndroidView(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .background(Color.Black),
                factory = { c ->
                    TerminalView(c, null).apply {
                        // KRITIKUS: a Termux `TerminalView` konstruktora NEM
                        // állítja be a focusable-t — a TermuxActivity csinálja
                        // explicit. Nélküle a View.toString() `V.ED.V...` flag-je
                        // `.` a 2. pozíción → requestFocus() no-op, és az
                        // InputMethodManager "view is not served" warning-gal
                        // eldobja a showSoftInput() hívást.
                        isFocusable = true
                        isFocusableInTouchMode = true
                        // setTextSize() inicializálja a renderert — KÖTELEZŐ
                        // hogy attachSession előtt fusson, különben NPE az
                        // onSizeChanged-ben.
                        setTextSize(36)
                        setTerminalViewClient(makeViewClient(::showKeyboard))
                        attachSession(s)
                        requestFocus()
                        terminalView = this
                    }
                },
            )

            // Extra-keys row — vízszintesen scrollozható, hogy minden gomb
            // elérhető maradjon kis screen-szélességen is.
            ExtraKeysRow(
                ctrlMod = ctrlMod,
                onCtrlToggle = { ctrlMod = !ctrlMod },
                onKey = { bytes ->
                    val buf = if (ctrlMod && bytes.size == 1) {
                        // Ctrl-modosító: az ASCII 'a'..'z' / '@'..'_' tartomány
                        // alsó 5 bitje a control-character. Pl. Ctrl-C = 0x03.
                        val c = bytes[0].toInt() and 0xFF
                        val mapped = when (c) {
                            in 0x60..0x7F -> c - 0x60   // 'a'..'~' → 0x00..0x1F
                            in 0x40..0x5F -> c - 0x40   // 'A'..'_' → 0x00..0x1F
                            else -> c
                        }
                        ctrlMod = false
                        byteArrayOf(mapped.toByte())
                    } else {
                        bytes
                    }
                    // TerminalSession.write(byte[], offset, count) — a JNI
                    // fd-be ír. A 1-arg `write(String)` overload UTF-8-ra
                    // konvertálna, ami az ESC (0x1b) byteoknál nem ideális.
                    s.write(buf, 0, buf.size)
                },
            )
        } else if (ready) {
            Button(onClick = { session = startSession(rootfs) }) { Text("Shell indítása") }
        }
    }
}

/**
 * Termux-stílusú extra-keys row: ESC, TAB, CTRL (sticky), nyilak, gyakori
 * shell-karakterek (-, /, |, ~). Ctrl gomb sticky → a következő alfa-key
 * Ctrl-modifierként megy ki.
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
        ExtraKey("↑")  { onKey("[A".toByteArray()) }
        ExtraKey("↓")  { onKey("[B".toByteArray()) }
        ExtraKey("←")  { onKey("[D".toByteArray()) }
        ExtraKey("→")  { onKey("[C".toByteArray()) }
        ExtraKey("HOME")  { onKey("[H".toByteArray()) }
        ExtraKey("END")   { onKey("[F".toByteArray()) }
        ExtraKey("PgUp")  { onKey("[5~".toByteArray()) }
        ExtraKey("PgDn")  { onKey("[6~".toByteArray()) }
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

private fun startSession(rootfs: RootfsManager): TerminalSession {
    val env = arrayOf(
        "HOME=${rootfs.bundleDir.absolutePath}",
        "PREFIX=${rootfs.bundleDir.absolutePath}",
        "ROOTFS_DIR=${rootfs.rootfsDir.absolutePath}",
        "USER_HOME=/root",
        "TERM=xterm-256color",
        "LANG=C.UTF-8",
        "PATH=/system/bin:/system/xbin",
    )
    val session = TerminalSession(
        /* shellPath = */ "/system/bin/sh",
        /* cwd       = */ rootfs.bundleDir.absolutePath,
        /* args      = */ arrayOf(rootfs.launchSh.absolutePath),
        /* env       = */ env,
        /* transcriptRows = */ 5000,
        /* client    = */ makeSessionClient(),
    )
    return session
}

private fun makeSessionClient() = object : TerminalSessionClient {
    private val tag = "kaliterm-shell"
    override fun onTextChanged(session: TerminalSession?) {}
    override fun onTitleChanged(session: TerminalSession?) {}
    override fun onSessionFinished(session: TerminalSession?) {
        Log.i(tag, "session exited rc=${session?.exitStatus}")
    }
    override fun onCopyTextToClipboard(session: TerminalSession?, text: String?) {}
    override fun onPasteTextFromClipboard(session: TerminalSession?) {}
    override fun onBell(session: TerminalSession?) {}
    override fun onColorsChanged(session: TerminalSession?) {}
    override fun onTerminalCursorStateChange(state: Boolean) {}
    override fun getTerminalCursorStyle(): Int = 0
    override fun logError(tag: String?, message: String?) { Log.e(tag ?: this.tag, message ?: "") }
    override fun logWarn(tag: String?,  message: String?) { Log.w(tag ?: this.tag, message ?: "") }
    override fun logInfo(tag: String?,  message: String?) { Log.i(tag ?: this.tag, message ?: "") }
    override fun logDebug(tag: String?, message: String?) {}
    override fun logVerbose(tag: String?, message: String?) {}
    override fun logStackTraceWithMessage(tag: String?, message: String?, e: Exception?) {
        Log.e(tag ?: this.tag, message ?: "", e)
    }
    override fun logStackTrace(tag: String?, e: Exception?) {
        Log.e(tag ?: this.tag, "", e)
    }
}

private fun makeViewClient(showKeyboard: () -> Unit) = object : TerminalViewClient {
    private val tag = "kaliterm-view"
    override fun onScale(scale: Float): Float = scale
    // FONTOS: tap → soft keyboard popup. A TerminalView nem tudja
    // önmagában megnyitni az IME-t, csak EditText-szerű view-k.
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
    override fun logError(tag: String?, message: String?) { Log.e(tag ?: this.tag, message ?: "") }
    override fun logWarn(tag: String?,  message: String?) { Log.w(tag ?: this.tag, message ?: "") }
    override fun logInfo(tag: String?,  message: String?) { Log.i(tag ?: this.tag, message ?: "") }
    override fun logDebug(tag: String?, message: String?) {}
    override fun logVerbose(tag: String?, message: String?) {}
    override fun logStackTraceWithMessage(tag: String?, message: String?, e: Exception?) {
        Log.e(tag ?: this.tag, message ?: "", e)
    }
    override fun logStackTrace(tag: String?, e: Exception?) {
        Log.e(tag ?: this.tag, "", e)
    }
}
