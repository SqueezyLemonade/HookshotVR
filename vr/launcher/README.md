# HookshotVR Launcher

The small window that opens before Hookshot starts: the one tool of the release. It builds the game from the player's own
game file with Hookshot's builder underneath (players never see Hookshot's build screens), offers **Hookshot** (Desktop
Mode) or **HookshotVR**, checks the VR runtime and the headset, starts the game itself (no script, no terminal), shows the
changelog, and updates everything in one click. It works with any OpenXR runtime and never starts one by itself. The design is the Launcher board on the canvas (`Launcher.dc.html`).

C++17, Dear ImGui (the repository's `third_party/imgui`) on SDL2 and OpenGL 3.3. SDL2 is loaded when the launcher
starts, as the game does it (`host/hos_sdl.c`), so building needs no SDL headers. What differs between Linux and Windows
is behind one interface, `src/platform.h` (Windows: see the end).

## Build and run

```bash
vr/launcher/build.sh
```

```bash
vr/launcher/build/hookshotvr-launcher
```

`build.sh` needs only a C++17 compiler, and libpng when it is there (for the art slots and `--screenshot`). It never
builds or touches the game. `build.sh test` also runs `tools/selftest.cpp`: the build's lines, a failed build, Cancel,
the game file's check and two launches, all against stand-ins (a fake `make_game.py` and a fake game), never the real
ones. It also checks the VR runtime choice, Start With, the game's environment (no `RX_VK_VRAM_MB`), the desktop entry
(written in a scratch home, a file of the player's kept), and an update through to its switch and clean-up, and a failed one
that leaves the build as it was.

| Option | |
| --- | --- |
| `--state <name>` | One screen with sample values, nothing started or saved: `setup`, `building`, `ready`, `desktop`, `waiting` (Start With set), `notrunning`, `noruntime`, `update`, `updating`, `settings`, `changelog`, `updatelog`. `live` is this PC's own state (with `--screenshot` only) |
| `--screenshot <file.png> [--scale <n>]` | That screen into a PNG, without showing a window |
| `--art <folder>` | The art slots from that folder |
| `--no-art` | The drawn placeholders instead of the art slots |
| `--fake-build <script.py>` | A script in `make_game.py`'s place, to try the build screens: `tools/fake_make_game.py` (`FAKE_FAIL=1` stops it in the compile) |
| `--play [vr\|desktop]` | Play pressed once the launcher is ready (in VR: the runtime and the headset answer); that start's mode, not saved. The desktop entry's right-click actions |
| `--build` | Build pressed once the first start's rows are good (the game file from `launcher.ini`, its check passed) |
| `--install` | The **Hookshot VR** desktop entry (below), then exits |
| `--icon <file.png> [--scale <n>]` | The launcher's icon alone, 256 px at scale 1 |

## The desktop entry

```bash
vr/launcher/build/hookshotvr-launcher --install
```

writes `hookshotvr.desktop` into the app menu (`~/.local/share/applications`) and `Hookshot VR.desktop` onto the desktop
(`xdg-user-dir DESKTOP`), both starting this launcher where it is, with `Terminal=false`, and its icon
(`~/.local/share/HookshotVR/hookshotvr.png`, drawn by the launcher). The desktop's copy is made executable and marked
trusted (`gio`), so GNOME's and Cinnamon's desktops start it without asking. A file of either name that the launcher did not
write (it has no `X-HookshotVR-Launcher` line) is left alone. Run it again after moving the launcher. Windows: not yet.

## What it does

- **Settings file**: `~/.config/HookshotVR/launcher.ini` (`$XDG_CONFIG_HOME`; `%LOCALAPPDATA%\HookshotVR` on Windows). It
  keeps the last mode (when Remember choice is ticked), On Game Start, Updates, VR Runtime (`vr_runtime`, Custom's
  `vr_runtime_json`), Start With (`start_with`), the folders, `game_folder` (the build Play starts, `main-game` until an
  update makes another) and the game file. One key has no row: `logs_folder` (empty: `~/.local/share/HookshotVR/logs`).
- **Hookshot Folder** holds `builder/` (an unpacked Hookshot builder: `builder/usr/python/bin/python3`, as
  `vr/tools/build.sh` wants it), the game it builds (`main-game/`) and the translation every build shares (`main-work/`).
  Not set: the folder beside the launcher that has `builder/` in it (the release archive's layout). **HookshotVR Folder** is the HookshotVR source (not set: `HookshotVR/` or
  `source/` beside the launcher, or the checkout the launcher was built in).
- **First start** (no game built yet): Game file [Choose] runs `make_game.py --check --game <file>` and shows its answer,
  Free space is measured at the Hookshot Folder against 60 GB, and Build runs
  `make_game.py --game <file> --out main-game --work main-work --log <logs>/build.log` with `vr/tools/build.sh`'s
  environment (the builder's toolchain, the source as `HOOKSHOT_TOOLS`, `XDG_CACHE_HOME` = `<Hookshot Folder>/cache`
  unless set), less its `HOOKSHOT_KEEP_BUILD_FILES`: the player's build lets the translated code go once the game is built
  (1.5 GB) and keeps the compiled objects, so an update compiles only what changed. A missing builder or source gets its own row with [Choose].
- **Building** follows `@@stage`, `@@progress`, `@@eta`, `@@done` and `@@error` (`nx2/make_game.py`'s docstring). The
  steps: Check (`read`), Translate (`lift`), Compile (`compile`, `finish`), Shaders (`shaders`). Cancel stops the
  builder's whole process group (a job object on Windows); closing the window cancels too.
- **VR Runtime** (Settings): System Default, WiVRn, SteamVR, Monado or Custom. System Default is the runtime the OpenXR
  loader picks itself (`XR_RUNTIME_JSON` as the launcher was started, then `active_runtime.json` in
  `$XDG_CONFIG_HOME/openxr/1`, `$XDG_CONFIG_DIRS` and `/etc`; on Windows the registry's `ActiveRuntime`), and the game is
  given nothing. WiVRn, SteamVR and Monado are the active runtime when it is that one, else the manifest at its usual
  install path (WiVRn's Flatpak, Steam's SteamVR folder and Steam's Flatpak, the packages' `share/openxr/1/*.json`; on
  Windows the registry's `AvailableRuntimes`); Custom is the `.json` manifest the player chooses (OpenXR runtimes are
  chosen by that file, not by a program). Any of those four goes to the game as `XR_RUNTIME_JSON`.
- **VR runtime rows** (main screen, labels and values): the runtime by name (WiVRn, SteamVR, Monado, else its manifest's
  own name) with **Running** or **Not running**, then **Headset** with Connected, Waiting or Unknown. With no runtime the
  row says **VR runtime**: **None installed** (none at any usual install path), **Not set** (System Default, and no
  active runtime) or **Not found** (the chosen one is missing), with [Change] (Settings).
  WiVRn answers on D-Bus (`io.github.wivrn.Server`, `HeadsetConnected`). SteamVR is never asked with an OpenXR instance
  (that would start SteamVR): its `vrserver` process says it runs. Other runtimes get an OpenXR instance and
  `xrGetSystem`, except while the game runs. Play waits for the runtime and the headset (the game started without them
  runs flat: `vr: no headset ... VR off this run`).
- **Start With** (Settings, off unless the player picks one): an app started before the game in VR, such as WiVRn's
  dashboard, ALVR or SteamVR. An executable, a desktop entry (`.desktop`, its `Exec=`) or, on Windows, a shortcut; in
  `launcher.ini` also a command line. With it set, the runtime row offers [Start], and Play starts it and then the game
  once the runtime and the headset answer (Starting; pressed again, it stops waiting). The launcher never starts a
  runtime by itself, and never stops one.
- **Play** starts the build's `bin/hookshot` (or `hookshot`; `hookshot.exe` on Windows) in its folder, in a session of
  its own, its output to `<logs>/last-run.log` (the run before kept as `last-run.1.log`). With On Game Start: Close
  Launcher the launcher then ends; with Keep Open the button says Running until the game ends. On Windows the game is
  given `--user %APPDATA%\Hookshot`, as Hookshot's own launcher does, so its saves and settings stay put when an update
  builds into a new folder; on Linux `--user ~/.local/share/hookshot` (`$XDG_DATA_HOME`), where the game kept its saves
  already, so its `hookshot_options.ini` lives there too and not beside the build (a build's own is carried over once).
- **Crash reports**: the game's own records (`<game>/bug-reports/pending/report-*.txt`) are copied into
  `<logs>/crashes` when the launcher starts, when a game it started ends, and before an update deletes the old build.

The environment the game gets (`sys::game_env`):

| Variable | Hookshot | HookshotVR | Why |
| --- | --- | --- | --- |
| `HOOKSHOT_NO_UPDATE_CHECK=1` | yes | yes | Hookshot's own update would replace the VR build |
| `HOS_INPUT_BACKGROUND=1`, `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` | yes | yes | controllers keep working when another window has the focus |
| `RX_VK_PRECOMPILE=0` | yes | yes | the build prepared the shaders |
| `HOOKSHOT_VR` | `0` | `1` | said either way: the game reads it before the ini's `vr=` |
| `__GL_SHADER_DISK_CACHE_PATH=<cache>/nvidia`, `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1` | when there | when there | NVIDIA's shader cache the build prepared, under its `XDG_CACHE_HOME` |
| `MESA_SHADER_CACHE_DIR=<cache>` | when there | when there | Mesa's, the same way |
| `HOS_FPS_TARGET=120` | | yes | a ceiling: the game's own limiter (60) lifted out of the headset's way; the runtime paces the frames (`xrWaitFrame`) at the headset's rate, and a 144 Hz headset is held to 120 |
| `RX_VK_IMMEDIATE=1` | | yes | the desktop window without vsync |
| `XR_RUNTIME_JSON` | | the VR Runtime's manifest | System Default: as the launcher was started |
| `RX_VK_PASS_TIMING=1` | when `gpu_profile=1` is in `hookshot_options.ini` | | |

No `RX_VK_VRAM_MB`: every card gets Hookshot's own budget.

## Updates

`src/update.cpp`, off until the public release repository exists: `upd::kReleaseRepo` is empty, so the check finds
nothing and the banner never shows (`--state update` and `--state updating` show it). Never the private backup remote,
and never a token. With it set, `git ls-remote --tags` finds a newer `vX.Y.Z`; Automatic also downloads it and reads its
`vr/launcher/CHANGELOG.txt` for the Update Log; Notify only shows the banner. The banner ("A new HookshotVR update is
available") or Update then does everything, and leaves the build the player plays alone until a new one is whole:

1. **Download** the release's source into `<data>/versions/<vX>/HookshotVR` (for now a shallow `git clone` of the tag).
2. **Build** the game from it into a new folder, `<Hookshot Folder>/<vX>-game`: from the game file when it is still
   there, else from the game files the old folder keeps (`romfs/`, `.hookshot_exefs/`). The translation (`main-work/`)
   is shared, so only what changed builds again.
3. **Check** the build: the builder said `@@done` for that folder, and the game and its `romfs/` are in it.
4. **Switch**: crash reports saved, the player's `hookshot_env.txt` carried over, then `launcher.ini` names the new
   source and folder (one whole write).
5. **Clean** up the old build: its folder in the Hookshot Folder, and its source when the launcher downloaded it. Never a
   folder the player chose.

A failure or Cancel anywhere before the switch deletes what the update made and leaves the old build as it was. What an
update stopped midway left (the launcher closed in it) is cleared at the next start. Untested against a real release.

The version (`upd::kVersion`, v1.0.0, shown as `kVersionName`, Alpha v1.0.0) opens the Changelog, read from `CHANGELOG.txt`: a `vX.Y.Z` line, then its lines.
View Online shows once `kReleasePage` is set.

## Art slots

The launcher draws each slot's PNG from `vr/launcher/art/` (the build copies them beside the launcher; `--art <folder>`
takes them from elsewhere). A missing file falls back to a drawn placeholder (`--no-art` shows those). No Nintendo art,
logos or Triforce.

| File | Size | Where it goes |
| --- | --- | --- |
| `frame.png` (in the repo) | 1670 × 941 px, 9-slice | The leather frame. Cut 150 px left and right, 215 px top, 226 px bottom; drawn 115 / 165 / 174 px on the 1280 × 820 window (the canvas's border-image) |
| `wordmark.png` (in the repo) | 1224 × 331 px, white on transparent | The HOOKSHOTVR title and the line under it, fitted into 565 × 153 px at the top centre |
| `icon_desktop.png` | 144 × 144 px | The Hookshot card's icon, shown at 72 × 72 inside its tile (drawn when missing) |
| `icon_vr.png` | 144 × 144 px | The HookshotVR card's icon (drawn when missing) |

Where the two in the repo come from: `art/PROVENANCE.md`.

Fonts: EB Garamond Medium, SemiBold and Italic in `fonts/` (OFL, static cuts of Google Fonts' variable files: its
`PROVENANCE.md`); the repository's Libertinus Serif when they are missing.

## Files

| File | |
| --- | --- |
| `src/main.cpp` | The screens and their state |
| `src/draw.cpp` | The canvas's look in ImGui draw calls: cut-corner copper buttons, cards, glows, icons, the frame |
| `src/window.cpp` | SDL2 and OpenGL loaded at run time; ImGui's input and its renderer (1.92's texture protocol) |
| `src/system.cpp` | Settings, folders, dialogs, the builder, the game and its environment (the same on every platform) |
| `src/runtime.cpp` | The VR Runtime choice, Start With and the VR checks |
| `src/update.cpp` | The update check and the one-click update |
| `src/platform.h` | All that differs between platforms: folders, processes, dialogs, the OpenXR loader and runtimes, the builder's toolchain |
| `src/platform_posix.cpp`, `src/platform_win32.cpp` | Its Linux half and its Windows half |
| `src/image.cpp` | PNG in and out (libpng) |
| `tools/` | `fake_make_game.py` and `selftest.cpp` |

## Windows

`src/platform_win32.cpp` (CreateProcessW in a job object for Cancel, `IFileDialog`, `%LOCALAPPDATA%\HookshotVR`, the
registry's `ActiveRuntime` and `AvailableRuntimes`, `hookshot.exe`, Start menu and desktop shortcuts for `--install`, UTF-8 to
UTF-16 at its edges) is cross-built on Linux with llvm-mingw:

```bash
HOOKSHOT_MINGW=<llvm-mingw's Linux release> vr/launcher/build-windows.sh
```

into `build/windows/hookshotvr-launcher.exe` (a GUI exe: no console; DEBUG=1 for one), with fonts, art and the changelog
beside it. It needs only Windows' own DLLs and, beside it, `SDL2.dll` with `SDL3.dll` (Hookshot's) and Khronos'
`openxr_loader.dll`, which the launcher copies beside the game before each start (Windows has no system loader). PNGs go
through Windows' own codec (WIC); a UTF-8 manifest is linked in, as the game's. The builder is Hookshot's Windows layout,
`builder\tools\python`, `tools\llvm-mingw`, `tools\cpm`: the release carries it (`scripts/hookshotvr_release.py`).
Checked under Wine (the screens, the builder found, the game file's check with the bundled Python and compiler); the
dialogs, shortcuts, job object and registry on a real Windows PC are not.

The Linux launcher the release ships: `vr/launcher/build-release-linux.sh` (the builder's clang and glibc 2.35 sysroot).
