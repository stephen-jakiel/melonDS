package me.magnum.melonds.ui.emulator.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.Icon
import androidx.compose.material.IconButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Menu
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/**
 * Always-visible button (top-left) that opens the pause menu, same
 * destination as pressing the system Back button -- a discoverable
 * alternative to it that doesn't depend on the current touch-control
 * layout including a dedicated Pause button.
 */
@Composable
fun PauseMenuButtonUi(onClick: () -> Unit, modifier: Modifier = Modifier) {
    Box(modifier = modifier.fillMaxSize()) {
        IconButton(
            onClick = onClick,
            modifier = Modifier.align(Alignment.TopStart).padding(8.dp),
        ) {
            Icon(
                imageVector = Icons.Default.Menu,
                contentDescription = null,
                tint = Color.White,
            )
        }
    }
}
