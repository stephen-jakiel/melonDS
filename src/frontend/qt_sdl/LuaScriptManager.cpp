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

#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "EmuInstance.h"
#include "EmuThread.h"
#include "LuaFormsManager.h"
#include "NDS.h"

using namespace melonDS;

// Key under which the owning LuaScriptManager* is stashed in the Lua
// registry, so our static C-function bindings (Lua's API only takes plain
// function pointers, no captureable state) can get back to it.
static const char* kSelfRegistryKey = "melonDS.LuaScriptManager";

LuaScriptManager::LuaScriptManager(EmuInstance* inst) : emuInstance(inst)
{
    // Constructed here on the UI thread (LuaScriptManager itself is always
    // constructed from EmuInstance's constructor), giving it the thread
    // affinity its Qt::BlockingQueuedConnection bridge to the script thread
    // depends on.
    formsManager = std::make_unique<LuaFormsManager>();
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

    // Matches BizHawk's behavior: a script's forms don't outlive it.
    formsManager->destroyAll();
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
        logf("No game running yet -- waiting for a ROM to be loaded or firmware booted...");

    // Most BizHawk-style scripts (this tracker included) load their own
    // submodules and read/write their own data files using paths relative
    // to the *script's own* directory, e.g. dofile("ironmon_tracker/Main.lua")
    // or io.open("Settings.ini") -- which only resolve correctly if that's
    // also the process's current working directory. melonDS's own file
    // handling doesn't depend on the working directory (it resolves
    // everything against an absolute emuDirectory instead), so changing it
    // here is safe.
    scriptDir = QFileInfo(scriptPath).absolutePath();
    QDir::setCurrent(scriptDir);

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

    static const luaL_Reg formsFuncs[] = {
        {"newform", l_forms_newform},
        {"button", l_forms_button},
        {"label", l_forms_label},
        {"checkbox", l_forms_checkbox},
        {"textbox", l_forms_textbox},
        {"dropdown", l_forms_dropdown},
        {"setdropdownitems", l_forms_setdropdownitems},
        {"pictureBox", l_forms_picturebox},
        {"setproperty", l_forms_setproperty},
        {"setlocation", l_forms_setlocation},
        {"settext", l_forms_settext},
        {"gettext", l_forms_gettext},
        {"ischecked", l_forms_ischecked},
        {"destroy", l_forms_destroy},
        {"destroyall", l_forms_destroyall},
        {"addclick", l_forms_addclick},
        {"getMouseX", l_forms_getmousex},
        {"getMouseY", l_forms_getmousey},
        {"openfile", l_forms_openfile},
        {"drawText", l_forms_drawtext},
        {"drawRectangle", l_forms_drawrectangle},
        {"refresh", l_forms_refresh},
        {nullptr, nullptr}
    };
    luaL_newlib(L, formsFuncs);
    lua_setglobal(L, "forms");

    static const luaL_Reg clientFuncs[] = {
        {"SetGameExtraPadding", l_client_setgameextrapadding},
        {"SetSoundOn", l_client_setsoundon},
        {"GetSoundOn", l_client_getsoundon},
        {"unpause", l_client_unpause},
        {"getversion", l_client_getversion},
        {"get_approx_framerate", l_client_get_approx_framerate},
        {"xpos", l_client_xpos},
        {"ypos", l_client_ypos},
        {"screenwidth", l_client_screenwidth},
        {"screenheight", l_client_screenheight},
        {"saveram", l_client_saveram},
        {nullptr, nullptr}
    };
    luaL_newlib(L, clientFuncs);
    lua_setglobal(L, "client");

    static const luaL_Reg savestateFuncs[] = {
        {"save", l_savestate_save},
        {"load", l_savestate_load},
        {nullptr, nullptr}
    };
    luaL_newlib(L, savestateFuncs);
    lua_setglobal(L, "savestate");

    static const luaL_Reg joypadFuncs[] = {
        {"get", l_joypad_get},
        {nullptr, nullptr}
    };
    luaL_newlib(L, joypadFuncs);
    lua_setglobal(L, "joypad");

    static const luaL_Reg inputFuncs[] = {
        {"getmouse", l_input_getmouse},
        {nullptr, nullptr}
    };
    luaL_newlib(L, inputFuncs);
    lua_setglobal(L, "input");

    static const luaL_Reg commFuncs[] = {
        {"httpTest", l_comm_stub_bool},
        {"socketServerSetTimeout", l_comm_stub_noop},
        {"socketServerSend", l_comm_stub_noop},
        {"socketServerIsConnected", l_comm_stub_bool},
        {"socketServerGetInfo", l_comm_stub_table},
        {nullptr, nullptr}
    };
    luaL_newlib(L, commFuncs);
    lua_setglobal(L, "comm");
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

    // Dispatch any pending forms.* callbacks (button clicks, form close)
    // queued by the UI thread since the last call -- always, even while
    // idling for a ROM to load, so e.g. a settings form stays responsive.
    mgr->dispatchFormsCallbacks();

    EmuThread* thread = mgr->emuInstance->getEmuThread();
    if (!thread->emuIsActive())
    {
        // No game loaded yet (or the cart was ejected mid-script) -- wait
        // rather than erroring out, so a script started before loading a
        // ROM (a very natural thing to do) just idles until one is loaded,
        // matching how BizHawk/mGBA scripts can always frame-advance.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if (mgr->stopRequested.load())
            return luaL_error(L, "script stopped");
        return 0;
    }

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

// --- forms.* -----------------------------------------------------------
//
// Every binding below marshals the actual QWidget work onto the UI thread
// via LuaFormsManager::runOnUI (a blocking cross-thread call: it's always
// safe to capture-by-value locals here, since the calling script thread is
// suspended for the whole call, not racing the UI thread against them).

// Pops a function argument (if present) into the Lua registry so it can be
// invoked later from dispatchFormsCallbacks(); returns 0 (an invalid ref)
// for a missing/nil argument.
static int refFunctionArg(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return 0;
    luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_pushvalue(L, idx);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

QStringList LuaScriptManager::checkStringList(lua_State* L, int idx)
{
    QStringList result;
    if (lua_isnoneornil(L, idx) || !lua_istable(L, idx))
        return result;

    lua_Integer n = lua_rawlen(L, idx);
    for (lua_Integer i = 1; i <= n; i++)
    {
        lua_rawgeti(L, idx, (int)i);
        if (lua_type(L, -1) == LUA_TSTRING)
            result << QString::fromUtf8(lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    if (result.isEmpty())
    {
        // Not a plain array -- fall back to iterating all values (covers an
        // associative table like {[key]=val, ...}).
        lua_pushnil(L);
        while (lua_next(L, idx) != 0)
        {
            if (lua_type(L, -1) == LUA_TSTRING)
                result << QString::fromUtf8(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    return result;
}

void LuaScriptManager::dispatchFormsCallbacks()
{
    for (int ref : formsManager->takePendingCallbacks())
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            logf("Lua error in forms callback: %s", err ? err : "(unknown error)");
            lua_pop(L, 1);
        }
    }
}

int LuaScriptManager::l_forms_newform(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    QString title = QString::fromUtf8(luaL_optstring(L, 3, "Script Window"));
    int onCloseRef = refFunctionArg(L, 4);

    auto* fm = mgr->formsManager.get();
    int handle = fm->runOnUI([=]() { return fm->newForm(w, h, title, onCloseRef); });
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_button(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int form = (int)luaL_checkinteger(L, 1);
    QString caption = QString::fromUtf8(luaL_optstring(L, 2, ""));
    int onClickRef = refFunctionArg(L, 3);
    int x = (int)luaL_optinteger(L, 4, 0);
    int y = (int)luaL_optinteger(L, 5, 0);
    int w = (int)luaL_optinteger(L, 6, 75);
    int h = (int)luaL_optinteger(L, 7, 23);

    auto* fm = mgr->formsManager.get();
    int handle = fm->runOnUI([=]() { return fm->addButton(form, caption, onClickRef, x, y, w, h); });
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_label(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int form = (int)luaL_checkinteger(L, 1);
    QString caption = QString::fromUtf8(luaL_optstring(L, 2, ""));
    int x = (int)luaL_optinteger(L, 3, 0);
    int y = (int)luaL_optinteger(L, 4, 0);
    int w = (int)luaL_optinteger(L, 5, 0);
    int h = (int)luaL_optinteger(L, 6, 0);

    auto* fm = mgr->formsManager.get();
    int handle = fm->runOnUI([=]() { return fm->addLabel(form, caption, x, y, w, h); });
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_checkbox(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int form = (int)luaL_checkinteger(L, 1);
    QString caption = QString::fromUtf8(luaL_optstring(L, 2, ""));
    int x = (int)luaL_optinteger(L, 3, 0);
    int y = (int)luaL_optinteger(L, 4, 0);

    auto* fm = mgr->formsManager.get();
    int handle = fm->runOnUI([=]() { return fm->addCheckbox(form, caption, x, y); });
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_textbox(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int form = (int)luaL_checkinteger(L, 1);
    QString caption = QString::fromUtf8(luaL_optstring(L, 2, ""));
    int w = (int)luaL_optinteger(L, 3, 100);
    int h = (int)luaL_optinteger(L, 4, 20);
    // arg 5 ("type", e.g. "HEX"/"NUMBER" input filtering) intentionally
    // ignored -- just a plain text field, no input validation.
    int x = (int)luaL_optinteger(L, 6, 0);
    int y = (int)luaL_optinteger(L, 7, 0);
    bool multiline = lua_gettop(L) >= 8 && lua_toboolean(L, 8);

    auto* fm = mgr->formsManager.get();
    int handle = fm->runOnUI([=]() { return fm->addTextbox(form, caption, w, h, x, y, multiline); });
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_dropdown(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int form = (int)luaL_checkinteger(L, 1);
    QStringList items = checkStringList(L, 2);
    int x = (int)luaL_optinteger(L, 3, 0);
    int y = (int)luaL_optinteger(L, 4, 0);
    int w = (int)luaL_optinteger(L, 5, 100);
    int h = (int)luaL_optinteger(L, 6, 20);

    auto* fm = mgr->formsManager.get();
    int handle = fm->runOnUI([=]() { return fm->addDropdown(form, items, x, y, w, h); });
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_setdropdownitems(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);
    QStringList items = checkStringList(L, 2);

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->setDropdownItems(handle, items); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_picturebox(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int form = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int w = (int)luaL_checkinteger(L, 4);
    int h = (int)luaL_checkinteger(L, 5);

    auto* fm = mgr->formsManager.get();
    int handle = fm->runOnUI([=]() { return fm->addPictureBox(form, x, y, w, h); });
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_setproperty(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);
    QString prop = QString::fromUtf8(luaL_checkstring(L, 2));

    QString value;
    if (lua_isboolean(L, 3))
        value = lua_toboolean(L, 3) ? "true" : "false";
    else
    {
        size_t len;
        const char* s = luaL_tolstring(L, 3, &len);
        value = QString::fromUtf8(s, (int)len);
        lua_pop(L, 1);
    }

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->setProperty(handle, prop, value); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_setlocation(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->setLocation(handle, x, y); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_settext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);
    QString text = QString::fromUtf8(luaL_checkstring(L, 2));

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->setText(handle, text); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_gettext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);

    auto* fm = mgr->formsManager.get();
    QString text = fm->runOnUI([=]() { return fm->getText(handle); });
    lua_pushstring(L, text.toUtf8().constData());
    return 1;
}

int LuaScriptManager::l_forms_ischecked(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);

    auto* fm = mgr->formsManager.get();
    bool checked = fm->runOnUI([=]() { return fm->isChecked(handle); });
    lua_pushboolean(L, checked);
    return 1;
}

int LuaScriptManager::l_forms_destroy(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->destroyHandle(handle); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_destroyall(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->destroyAll(); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_addclick(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);
    int ref = refFunctionArg(L, 2);
    if (ref == 0)
        return 0;

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->setClickCallback(handle, ref); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_getmousex(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);

    auto* fm = mgr->formsManager.get();
    int x = fm->runOnUI([=]() { return fm->getMouseX(handle); });
    lua_pushinteger(L, x);
    return 1;
}

int LuaScriptManager::l_forms_getmousey(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);

    auto* fm = mgr->formsManager.get();
    int y = fm->runOnUI([=]() { return fm->getMouseY(handle); });
    lua_pushinteger(L, y);
    return 1;
}

int LuaScriptManager::l_forms_openfile(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    QString initialDir = (lua_gettop(L) >= 2 && lua_isstring(L, 2)) ? QString::fromUtf8(lua_tostring(L, 2)) : QString();
    QString filter = (lua_gettop(L) >= 3 && lua_isstring(L, 3)) ? QString::fromUtf8(lua_tostring(L, 3)) : QString();
    QString title = (lua_gettop(L) >= 4 && lua_isstring(L, 4)) ? QString::fromUtf8(lua_tostring(L, 4)) : QString();

    auto* fm = mgr->formsManager.get();
    QString path = fm->runOnUI([=]() { return fm->openFile(initialDir, filter, title); });
    lua_pushstring(L, path.toUtf8().constData());
    return 1;
}

int LuaScriptManager::l_forms_drawtext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Text;
    cmd.x1 = (int)luaL_checkinteger(L, 2);
    cmd.y1 = (int)luaL_checkinteger(L, 3);
    cmd.text = QString::fromUtf8(luaL_checkstring(L, 4));
    cmd.color = lua_gettop(L) >= 5 ? checkColor(L, 5) : QColor(255, 255, 255, 255);

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->addDrawCommand(handle, cmd); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_drawrectangle(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Rect;
    cmd.x1 = (int)luaL_checkinteger(L, 2);
    cmd.y1 = (int)luaL_checkinteger(L, 3);
    cmd.x2 = (int)luaL_checkinteger(L, 4);
    cmd.y2 = (int)luaL_checkinteger(L, 5);
    cmd.color = checkColor(L, 6);
    cmd.fillColor = checkColor(L, 7);

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->addDrawCommand(handle, cmd); return 0; });
    return 0;
}

int LuaScriptManager::l_forms_refresh(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)luaL_checkinteger(L, 1);

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { fm->refresh(handle); return 0; });
    return 0;
}

// --- client.* / savestate.* / joypad.* / input.* / comm.* --------------

int LuaScriptManager::l_client_setgameextrapadding(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int left = (int)luaL_checkinteger(L, 1);
    int top = (int)luaL_checkinteger(L, 2);
    int right = (int)luaL_checkinteger(L, 3);
    int bottom = (int)luaL_checkinteger(L, 4);

    EmuInstance* inst = mgr->emuInstance;
    inst->luaPadLeft = left;
    inst->luaPadTop = top;
    inst->luaPadRight = right;
    inst->luaPadBottom = bottom;

    auto* fm = mgr->formsManager.get();
    fm->runOnUI([=]() { inst->getMainWindow()->updateScreenLayout(); return 0; });
    return 0;
}

int LuaScriptManager::l_client_setsoundon(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    bool on = lua_toboolean(L, 1);
    mgr->emuInstance->setAudioMuted(!on);
    return 0;
}

int LuaScriptManager::l_client_getsoundon(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    lua_pushboolean(L, !mgr->emuInstance->isAudioMuted());
    return 1;
}

int LuaScriptManager::l_client_unpause(lua_State* L)
{
    // Deliberately a no-op: this fork's script model always drives frame
    // progression itself via emu.frameadvance() (see that function), not
    // via the emulator's own running/paused state -- letting a script flip
    // that state would let the emu thread free-run uncontrolled frames
    // between frameadvance() calls, desyncing the script from what's
    // actually being displayed.
    return 0;
}

int LuaScriptManager::l_client_getversion(lua_State* L)
{
    // The tracker only branches on this to detect the old BizHawk 2.8 (Lua
    // 5.1) behavior; anything else selects its modern/Lua-5.4-era code
    // path, which is what we want.
    lua_pushstring(L, "2.9.1");
    return 1;
}

int LuaScriptManager::l_client_get_approx_framerate(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    lua_pushnumber(L, mgr->emuInstance->curFPS);
    return 1;
}

int LuaScriptManager::l_client_xpos(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    auto* fm = mgr->formsManager.get();
    EmuInstance* inst = mgr->emuInstance;
    int x = fm->runOnUI([=]() { return inst->getMainWindow()->x(); });
    lua_pushinteger(L, x);
    return 1;
}

int LuaScriptManager::l_client_ypos(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    auto* fm = mgr->formsManager.get();
    EmuInstance* inst = mgr->emuInstance;
    int y = fm->runOnUI([=]() { return inst->getMainWindow()->y(); });
    lua_pushinteger(L, y);
    return 1;
}

int LuaScriptManager::l_client_screenwidth(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    auto* fm = mgr->formsManager.get();
    EmuInstance* inst = mgr->emuInstance;
    // Approximated as the whole main window's width (menu bar etc.
    // included), not just the game panel -- good enough for the tracker's
    // actual use (centering popup dialogs), not pixel-exact vs. BizHawk.
    int w = fm->runOnUI([=]() { return inst->getMainWindow()->width(); });
    lua_pushinteger(L, w);
    return 1;
}

int LuaScriptManager::l_client_screenheight(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    auto* fm = mgr->formsManager.get();
    EmuInstance* inst = mgr->emuInstance;
    int h = fm->runOnUI([=]() { return inst->getMainWindow()->height(); });
    lua_pushinteger(L, h);
    return 1;
}

int LuaScriptManager::l_client_saveram(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    if (mgr->emuInstance->ndsSave)
        mgr->emuInstance->ndsSave->CheckFlush();
    return 0;
}

int LuaScriptManager::l_savestate_save(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    QString path = QString::fromUtf8(luaL_checkstring(L, 1));
    // EmuThread's save/load are already a thread-safe message-queue RPC
    // (same mechanism as emuFrameStep()), so no runOnUI needed here.
    mgr->emuInstance->getEmuThread()->saveState(path);
    return 0;
}

int LuaScriptManager::l_savestate_load(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    QString path = QString::fromUtf8(luaL_checkstring(L, 1));
    mgr->emuInstance->getEmuThread()->loadState(path);
    return 0;
}

int LuaScriptManager::l_joypad_get(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    static const char* keyNames[12] = {"A", "B", "X", "Y", "Left", "Right", "Up", "Down", "L", "R", "Select", "Start"};
    u32 mask = mgr->emuInstance->getInputMask();

    lua_newtable(L);
    for (int i = 0; i < 12; i++)
    {
        lua_pushboolean(L, !(mask & (1u << i))); // active-low: clear bit = pressed
        lua_setfield(L, -2, keyNames[i]);
    }
    return 1;
}

int LuaScriptManager::l_input_getmouse(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    auto* fm = mgr->formsManager.get();
    EmuInstance* inst = mgr->emuInstance;
    QPoint pos = fm->runOnUI([=]() { return inst->getMainWindow()->mapFromGlobal(QCursor::pos()); });

    lua_newtable(L);
    lua_pushinteger(L, pos.x());
    lua_setfield(L, -2, "X");
    lua_pushinteger(L, pos.y());
    lua_setfield(L, -2, "Y");
    lua_pushinteger(L, 0); // scroll wheel tracking not implemented
    lua_setfield(L, -2, "Wheel");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Left");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Middle");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Right");
    return 1;
}

int LuaScriptManager::l_comm_stub_bool(lua_State* L)
{
    lua_pushboolean(L, false);
    return 1;
}

int LuaScriptManager::l_comm_stub_table(lua_State* L)
{
    lua_newtable(L);
    return 1;
}

int LuaScriptManager::l_comm_stub_noop(lua_State* L)
{
    return 0;
}
