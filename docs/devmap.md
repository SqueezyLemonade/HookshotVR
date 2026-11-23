# The VR test map and its editor

A map of our own for testing VR features (and, later, a tutorial dungeon): **VrTestStage**, reached from the Insert menu's
**Teleport > VR > VR test map**. You design it in a 3D editor in your browser; the editor builds it as a Hookshot mod on
your PC from your own copy of the game. Nothing of the game's is in this repository: only the code and your layouts
(actor names, places and settings).

## Using it

1. Open the editor:

   ```bash
   HOOKSHOT_BUILDER=<unpacked builder> vr/devmap/editor.sh <game folder>
   ```

   (any OS: `<builder python> vr/devmap/editor/server.py <game folder>/romfs`, then open http://127.0.0.1:8765). The first
   start reads the game's maps for the actor catalog (kept in `~/.cache/hookshot-vr-devmap`).
2. Drag actors from the palette into the view (or double-click one), move, rotate and scale them with the handles (W, E,
   R), edit their settings on the right. The green arrow is where Link arrives. **Save** keeps the layout in
   `vr/devmap/layouts/<name>.json`.
3. **Build map** writes the mod into `<game folder>/mods/hookshot-vr-devmap`. Restart the game, then Teleport > VR test map.
   (A restart is needed after each Build: the game keeps a map it has loaded, even after visiting another one; tested.)

**Your save is safe there.** From the teleport to the test map until another map has loaded, the game's saves (it autosaves
on arrival and after getting an item) go to a copy of your save folder (`<save>-vrtest`), never your own. What you pick up
in the test map is still in Link's inventory while the game runs: load your save to be rid of it.

**Bases**: the default is Dark Link's room (a flat misty lake, cheap to draw in VR); the Iron Knuckle hall (a compact Spirit
Temple room) and Ganondorf's arena are good dungeon rooms. Any of the game's maps can be the base; the base is copied,
never changed.

**In the editor**:
- Each palette row shows a small picture of the actor (its model and texture; a box for the ~1 in 7 models whose meshes are
  stored in a form not read yet).
- **The base map's own pieces can be edited**: click one to move, turn, scale or remove it (Restore puts it back). Pieces
  baked into the base draw where you put them, but their collision stays baked where they were: the editor warns.
- **Baked light** (on by default): the base's pieces are shaded with the game's own baked lighting, so corners, gaps
  and undersides darken as in the game. (`Bake/Scene/<map>_*.bkres` places each piece's materials in a light atlas,
  `Tex/RBake_Scene_*`; its red channel, ambient occlusion, is used. The coloured bounce light, stored as spherical
  harmonics, is not read yet.)
- The base's **lights** show as glowing markers (and light the view); **water and lava** show see-through.
- **Right-click** something for quick actions: a chest's contents, a set member's type, duplicate, delete.
- **Wired sets** (Prefabs: "EyeSwitch opens SwitchShutter (ForestTemple)", 35 of them): a switch, what it opens (a
  shutter, bars, a chest that appears) and the game's own event between them. Click a member (the switch, the shutter) to
  move it on its own, or right-click to swap it for another of its kind; the wiring comes along. Tested: a floor switch
  moved 3 m from its shutter still opened it, with the game's event.

**Marking places from VR**: in the VR test map, hold the left Menu button and press X where you want something (a short
rumble). In the editor, **Marks > Load** shows each as a yellow pin: drop an actor on a pin to place it there, turned the
way you were facing. Marks are kept in `<game folder>/mods/hookshot-vr-devmap/marks.jsonl` until **Clear**.

Keys: W/E/R move/rotate/scale, Ctrl+D duplicate, Delete, F frame, Ctrl+Z/Ctrl+Y undo/redo, Ctrl+S save, Esc deselect.
The camera flies freely: hold the right button to look round and fly with WASD (Q/E down/up, Shift faster, the wheel
sets the speed); the arrow keys and Page Up/Down fly without holding it. The wheel alone moves toward the cursor by a fixed step, however close you are. Left drag on empty space
orbits round the point you pressed, middle drag pans.

## What works, and what does not yet

| | |
|---|---|
| **Blocks** (the palette's first category: `VrBlock`) | **solid**: floors, walls, steps and platforms Link stands on and bumps into; sized by their scale (2.6 x 2.5 x 2.6 m at scale 1; the **Size (m)** fields set it in metres); they look like dungeon stone blocks |
| Objects (chests, boxes, targets, switches, pots, ...), NPCs, enemies, effects | placed as the game places them, with their own settings and collision; settings the game's maps give several values get a menu (a chest's **Contents**: any of the 36 items chests hold) |
| **Prefabs** (the game's own sets: doors, locked doors, shutters, ...) | copied with their logic and parts; an ordinary dungeon door opens and a **chain-locked door opens with a small key** from a chest (walls of blocks on both sides and a block over it, or Link climbs round). Keys count because the map takes a dungeon's area: the Ice Cavern's by default (it has no small keys of its own; a layout's `"area"` picks another, `""` keeps the base's) |
| The base map (floor, walls, lighting, collision) | cloned from one of the game's maps (the **Base** menu); the base itself is never changed |
| **`VrBlockInvisible`** (Blocks) | solid, but **not drawn** in the game: an invisible wall or floor; put one inside a land piece to make the piece solid |
| Land pieces (floors, walls, rocks: `Land_*`) | **show, but do not collide**: for something solid, put a block (or an invisible block) under or in them |
| Baked lighting, landscape (terrain) | the base's own; cannot be made new |

Why blocks: the game's land pieces collide only through a map's baked collision file (a compiled Havok compound); a piece
placed anew is not in it. Objects with their own collision do collide wherever placed, and **the game scales an object's
collision with its placement's scale** (found headless 17 Nov with a push block: at scale 2 x 0.12 x 2 it was a 0.3 m slab
Link stepped onto; a tall thin one was a wall he slid along). `VrBlock` is that push block, renamed and made static so it
cannot be pushed. Editing an object's collision shapes instead stops it colliding (`vr/devmap/research.py`: all that was
tried).

## How it works (vr/devmap)

- `build_devmap.py`: clones the base map under the name VrTestStage (the files the game finds by a map's name), leaves
  out the base's enemies and events, and places the layout's actors, each starting from one of the game's own placements
  of it. A cloned map also needs its start position in the game's start index (`Banc/BancBasedResource/StartPos.byml`) and
  an entrance actor named like it (`FieldAndDungeonEntranceEventRequestActor`), or the screen stays black after the
  teleport; both are added. New files get entries in the resource size table.
- `prefab.py`: a set of the game's (a logic module and the actors it references, with what they link to), copied from a
  map that has it and placed in ours: new hashes, links and references remapped, room indexes dropped.
- `catalog.py`: every actor the game's maps place, with an example placement, its model and box, and a palette category.
- `editor/`: the local server (`server.py`, 127.0.0.1 only) and the page (three.js). `model_glb.py` reads a model (BFRES)
  into glTF for the view, with each material's colour texture (`textures.py`, decoded from `Tex/*.bntx.zs`).
- `byml.py`, `sarc.py`: the game's data and archive formats, read and written.
- Headless tests: `RX_VR_TELEPORT_AT=<s>` teleports that many seconds in (`RX_VR_TELEPORT_INDEX=31`: the VR test map, or
  `RX_VR_TELEPORT_BANC`/`_START` for any map) and logs where Link is every 2 s; `HOS_MODS=<folder>` picks the mods.
