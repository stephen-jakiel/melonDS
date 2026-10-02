package me.magnum.melonds.ui.emulator.lua

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.Orientation
import androidx.compose.foundation.gestures.draggable
import androidx.compose.foundation.gestures.rememberDraggableState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredHeight
import androidx.compose.foundation.layout.requiredWidth
import androidx.compose.foundation.layout.size
import androidx.compose.material.Icon
import androidx.compose.material.IconButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.LuaDrawCommand
import kotlin.math.roundToInt

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
// Extra slack (NDS-space units) added to the drag range beyond exactly
// "canvas x=0 at screen x=0" -- some screens draw content a little past
// their box's nominal left edge, which a hard clamp at precisely x=0
// leaves just out of reach. This tracker's Statistics bar graph is the
// confirmed case: both its row labels AND a per-row count further left
// of them land well negative (100 units of buffer still wasn't enough to
// reach the count). Padding generously rather than hand-deriving the
// exact figure through several layers of nested Lua frame math.
private const val EXTRA_DRAG_BUFFER_NDS_UNITS = 220f

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
    // Opt-in signal (android.setOverlayScrollEnabled(), re-asserted every
    // frame same as client.SetGameExtraPadding()) from specific screens
    // that draw real content into the region the overlay otherwise
    // sacrifices off-screen -- this tracker's Statistics and Log Viewer.
    // Off by default: the overlay is technically wider than the viewport
    // on every screen (that's why it's right-anchored at all), but most
    // screens don't need dragging since nothing of theirs lives over there.
    var scrollEnabled by remember { mutableStateOf(false) }
    val imageCache = remember { mutableMapOf<String, Bitmap?>() }

    LaunchedEffect(Unit) {
        while (isActive) {
            commands = MelonEmulator.getLuaDrawCommands().asList()
            padding = MelonEmulator.getLuaScreenPadding()
            scrollEnabled = MelonEmulator.isLuaOverlayScrollEnabled()
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
                // keeps content a sensible size; right-aligned (below) since
                // the padded area this tracker actually uses sits to the
                // right of the game screen, so anything that doesn't fit is
                // the (less important) game-aligned left portion, not it
                // *for most screens* -- a few (this tracker's Statistics and
                // Log Viewer) draw their real content into the game-aligned
                // side instead, so that side is never fully hidden: it's
                // draggable into view instead of being cut off outright.
                val density = LocalDensity.current
                val availableWidthPx = with(density) { maxWidth.toPx() }
                val availableHeightPx = with(density) { maxHeight.toPx() }
                val scale = if (totalHeight > 0f) (availableHeightPx / totalHeight) * OVERLAY_SCALE_FACTOR else 1f
                val canvasWidthPx = totalWidth * scale
                // Explicit pixel offset rather than Alignment.TopEnd: with a
                // child wider than its parent, alignment here didn't anchor
                // the right edges together the way it should have.
                //
                // This base offset is recomputed fresh from BoxWithConstraints'
                // own measurements every recomposition (same as it always
                // was) -- the one-time manual drag adjustment below is kept
                // entirely separate from it (and always starts at a constant
                // 0f) specifically so it can never capture a stale/wrong
                // value from an unsettled first measurement pass the way a
                // single remembered combined offset could.
                val baseXOffsetPx = availableWidthPx - canvasWidthPx
                val contentOffsetX = padLeft * scale
                val contentOffsetY = padTop * scale

                // Manual horizontal pan on top of the base (right-anchored)
                // position, so the game-aligned side is reachable by
                // dragging instead of only ever being cut off. Resets to 0
                // (i.e. back to the default right-anchored view) whenever
                // totalWidth changes -- padding changing means the script
                // switched screens, so this is "every screen opens at its
                // normal default look" rather than carrying a drag position
                // from a totally different screen's layout onto this one.
                var dragOffsetPx by remember(totalWidth) { mutableStateOf(0f) }
                // Gated on the explicit opt-in signal (see scrollEnabled
                // above), not on whether content technically overflows --
                // that's true on every screen given how this overlay is
                // scaled/anchored, so it can't tell "normal screen" from
                // "Statistics" apart on its own.
                val maxDragPx = if (scrollEnabled) -baseXOffsetPx + EXTRA_DRAG_BUFFER_NDS_UNITS * scale else 0f
                val draggableState = rememberDraggableState { delta ->
                    dragOffsetPx = (dragOffsetPx + delta).coerceIn(0f, maxDragPx)
                }
                val xOffsetPx = (baseXOffsetPx + dragOffsetPx).roundToInt()

                // pointerInput below only READS position/pressed state and
                // never calls change.consume() -- it must stay a passive
                // observer, not steal touches from the real DS touchscreen
                // handling (a separate, non-Compose view) underneath.
                // rememberUpdatedState lets that long-lived gesture loop
                // (keyed on Unit, so it doesn't restart every recomposition)
                // see the current scale/offsets rather than stale ones from
                // whenever it first launched.
                val latestScale = rememberUpdatedState(scale)
                val latestContentOffsetX = rememberUpdatedState(contentOffsetX)
                val latestContentOffsetY = rememberUpdatedState(contentOffsetY)

                Canvas(
                    modifier = Modifier
                        .offset { IntOffset(xOffsetPx, 0) }
                        .requiredWidth(with(density) { canvasWidthPx.toDp() })
                        .requiredHeight(with(density) { availableHeightPx.toDp() })
                        // Only attached when there's actually somewhere to
                        // drag to -- on every screen that already fits
                        // (the common case), this gesture recognizer would
                        // otherwise sit on top of taps for no reason.
                        .then(if (maxDragPx > 0f) Modifier.draggable(state = draggableState, orientation = Orientation.Horizontal) else Modifier)
                        .pointerInput(Unit) {
                            awaitPointerEventScope {
                                while (true) {
                                    val event = awaitPointerEvent(PointerEventPass.Initial)
                                    val change = event.changes.firstOrNull() ?: continue
                                    // change.position is already local to this Canvas
                                    // (i.e. post-offset/post-size), so only the drawing
                                    // content offset needs inverting here, not xOffsetPx.
                                    val logicalX = (change.position.x - latestContentOffsetX.value) / latestScale.value
                                    val logicalY = (change.position.y - latestContentOffsetY.value) / latestScale.value
                                    MelonEmulator.setLuaMousePosition(logicalX, logicalY, change.pressed)
                                }
                            }
                        },
                ) {
                    drawLuaCommandsAt(commands, imageCache, contentOffsetX, contentOffsetY, scale)
                }
            }
        }

        // Sits immediately to the right of PauseMenuButtonUi's icon (same
        // top-left corner, same size) rather than the opposite corner, so
        // both toggle icons are grouped together.
        IconButton(
            onClick = { expanded = !expanded },
            modifier = Modifier.align(Alignment.TopStart).padding(start = 40.dp, top = 4.dp).size(32.dp),
        ) {
            Icon(
                imageVector = if (expanded) Icons.Default.Visibility else Icons.Default.VisibilityOff,
                contentDescription = null,
                tint = Color.White,
                modifier = Modifier.size(20.dp),
            )
        }

        // This tracker's own "start a new run" trigger is normally a
        // simultaneous Start+Select+A+B hold on the real controller (see
        // Main.lua's patched checkForNextSeedCombo()), impractical on a
        // touchscreen -- this icon fires the same trigger directly via the
        // android.consumeNewRunRequested() bridge instead of the real NDS
        // input, so it can't have any side effect on the running game.
        IconButton(
            onClick = { MelonEmulator.requestLuaNewRun() },
            modifier = Modifier.align(Alignment.TopStart).padding(start = 76.dp, top = 4.dp).size(32.dp),
        ) {
            Icon(
                imageVector = Icons.Default.Refresh,
                contentDescription = null,
                tint = Color.White,
                modifier = Modifier.size(20.dp),
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
                        textSize = cmd.fontSize * scale
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
                    // A single NDS-native pixel should cover `scale` device
                    // pixels at the overlay's current zoom -- drawPoint()'s
                    // dot size doesn't scale with it (stays a fixed ~1px
                    // regardless), which made anything built pixel-by-pixel
                    // out of many drawPixel() calls (e.g. this tracker's
                    // icons, via IconDrawer.drawIcon) a near-invisible
                    // scatter of specks once zoomed in. A filled square of
                    // side `scale` is what "one pixel" actually means here.
                    if (cmd.color.hasAlpha()) {
                        val left = offsetX + cmd.x1 * scale
                        val top = offsetY + cmd.y1 * scale
                        drawRect(left, top, left + scale, top + scale, Paint().apply { color = cmd.color; style = Paint.Style.FILL })
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
