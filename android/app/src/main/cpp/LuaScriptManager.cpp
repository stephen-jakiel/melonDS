#include "LuaScriptManager.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "MelonLog.h"
#include "NDS.h"
#include "NDSCart.h"
#include "NDSCart/CartCommon.h"
#include "Savestate.h"
#include "sha1/sha1.hpp"
#include "MelonDS.h"

using namespace melonDS;

namespace MelonDSAndroid
{

static const char* kLogTag = "melonDS-Lua";

// Key under which the owning LuaScriptManager* is stashed in the Lua
// registry, so our static C-function bindings (Lua's API only takes plain
// function pointers, no captureable state) can get back to it.
static const char* kSelfRegistryKey = "melonDS.LuaScriptManager";

// Used in place of luaL_checkinteger/luaL_optinteger throughout this file.
// Lua 5.4 distinguishes integer and float subtypes, and any expression
// involving `/` (or other float-producing arithmetic) -- extremely common
// for script authors computing coordinates, e.g. `x + width/2` -- yields a
// float even when the result is a whole number. luaL_checkinteger rejects
// that outright ("number has no integer representation"); rounding instead
// accepts it while still behaving identically for genuine integers.
static lua_Integer checkIntArg(lua_State* L, int idx)
{
    return (lua_Integer)llround(luaL_checknumber(L, idx));
}

static lua_Integer optIntArg(lua_State* L, int idx, lua_Integer def)
{
    if (lua_isnoneornil(L, idx))
        return def;
    return (lua_Integer)llround(luaL_checknumber(L, idx));
}

LuaScriptManager::LuaScriptManager(melonDS::NDS* nds, const uint32_t* inputMask) : nds(nds), inputMask(inputMask)
{
    sem_init(&stepRequested, 0, 0);
    sem_init(&stepCompleted, 0, 0);
}

LuaScriptManager::~LuaScriptManager()
{
    stop();
    sem_destroy(&stepRequested);
    sem_destroy(&stepCompleted);
}

void LuaScriptManager::logf(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG_INFO(kLogTag, "%s", buf);
}

void LuaScriptManager::start(const std::string& scriptPath)
{
    if (running.load())
        return;

    // A previous script may have finished on its own (erroring out or
    // returning naturally sets running=false from inside threadMain())
    // without anyone calling stop() to join its thread object. Join it now
    // if so -- returns immediately since the thread function has already
    // exited -- otherwise the std::thread move-assignment below would be
    // reassigning over an already-joinable thread, which std::terminate()s
    // the whole process per the standard.
    if (scriptThread.joinable())
        scriptThread.join();

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
    // Wake the script thread if it's parked inside emu.frameadvance()
    // waiting on a step that will never complete (e.g. the emulate() loop
    // isn't running), and wake the emulate() loop if it's parked waiting
    // for a step request that will now never come.
    sem_post(&stepCompleted);
    sem_post(&stepRequested);

    scriptThread.join();
    running.store(false);
}

bool LuaScriptManager::waitForStepRequest()
{
    sem_wait(&stepRequested);
    return running.load() && !stopRequested.load();
}

void LuaScriptManager::signalStepComplete()
{
    sem_post(&stepCompleted);
}

LuaScriptManager* LuaScriptManager::self(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kSelfRegistryKey);
    auto* mgr = static_cast<LuaScriptManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return mgr;
}

void LuaScriptManager::threadMain(std::string scriptPath)
{
    // Most BizHawk-style scripts (this tracker included) load their own
    // submodules and read/write their own data files using paths relative
    // to the *script's own* directory, e.g. dofile("ironmon_tracker/Main.lua")
    // or io.open("Settings.ini") -- which only resolve correctly if that's
    // also the process's current working directory. Scripts are expected to
    // live under app-private external storage (no SAF/content:// URIs, so
    // plain POSIX file I/O -- including Lua's own io.*/dofile -- works
    // unmodified).
    size_t slash = scriptPath.find_last_of('/');
    scriptDir = (slash == std::string::npos) ? "." : scriptPath.substr(0, slash);
    chdir(scriptDir.c_str());

    L = luaL_newstate();
    luaL_openlibs(L);

    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kSelfRegistryKey);

    registerAPI();

    // Redirect Lua's print() to logcat instead of stdout.
    lua_register(L, "print", l_print);

    if (luaL_loadfile(L, scriptPath.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK)
    {
        const char* err = lua_tostring(L, -1);
        logf("Lua error: %s", err ? err : "(unknown error)");
    }

    // event.onexit/onconsoleclose handlers, run now while L is still valid.
    for (int ref : exitCallbackRefs)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            logf("Lua error in exit handler: %s", err ? err : "(unknown error)");
            lua_pop(L, 1);
        }
    }
    exitCallbackRefs.clear();

    lua_close(L);
    L = nullptr;

    running.store(false);
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

    // gui.*/forms.*: Phase 1 no-op stubs (logged once, harmless defaults).
    static const luaL_Reg guiFuncs[] = {
        {"drawText", l_stub_noop},
        {"drawRectangle", l_stub_noop},
        {"drawLine", l_stub_noop},
        {"drawPixel", l_stub_noop},
        {"drawPolygon", l_stub_noop},
        {"drawImage", l_stub_noop},
        {"drawImageRegion", l_stub_noop},
        {"clearImageCache", l_stub_noop},
        {nullptr, nullptr}
    };
    luaL_newlib(L, guiFuncs);
    lua_setglobal(L, "gui");

    static const luaL_Reg formsFuncs[] = {
        {"newform", l_stub_zero},
        {"button", l_stub_zero},
        {"label", l_stub_zero},
        {"checkbox", l_stub_zero},
        {"textbox", l_stub_zero},
        {"dropdown", l_stub_zero},
        {"setdropdownitems", l_stub_noop},
        {"pictureBox", l_stub_zero},
        {"setproperty", l_stub_noop},
        {"setlocation", l_stub_noop},
        {"settext", l_stub_noop},
        {"gettext", l_stub_emptystring},
        {"ischecked", l_stub_false},
        {"destroy", l_stub_noop},
        {"destroyall", l_stub_noop},
        {"addclick", l_stub_noop},
        {"getMouseX", l_stub_zero},
        {"getMouseY", l_stub_zero},
        {"openfile", l_stub_emptystring},
        {"drawText", l_stub_noop},
        {"drawRectangle", l_stub_noop},
        {"drawEllipse", l_stub_noop},
        {"drawImage", l_stub_noop},
        {"clear", l_stub_noop},
        {"refresh", l_stub_noop},
        {nullptr, nullptr}
    };
    luaL_newlib(L, formsFuncs);
    lua_setglobal(L, "forms");

    static const luaL_Reg clientFuncs[] = {
        {"SetGameExtraPadding", l_client_setgameextrapadding},
        {"SetSoundOn", l_client_setsoundon},
        {"GetSoundOn", l_client_getsoundon},
        {"pause", l_client_pause},
        {"unpause", l_client_unpause},
        {"getversion", l_client_getversion},
        {"get_approx_framerate", l_client_get_approx_framerate},
        {"xpos", l_client_xpos},
        {"ypos", l_client_ypos},
        {"screenwidth", l_client_screenwidth},
        {"screenheight", l_client_screenheight},
        {"bufferwidth", l_client_bufferwidth},
        {"saveram", l_client_saveram},
        {"closerom", l_client_closerom},
        {"openrom", l_client_openrom},
        {nullptr, nullptr}
    };
    luaL_newlib(L, clientFuncs);
    lua_setglobal(L, "client");

    static const luaL_Reg eventFuncs[] = {
        {"onexit", l_event_onexit},
        {"onconsoleclose", l_event_onconsoleclose},
        {nullptr, nullptr}
    };
    luaL_newlib(L, eventFuncs);
    lua_setglobal(L, "event");

    static const luaL_Reg consoleFuncs[] = {
        {"clear", l_console_clear},
        {nullptr, nullptr}
    };
    luaL_newlib(L, consoleFuncs);
    lua_setglobal(L, "console");

    static const luaL_Reg gameinfoFuncs[] = {
        {"getromname", l_gameinfo_getromname},
        {"getromhash", l_gameinfo_getromhash},
        {nullptr, nullptr}
    };
    luaL_newlib(L, gameinfoFuncs);
    lua_setglobal(L, "gameinfo");

    static const luaL_Reg bitFuncs[] = {
        {"band", l_bit_band},
        {"bor", l_bit_bor},
        {"bxor", l_bit_bxor},
        {"lshift", l_bit_lshift},
        {"rshift", l_bit_rshift},
        {nullptr, nullptr}
    };
    luaL_newlib(L, bitFuncs);
    lua_setglobal(L, "bit");

    static const luaL_Reg savestateFuncs[] = {
        {"save", l_savestate_save},
        {"load", l_savestate_load},
        {nullptr, nullptr}
    };
    luaL_newlib(L, savestateFuncs);
    lua_setglobal(L, "savestate");

    static const luaL_Reg memorysavestateFuncs[] = {
        {"savecorestate", l_memorysavestate_savecorestate},
        {"loadcorestate", l_memorysavestate_loadcorestate},
        {"removestate", l_memorysavestate_removestate},
        {nullptr, nullptr}
    };
    luaL_newlib(L, memorysavestateFuncs);
    lua_setglobal(L, "memorysavestate");

    static const luaL_Reg joypadFuncs[] = {
        {"get", l_joypad_get},
        {nullptr, nullptr}
    };
    luaL_newlib(L, joypadFuncs);
    lua_setglobal(L, "joypad");

    // input.* (mouse): no pointer device on Android -- stubbed to report a
    // neutral "nothing happening" state for now, in the same table shape
    // BizHawk's API returns (X/Y/Wheel/Left/Middle/Right all present), not
    // an empty table -- scripts that unconditionally do arithmetic on
    // e.g. mouse["Y"] (this tracker's Input.lua does) would otherwise
    // crash on a nil field.
    static const luaL_Reg inputFuncs[] = {
        {"getmouse", l_input_getmouse_stub},
        {nullptr, nullptr}
    };
    luaL_newlib(L, inputFuncs);
    lua_setglobal(L, "input");

    static const luaL_Reg commFuncs[] = {
        {"httpTest", l_comm_stub_bool},
        {"httpGet", l_comm_stub_table},
        {"httpPost", l_comm_stub_table},
        {"httpSetGetUrl", l_comm_stub_noop},
        {"httpSetPostUrl", l_comm_stub_noop},
        {"httpSetTimeout", l_comm_stub_noop},
        {"socketServerSetTimeout", l_comm_stub_noop},
        {"socketServerSetIp", l_comm_stub_noop},
        {"socketServerSetPort", l_comm_stub_noop},
        {"socketServerSend", l_comm_stub_noop},
        {"socketServerResponse", l_comm_stub_table},
        {"socketServerSuccessful", l_comm_stub_bool},
        {"socketServerIsConnected", l_comm_stub_bool},
        {"socketServerGetInfo", l_comm_stub_table},
        {nullptr, nullptr}
    };
    luaL_newlib(L, commFuncs);
    lua_setglobal(L, "comm");
}

int LuaScriptManager::l_print(lua_State* L)
{
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++)
    {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }

    LOG_INFO(kLogTag, "%s", line.c_str());
    return 0;
}

// Only the "Main RAM" domain is implemented -- it's the only one the
// Ironmon-Tracker-style scripts this was built for actually use.
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
    u32 addr = (u32)checkIntArg(L, 1);
    NDS* nds = mgr->nds;
    lua_pushinteger(L, nds->MainRAM[addr & nds->MainRAMMask]);
    return 1;
}

int LuaScriptManager::l_memory_read_u16_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)checkIntArg(L, 1);
    NDS* nds = mgr->nds;
    u32 a = addr & nds->MainRAMMask;
    u16 val = nds->MainRAM[a] | (nds->MainRAM[(a + 1) & nds->MainRAMMask] << 8);
    lua_pushinteger(L, val);
    return 1;
}

int LuaScriptManager::l_memory_read_u32_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)checkIntArg(L, 1);
    NDS* nds = mgr->nds;
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
    u32 addr = (u32)checkIntArg(L, 1);
    u8 val = (u8)checkIntArg(L, 2);
    NDS* nds = mgr->nds;
    nds->MainRAM[addr & nds->MainRAMMask] = val;
    return 0;
}

int LuaScriptManager::l_memory_write_u16_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)checkIntArg(L, 1);
    u16 val = (u16)checkIntArg(L, 2);
    NDS* nds = mgr->nds;
    u32 a = addr & nds->MainRAMMask;
    nds->MainRAM[a] = val & 0xFF;
    nds->MainRAM[(a + 1) & nds->MainRAMMask] = (val >> 8) & 0xFF;
    return 0;
}

int LuaScriptManager::l_memory_write_u32_le(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    u32 addr = (u32)checkIntArg(L, 1);
    u32 val = (u32)checkIntArg(L, 2);
    NDS* nds = mgr->nds;
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

    // Request exactly one frame step from the native emulate() loop and
    // wait for it to finish. Mirrors the desktop build's EmuThread-RPC
    // approach, just with a direct semaphore rendezvous instead of a
    // message queue (Android's emu loop lives on one dedicated thread, so
    // no generalized RPC is needed).
    sem_post(&mgr->stepRequested);
    sem_wait(&mgr->stepCompleted);
    mgr->frameCount++;

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

int LuaScriptManager::l_stub_noop(lua_State* L)
{
    return 0;
}

int LuaScriptManager::l_stub_zero(lua_State* L)
{
    lua_pushinteger(L, 0);
    return 1;
}

int LuaScriptManager::l_stub_emptystring(lua_State* L)
{
    lua_pushstring(L, "");
    return 1;
}

int LuaScriptManager::l_stub_false(lua_State* L)
{
    lua_pushboolean(L, false);
    return 1;
}

int LuaScriptManager::l_stub_emptytable(lua_State* L)
{
    lua_newtable(L);
    return 1;
}

int LuaScriptManager::l_input_getmouse_stub(lua_State* L)
{
    lua_newtable(L);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "X");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "Y");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "Wheel");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Left");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Middle");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Right");
    return 1;
}

int LuaScriptManager::l_client_setgameextrapadding(lua_State* L)
{
    // Phase 1: no on-screen overlay/forms host yet to reserve space in, so
    // nothing to actually resize. Accept the call silently so scripts that
    // always call this up front don't trip an "unknown function" error.
    return 0;
}

int LuaScriptManager::l_client_setsoundon(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    mgr->soundOn = lua_toboolean(L, 1);
    // Not yet wired to Android's actual Oboe audio output -- tracked state
    // only for now, since GetSoundOn() round-tripping matters more to this
    // tracker than the mute actually taking effect.
    return 0;
}

int LuaScriptManager::l_client_getsoundon(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    lua_pushboolean(L, mgr->soundOn);
    return 1;
}

int LuaScriptManager::l_client_pause(lua_State* L)
{
    // Deliberately a no-op, same reasoning as the desktop build: this
    // fork's script model always drives frame progression itself via
    // emu.frameadvance(), so honoring the emulator's own pause/unpause
    // state from a script would let frames free-run uncontrolled between
    // frameadvance() calls.
    return 0;
}

int LuaScriptManager::l_client_unpause(lua_State* L)
{
    return 0;
}

int LuaScriptManager::l_client_getversion(lua_State* L)
{
    lua_pushstring(L, "2.9.1");
    return 1;
}

int LuaScriptManager::l_client_get_approx_framerate(lua_State* L)
{
    // Not yet wired to the real measured FPS -- cosmetic value only.
    lua_pushnumber(L, 60.0);
    return 1;
}

int LuaScriptManager::l_client_bufferwidth(lua_State* L)
{
    lua_pushinteger(L, 256);
    return 1;
}

int LuaScriptManager::l_client_screenwidth(lua_State* L)
{
    lua_pushinteger(L, 256);
    return 1;
}

int LuaScriptManager::l_client_screenheight(lua_State* L)
{
    lua_pushinteger(L, 384);
    return 1;
}

int LuaScriptManager::l_client_xpos(lua_State* L)
{
    lua_pushinteger(L, 0);
    return 1;
}

int LuaScriptManager::l_client_ypos(lua_State* L)
{
    lua_pushinteger(L, 0);
    return 1;
}

int LuaScriptManager::l_client_saveram(lua_State* L)
{
    // SRAM is flushed to disk by the regular save-manager path on Android
    // (not under direct script control the way desktop's EmuInstance
    // exposes it) -- accepted as a no-op.
    return 0;
}

int LuaScriptManager::l_client_closerom(lua_State* L)
{
    // Not implemented in Phase 1 -- this tracker doesn't call it in normal
    // operation (desktop's audit found it only reachable from a menu this
    // script doesn't offer).
    return 0;
}

int LuaScriptManager::l_client_openrom(lua_State* L)
{
    return 0;
}

int LuaScriptManager::l_event_onexit(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    if (lua_isnoneornil(L, 1))
        return 0;
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    mgr->exitCallbackRefs.push_back(ref);
    return 0;
}

int LuaScriptManager::l_event_onconsoleclose(lua_State* L)
{
    // Treated the same as onexit here: no separate "console window" concept
    // distinct from the script itself in this fork.
    return l_event_onexit(L);
}

int LuaScriptManager::l_console_clear(lua_State* L)
{
    return 0;
}

int LuaScriptManager::l_gameinfo_getromname(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    NDS* nds = mgr->nds;
    auto* cart = nds ? nds->GetNDSCart() : nullptr;
    if (!cart)
    {
        lua_pushstring(L, "Null");
        return 1;
    }

    char buf[13] = {0};
    memcpy(buf, cart->GetHeader().GameTitle, 12);
    std::string title(buf);
    // Trim trailing whitespace/NULs, matching the desktop build's QString::trimmed().
    while (!title.empty() && (unsigned char)title.back() <= ' ')
        title.pop_back();
    lua_pushstring(L, title.empty() ? "Null" : title.c_str());
    return 1;
}

int LuaScriptManager::l_gameinfo_getromhash(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    NDS* nds = mgr->nds;
    auto* cart = nds ? nds->GetNDSCart() : nullptr;
    if (!cart || !cart->GetROM())
    {
        lua_pushstring(L, "");
        return 1;
    }

    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, cart->GetROM(), cart->GetROMLength());
    unsigned char digest[20];
    SHA1Final(digest, &ctx);

    char hex[41];
    for (int i = 0; i < 20; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    lua_pushstring(L, hex);
    return 1;
}

int LuaScriptManager::l_bit_band(lua_State* L)
{
    int n = lua_gettop(L);
    lua_Integer result = n >= 1 ? checkIntArg(L, 1) : 0;
    for (int i = 2; i <= n; i++)
        result &= checkIntArg(L, i);
    lua_pushinteger(L, result);
    return 1;
}

int LuaScriptManager::l_bit_bor(lua_State* L)
{
    int n = lua_gettop(L);
    lua_Integer result = n >= 1 ? checkIntArg(L, 1) : 0;
    for (int i = 2; i <= n; i++)
        result |= checkIntArg(L, i);
    lua_pushinteger(L, result);
    return 1;
}

int LuaScriptManager::l_bit_bxor(lua_State* L)
{
    int n = lua_gettop(L);
    lua_Integer result = n >= 1 ? checkIntArg(L, 1) : 0;
    for (int i = 2; i <= n; i++)
        result ^= checkIntArg(L, i);
    lua_pushinteger(L, result);
    return 1;
}

int LuaScriptManager::l_bit_lshift(lua_State* L)
{
    lua_Integer a = checkIntArg(L, 1);
    lua_Integer b = checkIntArg(L, 2);
    lua_pushinteger(L, a << b);
    return 1;
}

int LuaScriptManager::l_bit_rshift(lua_State* L)
{
    lua_Integer a = checkIntArg(L, 1);
    lua_Integer b = checkIntArg(L, 2);
    lua_pushinteger(L, (lua_Integer)((uint64_t)a >> b));
    return 1;
}

// savestate.*/memorysavestate.*: both call NDS::DoSavestate() (or, for
// file-based savestate.*, go through MelonDSAndroid::saveState/loadState)
// directly on the calling (script) thread. That's safe specifically because
// of how emu.frameadvance() works here: the native emulate() thread only
// touches nds between a step request and signalStepComplete(), and is
// parked the rest of the time -- so the window between two
// frameadvance() calls (the only time a script can reach these) is
// guaranteed free of concurrent access.
int LuaScriptManager::l_savestate_save(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    MelonDSAndroid::saveState(path);
    return 0;
}

int LuaScriptManager::l_savestate_load(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    MelonDSAndroid::loadState(path);
    return 0;
}

int LuaScriptManager::l_memorysavestate_savecorestate(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    NDS* nds = mgr->nds;

    Savestate state;
    if (state.Error)
        return luaL_error(L, "failed to allocate savestate buffer");
    nds->DoSavestate(&state);

    int id = mgr->nextMemorySavestateId++;
    const uint8_t* data = (const uint8_t*)state.Buffer();
    mgr->memorySavestates[id] = std::vector<uint8_t>(data, data + state.Length());

    lua_pushinteger(L, id);
    return 1;
}

int LuaScriptManager::l_memorysavestate_loadcorestate(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int id = (int)checkIntArg(L, 1);

    auto it = mgr->memorySavestates.find(id);
    if (it == mgr->memorySavestates.end())
        return luaL_error(L, "memorysavestate.loadcorestate: unknown id %d", id);

    NDS* nds = mgr->nds;
    Savestate state((void*)it->second.data(), (u32)it->second.size(), false);
    if (state.Error)
        return luaL_error(L, "failed to load in-memory savestate %d", id);
    nds->DoSavestate(&state);

    return 0;
}

int LuaScriptManager::l_memorysavestate_removestate(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int id = (int)checkIntArg(L, 1);
    mgr->memorySavestates.erase(id);
    return 0;
}

int LuaScriptManager::l_joypad_get(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    static const char* keyNames[12] = {"A", "B", "X", "Y", "Left", "Right", "Up", "Down", "L", "R", "Select", "Start"};
    u32 mask = *mgr->inputMask;

    lua_newtable(L);
    for (int i = 0; i < 12; i++)
    {
        lua_pushboolean(L, !(mask & (1u << i))); // active-low: clear bit = pressed
        lua_setfield(L, -2, keyNames[i]);
    }
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

}
