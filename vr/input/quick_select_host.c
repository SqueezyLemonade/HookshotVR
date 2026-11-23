/* Hookshot VR: The item quick select from the left stick in first person ("Stick click quick select"), and a bounded research
   reader of the selected item.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* (OoT VR) The item quick select from the left stick, first person ("Stick click quick select"). The game's own quick select
   (UI ShortCut_01: a sliding list; the game pauses; holding L, mapped bit 6, opens it and letting go selects) is driven as
   the planned radial will be: a clean left stick click (pressed and let go within 0.4 s with no D-pad on the way: holding it
   is the D-pad chord) holds L for the game; a held direction moves through the list; the stick back to centre after that
   lets L go, and the game selects. A second click, or 15 s, closes it the same way. The left stick click itself no longer
   reaches the game in first person. While it is open the headset shows the game's picture as a flat screen in front
   (hos_oot_vr_view), instead of stretched over the whole view. */
static volatile int oot_vr_quick_open;
static uint64_t oot_vr_radial_controls(uint64_t buttons);   /* (vr/menu/radial_host.c: "Stick click menus: VR radial") */
uint64_t hos_oot_vr_menu_controls(uint64_t buttons, const int32_t stick[4]) {
    const uint64_t LS = 1ULL << 4, L = 1ULL << 6, DPAD = 0xfULL << 12;
    static int ls_was, pending, open, deflected;
    static int64_t pressed_at, opened_at;
    const int64_t now = hos_now_ns();
    HookshotVrHead h;
    if (rx_option(901, 0) == 1 && rx_vr_head(&h) && h.active && h.mode == 1) return oot_vr_radial_controls(buttons);
    if (rx_option(901, 0) != 2 || !rx_vr_head(&h) || !h.active || h.mode != 1) {   /* (2: the game's quick select) */
        if (open) open = 0, __atomic_store_n(&oot_vr_quick_open, 0, __ATOMIC_RELEASE);
        pending = 0, ls_was = (buttons & LS) != 0;
        return buttons;
    }
    const int ls = (buttons & LS) != 0;
    uint64_t result = buttons & ~LS;
    const float x = stick[0] / 32767.0f, y = stick[1] / 32767.0f, m = sqrtf(x * x + y * y);
    if (ls && !ls_was) pending = 1, pressed_at = now;
    if (pending && (buttons & DPAD)) pending = 0;   /* (the D-pad chord: not a click) */
    int click = 0;
    if (!ls && ls_was && pending) click = now - pressed_at < 400000000LL;
    if (!ls) pending = 0;
    ls_was = ls;
    if (!open && click) {
        open = 1, deflected = 0, opened_at = now;
        __atomic_store_n(&oot_vr_quick_open, 1, __ATOMIC_RELEASE);
        hos_log("vr: quick select opened (the game's L held)");
    } else if (open) {
        if (m > .6f) deflected = 1;
        const char *why = click ? "stick clicked again" : deflected && m < .25f ? "stick let go" :
                          now - opened_at > 15000000000LL ? "15 s" : NULL;
        if (why) {
            open = 0;
            __atomic_store_n(&oot_vr_quick_open, 0, __ATOMIC_RELEASE);
            hos_log("vr: quick select closed (%s): the game's L let go, its selection", why);
        }
    }
    if (open) result |= L;
    return result;
}

/* (OoT VR, research for the radial, bounded) the game's selected assignable item as QueryGetSelectedAssignableItem
   (0x82a3d940) reads it: the selection byte at [*0x84e9cfa8] + 0x5c0, through the pouch (f_8019b6e0(pouch, byte, 0): an
   item index, 0..0x35) to its name (the table at *0x84ca1140: +0x250 + 4 * (index + 1), an offset from the table).
   Logged when it changes: which value the quick select writes, which items it offers by age and room. */
/* oot_vr_selected_bow: the selected item is the bow (its arrows), 1; another item, 0; unknown, -1. "Grip holds the bow"
   then works from the selection itself, with no ZR press to learn from; oot_vr_selection counts changes */
static volatile int oot_vr_selected_bow = -1, oot_vr_selection;
static volatile int oot_vr_selected_index = -1;   /* the selected item's number (vr/menu/radial_host.c marks it) */
int hos_vr_selected_is_bomb;   /* the last resolved selection is the bomb (not bombchus): vr/items/bomb/bomb_host.c */
int hos_vr_selected_is_hookshot;   /* ... the hookshot or longshot: vr/items/hookshot/hookshot_host.c */
int hos_vr_selected_is_slingshot;   /* ... the Fairy Slingshot (the bow's controls, its own model): vr/items/slingshot */
int hos_vr_selected_is_stick;   /* ... the Deku Stick (child Link: vr/items/deku_stick, vr/input/controllers_host.c) */
int hos_vr_selected_is_boomerang;   /* ... the Boomerang (child Link: vr/items/boomerang) */
int hos_vr_stick_selected(void) { return __atomic_load_n(&hos_vr_selected_is_stick, __ATOMIC_RELAXED); }
static void oot_vr_note_selected_item(Ctx *c) {
    static int last = -2, last_index = -2, last_bow = -2;
    static uint64_t last_holder;
    static int64_t checked_at;
    static unsigned told;
    const uint64_t holder = recomp_load64(c, 0x84e9cfa8ULL);
    if (holder < HOS_HEAP_BASE) return;
    const int sel = recomp_load8(c, holder + 0x5c0);
    /* Inventory may not exist at the first camera update. An unresolved item MUST
       be retried even with the same slot byte. Also recheck a resolved slot: the
       quick menu can replace its contents without changing the slot number. */
    const int64_t now = hos_now_ns();
    if (holder == last_holder && sel == last && checked_at && now-checked_at < 100000000LL) return;
    checked_at = now;
    const uint64_t pouch_slot = recomp_load64(c, 0x84c9e0d0ULL);
    /* The singleton SLOT lives in static BSS (observed 84e98e28), not the heap.
       Only the actual pouch object is heap allocated. Validate mapped memory. */
    const uint64_t pouch = pouch_slot >= 0x80000000ULL && !(pouch_slot & 7) && hos_guest_range(pouch_slot,8)
                         ? recomp_load64(c, pouch_slot) : 0;
    A64Fn index_of = a64_lookup(0x8019b6e0ULL);
    int index = -1;
    if (pouch >= HOS_HEAP_BASE && index_of) {
        const Ctx saved = *c;
        c->x[31] = (c->x[31] - 0x80) & ~0xfULL;
        c->x[0] = pouch, c->x[1] = (uint64_t)sel, c->x[2] = 0;
        index_of(c);
        index = (int)(int32_t)c->x[0];
        *c = saved;
    }
    char name[64] = "?";
    const uint64_t table = recomp_load64(c, 0x84ca1140ULL);
    if (index >= 0 && index <= 0x35 && table >= 0x80000000ULL) {
        const int32_t off = (int32_t)recomp_load32(c, table + 0x250 + 4 * (uint64_t)(index + 1));
        if (off >= 0)
            for (int i = 0; i < 63; i++) { name[i] = (char)recomp_load8(c, table + (uint64_t)off + i); name[i + 1] = 0; if (!name[i]) break; }
    }
    int bow = -1;
    if (index >= 0) __atomic_store_n(&oot_vr_selected_index, index, __ATOMIC_RELAXED);
    if (index >= 0 && name[0] != '?' && name[0]) {
        char lower[64];
        for (int i = 0; i < 64; i++) lower[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
        hos_vr_selected_is_bomb = strstr(lower, "bomb") && !strstr(lower, "bombchu");
        hos_vr_selected_is_hookshot = strstr(lower, "hookshot") || strstr(lower, "longshot");
        bow = (strstr(lower, "bow") || strstr(lower, "arrow")) && !strstr(lower, "rainbow") && !strstr(lower, "elbow");
        /* (the slingshot is a bow to the game, its BowParam: the bow's grips, string and shot, vr/items/slingshot) */
        hos_vr_selected_is_slingshot = strstr(lower, "slingshot") || strstr(lower, "dekuseed");
        __atomic_store_n(&hos_vr_selected_is_stick, strstr(lower, "dekustick") != NULL, __ATOMIC_RELAXED);   /* (the selection reader's "DekuStick") */
        __atomic_store_n(&hos_vr_selected_is_boomerang, strstr(lower, "boomerang") != NULL, __ATOMIC_RELAXED);
        if (hos_vr_selected_is_slingshot) bow = 1;
    }
    /* Unknown is not evidence of a different item. Retain the last resolved
       selection through a transient loading/menu read; explicit valid non-bow
       selection revokes ownership. A different holder/slot is unknown immediately. */
    if (bow < 0 && holder == last_holder && sel == last && last_bow >= 0) return;
    const int changed = holder != last_holder || sel != last || index != last_index || bow != last_bow;
    last_holder = holder, last = sel, last_index = index, last_bow = bow;
    if (!changed) return;
    __atomic_store_n(&oot_vr_selected_bow, bow, __ATOMIC_RELEASE);
    __atomic_add_fetch(&oot_vr_selection, 1, __ATOMIC_RELEASE);
    if (told++ < 300)
        hos_log("vr: selected assignable item: byte %d, item %d (%s)%s%s", sel, index, name,
                __atomic_load_n(&oot_vr_quick_open, __ATOMIC_ACQUIRE) ? " [quick select open]" : "",
                hos_vr_selected_is_slingshot ? " [the slingshot: the grips take it, as the bow]" : bow == 1 ? " [the bow: the grips take it]" :
                hos_vr_selected_is_stick ? " [the Deku Stick: a grip takes it (Deku Sticks in the hands)]" :
                hos_vr_selected_is_boomerang ? " [the Boomerang: a grip takes it (Boomerang in the hands)]" : "");
}
