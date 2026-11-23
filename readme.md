<p align="center">
  <img src="docs/images/hookshotvr-banner.png" alt="HookshotVR" width="640">
</p>

<h1 align="center">HookshotVR</h1>

<p align="center"><b>Play <i>Ocarina of Time</i> in your VR headset, on your PC. Alpha v1.0.1.</b></p>

<p align="center">
  <a href="#download">Download</a> |
  <a href="#quick-start">Quick start</a> |
  <a href="#what-you-need">What you need</a> |
  <a href="#the-views">Views</a> |
  <a href="#controls">Controls</a> |
  <a href="#not-done-yet">Not done yet</a> |
  <a href="#more-help">Help</a> |
  <a href="#credits">Credits</a> |
  <a href="#license">License</a>
</p>

> [!IMPORTANT]
> **Unofficial. Not affiliated with Nintendo or the Hookshot project. Bring your own legally obtained copy of the
> game.** HookshotVR contains no game files: a launcher builds the game on your PC from your own copy.

HookshotVR is a VR fork of [Hookshot](https://git.notrexed.dev/Hookshot/Hookshot), the static recompilation that runs the
game natively on a PC. It adds a headset view, hands, items you hold, a wrist HUD, and a launcher that sets it all up.
It works with any OpenXR runtime: WiVRn, SteamVR, Monado, Meta's and others.

This is an **alpha**: tested on one setup (Linux, an NVIDIA RTX 4070, a Quest 3 through WiVRn). Please try it and tell us
how it went.

## Download

Get the file for your system from the [latest release](../../releases/latest):

- **Windows 10 / 11:** [`HookshotVR-Alpha-v1.0.1-windows-x64.zip`](../../releases/download/v1.0.1/HookshotVR-Alpha-v1.0.1-windows-x64.zip)
- **Linux:** [`HookshotVR-Alpha-v1.0.1-linux-x86_64.tar.gz`](../../releases/download/v1.0.1/HookshotVR-Alpha-v1.0.1-linux-x86_64.tar.gz)

<!-- Placeholder: these links are relative to this repository, so they work wherever it lives. -->

One download holds everything: the launcher, HookshotVR, and Hookshot's builder.

## Quick start

1. Unpack the download anywhere (on Linux a folder in your home, on Windows one that does not need administrator rights)
   and start the launcher.
2. Press **Choose** under **Game file** and pick your copy of the game, then press **Build**. The first build takes a
   while, from half an hour to a couple of hours. You do it once.
3. Start your VR runtime yourself (SteamVR, the WiVRn server, Quest Link...) and put the headset on. The launcher never
   starts one for you. Use **Settings**, **VR Runtime** if your PC has more than one.
4. Choose the **HookshotVR** card and press **Play**. It says **Waiting** until the runtime and the headset are ready.
5. In the game, hold the left controller's menu button for the VR menu.

The first start, and each new area, stutters a little while shaders prepare. Later visits are smooth.

The whole guide: [Install and play](docs/vr-install.md).

## What you need

- **A PC VR headset with an OpenXR runtime:** WiVRn, SteamVR, Monado, Meta Quest Link and others. Quest-style (Touch)
  controllers are the ones we tested.
- **Your own copy of the game**, at the version Hookshot supports
  ([Hookshot's requirements](docs/hookshot-readme.md#what-you-need)).
- **A graphics card with Vulkan 1.3 and mesh shaders:** NVIDIA RTX 20 series or newer, AMD RX 6000 or newer, Intel Arc.
  Resizable BAR turned on is recommended; VR can run out of graphics memory without it.
- A CPU with AVX2, 16 GB of RAM, and about 60 GB of free space while it builds.
- Linux: the OpenXR loader (`libopenxr-loader1` on Debian and Ubuntu, `openxr` on Fedora and Arch). Windows: 64-bit
  Windows 10 or 11.

## The views

- **Third person:** the game's usual camera behind Link, in 3D in your headset. The easiest start. Aiming the slingshot,
  bow, boomerang or hookshot shows a reticle where the shot will go.
- **Window** (beta): third person seen through a frame hanging fixed in your room, like a window onto the game.
- **First person:** you are Link. You see his body and hands, and use every item by hand.

Choose with **View** in the VR menu (**General**).

## Controls

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

## Not done yet

These work through the game's own buttons (equip them in the menu and press the trigger), not by hand yet:
**Deku Nuts, Bombchus, Magic Beans, Megaton Hammer, Din's Fire, Farore's Wind, Nayru's Love, the Business Scrub's seed
and the trade items.**

Also in this alpha: True stereo can show white or stuck materials (**From depth**, the default, was clean); the Window
view is a beta; Windows has only been tried under Wine; controllers other than Quest Touch are untested.

## More help

- [Install and play](docs/vr-install.md): the full guide
- [Troubleshooting](docs/vr-troubleshooting.md): no runtime found, a black headset, controllers, Resizable BAR, and how
  to send a log
- [Known issues](docs/vr-known-issues.md)
- [NOTICE](NOTICE): what this fork changed, and the third-party licenses
- [Hookshot's own README](docs/hookshot-readme.md): its features, and building from source
- [Contributing](CONTRIBUTING.md) and [START HERE](START%20HERE.md), if you want to help

## Credits

HookshotVR is built on [Hookshot](https://git.notrexed.dev/Hookshot/Hookshot) by NotRexed and its many contributors. All
of it, the recompilation, the renderer and the builder, is theirs, and we are grateful. See
[Hookshot's credits](docs/hookshot-readme.md#credits).

Thanks also to [OcarinaVR](https://github.com/ReTracedGames/OcarinaVR) by ReTracedGames for ideas we built on, to BetterVR,
to Khronos for OpenXR, and to the makers of WiVRn, Monado and SteamVR. The launcher uses EB Garamond.

## License

HookshotVR is free software under the [GNU General Public License, version 3 or later](LICENSE), like Hookshot.
[NOTICE](NOTICE) says what changed and lists the third-party parts with their licenses.

"The Legend of Zelda" and "Ocarina of Time" are trademarks of Nintendo. This is an unofficial fan project, not affiliated
with or endorsed by Nintendo.
