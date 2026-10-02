package me.magnum.melonds.ui.emulator.lua

import androidx.compose.runtime.mutableStateMapOf
import me.magnum.melonds.domain.model.LuaDrawCommand
import me.magnum.melonds.domain.model.LuaFormsRequest

data class LuaFormsResultData(
    val intResult: Int = 0,
    val stringResult: String = "",
    val boolResult: Boolean = false,
)

/**
 * Owns the Compose-observable state for every form a running Lua script
 * has created, and interprets forms.* requests/commands against it. Forms
 * are rendered as draggable in-app panels (LuaFormsOverlayUi.kt), not real
 * separate OS windows -- see LuaScriptManager.h's FormsOp for why, and for
 * the native side of the request/command split this mirrors.
 *
 * Every function here must only be called from the main/Compose thread
 * (it mutates Compose state directly) -- the native side guarantees that by
 * only ever handing requests to Kotlin via polling from there.
 */
object LuaFormsManager {
    val forms = mutableStateMapOf<Int, LuaForm>()
    private var nextHandle = 1

    private fun allocHandle(): Int = nextHandle++

    fun reset() {
        forms.clear()
    }

    private fun findWidget(handle: Int): LuaWidget? {
        for (form in forms.values) {
            form.widgets.find { it.handle == handle }?.let { return it }
        }
        return null
    }

    /** Blocking ops: creation (returns a new handle) and getters. */
    fun processRequest(req: LuaFormsRequest): LuaFormsResultData {
        return when (req.op) {
            LuaFormsRequest.OP_NEW_FORM -> {
                val handle = allocHandle()
                forms[handle] = LuaForm(handle, req.text, req.w, req.h)
                LuaFormsResultData(intResult = handle)
            }
            LuaFormsRequest.OP_BUTTON -> {
                val handle = allocHandle()
                forms[req.handle]?.widgets?.add(LuaWidget.Button(handle, req.x, req.y, req.w, req.h, req.text))
                LuaFormsResultData(intResult = handle)
            }
            LuaFormsRequest.OP_LABEL -> {
                val handle = allocHandle()
                forms[req.handle]?.widgets?.add(LuaWidget.Label(handle, req.x, req.y, req.w, req.h, req.text))
                LuaFormsResultData(intResult = handle)
            }
            LuaFormsRequest.OP_CHECKBOX -> {
                val handle = allocHandle()
                forms[req.handle]?.widgets?.add(LuaWidget.Checkbox(handle, req.x, req.y, req.text))
                LuaFormsResultData(intResult = handle)
            }
            LuaFormsRequest.OP_TEXTBOX -> {
                val handle = allocHandle()
                forms[req.handle]?.widgets?.add(LuaWidget.Textbox(handle, req.x, req.y, req.w, req.h, req.text, req.boolArg))
                LuaFormsResultData(intResult = handle)
            }
            LuaFormsRequest.OP_DROPDOWN -> {
                val handle = allocHandle()
                forms[req.handle]?.widgets?.add(LuaWidget.Dropdown(handle, req.x, req.y, req.w, req.h, req.items.toList()))
                LuaFormsResultData(intResult = handle)
            }
            LuaFormsRequest.OP_PICTURE_BOX -> {
                val handle = allocHandle()
                forms[req.handle]?.widgets?.add(LuaWidget.PictureBox(handle, req.x, req.y, req.w, req.h))
                LuaFormsResultData(intResult = handle)
            }
            LuaFormsRequest.OP_GET_TEXT -> {
                val text = when (val widget = findWidget(req.handle)) {
                    is LuaWidget.Label -> widget.text
                    is LuaWidget.Button -> widget.text
                    is LuaWidget.Textbox -> widget.text
                    is LuaWidget.Checkbox -> widget.text
                    is LuaWidget.Dropdown -> widget.selected
                    else -> forms[req.handle]?.title ?: ""
                }
                LuaFormsResultData(stringResult = text)
            }
            LuaFormsRequest.OP_IS_CHECKED ->
                LuaFormsResultData(boolResult = (findWidget(req.handle) as? LuaWidget.Checkbox)?.checked ?: false)
            LuaFormsRequest.OP_GET_MOUSE_X, LuaFormsRequest.OP_GET_MOUSE_Y ->
                // Not plumbed to a real pointer position yet -- low value
                // until a form can actually be observed receiving touch
                // input distinct from the game surface underneath it.
                LuaFormsResultData(intResult = 0)
            else -> LuaFormsResultData()
        }
    }

    /** Fire-and-forget ops: mutations and picturebox drawing. */
    fun processCommand(req: LuaFormsRequest) {
        when (req.op) {
            LuaFormsRequest.OP_SET_DROPDOWN_ITEMS -> (findWidget(req.handle) as? LuaWidget.Dropdown)?.let {
                it.items = req.items.toList()
                if (it.selected !in it.items) it.selected = it.items.firstOrNull() ?: ""
            }
            LuaFormsRequest.OP_SET_PROPERTY -> applyProperty(req.handle, req.text, req.text2)
            LuaFormsRequest.OP_SET_LOCATION -> {
                val widget = findWidget(req.handle)
                if (widget != null) {
                    widget.x = req.x
                    widget.y = req.y
                } else {
                    forms[req.handle]?.let {
                        it.offsetX = req.x.toFloat()
                        it.offsetY = req.y.toFloat()
                    }
                }
            }
            LuaFormsRequest.OP_SET_TEXT -> {
                when (val widget = findWidget(req.handle)) {
                    is LuaWidget.Label -> widget.text = req.text
                    is LuaWidget.Button -> widget.text = req.text
                    is LuaWidget.Textbox -> widget.text = req.text
                    is LuaWidget.Checkbox -> widget.text = req.text
                    else -> forms[req.handle]?.let { it.title = req.text }
                }
            }
            LuaFormsRequest.OP_DESTROY -> {
                forms.remove(req.handle)
                for (form in forms.values) form.widgets.removeAll { it.handle == req.handle }
            }
            LuaFormsRequest.OP_DESTROY_ALL -> forms.clear()
            LuaFormsRequest.OP_DRAW_TEXT -> appendPictureBoxCommand(
                req.handle,
                LuaDrawCommand(LuaDrawCommand.KIND_TEXT, req.x, req.y, 0, 0, req.color, 0, req.text, FloatArray(0), false, 0, 0, 0, 0, 9),
            )
            LuaFormsRequest.OP_DRAW_RECTANGLE -> appendPictureBoxCommand(
                req.handle,
                LuaDrawCommand(LuaDrawCommand.KIND_RECT, req.x, req.y, req.w, req.h, req.color, req.fillColor, "", FloatArray(0), false, 0, 0, 0, 0, 9),
            )
            LuaFormsRequest.OP_DRAW_ELLIPSE -> appendPictureBoxCommand(
                req.handle,
                LuaDrawCommand(LuaDrawCommand.KIND_ELLIPSE, req.x, req.y, req.w, req.h, req.color, req.fillColor, "", FloatArray(0), false, 0, 0, 0, 0, 9),
            )
            LuaFormsRequest.OP_DRAW_IMAGE -> appendPictureBoxCommand(
                req.handle,
                LuaDrawCommand(LuaDrawCommand.KIND_IMAGE, req.x, req.y, req.w, req.h, 0, 0, req.text, FloatArray(0), false, 0, 0, 0, 0, 9),
            )
            LuaFormsRequest.OP_CLEAR -> (findWidget(req.handle) as? LuaWidget.PictureBox)?.let {
                it.commands.clear()
                it.backgroundColor = req.color
            }
            LuaFormsRequest.OP_REFRESH -> {} // Compose recomposes reactively; nothing extra needed
            else -> {} // blocking ops never reach here
        }
    }

    private fun appendPictureBoxCommand(handle: Int, cmd: LuaDrawCommand) {
        (findWidget(handle) as? LuaWidget.PictureBox)?.commands?.add(cmd)
    }

    private fun applyProperty(handle: Int, name: String, value: String) {
        val widget = findWidget(handle)
        when (name) {
            "Enabled" -> {
                if (widget != null) widget.enabled = value.toBoolean()
            }
            "Visible" -> {
                if (widget != null) widget.visible = value.toBoolean()
                else forms[handle]?.let { it.visible = value.toBoolean() }
            }
            "Checked" -> (widget as? LuaWidget.Checkbox)?.let { it.checked = value.toBoolean() }
            // AutoSize/TabStop/AutoCompleteSource/AutoCompleteMode/etc: no
            // Compose equivalent needed -- widgets already size to content,
            // and no desktop-style autocomplete behavior is implemented.
            else -> {}
        }
    }
}
