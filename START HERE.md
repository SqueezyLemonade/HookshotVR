# Hookshot VR: start here

This is a Hookshot fork with VR support. Work in `vr/`; Hookshot's engine remains in its original folders so upstream
updates can still be merged. [The folder map](vr/README.md) gives exact filenames.

Players: see [the install guide](docs/vr-install.md). Contributors: read [CONTRIBUTING.md](CONTRIBUTING.md) first (above
all: no game files, or anything made from them, ever leave your PC), then the [development guide](docs/development.md)
and the [design notes](docs/design.md).

## Find the thing you want to change

| You want to change | Start here | Important distinction |
| --- | --- | --- |
| A VR setting, default or description | `vr/menu/vr_tab_rows.inc`, `vr_options.inc`, `vr_values_menu.inc` | A setting's displayed name may differ from its saved key. Preserve existing preferences when renaming it. |
| Headset connection or startup | `vr/headset/runtime_loader.inc`, `runtime_session.inc` | OpenXR selects the active runtime; no particular headset brand is required by the connection code. Controller profile support is a separate question. |
| The picture reaching each eye | `vr/headset/eye_rendering.inc`, `frame_capture.inc` | Depth mode reconstructs eyes from one image; true stereo draws separate eye views. |
| Stereo lighting, fog, textures or ghosts | `vr/stereo/` | Check a texture's producer and every consumer. A per-eye scene cannot safely feed a shared screen-space result without deliberate handling. |
| Head movement, height, FOV or culling | `vr/camera/` | Game-side camera code and renderer-side eye presentation must agree. |
| Link's torso, arms, wrist twist or fingers | `vr/body/arms_and_pose.inc` | There is one rig implementation, assembled by `vk_vr_body.cpp`; its includes are sections, not competing rigs. |
| Tracking converted into Link's world | `vr/body/tracking.inc` | Positions are metres; controller axes and game axes are not interchangeable. |
| Sword/shield/sheath attachment | `vr/items/gear/held_gear.inc` | Drawn equipment uses semantic game state when known; sheaths and unsupported variants still have fallback paths. |
| Gauntlets, Goron or Zora tunics | `vr/items/gear/equipment_skin.inc`, `tunics.inc` | These are separate skinned models, not just new textures on the base arms. |
| Legs stepping sideways and backwards (first person) | `vr/body/gait.inc`, `vr/tools/generate/leg_gait.py` | Only the drawn legs change; the game's movement and speed stay its own. VR tab → Legs → Game compares. |
| Fingers on a held item (bow, sword, shield, bomb) | `vr/body/arms_and_pose.inc` (`Grip`, `ball_curl`) | Empty hands follow the buttons; a held item sets the pose. A new item gets its own `Grip` case. |
| Bombs: holding, lighting, throwing, catching | `vr/items/bomb/bomb_host.c` | Game-side: it steers the game's own physics body. The fuse is the `Bomb_Blink` animation frame. |
| What first person hides of Link's worn items | `vr/tools/generate/worn_items.py`, `link_worn_hidden` in `equipment_skin.inc` | Worn gear is pinned to the VR body or hidden, never drawn in the game's pose. |
| Bow grip, string and nocked arrow | `vr/items/bow/bow_pose.inc`, `held_arrow.inc` | The shared per-frame bow solution must drive model, string, reticle and launch together. |
| Bow controls or arrow launch | `vr/items/bow/bow_host.c`, `vr_bow_patch.py` | A renderer-only change cannot change the game's projectile collision or inventory logic. |
| Buttons, sticks or quick select | `vr/input/` | Raw OpenXR input, game button mapping and item state are separate layers. |
| Game addresses/hook sites | `vr/vr_game_hooks.py` | Version-specific. Changing hooks normally requires relifting/rebuilding the game. |

## Make a small, safe edit

1. Read the code around the change and its comments: they give the cause of each rule and what was tried. An unknown
   field is not permission to guess a value.
2. Start with a player setting. If it must be code, use a named constant with its unit and an explanation. Values taken from the
   game's own files are made at build time by `vr/tools/generate/`; change the generator, never its output.
3. Change one behavior at a time. Preserve third-person/native behavior unless the feature specifically applies there.
4. Run `python3 -I -B vr/tools/test.py`, then the relevant build/runtime checks. [Build instructions](vr/README.md#build-and-test).
5. Say in the pull request what you ran in the game or the headset, and what you did not. A build or a headless picture is
   not a headset test.

`.inc` and `*_host.c` files are included sections. Their parent entry points own includes, namespaces, static helpers
and compilation order. Do not compile a section by itself or add a second implementation to work around an ordering error.

## Keep these protections

- All passes of a game frame must use the same frozen body/bow solution. Otherwise colour, depth, shadow and temporal
  motion disagree, producing blinking, dark copies or jitter.
- The GPU replay reads submit-time captured game memory, not whatever live memory happens to contain later.
- Preserve GPU synchronization and both-eye routing when reorganizing renderer code. A faster-looking frame that skips
  writes or uses the other eye's data is not an optimization.
- Do not publish game files, extracted models/textures, generated headers, saves, captures, caches or compiled game output.
- Keep the GPL license and the third-party notices ([NOTICE](NOTICE), and each part's folder). A new third-party part
  adds its row to NOTICE.
- No Nintendo art or logos in the repository, and no personal information (names, home paths, emails) in files or logs.
