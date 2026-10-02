package me.magnum.melonds.ui.emulator.lua

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.SnapshotStateList
import androidx.compose.runtime.toMutableStateList
import me.magnum.melonds.domain.model.LuaDrawCommand

/**
 * One forms.newform() window. Rendered as a draggable Compose panel (not a
 * real separate OS window -- see LuaFormsManager/LuaFormsOverlayUi) that
 * hosts a flat list of child widgets, each positioned relative to the
 * panel's own content area by its own x/y.
 */
class LuaForm(val handle: Int, title: String, w: Int, h: Int) {
    var title by mutableStateOf(title)
    var w by mutableStateOf(w)
    var h by mutableStateOf(h)
    var offsetX by mutableStateOf(0f)
    var offsetY by mutableStateOf(0f)
    var visible by mutableStateOf(true)
    val widgets: SnapshotStateList<LuaWidget> = mutableListOf<LuaWidget>().toMutableStateList()
}

sealed class LuaWidget {
    abstract val handle: Int
    abstract var x: Int
    abstract var y: Int
    abstract var enabled: Boolean
    abstract var visible: Boolean

    class Button(override val handle: Int, override var x: Int, override var y: Int, var w: Int, var h: Int, text: String) : LuaWidget() {
        var text by mutableStateOf(text)
        override var enabled by mutableStateOf(true)
        override var visible by mutableStateOf(true)
    }

    class Label(override val handle: Int, override var x: Int, override var y: Int, var w: Int, var h: Int, text: String) : LuaWidget() {
        var text by mutableStateOf(text)
        override var enabled by mutableStateOf(true)
        override var visible by mutableStateOf(true)
    }

    class Checkbox(override val handle: Int, override var x: Int, override var y: Int, text: String) : LuaWidget() {
        var text by mutableStateOf(text)
        var checked by mutableStateOf(false)
        override var enabled by mutableStateOf(true)
        override var visible by mutableStateOf(true)
    }

    class Textbox(override val handle: Int, override var x: Int, override var y: Int, var w: Int, var h: Int, text: String, val multiline: Boolean) : LuaWidget() {
        var text by mutableStateOf(text)
        override var enabled by mutableStateOf(true)
        override var visible by mutableStateOf(true)
    }

    class Dropdown(override val handle: Int, override var x: Int, override var y: Int, var w: Int, var h: Int, items: List<String>) : LuaWidget() {
        var items by mutableStateOf(items)
        var selected by mutableStateOf(items.firstOrNull() ?: "")
        override var enabled by mutableStateOf(true)
        override var visible by mutableStateOf(true)
    }

    class PictureBox(override val handle: Int, override var x: Int, override var y: Int, var w: Int, var h: Int) : LuaWidget() {
        var backgroundColor by mutableStateOf(0)
        val commands: SnapshotStateList<LuaDrawCommand> = mutableListOf<LuaDrawCommand>().toMutableStateList()
        override var enabled by mutableStateOf(true)
        override var visible by mutableStateOf(true)
    }
}
