"""Dev map prefabs (vr/devmap): one of the game's own working sets of actors (a door, a locked door, a shutter: a logic
module and the actors it references), copied out of the game map that has it and placed in ours.

The game's dungeons wire a door to its parts through an AiGroup: a logic module (Logic/Module/<name>/...) and the actors it
references by Id (DungeonDoorSingleWithChainLockSet: the door, its two chain locks, its door-front air wall). Those actors
link on to others (Links: the anchors Link walks through a door by). A prefab is all of them, kept where they stand
relative to the first referenced actor (the anchor), placed at a position and turn of ours: each actor a new hash, the
links and the module's references remapped to them. Room indexes (DefaultDungeonRoomIndex) are dropped: our map has no
rooms. Nothing is kept in the repository: it is read from your game files when the map is built.
"""
import copy
import math
import os
import zlib

import numpy as np

import byml
from byml import Value

KEEP_LINKED = ("DestinationAnchor", "PushAirWall", "ShutterLocator", "DoorLocator", "ChainLock", "Obj_DoorChainLock",
               "Obj_SwitchShutter", "Obj_EnemyCounter")   # (actors a set's members link to that go with it)


def euler_matrix(r):
    """the game's rotation (radians, sead: Rz Ry Rx)"""
    cx, sx, cy, sy, cz, sz = math.cos(r[0]), math.sin(r[0]), math.cos(r[1]), math.sin(r[1]), math.cos(r[2]), math.sin(r[2])
    rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return rz @ ry @ rx


def matrix_euler(m):
    """back to (x, y, z) radians for Rz Ry Rx"""
    sy = -m[2, 0]
    y = math.asin(max(-1.0, min(1.0, sy)))
    if abs(sy) < 0.9999:
        x = math.atan2(m[2, 1], m[2, 2])
        z = math.atan2(m[1, 0], m[0, 0])
    else:
        x = math.atan2(-m[1, 2], m[1, 1])
        z = 0.0
    return (x, y, z)


def vec(a, key, default):
    v = a.get(key)
    return [x.get() for x in v] if v else list(default)


def find(g, cat, module, source=None, nth=0, group=None):
    """the set: (its AiGroup, its actors (dicts, copies), the anchor's place and rotation), from the game map that has it:
    the nth of that module there, or the group of that hash (hex)"""
    maps = [source] if source else sorted(cat["maps"])
    seen = 0
    for banc in maps:
        lists = []
        for d in sorted(os.listdir(g.path("Banc"))):
            for layer in ("Static", "Dynamic"):
                rel = "Banc/%s/%s_%s.bcett.byml.zs" % (d, banc, layer)
                if g.exists(rel): lists.append(byml.read(g.read(rel)))
        if not lists: continue
        by = {a["Hash"].raw: a for t in lists for a in t.get("Actors", []) if "Hash" in a}
        for t in lists:
            for gr in t.get("AiGroups", []):
                lg = gr.get("Logic", "")
                if "/Module/%s/" % module not in lg: continue
                if group is not None and "%016x" % gr["Hash"].raw != group: continue
                if group is None and seen < nth:
                    seen += 1
                    continue
                members = [by[r["Reference"].raw] for r in gr.get("References", []) if r["Reference"].raw in by]
                if not members: continue
                chosen = {a["Hash"].raw: a for a in members}
                for a in members:   # (what they link to that belongs with them)
                    for l in a.get("Links", []):
                        d = by.get(l["Dst"].raw)
                        if d is not None and d["Gyaml"].startswith(KEEP_LINKED): chosen.setdefault(d["Hash"].raw, d)
                anchor = members[0]
                return gr, [copy.deepcopy(a) for a in chosen.values()], vec(anchor, "Translate", (0, 0, 0)), \
                    vec(anchor, "Rotate", (0, 0, 0)), banc
    raise KeyError("no %s in %s" % (module, source or "the game's maps"))


def place(g, cat, spec, seed):
    """a prefab of the layout (spec: prefab, from, nth, position, rotation in degrees) as (actors, AiGroup) for our map"""
    gr, actors, at, rot, banc = find(g, cat, spec["prefab"], spec.get("from"), spec.get("nth", 0), spec.get("group"))
    base_r = euler_matrix(rot)
    mem = spec.get("members") or {}   # (members moved or swapped on their own: index -> position, rotation (from the set's
    # origin, the set's own axes), actor)
    new_r = euler_matrix([math.radians(v) for v in spec.get("rotation", [0, 0, 0])])
    turn = new_r @ base_r.T
    new_at = np.array(spec["position"], dtype=float)
    remap = {}
    for k, a in enumerate(actors):
        h = (zlib.crc32(("%s %d %d %s" % (spec["prefab"], seed, k, a["Gyaml"])).encode()) | 0x7a5c << 48 | (seed & 0xffff) << 32)
        remap[a["Hash"].raw] = h
    out = []
    for k, a in enumerate(actors):
        p = np.array(vec(a, "Translate", (0, 0, 0)))
        r = euler_matrix(vec(a, "Rotate", (0, 0, 0)))
        a["Translate"] = [Value.float(float(x)) for x in turn @ (p - np.array(at)) + new_at]
        a["Rotate"] = [Value.float(float(x)) for x in matrix_euler(turn @ r)]
        e = mem.get(str(k)) or {}
        if "position" in e: a["Translate"] = [Value.float(float(x)) for x in new_r @ np.array(e["position"], float) + new_at]
        if "rotation" in e: a["Rotate"] = [Value.float(float(x)) for x in matrix_euler(new_r @ euler_matrix([math.radians(v) for v in e["rotation"]]))]
        if e.get("actor"): a["Gyaml"] = e["actor"]
        h = remap[a["Hash"].raw]
        a["Hash"] = Value.u64(h)
        a["Phive"] = {"Placement": {"ID": Value.u64(h)}}
        a["SRTHash"] = Value.uint(zlib.crc32(repr(a["Translate"]).encode()))
        a.pop("Bakeable", None)
        if "Dynamic" in a: a["Dynamic"].pop("DefaultDungeonRoomIndex", None)
        if "Links" in a:
            a["Links"] = [dict(l, Src=Value.u64(remap[l["Src"].raw]), Dst=Value.u64(remap[l["Dst"].raw]))
                          for l in a["Links"] if l["Dst"].raw in remap and l["Src"].raw in remap]
            if not a["Links"]: a.pop("Links")
        out.append(a)
    group = copy.deepcopy(gr)
    group["Hash"] = Value.u64(zlib.crc32(("group %s %d" % (spec["prefab"], seed)).encode()) | 0x7a5d << 48)
    group["References"] = [dict(r, Reference=Value.u64(remap[r["Reference"].raw])) for r in group.get("References", [])
                           if r["Reference"].raw in remap]
    return out, group, banc


def members(g, cat, module, source=None, nth=0, group=None):
    """for the editor: the set's actors where they stand from its anchor (name, position, rotation in degrees)"""
    gr, actors, at, rot, banc = find(g, cat, module, source, nth, group)
    inv = euler_matrix(rot).T
    out = []
    for a in actors:
        p = inv @ (np.array(vec(a, "Translate", (0, 0, 0))) - np.array(at))
        r = matrix_euler(inv @ euler_matrix(vec(a, "Rotate", (0, 0, 0))))
        out.append({"actor": a["Gyaml"], "position": [round(float(x), 3) for x in p],
                    "rotation": [round(math.degrees(x), 2) for x in r]})
    return {"module": module, "from": banc, "members": out}
