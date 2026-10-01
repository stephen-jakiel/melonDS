package me.magnum.melonds.domain.model

/**
 * A single gui.draw*() call recorded by a running Lua script, as handed
 * back by MelonEmulator.getLuaDrawCommands() (constructed on the native
 * side via JNI, see MelonDSAndroidJNI.cpp). Coordinates are in NDS-native
 * pixel space (256 wide, top screen y=[0,192), bottom screen y=[192,384)).
 *
 * [color]/[fillColor] are packed 0xAARRGGBB, directly usable as an Android/
 * Compose Color int. [points] is a flat x0,y0,x1,y1,... array (Polygon
 * only). [hasSrcRect]/[srcX]/[srcY]/[srcW]/[srcH] describe an optional
 * source crop region (Image only).
 */
data class LuaDrawCommand(
    val kind: Int,
    val x1: Int,
    val y1: Int,
    val x2: Int,
    val y2: Int,
    val color: Int,
    val fillColor: Int,
    val text: String,
    val points: FloatArray,
    val hasSrcRect: Boolean,
    val srcX: Int,
    val srcY: Int,
    val srcW: Int,
    val srcH: Int,
) {
    companion object {
        const val KIND_TEXT = 0
        const val KIND_RECT = 1
        const val KIND_LINE = 2
        const val KIND_PIXEL = 3
        const val KIND_ELLIPSE = 4
        const val KIND_POLYGON = 5
        const val KIND_IMAGE = 6
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as LuaDrawCommand
        return kind == other.kind && x1 == other.x1 && y1 == other.y1 && x2 == other.x2 && y2 == other.y2 &&
            color == other.color && fillColor == other.fillColor && text == other.text &&
            points.contentEquals(other.points) && hasSrcRect == other.hasSrcRect &&
            srcX == other.srcX && srcY == other.srcY && srcW == other.srcW && srcH == other.srcH
    }

    override fun hashCode(): Int {
        var result = kind
        result = 31 * result + x1
        result = 31 * result + y1
        result = 31 * result + x2
        result = 31 * result + y2
        result = 31 * result + color
        result = 31 * result + fillColor
        result = 31 * result + text.hashCode()
        result = 31 * result + points.contentHashCode()
        result = 31 * result + hasSrcRect.hashCode()
        result = 31 * result + srcX
        result = 31 * result + srcY
        result = 31 * result + srcW
        result = 31 * result + srcH
        return result
    }
}
