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

#include "LuaScriptManager.h"

#include <QMutexLocker>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "EmuInstance.h"
#include "EmuThread.h"
#include "NDS.h"

using namespace melonDS;

// Key under which the owning LuaScriptManager* is stashed in the Lua
// registry, so our static C-function bindings (Lua's API only takes plain
// function pointers, no captureable state) can get back to it.
static const char* kSelfRegistryKey = "melonDS.LuaScriptManager";

LuaScriptManager::LuaScriptManager(EmuInstance* inst) : emuInstance(inst)
{
}

LuaScriptManager::~LuaScriptManager()
{
    stop();
}

std::vector<LuaDrawCommand> LuaScriptManager::getDrawCommands()
{
    QMutexLocker locker(&drawMutex);
    return displayCommands;
}

void LuaScriptManager::logf(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    emit consoleOutput(QString::fromUtf8(buf));
}

void LuaScriptManager::start(const QString& scriptPath)
{
    if (running.load())
        return;

    stopRequested.store(false);
    running.store(true);
    frameCount = 0;

    scriptThread = std::thread(&LuaScriptManager::threadMain, this, scriptPath);
}

void LuaScriptManager::stop()
{
    if (!scriptThread.joinable())
        return;

    stopRequested.store(true);
    // In case the script is blocked inside emu.frameadvance() waiting on a
    // frame that will never come because the emulator itself isn't running,
    // nudge it forward so the stop request actually gets seen.
    emuInstance->getEmuThread()->frameAdvanceSemaphore.release();
    scriptThread.join();
    running.store(false);
}

LuaScriptManager* LuaScriptManager::self(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kSelfRegistryKey);
    auto* mgr = static_cast<LuaScriptManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return mgr;
}

void LuaScriptManager::threadMain(QString scriptPath)
{
    if (!emuInstance->getEmuThread()->emuIsActive())
    {
        logf("No game is running -- load a ROM or boot firmware before running a script.");
        running.store(false);
        emit scriptStopped();
        return;
    }

    L = luaL_newstate();
    luaL_openlibs(L);

    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kSelfRegistryKey);

    registerAPI();

    // Redirect Lua's print() to our console signal instead of stdout, which
    // isn't visible to the user in a normal (non-console) Qt build.
    lua_register(L, "print", l_print);

    // Pause the emulator: while this script is active, it alone drives
    // frame progression via emu.frameadvance(), same as BizHawk/mGBA Lua
    // scripts (including against BizHawk's own melonDS-based NDS core,
    // which this script was originally written against).
    emuInstance->getEmuThread()->emuPause();

    std::string path = scriptPath.toStdString();
    if (luaL_loadfile(L, path.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK)
    {
        const char* err = lua_tostring(L, -1);
        logf("Lua error: %s", err ? err : "(unknown error)");
    }

    lua_close(L);
    L = nullptr;

    running.store(false);
    emit scriptStopped();
}

void LuaScriptManager::registerAPI()
{
    static const luaL_Reg memoryFuncs[] = {
        {"usememorydomain", l_memory_usememorydomain},
        {"read_u8", l_memory_read_u8},
        {"read_u16_le", l_memory_read_u16_le},
        {"read_u32_le", l_memory_read_u32_le},
        {"write_u8", l_memory_write_u8},
        {"write_u16_le", l_memory_write_u16_le},
        {"write_u32_le", l_memory_write_u32_le},
        {nullptr, nullptr}
    };
    luaL_newlib(L, memoryFuncs);
    lua_setglobal(L, "memory");

    static const luaL_Reg emuFuncs[] = {
        {"frameadvance", l_emu_frameadvance},
        {"framecount", l_emu_framecount},
        {nullptr, nullptr}
    };
    luaL_newlib(L, emuFuncs);
    lua_setglobal(L, "emu");

    static const luaL_Reg guiFuncs[] = {
        {"drawText", l_gui_drawtext},
        {"drawRectangle", l_gui_drawrectangle},
        {"drawLine", l_gui_drawline},
        {"drawPixel", l_gui_drawpixel},
        {nullptr, nullptr}
    };
    luaL_newlib(L, guiFuncs);
    lua_setglobal(L, "gui");
}

int LuaScriptManager::l_print(lua_State* L)
{
    LuaScriptManager* mgr = self(L);

    int n = lua_gettop(L);
    QString line;
    for (int i = 1; i <= n; i++)
    {
        size_t len;
        // luaL_tolstring converts any value (via __tostring if present) and
        // pushes it, matching what Lua's own print() does internally.
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line += QString::fromUtf8(s, (int)len);
        lua_pop(L, 1);
    }

    emit mgr->consoleOutput(line);
    return 0;
}

// Only the "Main RAM" domain is implemented -- it's the only one the
// Ironmon-Tracker-style scripts this was built for actually use. Anything
// else is accepted (so scripts that defensively probe domain availability
// don't hard-crash) but reads/writes against it will just be no-ops/zeroes,
// which is flagged loudly via the console.
int LuaScriptManager::l_memory_usememorydomain(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    const char* name = luaL_checkstring(L, 1);
    if (strcmp(name, "Main RAM") != 0)
    {
        mgr->logf("memory.usememorydomain(\"%s\"): only \"Main RAM\" is implemented; "
                   "reads/writes will return 0", name);
    }
    return 0;
}

int LuaScriptManager::l_memory_read_u8(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)luaL_checkinteger(L, 1);
    NDS* nds = mgr->emuInstance->getNDS();
    lua_pushinteger(L, nds->MainRAM[addr & nds->MainRAMMask]);
    return 1;
}

int LuaScriptManager::l_memory_read_u16_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)luaL_checkinteger(L, 1);
    NDS* nds = mgr->emuInstance->getNDS();
    u32 a = addr & nds->MainRAMMask;
    u16 val = nds->MainRAM[a] | (nds->MainRAM[(a + 1) & nds->MainRAMMask] << 8);
    lua_pushinteger(L, val);
    return 1;
}

int LuaScriptManager::l_memory_read_u32_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)luaL_checkinteger(L, 1);
    NDS* nds = mgr->emuInstance->getNDS();
    u32 a = addr & nds->MainRAMMask;
    u32 val = nds->MainRAM[a]
        | (nds->MainRAM[(a + 1) & nds->MainRAMMask] << 8)
        | (nds->MainRAM[(a + 2) & nds->MainRAMMask] << 16)
        | (nds->MainRAM[(a + 3) & nds->MainRAMMask] << 24);
    lua_pushinteger(L, val);
    return 1;
}

int LuaScriptManager::l_memory_write_u8(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)luaL_checkinteger(L, 1);
    u8 val = (u8)luaL_checkinteger(L, 2);
    NDS* nds = mgr->emuInstance->getNDS();
    nds->MainRAM[addr & nds->MainRAMMask] = val;
    return 0;
}

int LuaScriptManager::l_memory_write_u16_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)luaL_checkinteger(L, 1);
    u16 val = (u16)luaL_checkinteger(L, 2);
    NDS* nds = mgr->emuInstance->getNDS();
    u32 a = addr & nds->MainRAMMask;
    nds->MainRAM[a] = val & 0xFF;
    nds->MainRAM[(a + 1) & nds->MainRAMMask] = (val >> 8) & 0xFF;
    return 0;
}

int LuaScriptManager::l_memory_write_u32_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)luaL_checkinteger(L, 1);
    u32 val = (u32)luaL_checkinteger(L, 2);
    NDS* nds = mgr->emuInstance->getNDS();
    u32 a = addr & nds->MainRAMMask;
    nds->MainRAM[a] = val & 0xFF;
    nds->MainRAM[(a + 1) & nds->MainRAMMask] = (val >> 8) & 0xFF;
    nds->MainRAM[(a + 2) & nds->MainRAMMask] = (val >> 16) & 0xFF;
    nds->MainRAM[(a + 3) & nds->MainRAMMask] = (val >> 24) & 0xFF;
    return 0;
}

int LuaScriptManager::l_emu_frameadvance(lua_State* L)
{
    LuaScriptManager* mgr = self(L);

    if (mgr->stopRequested.load())
        return luaL_error(L, "script stopped");

    EmuThread* thread = mgr->emuInstance->getEmuThread();
    if (!thread->emuIsActive())
        return luaL_error(L, "no game is running");

    thread->emuFrameStep();
    thread->frameAdvanceSemaphore.acquire();
    mgr->frameCount++;

    // The frame this call just produced should show whatever the script
    // drew (via gui.*) since the *previous* frameadvance() -- i.e. what's
    // sitting in drawCommands right now. Publish it for paint to read, and
    // start collecting fresh for the next frame.
    {
        QMutexLocker locker(&mgr->drawMutex);
        mgr->displayCommands = std::move(mgr->drawCommands);
        mgr->drawCommands.clear();
    }

    if (mgr->stopRequested.load())
        return luaL_error(L, "script stopped");

    return 0;
}

int LuaScriptManager::l_emu_framecount(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    lua_pushinteger(L, (lua_Integer)mgr->frameCount);
    return 1;
}

QColor LuaScriptManager::checkColor(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return QColor(0, 0, 0, 0);

    uint32_t argb = (uint32_t)luaL_checkinteger(L, idx);
    return QColor(
        (argb >> 16) & 0xFF,
        (argb >> 8) & 0xFF,
        argb & 0xFF,
        (argb >> 24) & 0xFF);
}

int LuaScriptManager::l_gui_drawtext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Text;
    cmd.x1 = (int)luaL_checkinteger(L, 1);
    cmd.y1 = (int)luaL_checkinteger(L, 2);
    cmd.text = QString::fromUtf8(luaL_checkstring(L, 3));
    cmd.color = lua_gettop(L) >= 4 ? checkColor(L, 4) : QColor(255, 255, 255, 255);

    QMutexLocker locker(&mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawrectangle(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Rect;
    cmd.x1 = (int)luaL_checkinteger(L, 1);
    cmd.y1 = (int)luaL_checkinteger(L, 2);
    cmd.x2 = (int)luaL_checkinteger(L, 3); // width
    cmd.y2 = (int)luaL_checkinteger(L, 4); // height
    cmd.color = checkColor(L, 5);
    cmd.fillColor = checkColor(L, 6);

    QMutexLocker locker(&mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawline(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Line;
    cmd.x1 = (int)luaL_checkinteger(L, 1);
    cmd.y1 = (int)luaL_checkinteger(L, 2);
    cmd.x2 = (int)luaL_checkinteger(L, 3);
    cmd.y2 = (int)luaL_checkinteger(L, 4);
    cmd.color = lua_gettop(L) >= 5 ? checkColor(L, 5) : QColor(255, 255, 255, 255);

    QMutexLocker locker(&mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawpixel(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Pixel;
    cmd.x1 = (int)luaL_checkinteger(L, 1);
    cmd.y1 = (int)luaL_checkinteger(L, 2);
    cmd.color = lua_gettop(L) >= 3 ? checkColor(L, 3) : QColor(255, 255, 255, 255);

    QMutexLocker locker(&mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}
