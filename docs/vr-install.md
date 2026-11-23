# HookshotVR: install and play

**HookshotVR Alpha v1.0.0.** Unofficial. Not affiliated with Nintendo or the Hookshot project. Bring your own legally
obtained copy of the game.

HookshotVR plays *Ocarina of Time* in a VR headset, on your PC, in first or third person. It is a VR fork of
[Hookshot](hookshot-readme.md). It contains no game files: a launcher builds the game on your PC from your own copy, and
never changes that copy. We give no help finding one.

This is an alpha. It was tested on one setup: Linux, an NVIDIA RTX 4070, a Quest 3 through WiVRn. The Windows download
has only been tried under Wine so far. Anything else may work, or not: tell us how it went. See
[Known issues](vr-known-issues.md).

## What you need

- **A PC VR headset with an OpenXR runtime.** Any runtime that speaks OpenXR: [WiVRn](https://github.com/WiVRn/WiVRn)
  (a Quest or Pico, wireless, on Linux), SteamVR, [Monado](https://monado.freedesktop.org/), or Meta's (Quest Link,
  Air Link). Virtual Desktop and others should work too. Quest-style (Touch) controllers are the ones we tested.
- **Your own copy of the game**, the Nintendo Switch 2 edition at the version Hookshot supports (the same file
  Hookshot builds from: [Hookshot's requirements](hookshot-readme.md#what-you-need)). Other versions do not work.
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
| Windows 10 / 11 | `HookshotVR-Alpha-v1.0.0-windows-x64.zip` |
| Linux | `HookshotVR-Alpha-v1.0.0-linux-x86_64.tar.gz` |

It is one download: the launcher, HookshotVR, and Hookshot's builder, so there is nothing else to get.

- **Windows:** unzip it anywhere you like (not into a folder that needs administrator rights), for example
  `C:\Games\HookshotVR`, and start the launcher in it.
- **Linux:** unpack the tar (`tar xzf HookshotVR-Alpha-v1.0.0-linux-x86_64.tar.gz`) and start the launcher. To get a menu entry and a desktop
  icon, run the launcher once from a terminal with `--install` (`hookshotvr-launcher --install`). The icon opens the
  launcher with no terminal. Run it again if you move the folder.

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
4. Press **Play**. While the runtime or the headset is not ready, the button says **Waiting**.

**On Game Start** says whether the launcher closes or stays open while you play.

### The first start is slow

The first time, and each new area the first time you enter it, the game prepares shaders, so it can freeze for a
moment and look blurry or stutter. Later visits are smooth. See
[Known issues](vr-known-issues.md#stutter-when-you-enter-an-area).

## Playing

**Hold the menu button on the left controller** to open the VR menu. Point at it with either controller. Under
**General**, **Play Mode**:

- **View** chooses how you see the game: **First person**, **Third person** or **Window Beta** (below).
- **Recenter view** makes where you look now straight ahead.
- **3D** is **From depth** (the default: each eye is made from one picture and its depth, lighter on the PC) or
  **True stereo** (the game draws both eyes; sharper, slower, with more
  [problems](vr-known-issues.md#white-or-stuck-materials-in-true-stereo)).

The **Comfort** tab has snap turning (**Turn style**, **Snap angle**) and vignettes (**Comfort Aids**: a darker edge
while you move, turn, fly or carry something), for when the game makes you feel unwell.

### The views

- **Third person** (**View: Third person**): the game's usual camera behind Link, in 3D in the headset. The easiest start.
  Aiming the slingshot, bow, boomerang or hookshot shows a reticle where the shot will go, from your hand's aim.
- **Window** (beta, **View: Window Beta**): third person seen through a frame hanging fixed in your room, like a
  window onto the game. Its rows (under **Window** in the menu) move it, resize it and round its corners. It is a beta:
  it may change.
- **First person** (**View: First person**): you are Link. You see his body and hands, your hands are his hands, and every
  item is used by hand.

### Controls

| What | How |
| --- | --- |
| **VR menu** | Hold the left controller's menu button |
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
