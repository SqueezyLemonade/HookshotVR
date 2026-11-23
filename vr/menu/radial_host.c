/* Hookshot VR: the VR radial menus, first person (VR tab: Stick click menus: VR radial).

   A clean left stick click opens the item menu, a clean right stick click the equipment menu: what you own and can use at
   your age (vr/menu/radial_items.h), drawn by the renderer as a ring floating in the world (vr/menu/radial_draw.inc). Push
   that hand's stick toward one to highlight it; let the stick back to the middle to select it; click again to close without
   selecting (a click of the other stick switches menus). While the item menu is open the left stick does not move Link,
   while the equipment menu is open the right stick does not turn or jump (vr/input/stick_host.c, controllers_host.c).
   Items are selected through the game's own pouch_assignableitem_change (f_81231c00: the inventory, a flag (0 adult, else
   child: as its callers pass it; adult tested), the item's number, 0, 1), found headless on 17 Nov from its name string; it
   selected the Longshot and the Deku Stick, and the game's selection reader saw them. Equipment is equipped through the
   pause menu's own equip call (f_80a9acd0: the inventory manager *0x84e98e28, the address of a pointer to the name, 3, 0;
   found headless by catching the pause menu's copy of "Armor_Adult_Goron" into the manager); headless it changed the tunic
   and the sword with no menu open. What is worn: the save's PlayerStatus.EquipWeapon, EquipShield and EquipArmor (strings,
   murmur3 hashes of those names), which the call updates at once.

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/input/quick_select_host.c and
   vr/items/bottle/bottle_host.c (vr_bottle_owned, vr_bottle_content: the bottles' entries; it uses quick_select_host.c's
   selection reader's results and oot_item_count, oot_gamedata_bool, oot_gamedata_string from host/hos_main.c). Not compiled
   on its own. */
#include "radial_items.h"

int rx_option(int id, int otherwise);
int rx_vr_radial(const int32_t *data, int n);
static volatile int oot_vr_radial_side = -1;          /* the open menu: -1 none, 0 items (left stick), 1 equipment (right) */
static volatile int oot_vr_radial_highlight = -1;     /* the highlighted slot, or -1 */
static volatile int oot_vr_radial_pending = -1;       /* a selection for the game's thread: side * 256 + its table's index */
/* Each menu's entries (their table's indexes), listed by the game's thread twice a second, and the open menu's own copy:
   taken from them as it opens and kept until it closes, so a slot shown is the slot selected whatever is listed meanwhile.
   The input's thread opens, picks and selects (oot_vr_radial_controls), the game's lists and draws (oot_vr_radial_frame):
   both under one spin lock, held for a few dozen ints at a time and never across a call into the game (review 20 Nov: the
   game checked the menu was closed, then rewrote the list and its count; a click between them opened the menu on a list
   half rewritten). */
typedef struct { int count, index[VR_RADIAL_SLOTS]; } VrRadialList;
static int oot_vr_radial_lock_word;
static VrRadialList oot_vr_radial_live[2];   /* (0 the items, 1 the equipment, as last listed) */
static VrRadialList oot_vr_radial_shown;     /* (the open menu's, from the live list as it opened) */
static void oot_vr_radial_lock(void) {
    while (__atomic_exchange_n(&oot_vr_radial_lock_word, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&oot_vr_radial_lock_word, __ATOMIC_RELAXED)) {}
}
static void oot_vr_radial_unlock(void) { __atomic_store_n(&oot_vr_radial_lock_word, 0, __ATOMIC_RELEASE); }
/* (the game's thread) the lists just made, both at once */
static void oot_vr_radial_publish(const int *items, int n, const int *gear, int g) {
    oot_vr_radial_lock();
    oot_vr_radial_live[0].count = n, oot_vr_radial_live[1].count = g;
    for (int k = 0; k < n; k++) oot_vr_radial_live[0].index[k] = items[k];
    for (int k = 0; k < g; k++) oot_vr_radial_live[1].index[k] = gear[k];
    oot_vr_radial_unlock();
}
/* (the game's thread) the open menu and its entries (open: its own copy; closed: the item list), as one; side as -1 .. 1 */
static int oot_vr_radial_now(VrRadialList *out, int side_of_closed) {
    oot_vr_radial_lock();
    const int side = __atomic_load_n(&oot_vr_radial_side, __ATOMIC_RELAXED);
    *out = side >= 0 ? oot_vr_radial_shown : oot_vr_radial_live[side_of_closed];
    oot_vr_radial_unlock();
    return side;
}
static volatile int oot_vr_radial_child;
int hos_vr_ocarina_held(void);   /* (vr/items/ocarina/ocarina_host.c: a VR hand holds the ocarina: its ring is green) */
static int oot_vr_radial_bottle[5] = {-1, -1, -1, -1, -1};   /* each bottle's content as last listed (its icon) */

int hos_vr_radial_open(void) { return __atomic_load_n(&oot_vr_radial_side, __ATOMIC_RELAXED) + 1; }   /* (0 none, 1 items, 2 equipment) */

/* (host/nn_hid.c, through hos_oot_vr_menu_controls) the stick clicks and the sticks, with "VR radial" chosen. The clicks are
   read from the controllers themselves (their stick buttons, before any controller layout: through the player's layout the
   click came to the game as another button and opened its own menu, 17 Nov); vr/input/controllers_host.c keeps them from
   the game. A click: pressed and let go within 0.4 s with no menu button meanwhile (the Minus and Hookshot menu chords), and
   for the left one no right stick pushed (the D-pad chord). */
static uint64_t oot_vr_radial_controls(uint64_t buttons) {
    const uint64_t LS = 1ULL << 4, RS = 1ULL << 5;
    static int was[2], pending[2], armed;
    static int64_t pressed_at[2];
    const int64_t now = hos_now_ns();
    uint32_t hw[64] = {0};
    const int tracked = rx_vr_hands(hw);
    uint32_t bt[2] = {0, 0};
    float rx = 0, ry = 0;
    if (tracked) memcpy(&bt[0], &hw[1 + 25], 4), memcpy(&bt[1], &hw[1 + 26 + 25], 4), memcpy(&rx, &hw[1 + 26 + 23], 4),
                 memcpy(&ry, &hw[1 + 26 + 24], 4);
    int side = __atomic_load_n(&oot_vr_radial_side, __ATOMIC_RELAXED);
    for (int h = 0; h < 2; h++) {
        const int down = (bt[h] & 4) != 0;
        if (down && !was[h]) pending[h] = 1, pressed_at[h] = now;
        if (pending[h] && ((bt[0] & 8) || (!h && rx * rx + ry * ry > .3f))) pending[h] = 0;   /* (chords: not a click) */
        const int click = !down && was[h] && pending[h] && now - pressed_at[h] < 400000000LL;
        if (!down) pending[h] = 0;
        was[h] = down;
        if (!click) continue;
        int count = 0;
        oot_vr_radial_lock();   /* (the list it opens on taken as one: its count and its entries) */
        side = __atomic_load_n(&oot_vr_radial_side, __ATOMIC_RELAXED);
        const int closing = side == h;
        if (closing) side = -1;
        else if ((count = oot_vr_radial_live[h].count) > 0) side = h, oot_vr_radial_shown = oot_vr_radial_live[h];
        __atomic_store_n(&oot_vr_radial_side, side, __ATOMIC_RELAXED);
        oot_vr_radial_unlock();
        if (closing) hos_log("vr: radial: closed (clicked again), nothing selected");
        else if (count > 0) {
            armed = 0;
            __atomic_store_n(&oot_vr_radial_highlight, -1, __ATOMIC_RELAXED);
            hos_log("vr: radial: %s menu opened (%d)", h ? "equipment" : "item", count);
        } else hos_log("vr: radial: no %s to show", h ? "equipment" : "items");
    }
    if (side >= 0) {   /* (the clicked controller's own stick: the direction picks, back to the middle selects) */
        float x = 0, y = 0;
        if (tracked) memcpy(&x, &hw[1 + 26 * side + 23], 4), memcpy(&y, &hw[1 + 26 * side + 24], 4);
        const float m = sqrtf(x * x + y * y);
        const int n = oot_vr_radial_shown.count;   /* (this thread's own copy: only it writes the open menu's) */
        if (m > .55f && n > 0) {   /* (slot 0 at the top, clockwise) */
            float a = atan2f(x, y);
            if (a < 0) a += 6.2831853f;
            const int k = (int)lroundf(a / (6.2831853f / n)) % n;
            __atomic_store_n(&oot_vr_radial_highlight, k, __ATOMIC_RELAXED);
            armed = 1;
        } else if (armed && m < .25f) {   /* (the slot shown is the one selected: the open menu's own copy) */
            const int k = __atomic_load_n(&oot_vr_radial_highlight, __ATOMIC_RELAXED);
            oot_vr_radial_lock();
            if (k >= 0 && k < n) __atomic_store_n(&oot_vr_radial_pending, side * 256 + oot_vr_radial_shown.index[k], __ATOMIC_RELAXED);
            __atomic_store_n(&oot_vr_radial_side, -1, __ATOMIC_RELAXED);
            oot_vr_radial_unlock();
            armed = 0;
        }
    }
    return buttons & ~(LS | RS);
}

/* the gear now worn, by the save's names: PlayerStatus.EquipWeapon, EquipShield, EquipArmor */
static void oot_vr_radial_worn(Ctx *c, char worn[3][64]) {
    static const uint32_t keys[3] = {0xa95a3521u, 0xee463235u, 0x008d7f0cu};
    for (int i = 0; i < 3; i++) if (!oot_gamedata_string(c, keys[i], worn[i], 64)) worn[i][0] = 0;
}

/* (the game's thread) equip one of vr_radial_gear through the pause menu's own call */
static void oot_vr_radial_equip(Ctx *c, const char *name) {
    const uint64_t manager = recomp_load64(c, 0x84e98e28ULL);
    A64Fn equip = a64_lookup(0x80a9acd0ULL);
    if (manager < HOS_HEAP_BASE || !equip || strlen(name) >= 0x60) return;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x100) & ~0xfULL;
    memcpy(A64_PTR(frame + 0x40), name, strlen(name) + 1);
    recomp_store64_sp(c, frame + 0x20, frame + 0x40);
    c->x[31] = frame;
    c->x[0] = manager, c->x[1] = frame + 0x20, c->x[2] = 3, c->x[3] = 0;
    equip(c);
    *c = saved;
    hos_log("vr: radial: equipped %s", name);
}

/* (every game frame, from vr/camera/view_host.c, after the selection reader) what each menu shows (twice a second), a
   pending selection made through the game's own calls, and the open menu's state to the renderer */
static void oot_vr_radial_frame(Ctx *c) {
    static int64_t listed_at;
    static char worn[3][64];
    const int64_t now = hos_now_ns();
    static int test = -1;   /* (RX_VR_RADIAL_TEST, headless, 15 s in: the lists logged; 1: the second item selected, 2: the
                               Zora Tunic (else the first other tunic) equipped) */
    if (test < 0) test = vr_research_int("RX_VR_RADIAL_TEST", 0);
    if (test) {
        static int64_t first;
        static int done;
        if (!first) first = now;
        VrRadialList items, gear;
        oot_vr_radial_lock();
        items = oot_vr_radial_live[0], gear = oot_vr_radial_live[1];
        oot_vr_radial_unlock();
        if (!done && now - first > 15000000000LL && items.count > 1) {
            done = 1;
            for (int k = 0; k < items.count; k++)
                hos_log("vr: radial test: item slot %d: %s", k, vr_radial_items[items.index[k]].icon);
            for (int k = 0; k < gear.count; k++)
                hos_log("vr: radial test: equipment slot %d: %s", k, vr_radial_gear[gear.index[k]].name);
            hos_log("vr: radial test: worn %s, %s, %s", worn[0], worn[1], worn[2]);
            if (test == 1) __atomic_store_n(&oot_vr_radial_pending, items.index[1], __ATOMIC_RELAXED);
            for (int k = 0; test == 2 && k < gear.count; k++) {
                const VrRadialGear *g = &vr_radial_gear[gear.index[k]];
                if (g->kind == 2 && strcmp(g->name, worn[2])) __atomic_store_n(&oot_vr_radial_pending, 256 + gear.index[k], __ATOMIC_RELAXED);
                if (g->kind == 2 && !strcmp(g->name, "Armor_Adult_Zora") && strcmp(g->name, worn[2])) break;
            }
        }
    }
    if (rx_option(901, 0) != 1 && !test) {
        oot_vr_radial_lock();
        __atomic_store_n(&oot_vr_radial_side, -1, __ATOMIC_RELAXED);
        oot_vr_radial_unlock();
        rx_vr_radial(NULL, 0);
        return;
    }
    if (now - listed_at > 500000000LL) {
        listed_at = now;
        int child = 1;
        if (!oot_gamedata_bool(c, 0xcea220d3u, &child)) child = 1;
        __atomic_store_n(&oot_vr_radial_child, child, __ATOMIC_RELAXED);
        int list[VR_RADIAL_SLOTS], n = 0, longshot = 0;
        for (int i = 0; i < VR_RADIAL_ITEMS && n < VR_RADIAL_SLOTS; i++) {
            const VrRadialItem *it = &vr_radial_items[i];
            if ((it->age == 1 && child) || (it->age == 2 && !child)) continue;
            const int bottle = !strncmp(it->own, "bottle:", 7) ? it->own[7] - 'A' : -1;   /* (A..E: 0..4) */
            int have = bottle >= 0 ? vr_bottle_owned(c, bottle) : oot_item_count(c, it->own) > 0;
            if (bottle >= 0 && have) {
                const int content = vr_bottle_content(c, bottle);
                oot_vr_radial_bottle[bottle] = content >= -1 ? content : -1;
            }
            if (it->item == 2)   /* (bombs: a bag) */
                have = oot_item_count(c, "Item_BombBag_Small") > 0 || oot_item_count(c, "Item_BombBag_Medium") > 0 ||
                       oot_item_count(c, "Item_BombBag_Large") > 0;
            if (it->item == 6 && have) longshot = 1;
            if (it->item == 5 && longshot) have = 0;
            if (have) list[n++] = i;
        }
        int gear[VR_RADIAL_SLOTS], g = 0, bigger = 0;   /* (bigger: a later blade owned, its earlier forms not shown) */
        int ocarina = 0;   /* (the Ocarina of Time owned: the Fairy Ocarina not shown) */
        for (int i = 0; i < VR_RADIAL_GEAR && g < VR_RADIAL_SLOTS; i++) {
            const VrRadialGear *it = &vr_radial_gear[i];
            if ((it->age == 1 && child) || (it->age == 2 && !child)) continue;
            const int have = oot_item_count(c, it->name) > 0;
            const int blade = !strcmp(it->name, "Weapon_Sword_Biggoron") || !strcmp(it->name, "Weapon_Sword_Giant");
            if (!strcmp(it->name, "Weapon_Sword_Giant") || !strcmp(it->name, "Weapon_Sword_BrokenGiant")) { if (bigger) continue; }
            if (blade && have) bigger = 1;
            if (it->kind == VR_RADIAL_OCARINA && (ocarina || !rx_option(980, 0))) continue;   /* (Ocarina in the hands off) */
            if (it->kind == VR_RADIAL_OCARINA && have) ocarina = 1;
            if (have) gear[g++] = i;
        }
        oot_vr_radial_worn(c, worn);
        oot_vr_radial_publish(list, n, gear, g);   /* (an open menu keeps its own copy: its slots stay put) */
    }
    const int pending = __atomic_exchange_n(&oot_vr_radial_pending, -1, __ATOMIC_RELAXED);
    if (pending >= 0 && pending < VR_RADIAL_ITEMS) {
        const uint64_t slot = recomp_load64(c, 0x84c9e0d0ULL);
        const uint64_t pouch = slot >= 0x80000000ULL && !(slot & 7) && hos_guest_range(slot, 8) ? recomp_load64(c, slot) : 0;
        A64Fn change = a64_lookup(0x81231c00ULL);
        if (pouch >= HOS_HEAP_BASE && change) {
            const int child = __atomic_load_n(&oot_vr_radial_child, __ATOMIC_RELAXED);
            const Ctx saved = *c;
            c->x[31] = (c->x[31] - 0x100) & ~0xfULL;
            c->x[0] = pouch, c->x[1] = (uint64_t)(child ? 1 : 0), c->x[2] = (uint64_t)vr_radial_items[pending].item, c->x[3] = 0, c->x[4] = 1;
            change(c);
            *c = saved;
            hos_log("vr: radial: selected %s (item %d)", vr_radial_items[pending].icon, vr_radial_items[pending].item);
        }
    } else if (pending >= 256 && pending < 256 + VR_RADIAL_GEAR) {
        const VrRadialGear *it = &vr_radial_gear[pending - 256];
        void hos_vr_ocarina_take_from_radial(void);   /* (vr/items/ocarina/ocarina_host.c) */
        if (it->kind == VR_RADIAL_OCARINA) {
            hos_vr_ocarina_take_from_radial();
            hos_log("vr: radial: %s into the right hand", it->name);
        } else if (strcmp(worn[it->kind], it->name)) {   /* (already worn: nothing to do) */
            oot_vr_radial_equip(c, it->name);
            oot_vr_radial_worn(c, worn);
        }
    }
    /* (to the renderer: the open side, the count, the highlighted slot, the current slots as bits (the selected item; the
       worn sword, shield and tunic), and the entries, as indexes into that side's table) */
    int32_t data[4 + VR_RADIAL_SLOTS];
    VrRadialList entries;   /* (taken as one, then read with no lock held: the worn names and the ocarina are the game's) */
    const int side = oot_vr_radial_now(&entries, 0);
    const int shown = side < 0 ? 0 : side;
    const int n = entries.count;
    int32_t current = 0;
    for (int k = 0; k < n; k++) {
        const int i = entries.index[k];
        if (shown == 0 ? vr_radial_items[i].item == __atomic_load_n(&oot_vr_selected_index, __ATOMIC_RELAXED)
                       : vr_radial_gear[i].kind == VR_RADIAL_OCARINA ? hos_vr_ocarina_held()
                       : !strcmp(worn[vr_radial_gear[i].kind], vr_radial_gear[i].name)) current |= 1 << k;
    }
    data[0] = side, data[1] = n;
    data[2] = __atomic_load_n(&oot_vr_radial_highlight, __ATOMIC_RELAXED), data[3] = current;
    for (int k = 0; k < n; k++) {   /* (a bottle's entry carries its content for its icon: index + 256 x (content + 2)) */
        const int i = entries.index[k];
        const int bottle = shown == 0 && !strncmp(vr_radial_items[i].own, "bottle:", 7) ? vr_radial_items[i].own[7] - 'A' : -1;
        data[4 + k] = bottle >= 0 ? i + 256 * (oot_vr_radial_bottle[bottle] + 2) : i;
    }
    rx_vr_radial(data, 4 + n);
}
