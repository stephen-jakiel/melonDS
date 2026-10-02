package me.magnum.melonds.ui.emulator.lua

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.Icon
import androidx.compose.material.IconButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.LuaDrawCommand

// NDS-native coordinate space the gui.* overlay is drawn in: a standard
// top-then-bottom dual-screen layout (256 wide, top screen y=[0,192),
// bottom screen y=[192,384)), same simplification the desktop build makes
// -- custom/rotated screen layouts aren't accounted for yet. Expanded by
// whatever client.SetGameExtraPadding() last requested -- scripts (this
// tracker included) size their own UI against client.bufferwidth()/
// screenwidth()/screenheight(), which report this same expanded size, and
// draw into the extra space assuming it's really there.
private const val NDS_WIDTH = 256f
private const val NDS_HEIGHT = 384f
private const val POLL_INTERVAL_MS = 33L // ~30fps, independent of core framerate
// Tunable "how much of the available height to fill" factor for the gui.*
// overlay -- 1.0 fills it edge-to-edge, which read as too large in practice.
private const val OVERLAY_SCALE_FACTOR = 0.65f

/**
 * Renders a running Lua script's gui.draw*() overlay on top of the game,
 * with a small toggle button (top-right) to expand/collapse it. Polls
 * MelonEmulator.getLuaDrawCommands() on a timer rather than being driven by
 * the emulator's own frame callback, since the overlay only needs to look
 * smooth to a human, not match the core's exact frame cadence.
 */
@Composable
fun LuaOverlayUi(isScriptRunning: Boolean, modifier: Modifier = Modifier) {
    if (!isScriptRunning) {
        return
    }

    var expanded by rememberSaveable(isScriptRunning) { mutableStateOf(true) }
    var commands by remember { mutableStateOf<List<LuaDrawCommand>>(emptyList()) }
    var padding by remember { mutableStateOf(IntArray(4)) } // left, top, right, bottom
    val imageCache = remember { mutableMapOf<String, Bitmap?>() }

    LaunchedEffect(Unit) {
        while (isActive) {
            commands = MelonEmulator.getLuaDrawCommands().asList()
            padding = MelonEmulator.getLuaScreenPadding()
            delay(POLL_INTERVAL_MS)
        }
    }

    Box(modifier = modifier.fillMaxSize()) {
        if (expanded) {
            // Leave room at the top for the toggle icon (below) so the
            // overlay content doesn't sit underneath/overlap it.
            BoxWithConstraints(modifier = Modifier.fillMaxSize().padding(top = 56.dp)) {
                val (padLeft, padTop, padRight, padBottom) = padding
                val totalWidth = NDS_WIDTH + padLeft + padRight
                val totalHeight = NDS_HEIGHT + padTop + padBottom

                // Scale for height only, not "fit everything in view": this
                // tracker's extra padding (e.g. 199px for its stats panel)
                // assumes a wide desktop window, which a portrait phone
                // screen doesn't have -- fitting the full padded width would
                // shrink the whole overlay (game area included) down to a
                // cramped, barely-readable size. Scaling for height instead
                // keeps content a sensible size and lets the padded area
                // extend past the screen edge, reachable by scrolling.
                val density = LocalDensity.current
                val availableHeightPx = with(density) { maxHeight.toPx() }
                val scale = if (totalHeight > 0f) (availableHeightPx / totalHeight) * OVERLAY_SCALE_FACTOR else 1f
                val canvasWidthDp = with(density) { (totalWidth * scale).toDp() }

                Box(modifier = Modifier.fillMaxHeight().horizontalScroll(rememberScrollState())) {
                    Canvas(modifier = Modifier.width(canvasWidthDp).fillMaxHeight()) {
                        val offsetX = padLeft * scale
                        val offsetY = padTop * scale
                        drawLuaCommandsAt(commands, imageCache, offsetX, offsetY, scale)
                    }
                }
            }
        }

        IconButton(
            onClick = { expanded = !expanded },
            modifier = Modifier.align(Alignment.TopEnd).padding(8.dp),
        ) {
            Icon(
                imageVector = if (expanded) Icons.Default.Visibility else Icons.Default.VisibilityOff,
                contentDescription = null,
                tint = Color.White,
            )
        }
    }
}

private fun Int.hasAlpha() = (this ushr 24) != 0

// Shared by the gui.* screen overlay (scaled/letterboxed to fit the NDS's
// 256x384 native space) and forms.pictureBox content (offsetX=offsetY=0,
// scale=1 -- picturebox coordinates are already plain, unscaled pixels
// within its own bounds), so both stay visually consistent.
fun DrawScope.drawLuaCommandsAt(commands: List<LuaDrawCommand>, imageCache: MutableMap<String, Bitmap?>, offsetX: Float, offsetY: Float, scale: Float) {
    drawContext.canvas.nativeCanvas.apply {
        for (cmd in commands) {
            when (cmd.kind) {
                LuaDrawCommand.KIND_TEXT -> {
                    val paint = Paint().apply {
                        color = cmd.color
                        textSize = 14f * scale
                        isAntiAlias = true
                    }
                    drawText(cmd.text, offsetX + cmd.x1 * scale, offsetY + cmd.y1 * scale + paint.textSize, paint)
                }
                LuaDrawCommand.KIND_RECT -> {
                    val left = offsetX + cmd.x1 * scale
                    val top = offsetY + cmd.y1 * scale
                    val right = left + cmd.x2 * scale
                    val bottom = top + cmd.y2 * scale
                    if (cmd.fillColor.hasAlpha()) {
                        drawRect(left, top, right, bottom, Paint().apply { color = cmd.fillColor; style = Paint.Style.FILL })
                    }
                    if (cmd.color.hasAlpha()) {
                        drawRect(left, top, right, bottom, Paint().apply { color = cmd.color; style = Paint.Style.STROKE })
                    }
                }
                LuaDrawCommand.KIND_LINE -> {
                    if (cmd.color.hasAlpha()) {
                        drawLine(
                            offsetX + cmd.x1 * scale, offsetY + cmd.y1 * scale,
                            offsetX + cmd.x2 * scale, offsetY + cmd.y2 * scale,
                            Paint().apply { color = cmd.color },
                        )
                    }
                }
                LuaDrawCommand.KIND_PIXEL -> {
                    if (cmd.color.hasAlpha()) {
                        drawPoint(offsetX + cmd.x1 * scale, offsetY + cmd.y1 * scale, Paint().apply { color = cmd.color })
                    }
                }
                LuaDrawCommand.KIND_ELLIPSE -> {
                    val left = offsetX + cmd.x1 * scale
                    val top = offsetY + cmd.y1 * scale
                    val right = left + cmd.x2 * scale
                    val bottom = top + cmd.y2 * scale
                    if (cmd.fillColor.hasAlpha()) {
                        drawOval(RectF(left, top, right, bottom), Paint().apply { color = cmd.fillColor; style = Paint.Style.FILL })
                    }
                    if (cmd.color.hasAlpha()) {
                        drawOval(RectF(left, top, right, bottom), Paint().apply { color = cmd.color; style = Paint.Style.STROKE })
                    }
                }
                LuaDrawCommand.KIND_POLYGON -> {
                    if (cmd.points.size < 4) continue
                    val path = Path()
                    path.moveTo(offsetX + cmd.points[0] * scale, offsetY + cmd.points[1] * scale)
                    var i = 2
                    while (i + 1 < cmd.points.size) {
                        path.lineTo(offsetX + cmd.points[i] * scale, offsetY + cmd.points[i + 1] * scale)
                        i += 2
                    }
                    path.close()
                    if (cmd.fillColor.hasAlpha()) {
                        drawPath(path, Paint().apply { color = cmd.fillColor; style = Paint.Style.FILL })
                    }
                    if (cmd.color.hasAlpha()) {
                        drawPath(path, Paint().apply { color = cmd.color; style = Paint.Style.STROKE })
                    }
                }
                LuaDrawCommand.KIND_IMAGE -> {
                    val bitmap = imageCache.getOrPut(cmd.text) {
                        runCatching { BitmapFactory.decodeFile(cmd.text) }.getOrNull()
                    } ?: continue

                    val src = if (cmd.hasSrcRect) {
                        Rect(cmd.srcX, cmd.srcY, cmd.srcX + cmd.srcW, cmd.srcY + cmd.srcH)
                    } else {
                        Rect(0, 0, bitmap.width, bitmap.height)
                    }
                    val dw = if (cmd.x2 > 0) cmd.x2 else src.width()
                    val dh = if (cmd.y2 > 0) cmd.y2 else src.height()
                    val dst = RectF(
                        offsetX + cmd.x1 * scale, offsetY + cmd.y1 * scale,
                        offsetX + (cmd.x1 + dw) * scale, offsetY + (cmd.y1 + dh) * scale,
                    )
                    drawBitmap(bitmap, src, dst, null)
                }
            }
        }
    }
}
