"""Makes the VR mod's game-derived headers from YOUR copy of the game.

Some of the mod's numbers come from the game's own files: Link's skeleton, how the tunics and gauntlets attach, where
the sword and shield sit in his hands, which arm bones child Link's Goron Bracelet may sit on, the bow's rest pose and its
reticle masks, his walk and run cycles. Those files are Nintendo's, so neither they nor anything made from them goes into
this repository. Instead the build runs this on the player's own romfs and puts the
headers in the build's work folder (nx2/build.py, VR HOOK).

    python generate.py <romfs folder> <output folder>

Each generator is one file here: link_rig.py, tunics.py, gauntlets.py, hand_sockets.py, bow_rest.py, bow_reticle.py,
worn_items.py, leg_gait.py, arrows.py, hookshot.py, slingshot.py, ocarina.py, boomerang.py, bottle.py, bomb_flower.py,
icons.py, shield_grip.py. A header is
rewritten only when its contents change, so an unchanged game does not recompile anything. One more header is not the game's:
panel_frame.py packs the VR menu's frame art (assets/vr/panel_frame.png) the same way, as it is not in the repository either.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import arrows   # noqa: E402
import bomb_flower   # noqa: E402
import boomerang   # noqa: E402
import bottle   # noqa: E402
import bow_rest   # noqa: E402
import bow_reticle   # noqa: E402
import gauntlets   # noqa: E402
import hand_sockets   # noqa: E402
import hookshot   # noqa: E402
import icons   # noqa: E402
import leg_gait   # noqa: E402
import link_rig   # noqa: E402
import ocarina   # noqa: E402
import shield_grip   # noqa: E402
import slingshot   # noqa: E402
import tunics   # noqa: E402
import worn_items   # noqa: E402
from game_files import GameFiles   # noqa: E402
from models import Models   # noqa: E402

GENERATORS = (link_rig, tunics, gauntlets, hand_sockets, bow_rest, bow_reticle, worn_items, leg_gait, arrows, hookshot, slingshot,
              ocarina, boomerang, bottle, bomb_flower, icons, shield_grip)
OUTPUTS = tuple(g.OUTPUT for g in GENERATORS)


def sources():
    """The generators' own code, so a build can tell when a header's recipe changed"""
    return b"".join(open(os.path.join(HERE, f), "rb").read() for f in sorted(os.listdir(HERE)) if f.endswith(".py"))


def missing_inputs(romfs):
    game = GameFiles(romfs)
    need = {"Pack/ZsDic.pack.zs"} | {rel for g in GENERATORS for rel in g.INPUTS}
    return sorted(rel for rel in need if not game.exists(rel))


def generate(romfs, out_dir):
    """Writes every header into out_dir: all of them, or (when any fails) none. Returns the names of those that changed"""
    missing = missing_inputs(romfs)
    if missing:
        raise SystemExit("Hookshot VR: these game files are missing from %s: %s" % (romfs, ", ".join(missing)))
    models = Models(GameFiles(romfs))
    made = [(g.OUTPUT, g.make(models).encode()) for g in GENERATORS]
    os.makedirs(out_dir, exist_ok=True)
    changed = []
    for name, text in made:
        path = os.path.join(out_dir, name)
        try:
            with open(path, "rb") as f:
                if f.read() == text:
                    continue
        except OSError:
            pass
        with open(path + ".new", "wb") as f:
            f.write(text)
        os.replace(path + ".new", path)
        changed.append(name)
    import panel_frame   # (not the game's: the VR menu's frame, the user's own art, assets/vr/panel_frame.png)
    if panel_frame.write(out_dir):
        changed.append(panel_frame.OUTPUT)
    import wrist_maps   # (the wrist HUD's minimap pictures: slow to decode, so made again only when their inputs change)
    if wrist_maps.write(romfs, out_dir):
        changed.append(wrist_maps.OUTPUT)
    return changed


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    done = generate(sys.argv[1], sys.argv[2])
    print("Hookshot VR: game-derived headers %s" % (("updated: " + ", ".join(done)) if done else "unchanged"))
