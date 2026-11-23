"""vk_vr_gait.h: Link's own walk, run, side-step and back-step cycles for the first-person legs, adult Link.

First person turns Link's body to the VR direction, but the game still runs him forward along the stick: moving
sideways or backwards, his legs ran forward while the body slid. These are the game's own lock-on cycles (side, diagonal
and back steps, Player_Adult_02_Animation: LockOn_Run_*, LockOn_Walk_*) and its forward Move_Walk/Jog/Run, so the VR legs
can step in the direction Link actually travels. The renderer blends them by direction and speed (vr/body/gait.inc);
the game's own movement and speed are untouched.

Kept, per clip: the hips (Skl_Root, without the root motion) and every bone under Waist, as rotations (int16
quaternions, sampled every second frame of the clip's 60 fps) and the translations that move; how far the clip travels
in one cycle (its Root curve), so the steps can be matched to the ground speed; and where in the cycle the left foot is
planted, so all clips step in time. Per bone: its parent, its rest translation and the inverse bind of its palette slots.
"""
import numpy as np

from bfres_anim import animations
from models import ADULT, first_palette_slot, matrix

OUTPUT = "vk_vr_gait.h"
ANIMATIONS = "Model/Player_Adult_02_Animation.anim.bfres.zs"
STANDING = "Model/Player_Adult_04_Animation.anim.bfres.zs"   # (Wait: Link standing, its first frame held still)
INPUTS = (ADULT, ANIMATIONS, STANDING)
# (name in the header, the game's clip); the order is the header's GaitClip index
CLIPS = (("walk", "Move_Walk"), ("jog", "Move_Jog"), ("run", "Move_Run"),
         ("side_walk_left", "LockOn_Walk_L"), ("side_walk_right", "LockOn_Walk_R"),
         ("side_left", "LockOn_Run_L"), ("side_right", "LockOn_Run_R"),
         ("ahead_left", "LockOn_Run_LF"), ("ahead_right", "LockOn_Run_RF"),
         ("back_left", "LockOn_Run_LB"), ("back_right", "LockOn_Run_RB"), ("back", "LockOn_Run_BL"),
         ("stand", "Wait"),   # (one still frame: no idle sway)
         ("tuck", "Jump_Run_FootL"))   # (one still frame: its knees highest, for backflips and rolls)
HELD = ("stand", "tuck")   # (clips kept as one still frame, not a cycle)
STEP = 2              # sample every second frame of the 60 fps clips (30 Hz; the renderer interpolates between them)
MOVES = 1e-4          # (m) a bone whose translation never moves more than this keeps its rest translation
GROUND = .015         # (m) an ankle within this of its lowest point counts as planted


def euler(x, y, z):
    cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(z), np.sin(z)
    return np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]) @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @ \
        np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])


def quaternion(r):
    """(x, y, z, w) of a rotation matrix, w >= 0"""
    w = np.sqrt(max(0.0, 1 + r[0, 0] + r[1, 1] + r[2, 2])) / 2
    x = np.sqrt(max(0.0, 1 + r[0, 0] - r[1, 1] - r[2, 2])) / 2
    y = np.sqrt(max(0.0, 1 - r[0, 0] + r[1, 1] - r[2, 2])) / 2
    z = np.sqrt(max(0.0, 1 - r[0, 0] - r[1, 1] + r[2, 2])) / 2
    x, y, z = np.copysign(x, r[2, 1] - r[1, 2]), np.copysign(y, r[0, 2] - r[2, 0]), np.copysign(z, r[1, 0] - r[0, 1])
    q = np.array([x, y, z, w])
    return q / np.linalg.norm(q)


def local(bone, anim, t):
    """A bone's local rotation, translation and scale at frame t (the skeleton's own where the clip leaves them)"""
    s, r, p = anim.bone(bone["name"], t)
    keep = lambda got, rest: np.array([v if v is not None else d for v, d in zip(got or [None] * 3, rest)])
    return euler(*keep(r, bone["rot"][:3])), keep(p, bone["pos"]), keep(s, bone["scale"])


def lower_body(model):
    """The bones posed: Skl_Root (the hips' frame, first) and every bone under Waist, parents before children"""
    bones = model["bones"]
    by = {b["name"]: b["i"] for b in bones}
    assert bones[by["Waist"]]["parent"] == by["Skl_Root"] and bones[by["Skl_Root"]]["parent"] == by["Root"]
    assert bones[by["Spine_1"]]["parent"] == by["Skl_Root"]
    kids = {}
    for b in bones:
        kids.setdefault(b["parent"], []).append(b["i"])
    order, todo = [by["Skl_Root"]], [by["Waist"]]
    while todo:
        i = todo.pop(0)
        order.append(i)
        todo += kids.get(i, [])
    return order


def pose(model, order, anim, t):
    """Per posed bone: (local rotation, translation, scale); Skl_Root's is relative to the model, without the root motion"""
    return [local(model["bones"][i], anim, t) for i in order]


def world(model, order, locals_):
    """Model-space matrices of the posed bones (Maya scale: a bone's own scale, not its parent's)"""
    bones = model["bones"]
    index = {b: k for k, b in enumerate(order)}
    out = []
    for k, i in enumerate(order):
        r, t, s = locals_[k]
        m = np.eye(4)
        m[:3, :3], m[:3, 3] = r @ np.diag(s), t
        if k > 0:
            parent = index[bones[i]["parent"]]
            m[:3, :3] = np.diag(1 / locals_[parent][2]) @ m[:3, :3]
            m = out[parent] @ m
        out.append(m)
    return out


def anchor(model, order, anim):
    """Where in the cycle (0..1) the left foot is mid-stance: the middle of the longest stretch it is planted"""
    ankle = order.index(next(b["i"] for b in model["bones"] if b["name"] == "Ankle_L"))
    n = anim.frames * 4
    y = np.array([world(model, order, pose(model, order, anim, anim.frames * k / n))[ankle][1, 3] for k in range(n)])
    down = y < y.min() + GROUND
    if down.all():
        return 0.0
    start = int(np.argmin(down))   # (a lifted moment: runs are counted from there, so none wraps)
    best, run, at = (0, 0), 0, 0
    for k in range(1, n + 1):
        if down[(start + k) % n]:
            run += 1
            if run > best[0]:
                best = (run, at)
        else:
            run, at = 0, k
    length, first = best
    return ((start + first + 1 + (length - 1) / 2) / n) % 1.0


def make(models):
    model = models[ADULT]
    order = lower_body(model)
    bones = model["bones"]
    clips = animations(models.game.read(ANIMATIONS), [c for t, c in CLIPS if t != "stand"])
    ankles = [order.index(next(b["i"] for b in bones if b["name"] == n)) for n in ("Ankle_L", "Ankle_R")]

    def reach(anim, f):   # (pelvis to the lower ankle, m)
        w = world(model, order, pose(model, order, anim, f))
        return w[1][1, 3] - min(w[a][1, 3] for a in ankles)
    tuck = clips["Jump_Run_FootL"]
    held = {"stand": 0, "tuck": min(range(tuck.frames), key=lambda f: reach(tuck, f))}
    clips.update(animations(models.game.read(STANDING), ["Wait"]))
    slot_bone = {}
    for slot, b in enumerate(model["matrix_to_bone"]):
        if b in order[1:]:
            slot_bone[slot] = b
    rest = [(np.array(bones[i]["pos"]), np.array(bones[i]["scale"])) for i in order]
    samples, moving, info = [], set(), []
    for tag, name in CLIPS:
        anim = clips[name]
        assert anim.frames % STEP == 0, (name, anim.frames)
        first = len(samples)
        for f in [held[tag]] if tag in HELD else range(0, anim.frames, STEP):
            p = pose(model, order, anim, f)
            samples.append(p)
            for k, (_, t, s) in enumerate(p):
                if np.abs(t - rest[k][0]).max() > MOVES or np.abs(s - rest[k][1]).max() > MOVES:
                    moving.add(k)
        start, end = anim.bone("Root", 0)[2], anim.bone("Root", anim.frames)[2]
        travel = [(e or 0) - (s or 0) for s, e in zip(start or [0] * 3, end or [0] * 3)]
        if tag in HELD:
            travel = [0, 0, 0]
        info.append((tag, name, anim.frames, travel[0], travel[2], 0.0 if tag in HELD else anchor(model, order, anim),
                     first, len(samples) - first))
    moving = sorted(moving)
    index = {b: k for k, b in enumerate(order)}

    def f(v):
        return "%.6ff" % (0.0 if abs(v) < 5e-7 else v)

    out = ["// Generated by vr/tools/generate (leg_gait.py) from your game's Player_Adult skeleton and its walk/run cycles.",
           "// Rotations, translations and bind matrices only. Local file: do not commit or share.",
           "#pragma once", "#include <cstdint>", "namespace mkwvk::vr::rig {",
           "/// One cycle: its name in the game, its length at 60 fps, how far it travels per cycle (model space, m: +x is",
           "/// Link's left, +z ahead), where its left foot is mid-stance (0..1), and its samples (first, count)",
           "struct GaitClip { const char* name; int frames; float travel_left, travel_ahead, left_planted; int first, count; };",
           "/// A posed bone: its parent among the posed bones (-1: the model), its rest translation and scale (Maya scale: a",
           "/// bone does not inherit its parent's), and which row of gait_moving moves or scales it (-1: it keeps the rest)",
           "struct GaitBone { int16_t parent; float rest[3], scale[3]; int16_t moves; };",
           "/// A palette slot of the hips and legs: the posed bone it follows and its inverse bind (identity for a rigid slot)",
           "struct GaitSlot { int16_t slot, bone; float inv_bind[12]; };",
           "constexpr int kGaitClips = %d, kGaitBones = %d, kGaitSamples = %d, kGaitMoving = %d, kGaitSlots = %d;" % (
               len(CLIPS), len(order), len(samples), len(moving), len(slot_bone)),
           "constexpr int kGaitStep = %d;   // (clip frames per sample)" % STEP,
           "enum GaitClipId { %s };" % ", ".join("gait_" + tag for tag, *_ in info),
           "inline constexpr GaitClip gait_clips[kGaitClips] = {"]
    out += ['    {"%s", %d, %s, %s, %s, %d, %d},' % (n, fr, f(tx), f(tz), f(a), fi, c) for _, n, fr, tx, tz, a, fi, c in info]
    out += ["};", "inline constexpr GaitBone gait_bones[kGaitBones] = {"]
    for k, i in enumerate(order):
        parent = -1 if k == 0 else index[bones[i]["parent"]]
        out.append("    {%d, {%s}, {%s}, %d},   // %s" % (parent, ", ".join(f(v) for v in rest[k][0]),
                                                        ", ".join(f(v) for v in rest[k][1]),
                                                        moving.index(k) if k in moving else -1, bones[i]["name"]))
    out += ["};", "/// The ankles (left, right) among the posed bones",
            "inline constexpr int16_t gait_ankle[2] = {%d, %d};" % tuple(
                index[next(b["i"] for b in bones if b["name"] == n)] for n in ("Ankle_L", "Ankle_R")),
            "/// Spine_1's joint in the hips' frame (Skl_Root): where the torso sits on the posed hips",
            "inline constexpr float gait_spine[3] = {%s};" % ", ".join(f(v) for v in bones[next(
                b["i"] for b in bones if b["name"] == "Spine_1")]["pos"]),
            "inline constexpr GaitSlot gait_slots[kGaitSlots] = {"]
    for slot, b in sorted(slot_bone.items()):
        m = matrix(model["inv_bind"][slot]) if slot < model["nsmooth"] else np.eye(4)
        out.append("    {%d, %d, {%s}}," % (slot, index[b], ", ".join(f(v) for v in m[:3].flat)))
    out += ["};", "/// Per sample, per posed bone: its local rotation as a quaternion (x, y, z, w) times 32767",
            "inline constexpr int16_t gait_rotation[kGaitSamples][kGaitBones][4] = {"]
    for p in samples:
        out.append("{" + ",".join("{%s}" % ",".join(str(int(round(v * 32767))) for v in quaternion(r)) for r, _, _ in p) + "},")
    out += ["};", "/// Per sample, per moving bone (GaitBone::moves): its local translation (m) and scale",
            "inline constexpr float gait_moving[kGaitSamples][kGaitMoving][6] = {"]
    for p in samples:
        out.append("{" + ",".join("{%s}" % ",".join(f(v) for v in list(p[k][1]) + list(p[k][2])) for k in moving) + "},")
    out += ["};", "} // namespace mkwvk::vr::rig"]
    return "\n".join(out) + "\n"
