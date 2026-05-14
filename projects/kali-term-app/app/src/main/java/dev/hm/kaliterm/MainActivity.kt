package dev.hm.kaliterm

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
                            .padding(24.dp)
                    )
                }
            }
        }
    }
}

@Composable
fun Home(modifier: Modifier = Modifier) {
    // A natív rétegen átfutó stringeket cache-eljük. Ha a libtöltés
    // bedől, UnsatisfiedLinkError formájában látjuk.
    val native = runCatching {
        NativeBridge.nativeHello() to NativeBridge.nativeVersion()
    }
    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(12.dp),
        horizontalAlignment = Alignment.Start,
    ) {
        Text(
            text = "kaliterm",
            style = MaterialTheme.typography.headlineMedium,
        )
        Text(
            text = "Userspace Kali terminál — Phase 2a skeleton",
            style = MaterialTheme.typography.bodyMedium,
        )
        native.fold(
            onSuccess = { (msg, ver) ->
                Text("Natív réteg: $msg")
                Text("Build: ${ver / 10000}.${(ver / 100) % 100}.${ver % 100}")
            },
            onFailure = { e ->
                Text("Natív betöltés HIBA: ${e.message}")
            }
        )
    }
}

@Preview(showBackground = true)
@Composable
fun HomePreview() {
    AppTheme {
        Home()
    }
}
