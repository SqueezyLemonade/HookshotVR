# HookshotVR: troubleshooting

Problems that are Hookshot's own, in the flat **Hookshot** card too (a failed build, the wrong game version, a driver
crash), are in [Hookshot's troubleshooting](hookshot-readme.md#if-something-goes-wrong). This page is for VR. Known problems
are in [Known issues](vr-known-issues.md).

## No VR runtime found

The launcher's rows say **VR runtime**: **None installed**, **Not set**, **Not found**, or your runtime's name with **Not
running**, and **Play** says **Waiting**.

- **None installed:** install a VR app that is an OpenXR runtime: SteamVR, WiVRn, Monado, Meta Quest Link or Virtual
  Desktop.
- **Not set:** make your runtime the **active OpenXR runtime** (each VR app has a setting for it), or pick it in the
  launcher: **Settings**, **VR Runtime** (**WiVRn**, **SteamVR**, **Monado**, or **Custom** with the runtime's `.json`
  file). On Linux the active runtime is the file `~/.config/openxr/1/active_runtime.json`; on Windows it is in the
  registry, and SteamVR and Meta's app both have a button to set it.
- **Not found:** the runtime you picked is not where the launcher looks. Choose **System Default** or **Custom**.
- **Not running:** start your VR app yourself. The launcher never starts one. To have **Play** start it for you, set
  **Start With** in **Settings**.
- **The log says there is no OpenXR loader** (on Linux): install your system's OpenXR loader package
  (`libopenxr-loader1` on Debian and Ubuntu, `openxr` on Fedora and Arch). The game then runs flat.

## The headset shows nothing

- Check the launcher: the **HookshotVR** card, not **Hookshot** (which is flat on your monitor), with **Headset**
  **Connected**. **Play** waits for it.
- **Waiting** or **Unknown** under **Headset**: put the headset on and connect it in your VR app (for a Quest: WiVRn,
  Quest Link or Virtual Desktop in the headset). Restart the VR app if it stays stuck.
- Start the game after the runtime and the headset are ready. A game started without them runs flat on your monitor,
  and the log says `vr: no headset ... VR off this run`.
- Only one VR app can hold the headset. Close other VR games and overlays.
- Black, then a crash, or white textures: see below.

## A controller is not detected

- Turn both controllers on and wake them before you press **Play**.
- Check that your VR app sees them.
- **Tested: Quest Touch controllers.** Others (Index, Vive, Pico, WMR) are untested: some buttons may be missing or
  elsewhere. Tell us which controller you have and what is wrong.
- If your hands sit in the wrong place, use **Calibrate wrists** (VR menu, **Body**).

## White textures, then a crash

**Resizable BAR** is recommended: turn it on. Without it, the CPU can reach only a small part of the graphics card's memory,
and VR needs more of it than a flat screen. The game runs out of graphics memory: textures turn white, then it crashes.

When this happens, the log says so in one line:

```text
vk: VR: Resizable BAR is off: ...
```

The crash window says "VR ran out of graphics memory: Resizable BAR is off".

To turn it on:

1. Restart the PC and open its UEFI/BIOS settings (often Del or F2 at the start).
2. Turn on **Above 4G Decoding** and **Resizable BAR**. On AMD boards it can be called **Smart Access Memory**.
3. Save, and start the game again.

To check it: near the start of the log, the `vk: policy: facts` line says `resizable BAR 1` when it is on and
`resizable BAR 0` when it is off.

If it is on and textures still go white, lower **Eye resolution** (VR menu, **Graphics**), or choose **3D: From depth**
(VR menu, **General**).

## The first start is slow and stutters

The first time, and each new area the first time you enter it, the game prepares its shaders. It can freeze for a few
seconds and show blurry textures, and the **Shaders** step of the first build is the longest of all. This is normal and
passes: later visits are smooth. See [Known issues](vr-known-issues.md#stutter-when-you-enter-an-area).

## The view is turned, too high or too low

- **Recenter view** (VR menu, **General**) makes where you look now straight ahead.
- **Eye height** (VR menu, **General**, **Headset**) moves the first-person view up or down.

## Which drive is used

Everything big is in the folder you unpacked HookshotVR into (the **Hookshot Folder**), on that drive: the game
(`main-game`, about 40 GB), the build's work folder (`main-work`, with the build's temporary files and the game file's
check), the shader caches on Linux (`cache`), and an update's download (`.update`). A game rebuild needs a second game
folder there while it builds: about 60 GB free.

Some things always stay on the system drive (C: on Windows, your home on Linux). They are small, except the graphics
driver's shader cache:

| What | Windows | Linux | Size |
| --- | --- | --- | --- |
| Saves, save backups and the game's settings | `%APPDATA%\Hookshot`, `%LOCALAPPDATA%\Hookshot` | `~/.local/share/hookshot` | tens of MB |
| The launcher's settings, logs and crash reports | `%LOCALAPPDATA%\HookshotVR` | `~/.config/HookshotVR`, `~/.local/share/HookshotVR` | a few MB, more with many build logs |
| The graphics driver's own shader cache | `%LOCALAPPDATA%\NVIDIA\GLCache`, `AMD\VkCache` or `Intel\ShaderCache` | in the Hookshot Folder (`cache`) | up to a few GB, the driver's own limit |

On Windows the driver's cache is the driver's: it fills while the first build prepares the shaders. NVIDIA's limit is
**Shader Cache Size** in the NVIDIA Control Panel. Free space is checked on the Hookshot Folder's drive only.

## How to send a log

A log helps us find what went wrong. Send it with a short description: what you did, where you were in the game, your
graphics card, your headset and your VR app, and your system (Windows or Linux).

**Where the logs are.** In the launcher, **Settings**, **Logs**, **Open** opens the folder. It is:

- Linux: `~/.local/share/HookshotVR/logs`
- Windows: `%LOCALAPPDATA%\HookshotVR\logs`

In it:

- `last-run.log` is the game's last run (and `last-run.1.log` the one before). **Send this for a problem in the game.**
- `build.log` is the last build. Send it for a problem building.
- `crashes/` has the crash reports. After a crash, the crash window also has **Report**, which copies the same kind of
  report (the error, your PC's hardware and the end of the log) to the clipboard.

**What is in them:** the logs have your home folder and your user name taken out, so they are safe to send as they are.
Please look at one first if you want to be sure. Never send game files, saves, or anything from the `romfs` folder.

Report problems with VR to HookshotVR (on its GitHub page, as an issue), not to Hookshot.
