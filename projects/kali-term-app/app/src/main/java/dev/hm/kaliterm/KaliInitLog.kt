package dev.hm.kaliterm

import android.util.Log
import androidx.compose.runtime.mutableStateListOf
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * App-belső init-log buffer. Compose-reactive: a `lines` SnapshotStateList,
 * amit a UI közvetlenül megjelenít — minden append azonnal recompose-ot
 * okoz a feliratkozott Composable-ekben.
 *
 * Cél: a user lássa SOR-SOR-RA hol tart a bind/init folyamat, és ha
 * elakad, a "Másol" gombbal egy clipboard-ra tudja tenni az egészet
 * troubleshooting céljából.
 *
 * Minden bejegyzés `[HH:mm:ss.SSS] <tag>: <üzenet>` formátumban. Maximum
 * 300 sor; régiek FIFO-szerűen kiesnek.
 *
 * Egyúttal `Log.i`-t is hív, hogy a logcat-on is megjelenjenek a sorok.
 */
object KaliInitLog {

    private const val MAX_LINES = 300
    private const val LOG_TAG = "kaliterm-init"

    val lines: androidx.compose.runtime.snapshots.SnapshotStateList<String> =
        mutableStateListOf()

    private val timeFmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)
    private val mutex = Any()

    /**
     * Append egy új sor a buffer-be. Thread-safe; bárhonnan hívható
     * (main / IO / Binder szál).
     *
     * @param tag rövid forrás-jelölő (pl. "lkl-ctrl", "shell-svc", "init")
     * @param msg az emberi olvasható üzenet
     */
    fun add(tag: String, msg: String) {
        synchronized(mutex) {
            val ts = timeFmt.format(Date())
            val entry = "[$ts] $tag: $msg"
            Log.i(LOG_TAG, "$tag: $msg")
            lines.add(entry)
            while (lines.size > MAX_LINES) lines.removeAt(0)
        }
    }

    /** Az összes sor egyetlen multiline-String-ként (clipboard-ra valóra). */
    fun toMultilineString(): String = synchronized(mutex) { lines.joinToString("\n") }

    /** Üríti a buffert — ritkán hívandó (pl. user explicit reset). */
    fun clear() = synchronized(mutex) { lines.clear() }
}
