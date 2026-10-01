#ifndef LUASCRIPTMANAGER_H
#define LUASCRIPTMANAGER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <semaphore.h>
#include <string>
#include <thread>
#include <vector>

struct lua_State;

namespace melonDS { class NDS; }

namespace MelonDSAndroid
{

// Android port of the desktop (qt_sdl) LuaScriptManager -- runs a single
// BizHawk-API-compatible Lua script (e.g. NDS-Ironmon-Tracker) against this
// emulator instance, on its own thread, the same frame-advance-driven model
// as the desktop build (the script alone drives frame progression via
// emu.frameadvance(), once per call).
//
// Phase 1 scope: memory.*/emu.*/gameinfo.*/bit.*/console.*/savestate.*/
// memorysavestate.*/joypad.* are fully implemented. gui.* and forms.* (the
// on-screen overlay and native-widget UI the tracker's own interface is
// actually built from) are safe no-op stubs for now -- logged once via
// logcat, not yet rendered/created. See the project's Lua scripting memory
// for the planned follow-up phases (GLES overlay compositing, then an
// Android View-based forms.* host).
class LuaScriptManager
{
public:
    // inputMask points directly at MelonInstance's own live input-mask
    // member (handed out once at construction, same lifetime as nds) --
    // avoids needing a dedicated getter purely for this one read, while
    // still always reflecting the current pressed-keys state.
    LuaScriptManager(melonDS::NDS* nds, const uint32_t* inputMask);
    ~LuaScriptManager();

    bool isRunning() const { return running.load(); }

    // Starts running scriptPath on a new thread. No-op if a script is
    // already running (call stop() first).
    void start(const std::string& scriptPath);

    // Asks the running script to stop at its next frameadvance() call, then
    // waits for its thread to exit. Safe to call even if nothing is running.
    void stop();

    // --- Called only from the native emulate() thread (MelonDSAndroidJNI.cpp) ---

    // True for as long as a script is running and hasn't been asked to
    // stop -- the emulate() loop should hand frame-stepping control over to
    // the script (via waitForStepRequest()/signalStepComplete()) instead of
    // free-running on its own while this holds.
    bool isActive() const { return running.load() && !stopRequested.load(); }

    // Blocks until the script calls emu.frameadvance() requesting exactly
    // one frame step, or until stop() is called. Returns false if the
    // caller should abandon script-driven stepping for this iteration
    // (stop() was called while waiting) and fall back to the normal
    // free-running loop.
    bool waitForStepRequest();

    // Wakes the script's emu.frameadvance() call back up once the frame it
    // just requested (via MelonDSAndroid::loop()) has actually run.
    void signalStepComplete();

private:
    void threadMain(std::string scriptPath);
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

    // gui.*/forms.*: Phase 1 no-op stubs. Each logs a one-time notice (per
    // function name) and returns a harmless default so scripts that stash
    // the "handle" and keep calling methods on it don't crash outright.
    static int l_stub_noop(lua_State* L);
    static int l_stub_zero(lua_State* L);
    static int l_stub_emptystring(lua_State* L);
    static int l_stub_false(lua_State* L);
    static int l_stub_emptytable(lua_State* L);

    static int l_client_setgameextrapadding(lua_State* L);
    static int l_client_setsoundon(lua_State* L);
    static int l_client_getsoundon(lua_State* L);
    static int l_client_pause(lua_State* L);
    static int l_client_unpause(lua_State* L);
    static int l_client_getversion(lua_State* L);
    static int l_client_get_approx_framerate(lua_State* L);
    static int l_client_bufferwidth(lua_State* L);
    static int l_client_screenwidth(lua_State* L);
    static int l_client_screenheight(lua_State* L);
    static int l_client_xpos(lua_State* L);
    static int l_client_ypos(lua_State* L);
    static int l_client_saveram(lua_State* L);
    static int l_client_closerom(lua_State* L);
    static int l_client_openrom(lua_State* L);

    // Real BizHawk global API calls (unlike the many other event.* uses
    // across the tracker's source, which are a local function parameter
    // shadowing this global within specific functions).
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

    static int l_savestate_save(lua_State* L);
    static int l_savestate_load(lua_State* L);
    static int l_memorysavestate_savecorestate(lua_State* L);
    static int l_memorysavestate_loadcorestate(lua_State* L);
    static int l_memorysavestate_removestate(lua_State* L);

    static int l_joypad_get(lua_State* L);

    // comm.* (Streamerbot/Crowd Control network integration): stubbed as
    // harmless no-ops/empty results, same as the desktop build.
    static int l_comm_stub_bool(lua_State* L);
    static int l_comm_stub_table(lua_State* L);
    static int l_comm_stub_noop(lua_State* L);

    melonDS::NDS* nds;
    const uint32_t* inputMask;
    lua_State* L = nullptr;
    std::thread scriptThread;
    std::atomic<bool> running {false};
    std::atomic<bool> stopRequested {false};
    uint64_t frameCount = 0;
    std::string scriptDir;
    std::vector<int> exitCallbackRefs; // event.onexit/onconsoleclose
    bool soundOn = true;

    // memorysavestate.*: in-memory (not file-based) savestates, keyed by an
    // incrementing id handed back to the script.
    std::map<int, std::vector<uint8_t>> memorySavestates;
    int nextMemorySavestateId = 1;

    // Rendezvous between the script thread (producer of step *requests*)
    // and the native emulate() thread (producer of step *completions*).
    sem_t stepRequested;
    sem_t stepCompleted;
};

}

#endif // LUASCRIPTMANAGER_H
