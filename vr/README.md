# Hookshot VR: the map

For players: [install](../docs/vr-install.md), [troubleshooting](../docs/vr-troubleshooting.md) and
[known issues](../docs/vr-known-issues.md).

Start with [START HERE.md](../START%20HERE.md) for an editing guide, and [CONTRIBUTING.md](../CONTRIBUTING.md) for the
rules.

VR-specific systems live here. Integration points in Hookshot's files are marked `VR HOOK`; some renderer changes and
engine fixes still live upstream-side. The integration audit is not finished, so a marker search is not yet a complete diff.

## How the mod works, in one minute

Hookshot runs the Switch 2 *Ocarina of Time* on a PC. It has two halves, and the VR code has a part in each:

- **The game side** (`*_host.c` files). This code runs alongside the game's own code. It reads the game's memory and steers
  the game: it turns the camera with your head, reads Link's equipment, steers arrows and makes the shield face where you look.
  The game calls into it at **game hooks**: places in the game's code listed in `vr_game_hooks.py`.
- **The renderer side** (`.cpp` and `.inc` files). This code runs in Hookshot's Vulkan renderer. It draws the picture for the
  headset, poses Link's first-person body and the items in your hands, and draws both eyes for 3D.

`*_host.c` and `.inc` files are pieces of a larger Hookshot file (`host/hos_main.c`, `gpu/vk/vk_draw.cpp`, and others).
They are included there in a fixed order and are not compiled on their own. `.cpp` files are compiled on their own.

## Folders

| Folder | What's in it |
|---|---|
| `headset/` | `vk_vr.cpp` is the entry point/index. Named sections cover the runtime, GPU resources, head poses, eye rendering, frame loop/capture, markers and host bridge; `shaders/` contains depth conversion and reprojection |
| `camera/` | The game camera driven by your head (`view_host.c`), field of view and detail/foliage distance (`detail_host.c`), crawl spaces (`crawl_host.c`), culling around the eye (`culling_host.c`) |
| `stereo/` | 3D: Stereo (both eyes drawn every frame; Mono, the default, sends one picture to both eyes) |
| `body/` | `vk_vr_body.cpp` is the single rig entry point. Its ordered sections contain the pose, cache and skin replacement. `tracking.inc` converts headset/controller poses into game-world body inputs; it is included by the headset entry point |
| `items/bow/` | The bow: controls, the game patch that steers the arrow, and the hidden flat reticle |
| `items/slingshot/` | The Fairy Slingshot: the bow's controls and shot in another shape, held at the game's grip, aimed from the pouch through the fork |
| `items/gear/` | Link's equipment: what he owns and has drawn, keeping held gear visible near the eye |
| `items/shield/` | The shield: the guard faces where your body faces |
| `items/deku_stick/` | Deku Sticks in the VR hands: the game's own stick actor moved to the hand (`deku_stick_host.c`), its model drawn there (`held_stick.inc`); controls in `input/controllers_host.c` `vr_stick` |
| `items/bomb/` | Bombs in the VR hands: taken out with a grip, unlit (and quiet: their XLink asleep) until the same hand's trigger, dropped, thrown, picked up, caught and pulled from afar by hand, Bomb Flowers too (`bomb_host.c`); the throw arc and pull arithmetic (`bomb_math.h`); the arc and its target in the headset (`bomb_marks.inc`, parked with Arc to target) |
| `items/boomerang/` | Child Link's Boomerang in the VR hands: the game's own throw and flight, aimed by the holding hand, the empty hand or the headset (`boomerang_host.c`, the game hook in `vr_game_hooks.py`), its model in the hand (`held_boomerang.inc`), its reticle (`boomerang_reticle.inc`) |
| `items/ocarina/` | The ocarina: drawn at the front left of the VR belt and in the VR hand that takes it (`held_ocarina.inc`); held to the mouth it starts the game's own ocarina mode, its notes and song menu are the game's buttons, letting go or taking it away stops it (`ocarina_host.c`); the two messages between them (`ocarina_bridge.inc`, `ocarina_bridges_host.c`); the Song effects row (`song_effects.inc`) |
| `input/` | `openxr_controllers.inc` reads raw OpenXR input. `controllers_host.c` maps it to game buttons; the other files handle movement sticks and quick select |
| `menu/` | **The VR menu** (hold the left Menu button or Y half a second, or F2): its layout, controls and drawing (`vr_menu.inc`, in the Insert menu's file), the panel in the world and its pointers (`vr_menu_layer.inc`, `vr_menu_api.inc`); every setting (`vr_tab_rows.inc`: none on a tab of Hookshot's own menu; VR on or off is decided at the start, `HOOKSHOT_VR`), how its values show (`vr_values_menu.inc`) and their defaults and built-in values (`vr_options.inc`); the radial item wheel (`radial_*`) |
| `diagnostics/` | `cost_draw.inc`: the replay's CPU time per shader program (`RX_VR_PROG_COST=1`), a performance meter, kept while the frame-rate work goes on; the other research tools were retired as their work finished |
| `third_party/` | The OpenXR headers (Khronos, Apache-2.0 or MIT) |
| `tools/` | `build.sh` (build the game with the mod), `test.py` (the master tester) and `generate/` (below) |

## Data from your own game (`tools/generate/`)

A few of the mod's numbers come from the game's own files: Link's skeleton (which bones are the arms, hands and head),
how the Goron/Zora tunics and the gauntlets attach, where the sword and shield sit in his hands, the bow's rest pose
and the bow reticle's shape. Those files are Nintendo's, so neither they nor anything made from them is in this repository.

Instead, every build runs `tools/generate/` on **your** game files and writes the results into the build's work folder
(`<work>/vr_generated/`). One generator per file: `link_rig.py`, `tunics.py`, `gauntlets.py`, `hand_sockets.py`,
`bow_rest.py`, `bow_reticle.py`, `boomerang.py` (the Boomerang's draws and its hold), `bomb_flower.py` (a Bomb Flower fruit's draws and size), `ocarina.py` (the ocarinas' and their holders' draws), `worn_items.py` (what first person hides of what Link wears, and child Link's Goron Bracelet's draws and arm bones), `leg_gait.py` (Link's
walk, run, side and back steps for the first-person legs). They keep bone slots, matrices and mask values only, never meshes or textures. If a game file is
missing or not one they understand, the build stops with a message naming it. Never commit or share their output.

## I want to change...

| ...this | Open |
|---|---|
| A VR setting: its range, default or help text; where the VR menu shows it and under which name | `menu/vr_tab_rows.inc`, `menu/vr_options.inc` for the default; `menu/vr_menu.inc`'s table (`vrm_tabs`) for its place |
| Where the camera sits, your height, the horizon | `camera/view_host.c` |
| The view and body in a crawl space (how low, how fast, how the game marks a crawl) | `camera/crawl_host.c`; the body's crawl is `body/arms_and_pose.inc` `crawl_under_view` |
| Model or foliage detail at a distance | `camera/detail_host.c` |
| How the bow aims, draws or fires | `items/bow/bow_host.c` (game controls), `bow_pose.inc` (grip/string/pose), `bow_solution.inc` (shared aim) |
| Where the carried arrow sits | `items/bow/held_arrow.inc` |
| Where the slingshot sits, its pouch and straps | `items/slingshot/sling_rig.inc` (the grip), `held_slingshot.inc` (the pouch drawn); its fork and pouch feed `items/bow/bow_pose.inc` |
| Where the sword or shield sits in your hand | `items/gear/held_gear.inc`; fixed sockets come from the local generated header |
| Torso, arm reach and fingers | `body/arms_and_pose.inc`; `body/vk_vr_body.cpp` lists the sections in order |
| The first-person legs stepping sideways and backwards | `body/gait.inc` (blend and pace), `tools/generate/leg_gait.py` (which of the game's cycles) |
| Gauntlets or tunics | `items/gear/equipment_skin.inc` and `items/gear/tunics.inc` |
| Which worn items first person hides (belt items, boots, masks) | `tools/generate/worn_items.py`; the rule is `link_worn_hidden` in `items/gear/equipment_skin.inc` |
| The headset's flat screen or menu size | Named constants at the top of `headset/eye_rendering.inc` (degrees/metres) |
| Headset/controller tracking and wrist calibration | `headset/poses.inc`, `input/openxr_controllers.inc`, `body/tracking.inc` |
| Controller buttons | `input/controllers_host.c` |
| How bombs are held, thrown, caught, pulled or lit | `items/bomb/bomb_host.c` (the distances and speeds are named at the top) |
| A held bomb's or Bomb Flower's picture (size, in the palm, the pull's pulse) | `items/gear/equipment_skin.inc` bomb_drawn; the fruit's sizes: `tools/generate/bomb_flower.py` |
| The bombs' throw arc (parked: rows hidden) | `items/bomb/bomb_math.h` (where), `items/bomb/bomb_marks.inc` (how it looks) |
| How the Boomerang is held, aimed or thrown (the three throw styles) | `items/boomerang/boomerang_host.c` (the put-away is one function there: `vr_boomerang_put_away`) |
| Where the ocarina hangs on the belt, or how it sits in the hand | `items/ocarina/held_ocarina.inc` (`kOcarinaBeltAngle`; the hand's grip is the game's own, learned when Link first plays it) |
| How close to the mouth plays the ocarina, which control is which note, the song menu | `items/ocarina/ocarina_host.c` (the distances are named at the top) |
| 3D strength or Stereo | `stereo/vk_vr_eyes.cpp` |
| Where the game calls the mod | `vr_game_hooks.py` (changing it means the next build translates the game again, about 40 minutes) |

## Build and test

```bash
HOOKSHOT_BUILDER=<an unpacked Hookshot builder> vr/tools/build.sh <game folder>
```

```bash
python3 vr/tools/test.py
```

Add `--romfs <your game's romfs folder>` to include the checks that read your game files, and `--work <work folder>`
for the check of the translated game. Use the Hookshot builder's Python (`usr/python/bin/python3`) for those: it has
numpy and zstandard.

`test.py` compiles the real functions with small stand-ins for the game and checks what they must do. It needs no game,
headset or GPU, and it runs in under a minute. Run it after any change.

For an opt-in runtime check through the same tester:

```bash
python3 -I -B vr/tools/test.py --headless-game <built-game-folder> --save-root <your-save-folder> --seconds 150
```

It copies the supplied save to a temporary folder, uses that copy, and retains both-eye pictures and a log there for
inspection. It requires an installed Vulkan driver and an already built game. It does not verify headset submission,
physical controls or first-person equipment. Cold caches can require a longer run. Never publish the generated captures.
