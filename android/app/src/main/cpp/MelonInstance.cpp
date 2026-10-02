#include <ctime>
#include <chrono>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <filesystem>
#include <GLES3/gl3.h>
#include "Args.h"
#include "GPU3D_Compute.h"
#include "Configuration.h"
#include "DSi.h"
#include "DSiSupport.h"
#include "DSi_I2C.h"
#include "GPU3D_OpenGL.h"
// Full definitions of the unified (2D+3D) renderers, GLRenderer and
// SoftRenderer -- GPU3D_OpenGL.h/GPU3D_Compute.h only forward-declare
// GLRenderer (it's a friend of their 3D-only backend classes).
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"
#include "MelonDS.h"
#include "MelonInstance.h"
#include "NDS.h"
#include "NDSCart.h"
#include "net/Net_Slirp.h"
#include "Platform.h"
#include "SDCardArgsBuilder.h"

using namespace std;
using namespace melonDS;
using namespace melonDS::Platform;

namespace MelonDSAndroid
{

const int kRewindBufferSize = 1024 * 1024 * 20; // Use 20MB per savestate
const int kRewindScreenshotSize = 256 * 384 * 4;

MelonInstance::MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType) :
    instanceId(instanceId),
    currentConfiguration(configuration),
    net(net),
    screenshotRenderer(std::move(screenshotRenderer)),
    consoleType(consoleType),
    rewindManager(configuration->rewindEnabled, configuration->rewindLengthSeconds, configuration->rewindCaptureSpacingSeconds, kRewindBufferSize, kRewindScreenshotSize)
{
    // Software renderer is always used during initialisation. Actual renderer will be set of first frame run
    currentRenderer = Renderer::Software;
    isRenderConfigurationDirty = true;
    inputMask = 0xFFF;
    frame = 0;

    net->RegisterInstance(instanceId);

    if (consoleType == 1)
    {
        melonDS::DSiArgs &dsiArgs = static_cast<melonDS::DSiArgs &>(*args);
        nds = new DSi(std::move(dsiArgs), this);
    }
    else
    {
        nds = new NDS(std::move(*args), this);
    }

    if (configuration->userInternalFirmwareAndBios)
    {
        std::filesystem::path firmwarePath = MelonDSAndroid::internalFilesDir;
        firmwarePath /= "wfcsettings.bin";
        firmwareSave = std::make_unique<SaveManager>(firmwarePath);
    }
    else
    {
        std::string firmwarePathString;
        if (consoleType == 1)
            firmwarePathString = configuration->dsiFirmwarePath;
        else
            firmwarePathString = configuration->dsFirmwarePath;

        firmwareSave = std::make_unique<SaveManager>(firmwarePathString);
    }

    // All instances have a RetroAchievements manager, but only the first instance will actually load achievements
    retroAchievementsManager = std::make_unique<RetroAchievements::RetroAchievementsManager>(nds);
    luaScriptManager = std::make_unique<LuaScriptManager>(nds, &inputMask);

    nds->Reset();
    setBatteryLevels();
    setDateTime();
}

MelonInstance::~MelonInstance()
{
    frameQueue.clear();
    net->UnregisterInstance(instanceId);
    delete nds;
}

bool MelonInstance::loadRom(std::string romPath, std::string sramPath)
{
    unique_ptr<u8[]> romData;
    unique_ptr<u8[]> sramData;
    u32 romFileLength = 0;
    u32 sramFileLength = 0;

    // ROM file loading
    Platform::FileHandle* romFile = Platform::OpenFile(romPath, FileMode::Read);
    if (!romFile)
        return false;

    u64 length = Platform::FileLength(romFile);
    if (length > 0x40000000)
    {
        Platform::CloseFile(romFile);
        return false;
    }

    romFileLength = (u32) length;
    Platform::FileRewind(romFile);
    romData = make_unique<u8[]>(romFileLength);
    size_t nread = Platform::FileRead(romData.get(), (size_t) romFileLength, 1, romFile);
    Platform::CloseFile(romFile);
    if (nread != 1)
    {
        return false;
    }

    // SRAM file loading
    FileHandle* sramFile = Platform::OpenFile(sramPath, FileMode::Read);
    if (!sramFile)
    {
        return false;
    }
    else if (!Platform::CheckFileWritable(sramPath))
    {
        return false;
    }

    sramFileLength = (u32) Platform::FileLength(sramFile);

    FileRewind(sramFile);
    sramData = std::make_unique<u8[]>(sramFileLength);
    FileRead(sramData.get(), sramFileLength, 1, sramFile);
    CloseFile(sramFile);

    NDSCart::NDSCartArgs cartargs{
        // Don't load the SD card itself yet, because we don't know if
        // the ROM is homebrew or not.
        // So this is the card we *would* load if the ROM were homebrew.
        .SDCard = std::nullopt, // getSDCardArgs("DLDI"), // TODO: Re-enable this
        .SRAM = std::move(sramData),
        .SRAMLength = sramFileLength,
    };

    auto cart = NDSCart::ParseROM(std::move(romData), romFileLength, this, std::move(cartargs));
    if (!cart)
    {
        return false;
    }

    nds->SetNDSCart(std::move(cart));
    ndsSave = std::make_unique<SaveManager>(sramPath);

    return true;
}

bool MelonInstance::loadGbaRom(std::string romPath, std::string sramPath)
{
    unique_ptr<u8[]> romData;
    unique_ptr<u8[]> sramData = nullptr;
    u32 romFileLength = 0;
    u32 sramFileLength = 0;

    // ROM file loading
    Platform::FileHandle* romFile = Platform::OpenFile(romPath, FileMode::Read);
    if (!romFile)
        return false;

    u64 length = Platform::FileLength(romFile);
    if (length > 0x40000000)
    {
        Platform::CloseFile(romFile);
        return false;
    }

    romFileLength = length;
    Platform::FileRewind(romFile);
    romData = make_unique<u8[]>(romFileLength);
    size_t nread = Platform::FileRead(romData.get(), (size_t) romFileLength, 1, romFile);
    Platform::CloseFile(romFile);
    if (nread != 1)
    {
        return false;
    }

    FileHandle* saveFile = Platform::OpenFile(sramPath, FileMode::Read);
    if (!saveFile)
    {
        return false;
    }
    else if (!Platform::CheckFileWritable(sramPath))
    {
        return false;
    }

    sramFileLength = (u32) FileLength(saveFile);

    if (sramFileLength > 0)
    {
        FileRewind(saveFile);
        sramData = std::make_unique<u8[]>(sramFileLength);
        FileRead(sramData.get(), sramFileLength, 1, saveFile);
    }
    CloseFile(saveFile);

    auto cart = GBACart::ParseROM(std::move(romData), romFileLength, std::move(sramData), sramFileLength, this);
    if (!cart)
    {
        return false;
    }

    nds->SetGBACart(std::move(cart));
    gbaSave = std::make_unique<SaveManager>(sramPath);

    return true;
}

void MelonInstance::loadRumblePak()
{
    auto rumblePakCart = GBACart::LoadAddon(GBAAddon_RumblePak, this);
    nds->SetGBACart(std::move(rumblePakCart));
}

void MelonInstance::loadGbaMemoryExpansion()
{
    auto memoryExpansionCart = GBACart::LoadAddon(GBAAddon_RAMExpansion, this);
    nds->SetGBACart(std::move(memoryExpansionCart));
}

void MelonInstance::loadMotionPakHomebrew()
{
    auto motionPakCart = GBACart::LoadAddon(GBAAddon_MotionPakHomebrew, this);
    nds->SetGBACart(std::move(motionPakCart));
}

void MelonInstance::loadMotionPakRetail()
{
    auto motionPakCart = GBACart::LoadAddon(GBAAddon_MotionPakRetail, this);
    nds->SetGBACart(std::move(motionPakCart));
}

bool MelonInstance::bootFirmware()
{
    if (nds->NeedsDirectBoot())
        return false;

    return true;
}

void MelonInstance::start()
{
    auto cart = nds->NDSCartSlot.GetCart();
    if (nds->ConsoleType == 1 && cart != nullptr && cart->GetHeader().IsDSiWare() && !currentConfiguration->showBootScreen)
    {
        auto dsi = (DSi*) nds;
        DSiSupport::SetupDSiDirectBoot(dsi);
    }
    else if (!currentConfiguration->showBootScreen || nds->NeedsDirectBoot())
    {
        // This seems to be unused, but it's required
        std::string romName;
        nds->SetupDirectBoot(romName);
    }
    nds->Start();

    screenshotRenderer->init();
}

void MelonInstance::reset()
{
    nds->Reset();
    setBatteryLevels();
    setDateTime();

    // If there is a cart inserted, check if direct boot is required
    if (nds->GetNDSCart())
    {
        if (!currentConfiguration->showBootScreen || nds->NeedsDirectBoot())
        {
            // This seems to be unused, but it's required
            std::string romName;
            nds->SetupDirectBoot(romName);
        }
    }

    rewindManager.Reset();
    retroAchievementsManager->Reset();
    nds->Start();
}

u32 MelonInstance::runFrame()
{
    if (isRenderConfigurationDirty)
    {
        updateRenderer();
        isRenderConfigurationDirty = false;
    }

    int screenWidth;
    int screenHeight;
    if (currentRenderer == Renderer::OpenGl)
    {
        auto glRenderSettings = static_cast<OpenGlRenderSettings&>(*currentConfiguration->renderSettings);
        int scale = glRenderSettings.scale;
        screenWidth = 256 * scale;
        screenHeight = (192 + 1) * scale;
    }
    else if (currentRenderer == Renderer::Compute)
    {
        auto computeRenderSettings = static_cast<ComputeRenderSettings&>(*currentConfiguration->renderSettings);
        int scale = computeRenderSettings.scale;
        screenWidth = 256 * scale;
        screenHeight = (192 + 1) * scale;
    }
    else
    {
        screenWidth = 256;
        screenHeight = 192 + 1;
    }

    Frame* renderFrame = frameQueue.getRenderFrame();

    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    // Delete old render fence
    if (renderFrame->renderFence)
    {
        eglDestroySyncKHR(currentDisplay, renderFrame->renderFence);
        renderFrame->renderFence = 0;
    }

    // Ensure presentation is finished
    if (renderFrame->presentFence)
    {
        eglWaitSyncKHR(currentDisplay, renderFrame->presentFence, 0);
    }

    // Validate frame after ensuring that the frame has finished presenting
    frameQueue.validateRenderFrame(renderFrame, screenWidth, screenHeight * 2);

    [[unlikely]] if (nds->GPU.GetRenderer().NeedsShaderCompile())
    {
        // Compile all required shaders at once
        do
        {
            int currentShader;
            int shadersCount;
            nds->GPU.GetRenderer().ShaderCompileStep(currentShader, shadersCount);
        }
        while (nds->GPU.GetRenderer().NeedsShaderCompile());
    }

    // The renderer no longer accepts an externally-supplied output texture to
    // render directly into; it always owns its own output storage, which we
    // retrieve via GetFramebuffers() below, after the frame has run.
    u32 nLines = nds->RunFrame();
    retroAchievementsManager->FrameUpdate();

    void* topBuf = nullptr;
    void* bottomBuf = nullptr;
    // true -> topBuf/bottomBuf are RAM framebuffers (software renderer).
    // false -> topBuf points at the accelerated renderer's own GL texture handle.
    bool isRamBased = nds->GPU.GetRenderer().GetFramebuffers(&topBuf, &bottomBuf);

    if (isRamBased)
    {
        if (topBuf && bottomBuf)
        {
            glBindTexture(GL_TEXTURE_2D, renderFrame->frameTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, topBuf);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192 + 2, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, bottomBuf);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    else
    {
        // The accelerated renderer (GLRenderer) hands back a single GL_TEXTURE_2D_ARRAY
        // texture with 2 layers (0 = top screen, 1 = bottom screen), already scaled to
        // its configured resolution. Copy both layers directly into our plain 2D frame
        // texture, stacked the same way the software path lays out its RAM buffers
        // (a scaled 2px gap between screens), using glCopyImageSubData (core in GLES 3.2,
        // which this project targets) rather than an FBO blit or shader pass.
        GLuint outputTex = *static_cast<GLuint*>(topBuf);
        int scale = screenWidth / 256;

        glCopyImageSubData(
            outputTex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
            renderFrame->frameTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
            screenWidth, 192 * scale, 1);
        glCopyImageSubData(
            outputTex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1,
            renderFrame->frameTexture, GL_TEXTURE_2D, 0, 0, (192 + 2) * scale, 0,
            screenWidth, 192 * scale, 1);
    }

    bool isSleeping = nds->CPUStop & CPUStop_Sleep;
    if (!isSleeping) [[likely]]
    {
        renderFrame->renderFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
        glFlush();
        frameQueue.pushRenderedFrame(renderFrame);
    }
    else
    {
        frameQueue.discardRenderedFrame(renderFrame);
    }

    if (ndsSave)
        ndsSave->CheckFlush();

    if (gbaSave)
        gbaSave->CheckFlush();

    if (firmwareSave)
        firmwareSave->CheckFlush();

    frame++;
    bool needsRewindCapture = rewindManager.ShouldCaptureState(frame);
    bool needsScreenshot = screenshotRenderer->isScreenshotPending();

    if (needsRewindCapture || needsScreenshot) [[unlikely]]
        screenshotRenderer->renderScreenshot(&nds->GPU, currentRenderer, renderFrame);

    if (needsRewindCapture)
    {
        auto nextRewindState = rewindManager.GetNextRewindSaveState(frame);
        saveRewindState(nextRewindState);
    }

    return nLines;
}

void MelonInstance::stop()
{
    retroAchievementsManager = nullptr;
    screenshotRenderer->cleanup();
}

void MelonInstance::updateMotionData(float ax, float ay, float az, float rx, float ry, float rz)
{
    motionData[MotionQueryType::MotionAccelerationX].store(ax, std::memory_order_relaxed);
    motionData[MotionQueryType::MotionAccelerationY].store(ay, std::memory_order_relaxed);
    motionData[MotionQueryType::MotionAccelerationZ].store(az, std::memory_order_relaxed);
    motionData[MotionQueryType::MotionRotationX].store(rx, std::memory_order_relaxed);
    motionData[MotionQueryType::MotionRotationY].store(ry, std::memory_order_relaxed);
    motionData[MotionQueryType::MotionRotationZ].store(rz, std::memory_order_relaxed);
}

float MelonInstance::getMotionData(MotionQueryType type)
{
    if (type < MotionQueryType::MotionAccelerationX || type > MotionQueryType::MotionRotationZ)
        return 0.0f;

    return motionData[type].load(std::memory_order_relaxed);
}

void MelonInstance::touchScreen(u16 x, u16 y)
{
    nds->TouchScreen(x, y);
}

void MelonInstance::releaseScreen()
{
    nds->ReleaseScreen();
}

void MelonInstance::pressKey(u32 key)
{
    // Special handling for Lid input
    if (key == 16 + 7)
    {
        nds->SetLidClosed(true);
    }
    else
    {
        inputMask &= ~(1 << key);
        nds->SetKeyMask(inputMask);
    }
}

void MelonInstance::releaseKey(u32 key)
{
    // Special handling for Lid input
    if (key == 16 + 7)
    {
        nds->SetLidClosed(false);
    }
    else
    {
        inputMask |= (1 << key);
        nds->SetKeyMask(inputMask);
    }
}

int MelonInstance::readAudioOutput(s16* buffer, int length)
{
    return nds->SPU.ReadOutput(buffer, length);
}

void MelonInstance::setAudioOutputSkew(double skew)
{
    nds->SPU.SetOutputSkew(skew);
}

bool MelonInstance::takeScreenshot()
{
    return screenshotRenderer->takeScreenshot();
}

void MelonInstance::loadCheats(std::list<Cheat> cheats)
{
    std::vector<ARCode> codeList;

    for (auto cheat : cheats)
    {
        ARCode arCode {
            .Enabled = true,
            .Code = cheat.code,
        };
        codeList.push_back(arCode);
    }

    nds->AREngine.Cheats = codeList;
}

int MelonInstance::sendNetPacket(u8* data, int length)
{
    return net->SendPacket(data, length, instanceId);
}

int MelonInstance::receiveNetPacket(u8* data)
{
    return net->RecvPacket(data, instanceId);
}

Frame* MelonInstance::getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline)
{
    return frameQueue.getPresentFrame(deadline);
}

void MelonInstance::updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration)
{
    if (nds)
    {
        nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(newConfiguration->audioSettings.audioInterpolation));
        nds->SPU.SetDegrade10Bit(static_cast<AudioBitDepth>(newConfiguration->audioSettings.audioBitrate));
    }

    rewindManager.UpdateRewindSettings(newConfiguration->rewindEnabled, newConfiguration->rewindLengthSeconds, newConfiguration->rewindCaptureSpacingSeconds);

    currentConfiguration = newConfiguration;
    isRenderConfigurationDirty = true;
}

void MelonInstance::requestNdsSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (ndsSave)
        ndsSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

void MelonInstance::requestGbaSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (gbaSave)
        gbaSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

void MelonInstance::requestFirmwareSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (firmwareSave)
        firmwareSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

bool MelonInstance::saveState(Savestate* state)
{
    if (!retroAchievementsManager->DoSavestate(state))
        return false;

    return nds->DoSavestate(state);
}

bool MelonInstance::loadState(Savestate* state)
{
    if (!retroAchievementsManager->DoSavestate(state))
        return false;

    if (nds->DoSavestate(state))
    {
        setBatteryLevels();
        setDateTime();
        return true;
    }
    else
    {
        return false;
    }
}

RewindWindow MelonInstance::getRewindWindow()
{
    return RewindWindow {
        .currentFrame = frame,
        .rewindStates = rewindManager.GetRewindWindow(),
    };
}

bool MelonInstance::loadRewindState(RewindSaveState rewindSaveState)
{
    Savestate* savestate = new Savestate(rewindSaveState.buffer, rewindSaveState.bufferContentSize, false);
    if (savestate->Error)
    {
        delete savestate;
        return false;
    }

    bool result = loadState(savestate);
    if (result)
    {
        frame = rewindSaveState.frame;
        rewindManager.OnRewindFromState(rewindSaveState);
    }

    delete savestate;

    return result;
}

void MelonInstance::setupAchievements(
    std::list<RetroAchievements::RAAchievement> achievements,
    std::list<RetroAchievements::RALeaderboard> leaderboards,
    std::optional<std::string> richPresenceScript
)
{
    if (instanceId == 0)
    {
        retroAchievementsManager->LoadAchievements(achievements);
        retroAchievementsManager->LoadLeaderboards(leaderboards);
        if (richPresenceScript)
            retroAchievementsManager->SetupRichPresence(*richPresenceScript);
    }
}

void MelonInstance::unloadRetroAchievementsData()
{
    retroAchievementsManager->UnloadEverything();
}

std::string MelonInstance::getRichPresenceStatus()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRichPresenceStatus();
    else
        return "";
}

std::vector<RetroAchievements::RARuntimeAchievement> MelonInstance::getRuntimeAchievements()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeAchievements();
    else
        return { };
}

void MelonInstance::startLuaScript(const std::string& path)
{
    luaScriptManager->start(path);
}

void MelonInstance::stopLuaScript()
{
    luaScriptManager->stop();
}

bool MelonInstance::isLuaScriptActive()
{
    return luaScriptManager->isActive();
}

bool MelonInstance::waitForLuaStepRequest()
{
    return luaScriptManager->waitForStepRequest();
}

void MelonInstance::signalLuaStepComplete()
{
    luaScriptManager->signalStepComplete();
}

std::vector<LuaDrawCommand> MelonInstance::getLuaDrawCommands()
{
    return luaScriptManager->getDrawCommands();
}

std::optional<FormsRequest> MelonInstance::pollLuaFormsRequest()
{
    return luaScriptManager->pollFormsRequest();
}

void MelonInstance::deliverLuaFormsResult(FormsResult result)
{
    luaScriptManager->deliverFormsResult(std::move(result));
}

std::vector<FormsRequest> MelonInstance::takeLuaFormsCommands()
{
    return luaScriptManager->takeFormsCommands();
}

void MelonInstance::notifyLuaFormsClick(int handle)
{
    luaScriptManager->notifyFormsClick(handle);
}

void MelonInstance::notifyLuaFormsFormClosed(int handle)
{
    luaScriptManager->notifyFormsFormClosed(handle);
}

void MelonInstance::getLuaScreenPadding(int& left, int& top, int& right, int& bottom)
{
    luaScriptManager->getScreenPadding(left, top, right, bottom);
}

void MelonInstance::updateRenderer()
{
    Renderer newRenderer = currentConfiguration->renderer;

    if (newRenderer != currentRenderer)
    {
        switch (newRenderer)
        {
            case Renderer::Software:
                nds->GPU.SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
            case Renderer::OpenGl:
                nds->GPU.SetRenderer(std::make_unique<GLRenderer>(*nds, /* compute */ false));
                break;
            case Renderer::Compute:
                // The compute-shader 3D backend is now just an internal option of
                // GLRenderer (which handles both 2D and 3D output together),
                // rather than a separate top-level renderer.
                nds->GPU.SetRenderer(std::make_unique<GLRenderer>(*nds, /* compute */ true));
                break;
            default: __builtin_unreachable();
        }
        currentRenderer = newRenderer;
    }

    // 2D and 3D rendering are no longer configured separately; a single
    // RendererSettings is passed to whichever unified Renderer is active.
    melonDS::RendererSettings settings {};
    switch (newRenderer)
    {
        case Renderer::Software:
        {
            auto softwareRenderSettings = static_cast<SoftwareRenderSettings&>(*currentConfiguration->renderSettings);
            settings.ScaleFactor = 1;
            settings.Threaded = softwareRenderSettings.threadedRendering;
            settings.HiresCoordinates = false;
            settings.BetterPolygons = false;
            break;
        }
        case Renderer::OpenGl:
        {
            auto glRenderSettings = static_cast<OpenGlRenderSettings&>(*currentConfiguration->renderSettings);
            settings.ScaleFactor = glRenderSettings.scale;
            settings.Threaded = false;
            settings.HiresCoordinates = false;
            settings.BetterPolygons = glRenderSettings.betterPolygons;
            break;
        }
        case Renderer::Compute:
        {
            auto computeRenderSettings = static_cast<ComputeRenderSettings&>(*currentConfiguration->renderSettings);
            settings.ScaleFactor = computeRenderSettings.scale;
            settings.Threaded = false;
            settings.HiresCoordinates = computeRenderSettings.highResCoordinates;
            settings.BetterPolygons = false;
            break;
        }
        default: __builtin_unreachable();
    }
    nds->GPU.GetRenderer().SetRenderSettings(settings);
}

void MelonInstance::setBatteryLevels()
{
    if (consoleType == 1)
    {
        auto dsi = static_cast<DSi*>(nds);
        dsi->I2C.GetBPTWL()->SetBatteryLevel(DSi_BPTWL::batteryLevel_Full);
        dsi->I2C.GetBPTWL()->SetBatteryCharging(false);
    }
    else
    {
        nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
    }
}

void MelonInstance::setDateTime()
{
    std::time_t t = std::time(0);
    std::tm* now = std::localtime(&t);

    nds->RTC.SetDateTime(now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);
}

void MelonInstance::saveRewindState(RewindSaveState* rewindSaveState)
{
    Savestate* savestate = new Savestate(rewindSaveState->buffer, rewindSaveState->bufferSize, true);
    if (saveState(savestate))
    {
        rewindSaveState->bufferContentSize = savestate->Length();
        memcpy(rewindSaveState->screenshot, screenshotRenderer->getScreenshot(), rewindSaveState->screenshotSize);
    }

    delete savestate;
}

}