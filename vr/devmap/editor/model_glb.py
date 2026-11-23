"""The dev map editor's models: an actor's model from YOUR copy of the game, as a glTF binary the editor draws.

Read from the model file (BFRES v10) on request and kept in memory only: positions, normals, UVs and triangles of each
shape's most detailed mesh (its first level of detail), placed by its bone, with its material's colour texture
(textures.py; FMAT +0x20 its texture names, the one ending _Alb).
What a BFRES v10 file holds, as read here (checked on Land_DeathMtTrail_Base_A_12, 17 Nov):
- the relocation table (_RLT, at the header's 0x18) lists sections; section 1 starts the buffer data (indices, then vertices);
- FVTX: 0x08 its attributes (0x10 each: name, format u16 at +8, offset u16 at +12, buffer u16 at +14), 0x30 buffer sizes and
  0x38 strides (0x10 each), 0x48 its data's offset from the buffer data, 0x4C attribute and buffer counts, 0x50 vertices;
- FSHP: 0x08 name, 0x10 its FVTX, 0x54 its bone, 0x5A its skin count, 0x18 its meshes (0x38 each: +0x20 index data offset, +0x24 primitive (3: triangles),
  +0x28 index format (0 u8, 1 u16, 2 u32), +0x2C index count, +0x30 first vertex);
- attribute formats: the high byte the layout (0x15 four 16-bit, 0x0E 10-10-10-2, 0x12 two 16-bit, 0x18 three 32-bit, ...),
  the low byte its kind (1 unorm, 2 snorm, 3 uint, 4 sint, 5 float).
"""
import json
import struct
import threading

import numpy as np

LAYOUTS = {0x02: (1, 8), 0x09: (2, 8), 0x0B: (4, 8), 0x0E: (4, 1010102), 0x10: (1, 16), 0x12: (2, 16), 0x15: (4, 16),
           0x14: (1, 32), 0x17: (2, 32), 0x18: (3, 32), 0x19: (4, 32)}


def decode(raw, count, stride, offset, fmt):
    """an attribute's values (count x components, float32), from interleaved vertex data"""
    layout, kind = fmt >> 8, fmt & 0xff
    comps, bits = LAYOUTS[layout]
    rows = np.frombuffer(raw, dtype=np.uint8, count=count * stride).reshape(count, stride)
    if bits == 1010102:
        w = rows[:, offset:offset + 4].copy().view("<u4").reshape(count).astype(np.int64)
        out = np.zeros((count, 4), np.float32)
        for k, (sh, n) in enumerate(((0, 10), (10, 10), (20, 10), (30, 2))):
            v = (w >> sh) & ((1 << n) - 1)
            if kind == 2:
                v = np.where(v >= (1 << (n - 1)), v - (1 << n), v)
                out[:, k] = np.maximum(v / ((1 << (n - 1)) - 1), -1)
            else:
                out[:, k] = v / ((1 << n) - 1)
        return out
    size = comps * bits // 8
    chunk = rows[:, offset:offset + size].copy()
    if kind == 5:
        dt = {16: "<f2", 32: "<f4"}[bits]
        return chunk.view(dt).reshape(count, comps).astype(np.float32)
    dt = {(8, 1): "u1", (8, 2): "i1", (8, 3): "u1", (8, 4): "i1", (16, 1): "<u2", (16, 2): "<i2", (16, 3): "<u2",
          (16, 4): "<i2", (32, 3): "<u4", (32, 4): "<i4"}[(bits, kind)]
    v = chunk.view(dt).reshape(count, comps).astype(np.float32)
    if kind == 1: v /= float((1 << bits) - 1)
    if kind == 2: v = np.maximum(v / float((1 << (bits - 1)) - 1), -1)
    return v


def quat_matrix(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def bone_worlds(d, u64):
    """each bone's model-space matrix (4x4), from its local scale, rotation (a quaternion here) and position"""
    k = d.find(b"FSKL")
    if k < 0: return []
    flags = struct.unpack_from("<I", d, k + 4)[0]
    bone_arr, m2b = u64(k + 0x10), u64(k + 0x18)
    nbone = struct.unpack_from("<H", d, k + 0x38)[0]
    size = 0x58   # (a bone's record, BFRES v10; props have no palette (m2b 0) to measure it by)
    worlds = []
    for i in range(nbone):
        b = bone_arr + i * size
        parent = struct.unpack_from("<h", d, b + 0x22)[0]
        s = struct.unpack_from("<3f", d, b + 0x30)
        r = struct.unpack_from("<4f", d, b + 0x3C)
        p = struct.unpack_from("<3f", d, b + 0x4C)
        m = np.eye(4)
        if (flags >> 12) & 3 == 0:   # (rotation as a quaternion; flags 0x1000: Euler XYZ, radians, as every model checked)
            m[:3, :3] = quat_matrix(r)
        else:
            cx, sx, cy, sy, cz, sz = np.cos(r[0]), np.sin(r[0]), np.cos(r[1]), np.sin(r[1]), np.cos(r[2]), np.sin(r[2])
            rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
            ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
            rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
            m[:3, :3] = rz @ ry @ rx
        m[:3, :3] = m[:3, :3] * np.array(s)
        m[:3, 3] = p
        worlds.append(worlds[parent] @ m if 0 <= parent < len(worlds) else m)
    return worlds


# (a material is water or lava when one of its name's words is (Mt_Water_Front, Mt_WaterVolume_Surface, Mt_DgnObjLava):
# not when water is only part of a word (Mt_Rock_WaterTemple_Wall_A, Mt_Stone_Kakariko_Waterway_A), nor when another word
# names it as solid (Land_GerudoValley_Waterfall_ground_A_01_rock_B, Mt_Lava_DeathMtCrater_Hardened_A))
WATER_WORDS = {"water", "watervolume", "watervolumesurface", "waterfall", "waterplane"}
LAVA_WORDS = {"lava", "lavaplane", "dgnobjlava", "magma"}
SOLID_WORDS = {"rock", "stone", "sand", "wood", "metal", "terracotta", "ground", "guardrail", "moss", "plant", "tree",
               "hardened", "cool", "head", "crackwall", "rollingfloor", "rollingrock", "item"}


def fluid(material):
    words = set(material.lower().split("_"))
    if words & SOLID_WORDS: return None
    if words & LAVA_WORDS: return "lava"
    if words & WATER_WORDS: return "water"
    return None


def meshes(d):
    """[(positions Nx3, normals Nx3, triangles Mx3, uvs Nx2 or None, its material's colour texture name or None)] for each
    shape's first mesh, in model space"""
    u64 = lambda o: struct.unpack_from("<Q", d, o)[0]
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    u16 = lambda o: struct.unpack_from("<H", d, o)[0]

    def name(o):
        n = u16(o)
        return d[o + 2:o + 2 + n].decode(errors="replace")

    albedo, kinds = [], []   # (each material's colour texture (FMAT +0x20 its texture names, +0xA2 their count), and
    # whether it is water or lava by its name (the editor draws those see-through))
    k = d.find(b"FMAT")
    while k >= 0:
        names = []
        try:
            for j in range(d[k + 0xA2]): names.append(name(u64(u64(k + 0x20) + 8 * j)))
        except Exception:
            names = []
        albedo.append(next((n for n in names if n.endswith("_Alb")), names[0] if names else None))
        kinds.append(fluid(name(u64(k + 8)) or ""))
        k = d.find(b"FMAT", k + 4)
    rlt = u32(0x18)
    assert d[rlt:rlt + 4] == b"_RLT"
    buffers = u32(rlt + 0x10 + 0x18 + 8)   # (section 1's start)
    worlds = bone_worlds(d, u64)
    k = d.find(b"FSKL")
    m2b = []
    if k >= 0:
        nsmooth, nrigid = struct.unpack_from("<2H", d, k + 0x3A)
        m2b = list(struct.unpack_from("<%dH" % (nsmooth + nrigid), d, u64(k + 0x18)))
    out = []
    i = d.find(b"FSHP")
    while i >= 0:
        vtx, mesh = u64(i + 0x10), u64(i + 0x18)
        if not 0 < d[vtx + 0x4C] <= 32 or not 0 < d[vtx + 0x4D] <= 16:   # (its "FVTX" tag may be zeroed: its counts tell)
            i = d.find(b"FSHP", i + 4)
            continue
        bone, skin = u16(i + 0x54), d[i + 0x5A]
        nattr, count = d[vtx + 0x4C], u32(vtx + 0x50)
        data_at = buffers + u32(vtx + 0x48)
        nbuf = d[vtx + 0x4D]
        sizes = [u32(u64(vtx + 0x30) + 16 * k) for k in range(nbuf)]
        strides = [u32(u64(vtx + 0x38) + 16 * k) for k in range(nbuf)]
        starts = [data_at + sum(((s + 7) // 8) * 8 for s in sizes[:k]) for k in range(nbuf)]
        attrs = {}
        for k in range(nattr):
            a = u64(vtx + 8) + 0x10 * k
            attrs[name(u64(a))] = (u16(a + 8), u16(a + 12), u16(a + 14))
        if "_p0" not in attrs:
            i = d.find(b"FSHP", i + 4)
            continue
        fmt, off, b = attrs["_p0"]
        pos = decode(d[starts[b]:starts[b] + sizes[b]], count, strides[b], off, fmt)[:, :3]
        uv = None
        if "_u0" in attrs:
            fmt, off, b = attrs["_u0"]
            uv = decode(d[starts[b]:starts[b] + sizes[b]], count, strides[b], off, fmt)[:, :2]
        if "_n0" in attrs:
            fmt, off, b = attrs["_n0"]
            nrm = decode(d[starts[b]:starts[b] + sizes[b]], count, strides[b], off, fmt)[:, :3]
        else:
            nrm = np.tile(np.array([0, 1, 0], np.float32), (count, 1))
        bake = None   # (where the shape's surface lies in its map's baked light atlas: the game's bake files place each
        # material of each placed piece in an atlas by a scale and an offset of these, server.py bake())
        if "bake" in attrs:
            fmt, off, b = attrs["bake"]
            bake = decode(d[starts[b]:starts[b] + sizes[b]], count, strides[b], off, fmt)[:, :2]
        face, prim, ifmt, icount, first = struct.unpack_from("<5I", d, mesh + 0x20)
        if prim != 3 or ifmt not in (0, 1, 2) or icount == 0:
            i = d.find(b"FSHP", i + 4)
            continue
        dt = ("u1", "<u2", "<u4")[ifmt]
        idx = np.frombuffer(d, dtype=dt, count=icount, offset=buffers + face).astype(np.uint32) + first
        idx = idx[: icount - icount % 3].reshape(-1, 3)
        idx = idx[(idx < count).all(axis=1)]
        if skin == 0 and 0 <= bone < len(worlds):   # (rigid on the shape's bone)
            w = worlds[bone]
            pos = pos @ w[:3, :3].T + w[:3, 3]
            nrm = nrm @ w[:3, :3].T
        elif skin == 1 and "_i0" in attrs and m2b:   # (each vertex on one bone, through the palette)
            fmt, off, b = attrs["_i0"]
            which = decode(d[starts[b]:starts[b] + sizes[b]], count, strides[b], off, fmt)[:, 0].astype(np.int64)
            if (fmt & 0xff) in (1, 2): which = np.rint(which * 255).astype(np.int64)
            for slot in np.unique(which):
                if not 0 <= slot < len(m2b) or not 0 <= m2b[slot] < len(worlds): continue
                w, sel = worlds[m2b[slot]], which == slot
                pos[sel] = pos[sel] @ w[:3, :3].T + w[:3, 3]
                nrm[sel] = nrm[sel] @ w[:3, :3].T
        n = np.linalg.norm(nrm, axis=1, keepdims=True)
        nrm = np.where(n > 1e-6, nrm / np.maximum(n, 1e-6), np.array([0, 1, 0], np.float32))
        mat = u16(i + 0x52)
        out.append((pos.astype(np.float32), nrm.astype(np.float32), idx,
                    None if uv is None else uv.astype(np.float32), albedo[mat] if mat < len(albedo) else None,
                    kinds[mat] if mat < len(kinds) else None, None if bake is None else bake.astype(np.float32), mat))
        i = d.find(b"FSHP", i + 4)
    return out


def glb(parts, texture=None):
    """a glTF binary: one mesh, a primitive per part; texture(name) -> PNG bytes or None: each part's colour texture"""
    blob = bytearray()
    views, accessors, prims, materials, images = [], [], [], [], []
    image_of, material_of = {}, {}

    def view(data, target=None):
        while len(blob) % 4: blob.append(0)
        v = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if target: v["target"] = target
        views.append(v)
        blob.extend(data)
        return len(views) - 1

    def add(arr, target, kind, comps):
        acc = {"bufferView": view(arr.tobytes(), target), "componentType": kind, "count": arr.shape[0], "type": comps}
        if comps == "VEC3" and kind == 5126:
            acc["min"] = arr.min(axis=0).tolist()
            acc["max"] = arr.max(axis=0).tolist()
        accessors.append(acc)
        return len(accessors) - 1

    def material(tex):
        if tex in material_of: return material_of[tex]
        png = texture(tex) if (texture and tex) else None
        m = {"pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.8, 0.8, 1], "metallicFactor": 0, "roughnessFactor": 0.9},
             "doubleSided": True}
        if png:
            if tex not in image_of:
                images.append({"bufferView": view(png), "mimeType": "image/png"})
                image_of[tex] = len(images) - 1
            m["pbrMetallicRoughness"]["baseColorFactor"] = [1, 1, 1, 1]
            m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": image_of[tex]}
            m["extras"] = {"textured": True}
        materials.append(m)
        material_of[tex] = len(materials) - 1
        return material_of[tex]

    for pos, nrm, tri, uv, tex, kind, bake, mat in parts:
        if not len(tri): continue
        attrs = {"POSITION": add(np.ascontiguousarray(pos), 34962, 5126, "VEC3"),
                 "NORMAL": add(np.ascontiguousarray(nrm), 34962, 5126, "VEC3")}
        if uv is not None: attrs["TEXCOORD_0"] = add(np.ascontiguousarray(uv), 34962, 5126, "VEC2")
        if bake is not None: attrs["TEXCOORD_1"] = add(np.ascontiguousarray(bake), 34962, 5126, "VEC2")
        ix = add(np.ascontiguousarray(tri.reshape(-1)), 34963, 5125, "SCALAR")
        mi = material(tex if uv is not None else None)
        if kind:   # (water or lava: a material of its own, marked for the editor)
            m = json.loads(json.dumps(materials[mi]))
            m["extras"] = dict(m.get("extras", {}), kind=kind)
            materials.append(m)
            mi = len(materials) - 1
        prims.append({"attributes": attrs, "indices": ix, "material": mi, "extras": {"mat": mat}})
    if not prims: return None
    doc = {"asset": {"version": "2.0", "generator": "Hookshot VR dev map editor"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"mesh": 0}], "meshes": [{"primitives": prims}], "materials": materials,
           "buffers": [{"byteLength": len(blob)}], "bufferViews": views, "accessors": accessors}
    if images:
        doc["images"] = images
        doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
        doc["textures"] = [{"source": k, "sampler": 0} for k in range(len(images))]
        for m in materials:
            t = m["pbrMetallicRoughness"].get("baseColorTexture")
            if t: t["index"] = t["index"]   # (texture k shows image k)
    js = json.dumps(doc).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    while len(blob) % 4: blob.append(0)
    total = 12 + 8 + len(js) + 8 + len(blob)
    return (struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(js), 0x4E4F534A) + js +
            struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


class Models:
    def __init__(self, game, cat):
        self.g = game
        self.project = {}
        for e in cat["actors"].values():
            if e.get("FmdbName") and e.get("ModelProjectName"): self.project[e["FmdbName"]] = e["ModelProjectName"]
        self.cache = {}
        self.textures = {}
        self.lock = threading.Lock()

    def atlas(self, name):
        """a baked light atlas (Tex/RBake_Scene_*.bntx.zs) as PNG bytes, larger than a colour texture: its pieces are small"""
        key = "atlas:" + name
        with self.lock:
            if key in self.textures: return self.textures[key]
        png = None
        rel = "Tex/%s.bntx.zs" % name
        if name.startswith("RBake_Scene_") and self.g.exists(rel):
            try:
                import textures
                png = textures.decode(self.g.read(rel), size=2048)
            except Exception as e:
                print("dev map editor: atlas %s not read: %s" % (name, e))
        with self.lock:
            self.textures[key] = png
        return png

    def texture(self, name):
        """a colour texture as PNG bytes (Tex/<name>.bntx.zs), kept; None when there is none or its format is not read"""
        with self.lock:
            if name in self.textures: return self.textures[name]
        png = None
        rel = "Tex/%s.bntx.zs" % name
        if self.g.exists(rel):
            try:
                import textures
                png = textures.decode(self.g.read(rel))
            except Exception as e:
                print("dev map editor: texture %s not read: %s" % (name, e))
        with self.lock:
            if len(self.textures) > 600: self.textures.clear()
            self.textures[name] = png
        return png

    def glb(self, fmdb):
        with self.lock:
            if fmdb in self.cache: return self.cache[fmdb]
        data = None
        proj = self.project.get(fmdb)
        rel = "Model/%s.%s.bfres.zs" % (proj, fmdb) if proj else None
        if rel and self.g.exists(rel):
            try:
                data = glb(meshes(self.g.read(rel)), self.texture)
            except Exception as e:
                print("dev map editor: model %s not read: %s" % (fmdb, e))
        with self.lock:
            if len(self.cache) > 400: self.cache.clear()
            self.cache[fmdb] = data
        return data
