"""Where the generators find each model in the game's romfs, and small matrix helpers they share."""
import numpy as np

from bfres_skeleton import parse

ADULT = "Model/Player_Adult.Player_Adult.bfres.zs"
CHILD = "Model/Player_Child.Player_Child.bfres.zs"
GORON_TUNIC = "Model/Armor_Adult_Goron.Armor_Adult_Goron.bfres.zs"
ZORA_TUNIC = "Model/Armor_Adult_Zora.Armor_Adult_Zora.bfres.zs"
SILVER_GAUNTLETS = "Model/Armor_Adult_Gauntlet_Silver.Armor_Adult_Gauntlet_Silver.bfres.zs"
GOLDEN_GAUNTLETS = "Model/Armor_Adult_Gauntlet_Gold.Armor_Adult_Gauntlet_Gold.bfres.zs"
GORON_BRACELET = "Model/Item_Gauntlet_Goron.Item_Gauntlet_Goron.bfres.zs"   # (child Link's Goron Bracelet, worn)
MASTER_SWORD = "Model/Weapon_Sword_Master.Weapon_Sword_Master.bfres.zs"
KOKIRI_SWORD = "Model/Weapon_Sword_Kokiri.Weapon_Sword_Kokiri.bfres.zs"
DEKU_STICK = "Model/Item_DekuStick.Item_DekuStick.bfres.zs"   # (child Link's held stick, Weapon_DekuStick's model)
CHILD_ANIMATIONS = "Model/Player_Child_01_Animation.anim.bfres.zs"
BOW = "Model/Weapon_Range_Bow.Weapon_Range_Bow.bfres.zs"
BOW_ANIMATIONS = "Model/Weapon_Range_Bow_Animation.anim.bfres.zs"
COMMON_TEXTURES = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs"


class Models:
    """Each model parsed once, however many generators use it"""

    def __init__(self, game):
        self.game = game
        self._cache = {}

    def __getitem__(self, rel):
        if rel not in self._cache:
            self._cache[rel] = parse(self.game.read(rel))
        return self._cache[rel]


def matrix(rows3x4):
    """A 3x4 row-major matrix from the model file as a 4x4"""
    m = np.eye(4)
    m[:3] = np.array(rows3x4).reshape(3, 4)
    return m


def first_palette_slot(model):
    """bone index -> the first palette slot that uses that bone"""
    out = {}
    for slot, bone in enumerate(model["matrix_to_bone"]):
        out.setdefault(bone, slot)
    return out
