# HookshotVR: install and play

**HookshotVR Alpha v1.0.2.** Unofficial. Not affiliated with Nintendo or the Hookshot project. Bring your own legally
obtained copy of the game.

**VR menu:** hold the left Menu button (or Y) for half a second. View › First person / Third person / Window.

HookshotVR plays *Ocarina of Time* in a VR headset, on your PC, in first or third person. It is a VR fork of
[Hookshot](hookshot-readme.md). It contains no game files: a launcher builds the game on your PC from your own copy, and
never changes that copy. We give no help finding one.

This is an alpha. We tested it on one setup: Linux, an NVIDIA RTX 4070, a Quest 3 through WiVRn. The Windows download
we have only tried under Wine; real Windows is up to players, and the picture on Meta's PC runtime (Link, Air Link, Rift)
is not confirmed yet. Anything else may work, or not: tell us how it went. See
[Known issues](vr-known-issues.md).

## What you need

- **A PC VR headset with an OpenXR runtime.** Any runtime that speaks OpenXR: [WiVRn](https://github.com/WiVRn/WiVRn)
  (a Quest or Pico, wireless, on Linux), SteamVR, [Monado](https://monado.freedesktop.org/), or Meta's (Quest Link,
  Air Link). Virtual Desktop and others should work too. Quest-style (Touch) controllers are the ones we tested.
- **Your own copy of the game**, the Nintendo Switch 2 edition at the version Hookshot supports: 1.1.1 (program
  `0400486024DEC000`, update v131072, build `e0f3ccfa`). It is the same file Hookshot builds from
  ([Hookshot's requirements](hookshot-readme.md#what-you-need)). Other versions do not work.
- **A graphics card with Vulkan 1.3 and mesh shaders**: NVIDIA RTX 20 series or newer, AMD RX 6000 or newer, Intel Arc.
  Only an NVIDIA RTX 4070 is tested. 8 GB of graphics memory or more is better, and VR wants more than a flat screen.
- **Resizable BAR turned on** in your PC's UEFI/BIOS (AMD calls it Smart Access Memory). It is recommended, and VR can
  run out of graphics memory without it. See [Troubleshooting](vr-troubleshooting.md#white-textures-then-a-crash).
- A CPU with AVX2, 16 GB of RAM, and **about 60 GB of free space** while it builds (an SSD is much faster).
- On Linux: the OpenXR loader (`libopenxr_loader.so.1`; the package is `libopenxr-loader1` on Debian and Ubuntu,
  `openxr` on Fedora and Arch). Your VR runtime usually installs it.
- On Windows: 64-bit Windows 10 or 11.

## 1. Download

Get the file for your system from this release's page on GitHub:

| System | File |
| --- | --- |
| Windows 10 / 11 | `HookshotVR-Alpha-v1.0.2-windows-x64.zip` |
| Linux | `HookshotVR-Alpha-v1.0.2-linux-x86_64.tar.gz` |

It is one download: the launcher, HookshotVR, and Hookshot's builder, so there is nothing else to get.

- **Windows:** unzip it anywhere you like (not into a folder that needs administrator rights), for example
  `C:\Games\HookshotVR`, and start the launcher in it.
- **Linux:** unpack the tar (`tar xzf HookshotVR-Alpha-v1.0.2-linux-x86_64.tar.gz`) and start the launcher. The first time it starts, it asks
  "Add to menu and desktop?": say yes for a menu entry and a desktop icon that open the launcher with no terminal. You
  can also do it later from a terminal with `--install` (`hookshotvr-launcher --install`), and again if you move the folder.

The launcher's window is the only thing you use. Nothing starts a VR program for you.

## 2. First run: build the game

The first time, the launcher builds the game from your copy.

1. Under **Game file**, press **Choose** and pick your copy of the game. The launcher checks it and says whether it is
   the supported version.
2. **Free space** should be green. If you need to, change **Hookshot Folder** under **Settings** (it is where the game
   is built).
3. Press **Build**. The steps are **Check**, **Translate**, **Compile** and **Shaders**. The first build takes a
   while, from half an hour to a couple of hours depending on your PC (**Shaders** is the longest). You can use your
   PC meanwhile. **Cancel** stops it, and you can start again.

You do this once. Next time the launcher opens ready to play.

## 3. Pick your VR runtime and play

1. Open **Settings** and look at **VR Runtime**.
   - **System Default** is what you want almost always: the runtime your PC already uses for VR (the one your VR app
     set as active).
   - **WiVRn**, **SteamVR** and **Monado** pick one by name, if you have several. **Custom** takes a runtime's OpenXR
     `.json` file.
2. **Start your VR runtime yourself** (SteamVR, the WiVRn server, Quest Link...), then put the headset on and connect it.
   The launcher never starts one on its own. If you want it to, set **Start With** in **Settings** to your VR app, and
   **Play** will start it first.
3. Choose the **HookshotVR** card. The rows show your runtime (**Running**) and **Headset** (**Connected**).
   (The **Hookshot** card plays flat on your monitor, with no VR.)
4. In **Settings**, choose your **View** (**First person**, **Third person** or **Window**), your **3D** (**Mono** or
   **Stereo**) and your **Menu button** (**Menu or Y** or **Both sticks**). They are saved in the game, so they are
   already set when you start, and the VR menu changes them too. The Play screen says how to open the VR menu.
5. Press **Play**. The launcher checks your runtime and headset, but the checks are advice: **Play** always works. If it
   does not see the headset, it asks "Headset not found. Play anyway?" (or "VR runtime not running. Play anyway?") and
   you choose **Play** or **Cancel**. It can be wrong, for example when the headset is asleep, or when two WiVRn servers
   run (the runtime row says "2 running"). **SteamVR**'s headset row reads "Not checked": the launcher does not start
   SteamVR just to look.

**On Game Start** says whether the launcher closes or stays open while you play.

### The first start is slow

The first time, and each new area the first time you enter it, the game prepares shaders, so it can freeze for a
moment and look blurry or stutter. Later visits are smooth. See
[Known issues](vr-known-issues.md#stutter-when-you-enter-an-area).

## Playing

**Hold the left Menu button, or Y** (on Index, the left **B**), for half a second to open the VR menu, and again to
close it. On the keyboard, **F2** does it too, with the game window active. The Play screen says it: "VR menu: hold left
menu or Y, or F2". The menu names your controller's own buttons (**Body** › **Controls**).

- **Menu button** (**Body** › **Controls** in the VR menu, and **Settings** in the launcher) is **Menu or Y** (the
  default) or **Both sticks** (click both sticks, or trackpads, together). The left Menu button works either way.
- **Vive wands and older Windows Mixed Reality controllers have no Y:** use Menu, **Both sticks** or **F2**.
- **SteamVR can keep the Menu button** for its own dashboard: use **Y**, or see
  [SteamVR's menu button](vr-troubleshooting.md#steamvr-opens-its-dashboard-instead-of-the-vr-menu).
- **Steam Frame:** no layout of its own yet. Valve says a Touch controller's Menu becomes View and Y becomes the top
  buttons (we have not tried it).

Point at the menu with either controller. Under **General**, **Play Mode**:

- **View** chooses how you see the game: **First person** (the default), **Third person** or **Window Beta** (below).
  It is also in the launcher, before you play.
- **Recenter view** makes where you look now straight ahead.
- **3D** is **Mono** (the default: both eyes see the same picture; no depth, the lightest on your PC) or **Stereo** (the
  game draws each eye: real 3D, but slower and with [problems](vr-known-issues.md#stereo-white-or-stuck-materials)).
  The launcher's **Settings** has it too.

The **Comfort** tab has snap turning (**Turn style**, **Snap angle**) and vignettes (**Comfort Aids**: a darker edge
while you move, turn, fly or carry something), for when the game makes you feel unwell.

### The views

- **First person** (**View: First person**, the default): you are Link. You see his body and hands, your hands are his
  hands, and every item is used by hand.
- **Third person** (**View: Third person**): the game's usual camera behind Link, in the headset. Aiming the
  slingshot, bow, boomerang or hookshot shows a reticle where the shot will go, from your hand's aim.
- **Window** (beta, **View: Window Beta**): third person seen through a frame hanging fixed in your room, like a
  window onto the game. Its rows (under **Window** in the menu) move it, resize it and round its corners. It is a beta:
  it may change.

### Controls

| What | How |
| --- | --- |
| **VR menu** | Hold the left Menu button or **Y** (Index: left **B**) for half a second, or press **F2** on the keyboard. SteamVR may keep the Menu button: use Y |
| **Radial wheel**, items | Click the left stick |
| **Radial wheel**, equipment | Click the right stick |
| **Walk** | Left stick |
| **Turn** | Right stick, left or right |
| **Jump** | Right stick, up |
| **Health, magic, rupees and keys** | Hold a wrist up like a watch (the back of either wrist). It does not show while you fight |
| **Sword and shield** | Put the sword hand over your back and press the grip to draw. Swing to slash. Press the sword trigger to spin quickly, hold it to charge a spin attack. Let go of the grip to sheathe. Press the shield trigger to lock on and guard |
| **Items** | Hold the grip to hold an item, let go to put it down, trigger for its action |
| **Deku Stick** | Put it in a torch to light it. Swing to hit |
| **Bow and Fairy Slingshot** | Hold the grip in the bow hand. Pull the string back with the other hand (grip or trigger). Let go to fire |
| **Bombs** | Hold the grip to take one out. Let go to drop it, or let go while your hand moves to throw. Grip near a bomb or a Bomb Flower to pick it up. Press the trigger while holding to start the fuse |
| **Boomerang and Hookshot** | Hold the grip to hold it, hold the trigger to aim, let go of the trigger to fire |
| **Bottles** | Hold the grip to hold it. Trigger on an empty bottle to catch something. Trigger on a corked bottle to use what is in it |

Not done yet: **Deku Nuts, Bombchus, Magic Beans, Megaton Hammer, Din's Fire, Farore's Wind, Nayru's Love, the
Business Scrub's seed and the trade items**. Equip them in the menu and press the trigger to use them, the game's own way.
They work, but not by hand yet.

## Updates

From v1.0.1 on, the launcher updates itself from this project's GitHub releases. When a new one is out, the main screen
shows **Update** with its version and what it takes:

- **Launcher only:** a small download (a few hundred MB). The launcher is replaced and starts again; your game is kept as
  it is, nothing is built.
- **Game rebuild:** the game changed too. After the download the launcher starts again and builds the game into a second
  folder beside the one you play (about 60 GB free while it builds), then switches to it and deletes the old one. The
  first start after a rebuild prepares shaders again, so it stutters like your first start did.

**Update Log** shows what is new, and **View Online** opens the release on GitHub. **Settings**, **Updates**: **Notify**
(the default) asks first, **Automatic** downloads in the background so **Update** only installs, **Off** never looks.
The launcher looks once each time it starts, and says nothing when it is offline. Your saves, settings,
`hookshot_env.txt` and crash reports are kept.

### Upgrading from v1.0.0 or v1.0.1

v1.0.2 changes the game too, so this update **rebuilds the game** (about 60 GB free while it does, and the shaders are
prepared again on the first start).

- **From v1.0.1:** the launcher updates itself: press **Update** when it shows. This is the first real use of the
  updater, so if it fails, download v1.0.2 and unpack it over your old folder, as below.
- **From v1.0.0:** v1.0.0 cannot update itself, so download this version once by hand. Close the launcher, unpack the
  download over your old folder and choose **Replace all** (or **Overwrite**), then start the launcher. Your saves,
  settings are kept, but your game is still v1.0.0's until you press **Rebuild v1.0.2** in the launcher: do that before
  you play. Or unpack into a new folder and build there.

## Saves and settings

Your saves are Hookshot's: on Linux in `~/.local/share/hookshot`, on Windows in `%APPDATA%\Hookshot`.
The launcher's own settings are in `launcher.ini` (`~/.config/HookshotVR` on Linux, `%LOCALAPPDATA%\HookshotVR` on
Windows).

## Staying safe

Clear your play area. Take a break every half hour or so, and stop if you feel sick: the comfort options help.
The game has flashing lights in places.

## If something goes wrong

See [Troubleshooting](vr-troubleshooting.md) and [Known issues](vr-known-issues.md). Licences and thanks: the
[NOTICE](../NOTICE) file.
