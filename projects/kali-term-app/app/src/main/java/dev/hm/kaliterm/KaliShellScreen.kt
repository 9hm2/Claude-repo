package dev.hm.kaliterm

import android.util.Log
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
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
 * Lifecycle:
 *   1) RootfsManager.prepare() — első indításkor kicsomagolja a tar.xz-t
 *      (kb. 30-60 sec). Progress UI alatt.
 *   2) Ha kész, TerminalSession indul shell="/system/bin/sh", arg="launch.sh",
 *      env=PREFIX, ROOTFS_DIR, TERM=xterm-256color, stb.
 *   3) Display: AndroidView(TerminalView) — a Termux saját View-ja, ami
 *      a fókusz/touch/billentyűzet eseményeket is intézi.
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

    // Első renderelésnél prepare a háttérben, ha még nincs kész.
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

        // Termux TerminalView Compose-ba ágyazva.
        val s = session
        if (s != null) {
            AndroidView(
                modifier = Modifier.fillMaxSize().background(Color.Black),
                factory = { c ->
                    TerminalView(c, null).apply {
                        setTerminalViewClient(makeViewClient())
                        attachSession(s)
                        requestFocus()
                    }
                },
            )
        } else if (ready) {
            // Ready, de a session indítás még folyamatban — defenzív
            Button(onClick = { session = startSession(rootfs) }) { Text("Shell indítása") }
        }
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
    override fun getTerminalCursorStyle(): Int = 0     // BLOCK
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

private fun makeViewClient() = object : TerminalViewClient {
    private val tag = "kaliterm-view"
    override fun onScale(scale: Float): Float = scale
    override fun onSingleTapUp(e: android.view.MotionEvent?) {}
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
