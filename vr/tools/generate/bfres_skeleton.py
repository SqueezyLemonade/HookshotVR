"""A model's skeleton and shape list, read from a BFRES v10 (Switch) model file.

Returns plain numbers only: per bone its name, parent, palette slots, flags and local scale/rotation/position; the
palette's matrix-to-bone list; the inverse bind matrices (3x4, row major); and per shape its name, vertex buffer 0 size,
stride, vertex count, skin count and the index count of each level of detail. No vertex, index or texture data.
"""
import struct


def parse(d):
    u64 = lambda o: struct.unpack_from("<Q", d, o)[0]

    def name(o):
        n = struct.unpack_from("<H", d, o)[0]
        return d[o + 2:o + 2 + n].decode()

    # The skeleton (FSKL): bone array, palette (matrix -> bone), inverse binds, then the counts
    k = d.find(b"FSKL")
    if k < 0:
        raise ValueError("no skeleton (FSKL) in this model")
    flags = struct.unpack_from("<I", d, k + 4)[0]
    bone_arr, m2b, inv = u64(k + 0x10), u64(k + 0x18), u64(k + 0x20)
    nbone, nsmooth, nrigid = struct.unpack_from("<3H", d, k + 0x38)
    size = (m2b - bone_arr) // nbone
    bones = []
    for i in range(nbone):
        b = bone_arr + i * size
        _, parent, smooth, rigid, _, _ = struct.unpack_from("<HhhhHH", d, b + 0x20)
        bones.append({"i": i, "name": name(u64(b)), "parent": parent, "smooth": smooth, "rigid": rigid,
                      "flags": struct.unpack_from("<I", d, b + 0x2C)[0],
                      "scale": struct.unpack_from("<3f", d, b + 0x30),
                      "rot": struct.unpack_from("<4f", d, b + 0x3C),
                      "pos": struct.unpack_from("<3f", d, b + 0x4C)})
    matrix_to_bone = list(struct.unpack_from("<%dH" % (nsmooth + nrigid), d, m2b))
    inv_bind = [list(struct.unpack_from("<12f", d, inv + 48 * i)) for i in range(nsmooth)]

    # The shapes (FSHP): sizes and counts only, used to recognise a model's draws by their vertex buffer size
    shapes = []
    i = d.find(b"FSHP")
    while i >= 0:
        vtx, meshes = u64(i + 0x10), u64(i + 0x18)
        vb0 = struct.unpack_from("<I", d, u64(vtx + 0x30))[0]
        stride = struct.unpack_from("<I", d, u64(vtx + 0x38))[0]
        lods = []
        for j in range(8):   # (the meshes are contiguous; the first entry that is not a mesh ends the list)
            _, prim, fmt, count, _ = struct.unpack_from("<5I", d, meshes + j * 0x38 + 0x20)
            if prim not in (3, 4, 5) or fmt not in (0, 1, 2) or count == 0 or count > 5_000_000:
                break
            lods.append(count)
        shapes.append({"name": name(u64(i + 8)), "vb0": vb0, "stride": stride,
                       "vertices": struct.unpack_from("<I", d, vtx + 0x50)[0], "skin": d[vtx + 0x54], "lods": lods})
        i = d.find(b"FSHP", i + 4)
    return {"flags": flags, "nsmooth": nsmooth, "nrigid": nrigid, "bones": bones, "matrix_to_bone": matrix_to_bone,
            "inv_bind": inv_bind, "shapes": shapes}
