package me.magnum.melonds.ui.emulator.lua

import android.os.Environment
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.AlertDialog
import androidx.compose.material.Checkbox
import androidx.compose.material.DropdownMenu
import androidx.compose.material.DropdownMenuItem
import androidx.compose.material.Button
import androidx.compose.material.Text
import androidx.compose.material.TextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.LuaFormsRequest
import java.io.File
import kotlin.math.roundToInt

private const val POLL_INTERVAL_MS = 16L

private class OpenFileRequest(val initialDir: String, val onPicked: (String) -> Unit, val onCancel: () -> Unit)

/**
 * Renders every form a running Lua script has created (LuaFormsManager's
 * state) as draggable in-app panels on top of the game, and drives the
 * request/command bridge described in LuaScriptManager.h: fire-and-forget
 * commands are applied every poll tick with no response, while the single
 * in-flight blocking request (if any) is answered via
 * MelonEmulator.deliverLuaFormsResult() once Kotlin has processed it --
 * except forms.openfile(), which instead shows a picker and only answers
 * once the user actually makes a choice.
 */
@Composable
fun LuaFormsOverlayUi(isScriptRunning: Boolean, modifier: Modifier = Modifier) {
    if (!isScriptRunning) {
        return
    }

    var openFileRequest by remember { mutableStateOf<OpenFileRequest?>(null) }

    androidx.compose.runtime.LaunchedEffect(Unit) {
        LuaFormsManager.reset()
        while (isActive) {
            for (cmd in MelonEmulator.takeLuaFormsCommands()) {
                LuaFormsManager.processCommand(cmd)
            }

            val req = MelonEmulator.pollLuaFormsRequest()
            if (req != null) {
                if (req.op == LuaFormsRequest.OP_OPEN_FILE) {
                    openFileRequest = OpenFileRequest(
                        initialDir = req.text,
                        onPicked = { path ->
                            MelonEmulator.deliverLuaFormsResult(0, path, false)
                            openFileRequest = null
                        },
                        onCancel = {
                            MelonEmulator.deliverLuaFormsResult(0, "", false)
                            openFileRequest = null
                        },
                    )
                } else {
                    val result = LuaFormsManager.processRequest(req)
                    MelonEmulator.deliverLuaFormsResult(result.intResult, result.stringResult, result.boolResult)
                }
            }

            delay(POLL_INTERVAL_MS)
        }
    }

    Box(modifier = modifier) {
        for ((handle, form) in LuaFormsManager.forms) {
            if (form.visible) {
                key(handle) {
                    LuaFormPanel(form)
                }
            }
        }
    }

    openFileRequest?.let { request ->
        LuaFormsFileBrowserDialog(
            initialDirHint = request.initialDir,
            onPicked = request.onPicked,
            onCancel = request.onCancel,
        )
    }
}

@Composable
private fun LuaFormPanel(form: LuaForm) {
    Column(
        modifier = Modifier
            .offset { IntOffset(form.offsetX.roundToInt(), form.offsetY.roundToInt()) }
            .width(form.w.dp)
            .clip(androidx.compose.foundation.shape.RoundedCornerShape(4.dp))
            .background(Color(0xFFE8E8E8)),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(Color(0xFF3C5A8A))
                .pointerInput(form.handle) {
                    detectDragGestures { change, dragAmount ->
                        change.consume()
                        form.offsetX += dragAmount.x
                        form.offsetY += dragAmount.y
                    }
                }
                .padding(horizontal = 6.dp, vertical = 3.dp),
        ) {
            Text(
                text = form.title,
                color = Color.White,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = "✕",
                color = Color.White,
                fontSize = 12.sp,
                modifier = Modifier.clickable {
                    LuaFormsManager.forms.remove(form.handle)
                    MelonEmulator.notifyLuaFormsFormClosed(form.handle)
                },
            )
        }

        Box(modifier = Modifier.width(form.w.dp).size(width = form.w.dp, height = form.h.dp)) {
            for (widget in form.widgets) {
                if (widget.visible) {
                    key(widget.handle) {
                        LuaWidgetView(widget)
                    }
                }
            }
        }
    }
}

@Composable
private fun LuaWidgetView(widget: LuaWidget) {
    val offsetModifier = Modifier.offset { IntOffset(widget.x, widget.y) }
    when (widget) {
        is LuaWidget.Button -> Button(
            onClick = { MelonEmulator.notifyLuaFormsClick(widget.handle) },
            enabled = widget.enabled,
            modifier = offsetModifier.size(width = widget.w.dp, height = widget.h.dp),
        ) {
            Text(text = widget.text, fontSize = 10.sp)
        }
        is LuaWidget.Label -> Text(
            text = widget.text,
            fontSize = 11.sp,
            color = Color.Black,
            modifier = offsetModifier,
        )
        is LuaWidget.Checkbox -> Row(
            modifier = offsetModifier,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Checkbox(
                checked = widget.checked,
                onCheckedChange = { checked ->
                    widget.checked = checked
                    MelonEmulator.notifyLuaFormsClick(widget.handle)
                },
                enabled = widget.enabled,
            )
            Text(text = widget.text, fontSize = 11.sp)
        }
        is LuaWidget.Textbox -> TextField(
            value = widget.text,
            onValueChange = { widget.text = it },
            enabled = widget.enabled,
            singleLine = !widget.multiline,
            modifier = offsetModifier.size(width = widget.w.dp, height = widget.h.dp),
        )
        is LuaWidget.Dropdown -> {
            var expanded by remember { mutableStateOf(false) }
            Box(modifier = offsetModifier.size(width = widget.w.dp, height = widget.h.dp)) {
                Button(onClick = { expanded = true }, enabled = widget.enabled) {
                    Text(text = widget.selected, fontSize = 10.sp)
                }
                DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                    for (item in widget.items) {
                        DropdownMenuItem(onClick = {
                            widget.selected = item
                            expanded = false
                        }) {
                            Text(text = item)
                        }
                    }
                }
            }
        }
        is LuaWidget.PictureBox -> {
            val imageCache = remember(widget.handle) { mutableMapOf<String, android.graphics.Bitmap?>() }
            androidx.compose.foundation.Canvas(
                modifier = offsetModifier.size(width = widget.w.dp, height = widget.h.dp),
            ) {
                if (widget.backgroundColor != 0) {
                    drawRect(color = Color(widget.backgroundColor))
                }
                drawLuaCommandsAt(widget.commands, imageCache, 0f, 0f, 1f)
            }
        }
    }
}

@Composable
private fun LuaFormsFileBrowserDialog(initialDirHint: String, onPicked: (String) -> Unit, onCancel: () -> Unit) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val root = remember { context.getExternalFilesDir(null) ?: Environment.getExternalStorageDirectory() }
    val startDir = remember {
        val hinted = if (initialDirHint.isNotBlank()) File(initialDirHint) else root
        if (hinted.isDirectory) hinted else root
    }
    var currentDir by remember { mutableStateOf(startDir) }

    val entries = remember(currentDir) {
        currentDir.listFiles()?.sortedWith(compareBy({ !it.isDirectory }, { it.name.lowercase() })) ?: emptyList()
    }
    val canGoUp = currentDir != root && currentDir.parentFile != null

    AlertDialog(
        onDismissRequest = onCancel,
        title = { Text(text = currentDir.path.removePrefix(root.path).ifEmpty { "/" }) },
        text = {
            LazyColumn {
                if (canGoUp) {
                    item {
                        Text(
                            text = "..",
                            modifier = Modifier.fillMaxWidth().clickable { currentDir = currentDir.parentFile ?: root }.padding(8.dp),
                        )
                    }
                }
                items(entries) { entry ->
                    Text(
                        text = if (entry.isDirectory) "${entry.name}/" else entry.name,
                        modifier = Modifier.fillMaxWidth().clickable {
                            if (entry.isDirectory) currentDir = entry else onPicked(entry.absolutePath)
                        }.padding(8.dp),
                    )
                }
            }
        },
        confirmButton = {},
        dismissButton = { Button(onClick = onCancel) { Text("Cancel") } },
    )
}
