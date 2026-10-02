#ifndef LUASCRIPTMANAGER_H
#define LUASCRIPTMANAGER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore.h>
#include <string>
#include <thread>
#include <vector>

struct lua_State;

namespace melonDS { class NDS; }

namespace MelonDSAndroid
{

// A single gui.draw*() call recorded by the script thread, consumed by
// Kotlin (via MelonEmulator.getLuaDrawCommands()) when painting the next
// overlay frame. Coordinates are in NDS-native pixel space (256 wide, top
// screen y=[0,192), bottom screen y=[192,384)) -- same convention as the
// desktop build's LuaDrawCommand, just Qt-free (plain packed 0xAARRGGBB
// colors instead of QColor, std::string/std::vector instead of
// QString/QRect/QPointF).
struct LuaDrawCommand
{
    enum Kind { Text, Rect, Line, Pixel, Ellipse, Polygon, Image };
    Kind kind;
    int x1, y1, x2, y2; // Image: x1,y1=dest pos, x2,y2=dest size (0=natural)
    uint32_t color = 0;
    uint32_t fillColor = 0;
    std::string text; // Text: the string. Image: the file path.
    std::vector<std::pair<float, float>> points; // Polygon only
    bool hasSrcRect = false;
    int srcX = 0, srcY = 0, srcW = 0, srcH = 0; // Image only, source crop region
    int fontSize = 9; // Text only, in NDS-native pixel units; 9 matches this tracker's own default
};

// forms.*: unlike gui.* (which only ever accumulates draw commands onto the
// main screen overlay), forms.* creates and queries actual Kotlin-side
// Compose UI state (windows, buttons, text fields, ...) that only the main
// thread can touch -- so every operation has to cross into Kotlin somehow.
// Split into two channels based on whether the script needs a value back
// *before* it can continue (creating a widget, reading text/checked state):
//
//   - Blocking ops (creation + getters): exactly one in flight at a time is
//     ever possible (every forms.* call comes from the single script
//     thread), so a single-slot "mailbox" plus a semaphore is enough --
//     Kotlin polls pollFormsRequest(), does the work on the UI thread, and
//     calls deliverFormsResult() to hand the result back and wake the
//     script thread. See FormsOp below for which ops these are.
//   - Fire-and-forget ops (mutations, forms.* picturebox drawing): queued
//     via takeFormsCommands(), drained by Kotlin each poll tick with no
//     result expected and no blocking -- these can happen as often as a
//     script likes (e.g. updating a label's text every frame) without
//     paying a round-trip each time.
//
// Button clicks and a form's close ("X") button go the other direction
// (Kotlin -> native): notifyFormsClick()/notifyFormsFormClosed() look up
// the Lua callback ref registered for that handle (if any) and queue it
// for dispatchFormsCallbacks() to actually invoke, from the script thread,
// once per emu.frameadvance() -- the same model as desktop's
// LuaFormsManager::takePendingCallbacks().
enum class FormsOp
{
    // Blocking: creation (returns a new handle in intResult)
    NewForm, Button, Label, Checkbox, Textbox, Dropdown, PictureBox,
    // Blocking: getters
    GetText, IsChecked, GetMouseX, GetMouseY, OpenFile,
    // Fire-and-forget: mutations
    SetDropdownItems, SetProperty, SetLocation, SetText, Destroy, DestroyAll,
    // Fire-and-forget: picturebox drawing (handle = the picturebox's own handle)
    DrawText, DrawRectangle, DrawEllipse, DrawImage, Clear, Refresh,
    // Blocking, Android-only (not part of BizHawk's real API, not touched
    // by forms.* at all -- reuses this same bridge purely because it
    // already solves "block the script thread until Kotlin finishes
    // something asynchronous"). See the android.* Lua binding: network
    // operations NDS-Ironmon-Tracker's patched TrackerUpdater.lua needs,
    // since the real os.execute()-based update mechanism can't work on
    // Android (see l_os_execute_stub's comment). Kotlin defers these to a
    // background (IO dispatcher) coroutine before answering.
    HttpGet, DownloadAndExtractUpdate, OpenUrl,
    // Same reasoning as the above, for QuickLoader.lua's "Generate ROMs"
    // quickload mode: the real implementation shells out to `java -jar
    // <randomizer>.jar cli ...`, which can't work on Android either (no
    // JVM/java binary at all, a harder wall than os.execute() -- see
    // android.randomizeRom()'s own comment). Kotlin calls the Universal
    // Pokemon Randomizer's CliRandomizer directly in-process instead (it's
    // compiled into this app as the :randomizer-core module), on a
    // background thread since randomization is CPU-heavy, not just I/O.
    RandomizeRom,
};

struct FormsRequest
{
    FormsOp op;
    int handle = 0; // target widget/form handle; 0 for ops that create one
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0, fillColor = 0;
    std::string text;  // caption/text/propName/path/filter/title
    std::string text2; // propValue, or a second string arg
    std::vector<std::string> items; // dropdown items
    bool boolArg = false; // textbox multiline
};

struct FormsResult
{
    int intResult = 0;
    std::string stringResult;
    bool boolResult = false;
};

// Android port of the desktop (qt_sdl) LuaScriptManager -- runs a single
// BizHawk-API-compatible Lua script (e.g. NDS-Ironmon-Tracker) against this
// emulator instance, on its own thread, the same frame-advance-driven model
// as the desktop build (the script alone drives frame progression via
// emu.frameadvance(), once per call).
//
// Phase 1: memory.*/emu.*/gameinfo.*/bit.*/console.*/savestate.*/
// memorysavestate.*/joypad.*. Phase 2: gui.* (the on-screen overlay --
// drawing happens on the Kotlin side, a Compose Canvas, see
// LuaOverlayUi.kt). Phase 3: forms.* (the tracker's actual native-widget
// UI) -- also rendered on the Kotlin side (LuaFormsOverlayUi.kt), as
// Compose panels/widgets rather than real separate OS windows (Android
// apps can't normally create those); see the FormsOp/FormsRequest
// declarations above for how the cross-thread bridge works.
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

    // Thread-safe: called from Kotlin (via JNI) to pull whatever the script
    // has drawn since the last emu.frameadvance(). Returns a copy.
    std::vector<LuaDrawCommand> getDrawCommands();

    // --- forms.* bridge: all called from Kotlin (any thread calling into
    // native from Kotlin is always the main/Compose thread in practice) ---

    // Consumes the one in-flight blocking request, if any (clears it so it
    // isn't delivered twice).
    std::optional<FormsRequest> pollFormsRequest();
    // Hands the result of the request pollFormsRequest() just returned back
    // to the script thread, which is blocked waiting for exactly this.
    void deliverFormsResult(FormsResult result);
    // Drains all fire-and-forget commands queued since the last call.
    std::vector<FormsRequest> takeFormsCommands();
    // A button (handle) was clicked, or a form (handle)'s close button was
    // pressed -- queues the registered Lua callback ref, if any, for
    // dispatchFormsCallbacks() to invoke from the script thread.
    void notifyFormsClick(int handle);
    void notifyFormsFormClosed(int handle);

    // client.SetGameExtraPadding()'s current values, for the Kotlin-side
    // gui.* overlay to size its NDS-native coordinate space against (see
    // l_client_setgameextrapadding's comment for why this matters).
    void getScreenPadding(int& left, int& top, int& right, int& bottom) const
    {
        left = luaPadLeft.load();
        top = luaPadTop.load();
        right = luaPadRight.load();
        bottom = luaPadBottom.load();
    }

    // Called from Kotlin as the user's finger moves over the gui.* overlay
    // area (NOT the real DS touchscreen -- this is purely so input.getmouse()
    // has something real to report, e.g. for this tracker's own gear-icon
    // click detection). x/y are in the same NDS-native coordinate space as
    // everything else here.
    void setMousePosition(float x, float y, bool pressed)
    {
        mouseX.store(x);
        mouseY.store(y);
        mousePressed.store(pressed);
    }

    // Called from Kotlin when the user taps the overlay's "new run" icon --
    // this tracker's own "start a new run" trigger is normally a
    // simultaneous Start+Select+A+B hold on the real controller (see
    // checkForNextSeedCombo() in the patched Main.lua), impractical on a
    // touchscreen. Consumed (test-and-clear) via android.consumeNewRunRequested()
    // rather than driving the real NDS input mask, so this can't have any
    // side effect on the actual running game.
    void requestNewRun() { newRunRequested.store(true); }

    // Whether the gui.* overlay should let the user manually drag it
    // horizontally right now -- set (every frame, like
    // client.SetGameExtraPadding()) via android.setOverlayScrollEnabled()
    // by whichever specific screens need it (this tracker's Statistics and
    // Log Viewer draw real content into the region the overlay otherwise
    // sacrifices off-screen to keep its main panel visible); off by
    // default so dragging isn't available on every screen just because
    // the overlay is technically wider than the viewport on all of them.
    bool isOverlayScrollEnabled() const { return overlayScrollEnabled.load(); }

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

    static int l_input_getmouse(lua_State* L);

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

    // Blocks until Kotlin delivers a result for req (see pollFormsRequest/
    // deliverFormsResult above).
    FormsResult callFormsBridge(FormsRequest req);
    // Fire-and-forget: queues req for Kotlin to pick up next poll, no wait.
    void queueFormsCommand(const FormsRequest& req);
    // Pops a function argument (if present) into the Lua registry so it can
    // be invoked later from dispatchFormsCallbacks(); returns 0 (an invalid
    // ref) for a missing/nil argument.
    static int refFunctionArg(lua_State* L, int idx);
    // Accepts either a plain array table ({"a","b"}) or an associative one
    // ({[key]=val, ...}), matching how the tracker calls forms.dropdown/
    // forms.setdropdownitems both ways in practice.
    static std::vector<std::string> checkStringList(lua_State* L, int idx);
    // Invokes every Lua callback (button clicks, form close) queued since
    // the last call. Must only run on the script thread; called once per
    // emu.frameadvance().
    void dispatchFormsCallbacks();

    static int l_gui_drawtext(lua_State* L);
    static int l_gui_drawrectangle(lua_State* L);
    static int l_gui_drawline(lua_State* L);
    static int l_gui_drawpixel(lua_State* L);
    static int l_gui_drawellipse(lua_State* L);
    static int l_gui_drawpolygon(lua_State* L);
    static int l_gui_drawimage(lua_State* L);
    static int l_gui_drawimageregion(lua_State* L);
    static int l_gui_clearimagecache(lua_State* L);

    // Parses a BizHawk-style packed 0xAARRGGBB color argument at the given
    // stack index. Absent/nil is treated the same as alpha 0 (invisible),
    // matching how scripts use 0x00000000 to mean "don't draw this part."
    static uint32_t checkColor(lua_State* L, int idx);

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

    // android.*: Android-only additions, not part of BizHawk's API -- see
    // the FormsOp::HttpGet/DownloadAndExtractUpdate comment above.
    static int l_android_getscriptdirectory(lua_State* L);
    static int l_android_httpget(lua_State* L);
    static int l_android_downloadandextractupdate(lua_State* L);
    static int l_android_openurl(lua_State* L);
    static int l_android_consumenewrunrequested(lua_State* L);
    static int l_android_setoverlayscrollenabled(lua_State* L);
    static int l_android_randomizerom(lua_State* L);

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

    // drawCommands accumulates the script's gui.draw*() calls for the frame
    // currently being stepped into. Right after that frame finishes,
    // frameadvance() moves it into displayCommands (what getDrawCommands()
    // returns) and starts drawCommands fresh -- so Kotlin always sees a
    // complete set from one specific frame. Only swapped when non-empty, so
    // a script that doesn't redraw every single frame (relying on the
    // overlay persisting, same as real hardware/BizHawk) doesn't flicker.
    std::mutex drawMutex;
    std::vector<LuaDrawCommand> drawCommands;
    std::vector<LuaDrawCommand> displayCommands;

    // forms.* bridge state -- see the FormsOp/FormsRequest comment above.
    std::mutex formsBridgeMutex;
    std::optional<FormsRequest> pendingFormsRequest;
    sem_t formsResultReady;
    FormsResult formsResult;

    std::mutex formsCommandsMutex;
    std::vector<FormsRequest> formsCommands;

    std::mutex formsCallbackMutex;
    std::map<int, int> formsClickRefs;              // widget handle -> Lua ref
    std::map<int, int> formsCloseRefs;               // form handle -> Lua ref
    std::vector<int> pendingFormsCallbackRefs;       // refs ready to invoke

    // client.SetGameExtraPadding(left, top, right, bottom): extra drawing
    // space beyond the 256x384 NDS screen the script has reserved for its
    // own UI (e.g. this tracker asks for 199px to its right). Atomic since
    // it's written from the script thread but read from Kotlin's overlay
    // rendering loop.
    std::atomic<int> luaPadLeft {0};
    std::atomic<int> luaPadTop {0};
    std::atomic<int> luaPadRight {0};
    std::atomic<int> luaPadBottom {0};

    // Set via setMousePosition(), read by l_input_getmouse(). x/y in the
    // gui.* overlay's own NDS-native coordinate space (written from the
    // Kotlin/main thread, read from the script thread).
    std::atomic<float> mouseX {0};
    std::atomic<float> mouseY {0};
    std::atomic<bool> mousePressed {false};

    // Set via requestNewRun() (Kotlin, on the overlay's "new run" icon tap),
    // test-and-cleared by l_android_consumenewrunrequested().
    std::atomic<bool> newRunRequested {false};

    // Set (every frame) by l_android_setoverlayscrollenabled(), read by
    // isOverlayScrollEnabled().
    std::atomic<bool> overlayScrollEnabled {false};
};

}

#endif // LUASCRIPTMANAGER_H
