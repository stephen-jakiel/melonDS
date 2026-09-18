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
#include <QString>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

struct lua_State;
class EmuInstance;

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
    enum Kind { Text, Rect, Line, Pixel } kind;
    int x1, y1, x2, y2;
    QColor color;
    QColor fillColor;
    QString text;
};

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

    // Parses a BizHawk-style packed 0xAARRGGBB color argument at the given
    // stack index. Absent/nil is treated the same as alpha 0 (invisible),
    // matching how the tracker itself uses 0x00000000 to mean "don't draw
    // this part" (e.g. no outline on a filled rectangle).
    static QColor checkColor(lua_State* L, int idx);

    EmuInstance* emuInstance;
    lua_State* L = nullptr;
    std::thread scriptThread;
    std::atomic<bool> running {false};
    std::atomic<bool> stopRequested {false};
    uint64_t frameCount = 0;

    QMutex drawMutex;
    std::vector<LuaDrawCommand> drawCommands;
};

#endif // LUASCRIPTMANAGER_H
