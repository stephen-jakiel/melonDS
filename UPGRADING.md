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

### The four patch sites (as of this writing)

All four are guarded by `if android ~= nil and android.<thing> ~= nil then
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

## 4. Universal Pokemon Randomizer (planned, not yet built)

Once the in-app randomizer integration exists, it should follow the same
pattern as the Android port (section 2), not the tracker's pattern (section
3) -- this is vendored Java *source* compiled into the app at build time, not
a runtime-downloaded script, so a real git-based workflow is both possible
and worth using:

1. Maintain a fork of `Ajarmar/universal-pokemon-randomizer-zx` under this
   project's GitHub account, with our Android-compatibility changes (strip
   the AWT-dependent GUI preview methods, trim to just the Gen4/Gen5 handlers
   melonDS actually needs) as real commits on top of upstream's history.
2. Pull it into this repo the same way as `android/` -- `git subtree add`
   initially, `git subtree pull` to update -- pinned to a specific commit of
   our fork, not tracking its `master` live.
3. To pick up an upstream fix/feature: fetch upstream into the fork, rebase
   our small patch commit(s) on top, retest, push, then `git subtree pull`
   here to bring the new commit in.

Expect this to be low-frequency maintenance: the Gen4/Gen5 randomization
logic is mature, and most upstream activity is new-game support (Gen8/9)
irrelevant to melonDS.
