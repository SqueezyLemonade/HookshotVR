"""vk_vr_shield_grip.h: the shields' grips, moved across their backs for the left VR hand (vr/items/gear/shield_grip.inc).

The shields are made for Link's right forearm: the grip (the bar the fist holds, and its mounts) sits at one edge of the
plate's back (model x near 0), the plate and its strap reach from there along the forearm (+x, toward the elbow). Held in
the left hand, the placement mirrored, the plate reached forward past the fist instead; the model mirrored showed inside
out (headset 17 Nov). So for the left hand only the grip moves, sideways across the back to the plate's other edge (a
translation keeps every triangle's winding), and the placement moves by the opposite amount, so the grip stays in the
fingers and the plate and strap come to lie over the forearm.

Read from your game's files, per shape that has a grip: its vertex buffer's size, stride and vertex count, a fingerprint
of its bytes (the renderer moves the grip only in a buffer that matches it), which vertices are the grip's (in every
level of detail), how far they move and how far the placement moves back. No vertex data is kept: the renderer edits the
game's own buffer as it draws it.

The grip: pieces of connected triangles (vertices at the same position count as connected, so a UV seam does not split a
piece), each rigid to the shield's root (Skl_Root, full weight) and within 5 cm of the grip's axis (model x = 0) end to
end. The plate (its largest piece, rigid too) is mirror-symmetric across a line x = P: the grip moves to its mirror image
across that line (x -> x + 2 (P - c), c the grip's middle), so its mounts meet the shell where it had them, and the
placement moves by -2 P (the grip then lands where it was: in the palm). It goes up to 1.5 cm further out (and the
placement back as far), toward the right arm's room between the handle and the strap: mirrored, a strap not at the
plate's middle came nearer the grip (the Deku Shield's, 2.8 cm; headset 22 Nov: a bit more room).

The plate's back has the mounts' holes painted in its texture, which showed where the mounts had been (headset 22 Nov).
So the back's UV islands (those facing the arm; the front's, the emblem, are left alone) get the texture coordinates of
the place under their mirror image across the grip's new line, seen through the plate, their tangents mirrored too: the
holes come to lie under the moved mounts (to 4 mm, median 0, on every shield), the rest of the back is the same wood
mirrored. These values are worked out here, as numbers in the header, from your files.
"""
import struct

import numpy as np

OUTPUT = "vk_vr_shield_grip.h"
MODELS = ("Weapon_Shield_Deku", "Weapon_Shield_Hylian", "Weapon_Shield_Hylian_Dark", "Weapon_Shield_Mirror")
INPUTS = tuple("Model/%s.%s.bfres.zs" % (n, n) for n in MODELS)

GRIP_REACH = .05   # (m) every vertex of a grip piece this close to the grip's axis, model x = 0
POSITION_HALF4 = 0x515   # (the position attribute's format: half floats x4)
SNORM_10_10_10_2, UNORM_16_16 = 0x20e, 0x112   # (the normals' and tangents', the texture coordinates')
NUDGE_MOST = .015   # (m) the most the grip moves outward past its mirror image, toward the right arm's strap spacing


def shapes(data):
    """each shape's name, vertex buffer (bytes, stride, count), position/skin attribute offsets and its levels of detail
    (indices, first vertex). The level count is the shape's own (ten for the Deku and Hylian Shields)"""
    u64 = lambda o: struct.unpack_from("<Q", data, o)[0]
    pool = u64(u64(0xb0) + 8)   # (the file's buffer info: where vertex and index data start)

    def name(o):
        return data[o + 2:o + 2 + struct.unpack_from("<H", data, o)[0]].decode()
    out = []
    i = data.find(b"FSHP")
    while i >= 0:
        vtx, meshes, levels = u64(i + 0x10), u64(i + 0x18), data[i + 0x5B]
        offset, count = struct.unpack_from("<I", data, vtx + 0x48)[0], struct.unpack_from("<I", data, vtx + 0x50)[0]
        stride = struct.unpack_from("<I", data, u64(vtx + 0x38))[0]
        size = struct.unpack_from("<I", data, u64(vtx + 0x30))[0]
        attrs = {}
        for a in range(data[vtx + 0x4C]):
            at = u64(vtx + 8) + a * 0x10
            fmt = struct.unpack_from(">I", data, at + 8)[0] >> 16
            where, buffer = struct.unpack_from("<HH", data, at + 12)
            attrs[name(u64(at))] = (fmt, where, buffer)
        lods = []
        for j in range(levels):
            index_at, prim, fmt, n, first = struct.unpack_from("<5I", data, meshes + j * 0x38 + 0x20)
            if prim != 3 or fmt != 1:   # (triangles, 16-bit indices: as every shield stores them)
                lods = None
                break
            lods.append((np.frombuffer(data, dtype="<u2", count=n, offset=pool + index_at).astype(np.int64), first))
        out.append({"name": name(u64(i + 8)), "bytes": data[pool + offset:pool + offset + size], "stride": stride,
                    "vertices": count, "attrs": attrs, "lods": lods})
        i = data.find(b"FSHP", i + 4)
    return out


def pieces(keys, tris):
    """the connected pieces of these triangles (vertex numbers), vertices with the same position key joined"""
    parent = {}

    def find(a):
        parent.setdefault(a, a)
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def join(a, b):
        a, b = find(a), find(b)
        if a != b:
            parent[a] = b
    seen = {}
    for v in sorted(set(tris.reshape(-1).tolist())):
        find(v)
        if keys[v] in seen:
            join(v, seen[keys[v]])
        else:
            seen[keys[v]] = v
    for a, b, c in tris.tolist():
        join(a, b)
        join(b, c)
    groups = {}
    for v in parent:
        groups.setdefault(find(v), []).append(v)
    return sorted((sorted(g) for g in groups.values()), key=len, reverse=True)


def symmetry_line(p):
    """x = P across which these points (the plate) are most nearly their own mirror image, to half a millimetre"""
    q = p[::max(1, len(p) // 400)]
    mid = (p[:, 0].min() + p[:, 0].max()) / 2

    def miss(P):
        r = q.copy()
        r[:, 0] = 2 * P - r[:, 0]
        return np.sqrt(((r[:, None, :] - p[None, :, :]) ** 2).sum(-1).min(1)).mean()
    return min((mid + k * .0005 for k in range(-60, 61)), key=miss)


def fnv1a64(b):
    """the fingerprint the renderer takes of the buffer it draws (FNV-1a, 64-bit: vr/items/gear/shield_grip.inc)"""
    h = 0xcbf29ce484222325
    for k in np.frombuffer(b, dtype=np.uint8).tolist():
        h = ((h ^ k) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def snorm10(words):
    """10_10_10_2 snorm words (the normals' and tangents' format) as x, y, z, w"""
    w = words.astype(np.int64)
    out = np.zeros((len(w), 4))
    for k in range(3):
        v = (w >> (10 * k)) & 1023
        out[:, k] = np.where(v >= 512, v - 1024, v) / 511.0
    v = (w >> 30) & 3
    out[:, 3] = np.where(v >= 2, v - 4, v)
    return out


def to_snorm10(t):
    """x, y, z, w back into a 10_10_10_2 snorm word"""
    q = [int(round(np.clip(v, -1, 1) * 511)) & 1023 for v in t[:3]]
    return q[0] | q[1] << 10 | q[2] << 20 | (int(t[3]) & 3) << 30


def islands(tris):
    """the plate's UV islands: its triangles joined by the vertices they share (a seam's copies are other vertices)"""
    parent = {}

    def find(a):
        parent.setdefault(a, a)
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for t in tris.tolist():
        for v in t[1:]:
            a, b = find(t[0]), find(v)
            if a != b:
                parent[a] = b
    out = {}
    for t in tris:
        out.setdefault(find(int(t[0])), []).append(t)
    return [np.array(ts) for ts in out.values()]


def closest_on_triangles(m, A, B, C):
    """the point of these triangles nearest m: (which triangle, its barycentrics, the distance)"""
    e0, e1 = B - A, C - A
    n = np.cross(e0, e1)
    nn = np.maximum((n * n).sum(1), 1e-18)
    d = m - A
    d00, d01, d11 = (e0 * e0).sum(1), (e0 * e1).sum(1), (e1 * e1).sum(1)
    d20, d21 = (d * e0).sum(1), (d * e1).sum(1)
    den = np.maximum(d00 * d11 - d01 * d01, 1e-18)
    l1, l2 = (d11 * d20 - d01 * d21) / den, (d00 * d21 - d01 * d20) / den
    best = np.where((l1 >= 0) & (l2 >= 0) & (l1 + l2 <= 1), np.abs((d * n).sum(1)) / np.sqrt(nn), np.inf)
    bary = np.stack([1 - l1 - l2, l1, l2], 1)
    for i, j in ((0, 1), (1, 2), (2, 0)):   # (or on an edge)
        P0, P1 = (A, B, C)[i], (A, B, C)[j]
        e = P1 - P0
        t = np.clip(((m - P0) * e).sum(1) / np.maximum((e * e).sum(1), 1e-18), 0, 1)
        dist = np.sqrt((((P0 + t[:, None] * e) - m) ** 2).sum(1))
        better = dist < best
        best = np.where(better, dist, best)
        b = np.zeros_like(bary)
        b[:, i], b[:, j] = 1 - t, t
        bary = np.where(better[:, None], b, bary)
    k = int(np.argmin(best))
    return k, bary[k], float(best[k])


def under_in_xz(m, A, B, C):
    """the triangle under m seen through the plate (along y: a triangle containing m's x, z, the one nearest m in y), and
    its barycentrics; None when no triangle is under it (past the island's outline, or only its steep rim)"""
    det = (B[:, 2] - C[:, 2]) * (A[:, 0] - C[:, 0]) + (C[:, 0] - B[:, 0]) * (A[:, 2] - C[:, 2])
    flat = np.abs(det) > 1e-9
    det = np.where(flat, det, 1)
    l0 = ((B[:, 2] - C[:, 2]) * (m[0] - C[:, 0]) + (C[:, 0] - B[:, 0]) * (m[2] - C[:, 2])) / det
    l1 = ((C[:, 2] - A[:, 2]) * (m[0] - C[:, 0]) + (A[:, 0] - C[:, 0]) * (m[2] - C[:, 2])) / det
    l2 = 1 - l0 - l1
    inside = flat & (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
    if not inside.any():
        return None
    y = l0 * A[:, 1] + l1 * B[:, 1] + l2 * C[:, 1]
    k = int(np.argmin(np.where(inside, np.abs(y - m[1]), np.inf)))
    return k, np.array([l0[k], l1[k], l2[k]])


def mirrored_back(pos, normal, uv, tangent, tris, line):
    """the plate's back with its texture mirrored across x = line: each vertex of a UV island that faces the arm (its
    normals' mean -y) takes the texture coordinates and tangent of the place under its mirror image, seen through the
    plate (or, past the outline or on the steep rim, of the island's point nearest it), the tangent mirrored too. So
    what is painted on the back (the mounts' holes) moves with the grip; the front's islands (the emblem) are not touched.
    Returns ({vertex: (u, v, tangent)}, how many took the nearest point)"""
    out, nearest = {}, 0
    for ts in islands(tris):
        vs = np.unique(ts)
        if normal[vs, 1].mean() >= 0:
            continue
        A, B, C = pos[ts[:, 0]], pos[ts[:, 1]], pos[ts[:, 2]]
        for v in vs.tolist():
            m = pos[v].copy()
            m[0] = 2 * line - m[0]
            found = under_in_xz(m, A, B, C) if abs(normal[v, 1]) > .5 else None
            if found is None:
                nearest += 1
                k, w, _ = closest_on_triangles(m, A, B, C)
            else:
                k, w = found
            corners = ts[k]
            at_uv = (w[:, None] * uv[corners]).sum(0)
            at_t = (w[:, None] * tangent[corners, :3]).sum(0)
            sign = tangent[corners[int(np.argmax(w))], 3]
            at_t[0] = -at_t[0]   # (the tangent there, mirrored: u now runs the other way in x)
            at_t /= max(np.linalg.norm(at_t), 1e-9)
            q = np.clip(np.round(at_uv * 65535), 0, 65535).astype(int)
            out[v] = (int(q[0]), int(q[1]), to_snorm10(list(at_t) + [-sign]))   # (and its handedness flips)
    return out, nearest


def grip(shape):
    """for one shape: {moved: the grip's vertices, shift: how far they move in x, place: how far the placement moves back,
    line: the plate's symmetry line, middle: the grip's middle, nudge: the extra outward move, backs: {vertex: (u, v,
    tangent)} for the plate's back, nearest: how many of those took the nearest point}; None when it has no grip this reading finds
    (the renderer then holds that shield as before)"""
    a = shape["attrs"]
    if shape["lods"] is None or any(k not in a for k in ("_p0", "_i0", "_w0", "_n0", "_t0", "_u0")):
        return None
    if (a["_p0"] != (POSITION_HALF4, 0, 0) or a["_n0"][0] != SNORM_10_10_10_2 or a["_t0"][0] != SNORM_10_10_10_2 or
            a["_u0"][0] != UNORM_16_16 or any(a[k][2] for k in a)):
        return None
    b, stride, n = shape["bytes"], shape["stride"], shape["vertices"]
    raw = np.frombuffer(b, dtype=np.uint8)[:stride * n].reshape(n, stride)
    pos = raw[:, :8].copy().view("<f2")[:, :3].astype(np.float64)
    word = lambda k: raw[:, a[k][1]:a[k][1] + 4].copy().view("<u4")[:, 0]
    normal, tangent = snorm10(word("_n0")), snorm10(word("_t0"))
    uv = raw[:, a["_u0"][1]:a["_u0"][1] + 4].copy().view("<u2").reshape(-1, 2).astype(np.float64) / 65535
    keys = [bytes(r) for r in raw[:, :6]]
    rigid = (raw[:, a["_i0"][1]] == 0) & (raw[:, a["_w0"][1]] == 255)   # (all of it on palette slot 0, Skl_Root)
    moved, plates, held, strap = set(), [], [], None
    for level, (indices, first) in enumerate(shape["lods"]):
        tris = indices.reshape(-1, 3) + first
        found = pieces(keys, tris)
        plates.append((set(found[0]), tris))
        for g in found[1:]:
            g = np.array(g)
            if rigid[g].all() and np.abs(pos[g, 0]).max() <= GRIP_REACH:
                moved.update(g.tolist())
                if level == 0:
                    held.extend(g.tolist())
            elif level == 0 and strap is None and not rigid[g].all():
                strap = g   # (the largest piece the strap's bone moves)
    plate = np.array(sorted(plates[0][0]))
    if not held or not rigid[plate].all():
        return None
    P = symmetry_line(pos[plate])
    c = (pos[held, 0].min() + pos[held, 0].max()) / 2
    # (mirrored across the plate's line, the strap's middle s comes to 2 P - s: nearer the grip than on the right arm when
    # the strap is not at the plate's middle (the Deku Shield's, 2.8 cm). The grip moves outward toward the right arm's
    # spacing, at most NUDGE_MOST (headset 22 Nov: a bit more room between the handle and the strap); the placement back
    # by as much, so the grip stays in the palm and the plate and strap move toward the elbow)
    nudge = 0.0
    if strap is not None:
        s_mid = (pos[strap, 0].min() + pos[strap, 0].max()) / 2
        nudge = float(np.clip(2 * (s_mid - P), 0, NUDGE_MOST))   # (the right arm's spacing s - c, the left's 2 P - s - c)
    backs, nearest = {}, 0
    for plate_vs, tris in plates:   # (every level of detail's plate)
        mine = tris[np.isin(tris[:, 0], list(plate_vs))]
        got, k = mirrored_back(pos, normal, uv, tangent, mine, P + nudge / 2)   # (under the grip's new place)
        backs.update(got)
        nearest += k
    return {"moved": sorted(moved), "shift": 2 * (P - c) + nudge, "place": -2 * P - nudge, "line": P, "middle": c,
            "nudge": nudge, "backs": backs, "nearest": nearest}


def ranges(vertices):
    out = []
    for v in vertices:
        if out and out[-1][0] + out[-1][1] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return out


def make(models):
    text = ["// Generated by vr/tools/generate (shield_grip.py) from your game's shield models.",
            "// Local file: do not commit or share.", "#pragma once", "#include <cstdint>", "namespace mkwvk::vr::rig {",
            "/// A shield shape with a grip, moved across its back for the left VR hand: its vertex buffer (vb0 bytes, stride,",
            "/// vertices), the FNV-1a fingerprint of those bytes, how far the grip's vertices move in model x (shift) and the",
            "/// placement moves back (place), its vertex runs (first, count) in shield_grip_runs; and the plate's back with",
            "/// its texture mirrored (the old mount holes under the moved mounts): where a vertex's texture coordinates",
            "/// (16_16 unorm) and tangent (10_10_10_2 snorm) sit in it, and its entries (first, count) in shield_grip_backs",
            "struct ShieldGrip { uint64_t vb0; uint32_t stride, vertices; uint64_t fingerprint; float shift, place; "
            "uint32_t first, runs; uint32_t uv_at, tangent_at, back_first, backs; };",
            "/// A shape of the same shield without a grip of its own (the Mirror Shield's front): placed as its grip shape is",
            "struct ShieldPart { uint64_t vb0, grip_vb0; };"]
    grips, parts, runs, backs, notes = [], [], [], [], []
    for name, rel in zip(MODELS, INPUTS):
        found = [(s, grip(s)) for s in shapes(models.game.read(rel))]
        with_grip = [(s, g) for s, g in found if g]
        if len(with_grip) != 1:   # (none found, or more than one: that shield is held as before)
            notes.append("/// %s: no grip found (held as before)" % name)
            continue
        s, g = with_grip[0]
        r = ranges(g["moved"])
        b = sorted(g["backs"].items())
        grips.append("{%d, %d, %d, 0x%016xull, %.6ff, %.6ff, %d, %d, %d, %d, %d, %d}" % (
            len(s["bytes"]), s["stride"], s["vertices"], fnv1a64(s["bytes"]), g["shift"], g["place"], len(runs), len(r),
            s["attrs"]["_u0"][1], s["attrs"]["_t0"][1], len(backs), len(b)))
        runs += r
        backs += ["{%d, %du, %du}" % (v, u | w << 16, t) for v, (u, w, t) in b]
        notes.append("/// %s (%s): %d of %d vertices in %d levels, the plate's line x = %.4f, the grip's middle %.4f, "
                     "nudged %.4f outward; %d back vertices mirrored (%d of them from the nearest point: rim, outline)" %
                     (name, s["name"], len(g["moved"]), s["vertices"], len(s["lods"]), g["line"], g["middle"], g["nudge"],
                      len(b), g["nearest"]))
        parts += ["{%d, %d}" % (len(o["bytes"]), len(s["bytes"])) for o, _ in found if o is not s]
    text += notes
    text.append("inline constexpr ShieldGrip shield_grips[] = {%s};" % ", ".join(grips + ["{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}"]))
    text.append("inline constexpr uint32_t shield_grip_runs[][2] = {%s};" %
                ", ".join(["{%d, %d}" % (a, n) for a, n in runs] + ["{0, 0}"]))
    text.append("/// (vertex, its texture coordinates u | v << 16, its tangent)")
    text.append("inline constexpr uint32_t shield_grip_backs[][3] = {%s};" % ", ".join(backs + ["{0, 0, 0}"]))
    text.append("inline constexpr ShieldPart shield_parts[] = {%s};" % ", ".join(parts + ["{0, 0}"]))
    text.append("} // namespace mkwvk::vr::rig")
    return "\n".join(text) + "\n"
