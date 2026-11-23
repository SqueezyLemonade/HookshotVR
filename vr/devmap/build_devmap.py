"""The VR dev map: a map of our own, VrTestStage, made from YOUR copy of the game as a Hookshot mod (a romfs folder).

    python build_devmap.py <romfs> <mod folder> [layout.json]
        (e.g. <game>/romfs  <game>/mods/hookshot-vr-devmap  vr/devmap/layouts/default.json)

The layout (made with the editor, vr/devmap/editor) says which of the game's maps is the base (its floor, walls, light and
collision) and what we place in it. The base is cloned under a new name: the files the game finds by a map's name are
copied under VrTestStage's (its actor lists, settings, baked light and probes, collision, navigation mesh, bake grid, volume
stats, foliage), and what the settings name by path stays the base's own, only read. The base map itself is never
replaced: the game still loads it as it was. Ours is reached by our teleport only.
What a map needs that a cloned one lacks (found headless, 17 Nov):
- its named start position in the game's index (Banc/BancBasedResource/StartPos.byml): the teleport's NextStartPos is
  looked up there, not in the map's actor list (without it Link appeared at the world's origin);
- an entrance actor of the start position's name (FieldAndDungeonEntranceEventRequestActor): after a map change the game
  keeps the arrival event and waits for it to start, the loading screen up, and starts it through that actor
  (Sequence/AfterChangeBanc); without it the screen stayed black;
- a floor in the base's own baked collision (Phive/StaticCompoundBody): land pieces placed anew do not show or collide (they
  are pre-actors, drawn and collided through a map's baked data; vr/devmap/research.py), objects (a chest) do. The Sages'
  room was tried first: its floor has no collision at all (a cutscene room).
Each placed object starts from one of the game's own placements of that actor (vr/devmap/catalog.py: its settings, the
Dynamic fields), then takes the layout's position, rotation, scale and edited settings.
The resource size table gets an entry for each new file (the game's table with ours added; Hookshot merges its button
prompts' sizes into it, host/prompts.c), and the start position index is the game's with our map added.

Nothing here is the game's: the mod folder is made on your PC from your files, and is not part of the repository.
"""
import copy
import json
import math
import os
import shutil
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "generate"))
import byml  # noqa: E402
import catalog  # noqa: E402
import prefab  # noqa: E402
import research  # noqa: E402
from byml import Value  # noqa: E402
from game_files import GameFiles  # noqa: E402

NAME = "VrTestStage"
START = "VrTestStart"   # (our start position's name: the teleport's NextStartPos)
DEFAULT_LAYOUT = os.path.join(HERE, "layouts", "default.json")
# (the base's actors left out by default: the Hall of Fallen Foes' own (its enemies, timer, rematch and boss events, Navi's
# hints, exits), NPCs and start positions (ours replaces them); a layout may keep more with "keep")
LEFT_OUT = ("Enemy_", "Npc_", "Obj_DreamWorldTimerManager", "EventRequestActor", "NaviAreaHintEventActor", "NoEscapeRangeArea",
            "StartPos", "DestinationAnchor", "Obj_TBoxPedestal", "FieldAndDungeonEntranceEventRequestActor", "Obj_TBox",
            "Obj_DreamWorld")
# (the folders where the game finds a map's files by its name: each file there whose name starts with the base's)
BY_NAME = ("Bake/Light", "Bake/Scene", "Phive/NavMesh", "Game/BakeSceneLayerConfig", "Game/VolumeStatsAABB")
BY_NAME_IN_FOLDER = ("Phive/StaticCompoundBody", "Phive/VolumeStats", "Foliage/Placement/Scene")


class Mod:
    def __init__(self, romfs, out):
        self.g = GameFiles(romfs)
        self.romfs, self.out = romfs, out
        self.sizes = {}   # (each new file's path without .zs: its size table entry)
        self.edited = {}  # (the game's shared BYML files we add to, written at the end: path -> tree)
        self.own = set()  # (actors of ours made this build)
        import zstandard
        self.z = zstandard
        t = self.z.ZstdDecompressor().decompress(open(self.table_path(), "rb").read(), max_output_size=1 << 28)
        assert t[:6] == b"RESTBL"
        self.table = t
        name, hashed, named = struct.unpack_from("<III", t, 10)
        self.entries = {struct.unpack_from("<I", t, 22 + 8 * i)[0]: struct.unpack_from("<I", t, 26 + 8 * i)[0]
                        for i in range(hashed)}

    def table_path(self):
        d = self.g.path("System/Resource")
        return os.path.join(d, sorted(f for f in os.listdir(d) if f.endswith(".rsizetable.zs"))[-1])

    def entry(self, rel):
        return self.entries.get(zlib.crc32(rel[:-3].encode() if rel.endswith(".zs") else rel.encode()))

    def dest(self, rel):
        p = os.path.join(self.out, "romfs", *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    def copy(self, src, dst):
        """a file as it is, under a new name; its size entry the original's"""
        shutil.copyfile(self.g.path(src), self.dest(dst))
        size = self.entry(src)
        assert size, "%s has no size table entry" % src
        self.sizes[dst[:-3] if dst.endswith(".zs") else dst] = size

    def dictionary(self, rel):
        """the game's dictionary that decompresses this file (ours is compressed with the same), or None"""
        data = open(self.g.path(rel), "rb").read()
        try:
            self.z.ZstdDecompressor().decompress(data, max_output_size=1 << 31)
            return None
        except self.z.ZstdError:
            pass
        for d in self.g.dictionaries():
            full = self.z.ZstdCompressionDict(d, dict_type=self.z.DICT_TYPE_FULLDICT)
            try:
                self.z.ZstdDecompressor(dict_data=full).decompress(data, max_output_size=1 << 31)
                return full
            except self.z.ZstdError:
                continue
        raise ValueError(rel)

    def put_byml(self, src, dst, tree):
        """a BYML file of ours, compressed as the original was; its size entry the original's, grown with the file"""
        data = byml.write(tree)
        if dst.endswith(".zs"):
            d = self.dictionary(src)
            c = self.z.ZstdCompressor(level=19, dict_data=d) if d else self.z.ZstdCompressor(level=19)
            blob = c.compress(data)
        else:
            blob = data
        open(self.dest(dst), "wb").write(blob)
        original = len(self.g.read(src))
        size = self.entry(src)
        assert size, "%s has no size table entry" % src
        self.sizes[dst[:-3] if dst.endswith(".zs") else dst] = int(size * max(1.0, len(data) / original) * 1.5) + 4096

    def write_table(self):
        name, hashed, named = struct.unpack_from("<III", self.table, 10)
        entries = dict(self.entries)
        for rel, size in self.sizes.items():
            k = zlib.crc32(rel.encode())
            entries[k] = max(size, self.entries.get(k, 0))   # (a shared file of the game's we add to: never smaller)
        t = bytearray(self.table[:10]) + struct.pack("<III", name, len(entries), named)
        for k in sorted(entries): t += struct.pack("<II", k, entries[k])
        t += self.table[22 + 8 * hashed:]
        dst = os.path.relpath(self.table_path(), self.romfs).replace(os.sep, "/")
        open(self.dest(dst), "wb").write(self.z.ZstdCompressor(level=19, write_content_size=True).compress(bytes(t)))


def actor(gyaml, at, name=None, rot=None, seed=0, **extra):
    """one placed actor: the game's fields, its hashes our own (stable: from its name, place and seed)"""
    h = zlib.crc32(("%s %s %r %d" % (gyaml, name, at, seed)).encode()) | 0x7a5b << 48 | (seed & 0xffff) << 32
    a = {"Gyaml": gyaml, "Hash": Value.u64(h), "Phive": {"Placement": {"ID": Value.u64(h)}},
         "SRTHash": Value.uint(zlib.crc32(repr((at, rot)).encode())), "Translate": [Value.float(x) for x in at]}
    if name: a["Name"] = name
    if rot: a["Rotate"] = [Value.float(x) for x in rot]
    a.update(extra)
    return a


def from_json(v, like=None):
    """a value of the layout (JSON) as BYML, typed as the game's own value it replaces (like) when there is one"""
    if isinstance(v, dict) and "$t" in v: return Value(v["$t"], v["v"] if not isinstance(v["v"], float) else v["v"])
    if isinstance(v, dict): return {k: from_json(x, like.get(k) if isinstance(like, dict) else None) for k, x in v.items()}
    if isinstance(v, list): return [from_json(x, like[i] if isinstance(like, list) and i < len(like) else None) for i, x in enumerate(v)]
    if isinstance(v, str): return v
    if isinstance(like, Value):
        if like.t == 0xd2: return Value.float(float(v))
        if like.t == 0xd6: return Value(0xd6, struct.unpack("<Q", struct.pack("<d", float(v)))[0])
        if like.t == 0xd0: return Value.bool(bool(v))
        if like.t in (0xd1, 0xd3, 0xd4, 0xd5): return Value(like.t, int(v) & ((1 << (64 if like.t in (0xd4, 0xd5) else 32)) - 1))
    if isinstance(v, bool): return Value.bool(v)
    if isinstance(v, int): return Value.int(v)
    if isinstance(v, float): return Value.float(v)
    raise TypeError(v)


class Base:
    """the map our map is cloned from: its Banc folder and file names, and a start position (its own first)"""

    def __init__(self, g, name):
        self.name = name
        p = byml.read(g.read("Banc/GameBancParam/%s.game__banc__GameBancParam.bgyml" % name))
        main = p["Banc"]["Main"]   # ("Work/Banc/Scene/<folder>/<file>.bcett.json")
        parts = main.split("/")
        self.folder, self.file = parts[3], parts[4].split(".")[0]
        starts = byml.read(g.read("Banc/BancBasedResource/StartPos.byml.zs")).get(name, {})
        first = starts.get(name) or (next(iter(starts.values())) if starts else None)
        self.start = [x.get() for x in first["Trans"]] if first else [0.0, 0.0, 0.0]
        self.start_rot = [x.get() for x in first["Rot"]] if first else [0.0, 0.0, 0.0]


# (actors of our own the layout may place: name -> (the game's actor it is made from, how). VrBlock: a solid block for
# floors, walls and steps, sized by its scale (2.6 x 2.5 x 2.6 m at scale 1, its bottom at its origin): a push block of the
# game's (its collision is its own, unlike land pieces', and the game scales it with the placement), renamed and its body
# made static so it is not pushed (vr/devmap/research.py fixed_copy). Headless 17 Nov: a slab Link stepped onto, a wall he
# slid along.)
OWN = {"VrBlock": ("Obj_MovableBox_A_05", "fixed"), "VrBlockInvisible": ("Obj_MovableBox_A_05", "fixed+unseen")}
BLOCK_SIZE = (2.6, 2.5, 2.6)


def placed(m, cat, obj, seed):
    """one object of the layout as a placed actor: the game's own placement of that actor, ours where the layout says.
    A land piece (a pre-actor: placed anew it would not show) is placed as our own copy of it, a whole actor: it shows,
    but does not collide yet (vr/devmap/research.py)."""
    name = obj["actor"]
    mine_as = None
    if name in OWN:   # (one of ours: placed as the game's actor it is made from, then renamed)
        mine_as, name = name, OWN[name][0]
        if mine_as not in m.own:
            research.fixed_copy(m, mine_as, name)
            if "unseen" in OWN[mine_as][1]: research.unseen(m, mine_as)
            m.own.add(mine_as)
    e = cat["actors"].get(name)
    own = None
    if e and e.get("pre_actor") and e.get("pack"):
        own = "Vr_" + name
        if own not in m.own:
            research.own_actor(m, own, name)
            m.own.add(own)
    a = None
    if e and "from" in e:
        folder, banc, layer = e["from"]
        rel = "Banc/%s/%s_%s.bcett.byml.zs" % (folder, banc, layer)
        for x in byml.read(m.g.read(rel)).get("Actors", []):
            if x.get("Gyaml") == name:
                a = copy.deepcopy(x)
                break
    rot = [math.radians(v) for v in obj.get("rotation", [0, 0, 0])]
    mine = actor(name, tuple(obj["position"]), name=None, rot=tuple(rot), seed=seed)
    if a is None: a = mine
    for k in ("Hash", "Phive", "SRTHash", "Translate", "Rotate"): a[k] = mine[k]
    a.pop("Links", None)
    if any(abs(v) > 1e-6 for v in rot): a["Rotate"] = mine["Rotate"]
    else: a.pop("Rotate", None)
    scale = obj.get("scale", [1, 1, 1])
    if any(abs(v - 1) > 1e-6 for v in scale): a["Scale"] = [Value.float(v) for v in scale]
    else: a.pop("Scale", None)
    if obj.get("name"): a["Name"] = obj["name"]
    if own: a["Gyaml"] = own
    if mine_as: a["Gyaml"] = mine_as
    if obj.get("box"):   # (research: a chest's physics as a box of ours, vr/devmap/research.py chest_box)
        n2 = "VrTest_Box_%s" % "_".join("%g" % v for v in obj["box"]["size"]).replace(".", "p")
        if obj["box"].get("model"): n2 += "_m"
        if n2 not in m.own:
            research.chest_box(m, n2, obj["box"]["size"], obj["box"].get("model"))
            m.own.add(n2)
        a["Gyaml"] = n2
    if obj.get("fixed"):   # (research: an object renamed, its bodies made static, vr/devmap/research.py fixed_copy)
        n4 = obj["fixed"]
        if n4 not in m.own:
            research.fixed_copy(m, n4, name, int(obj.get("motion", 3498399415)))
            m.own.add(n4)
        a["Gyaml"] = n4
    if obj.get("resize"):   # (research: an object of Box collision, renamed and resized, vr/devmap/research.py resized_box)
        n3 = "VrTest_%s_%s" % (name.split("_", 1)[1][:24], "_".join("%g" % v for v in obj["resize"]).replace(".", "p"))
        if n3 not in m.own:
            research.resized_box(m, n3, name, obj["resize"])
            m.own.add(n3)
        a["Gyaml"] = n3
    if obj.get("solid"):   # (research: an invisible twin carrying a piece's own collision mesh, research.mesh_twin)
        n5 = ("VrSolidShown_" if obj.get("shown") else "VrSolid_") + obj["solid"]
        if n5 not in m.own:
            pe = cat["actors"].get(obj["solid"]) or next((x for x in cat["actors"].values() if x.get("FmdbName") == obj["solid"]), None)
            research.mesh_twin(m, n5, obj["solid"], pe and pe.get("aabb"), obj.get("template", research.MESH_TEMPLATE), bool(obj.get("shown")))
            m.own.add(n5)
        a["Gyaml"] = n5
    if obj.get("as"):   # (research: the same actor under a name of ours, vr/devmap/research.py own_actor)
        if obj["as"] not in m.own:
            research.own_actor(m, obj["as"], name)
            m.own.add(obj["as"])
        a["Gyaml"] = obj["as"]
    a.pop("Bakeable", None)
    for k, v in (obj.get("params") or {}).items():
        dyn = a.setdefault("Dynamic", {})
        dyn[k] = from_json(v, dyn.get(k))
    return a


def build(romfs, out, layout_path=DEFAULT_LAYOUT):
    with open(layout_path) as f:
        layout = json.load(f)
    if os.path.isdir(os.path.join(out, "romfs")):
        shutil.rmtree(os.path.join(out, "romfs"))
    m = Mod(romfs, out)
    cat = catalog.load(romfs)
    base = Base(m.g, layout.get("base", "DarkLinkStage"))
    start = layout.get("start") or {}
    start_at = tuple(start.get("position") or base.start)
    start_rot = (0.0, math.radians(start["yaw"]), 0.0) if "yaw" in start else tuple(base.start_rot)
    keep = tuple(layout.get("keep", ()))
    left_out = lambda a: any(a["Gyaml"].startswith(x) for x in LEFT_OUT) and \
        not any(a["Gyaml"].startswith(x) for x in keep)
    main = "Work/Banc/Scene/%s/%s.bcett.json" % (base.folder, NAME)
    banc = "Banc/%s/%s_%%s.bcett.byml.zs" % (base.folder, base.file)
    ours = "Banc/%s/%s_%%s.bcett.byml.zs" % (base.folder, NAME)

    edits = {int(h, 16): e for h, e in (layout.get("base_edits") or {}).items()}

    def edited(actors):   # (the base's own actors as the editor changed them: moved, turned, scaled or removed, by hash)
        out = []
        for a in actors:
            e = edits.get(a["Hash"].raw) if "Hash" in a else None
            if not e:
                out.append(a)
                continue
            if e.get("removed"): continue
            if "position" in e: a["Translate"] = [Value.float(v) for v in e["position"]]
            if "rotation" in e: a["Rotate"] = [Value.float(math.radians(v)) for v in e["rotation"]]
            if "scale" in e:
                if any(abs(v - 1) > 1e-6 for v in e["scale"]): a["Scale"] = [Value.float(v) for v in e["scale"]]
                else: a.pop("Scale", None)
            out.append(a)
        return out

    static = byml.read(m.g.read(banc % "Static"))
    static["FilePath"] = main
    static["Actors"] = edited([a for a in static["Actors"] if not left_out(a)])
    hashes = {a["Hash"].raw for a in static["Actors"]}
    if "AiGroups" in static:   # (the logic of actors left out goes with them)
        static["AiGroups"] = [gr for gr in static["AiGroups"]
                              if all(r["Reference"].raw in hashes for r in gr.get("References", []))]
    for k in ("FarDeleteGroups", "SimultaneousGroups"):
        if k in static:
            static[k] = [g for g in ([h for h in grp if h.raw in hashes] for grp in static[k]) if g]
    # (the arrival event's starter, found by the start position's name; as an exit (where it leads) it is ours too, and with
    # no areas linked to it nothing walks into it)
    entrance = placed(m, cat, {"actor": "FieldAndDungeonEntranceEventRequestActor", "position": start_at,
                               "rotation": [math.degrees(v) for v in start_rot]}, 2)
    entrance["Name"] = START
    entrance["Dynamic"] = {"FirstEventName": "Dm_SY_BancChangeGo", "NextBancParamNameAdult": NAME, "NextBancParamNameChild": NAME,
                           "NextEventName": "Dm_SY_BancChangeArrive", "NextStartPosName": START}
    static["Actors"].append(entrance)
    for k, obj in enumerate(layout.get("objects", [])):   # (prefabs: the game's own sets, logic and all, vr/devmap/prefab.py)
        if "prefab" not in obj: continue
        acts, group, source = prefab.place(m.g, cat, obj, 3000 + k)
        static["Actors"].extend(acts)
        static.setdefault("AiGroups", []).append(group)
        print("dev map: prefab %s (from %s): %d actors" % (obj["prefab"], source, len(acts)))
    m.put_byml(banc % "Static", ours % "Static", static)

    dynamic = byml.read(m.g.read(banc % "Dynamic"))
    dynamic["FilePath"] = main
    dynamic["Actors"] = edited([a for a in dynamic["Actors"] if not left_out(a)])
    dynamic["Actors"].append(actor("StartPos", start_at, name=START, rot=start_rot))
    for k, obj in enumerate(layout.get("objects", [])):
        if "prefab" in obj: continue
        dynamic["Actors"].append(placed(m, cat, obj, 1000 + k))
    m.put_byml(banc % "Dynamic", ours % "Dynamic", dynamic)

    # (the game's index of named start positions, map by map: ours added, every other entry as it was)
    starts = "Banc/BancBasedResource/StartPos.byml.zs"
    index = byml.read(m.g.read(starts))
    index[NAME] = {START: {"Rot": [Value.float(x) for x in start_rot], "Trans": [Value.float(x) for x in start_at]}}
    m.put_byml(starts, starts, index)

    param = "Banc/GameBancParam/%s.game__banc__GameBancParam.bgyml"
    p = byml.read(m.g.read(param % base.name))
    p["Banc"]["Main"] = main
    if layout.get("area", "IceCavern"):   # (a dungeon's area for ours: its area index and dungeon settings, so small keys
        # count here; the Dream World's own (category and index 0) leave a key got but no lock opened. Default: the Ice
        # Cavern's, a dungeon with no small keys of its own; "" keeps the base's)
        area = byml.read(m.g.read(param % layout.get("area", "IceCavern")))
        p.pop("AreaCategory", None)
        for k in ("AreaIndex", "DungeonParam"):
            if k in area: p[k] = area[k]
    m.put_byml(param % base.name, param % NAME, p)

    folders = list(BY_NAME) + ["%s/%s" % (d, base.folder) for d in BY_NAME_IN_FOLDER]
    for d in folders:
        if not os.path.isdir(m.g.path(d)): continue
        for f in sorted(os.listdir(m.g.path(d))):
            if f.startswith(base.name) and len(f) > len(base.name) and f[len(base.name)] in "._":
                m.copy(d + "/" + f, d + "/" + NAME + f[len(base.name):])
    probes = "Bake/Probe/Scene/%s/"
    if os.path.isdir(m.g.path(probes % base.name)):
        for f in sorted(os.listdir(m.g.path(probes % base.name))):
            m.copy(probes % base.name + f, probes % NAME + f)

    for table, t in m.edited.items():
        m.put_byml(table, table, t)
    m.write_table()
    print("dev map: %s (from %s): %d objects placed, %d files, in %s" % (NAME, base.name, len(layout.get("objects", [])),
                                                                       len(m.sizes) + 1, out))


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2], *(sys.argv[3:4] or [DEFAULT_LAYOUT]))
