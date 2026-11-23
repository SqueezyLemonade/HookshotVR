"""A still pose (a one-frame skeletal animation, FSKA) from a BFRES v10 (Switch) animation file.

Returns, per bone the pose names, the scale, rotation and translation it sets (None where it keeps the skeleton's own).
Only one-frame poses are read: each bone's base values are the whole pose, so no keyframe curves are decoded.
"""
import struct

EULER = 1 << 12        # (the animation's rotations are Euler XYZ angles, as the sockets' are)
SCALE_MAYA = 2 << 8    # (Maya scale: a bone does not inherit its parent's scale, "segment scale compensation")
BONE_ENTRY = 0x38      # (the size of one bone's entry in the animation)
HAS_SCALE, HAS_ROTATE, HAS_TRANSLATE = 1 << 3, 1 << 4, 1 << 5


def poses(d):
    """{animation name: offset of its FSKA header} for every skeletal animation in the file"""
    out, p = {}, 0
    while True:
        p = d.find(b"FSKA", p)
        if p < 0:
            return out
        out[_name(d, struct.unpack_from("<Q", d, p + 8)[0])] = p
        p += 4


def _name(d, o):
    n = struct.unpack_from("<H", d, o)[0]
    return d[o + 2:o + 2 + n].decode()


def still_pose(d, animation):
    p = poses(d).get(animation)
    if p is None:
        raise ValueError("no animation named %s" % animation)
    flags = struct.unpack_from("<I", d, p + 4)[0]
    frames = struct.unpack_from("<i", d, p + 0x40)[0]
    count = struct.unpack_from("<I", d, p + 0x4c)[0]
    if frames != 1 or not flags & EULER or flags & (3 << 8) != SCALE_MAYA:
        raise ValueError("%s is not a one-frame Euler pose with Maya scale (frames %d, flags %#x)" % (animation, frames, flags))
    array = struct.unpack_from("<Q", d, p + 0x28)[0]
    out = {}
    for i in range(count):
        q = array + i * BONE_ENTRY
        base = struct.unpack_from("<Q", d, q + 0x10)[0]
        bits = struct.unpack_from("<I", d, q + 0x28)[0]
        s = r = t = None
        if bits & HAS_SCALE:
            s = struct.unpack_from("<3f", d, base)
            base += 12
        if bits & HAS_ROTATE:
            r = struct.unpack_from("<4f", d, base)
            base += 16
        if bits & HAS_TRANSLATE:
            t = struct.unpack_from("<3f", d, base)
        out[_name(d, struct.unpack_from("<Q", d, q)[0])] = (s, r, t)
    return out
