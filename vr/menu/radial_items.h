/* Hookshot VR: what the VR radial menus offer (vr/menu/radial_host.c chooses and selects, vr/menu/radial_draw.inc draws).
   Shared by the game side (C) and the renderer (C++).

   The item menu (left stick click): the game's assignable items, the ones its own quick select offers. `item` is the game's
   own number for it (AssignableItemEnum: the table at *0x84ca1140 + 0x250, read headless 17 Nov: 0 DekuStick, 1 DekuNut,
   2 Bomb, 3 Boomerang, 4 SlingShot, 5 Hookshot, 6 Longshot, 7 Hammer, 8 GlassTruth, 9 Bombchu, 10 DinFire, 11 FaroreWind,
   12 NayruLove, 13 MagicBean, 28 HeavyBoot, 29 HoverBoot, 30 Bow, 46-48 Fire/Ice/LightArrow, ...), selected through the
   game's own pouch_assignableitem_change. `own` is the pouch content that says it is owned (its count above 0), `icon` the
   game's icon (vr/tools/generate/icons.py), `age` who can use it as in the original game: 0 both, 1 adult, 2 child (the
   game's change does not check: it selected the Boomerang for adult Link). The five bottles (items 14, 15, 16, 17 and 50) are
   owned when the save's PlayerStatus.AssignableItem.BottleInfo.Bottle<X>.IsValid is set, `own` naming the bottle ("bottle:A");
   their icon is their content's (vr_radial_bottle_icons, sent with the entry: vr/menu/radial_host.c). Masks and trade items:
   not yet.

   The equipment menu (right stick click): swords, shields and tunics, by the game's own names, which are also their icons'.
   Equipped through the pause menu's own equip call (radial_host.c); owned when the pouch counts it; `kind` 0 sword,
   1 shield, 2 tunic, 3 the ocarina (not equipped: chosen, it goes into the right VR hand, vr/items/ocarina/ocarina_host.c;
   the Fairy Ocarina shows only without the Ocarina of Time); `age` as above (the original game's rules: the equip call does not check them either: it put the
   Biggoron Sword, not owned, on adult Link's back, headless). The Giant's Knife (Weapon_Sword_Giant, the pause menu's third
   sword on the user's save) shows only without the Biggoron Sword, its broken blade only without either. Boots: not yet
   (assignable items 28 and 29 in this game, and PlayerStatus.EquipBoot). */
#ifndef HOOKSHOT_VR_RADIAL_ITEMS_H
#define HOOKSHOT_VR_RADIAL_ITEMS_H

typedef struct { int item; const char *own; const char *icon; int age; } VrRadialItem;

static const VrRadialItem vr_radial_items[] = {
    {30, "Weapon_Range_Bow", "Weapon_Range_Bow", 1},
    {46, "Item_Arrow_Fire", "Pouch_Range_BowAndArrow_Fire", 1},
    {47, "Item_Arrow_Ice", "Pouch_Range_BowAndArrow_Ice", 1},
    {48, "Item_Arrow_Light", "Pouch_Range_BowAndArrow_Light", 1},
    {6, "Item_Longshot", "Item_Longshot", 1},
    {5, "Item_Hookshot", "Item_Hookshot", 1},   /* (not shown once the Longshot is owned) */
    {2, "Item_Bomb", "Item_Bomb", 0},           /* (owned: a bomb bag) */
    {9, "Item_Bombchu", "Item_Bombchu", 0},
    {7, "Item_Hammer", "Item_Hammer", 1},
    {1, "Item_DekuNut", "Item_DekuNut", 0},
    {0, "Item_DekuStick", "Item_DekuStick", 2},
    {4, "Weapon_Range_SlingShot", "Weapon_Range_SlingShot", 2},
    {3, "Item_Boomerang", "Item_Boomerang", 2},
    {8, "Item_GlassTruth", "Armor_GlassTruth", 0},
    {10, "Item_DinFire", "Item_DinFire", 0},
    {11, "Item_FaroreWind", "Item_FaroreWind", 0},
    {12, "Item_NayruLove", "Item_NayruLove", 0},
    {13, "Item_MagicBean", "Item_MagicBean", 2},
    {14, "bottle:A", "Item_Bottle", 0},   /* (bottles: their icon is their content's, vr_radial_bottle_icons) */
    {15, "bottle:B", "Item_Bottle", 0},
    {16, "bottle:C", "Item_Bottle", 0},
    {17, "bottle:D", "Item_Bottle", 0},
    {50, "bottle:E", "Item_Bottle", 0},
};
/* A bottle's icon by its content (the save's order: Empty, RedPotion, ..., HalfMilk; the game's own icons, named as the
   bottle actors of UI/Pouch/BottleTable). The game side sends a bottle's entry as its table index + 256 x (content + 2),
   content -1 (empty) to 11 (the game's code index) */
static const char *const vr_radial_bottle_icons[13] = {
    "Item_Bottle", "Item_Bottle_RedPotion", "Item_Bottle_GreenPotion", "Item_Bottle_BluePotion", "Item_Bottle_BlueFire",
    "Item_Bottle_Fish", "Item_Bottle_Letter", "Item_Bottle_BigPoe", "Item_Bottle_Poe", "Item_Bottle_Bug",
    "Item_Bottle_Fairy", "Item_Bottle_Milk", "Item_Bottle_HalfMilk"};
#define VR_RADIAL_ITEMS ((int)(sizeof vr_radial_items / sizeof vr_radial_items[0]))
#define VR_RADIAL_SLOTS 24   /* (at most this many shown) */

typedef struct { const char *name; int kind; int age; } VrRadialGear;

static const VrRadialGear vr_radial_gear[] = {
    {"Weapon_Sword_Kokiri", 0, 2},
    {"Weapon_Sword_Master", 0, 1},
    {"Weapon_Sword_Biggoron", 0, 1},
    {"Weapon_Sword_Giant", 0, 1},        /* (not shown with the Biggoron Sword) */
    {"Weapon_Sword_BrokenGiant", 0, 1},  /* (not shown with either) */
    {"Weapon_Shield_Deku", 1, 2},
    {"Weapon_Shield_Hylian", 1, 0},
    {"Weapon_Shield_Mirror", 1, 1},
    {"Armor_Adult_Normal", 2, 1},
    {"Armor_Adult_Goron", 2, 1},
    {"Armor_Adult_Zora", 2, 1},
    {"Item_Ocarina_Time", 3, 0},
    {"Item_Ocarina_Fairy", 3, 0},         /* (not shown with the Ocarina of Time) */
};
#define VR_RADIAL_OCARINA 3   /* (the ocarina's kind: not worn gear) */
#define VR_RADIAL_GEAR ((int)(sizeof vr_radial_gear / sizeof vr_radial_gear[0]))

#endif
