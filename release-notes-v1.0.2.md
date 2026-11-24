# HookshotVR Alpha v1.0.2

An easier start (First person by default, more ways to open the VR menu), a change to how the picture reaches
your headset, and much better logs. Thank you to everyone who sent a report and a log.

**VR menu:** hold the left Menu button (or Y) for half a second. View › First person / Third person / Window.

## Meta's PC runtime: help us test

Some players on **Meta's PC runtime** (Quest Link, Air Link, Rift S and CV1) saw a doubled or wrong picture, flat and far
away, including on the title screen and the menus. **We have not confirmed a fix.** v1.0.2 changes how the title, the
menus, the movies and the game are sent to the headset, and we could not test it on Meta's runtime. If you have one, we
would be glad of 20 minutes and your logs: [Testing on Meta's runtime](docs/vr-test-meta.md). One player reported SteamVR
working on the same PC, which is a thing to try, not a promise.

## Fixed

- **Stereo below 100% Render resolution:** both eyes had one picture, so the 3D looked flat or the eye distance looked
  wrong. We proved it in our tests, and it was seen on a Rift CV1. It is fixed. (It is not a fix for Meta's runtime.)
- **Play is never blocked by "Waiting".** If the launcher does not see your headset, it asks "Headset not found. Play
  anyway?" and you can choose **Play**. With two WiVRn servers, the launcher could wait on the one without your headset
  for good: it now says "2 running".
- **SteamVR's headset row** reads "Not checked" instead of looking like a failure. The launcher does not start SteamVR
  just to look.
- **No silent exit:** the launcher says why when it cannot open a window.

## New

- **3D is Mono or Stereo.** **Mono** (the default) shows both eyes one picture: no depth, the lightest on your PC.
  **Stereo** draws each eye. The older **From depth** mode is hidden for now and will come back; a saved setting for it
  loads as Mono.
- **First person is the default View** for new players. **View**, **3D** and **Menu button** are in the launcher's
  **Settings**, so you can set them before you play, with no controller.
- **More ways to open the VR menu:** hold the left Menu button or **Y** (Index: the left **B**) for half a second,
  or press **F2** on the keyboard. Vive wands and older Windows Mixed Reality controllers have no Y: set **Menu button**
  to **Both sticks**. SteamVR can keep the Menu button for its own dashboard: use Y. The menu names your controller's own
  buttons, and the Play screen says how to open it.
- **Each eye's own pose and field of view** go to the runtime. Switches for `hookshot_env.txt` if you want to compare:
  `RX_VR_FLAT_LAYER=eyes` (the default), `quad` or `projection`; `RX_VR_MONO_POSE=eyes` (the default) or `head`;
  `RX_VR_LAYER_FOV=located` (the default) or `symmetric`.
- **Much better logs.** Six runs are kept (`last-run.log`, `last-run.1.log` to `last-run.5.log`) and they say what a bug
  report needs. In the launcher: **Settings** › **Logs** › **Open**. They are safe to send. The bug report form asks for
  them, and for your headset, runtime, 3D mode, View and Render resolution.
- **Add to menu and desktop?** On Linux, the launcher offers it the first time it starts.

## Upgrading

v1.0.2 changes the game, so this update **rebuilds the game** (about 60 GB free while it does), and the first start
prepares shaders again, so it stutters at first.

- **From v1.0.1:** the launcher updates itself: press **Update**. This is the updater's first real use. If it fails,
  download v1.0.2 and unpack it over your old folder, as below.
- **From v1.0.0:** download v1.0.2 once by hand. Close the launcher, unpack the download over your old folder, choose
  **Replace all**, and start the launcher. Your saves and settings are kept. Your game is still v1.0.0's until you press
  **Rebuild v1.0.2** in the launcher, so do that before you play.

## Download

| System | File |
| --- | --- |
| Windows 10 / 11 (64-bit) | `HookshotVR-Alpha-v1.0.2-windows-x64.zip` |
| Linux (64-bit) | `HookshotVR-Alpha-v1.0.2-linux-x86_64.tar.gz` |

`SHA256SUMS.txt` has the checksums and `GAME_STAMPS.txt` the game stamps (the launcher reads both). The game you need
is version 1.1.1 (program `0400486024DEC000`, update v131072, build `e0f3ccfa`). More in the
[install guide](docs/vr-install.md).

## Known issues

- **Meta's PC runtime is not confirmed** (above).
- **Stereo** can show white or stuck materials, fog in the graveyard turning grey and flat, and a double shadow under
  Navi. It is the only 3D mode with depth for now.
- **We tested on a Quest 3 through WiVRn on Linux.** Windows only under Wine; players report on real Windows.
  Controllers other than Quest Touch are untested, and the Steam Frame has no layout of its own yet.
- **Not in this version:** a mirror or recording view, full button remapping.
- The Window view is a beta.

The whole list is in [known issues](docs/vr-known-issues.md).
