"""vk_vr_sword_socket.h: where the Master Sword, the Kokiri Sword and the shields sit in Link's hands, for adult and child Link.

The game mounts weapons on sockets: Weapon_L (left hand, the sword) under Wrist_L and Weapon_R (right hand, the shield)
under Wrist_R. These sockets are not in the palette, so the generator chains: the wrist's bind pose x the socket's local
transform x the item's own root. The Master Sword has one rigid root matrix; the shields are skinned in model space, so
their root is the identity.

The Deku Stick (Item_DekuStick: Weapon_DekuStick's model) is a sword to the game (a long sword's WeaponParam) and is
held on the same Weapon_L mount. Its palette is 3 smooth matrices (its leaf's cloth bones) and one rigid one (Skl_Root,
the stick itself, 0.32 m up its own z from the model's origin); the held placement puts that rigid matrix at
wrist x Weapon_L x (the grip back to the origin) x Skl_Root's chain and moves the leaf's matrices with it
(vr/items/deku_stick/held_stick.inc). The VR hand holds it near its butt, not at its origin (STICK_GRIP).

Child Link's Weapon_R is the exception: its bind pose is a swimming pose (on the forearm, 11 cm toward the elbow and
turned out: the Deku Shield bent out to the right through the forearm, headset 18 Nov), while his animations (1078 of
them, every one with the shield out) hold it in the palm, opposite Weapon_L. So the child's shield mount is read from the
game's own clip of Link with the sword and shield out (Empty_Sword_Shield). Adult Link's bind pose already is his held one.
"""
import math

import numpy as np

from bfres_anim import animations
from models import ADULT, CHILD, CHILD_ANIMATIONS, DEKU_STICK, KOKIRI_SWORD, MASTER_SWORD, matrix

OUTPUT = "vk_vr_sword_socket.h"
INPUTS = (ADULT, CHILD, MASTER_SWORD, KOKIRI_SWORD, CHILD_ANIMATIONS, DEKU_STICK)
STICK_ROOT = "Skl_Root"   # (the Deku Stick's rigid bone: the stick; the others are its leaf's cloth)
STICK_HEAD = "Effect_0"   # (its tip, where the game's flame sits: in the log's placement lines)
# Where the VR hand holds the Deku Stick, in its model (m): the shaft's centre 9 cm up from its butt. The model's origin, where
# the game holds it, is 31 cm up the shaft (its mesh runs z -0.31 to 0.82) and 2 to 3 cm off the shaft's centre there: held
# at the origin it sat beside the palm with most of it behind the hand (headset 18 Nov, "hold it further down"). Measured
# from the stick's mesh (the median of its vertices within 3.5 cm of z -0.22). The game side places the stick's actor (its
# attack and fire sensors) as far up the fist: kVrStickGripZ in vr/items/deku_stick/deku_stick_host.c is -STICK_GRIP[2].
STICK_GRIP = (0.0, 0.011, -0.22)
SHIELD_OUT = "Empty_Sword_Shield"   # (Player_Child_01: Link standing with the sword and shield out)


def local(bone):
    """A bone's local transform. Sockets store Euler XYZ rotation in radians; any other layout is refused"""
    if not bone["flags"] & (1 << 12):
        raise ValueError("socket does not declare Euler rotation: " + bone["name"])
    x, y, z = bone["rot"][:3]
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    r = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]) @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @ \
        np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    out = np.eye(4)
    out[:3, :3] = r @ np.diag(bone["scale"])
    out[:3, 3] = bone["pos"]
    return out


def sword_root(sword):
    assert sword["nsmooth"] == 0 and sword["nrigid"] == 1
    b, idx, chain = sword["bones"], sword["matrix_to_bone"][0], []
    while idx >= 0:
        chain.append(b[idx])
        idx = b[idx]["parent"]
    out = np.eye(4)
    for bone in reversed(chain):
        out = out @ local(bone)
    return out


def chain(bones, i):
    """a bone's transform in its model (its parents' local transforms in turn)"""
    out = np.eye(4)
    while i >= 0:
        out = local(bones[i]) @ out
        i = bones[i]["parent"]
    return out


def deku_stick(stick):
    """The Deku Stick model's layout as the held placement needs it: its shapes' vertex buffer sizes, its palette size, the
    palette slot of its rigid stick bone and that bone's place in the model; refused if a game version changed it"""
    bones = stick["bones"]
    by = {b["name"]: b["i"] for b in bones}
    root = by[STICK_ROOT]
    slots = [i for i, b in enumerate(stick["matrix_to_bone"]) if b == root]
    assert stick["nrigid"] == 1 and len(slots) == 1 and slots[0] >= stick["nsmooth"], (stick["nsmooth"], stick["nrigid"], slots)
    place = chain(bones, root)
    head = chain(bones, by[STICK_HEAD])[:3, 3]
    # (the stick runs along its own +z, the head well up it: the game side's placement assumes the same axis, z along the
    # fist, for its sensors, vr/items/deku_stick/deku_stick_host.c)
    assert np.allclose(place[:3, :3], np.eye(3), atol=1e-4) and head[2] > .5, (place, head)
    return [s["vb0"] for s in stick["shapes"]], len(stick["matrix_to_bone"]), slots[0], place, head


def held_mount(body, clip, weapon):
    """A mount bone as an animation holds it: its first frame's rotation and translation over the skeleton's bone"""
    bone = next(b for b in body["bones"] if b["name"] == weapon)
    s, r, t = clip.bone(weapon, 0)
    if r is None or t is None or None in r or None in t:
        raise ValueError(clip.name + " does not place " + weapon)
    return dict(bone, rot=tuple(r) + (1.0,), pos=tuple(t), scale=tuple(s) if s and None not in s else bone["scale"])


def socket(body, root, wrist="Wrist_L", weapon="Weapon_L", mount=None):
    bones = body["bones"]
    w = next(b for b in bones if b["name"] == wrist)
    mount = mount or next(b for b in bones if b["name"] == weapon)
    assert mount["parent"] == w["i"]
    slot = body["matrix_to_bone"].index(w["i"])
    assert slot < body["nsmooth"]
    return np.linalg.inv(matrix(body["inv_bind"][slot])) @ local(mount) @ root


def bind_place(body, name):
    """a skinned bone's place in the bind pose (its inverse bind matrix inverted)"""
    i = next(b["i"] for b in body["bones"] if b["name"] == name)
    slot = body["matrix_to_bone"].index(i)
    assert slot < body["nsmooth"], name
    return np.linalg.inv(matrix(body["inv_bind"][slot]))


def shield_pivot(body, placed):
    """the right wrist's joint in a shield's model space (placed: the shield's model -> the bind pose); refused unless the
    forearm runs along the shield's +x, as the guard pose's quarter turn assumes (held_gear.inc shield_guard_pose)"""
    to_shield = np.linalg.inv(placed)
    wrist = (to_shield @ bind_place(body, "Wrist_R")[:, 3])[:3]
    elbow = (to_shield @ bind_place(body, "Arm_2_R")[:, 3])[:3]
    along = (elbow - wrist) / np.linalg.norm(elbow - wrist)
    assert along[0] > .95, ("the forearm no longer runs along the shield's x", along)
    return wrist


def row(name, m):
    assert np.allclose(m[:3, :3].T @ m[:3, :3], np.eye(3), atol=1e-5), name + " is not a rotation"
    return "inline constexpr float " + name + "[12] = {" + ", ".join(f"{v:.9f}f" for v in m[:3].flat) + "};"


def make(models):
    adult, child, root = models[ADULT], models[CHILD], sword_root(models[MASTER_SWORD])
    kokiri = sword_root(models[KOKIRI_SWORD])
    clip = animations(models.game.read(CHILD_ANIMATIONS), [SHIELD_OUT])[SHIELD_OUT]
    child_shield = held_mount(child, clip, "Weapon_R")
    # (the held child mount sits where the hand is: within 4 cm of the mirror of Weapon_L, as adult Link's does; the bind
    # pose's was 11 cm off)
    assert abs(child_shield["pos"][0] + next(b for b in child["bones"] if b["name"] == "Weapon_L")["pos"][0]) < .04

    def shield(b, n):
        return socket(b, np.eye(4), "Wrist_R", "Weapon_R", child_shield if n == "child" else None)
    text = ["// Generated by vr/tools/generate (hand_sockets.py) from your game's skeleton bind data.",
            "// Local file: do not commit or share.", "#pragma once", "#include <cstdint>", "namespace mkwvk::vr::rig {"]
    text += [row("master_sword_" + n, socket(b, root)) for n, b in (("adult", adult), ("child", child))]
    text += [row("shield_" + n, shield(b, n)) for n, b in (("adult", adult), ("child", child))]
    # (the other hands, for a sword taken by the right VR hand: each grip mirrored across Link's body (model x = 0, his
    # skeleton is mirrored), X S X: the Weapon_R / Weapon_L mounts instead held each at the other item's angle, headset 17 Nov)
    x = np.diag([-1.0, 1.0, 1.0, 1.0])
    # (where the hand holds the sword, in the sword's own frame: the mount's origin through the inverse of its root chain;
    # the Sword angle tilt turns about it, so the handle stays in the palm)
    grip = np.linalg.inv(root)[:3, 3]
    text.append("inline constexpr float master_sword_grip[3] = {" + ", ".join(f"{v:.9f}f" for v in grip) + "};")
    text += [row("master_sword_right_" + n, x @ socket(b, root) @ x) for n, b in (("adult", adult), ("child", child))]
    # (the shield's placement mirrored, not the model: mirrored as well (x S), its handle came to the arm's side but it showed
    # inside out, its draw culls back faces, headset 17 Nov. So its handle stays on the left; a mirrored model needs that
    # draw's face culling flipped)
    text += [row("shield_left_" + n, x @ shield(b, n) @ x) for n, b in (("adult", adult), ("child", child))]
    # (the shield turned to face forward while guarding, vr/items/gear/held_gear.inc shield_guard_pose: about the grip (the
    # mount, the shield's origin), in the shield's own space. Every shield's face is its +y (its thin axis; the Mirror
    # Shield's mirror on the +y side) and Link's forearm runs along its x, the elbow toward +x, the thumb toward +z (measured
    # 19 Nov): a quarter turn about z turns the face to -x, where the fist points. Checked here, nothing written)
    for n, b in (("adult", adult), ("child", child)):
        shield_pivot(b, shield(b, n))
    # (the Kokiri Sword: one rigid root like the Master Sword's, shorter (0.204 m to its root, the Master Sword's 0.445),
    # on the same Weapon_L mount; child Link's, and adult's for completeness)
    text += [row("kokiri_sword_" + n, socket(b, kokiri)) for n, b in (("adult", adult), ("child", child))]
    text += [row("kokiri_sword_right_" + n, x @ socket(b, kokiri) @ x) for n, b in (("adult", adult), ("child", child))]
    # (the Deku Stick, child Link's (adult's for completeness: he cannot use one): its rigid matrix on Weapon_L, the right
    # hand's mirrored as the swords'; and how its draws are recognised)
    shapes, matrices, slot, place, head = deku_stick(models[DEKU_STICK])
    text += ["/// The Deku Stick (Item_DekuStick): its shapes' vertex buffer sizes, its palette (matrices), the palette slot of its",
             "/// rigid stick bone (the others: its leaf's cloth), that bone in the model, and its tip (the flame) in the model;",
             "/// after the sockets, where the VR hand holds it in the model (STICK_GRIP)",
             "inline constexpr uint64_t deku_stick_shapes[%d] = {%s};" % (len(shapes), ", ".join("%d" % v for v in shapes)),
             "inline constexpr unsigned deku_stick_matrices = %d, deku_stick_rigid_slot = %d;" % (matrices, slot),
             row("deku_stick_root", place),
             "inline constexpr float deku_stick_head[3] = {" + ", ".join(f"{v:.9f}f" for v in head) + "};"]
    # (held at STICK_GRIP: the model moved so that point is on the mount. The right hand's mount is mirrored, not the stick:
    # X S X mirrored its bone's 1.2 cm side offset too, 2.5 cm off the palm, headset 18 Nov)
    grip = np.eye(4)
    grip[:3, 3] = -np.array(STICK_GRIP)
    text += [row("deku_stick_" + n, socket(b, np.eye(4)) @ grip @ place) for n, b in (("adult", adult), ("child", child))]
    text += [row("deku_stick_right_" + n, x @ socket(b, np.eye(4)) @ x @ grip @ place) for n, b in (("adult", adult), ("child", child))]
    text.append("inline constexpr float deku_stick_grip[3] = {" + ", ".join(f"{v:.9f}f" for v in STICK_GRIP) + "};")
    text.append("} // namespace mkwvk::vr::rig")
    return "\n".join(text) + "\n"
