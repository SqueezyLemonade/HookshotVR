"""vk_vr_worn_items.h: the vertex buffer sizes of what Link wears that first person hides: belt and back items (bags,
quivers, wallets, scales, pouch bottles, the Stone of Agony), boots, the Lens of Truth's glasses, masks
and hats. None of them has a VR pose yet, and in the game's pose they float off the VR body; hidden, they cannot.

Read from your game's models (their shapes' vertex buffer sizes, which is how their draws are recognised); a model your
game does not have is left out.

Also what is worn that first person does NOT hide, child Link's Goron Bracelet (Item_Gauntlet_Goron, worn by the game's
Armor_Child_Gauntlet_Goron actor): its own small rigid model (one palette matrix, its Skl_Root, not skinned to Link), which
the game puts on one of his arm bones every frame. The renderer finds which bone at run time and pins it to that VR bone
(vr/items/gear/equipment_skin.inc, bracelet_skin). Written for it:
  bracelet_matrices  the palette matrices its draws bind (0: your game has no bracelet model; nothing is recognised)
  bracelet_vb0       its shapes' vertex buffer 0 sizes (how its draws are recognised)
  bracelet_bones     the bones it may be worn on, by name (each forearm and wrist): child Link's palette slot, and the
                     bind-pose positions of the bone's joint and of its end (the next joint), for the reach test
"""
import numpy as np

from models import CHILD, GORON_BRACELET, first_palette_slot, matrix

OUTPUT = "vk_vr_worn_items.h"
# (romfs model files; the generator takes every shape of each)
MODELS = (
    "Item_BombBag_Small.Item_BombBag_Small", "Item_BombBag_Medium.Item_BombBag_Medium", "Item_BombBag_Large.Item_BombBag_Large",
    "Item_BulletBag_Small.Item_BulletBag_Small", "Item_BulletBag_Medium.Item_BulletBag_Medium",
    "Item_BulletBag_Large.Item_BulletBag_Large",
    "Item_NutBag_Small.Item_NutBag_Small", "Item_NutBag_Medium.Item_NutBag_Medium", "Item_NutBag_Large.Item_NutBag_Large",
    "Item_StickBag_Small.Item_StickBag_Small", "Item_StickBag_Medium.Item_StickBag_Medium",
    "Item_StickBag_Large.Item_StickBag_Large",
    "Item_Quiver_Small.Item_Quiver_Small", "Item_Quiver_Medium.Item_Quiver_Medium", "Item_Quiver_Large.Item_Quiver_Large",
    "Item_Wallet_Small.Item_Wallet_Small", "Item_Wallet_Medium.Item_Wallet_Medium", "Item_Wallet_Large.Item_Wallet_Large",
    # (not the ocarinas and their holders: their own VR pose, vr/items/ocarina; with it off they are hidden as Link's gear)
    "Item_Scale_Silver.Item_Scale_Silver", "Item_Scale_Gold.Item_Scale_Gold",
    "Item_Bottle.Item_Bottle_Pouch", "Item_Bottle_Fairy.Item_Bottle_Fairy_Pouch", "Item_Bottle_Poe.Item_Bottle_Poe_Pouch",
    "Item_AgonyStone.Item_AgonyStone",
    "Armor_Adult_HeavyBoot.Armor_Adult_HeavyBoot", "Armor_Adult_HoverBoot.Armor_Adult_HoverBoot",
    "Armor_Adult_GlassTruth.Armor_Adult_GlassTruth", "Armor_Child_GlassTruth.Armor_Child_GlassTruth",
    "Armor_Adult_Default_Head.Armor_Adult_Default_Head", "Armor_Child_Default_Head.Armor_Child_Default_Head",
    "Armor_Child_Mask_Bunny.Armor_Child_Mask_Bunny", "Armor_Child_Mask_Gerudo.Armor_Child_Mask_Gerudo",
    "Armor_Child_Mask_Goron.Armor_Child_Mask_Goron", "Armor_Child_Mask_Keaton.Armor_Child_Mask_Keaton",
    "Armor_Child_Mask_Skull.Armor_Child_Mask_Skull", "Armor_Child_Mask_Spooky.Armor_Child_Mask_Spooky",
    "Armor_Child_Mask_Truth.Armor_Child_Mask_Truth", "Armor_Child_Mask_Zora.Armor_Child_Mask_Zora",
    "Armor_Child_Mask_Amiibo0.Armor_Child_Mask_Amiibo0", "Armor_Child_Mask_Amiibo1.Armor_Child_Mask_Amiibo1",
)
INPUTS = (CHILD,)   # (Link's skeleton, for the bracelet's bones; every worn model is optional: checked below, not required)
# The bracelet's possible bones: (bone, the bone whose joint is its end), each forearm and each wrist, left then right
BRACELET_BONES = (("Arm_2_L", "Wrist_L"), ("Wrist_L", "Finger_C_1_L"), ("Arm_2_R", "Wrist_R"), ("Wrist_R", "Finger_C_1_R"))


# Link's own models: a size they use is never hidden (his body, clothes and held gear have their own VR poses)
OWN = ("Player_Adult.Player_Adult", "Player_Child.Player_Child", "Armor_Adult_Normal.Armor_Adult_Normal",
       "Armor_Adult_Goron.Armor_Adult_Goron", "Armor_Adult_Zora.Armor_Adult_Zora",
       "Armor_Adult_Gauntlet_Silver.Armor_Adult_Gauntlet_Silver", "Armor_Adult_Gauntlet_Gold.Armor_Adult_Gauntlet_Gold",
       "Item_Gauntlet_Goron.Item_Gauntlet_Goron", "Weapon_Sword_Kokiri.Weapon_Sword_Kokiri",
       "Weapon_Sword_Master.Weapon_Sword_Master", "Weapon_Sword_Biggoron.Weapon_Sword_Biggoron",
       "Weapon_Shield_Deku.Weapon_Shield_Deku", "Weapon_Shield_Hylian.Weapon_Shield_Hylian",
       "Weapon_Shield_Mirror.Weapon_Shield_Mirror", "Weapon_Sheath_Kokiri.Weapon_Sheath_Kokiri",
       "Weapon_Sheath_Master.Weapon_Sheath_Master", "Weapon_Sheath_Biggoron.Weapon_Sheath_Biggoron",
       "Weapon_Range_Bow.Weapon_Range_Bow", "Weapon_Range_SlingShot.Weapon_Range_SlingShot")


def bind_joint(model, slot_of, by, name):
    """A bone's bind-pose position, from its (smooth) inverse bind"""
    s = slot_of[by[name]]
    assert s < model["nsmooth"], name
    return np.linalg.inv(matrix(model["inv_bind"][s]))[:3, 3]


def bracelet(models):
    """The Goron Bracelet's lines of the header"""
    child = models[CHILD]
    matrices, vb0 = 0, []
    if models.game.exists(GORON_BRACELET):
        m = models[GORON_BRACELET]
        matrices, vb0 = m["nsmooth"] + m["nrigid"], sorted({s["vb0"] for s in m["shapes"]})
        assert matrices >= 1 and vb0, "Item_Gauntlet_Goron: no palette or no shapes"
    by = {b["name"]: b["i"] for b in child["bones"]}
    slot_of = first_palette_slot(child)
    f = lambda p: "{%.7ff, %.7ff, %.7ff}" % tuple(p)
    out = ["// The Goron Bracelet (child Link's, worn on an arm bone; not hidden: pinned to the VR arm)",
           "struct BraceletBone { int16_t slot; float joint[3], end[3]; };",
           "inline constexpr uint32_t bracelet_matrices = %d;" % matrices,
           "inline constexpr uint64_t bracelet_vb0[%d] = {%s};" % (max(1, len(vb0)), ", ".join(map(str, vb0)) or "0"),
           "inline constexpr BraceletBone bracelet_bones[%d] = {   // (left forearm, left wrist, right forearm, right wrist)"
           % len(BRACELET_BONES)]
    for name, end in BRACELET_BONES:
        assert name in by and end in by, "Player_Child has no %s or %s" % (name, end)
        out.append("    {%d, %s, %s},   // %s" % (slot_of[by[name]], f(bind_joint(child, slot_of, by, name)),
                                                f(bind_joint(child, slot_of, by, end)), name))
    return out + ["};"]


def make(models):
    game = models.game
    found, pairs, own = [], set(), set()
    for name in OWN:
        rel = "Model/%s.bfres.zs" % name
        if game.exists(rel):
            own |= {s["vb0"] for s in models[rel]["shapes"]}
    for name in MODELS:
        rel = "Model/%s.bfres.zs" % name
        if not game.exists(rel):
            continue
        found.append(name.split(".")[1])
        m = models[rel]
        pairs |= {(s["vb0"], m["nsmooth"] + m["nrigid"]) for s in m["shapes"] if s["vb0"] not in own}
    rows = sorted(pairs)
    out = ["// Generated by vr/tools/generate (worn_items.py) from your game's models: what first person hides of what Link wears,",
           "// and where child Link's Goron Bracelet may sit. Vertex buffer sizes, palette slots and bind positions only.",
           "// Local file: do not commit or share.", "#pragma once", "#include <cstdint>",
           "namespace mkwvk::vr::rig {",
           "struct Worn { uint64_t vb0; uint32_t matrices; };",
           "// %s" % ", ".join(found),
           "inline constexpr Worn worn_hidden[%d] = {%s};" % (max(1, len(rows)), ", ".join("{%d, %d}" % r for r in rows) or "{0, 0}")]
    out += bracelet(models) + ["} // namespace mkwvk::vr::rig"]
    return "\n".join(out) + "\n"
