"""Dev map research (vr/devmap): actors of our own for floors and walls that collide wherever they are placed.
SOLVED 17 Nov by fixed_copy (VrBlock, build_devmap.OWN): a push block (Obj_MovableBox_A_05, its collision its own Box) placed
with a scale: the game scales the collision with the placement (a 2 x 0.12 x 2 slab Link stepped onto, a 0.3 x 3 x 2 wall
he slid along), renamed with its bodies made static. What was tried before that, kept for the record: Found headless 17 Nov with a walking test (the stick held after the arrival, Link's place
logged every 2 s: blocked, he stops; a step, his height rises):
- the game's land pieces placed anew neither draw nor collide: they are pre-actors (RSDB ActorInfo IsNotTurnToActor and
  UsePreActorRenderer; PreActorSetting ConstPassive), drawn and collided only through a map's baked data;
- own_actor: a land piece's pack and rows renamed and made a whole actor (ActorOnly): it DRAWS, does not collide;
- block_actor: a box of ours with a chest-like body (kinematic, then fixed, a polytope in the Object layer, the body
  following the actor or not): it draws, does not collide;
- real chests (Obj_TBox_A_01_NoBake) collide, and so does own_actor's renamed copy of one: new actor names are not the problem;
- chest_box (a renamed chest, its main body's shape changed): its shape's corners 1 % further out, or fitted at the chest's
  own size: still blocks; fitted to 4 x 0.5 x 4 m or a 0.5 x 2 x 4 m wall: does not, also with that body moved to the
  GroundObject layer and made static. A hand-made Box or polytope instead: the actor may not even load.
  The chest's bodies: Body (Object layer, kinematic), Cover (Object, dynamic), CloseBody (GroundObject, static, shape
  Body_Close: not among the pack's ShapeParam files), AirWall (GroundObject/AirWall, kinematic, tracking the lid's bone).
  Characters likely collide with the GroundObject ones, so the blocking seen may be CloseBody's or AirWall's, never
  the edited Body's.
- resized_box: the push block renamed, its Box shape resized (4 x 0.3 x 4): no collision. Editing a shape (beyond 1 %)
  loses it; scaling the placement keeps it.
- Obj_InsideGanonCastle_GlassTruth_A_05 (a platform with a Box shape) placed as it is: no collision (it is tied to its
  dungeon room, DefaultDungeonRoomIndex, and likely off outside it).
- The baked collision (Phive/StaticCompoundBody/*.bphsc, a Havok compound): it holds a table of nearly every placed
  actor's Phive Placement ID (32 bytes apart); adding instances to it would need its compiled format, not documented.
"""
import copy
import os

import byml
import sarc
from byml import Value


def own_actor(m, name, base, rows=None):
    """an actor of ours, VrLand_...: a land piece of the game's (base) made a whole actor. Land pieces are pre-actors
    (RSDB ActorInfo IsNotTurnToActor, UsePreActorRenderer; ActorParam PreActorSetting ConstPassive): drawn and collided only
    through a map's baked data, so one placed anew neither shows nor collides. Ours: the same pack and database rows,
    renamed, as a chest's are set (ActorOnly, not a pre-actor). rows: database fields to change."""
    pack = "Pack/Actor/%s.pack.zs"
    files = sarc.read(m.g.read(pack % base))
    ours = {}
    for k, v in files.items():
        if k == "Actor/%s.engine__actor__ActorParam.bgyml" % base:
            t = byml.read(v)
            t["Components"]["PreActorSettingRef"] = "?ActorSystem/PreActorSetting/ActorOnly.game__actor__PreActorSetting.bgyml"
            ours["Actor/%s.engine__actor__ActorParam.bgyml" % name] = byml.write(t)
        else:
            ours[k] = v
    data = sarc.write(ours)
    d = m.dictionary(pack % base)
    c = m.z.ZstdCompressor(level=19, dict_data=d) if d else m.z.ZstdCompressor(level=19)
    open(m.dest(pack % name), "wb").write(c.compress(data))
    m.sizes[(pack % name)[:-3]] = int(m.entry(pack % base) * max(1.0, len(data) / len(m.g.read(pack % base)))) + 4096
    for table, change in (("RSDB/ActorInfo.Product.111.rstbl.byml.zs", {"IsNotTurnToActor": None, "UsePreActorRenderer": None}),
                          ("RSDB/GameActorInfo.Product.111.rstbl.byml.zs", {"IsBakeCaster": None})):
        t = m.edited.get(table) or byml.read(m.g.read(table))
        row = copy.deepcopy(next(e for e in t if e["__RowId"] == base))
        row["__RowId"] = name
        for k, v in dict(change, **(rows or {})).items():
            if v is None: row.pop(k, None)
            else: row[k] = v
        t[:] = sorted([e for e in t if e["__RowId"] != name] + [row], key=lambda e: e["__RowId"].encode())
        m.edited[table] = t


def block_actor(m, name, size, model="Land_DeathMtTrail_Base_A_12"):
    """an actor of ours, a solid box: size (x, y, z) metres, its top face at its origin, centred on it across; drawn with a
    model of the game's (model: an actor whose model it borrows). Its collision is its own rigid body (the chest's kind: a
    kinematic body in the Object layer, a convex box shape), not the baked land kind, so it collides wherever it is placed."""
    chest, rock = sarc.read(m.g.read("Pack/Actor/Obj_TBox_A_01_NoBake.pack.zs")), sarc.read(m.g.read("Pack/Actor/%s.pack.zs" % model))
    w = lambda tree: byml.write(tree)
    f = lambda x: Value.float(x)
    work = lambda kind, file: "Work/%s/%s.gyml" % (kind, file)
    files = {}
    actor_param = byml.read(rock["Actor/%s.engine__actor__ActorParam.bgyml" % model])
    actor_param["Components"]["PhysicsRef"] = "?Component/Physics/%s.engine__component__PhysicsParam.bgyml" % name
    actor_param["Components"]["PreActorSettingRef"] = "?ActorSystem/PreActorSetting/ActorOnly.game__actor__PreActorSetting.bgyml"
    files["Actor/%s.engine__actor__ActorParam.bgyml" % name] = w(actor_param)
    for k, v in rock.items():
        if k.startswith("Component/ModelInfo/"): files[k] = v
    files["Component/Physics/%s.engine__component__PhysicsParam.bgyml" % name] = w({
        "$parent": "Work/Component/Physics/ProjectBase.engine__component__PhysicsParam.gyml",
        "ControllerSetPath": work("Phive/ControllerSetParam", name + ".phive__ControllerSetParam")})
    files["Phive/ControllerSetParam/%s.phive__ControllerSetParam.bgyml" % name] = w({
        "AliasEntityList": [{"AliasEntity": "Main", "ControllerName": "Body", "IsCharacterController": Value.bool(False),
                             "RigidBodyName": "Body"}],
        "ControllerEntityNamePathAry": [{"FilePath": work("Phive/RigidBodyControllerEntityParam", name + ".phive__RigidBodyControllerEntityParam"),
                                         "Name": "Body"}],
        "ControllerSensorNamePathAry": [],
        "RigidBodyEntityNamePathAry": [{"FilePath": work("Phive/RigidBodyEntityParam", name + "_Body.phive__RigidBodyEntityParam"),
                                        "Name": "Body"}],
        "RigidBodySensorNamePathAry": [],
        "ShapeNamePathAry": [{"FilePath": work("Phive/ShapeParam", name + "__Body.phive__ShapeParam"), "Name": "Body_Body"}]})
    unit = byml.read(next(v for k, v in rock.items() if k.startswith("Phive/RigidBodyControllerEntityParam/")))["RigidBodyControllerUnitAry"][0]
    unit["Name"] = "Body"
    if os.environ.get("DEVMAP_BODY") == "track":   # (research: the body follows the actor)
        unit["IsTrackingActor"] = Value.bool(True)
        unit["UseActorPosForNonTrackingComp"] = Value.bool(True)
        unit["UseNextMainRigidBodyMatrix"] = Value.bool(False)
    files["Phive/RigidBodyControllerEntityParam/%s.phive__RigidBodyControllerEntityParam.bgyml" % name] = w({"RigidBodyControllerUnitAry": [unit]})
    body = byml.read(chest["Phive/RigidBodyEntityParam/Obj_TBox_Body_0.phive__RigidBodyEntityParam.bgyml"])
    body["ShapeName"] = "Body_Body"
    if os.environ.get("DEVMAP_MOTION"): body["MotionType"] = Value(body["MotionType"].t, int(os.environ["DEVMAP_MOTION"]))
    files["Phive/RigidBodyEntityParam/%s_Body.phive__RigidBodyEntityParam.bgyml" % name] = w(body)
    shape = byml.read(chest["Phive/ShapeParam/Obj_TBox_A_01__Body_0.phive__ShapeParam.bgyml"])
    sx, sy, sz = size
    lo, hi = (-sx / 2, -sy, -sz / 2), (sx / 2, 0.0, sz / 2)
    vol = sx * sy * sz
    calc = {"Axis": {"X": f(0), "Y": f(0), "Z": f(0)}, "Center": {"X": f(0), "Y": f(-sy / 2), "Z": f(0)},
            "Max": {"X": f(hi[0]), "Y": f(hi[1]), "Z": f(hi[2])}, "Min": {"X": f(lo[0]), "Y": f(lo[1]), "Z": f(lo[2])},
            "Tensor": {"X": f((sy * sy + sz * sz) / 12), "Y": f((sx * sx + sz * sz) / 12), "Z": f((sx * sx + sy * sy) / 12)},
            "Volume": f(vol)}
    poly = shape["Polytope"][0]
    poly["AutoCalc"] = copy.deepcopy(calc)
    poly["MaterialPresets"] = ["Material_Stone"]
    poly["OffsetTranslation"] = {"X": f(0), "Y": f(0), "Z": f(0)}
    poly["Vertices"] = [{"X": f(x), "Y": f(y), "Z": f(z)} for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
    shape["AutoCalc"] = calc
    shape["Polytope"] = [poly]
    files["Phive/ShapeParam/%s__Body.phive__ShapeParam.bgyml" % name] = w(shape)
    data = sarc.write(files)
    pack = "Pack/Actor/%s.pack.zs" % name
    d = m.dictionary("Pack/Actor/%s.pack.zs" % model)
    c = m.z.ZstdCompressor(level=19, dict_data=d) if d else m.z.ZstdCompressor(level=19)
    open(m.dest(pack), "wb").write(c.compress(data))
    m.sizes[pack[:-3]] = len(data) * 4 + 16384
    info = m.edited.get("RSDB/ActorInfo.Product.111.rstbl.byml.zs") or byml.read(m.g.read("RSDB/ActorInfo.Product.111.rstbl.byml.zs"))
    row = copy.deepcopy(next(e for e in info if e["__RowId"] == "Obj_TBox_A_01_NoBake"))
    base = next(e for e in info if e["__RowId"] == model)
    for k in ("ActorName", "AnimationResources", "ELinkUserName", "SLinkUserName", "ApplyScaleType"): row.pop(k, None)
    for k in ("FmdbName", "ModelProjectName", "ModelAabbMax", "ModelAabbMin"): row[k] = copy.deepcopy(base[k])
    row["__RowId"] = name
    info[:] = sorted([e for e in info if e["__RowId"] != name] + [row], key=lambda e: e["__RowId"].encode())
    m.edited["RSDB/ActorInfo.Product.111.rstbl.byml.zs"] = info
    game = m.edited.get("RSDB/GameActorInfo.Product.111.rstbl.byml.zs") or byml.read(m.g.read("RSDB/GameActorInfo.Product.111.rstbl.byml.zs"))
    game[:] = sorted([e for e in game if e["__RowId"] != name] + [{"MainRigidBodyLayer": "Object", "__RowId": name}],
                     key=lambda e: e["__RowId"].encode())
    m.edited["RSDB/GameActorInfo.Product.111.rstbl.byml.zs"] = game




def chest_box(m, name, size, model=None):
    """research: a chest's whole physics (it collides; renamed copies of it do too, 17 Nov), its body's shape a box of
    size (x, y, z) metres, its top at the origin; drawn as the chest, or with another actor's model (model)"""
    own_actor(m, name, "Obj_TBox_A_01_NoBake")
    pack = "Pack/Actor/%s.pack.zs" % name
    files = sarc.read(m.z.ZstdDecompressor(dict_data=m.dictionary("Pack/Actor/Obj_TBox_A_01_NoBake.pack.zs")).decompress(
        open(m.dest(pack), "rb").read(), max_output_size=1 << 28))
    f = Value.float
    sx, sy, sz = size
    lo, hi = (-sx / 2, -sy, -sz / 2), (sx / 2, 0.0, sz / 2)
    mode = os.environ.get("DEVMAP_BOXMODE", "box")
    if mode == "fit":   # (the chest's own body box, its corners in their order, scaled to size, its top at the origin)
        k = "Phive/ShapeParam/Obj_TBox_A_01__Body_0.phive__ShapeParam.bgyml"
        t = byml.read(files[k])
        poly = t["Polytope"][0]
        vs = [[v[c].get() for c in "XYZ"] for v in poly["Vertices"]]
        off = [poly["OffsetTranslation"][c].get() for c in "XYZ"]
        lo = [min(v[i] for v in vs) for i in range(3)]
        hi = [max(v[i] for v in vs) for i in range(3)]
        want_lo, want_hi = (-sx / 2, -sy, -sz / 2), (sx / 2, 0.0, sz / 2)
        for v, raw in zip(poly["Vertices"], vs):
            for i, c in enumerate("XYZ"):
                u = (raw[i] - lo[i]) / max(hi[i] - lo[i], 1e-6)
                v[c] = f(want_lo[i] + u * (want_hi[i] - want_lo[i]))
        poly["OffsetTranslation"] = {"X": f(0), "Y": f(0), "Z": f(0)}
        for calc in (poly["AutoCalc"], t["AutoCalc"]):
            calc["Min"] = {"X": f(want_lo[0]), "Y": f(want_lo[1]), "Z": f(want_lo[2])}
            calc["Max"] = {"X": f(want_hi[0]), "Y": f(want_hi[1]), "Z": f(want_hi[2])}
            calc["Center"] = {"X": f(0), "Y": f(-sy / 2), "Z": f(0)}
        files[k] = byml.write(t)
        # (characters collide with the GroundObject layer (the chest's closed body and air wall), not Object (its main body))
        k = "Phive/RigidBodyEntityParam/Obj_TBox_Body_0.phive__RigidBodyEntityParam.bgyml"
        t = byml.read(files[k])
        t["LayerEntity"] = "GroundObject"
        t["MotionType"] = Value(t["MotionType"].t, 3498399415)   # (Static)
        files[k] = byml.write(t)
    if mode == "scale":   # (the chest's own body shape, its corners 1 % further out: does any edit break it?)
        k = "Phive/ShapeParam/Obj_TBox_A_01__Body_0.phive__ShapeParam.bgyml"
        t = byml.read(files[k])
        for v in t["Polytope"][0]["Vertices"]:
            for c in "XYZ": v[c] = f(v[c].get() * 1.01)
        files[k] = byml.write(t)
    if mode == "box":   # (the body's shape a Box entry, as the chest's sensor is; its other shapes left as they were)
        k = "Phive/ShapeParam/Obj_TBox_A_01__Body_0.phive__ShapeParam.bgyml"
        t = byml.read(files[k])
        sensor = byml.read(files["Phive/ShapeParam/Obj_TBox_A_01__Sensor.phive__ShapeParam.bgyml"])
        bx = copy.deepcopy(sensor["Box"][0])
        bx["Center"] = {"X": f(0), "Y": f(-sy / 2), "Z": f(0)}
        bx["HalfExtents"] = {"X": f(sx / 2), "Y": f(sy / 2), "Z": f(sz / 2)}
        bx["MaterialPresets"] = ["Material_Stone"]
        t.pop("Polytope", None)
        t["Box"] = [bx]
        files[k] = byml.write(t)
    for k in list(files) if mode == "poly" else []:
        if k.startswith("Phive/ShapeParam/Obj_TBox_A_01__Body") or k.startswith("Phive/ShapeParam/Obj_TBox_A_01__Close"):
            t = byml.read(files[k])
            for poly in t.get("Polytope", []):
                poly["OffsetTranslation"] = {"X": f(0), "Y": f(0), "Z": f(0)}
                poly["Vertices"] = [{"X": f(x), "Y": f(y), "Z": f(z)} for x in (lo[0], hi[0]) for y in (lo[1], hi[1])
                                    for z in (lo[2], hi[2])]
                for calc in (poly.get("AutoCalc"), t.get("AutoCalc")):
                    if calc:
                        calc["Min"] = {"X": f(lo[0]), "Y": f(lo[1]), "Z": f(lo[2])}
                        calc["Max"] = {"X": f(hi[0]), "Y": f(hi[1]), "Z": f(hi[2])}
                        calc["Center"] = {"X": f(0), "Y": f(-sy / 2), "Z": f(0)}
                        calc["Volume"] = f(sx * sy * sz)
            files[k] = byml.write(t)
    for k in list(files):
        if model and k.startswith("Component/ModelInfo/"):
            files[k] = byml.write({"FmdbName": model[0], "ModelProjectName": model[1]})
    data = sarc.write(files)
    d = m.dictionary("Pack/Actor/Obj_TBox_A_01_NoBake.pack.zs")
    open(m.dest(pack), "wb").write(m.z.ZstdCompressor(level=19, dict_data=d).compress(data))
    m.sizes[pack[:-3]] = max(m.sizes.get(pack[:-3], 0), len(data) * 2 + 16384)


def resized_box(m, name, base, size):
    """research: an object of the game's whose collision is Box shapes (a push block), renamed, every Box resized to size
    (x, y, z) metres, its bottom at the origin, centred across"""
    own_actor(m, name, base)
    pack = "Pack/Actor/%s.pack.zs" % name
    d = m.dictionary("Pack/Actor/%s.pack.zs" % base)
    files = sarc.read(m.z.ZstdDecompressor(dict_data=d).decompress(open(m.dest(pack), "rb").read(), max_output_size=1 << 28))
    f = Value.float
    sx, sy, sz = size
    for k in list(files):
        if "/ShapeParam/" not in k: continue
        t = byml.read(files[k])
        for bx in t.get("Box", []):
            bx["Center"] = {"X": f(0), "Y": f(sy / 2), "Z": f(0)}
            bx["HalfExtents"] = {"X": f(sx / 2), "Y": f(sy / 2), "Z": f(sz / 2)}
            bx["OffsetTranslation"] = {"X": f(0), "Y": f(0), "Z": f(0)}
        for calc in [t.get("AutoCalc")] + [b.get("AutoCalc") for b in t.get("Box", [])]:
            if calc:
                calc["Min"] = {"X": f(-sx / 2), "Y": f(0), "Z": f(-sz / 2)}
                calc["Max"] = {"X": f(sx / 2), "Y": f(sy), "Z": f(sz / 2)}
                calc["Center"] = {"X": f(0), "Y": f(sy / 2), "Z": f(0)}
                calc["Volume"] = f(sx * sy * sz)
        files[k] = byml.write(t)
    data = sarc.write(files)
    open(m.dest(pack), "wb").write(m.z.ZstdCompressor(level=19, dict_data=d).compress(data))
    m.sizes[pack[:-3]] = max(m.sizes.get(pack[:-3], 0), len(data) * 2 + 16384)


def fixed_copy(m, name, base, motion=3498399415):
    """research: an object renamed (own_actor), every rigid body of the Ground layers made static (motion: Static's hash;
    Fixed 4095225777): a push block that does not move. Only the bodies' settings change, never their shapes (a shape
    edited, the object stops colliding: resized_box); the size comes from the placement's scale, which the game applies
    to the collision too (found 17 Nov: a push block placed at scale 2 x 0.12 x 2 was a slab Link stepped onto)."""
    own_actor(m, name, base)
    pack = "Pack/Actor/%s.pack.zs" % name
    d = m.dictionary("Pack/Actor/%s.pack.zs" % base)
    files = sarc.read(m.z.ZstdDecompressor(dict_data=d).decompress(open(m.dest(pack), "rb").read(), max_output_size=1 << 28))
    for k in list(files):
        if "/RigidBodyEntityParam/" not in k: continue
        t = byml.read(files[k])
        if "MotionType" in t:
            t["MotionType"] = Value(t["MotionType"].t, motion)
            files[k] = byml.write(t)
    data = sarc.write(files)
    open(m.dest(pack), "wb").write(m.z.ZstdCompressor(level=19, dict_data=d).compress(data))
    m.sizes[pack[:-3]] = max(m.sizes.get(pack[:-3], 0), len(data) * 2 + 16384)


def unseen(m, name):
    """research: an actor of ours (already made, m.dest) drawn with no model: its model info emptied"""
    pack = "Pack/Actor/%s.pack.zs" % name
    d = m.dictionary("Pack/Actor/Obj_MovableBox_A_05.pack.zs")
    files = sarc.read(m.z.ZstdDecompressor(dict_data=d).decompress(open(m.dest(pack), "rb").read(), max_output_size=1 << 28))
    for k in list(files):
        if k.startswith("Component/ModelInfo/"):
            files[k] = byml.write({"FmdbName": "", "ModelProjectName": ""})
    data = sarc.write(files)
    open(m.dest(pack), "wb").write(m.z.ZstdCompressor(level=19, dict_data=d).compress(data))


MESH_TEMPLATE = "Obj_Common_Bed_A_01"   # (a plain object whose collision is a mesh shape file of its
# own: PhysicsParam -> ControllerSetParam (Ptm_FldObjStatic_01, static) -> ShapeParam PhshMesh -> Phive/Shape/Dcc/*.bphsh.
# Not Obj_InsideGanonCastle_SageRoomTube_A_01: an on/off object, it did not collide placed elsewhere, headless 17 Nov)


def mesh_twin(m, name, piece, aabb=None, template=MESH_TEMPLATE, shown=False):
    """research (17 Nov): a piece's own collision, solid wherever placed. The game keeps most pieces' collision meshes as
    files of their own (Phive/Shape/Dcc/<piece>__Physical.bphsh), which its tools bake into a map's compound; an object
    of the game's (template) collides through such a file named in its ShapeParam. Ours: that object renamed (own_actor),
    its pack's own files renamed with it (so the real one's are not confused with ours: the game keeps resources by
    path), its ShapeParam naming the piece's mesh instead, and no model (an invisible twin: placed on the piece, at its
    place, turn and scale, the piece becomes solid). The shape file itself is the game's, unchanged: shapes edited stop
    colliding (resized_box). aabb: the piece's box ((min x, y, z), (max x, y, z)) for the shape's bounds.
    NOT WORKING YET (headless 17 Nov, the layout's "solid"): twins of the tube and of a bed (Obj_Common_Bed_A_01, its own
    shape or Obj_IronKnuckle_Pillar_A_01's, shown or not) did not stop Link in the Dinolfos arena; but the real bed placed
    there did not either, so the template may be what fails (not spawned or not solid where placed), not the shape swap.
    Also found: Land_SageRoom_Base_A_01__Physical is 5.7 KB, the spawn pedestal only (the Sages' room never had a floor);
    and in a clone of SageRoom even VrBlockInvisible (proven elsewhere) did not hold Link: placed objects there do not
    collide, cause unknown."""
    own_actor(m, name, template)
    pack = "Pack/Actor/%s.pack.zs" % name
    d = m.dictionary("Pack/Actor/%s.pack.zs" % template)
    files = sarc.read(m.z.ZstdDecompressor(dict_data=d).decompress(open(m.dest(pack), "rb").read(), max_output_size=1 << 28))
    stems = {}   # (the pack's files named for the template, by their path without the extension: renamed for ours)
    for k in list(files):
        if template in k and not k.startswith("Actor/"):
            stem = k.rsplit(".", 2)[0] if k.endswith(".bgyml") else k
            stems[stem.split(".")[0]] = stem.split(".")[0].replace(template, name)
            files[k.replace(template, name)] = files.pop(k)

    def swap(x):
        if isinstance(x, dict): return {k: swap(v) for k, v in x.items()}
        if isinstance(x, list): return [swap(v) for v in x]
        if isinstance(x, str):
            for old, new in stems.items():
                if old in x: x = x.replace(old, new)
        return x

    f = Value.float
    for k in list(files):
        if not k.endswith(".bgyml"): continue
        t = swap(byml.read(files[k]))
        if k.startswith("Phive/ShapeParam/"):
            t["PhshMesh"] = [{"PhshMeshPath": "Work/Phive/Shape/Dcc/%s__Physical.phsh" % piece}]
            if aabb and t.get("AutoCalc"):
                lo, hi = aabb
                t["AutoCalc"]["Min"] = {"X": f(lo[0]), "Y": f(lo[1]), "Z": f(lo[2])}
                t["AutoCalc"]["Max"] = {"X": f(hi[0]), "Y": f(hi[1]), "Z": f(hi[2])}
        if k.startswith("Component/ModelInfo/") and not shown:
            t = {"FmdbName": "", "ModelProjectName": ""}
        files[k] = byml.write(t)
    data = sarc.write(files)
    open(m.dest(pack), "wb").write(m.z.ZstdCompressor(level=19, dict_data=d).compress(data))
    m.sizes[pack[:-3]] = max(m.sizes.get(pack[:-3], 0), len(data) * 2 + 16384)
