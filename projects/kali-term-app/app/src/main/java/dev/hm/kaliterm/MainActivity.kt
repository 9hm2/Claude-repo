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
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
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
                    UsbDeviceCard(
                        state = state,
                        onRequestPermission = { controller.requestPermission(state) },
                        onAttach = { controller.attachToBridge(state) },
                    )
                }
            }
        }
    }
}

@Composable
private fun UsbDeviceCard(
    state: UsbDeviceState,
    onRequestPermission: () -> Unit,
    onAttach: () -> Unit,
) {
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
                } else if (state.attached) {
                    OutlinedButton(onClick = onAttach, enabled = false) {
                        Text("Attached ✓")
                    }
                } else {
                    Button(onClick = onAttach) { Text("Attach (bridge)") }
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
