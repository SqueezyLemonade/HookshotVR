# HookshotVR Alpha v1.0.2: known issues

Problems we know about. If you see one of these, you don't need to report it. If you have something new, or more to add
(where it happens, your hardware), please do: see [how to send a log](vr-troubleshooting.md#how-to-send-a-log).

## Meta's PC runtime: the picture is not confirmed

**What you see:** on Meta's PC runtime (Quest Link, Air Link, Rift S and CV1), some players saw a doubled or wrong
picture, flat and far away, including on the title screen and the menus.

**Where we are:** v1.0.2 **changes** how the title, the menus, the movies and the game are sent to the headset. We could
not test it on Meta's runtime, so we do not know yet whether it helps. Please [help us find out](vr-test-meta.md): it
takes about 20 minutes and a few logs. One player reported SteamVR working on the same PC, so SteamVR is a thing to try.

**Switches** for `hookshot_env.txt` (one line each, then start the game again): `RX_VR_FLAT_LAYER=eyes` (the default),
`quad` or `projection`; `RX_VR_MONO_POSE=eyes` (the default) or `head`; `RX_VR_LAYER_FOV=located` (the default) or
`symmetric`.

## 3D is Mono or Stereo

**3D** (VR menu, **General**, or the launcher's **Settings**) is **Mono** by default: both eyes see the same picture, so
there is no depth, but it is the lightest. **Stereo** draws each eye, for real 3D, and it is the only 3D mode for now.
"From depth", an older third mode, is hidden while we rework it; a saved setting for it loads as Mono.

## Stereo: white or stuck materials

**What you see:** some materials, most often leaves and grass, stay white, or stay stuck on the wrong picture, until the
game has drawn them a few times.

**When:** with **3D: Stereo**.

**What to do:** stay on **3D: Mono** if it bothers you. We are still working on Stereo.

## The Window view is a beta

The Window view (**View: Window Beta**: third person seen through a frame in your room) works, but it is new. Its rows, their ranges and
how it behaves can change, and you may find rough edges: tell us what you find.

## Some items are not done in VR yet

These use the game's own buttons for now (equip them in the menu and press the trigger), not your hands:
**Deku Nuts, Bombchus, Magic Beans, Megaton Hammer, Din's Fire, Farore's Wind, Nayru's Love, the Business Scrub's seed
and the trade-quest items.**

## Windows: tested by players, not by us

We build and test on Linux (a Quest 3 through WiVRn), and the Windows download has only been tried under Wine here.
Players have run it on real Windows with headsets (Rift S and CV1 among them), but we have not. It may need more fixes.
Tell us what happens on yours.

## Controllers other than Quest Touch are untested

Controls are mapped for Quest-style (Touch) controllers. Other controllers (Index, Vive, Pico, WMR and others) have
suggested bindings but are untested: some buttons may be missing or in odd places. The VR menu opens with a half-second
hold of the left Menu button or of **Y** (Index: the left **B**), or with **F2** on the keyboard. Vive wands and older
Windows Mixed Reality controllers have no Y: set **Menu button** to **Both sticks** (VR menu, **Body** › **Controls**, or
the launcher's **Settings**). SteamVR can keep the Menu button for its own dashboard: use Y.

## Steam Frame controller layout: not yet

The Steam Frame's controllers have no layout of their own yet. Valve says a Touch controller's Menu becomes View and Y
becomes the top buttons; we have not tried it. Tell us how it goes, and see
[SteamVR's menu button](vr-troubleshooting.md#steamvr-opens-its-dashboard-instead-of-the-vr-menu).

## Not tested yet

These may work, but we have not tried them:

- AMD and Intel graphics cards, and cards with 8 GB of graphics memory or less.
- Headsets other than a Quest 3, and runtimes other than WiVRn (players have tried Rift S, Rift CV1 and SteamVR; we have not).
- Refresh rates other than the tester's, seated play, left-handed play.
- The whole game. This release aims at the child years; the rest has not been played through in VR.

## Stereo: fog in the graveyard

**What you see:** in the graveyard, the fog can turn into grey, flat shapes with hard edges. It depends on which way you
look.

**When:** with **3D: Stereo**. **Mono** draws the fog right.

## Stereo: Navi's double shadow

**What you see:** Navi has two dark shadows on the ground, one on each side of her. Link, enemies and small objects such
as pebbles can have two shadows too.

**When:** with **3D: Stereo**. **Mono** shows one shadow.

## Not in this version

A mirror or recording view of what the headset sees, full button remapping, and a Steam Frame controller layout. Use your
headset's own recording for now.

## Stutter when you enter an area

**What you see:** when you enter a new area (Hyrule Field, a village, a dungeon), the game can freeze for a few seconds,
and some textures show up late, blurry at first. The first start is the slowest.

**Why:** the game prepares the shaders for what it sees for the first time, and the picture waits for them.

**What to do:** wait a moment. The next time you enter the same area, it is much smoother.
