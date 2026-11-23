"""The dev map editor's catalog: every kind of actor YOUR copy of the game places in its maps, for the editor's palette.

    python catalog.py <romfs>        (prints a summary; the editor's server builds it on its first start)

For each actor (its Gyaml name): how many times the game's maps place it, one placement to start ours from (its own
settings: the Dynamic fields, Phive, the rest; the map it came from), its model's bounding box and model names (RSDB
ActorInfo), whether it is a pre-actor (a land piece: drawn and collided only through a map's baked data, so placed
anew it does not show: the editor says so) and a category for the palette.
It is made from your files and kept on your PC (CACHE), never in the repository.
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "generate"))
import byml  # noqa: E402
from game_files import GameFiles  # noqa: E402

CACHE = os.path.join(os.path.expanduser("~"), ".cache", "hookshot-vr-devmap")
VERSION = 7   # (the cache's format: a newer one rebuilds it)

# (the palette's categories, the first whose test matches; names are the game's own)
CATEGORIES = (
    ("Lighting", re.compile(r"^EftLoc_|Light|Lamp|Torch|Candle|Lantern|Brazier|Bonfire")),
    ("Doors", re.compile(r"Door|Shutter|Gate|Bars")),
    ("Walls", re.compile(r"Wall|Fence|Pillar|Column")),
    ("Floors", re.compile(r"Floor|Base|Ground|Plate|Tile|Stair|Step|Bridge|Scaffold|Lift")),
    ("Foliage", re.compile(r"Plant|Grass|Tree|Bush|Flower|Foliage|Weed|Leaf|Vine|Mushroom")),
    ("Landscape", re.compile(r"^Land_|Rock|Cliff|Mountain|Hill")),
    ("NPCs", re.compile(r"^Npc_")),
    ("Enemies", re.compile(r"^Enemy_")),
    ("Items", re.compile(r"^Item_|^Weapon_|^Armor_|TBox|Rupee|Heart")),
    ("Logic", re.compile(r"Area|StartPos|Anchor|Event|Manager|Switch|Sensor|Trigger|Timer|Link|Spawn|Pos$")),
    ("Objects", re.compile(r"^Obj_")),
)


PREFABS = re.compile(r"^Dungeon.*Set|Shutter|Door|ChainLock|DefeatEnemy|Gimmick_TrapTreasureBox|MovableBox")
PREFAB_SKIP = re.compile(r"Level_|Prefab|Npc|Restricted")


def plain(x):
    """a BYML tree as JSON (the editor's), types kept beside values that are not plain: [type, value]"""
    if isinstance(x, dict): return {k: plain(v) for k, v in x.items()}
    if isinstance(x, list): return [plain(v) for v in x]
    if isinstance(x, str): return x
    v = x.get()
    return v if x.t in (0xd0, 0xd1, 0xd2) else {"$t": x.t, "v": v}


def category(name):
    for label, test in CATEGORIES:
        if test.search(name): return label
    return "Other"


def build(romfs):
    g = GameFiles(romfs)
    actors = {}
    maps = []
    prefabs = {}
    wired = []
    for f in sorted(glob.glob(os.path.join(romfs, "Banc", "*", "*_*.bcett.byml.zs"))):
        rel = os.path.relpath(f, romfs).replace(os.sep, "/")
        try:
            tree = byml.read(g.read(rel))
        except Exception:
            continue
        if not isinstance(tree, dict): continue
        layer = "Static" if rel.endswith("_Static.bcett.byml.zs") else "Dynamic"
        banc = os.path.basename(f).rsplit("_", 1)[0]
        maps.append(banc)
        by = {x["Hash"].raw: x["Gyaml"] for x in tree.get("Actors") or [] if "Hash" in x}
        for gr in tree.get("AiGroups") or []:   # (wired sets: a switch, what it opens and the event that opens it)
            refs = [by.get(r["Reference"].raw, "") for r in gr.get("References", [])]
            switch = next((r for r in refs if "Switch" in r and "Shutter" not in r), None)
            target = next((r for r in refs if re.search(r"Shutter|Door|Gate|Bridge|Elevator|TBox", r)), None)
            if switch and target and "EventRequestActor" in refs and len(refs) <= 12:
                wired.append({"module": gr["Logic"].split("/")[2], "from": banc, "group": "%016x" % gr["Hash"].raw,
                              "label": "%s opens %s" % (re.sub(r"^Obj_|_A_\d+$", "", switch), re.sub(r"^Obj_|_A_\d+$", "", target)),
                              "actors": refs})
        for gr in tree.get("AiGroups") or []:   # (the sets the editor offers as prefabs: doors, shutters, locks)
            lg = gr.get("Logic", "")
            if "/Module/" in lg:
                mod = lg.split("/")[2]
                if PREFABS.search(mod) and not PREFAB_SKIP.search(mod):
                    p = prefabs.setdefault(mod, [])
                    if banc not in p: p.append(banc)
        for a in tree.get("Actors") or []:
            n = a.get("Gyaml")
            if not n: continue
            e = actors.setdefault(n, {"count": 0, "maps": []})
            e["count"] += 1
            for k, v in (a.get("Dynamic") or {}).items():   # (each setting's values in the game's maps: the editor's choices)
                if isinstance(v, str):
                    ch = e.setdefault("choices", {}).setdefault(k, [])
                    if v not in ch and len(ch) < 80: ch.append(v)
            if banc not in e["maps"] and len(e["maps"]) < 8: e["maps"].append(banc)
            if "example" not in e:
                ex = {k: v for k, v in a.items() if k not in ("Hash", "SRTHash", "Phive", "Translate", "Rotate", "Links")}
                e["example"] = plain(ex)
                e["from"] = [rel.split("/")[1], banc, layer]
    for e in actors.values():
        for k in e.get("choices", {}): e["choices"][k].sort()
    # (a chest holds any of the items chests hold anywhere: their choices shared between every chest)
    chests = [e for n, e in actors.items() if "TBox" in n and "Pedestal" not in n]
    items = sorted({v for e in chests for v in e.get("choices", {}).get("Drop__DropActor", [])})
    for e in chests:
        if items: e.setdefault("choices", {})["Drop__DropActor"] = items
    info = {r["__RowId"]: r for r in byml.read(g.read(_one(g, "RSDB/ActorInfo.Product.*.rstbl.byml.zs")))}
    for n, e in actors.items():
        r = info.get(n)
        e["category"] = category(n)
        e["pack"] = g.exists("Pack/Actor/%s.pack.zs" % n)
        if r:
            lo, hi = r.get("ModelAabbMin"), r.get("ModelAabbMax")
            if lo and hi:
                e["aabb"] = [[lo[k].get() for k in "XYZ"], [hi[k].get() for k in "XYZ"]]
            for k in ("FmdbName", "ModelProjectName"):
                if k in r: e[k] = r[k]
            e["pre_actor"] = bool(r.get("IsNotTurnToActor") and r["IsNotTurnToActor"].get())
    return {"version": VERSION, "actors": actors, "maps": sorted(set(maps)), "prefabs": prefabs, "wired": wired}


def _one(g, pattern):
    hits = sorted(glob.glob(g.path(pattern)))
    return os.path.relpath(hits[-1], g.romfs).replace(os.sep, "/")


def load(romfs):
    """the catalog, from the cache when it is there and current"""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, "catalog.json")
    try:
        with open(path) as f:
            c = json.load(f)
        if c.get("version") == VERSION and c.get("romfs") == os.path.abspath(romfs): return c
    except (OSError, ValueError):
        pass
    c = build(romfs)
    c["romfs"] = os.path.abspath(romfs)
    with open(path, "w") as f:
        json.dump(c, f)
    return c


if __name__ == "__main__":
    import collections
    c = load(sys.argv[1])
    print(len(c["actors"]), "actors in", len(c["maps"]), "maps")
    print(collections.Counter(e["category"] for e in c["actors"].values()).most_common())
