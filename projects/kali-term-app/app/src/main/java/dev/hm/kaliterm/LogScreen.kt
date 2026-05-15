package dev.hm.kaliterm

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Log viewer Compose screen — két forrásból olvas:
 *   1) Crash-fájlok (filesDir/crashes/) — KaliApplication írja
 *   2) Logcat (`logcat -d --uid=N -v time -T 2000`) — saját és :lkl
 *      process logjai együtt
 *
 * Funkciók:
 *   - Filter (text-search, case-insensitive)
 *   - Másol gomb (clipboard)
 *   - Megosztás gomb (intent)
 *   - Frissítés gomb
 *
 * Logcat olvasás: API 21+ saját UID-re engedélyezett külön permission
 * nélkül. A `--uid=` flag biztosítja hogy mindkét process (main + :lkl)
 * logjai bejönnek.
 */
@Composable
fun LogScreen(onBack: () -> Unit = {}) {
    val ctx = LocalContext.current

    var filter by remember { mutableStateOf("") }
    var lines by remember { mutableStateOf<List<String>>(emptyList()) }
    var status by remember { mutableStateOf("betöltés…") }
    var refreshTrigger by remember { mutableStateOf(0) }

    LaunchedEffect(refreshTrigger) {
        status = "betöltés…"
        val collected = withContext(Dispatchers.IO) { collectLogs(ctx) }
        lines = collected
        status = "${collected.size} sor — szűrő: '${if (filter.isEmpty()) "minden" else filter}'"
    }

    val visible = remember(lines, filter) {
        if (filter.isBlank()) lines
        else lines.filter { it.contains(filter, ignoreCase = true) }
    }

    Column(modifier = Modifier.fillMaxSize().padding(8.dp)) {
        Row(
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            OutlinedButton(onClick = onBack) { Text("← Vissza") }
            OutlinedButton(onClick = { refreshTrigger++ }) { Text("Frissít") }
            OutlinedButton(onClick = { copyToClipboard(ctx, visible.joinToString("\n")) }) {
                Text("Másol")
            }
            OutlinedButton(onClick = { share(ctx, visible.joinToString("\n")) }) {
                Text("Megoszt")
            }
        }

        Spacer(Modifier.height(4.dp))
        OutlinedTextField(
            value = filter,
            onValueChange = { filter = it },
            label = { Text("Szűrő (substring, case-insensitive)") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )

        Spacer(Modifier.height(4.dp))
        Text(
            text = status + " — összesen ${visible.size} látható",
            style = MaterialTheme.typography.bodySmall,
        )

        Spacer(Modifier.height(4.dp))
        val listState = rememberLazyListState()
        SelectionContainer(modifier = Modifier.fillMaxSize()) {
            LazyColumn(
                state = listState,
                modifier = Modifier
                    .fillMaxSize()
                    .background(MaterialTheme.colorScheme.surfaceVariant),
            ) {
                items(visible, key = { it }) { line ->
                    val color = when {
                        " E " in line || "FATAL" in line || "fatal" in line -> MaterialTheme.colorScheme.error
                        " W " in line -> MaterialTheme.colorScheme.tertiary
                        line.startsWith("=== kaliterm crash") -> MaterialTheme.colorScheme.error
                        else -> MaterialTheme.colorScheme.onSurfaceVariant
                    }
                    Text(
                        text = line,
                        style = MaterialTheme.typography.bodySmall.copy(
                            fontFamily = FontFamily.Monospace,
                        ),
                        color = color,
                        modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp),
                    )
                }
            }
        }
    }
}

/** Crash-fájlok + logcat olvasás egy listába. Crash-ek elöl. */
private fun collectLogs(ctx: Context): List<String> {
    val out = mutableListOf<String>()

    // 1) Crash-fájlok időrendben (legfrissebb elöl).
    val crashDir = File(ctx.filesDir, "crashes")
    if (crashDir.isDirectory) {
        val files = crashDir.listFiles()?.sortedByDescending { it.lastModified() } ?: emptyList()
        if (files.isNotEmpty()) {
            out += "════════ ${files.size} CRASH FÁJL ($crashDir) ════════"
        }
        for (f in files) {
            out += ""
            out += "──────── ${f.name} (${f.length()} byte) ────────"
            try {
                out += f.readText().lines()
            } catch (e: Throwable) {
                out += "(olvasási hiba: ${e.message})"
            }
        }
        if (files.isNotEmpty()) {
            out += ""
            out += "════════ /CRASH FÁJL VÉGE ════════"
            out += ""
        }
    }

    // 2) Logcat saját UID-re (main + :lkl process is bejön).
    out += "════════ logcat (saját UID) ════════"
    try {
        val uid = android.os.Process.myUid()
        val pb = ProcessBuilder(
            "logcat", "-d",
            "--uid=$uid",
            "-v", "time",
            "-T", "2000",       // utolsó 2000 sor
            "*:V",
        )
        pb.redirectErrorStream(true)
        val p = pb.start()
        out += p.inputStream.bufferedReader().readText().lines()
        p.waitFor()
    } catch (e: Throwable) {
        out += "(logcat parancs hiba: ${e.javaClass.simpleName}: ${e.message})"
        out += "(API < 21? READ_LOGS permission hiányzik?)"
    }

    return out
}

private fun copyToClipboard(ctx: Context, text: String) {
    val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
    cm.setPrimaryClip(ClipData.newPlainText("kaliterm log", text))
}

private fun share(ctx: Context, text: String) {
    val intent = Intent(Intent.ACTION_SEND).apply {
        type = "text/plain"
        putExtra(Intent.EXTRA_SUBJECT, "kaliterm log")
        putExtra(Intent.EXTRA_TEXT, text.take(500_000))  // limit
    }
    ctx.startActivity(Intent.createChooser(intent, "Megosztás").apply {
        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    })
}
