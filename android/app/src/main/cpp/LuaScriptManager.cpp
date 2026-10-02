#include "LuaScriptManager.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
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

// Lua's standard os.execute()/io.popen() shell out via libc's system()/
// posix_spawn(). On Android, calling that from an app's own sandboxed,
// heavily multi-threaded process doesn't fail cleanly -- it SIGSEGVs
// (confirmed via a real crash: NDS-Ironmon-Tracker's Main.lua calls
// os.execute("pwd ...") on startup to resolve its own working directory,
// purely as a "nice to have" for building absolute paths elsewhere --
// the dofile()/io.open() calls that actually matter already work via our
// own chdir() in threadMain()). Replace both with stubs that report
// "command not available" the way Lua itself would on a platform that
// genuinely has no shell, rather than letting the real ones crash the
// whole process. Callers that already treat a failed command as "no
// output produced" (as this tracker's MiscUtils.runExecuteCommand does)
// keep working; anything that would have hard-required shelling out
// simply can't on Android regardless.
static int l_os_execute_stub(lua_State* L)
{
    lua_pushnil(L);
    return 1;
}

static int l_io_popen_stub(lua_State* L)
{
    lua_pushnil(L);
    lua_pushstring(L, "io.popen is not supported on Android");
    return 2;
}

LuaScriptManager::LuaScriptManager(melonDS::NDS* nds, const uint32_t* inputMask) : nds(nds), inputMask(inputMask)
{
    sem_init(&stepRequested, 0, 0);
    sem_init(&stepCompleted, 0, 0);
    sem_init(&formsResultReady, 0, 0);
}

LuaScriptManager::~LuaScriptManager()
{
    stop();
    sem_destroy(&stepRequested);
    sem_destroy(&stepCompleted);
    sem_destroy(&formsResultReady);
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

    if (scriptPath.empty())
    {
        logf("No script path given -- nothing to run");
        return;
    }

    // A previous script may have finished on its own (erroring out or
    // returning naturally sets running=false from inside threadMain())
    // without anyone calling stop() to join its thread object. Join it now
    // if so -- returns immediately since the thread function has already
    // exited -- otherwise the std::thread move-assignment below would be
    // reassigning over an already-joinable thread, which std::terminate()s
    // the whole process per the standard.
    if (scriptThread.joinable())
        scriptThread.join();

    // A previous run may have left a stray post on stepRequested/
    // stepCompleted that nobody ever consumed (see the matching sem_post in
    // threadMain()'s cleanup path) -- drain both back to zero so this run
    // starts from a clean rendezvous state instead of the emulate() loop's
    // very first waitForStepRequest() spuriously succeeding before the
    // script has actually asked for a step.
    while (sem_trywait(&stepRequested) == 0) {}
    while (sem_trywait(&stepCompleted) == 0) {}
    while (sem_trywait(&formsResultReady) == 0) {}
    pendingFormsRequest.reset();

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
    // for a step request that will now never come. Also wake it if it's
    // instead parked waiting on a forms.* bridge response that Kotlin may
    // never get around to answering now.
    sem_post(&stepCompleted);
    sem_post(&stepRequested);
    sem_post(&formsResultReady);

    scriptThread.join();
    running.store(false);

    // Matches desktop's LuaFormsManager::destroyAll() call in its own
    // stop(): a script's forms don't outlive it. Fire-and-forget is fine
    // here (nothing is waiting on this).
    {
        std::lock_guard<std::mutex> lock(formsCallbackMutex);
        formsClickRefs.clear();
        formsCloseRefs.clear();
        pendingFormsCallbackRefs.clear();
    }
    FormsRequest destroyAll;
    destroyAll.op = FormsOp::DestroyAll;
    queueFormsCommand(destroyAll);
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

std::vector<LuaDrawCommand> LuaScriptManager::getDrawCommands()
{
    std::lock_guard<std::mutex> lock(drawMutex);
    return displayCommands;
}

std::optional<FormsRequest> LuaScriptManager::pollFormsRequest()
{
    std::lock_guard<std::mutex> lock(formsBridgeMutex);
    std::optional<FormsRequest> req = std::move(pendingFormsRequest);
    pendingFormsRequest.reset();
    return req;
}

void LuaScriptManager::deliverFormsResult(FormsResult result)
{
    // No lock needed on formsResult itself: sem_post/sem_wait already
    // establishes the happens-before relationship the script thread needs
    // to safely read what's written here before waking up.
    formsResult = std::move(result);
    sem_post(&formsResultReady);
}

std::vector<FormsRequest> LuaScriptManager::takeFormsCommands()
{
    std::lock_guard<std::mutex> lock(formsCommandsMutex);
    std::vector<FormsRequest> commands = std::move(formsCommands);
    formsCommands.clear();
    return commands;
}

void LuaScriptManager::notifyFormsClick(int handle)
{
    std::lock_guard<std::mutex> lock(formsCallbackMutex);
    auto it = formsClickRefs.find(handle);
    if (it != formsClickRefs.end())
        pendingFormsCallbackRefs.push_back(it->second);
}

void LuaScriptManager::notifyFormsFormClosed(int handle)
{
    std::lock_guard<std::mutex> lock(formsCallbackMutex);
    auto it = formsCloseRefs.find(handle);
    if (it != formsCloseRefs.end())
        pendingFormsCallbackRefs.push_back(it->second);
}

FormsResult LuaScriptManager::callFormsBridge(FormsRequest req)
{
    {
        std::lock_guard<std::mutex> lock(formsBridgeMutex);
        pendingFormsRequest = std::move(req);
    }
    sem_wait(&formsResultReady);
    return formsResult;
}

void LuaScriptManager::queueFormsCommand(const FormsRequest& req)
{
    std::lock_guard<std::mutex> lock(formsCommandsMutex);
    formsCommands.push_back(req);
}

int LuaScriptManager::refFunctionArg(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return 0;
    luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_pushvalue(L, idx);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

std::vector<std::string> LuaScriptManager::checkStringList(lua_State* L, int idx)
{
    std::vector<std::string> result;
    if (lua_isnoneornil(L, idx) || !lua_istable(L, idx))
        return result;

    lua_Integer n = lua_rawlen(L, idx);
    for (lua_Integer i = 1; i <= n; i++)
    {
        lua_rawgeti(L, idx, (int)i);
        if (lua_type(L, -1) == LUA_TSTRING)
            result.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    if (result.empty())
    {
        // Not a plain array -- fall back to iterating all values (covers an
        // associative table like {[key]=val, ...}).
        lua_pushnil(L);
        while (lua_next(L, idx) != 0)
        {
            if (lua_type(L, -1) == LUA_TSTRING)
                result.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    return result;
}

void LuaScriptManager::dispatchFormsCallbacks()
{
    std::vector<int> refs;
    {
        std::lock_guard<std::mutex> lock(formsCallbackMutex);
        refs = std::move(pendingFormsCallbackRefs);
        pendingFormsCallbackRefs.clear();
    }
    for (int ref : refs)
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

    lua_getglobal(L, "os");
    lua_pushcfunction(L, l_os_execute_stub);
    lua_setfield(L, -2, "execute");
    lua_pop(L, 1);

    lua_getglobal(L, "io");
    lua_pushcfunction(L, l_io_popen_stub);
    lua_setfield(L, -2, "popen");
    lua_pop(L, 1);

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

    // If this script errored out (or its main chunk simply never looped)
    // before ever calling emu.frameadvance(), the native emulate() thread
    // could be parked right now in waitForStepRequest() waiting for a step
    // that will never come -- which would otherwise freeze the entire
    // emulator, not just the script. Post unconditionally so it wakes up:
    // isActive() is already false by this point (running just cleared
    // above), so it falls back to the normal free-running loop instead of
    // treating this as a real step request. Any post left unconsumed here
    // (the emulate() thread wasn't actually waiting) is harmless -- the
    // next start() drains stale posts before launching a new script.
    sem_post(&stepRequested);
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

    // gui.*: real as of Phase 2 -- recorded here, actually rendered on the
    // Kotlin side (a Compose Canvas fed by getDrawCommands()).
    static const luaL_Reg guiFuncs[] = {
        {"drawText", l_gui_drawtext},
        {"drawRectangle", l_gui_drawrectangle},
        {"drawLine", l_gui_drawline},
        {"drawPixel", l_gui_drawpixel},
        {"drawEllipse", l_gui_drawellipse},
        {"drawPolygon", l_gui_drawpolygon},
        {"drawImage", l_gui_drawimage},
        {"drawImageRegion", l_gui_drawimageregion},
        {"clearImageCache", l_gui_clearimagecache},
        {nullptr, nullptr}
    };
    luaL_newlib(L, guiFuncs);
    lua_setglobal(L, "gui");

    // forms.*: real as of Phase 3 -- bridged to Kotlin-side Compose UI
    // state (LuaFormsOverlayUi.kt), not real separate OS windows.
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
        {"drawEllipse", l_forms_drawellipse},
        {"drawImage", l_forms_drawimage},
        {"clear", l_forms_clear},
        {"refresh", l_forms_refresh},
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

    // input.* (mouse): no real pointer device on Android, but the user's
    // finger over the gui.* overlay area stands in for one -- see
    // setMousePosition()/l_input_getmouse().
    static const luaL_Reg inputFuncs[] = {
        {"getmouse", l_input_getmouse},
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

    // android.*: not part of BizHawk's API -- see FormsOp's comment on
    // HttpGet/DownloadAndExtractUpdate for why these exist.
    static const luaL_Reg androidFuncs[] = {
        {"getScriptDirectory", l_android_getscriptdirectory},
        {"httpGet", l_android_httpget},
        {"downloadAndExtractUpdate", l_android_downloadandextractupdate},
        {"openUrl", l_android_openurl},
        {nullptr, nullptr}
    };
    luaL_newlib(L, androidFuncs);
    lua_setglobal(L, "android");
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

    // Dispatch any pending forms.* callbacks (button clicks, form close)
    // queued by Kotlin since the last call -- always, even while idling for
    // a ROM to load, so e.g. a settings form stays responsive.
    mgr->dispatchFormsCallbacks();

    // Request exactly one frame step from the native emulate() loop and
    // wait for it to finish. Mirrors the desktop build's EmuThread-RPC
    // approach, just with a direct semaphore rendezvous instead of a
    // message queue (Android's emu loop lives on one dedicated thread, so
    // no generalized RPC is needed).
    sem_post(&mgr->stepRequested);
    sem_wait(&mgr->stepCompleted);
    mgr->frameCount++;

    // The frame this call just produced should show whatever the script
    // drew (via gui.*) since the *previous* frameadvance() -- publish it
    // for Kotlin's getDrawCommands() to read, and start collecting fresh.
    {
        std::lock_guard<std::mutex> lock(mgr->drawMutex);
        if (!mgr->drawCommands.empty())
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

uint32_t LuaScriptManager::checkColor(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return 0;
    // luaL_checknumber (not checkinteger): some callers compute colors via
    // float arithmetic (e.g. darkening one channel), producing a Lua float
    // that Lua 5.4's strict luaL_checkinteger would reject outright even
    // though it's a perfectly good color value once rounded.
    return (uint32_t)(int64_t)llround(luaL_checknumber(L, idx));
}

int LuaScriptManager::l_gui_drawtext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Text;
    cmd.x1 = (int)checkIntArg(L, 1);
    cmd.y1 = (int)checkIntArg(L, 2);
    // BizHawk's real gui.text() tolerates a nil/missing text argument
    // (observed e.g. with NDS-Ironmon-Tracker's FormsUtils.shortenFolderName(),
    // which falls through with no explicit return -- and thus returns nil --
    // for an empty path, a real case on a fresh install's unset quickload
    // paths); luaL_checkstring() would hard-error the whole script on that,
    // so treat a non-string arg here as an empty string instead.
    cmd.text = luaL_optstring(L, 3, "");
    cmd.color = lua_gettop(L) >= 4 ? checkColor(L, 4) : 0xFFFFFFFF;
    // arg 5 is a background color (not currently drawn), arg 6 is the font
    // size this tracker always passes explicitly (its own default is 9,
    // much smaller than a fixed guess would be) -- honor it instead of
    // guessing a size on the Kotlin rendering side.
    cmd.fontSize = (int)optIntArg(L, 6, 9);

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawrectangle(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Rect;
    cmd.x1 = (int)checkIntArg(L, 1);
    cmd.y1 = (int)checkIntArg(L, 2);
    cmd.x2 = (int)checkIntArg(L, 3); // width
    cmd.y2 = (int)checkIntArg(L, 4); // height
    cmd.color = checkColor(L, 5);
    cmd.fillColor = checkColor(L, 6);

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawline(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Line;
    cmd.x1 = (int)checkIntArg(L, 1);
    cmd.y1 = (int)checkIntArg(L, 2);
    cmd.x2 = (int)checkIntArg(L, 3);
    cmd.y2 = (int)checkIntArg(L, 4);
    cmd.color = lua_gettop(L) >= 5 ? checkColor(L, 5) : 0xFFFFFFFF;

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawpixel(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Pixel;
    cmd.x1 = (int)checkIntArg(L, 1);
    cmd.y1 = (int)checkIntArg(L, 2);
    cmd.color = lua_gettop(L) >= 3 ? checkColor(L, 3) : 0xFFFFFFFF;

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawellipse(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Ellipse;
    cmd.x1 = (int)checkIntArg(L, 1);
    cmd.y1 = (int)checkIntArg(L, 2);
    cmd.x2 = (int)checkIntArg(L, 3);
    cmd.y2 = (int)checkIntArg(L, 4);
    cmd.color = checkColor(L, 5);
    cmd.fillColor = checkColor(L, 6);

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawpolygon(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Polygon;

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer n = lua_rawlen(L, 1);
    for (lua_Integer i = 1; i <= n; i++)
    {
        lua_rawgeti(L, 1, (int)i);
        if (lua_istable(L, -1))
        {
            lua_rawgeti(L, -1, 1);
            float x = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 2);
            float y = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            cmd.points.emplace_back(x, y);
        }
        lua_pop(L, 1);
    }
    cmd.color = lua_gettop(L) >= 2 ? checkColor(L, 2) : 0xFFFFFFFF;
    cmd.fillColor = checkColor(L, 3);

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawimage(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Image;
    cmd.text = luaL_checkstring(L, 1);
    cmd.x1 = (int)checkIntArg(L, 2);
    cmd.y1 = (int)checkIntArg(L, 3);
    cmd.x2 = (int)optIntArg(L, 4, 0);
    cmd.y2 = (int)optIntArg(L, 5, 0);

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_drawimageregion(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    LuaDrawCommand cmd;
    cmd.kind = LuaDrawCommand::Image;
    cmd.text = luaL_checkstring(L, 1);
    cmd.hasSrcRect = true;
    cmd.srcX = (int)checkIntArg(L, 2);
    cmd.srcY = (int)checkIntArg(L, 3);
    cmd.srcW = (int)checkIntArg(L, 4);
    cmd.srcH = (int)checkIntArg(L, 5);
    cmd.x1 = (int)checkIntArg(L, 6);
    cmd.y1 = (int)checkIntArg(L, 7);
    cmd.x2 = (int)optIntArg(L, 8, cmd.srcW);
    cmd.y2 = (int)optIntArg(L, 9, cmd.srcH);

    std::lock_guard<std::mutex> lock(mgr->drawMutex);
    mgr->drawCommands.push_back(cmd);
    return 0;
}

int LuaScriptManager::l_gui_clearimagecache(lua_State* L)
{
    // The actual image cache lives on the Kotlin side (LuaOverlayUi.kt),
    // keyed by path -- nothing to clear here. Accepted as a no-op so the
    // script doesn't error calling it.
    return 0;
}

int LuaScriptManager::l_forms_newform(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::NewForm;
    req.w = (int)checkIntArg(L, 1);
    req.h = (int)checkIntArg(L, 2);
    req.text = luaL_optstring(L, 3, "Script Window");
    int onCloseRef = refFunctionArg(L, 4);

    int handle = mgr->callFormsBridge(req).intResult;
    if (onCloseRef != 0)
    {
        std::lock_guard<std::mutex> lock(mgr->formsCallbackMutex);
        mgr->formsCloseRefs[handle] = onCloseRef;
    }
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_button(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::Button;
    req.handle = (int)checkIntArg(L, 1); // parent form handle
    req.text = luaL_optstring(L, 2, "");
    int onClickRef = refFunctionArg(L, 3);
    req.x = (int)optIntArg(L, 4, 0);
    req.y = (int)optIntArg(L, 5, 0);
    req.w = (int)optIntArg(L, 6, 75);
    req.h = (int)optIntArg(L, 7, 23);

    int handle = mgr->callFormsBridge(req).intResult;
    if (onClickRef != 0)
    {
        std::lock_guard<std::mutex> lock(mgr->formsCallbackMutex);
        mgr->formsClickRefs[handle] = onClickRef;
    }
    lua_pushinteger(L, handle);
    return 1;
}

int LuaScriptManager::l_forms_label(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::Label;
    req.handle = (int)checkIntArg(L, 1);
    req.text = luaL_optstring(L, 2, "");
    req.x = (int)optIntArg(L, 3, 0);
    req.y = (int)optIntArg(L, 4, 0);
    req.w = (int)optIntArg(L, 5, 0);
    req.h = (int)optIntArg(L, 6, 0);

    lua_pushinteger(L, mgr->callFormsBridge(req).intResult);
    return 1;
}

int LuaScriptManager::l_forms_checkbox(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::Checkbox;
    req.handle = (int)checkIntArg(L, 1);
    req.text = luaL_optstring(L, 2, "");
    req.x = (int)optIntArg(L, 3, 0);
    req.y = (int)optIntArg(L, 4, 0);

    lua_pushinteger(L, mgr->callFormsBridge(req).intResult);
    return 1;
}

int LuaScriptManager::l_forms_textbox(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::Textbox;
    req.handle = (int)checkIntArg(L, 1);
    req.text = luaL_optstring(L, 2, "");
    req.w = (int)optIntArg(L, 3, 100);
    req.h = (int)optIntArg(L, 4, 20);
    // arg 5 ("type", e.g. "HEX"/"NUMBER" input filtering) intentionally
    // ignored -- just a plain text field, no input validation.
    req.x = (int)optIntArg(L, 6, 0);
    req.y = (int)optIntArg(L, 7, 0);
    req.boolArg = lua_gettop(L) >= 8 && lua_toboolean(L, 8); // multiline

    lua_pushinteger(L, mgr->callFormsBridge(req).intResult);
    return 1;
}

int LuaScriptManager::l_forms_dropdown(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::Dropdown;
    req.handle = (int)checkIntArg(L, 1);
    req.items = checkStringList(L, 2);
    req.x = (int)optIntArg(L, 3, 0);
    req.y = (int)optIntArg(L, 4, 0);
    req.w = (int)optIntArg(L, 5, 100);
    req.h = (int)optIntArg(L, 6, 20);

    lua_pushinteger(L, mgr->callFormsBridge(req).intResult);
    return 1;
}

int LuaScriptManager::l_forms_setdropdownitems(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::SetDropdownItems;
    req.handle = (int)checkIntArg(L, 1);
    req.items = checkStringList(L, 2);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_picturebox(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::PictureBox;
    req.handle = (int)checkIntArg(L, 1);
    req.x = (int)checkIntArg(L, 2);
    req.y = (int)checkIntArg(L, 3);
    req.w = (int)checkIntArg(L, 4);
    req.h = (int)checkIntArg(L, 5);

    lua_pushinteger(L, mgr->callFormsBridge(req).intResult);
    return 1;
}

int LuaScriptManager::l_forms_setproperty(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::SetProperty;
    req.handle = (int)checkIntArg(L, 1);
    req.text = luaL_checkstring(L, 2);

    if (lua_isboolean(L, 3))
        req.text2 = lua_toboolean(L, 3) ? "true" : "false";
    else
    {
        size_t len;
        const char* s = luaL_tolstring(L, 3, &len);
        req.text2.assign(s, len);
        lua_pop(L, 1);
    }

    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_setlocation(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::SetLocation;
    req.handle = (int)checkIntArg(L, 1);
    req.x = (int)checkIntArg(L, 2);
    req.y = (int)checkIntArg(L, 3);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_settext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::SetText;
    req.handle = (int)checkIntArg(L, 1);
    req.text = luaL_checkstring(L, 2);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_gettext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::GetText;
    req.handle = (int)checkIntArg(L, 1);
    lua_pushstring(L, mgr->callFormsBridge(req).stringResult.c_str());
    return 1;
}

int LuaScriptManager::l_forms_ischecked(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::IsChecked;
    req.handle = (int)checkIntArg(L, 1);
    lua_pushboolean(L, mgr->callFormsBridge(req).boolResult);
    return 1;
}

int LuaScriptManager::l_forms_destroy(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)checkIntArg(L, 1);
    {
        std::lock_guard<std::mutex> lock(mgr->formsCallbackMutex);
        mgr->formsClickRefs.erase(handle);
        mgr->formsCloseRefs.erase(handle);
    }
    FormsRequest req;
    req.op = FormsOp::Destroy;
    req.handle = handle;
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_destroyall(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    {
        std::lock_guard<std::mutex> lock(mgr->formsCallbackMutex);
        mgr->formsClickRefs.clear();
        mgr->formsCloseRefs.clear();
    }
    FormsRequest req;
    req.op = FormsOp::DestroyAll;
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_addclick(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    int handle = (int)checkIntArg(L, 1);
    int ref = refFunctionArg(L, 2);
    if (ref == 0)
        return 0;
    std::lock_guard<std::mutex> lock(mgr->formsCallbackMutex);
    mgr->formsClickRefs[handle] = ref;
    return 0;
}

int LuaScriptManager::l_forms_getmousex(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::GetMouseX;
    req.handle = (int)checkIntArg(L, 1);
    lua_pushinteger(L, mgr->callFormsBridge(req).intResult);
    return 1;
}

int LuaScriptManager::l_forms_getmousey(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::GetMouseY;
    req.handle = (int)checkIntArg(L, 1);
    lua_pushinteger(L, mgr->callFormsBridge(req).intResult);
    return 1;
}

int LuaScriptManager::l_forms_openfile(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::OpenFile;
    req.text = (lua_gettop(L) >= 2 && lua_isstring(L, 2)) ? lua_tostring(L, 2) : ""; // initial dir
    req.text2 = (lua_gettop(L) >= 3 && lua_isstring(L, 3)) ? lua_tostring(L, 3) : ""; // filter
    // arg 4 (dialog title) not plumbed through -- low value for a mobile
    // file browser that's already scoped to the app's own script folder.
    lua_pushstring(L, mgr->callFormsBridge(req).stringResult.c_str());
    return 1;
}

int LuaScriptManager::l_forms_drawtext(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::DrawText;
    req.handle = (int)checkIntArg(L, 1);
    req.x = (int)checkIntArg(L, 2);
    req.y = (int)checkIntArg(L, 3);
    req.text = luaL_checkstring(L, 4);
    req.color = lua_gettop(L) >= 5 ? checkColor(L, 5) : 0xFFFFFFFF;
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_drawrectangle(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::DrawRectangle;
    req.handle = (int)checkIntArg(L, 1);
    req.x = (int)checkIntArg(L, 2);
    req.y = (int)checkIntArg(L, 3);
    req.w = (int)checkIntArg(L, 4);
    req.h = (int)checkIntArg(L, 5);
    req.color = checkColor(L, 6);
    req.fillColor = checkColor(L, 7);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_drawellipse(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::DrawEllipse;
    req.handle = (int)checkIntArg(L, 1);
    req.x = (int)checkIntArg(L, 2);
    req.y = (int)checkIntArg(L, 3);
    req.w = (int)checkIntArg(L, 4);
    req.h = (int)checkIntArg(L, 5);
    req.color = checkColor(L, 6);
    req.fillColor = checkColor(L, 7);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_drawimage(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::DrawImage;
    req.handle = (int)checkIntArg(L, 1);
    req.text = luaL_checkstring(L, 2); // image path
    req.x = (int)checkIntArg(L, 3);
    req.y = (int)checkIntArg(L, 4);
    req.w = (int)optIntArg(L, 5, 0);
    req.h = (int)optIntArg(L, 6, 0);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_clear(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::Clear;
    req.handle = (int)checkIntArg(L, 1);
    req.color = checkColor(L, 2);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_forms_refresh(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::Refresh;
    req.handle = (int)checkIntArg(L, 1);
    mgr->queueFormsCommand(req);
    return 0;
}

int LuaScriptManager::l_input_getmouse(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    float x = mgr->mouseX.load();
    float y = mgr->mouseY.load();
    bool pressed = mgr->mousePressed.load();

    // BizHawk's own NDS core reports mouse Y in a range of roughly
    // [-384, 384] centered on the middle of the combined dual-screen view,
    // not plain 0..384 -- confirmed from this tracker's own source
    // (Input.lua: `y = (mouse["Y"] + 384) / 2`, applied to every reported
    // position before use anywhere else), same convention the desktop
    // build replicates.
    double rawY = y * 2.0 - 384.0;

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)llround(x));
    lua_setfield(L, -2, "X");
    lua_pushinteger(L, (lua_Integer)llround(rawY));
    lua_setfield(L, -2, "Y");
    lua_pushinteger(L, 0); // scroll wheel delta tracking not implemented
    lua_setfield(L, -2, "Wheel");
    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "Left");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Middle");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "Right");
    return 1;
}

int LuaScriptManager::l_client_setgameextrapadding(lua_State* L)
{
    // Stored so client.bufferwidth()/screenwidth()/screenheight() (below)
    // and the Kotlin-side gui.* overlay (which reads this via
    // getScreenPadding()) agree on how much extra room the script has to
    // draw in. Without this, this tracker's own UI framework computes a
    // 0x0 size for its main screen's content box and silently skips
    // drawing it entirely (Box.show()'s own "nothing to draw" guard) --
    // confirmed as the actual root cause of its whole UI never appearing.
    LuaScriptManager* mgr = self(L);
    mgr->luaPadLeft.store((int)checkIntArg(L, 1));
    mgr->luaPadTop.store((int)checkIntArg(L, 2));
    mgr->luaPadRight.store((int)checkIntArg(L, 3));
    mgr->luaPadBottom.store((int)checkIntArg(L, 4));
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
    LuaScriptManager* mgr = self(L);
    lua_pushinteger(L, 256 + mgr->luaPadLeft.load() + mgr->luaPadRight.load());
    return 1;
}

int LuaScriptManager::l_client_screenwidth(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    lua_pushinteger(L, 256 + mgr->luaPadLeft.load() + mgr->luaPadRight.load());
    return 1;
}

int LuaScriptManager::l_client_screenheight(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    lua_pushinteger(L, 384 + mgr->luaPadTop.load() + mgr->luaPadBottom.load());
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

int LuaScriptManager::l_android_getscriptdirectory(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    lua_pushstring(L, mgr->scriptDir.c_str());
    return 1;
}

int LuaScriptManager::l_android_httpget(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::HttpGet;
    req.text = luaL_checkstring(L, 1);
    FormsResult res = mgr->callFormsBridge(req);
    if (res.boolResult)
        lua_pushstring(L, res.stringResult.c_str());
    else
        lua_pushnil(L);
    return 1;
}

int LuaScriptManager::l_android_downloadandextractupdate(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::DownloadAndExtractUpdate;
    req.text = luaL_checkstring(L, 1);  // URL
    req.text2 = luaL_checkstring(L, 2); // destination directory
    FormsResult res = mgr->callFormsBridge(req);
    lua_pushboolean(L, res.boolResult);
    return 1;
}

int LuaScriptManager::l_android_openurl(lua_State* L)
{
    LuaScriptManager* mgr = self(L);
    FormsRequest req;
    req.op = FormsOp::OpenUrl;
    req.text = luaL_checkstring(L, 1);
    FormsResult res = mgr->callFormsBridge(req);
    lua_pushboolean(L, res.boolResult);
    return 1;
}

}
