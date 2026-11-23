# HookshotVR Alpha v1.0.0

The first public release of HookshotVR: *Ocarina of Time* in your VR headset, on your PC, in first or third person.

> **Unofficial. Not affiliated with Nintendo or the Hookshot project. Bring your own legally obtained copy of the game.**
> HookshotVR contains no game files. A launcher builds the game on your PC from your own copy.

This is an **alpha**. It was tested on one setup (Linux, an NVIDIA RTX 4070, a Quest 3 through WiVRn). Please try it
and tell us how it went, good or bad.

## Download

| System | File |
| --- | --- |
| Windows 10 / 11 (64-bit) | `HookshotVR-Alpha-v1.0.0-windows-x64.zip` |
| Linux (64-bit) | `HookshotVR-Alpha-v1.0.0-linux-x86_64.tar.gz` |

One download holds everything: the launcher, HookshotVR, and Hookshot's builder.

## Quick start

1. Download the file for your system and unpack it.
2. Start the launcher. Press **Choose** under **Game file** and pick your copy of the game, then press **Build**. This
   takes a while (up to a couple of hours), once.
3. Start your VR runtime (SteamVR, WiVRn, Meta Quest Link, Monado, ...) and put the headset on. The launcher never starts
   one by itself.
4. Pick the **HookshotVR** card and press **Play**.

The full guide, with the controls: [Install and play](docs/vr-install.md).

## What you need

- A PC VR headset with an OpenXR runtime: WiVRn, SteamVR, Monado, Meta's, others.
- Your own copy of the game, at the version Hookshot supports.
- A graphics card with Vulkan 1.3 and mesh shaders (NVIDIA RTX 20 series, AMD RX 6000 or newer, Intel Arc). Resizable
  BAR turned on is recommended.
- 16 GB of RAM and about 60 GB of free space while building.

## What is in it

- **Sword combat with both hands:** draw from your back, swing to slash, spin and charge, the shield, Deku Sticks.
- **Items in your hands:** bombs and Bomb Flowers, bottles, the bow, the Fairy Slingshot, the Boomerang and the Hookshot.
- **The wrist HUD and the radial wheel:** look at your wrist for hearts, magic, rupees and keys; click a stick for the
  item or equipment wheel.
- **Third-person aim:** a reticle where your shot will go, from your hand.
- **Item gets and cutscenes in VR:** items shown in front of you, and cutscenes you watch from the camera in the 3D world.
- **Three views:** first person, third person, and the Window view (beta), a frame hanging in your room.
- **Comfort options:** snap turning and vignettes.
- **The HookshotVR Launcher:** builds and starts the game, works with any OpenXR runtime.

## Known issues

- **True stereo** can show white or stuck materials (leaves, grass). **From depth**, the default, was clean in our tests.
  Please also tell us if you see it there.
- The **Window view** is a beta.
- Not done in VR yet (use the game's own trigger): Deku Nuts, Bombchus, Magic Beans, Megaton Hammer, Din's Fire,
  Farore's Wind, Nayru's Love, the Business Scrub's seed and the trade items.
- **Windows** has only been tried under Wine so far.
- **Controllers other than Quest Touch** are untested.
- The first start, and each new area, stutters while shaders prepare.

All of them, and fixes: [Known issues](docs/vr-known-issues.md). If something is wrong, see
[Troubleshooting](docs/vr-troubleshooting.md), and how to report a bug with a log (it has no personal information in it).

## Licence and thanks

HookshotVR is free software under the GPL-3.0, a VR fork of [Hookshot](docs/hookshot-readme.md). [NOTICE](NOTICE) says what changed and
lists the third-party parts (EB Garamond under the SIL Open Font License, the OpenXR loader under Apache-2.0, and others).

Thanks to NotRexed and everyone behind Hookshot, to OcarinaVR by ReTracedGames for ideas we built on, to BetterVR, and to
the makers of WiVRn, Monado and SteamVR.
