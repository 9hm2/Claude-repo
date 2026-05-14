package dev.hm.kaliterm

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.hm.kaliterm.ui.theme.AppTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            AppTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    Home(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(innerPadding)
                            .padding(horizontal = 16.dp, vertical = 12.dp)
                    )
                }
            }
        }
    }
}

@Composable
fun Home(modifier: Modifier = Modifier) {
    val nativeStatus = runCatching {
        NativeBridge.nativeHello() to NativeBridge.nativeVersion()
    }
    val controller = rememberUsbController()
    val lastLog by controller.lastAttachLog
    val lastDesc by controller.lastDescription
    val bridgeStatus by controller.bridgeStatus
    val activeBridgeId by controller.activeBridgeDeviceId
    val lklStatus = remember { mutableStateOf(runCatching { NativeBridge.nativeLklStatus() }.getOrDefault("?")) }
    fun refreshLkl() {
        lklStatus.value = runCatching { NativeBridge.nativeLklStatus() }.getOrDefault("?")
    }

    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            text = "kaliterm",
            style = MaterialTheme.typography.headlineMedium,
        )
        Text(
            text = "Userspace Kali terminál — Phase 2b.1",
            style = MaterialTheme.typography.bodyMedium,
        )

        nativeStatus.fold(
            onSuccess = { (msg, ver) ->
                Text("Natív: $msg")
                Text("Build: ${ver / 10000}.${(ver / 100) % 100}.${ver % 100}")
            },
            onFailure = { e ->
                Text("Natív betöltés HIBA: ${e.message}")
            }
        )

        HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))

        // LKL állapot-panel.
        LklStatusBar(
            status = lklStatus.value,
            libraryLoaded = NativeBridge.lklLibraryLoaded,
            onStart = {
                NativeBridge.nativeLklStart()
                refreshLkl()
            },
            onStop = {
                NativeBridge.nativeLklStop()
                refreshLkl()
            },
            onRefresh = { refreshLkl() },
        )

        // Bridge állapot-panel.
        BridgeStatusBar(
            status = bridgeStatus,
            running = activeBridgeId != null,
            onStop = { controller.stopBridge() },
        )

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "USB eszközök (${controller.devices.size})",
                style = MaterialTheme.typography.titleMedium,
            )
            OutlinedButton(onClick = { controller.refresh() }) { Text("Frissít") }
        }

        if (lastLog != null) {
            Text(
                text = "Utolsó attach: $lastLog",
                style = MaterialTheme.typography.bodySmall,
            )
        }

        if (!lastDesc.isNullOrBlank()) {
            CopyableLogCard(
                title = "Descriptor (libusb_wrap_sys_device)",
                content = lastDesc.orEmpty(),
            )
        }

        if (controller.devices.isEmpty()) {
            Text(
                text = "Nincs csatlakoztatott USB eszköz. OTG kábellel kapcsolj " +
                       "rá valamit (pl. CH340/FTDI USB-soros adapter).",
                style = MaterialTheme.typography.bodyMedium,
            )
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                items(controller.devices, key = { it.device.deviceId }) { state ->
                    val isThisBridged = activeBridgeId == state.device.deviceId
                    val otherBridgeRunning = activeBridgeId != null && !isThisBridged
                    UsbDeviceCard(
                        state = state,
                        bridgeOnThisDevice = isThisBridged,
                        otherBridgeBlocking = otherBridgeRunning,
                        onRequestPermission = { controller.requestPermission(state) },
                        onProbe = { controller.attachToBridge(state) },
                        onStartBridge = { controller.startBridge(state) },
                        onStopBridge = { controller.stopBridge() },
                    )
                }
            }
        }
    }
}

@Composable
private fun BridgeStatusBar(
    status: String,
    running: Boolean,
    onStop: () -> Unit,
) {
    val clipboard = LocalClipboardManager.current
    val color = if (running) MaterialTheme.colorScheme.tertiaryContainer
                else          MaterialTheme.colorScheme.surfaceVariant
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = color),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = if (running) "Bridge — RUNNING" else "Bridge — STOPPED",
                    style = MaterialTheme.typography.titleSmall,
                )
                // SelectionContainer: long-press → select → system "copy".
                SelectionContainer {
                    Text(
                        text = status,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
            Column(horizontalAlignment = Alignment.End) {
                TextButton(onClick = {
                    clipboard.setText(AnnotatedString(status))
                }) { Text("Másol") }
                if (running) {
                    OutlinedButton(onClick = onStop) { Text("Stop") }
                }
            }
        }
    }
}

@Composable
private fun LklStatusBar(
    status: String,
    libraryLoaded: Boolean,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onRefresh: () -> Unit,
) {
    val clipboard = LocalClipboardManager.current
    val running = status.contains("running = YES")
    val available = status.startsWith("AVAILABLE")
    val color = when {
        running   -> MaterialTheme.colorScheme.tertiaryContainer
        available -> MaterialTheme.colorScheme.secondaryContainer
        else      -> MaterialTheme.colorScheme.surfaceVariant
    }
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = color),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = when {
                        running   -> "LKL — RUNNING"
                        available -> "LKL — LOADED"
                        else      -> "LKL — UNAVAILABLE"
                    },
                    style = MaterialTheme.typography.titleSmall,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    TextButton(onClick = {
                        clipboard.setText(AnnotatedString(status))
                    }) { Text("Másol") }
                    OutlinedButton(onClick = onRefresh) { Text("Frissít") }
                    if (running) {
                        Button(onClick = onStop) { Text("Halt") }
                    } else if (available) {
                        Button(onClick = onStart) { Text("Start") }
                    }
                }
            }
            SelectionContainer {
                Text(
                    text = status + if (!libraryLoaded)
                            "\n(System.loadLibrary(\"lkl-host-lib\") sikertelen — .so nincs az APK-ban)"
                        else "",
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = 160.dp)
                        .verticalScroll(rememberScrollState()),
                )
            }
        }
    }
}

/**
 * Hosszabb, scrollozható, mono-spaced szöveg dobozban — `SelectionContainer`
 * + egy "Másolás" gomb, ami a teljes tartalmat egy érintéssel vágólapra teszi.
 * Logok és descriptor-dump-ok megjelenítésére használjuk.
 */
@Composable
private fun CopyableLogCard(
    title: String,
    content: String,
    maxHeight: androidx.compose.ui.unit.Dp = 220.dp,
) {
    val clipboard = LocalClipboardManager.current
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                )
                TextButton(onClick = {
                    clipboard.setText(AnnotatedString(content))
                }) { Text("Másolás") }
            }
            SelectionContainer {
                Text(
                    text = content,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = maxHeight)
                        .verticalScroll(rememberScrollState()),
                )
            }
        }
    }
}

@Composable
private fun UsbDeviceCard(
    state: UsbDeviceState,
    bridgeOnThisDevice: Boolean,
    otherBridgeBlocking: Boolean,
    onRequestPermission: () -> Unit,
    onProbe: () -> Unit,
    onStartBridge: () -> Unit,
    onStopBridge: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (bridgeOnThisDevice)
                MaterialTheme.colorScheme.tertiaryContainer
            else
                MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            val d = state.device
            Text(
                text = "%04x:%04x  ${d.productName ?: "(névtelen)"}"
                    .format(d.vendorId, d.productId),
                style = MaterialTheme.typography.titleSmall,
            )
            Text(
                text = "gyártó: ${d.manufacturerName ?: "?"}   class: ${d.deviceClass}.${d.deviceSubclass}",
                style = MaterialTheme.typography.bodySmall,
            )
            Text(
                text = "path: ${d.deviceName}   ifs: ${d.interfaceCount}",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(4.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (!state.granted) {
                    Button(onClick = onRequestPermission) { Text("Engedély") }
                } else if (bridgeOnThisDevice) {
                    Button(onClick = onStopBridge) { Text("Stop bridge") }
                } else {
                    OutlinedButton(onClick = onProbe) { Text("Probe") }
                    Button(
                        onClick = onStartBridge,
                        enabled = !otherBridgeBlocking,
                    ) { Text("Start bridge") }
                }
            }
        }
    }
}

@Preview(showBackground = true)
@Composable
fun HomePreview() {
    AppTheme {
        Home()
    }
}
