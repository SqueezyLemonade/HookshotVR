# HookshotVR Alpha v1.0.0: known issues

Problems we know about. If you see one of these, you don't need to report it. If you have something new, or more to add
(where it happens, your hardware), please do: see [how to send a log](vr-troubleshooting.md#how-to-send-a-log).

## White or stuck materials in True stereo

**What you see:** some materials, most often leaves and grass, stay white, or stay stuck on the wrong picture, until the
game has drawn them a few times.

**When:** with **3D: True stereo** (VR menu, **General**). **From depth**, the default, was clean in our tests. One run
may have shown it there too, so if you see white materials in From depth, please tell us.

**What to do:** stay on **3D: From depth**. We are still working on True stereo.

## The Window view is a beta

The Window view (**View: Window Beta**: third person seen through a frame in your room) works, but it is new. Its rows, their ranges and
how it behaves can change, and you may find rough edges: tell us what you find.

## Some items are not done in VR yet

These use the game's own buttons for now (equip them in the menu and press the trigger), not your hands:
**Deku Nuts, Bombchus, Magic Beans, Megaton Hammer, Din's Fire, Farore's Wind, Nayru's Love, the Business Scrub's seed
and the trade-quest items.**

## Windows is only tested under Wine

The Windows download has only been tried under Wine on Linux. Nobody has played it on real Windows with a headset yet.
It may need fixes. Tell us what happens on yours.

## Only Quest controllers are tested

Controls are mapped for Quest-style (Touch) controllers. Other controllers (Index, Vive, Pico, WMR and others) have
suggested bindings but are untested: some buttons may be missing or in odd places.

## Not tested yet

These may work, but nobody has tried them:

- AMD and Intel graphics cards, and cards with 8 GB of graphics memory or less.
- Headsets other than a Quest 3, and runtimes other than WiVRn.
- Refresh rates other than the tester's, seated play, left-handed play.
- The whole game. This release aims at the child years; the rest has not been played through in VR.

## Fog in the graveyard, in True stereo

**What you see:** in the graveyard, the fog can turn into grey, flat shapes with hard edges. It depends on which way you
look.

**When:** only with **3D: True stereo**. **From depth** draws the fog right.

## Navi's double shadow, in True stereo

**What you see:** Navi has two dark shadows on the ground, one on each side of her. Link, enemies and small objects such
as pebbles can have two shadows too.

**When:** only with **3D: True stereo**. **From depth** shows one shadow.

## Stutter when you enter an area

**What you see:** when you enter a new area (Hyrule Field, a village, a dungeon), the game can freeze for a few seconds,
and some textures show up late, blurry at first. The first start is the slowest.

**Why:** the game prepares the shaders for what it sees for the first time, and the picture waits for them.

**What to do:** wait a moment. The next time you enter the same area, it is much smoother.

## No update button yet

There is no updater in this release. To get a new version, download it from the release page and build again.
