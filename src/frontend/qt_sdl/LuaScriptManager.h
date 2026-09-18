/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef LUASCRIPTMANAGER_H
#define LUASCRIPTMANAGER_H

#include <QColor>
#include <QMutex>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

struct lua_State;
class EmuInstance;
class LuaFormsManager;

// A single gui.draw*() call recorded by the script thread, consumed by the
// UI thread when painting the next frame. Coordinates are in NDS-native
// pixel space (256 wide, with the top screen at y=[0,192) and the bottom
// screen at y=[192,384), matching BizHawk's addressing convention) --
// currently only rendered when using the software (non-OpenGL) renderer,
// scaled to fill the screen panel assuming the standard top-then-bottom
// layout. Custom/rotated layouts and the OpenGL renderer are not yet
// supported.
struct LuaDrawCommand
{
    enum Kind { Text, Rect, Line, Pixel, Ellipse, Polygon, Image } kind;
    int x1, y1, x2, y2; // Image: x1,y1=dest pos, x2,y2=dest size (0=natural)
    QColor color;
    QColor fillColor;
    QString text;  // Text: the string. Image: the file path.
    std::vector<QPointF> points; // Polygon only
    QRect srcRect; // Image only, source crop region; invalid = whole image
};

class QPainter;
class QImage;

// Renders a batch of LuaDrawCommands via the given QPainter, in whatever
// pixel space the caller's painter transform already maps to. Shared
// between the gui.* screen overlay (Screen.cpp) and forms.* pictureBox
// widgets (LuaFormsManager.cpp) so both stay visually consistent.
void paintLuaDrawCommands(QPainter& painter, const std::vector<LuaDrawCommand>& commands);

// Shared by gui.drawImage/drawImageRegion and forms.drawImage, and by
// paintLuaDrawCommands() itself. Cache is keyed by path and shared
// process-wide (not per-script) -- fine, since scripts don't mutate loaded
// images, just redraw them each frame.
QImage getCachedLuaImage(const QString& path);

// Runs a single Lua script against this emulator instance, on its own
// thread. Scripts are expected to drive their own main loop by calling
// emu.frameadvance() repeatedly (the same model BizHawk/mGBA Lua scripts,
// including BizHawk's melonDS-based NDS core, already use) -- so while a
// script is active, frame progression is entirely script-driven rather than
// free-running: start() pauses the emulator and each emu.frameadvance()
// call steps exactly one frame and waits for it to finish before returning.
class LuaScriptManager : public QObject
{
    Q_OBJECT

public:
    explicit LuaScriptManager(EmuInstance* inst);
    ~LuaScriptManager() override;

    bool isRunning() const { return running.load(); }

    // Starts running scriptPath on a new thread. No-op if a script is
    // already running (call stop() first).
    void start(const QString& scriptPath);

    // Asks the running script's thread to stop at its next frameadvance()
    // call, then waits for it to exit. Safe to call even if nothing is
    // running.
    void stop();

    // Thread-safe: called from the UI thread during paint. Returns a copy
    // of whatever the script has drawn since the last frameadvance().
    std::vector<LuaDrawCommand> getDrawCommands();

signals:
    // Emitted (queued, safe to connect to UI slots) whenever the script
    // calls print(), or when it errors out / finishes.
    void consoleOutput(QString text);
    void scriptStopped();
    void consoleCleared();

private:
    void threadMain(QString scriptPath);
    void registerAPI();
    void logf(const char* fmt, ...);

    static LuaScriptManager* self(lua_State* L);

    static int l_print(lua_State* L);
    static int l_memory_usememorydomain(lua_State* L);
    static int l_memory_read_u8(lua_State* L);
    static int l_memory_read_u16_le(lua_State* L);
    static int l_memory_read_u32_le(lua_State* L);
    static int l_memory_write_u8(lua_State* L);
    static int l_memory_write_u16_le(lua_State* L);
    static int l_memory_write_u32_le(lua_State* L);
    static int l_emu_frameadvance(lua_State* L);
    static int l_emu_framecount(lua_State* L);
    static int l_gui_drawtext(lua_State* L);
    static int l_gui_drawrectangle(lua_State* L);
    static int l_gui_drawline(lua_State* L);
    static int l_gui_drawpixel(lua_State* L);

    static int l_forms_newform(lua_State* L);
    static int l_forms_button(lua_State* L);
    static int l_forms_label(lua_State* L);
    static int l_forms_checkbox(lua_State* L);
    static int l_forms_textbox(lua_State* L);
    static int l_forms_dropdown(lua_State* L);
    static int l_forms_setdropdownitems(lua_State* L);
    static int l_forms_picturebox(lua_State* L);
    static int l_forms_setproperty(lua_State* L);
    static int l_forms_setlocation(lua_State* L);
    static int l_forms_settext(lua_State* L);
    static int l_forms_gettext(lua_State* L);
    static int l_forms_ischecked(lua_State* L);
    static int l_forms_destroy(lua_State* L);
    static int l_forms_destroyall(lua_State* L);
    static int l_forms_addclick(lua_State* L);
    static int l_forms_getmousex(lua_State* L);
    static int l_forms_getmousey(lua_State* L);
    static int l_forms_openfile(lua_State* L);
    static int l_forms_drawtext(lua_State* L);
    static int l_forms_drawrectangle(lua_State* L);
    static int l_forms_drawellipse(lua_State* L);
    static int l_forms_drawimage(lua_State* L);
    static int l_forms_clear(lua_State* L);
    static int l_forms_refresh(lua_State* L);

    static int l_gui_clearimagecache(lua_State* L);
    static int l_gui_drawimage(lua_State* L);
    static int l_gui_drawimageregion(lua_State* L);
    static int l_gui_drawpolygon(lua_State* L);

    static int l_client_bufferwidth(lua_State* L);
    static int l_client_closerom(lua_State* L);
    static int l_client_openrom(lua_State* L);
    static int l_client_pause(lua_State* L);

    // Real BizHawk global API calls (unlike the many other event.* uses
    // across the tracker's source, which are a local variable shadowing
    // this global within specific functions -- checked each one).
    static int l_event_onexit(lua_State* L);
    static int l_event_onconsoleclose(lua_State* L);

    static int l_console_clear(lua_State* L);
    static int l_gameinfo_getromname(lua_State* L);
    static int l_gameinfo_getromhash(lua_State* L);

    // bit.*: Lua 5.4 has native bitwise operators, but BizHawk scripts
    // (written against Lua 5.1, which lacked them) commonly still use this
    // library-style API, so it's provided as a thin wrapper.
    static int l_bit_band(lua_State* L);
    static int l_bit_bor(lua_State* L);
    static int l_bit_bxor(lua_State* L);
    static int l_bit_lshift(lua_State* L);
    static int l_bit_rshift(lua_State* L);

    static int l_client_setgameextrapadding(lua_State* L);
    static int l_client_setsoundon(lua_State* L);
    static int l_client_getsoundon(lua_State* L);
    static int l_client_unpause(lua_State* L);
    static int l_client_getversion(lua_State* L);
    static int l_client_get_approx_framerate(lua_State* L);
    static int l_client_xpos(lua_State* L);
    static int l_client_ypos(lua_State* L);
    static int l_client_screenwidth(lua_State* L);
    static int l_client_screenheight(lua_State* L);
    static int l_client_saveram(lua_State* L);
    static int l_savestate_save(lua_State* L);
    static int l_savestate_load(lua_State* L);
    static int l_joypad_get(lua_State* L);
    static int l_input_getmouse(lua_State* L);
    // comm.* (Streamerbot/Crowd Control network integration): stubbed as
    // harmless no-ops/empty results. That's an optional streaming feature
    // this fork doesn't implement, not something the live tracker needs.
    static int l_comm_stub_bool(lua_State* L);
    static int l_comm_stub_table(lua_State* L);
    static int l_comm_stub_noop(lua_State* L);

    // Parses a BizHawk-style packed 0xAARRGGBB color argument at the given
    // stack index. Absent/nil is treated the same as alpha 0 (invisible),
    // matching how the tracker itself uses 0x00000000 to mean "don't draw
    // this part" (e.g. no outline on a filled rectangle).
    static QColor checkColor(lua_State* L, int idx);
    // Accepts either a plain array table ({"a","b"}) or an associative one
    // ({[key]=val, ...}), matching how the tracker calls forms.dropdown/
    // forms.setdropdownitems both ways in practice.
    static QStringList checkStringList(lua_State* L, int idx);

    // Invokes every Lua callback (button clicks, form close) queued by the
    // UI thread since the last call. Must only run on the script thread
    // (Lua state isn't safe to touch from anywhere else); called once per
    // emu.frameadvance(), same as gui.* command publishing.
    void dispatchFormsCallbacks();

    EmuInstance* emuInstance;
    lua_State* L = nullptr;
    std::thread scriptThread;
    std::atomic<bool> running {false};
    std::atomic<bool> stopRequested {false};
    uint64_t frameCount = 0;

    // drawCommands accumulates the script's gui.draw*() calls for the frame
    // currently being stepped into. Right after that frame finishes,
    // frameadvance() moves it into displayCommands (what paint reads) and
    // starts drawCommands fresh -- so paint always sees a complete set from
    // one specific frame, never a partially-drawn/just-cleared one, even
    // though the script thread and UI thread run concurrently.
    QMutex drawMutex;
    std::vector<LuaDrawCommand> drawCommands;
    std::vector<LuaDrawCommand> displayCommands;

    std::unique_ptr<LuaFormsManager> formsManager;
    QString scriptDir;
    std::vector<int> exitCallbackRefs; // event.onexit/onconsoleclose
};

#endif // LUASCRIPTMANAGER_H
