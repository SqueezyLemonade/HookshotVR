"""Keyframed skeletal animations (FSKA) from a BFRES v10 (Switch) animation file: each bone's scale, rotation and
translation at any frame.

bfres_pose.py reads one-frame poses only; this reads the curves too. Three curve encodings occur in Link's animations:

- 0x408x: a plain cubic (0x4082 byte frames, 0x4081 16-bit frames), four 32-bit float coefficients per key (value = c0 + c1 u + c2 u^2 + c3 u^3, u in 0..1).
- 0x48xx: Hermite, quantised. The key block starts with two floats, the slope step and the lowest slope;
  then per key a value and a slope as unsigned 16-bit (key type 1: 0x4805, 0x4806) or 8-bit (key type 2: 0x480a)
  integers. Frames are bytes (frame type 2) or, in clips longer than 255 frames, 16-bit frame x 32 (frame type 1). value = q * scale + offset
  (the curve's own scale and offset); slope = q * step + lowest, in units per frame. A frame listed twice is one key
  with different slopes on its two sides.

Found by measurement on the player's files (15 November 2026): with that reading every looping cycle ends where it
starts, and the decoded slopes match the curve's own finite differences (correlation 0.96-1.00). The animations
store Euler XYZ rotations in radians, with W padding, although their header does not set the Euler flag.
"""
import struct

from bfres_pose import BONE_ENTRY, _name, poses

CURVE_ENTRY = 0x30
# where a curve writes, as a byte offset into the bone's values (BFRES's Switch layout)
TARGETS = {4: ("s", 0), 8: ("s", 1), 12: ("s", 2), 16: ("t", 0), 20: ("t", 1), 24: ("t", 2),
           32: ("r", 0), 36: ("r", 1), 40: ("r", 2)}
HAS_SCALE, HAS_ROTATE, HAS_TRANSLATE = 1 << 3, 1 << 4, 1 << 5
FRAME_BYTES = {0: ("<f", 4), 1: ("<h", 2), 2: ("<B", 1)}


class Curve:
    def __init__(self, d, o):
        frames_at, keys_at = struct.unpack_from("<QQ", d, o)
        self.flags, n, target = struct.unpack_from("<HHI", d, o + 0x10)
        self.start, self.end = struct.unpack_from("<2f", d, o + 0x18)
        scale, offset = struct.unpack_from("<2f", d, o + 0x24)
        if target not in TARGETS:
            raise ValueError("curve target %d is not a scale, rotation or translation" % target)
        self.target = TARGETS[target]
        ff, fs = FRAME_BYTES[self.flags & 3]
        self.frames = [struct.unpack_from(ff, d, frames_at + i * fs)[0] * (1 / 32 if fs == 2 else 1) for i in range(n)]
        if self.flags & ~3 == 0x4080:
            self.kind = "cubic"
            self.keys = [struct.unpack_from("<4f", d, keys_at + 16 * i) for i in range(n)]
        elif self.flags & 0xff00 == 0x4800 and (self.flags >> 2) & 3 in (1, 2):
            self.kind = "hermite"
            step, lowest = struct.unpack_from("<2f", d, keys_at)
            size, fmt = (2, "<H") if (self.flags >> 2) & 3 == 1 else (1, "<B")
            q = [struct.unpack_from(fmt, d, keys_at + 8 + i * size)[0] for i in range(2 * n)]
            self.keys = [(q[2 * i] * scale + offset, q[2 * i + 1] * step + lowest) for i in range(n)]
        else:
            raise ValueError("unknown animation curve encoding %#x" % self.flags)

    def at(self, t):
        f, k = self.frames, self.keys
        if t <= f[0]:
            i = 0
        elif t >= f[-1]:
            return k[-1][0] if self.kind == "hermite" else sum(k[-1])
        else:   # the last key at or before t, and the first key after it (past any repeated frame)
            i = max(j for j in range(len(f)) if f[j] <= t)
        j = i + 1
        h = f[j] - f[i]
        u = (t - f[i]) / h if h > 0 else 0.0
        if self.kind == "cubic":
            c = k[i]
            return c[0] + c[1] * u + c[2] * u * u + c[3] * u * u * u
        (v0, m0), (v1, m1) = k[i], k[j]
        u2, u3 = u * u, u * u * u
        return (2 * u3 - 3 * u2 + 1) * v0 + (u3 - 2 * u2 + u) * h * m0 + (-2 * u3 + 3 * u2) * v1 + (u3 - u2) * h * m1


class Animation:
    """One skeletal animation: frames (its length) and, per bone name, its base values and curves"""

    def __init__(self, d, p):
        self.name = _name(d, struct.unpack_from("<Q", d, p + 8)[0])
        self.frames = struct.unpack_from("<i", d, p + 0x40)[0]
        count = struct.unpack_from("<I", d, p + 0x4c)[0]
        array = struct.unpack_from("<Q", d, p + 0x28)[0]
        self.bones = {}
        for i in range(count):
            q = array + i * BONE_ENTRY
            curves_at, base = struct.unpack_from("<QQ", d, q + 8)
            bits = struct.unpack_from("<I", d, q + 0x28)[0]
            values = {}
            if bits & HAS_SCALE:
                values["s"] = list(struct.unpack_from("<3f", d, base))
                base += 12
            if bits & HAS_ROTATE:
                values["r"] = list(struct.unpack_from("<4f", d, base))[:3]
                base += 16
            if bits & HAS_TRANSLATE:
                values["t"] = list(struct.unpack_from("<3f", d, base))
            curves = [Curve(d, curves_at + c * CURVE_ENTRY) for c in range(d[q + 0x2e])]
            self.bones[_name(d, struct.unpack_from("<Q", d, q)[0])] = (values, curves)

    def bone(self, name, t):
        """A bone's (scale, rotation, translation) at frame t; None for a part the animation leaves to the skeleton"""
        if name not in self.bones:
            return None, None, None
        values, curves = self.bones[name]
        out = {k: list(v) for k, v in values.items()}
        for c in curves:
            kind, axis = c.target
            out.setdefault(kind, [None] * 3)[axis] = c.at(t)
        return out.get("s"), out.get("r"), out.get("t")


def animations(d, names):
    """{name: Animation} for the named animations in one file"""
    at = poses(d)
    missing = [n for n in names if n not in at]
    if missing:
        raise ValueError("no animation named " + ", ".join(missing))
    return {n: Animation(d, at[n]) for n in names}
