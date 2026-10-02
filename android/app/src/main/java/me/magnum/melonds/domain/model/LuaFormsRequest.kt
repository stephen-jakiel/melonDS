package me.magnum.melonds.domain.model

/**
 * A single forms.* operation, as handed back by either
 * MelonEmulator.pollLuaFormsRequest() (one in-flight blocking op at a time,
 * answer with MelonEmulator.deliverLuaFormsResult()) or
 * MelonEmulator.takeLuaFormsCommands() (fire-and-forget mutations/drawing,
 * no response expected). See LuaScriptManager.h's FormsOp/FormsRequest for
 * the native side of this -- [op] must match that enum's order exactly.
 *
 * [handle] is the target widget/form handle for mutations and getters, or
 * the *parent form* handle for widget-creation ops (new widgets get their
 * own handle allocated on this, the Kotlin, side -- see LuaFormsManager).
 */
data class LuaFormsRequest(
    val op: Int,
    val handle: Int,
    val x: Int,
    val y: Int,
    val w: Int,
    val h: Int,
    val color: Int,
    val fillColor: Int,
    val text: String,
    val text2: String,
    val items: Array<String>,
    val boolArg: Boolean,
) {
    companion object {
        // Blocking: creation (returns a new handle)
        const val OP_NEW_FORM = 0
        const val OP_BUTTON = 1
        const val OP_LABEL = 2
        const val OP_CHECKBOX = 3
        const val OP_TEXTBOX = 4
        const val OP_DROPDOWN = 5
        const val OP_PICTURE_BOX = 6
        // Blocking: getters
        const val OP_GET_TEXT = 7
        const val OP_IS_CHECKED = 8
        const val OP_GET_MOUSE_X = 9
        const val OP_GET_MOUSE_Y = 10
        const val OP_OPEN_FILE = 11
        // Fire-and-forget: mutations
        const val OP_SET_DROPDOWN_ITEMS = 12
        const val OP_SET_PROPERTY = 13
        const val OP_SET_LOCATION = 14
        const val OP_SET_TEXT = 15
        const val OP_DESTROY = 16
        const val OP_DESTROY_ALL = 17
        // Fire-and-forget: picturebox drawing (handle = the picturebox's own handle)
        const val OP_DRAW_TEXT = 18
        const val OP_DRAW_RECTANGLE = 19
        const val OP_DRAW_ELLIPSE = 20
        const val OP_DRAW_IMAGE = 21
        const val OP_CLEAR = 22
        const val OP_REFRESH = 23
        // Blocking, Android-only (not part of BizHawk's API) -- see
        // LuaScriptManager.h's FormsOp enum comment for why these exist.
        const val OP_HTTP_GET = 24
        const val OP_DOWNLOAD_EXTRACT_UPDATE = 25
        const val OP_OPEN_URL = 26
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as LuaFormsRequest
        return op == other.op && handle == other.handle && x == other.x && y == other.y && w == other.w && h == other.h &&
            color == other.color && fillColor == other.fillColor && text == other.text && text2 == other.text2 &&
            items.contentEquals(other.items) && boolArg == other.boolArg
    }

    override fun hashCode(): Int {
        var result = op
        result = 31 * result + handle
        result = 31 * result + x
        result = 31 * result + y
        result = 31 * result + w
        result = 31 * result + h
        result = 31 * result + color
        result = 31 * result + fillColor
        result = 31 * result + text.hashCode()
        result = 31 * result + text2.hashCode()
        result = 31 * result + items.contentHashCode()
        result = 31 * result + boolArg.hashCode()
        return result
    }
}
