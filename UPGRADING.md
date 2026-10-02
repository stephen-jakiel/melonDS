# Upgrading this project's vendored/forked components

This fork pulls in code from several outside projects, each handled differently.
This doc covers how to bring in upstream changes for each one without losing
this fork's own patches.

## 1. melonDS core (desktop)

This repo's `origin` (`stephen-jakiel/melonDS`) is a long-running fork of
`melonDS-emu/melonDS` (the `upstream` remote). Core emulation code (`src/*.cpp`,
NDS/DSi/GPU/CPU logic) is largely untouched by this fork's Lua-scripting and
Android work, so merges here are usually low-risk.

```sh
git fetch upstream
git merge upstream/master   # or: git rebase upstream/master
```

**Where conflicts are actually likely:** `src/frontend/qt_sdl/LuaScriptManager.cpp`,
`LuaScriptManager.h`, `EmuInstance.cpp`, `EmuInstance.h`, `EmuThread.cpp` --
this fork's Lua scripting support lives here, so if upstream touches the same
files (e.g. refactors `EmuInstance`'s ROM-loading path), resolve conflicts by
keeping this fork's Lua-related additions and taking upstream's unrelated
changes. After merging, rebuild and smoke-test the desktop build (see
`BUILD.md`), then re-test Lua scripting manually (load a script, run a few
frames) since nothing automated covers that path yet.

## 2. Android port (`android/` directory)

`android/` was vendored from `rafaelvcaetano/melonDS-android` (`android-app`
remote) and `melonDS-android-lib` (`android-lib` remote) via `git subtree`
(see commits `851a8197` and `0d0f6262`), not a submodule -- the files are
regular tracked files in this repo with subtree merge history.

To pull in upstream Android-port updates:

```sh
git fetch android-app
git subtree pull --prefix=android android-app master --squash
```

(`android-lib` has several topic branches -- `jit`, `dsi-camera`, `imgui`,
etc. -- only cherry-pick from those if you specifically want that feature;
`master` is the one to track normally.)

**Where conflicts are actually likely:** every file this session's Lua
scripting work touched, since none of it exists upstream:
`android/app/src/main/cpp/LuaScriptManager.{h,cpp}`,
`MelonInstance.{h,cpp}`, `MelonDS.{h,cpp}`, `MelonDSAndroidJNI.cpp`, and
`android/app/src/main/java/me/magnum/melonds/ui/emulator/lua/*.kt`. A real
conflict here (upstream modifying the same function) needs manual resolution;
upstream adding unrelated new files/features should merge cleanly. After
merging, rebuild with `./gradlew assembleGitHubProdDebug` (see the note in
project memory on why not plain `assembleDebug` -- it builds every product
flavor at once and reliably OOMs the Gradle daemon's default 1.5GB heap) and
re-test Lua scripting on-device.

## 3. NDS-Ironmon-Tracker (the Lua script)

This one is **not version-controlled at all**. It lives at
`build/NDS-Ironmon-Tracker/` on disk, which is gitignored (`build*/` in
`.gitignore`), either freshly downloaded or updated via the tracker's own
in-app "Check for Updates" feature.

**This is the one that actively bites you**: both a fresh download and the
in-app updater overwrite the whole folder with unpatched upstream content,
silently discarding the Android-compatibility patches below. There is no way
around this short of the tracker's own maintainers accepting these patches
upstream -- treat every update as "re-apply patches," not "update and go."

### Re-applying patches after an update

1. Pull the current (freshly-updated) files from the device and diff them
   against your last-known-patched local copies **before** overwriting
   anything -- a real upstream change can land in the same file in the same
   window (this has happened: an unrelated commented-out block appeared in
   `Main.lua` alongside a tracker update once). Don't assume the only diff
   is your own patch; diff first, then decide.
2. Re-apply the four patches below (search each file for the literal string
   `melonDS-android patch` to find every patch site quickly).
3. Push the files back to the device:
   ```sh
   adb push "<local file>" "//sdcard/__push_tmp"
   adb shell "cat //sdcard/__push_tmp | run-as me.magnum.melonds.dev sh -c 'cat > <path on device>'"
   adb shell rm "//sdcard/__push_tmp"
   ```
   (Device paths are under
   `/storage/emulated/0/Android/data/me.magnum.melonds.dev/files/NDS-Ironmon-Tracker/ironmon_tracker/`.
   The double leading slash on the temp path avoids MSYS/Git-Bash mangling
   it into a Windows path on this machine.)

### The five patch sites (as of this writing)

All five are guarded by `if android ~= nil and android.<thing> ~= nil then
... else <original BizHawk behavior> end`, so a patched copy stays compatible
if it were ever run on real BizHawk/desktop.

**`Main.lua`, `Paths.CURRENT_DIRECTORY` resolution** (inside `Main()`, right
after the `dofile`s, before `local settings`) -- the original resolves this
via `os.execute("pwd")`, which is disabled on Android:
```lua
if android ~= nil and android.getScriptDirectory ~= nil then
    Paths.CURRENT_DIRECTORY = android.getScriptDirectory()
elseif Paths.SLASH == "\\" then
    Paths.CURRENT_DIRECTORY = MiscUtils.runExecuteCommand("cd")
else
    Paths.CURRENT_DIRECTORY = MiscUtils.runExecuteCommand("pwd")
end
```

**`Main.lua`, `checkForNextSeedCombo()`** -- adds a check for the overlay's
touch-friendly "new run" icon before the original physical Start+Select+A+B
combo check:
```lua
local function checkForNextSeedCombo()
    if program ~= nil and not program.isInControlsMenu() then
        if android ~= nil and android.consumeNewRunRequested ~= nil and android.consumeNewRunRequested() then
            return true
        end
        local check = MiscUtils.split(settings.controls.LOAD_NEXT_SEED, " ")
        -- ...unchanged from here
```

**`TrackerUpdater.lua`**, three spots:
- A new `runBatchCommandAndroid()` function (inserted right before the
  original `runBatchCommand()`), used instead of shelling out to
  curl/tar/cp:
  ```lua
  local function runBatchCommandAndroid()
      local TAR_URL = "https://github.com/Brian0255/NDS-Ironmon-Tracker/archive/main.tar.gz"
      print(string.format("Installing upgrade to version " .. self.getNewestVersionString() .. "."))
      local success = android.downloadAndExtractUpdate(TAR_URL, Paths.CURRENT_DIRECTORY)
      if not success then
          print("Error trying to install: Unable to download, extract, or overwrite files properly.")
          return false
      end
      print("Update completed successfully.")
      return true
  end
  ```
- In `updateLatestVersion()`, prefer `android.httpGet()` over `curl` via
  `os.execute`:
  ```lua
  if android ~= nil and android.httpGet ~= nil then
      response = android.httpGet(versionURL)
  else
      local command = "curl " .. versionURL .. " --ssl-no-revoke"
      response = MiscUtils.runExecuteCommand(command)
  end
  ```
- In `self.downloadUpdate()`, call the new function instead of the original:
  ```lua
  if android ~= nil and android.downloadAndExtractUpdate ~= nil then
      success = runBatchCommandAndroid()
  else
      success = runBatchCommand()
  end
  ```

**`ui/UpdaterScreen.lua`, `onOpenReleaseNotesClick()`** -- the original opens
a URL via `os.execute('start "" "<url>"')`, a no-op on Android:
```lua
if android ~= nil and android.openUrl ~= nil then
    android.openUrl(releaseNotesURL)
else
    os.execute(string.format('start "" "%s"', releaseNotesURL))
end
```

**`QuickLoader.lua`, `generateROM()`** -- the original shells out to
`java -jar <randomizer>.jar cli ...` to run the Universal Pokemon Randomizer,
which can't work at all on Android (no JVM/`java` binary, a harder wall than
`os.execute()` -- see section 4 below for how this is solved instead):
```lua
local useNativeRandomizer = android ~= nil and android.randomizeRom ~= nil
-- ...
if not useNativeRandomizer then
    paths.JARPath = quickLoadSettings.JAR_PATH  -- skip this file-existence check on Android
end
-- ...
if useNativeRandomizer then
    local success = android.randomizeRom(paths.RNQSPath, paths.ROMPath, nextRomPath)
    if not success then
        FormsUtils.displayError('Next ROM failed to generate.')
        return nil
    end
else
    -- ...original java -jar shellout, unchanged
end
```

## 4. Universal Pokemon Randomizer ZX (`android/randomizer-core`)

Built, not just planned: the randomizer's Java source is vendored and
compiled directly into the app (see `android.randomizeRom()` above), not
shelled out to as a jar, since there's no JVM on Android at all. Follows the
same pattern as the Android port (section 2), not the tracker's pattern
(section 3) -- this is vendored source compiled at build time, not a
runtime-downloaded script, so a real git-based workflow applies:

1. A fork, `stephen-jakiel/universal-pokemon-randomizer-zx`
   (`android-compatibility` branch), carries our Android-compatibility
   changes as real commits on top of `Ajarmar/universal-pokemon-randomizer-zx`'s
   history:
   - Deleted: `newgui/` (all Swing GUI), `ctr/` (3DS file formats, AMX/BFLIM/
     GARCArchive/Mini/NCCH/RomfsFile/SMDH), `GFXFunctions.java`, `launcher/`,
     `Gen6RomHandler.java`/`Gen7RomHandler.java` (3DS-only games),
     `Abstract3DSRomHandler.java`.
   - Stripped: every `getMascotImage()` override (Gen1-5RomHandler) and the
     `RomHandler` interface method itself -- the only other
     `java.awt`/`javax.swing` touch points, a Swing-only sprite preview never
     called from the CLI entry point.
   - **Gen1RomHandler/Gen2RomHandler/Gen3RomHandler are kept** (just their
     `getMascotImage()` stripped), even though melonDS only emulates DS/DSi
     and `CliRandomizer` only needs Gen4/Gen5 for that -- `Randomizer.java`
     and `Settings.java` have `instanceof Gen1RomHandler` etc. checks deep in
     shared logic that Gen4/Gen5 randomization itself depends on. Don't
     delete these three; Gen6/Gen7 were safe to delete only because nothing
     outside `CliRandomizer.java`'s own handler-list referenced them.
   - `config/` (ROM offset `.ini` tables), `patches/` (binary `.ips` files),
     and `newgui/Bundle.properties` (UI strings) are loaded at runtime via
     `FileFunctions`'s `getResourceAsStream()` calls but were **never tracked
     in upstream's git repo at all** -- they were recovered by extracting
     them from the official release jar
     (`gh api repos/Ajarmar/universal-pokemon-randomizer-zx/releases/latest`,
     unzip `PokeRandoZX.jar`) into matching paths in the fork. If a future
     `git subtree pull` ever appears to lose `config/`, `patches/`, or
     `newgui/Bundle.properties`, re-check this -- it means upstream still
     doesn't track them and they need re-extracting from whatever the
     current release jar is.
   - Verify after any change: `grep -rln "import java\.awt\|import javax\.swing" src/`
     must return nothing, and a standalone `javac` compile of everything
     under `src/` must succeed.
2. Pulled into this repo at `android/randomizer-core` via `git subtree add`
   (see commits `9906f4be`/`8de14c1b`), same mechanism as `android/` itself --
   `git subtree pull --prefix=android/randomizer-core randomizer android-compatibility --squash`
   to update (add the `randomizer` remote first if it's missing:
   `git remote add randomizer https://github.com/stephen-jakiel/universal-pokemon-randomizer-zx.git`).
3. `android/randomizer-core/build.gradle.kts` is a plain `java-library`
   module (not `com.android.library`) -- deliberately so, since compiling
   against a real JDK rather than Android's stripped `android.jar` is what
   lets the fork's source compile unmodified outside the handful of actual
   AWT/Swing call sites already removed on the fork side. Its `sourceSets`
   point straight at the subtree's own `src/com/dabomstew/...` layout rather
   than moving files to Gradle's usual `src/main/java/...` convention, to
   keep the directory a clean 1:1 mirror of the fork for future subtree
   pulls.
4. The Lua binding is `android.randomizeRom(settingsPath, inputRomPath,
   outputRomPath)` (`LuaScriptManager.h`/`.cpp`'s `RandomizeRom` FormsOp,
   dispatched in `LuaFormsOverlayUi.kt`), which calls
   `com.dabomstew.pkrandom.cli.CliRandomizer.invoke(String[])` in-process on
   a background thread with synthesized CLI-style args
   (`-s <settings> -i <input> -o <output> -l`), the same public entry point
   the jar's own `java -jar ... cli ...` invocation would have used.
5. To pick up an upstream fix/feature: fetch upstream into the fork, rebase
   our patch commits on top, retest (`grep`/`javac` checks above, then
   `./gradlew assembleGitHubProdDebug`), push, then `git subtree pull` here.

Expect this to be low-frequency maintenance: the Gen4/Gen5 randomization
logic is mature, and most upstream activity is new-game support (Gen8/9)
irrelevant to melonDS.
