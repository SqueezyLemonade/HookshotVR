/* Hookshot VR: putting away what Link holds with the game's own call, never its button (game side). The controls' side,
   one put-away from the let-go until the game has it away, is vr/items/gear/put_away_stow.inc (the sword, the Deku
   Stick, the bow and slingshot, the hookshot, the boomerang). Option 945 (Put away: Game call) is fixed: no Button way.

   Why: the game's B (pad SOUTH) puts a drawn weapon away only while Link stands with it out and nothing is beside him:
   moving, B rolls; beside a person, a chest or a door, B talks, opens or reads. So the button put-away had to wait for a
   moment when B could only mean "put away", and in a busy place it never came (headset 18 Nov, Goron City: a burning Deku
   Stick let go, the VR stick hidden while it waited, its fire and embers left at Link's game hand). After an event Link's
   action state was even 0 with the stick still out (the GoronCity_LargePot event: 0x408 -> 0), where the button rule
   (exactly 0x408) never pressed at all.

   The call (game 1.1.1, found 18 Nov from the AI node names in the image; the evidence is in docs/claude-handoff.md):
   f_804c8690(Link's actor) is what the AI node OneShotPlayerRequestAllEquipmentSheathed runs
   (its execute f_8292caa0: the node's actor, then this call), the one events use to put Link's gear away. It takes Link's
   EquipmentUser (actor +0x270 components, +0x200) and, for each of its slots 0, 1, 3 and 4 the game's own test calls drawn
   (f_804c8a60, QueryEquipmentUserIsStateDrawnDynamicEquipmentSlot: the slot's Equipment component enabled, +0x98 bit 0, in
   state 1, +0x9c), it calls the slot's sheathe (f_80e54c70). Headless, 18 Nov: the Kokiri Sword is slot 0, the Deku Shield
   slot 4, the Deku Stick slot 1 (the held item's). So it is item-agnostic: whatever the game has out goes away as an event
   puts it away, without Link's put-away animation (first person shows the VR hands, not it): action state 0x408 -> 0, not
   B's 0x408 -> 0x80000000 -> 0. The sword's shield goes with it, as with B (both drawn by the attack button, both away by
   B: headset log 18 Nov, "drawn 0x3" -> "drawn 0"). The stick actor stays in its slot, not out, as after B; the next take
   brings the same actor out again.
   The other nodes, not used: OneShotPlayerRequestEquipmentSheathedOnly (f_83044350(actor, kind): kind 1-3 slot 0, 4 slot 3,
   else slot 4: one slot, for an item that must not take the shield with it) and ExecutePlayerPutAway (the B put-away's sub action, Player.sub.action.putaway: its animation first).

   Use, from any thread (the controllers' code runs on the input thread, not the game's): through put_away_stow.inc
   (vr_stow_start at the let-go, vr_stow_step every read, vr_stow_cancel if taken again), or directly:
       hos_vr_put_away_held();    (asks; always 1)
       hos_vr_put_away_cancel();  (a request not yet made, withdrawn: the item was taken again)
   hos_vr_put_away_held() only asks; the call is made on Link's next update (host/hos_main.c hos_oot_lift_player, VR HOOK),
   on the game's thread (headless 18 Nov: put away 13-16 ms after asking, standing or running). Ask again (every 300 ms or so) while the item's own reader still says it is out: a request made
   while the game is drawing it (the take's ZR still held) can be undone by the game. Each request and what it did is logged
   (vr: put away: ...).

   RX_VR_PUT_AWAY_TEST=1 (headless): whenever Link has had something drawn for kVrPutAwayTestNs, the put-away is asked, at
   most kVrPutAwayTestMax times a run; =2 also selects Deku Sticks first (with RX_VR_HOOK_TEST=1 and RX_VR_RADIAL_TEST=3,
   which run the radial's selection headless). The input script draws: ZR the selected item, Y the sword.

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/items/bomb/bomb_host.c (vr_bomb_call),
   vr/items/gear/equipment_host.c (its readers) and vr/menu/radial_host.c (the test's selection). Not compiled on its own. */
#define kVrPutAwayCall 0x804c8690ULL        /* (f_804c8690(actor): OneShotPlayerRequestAllEquipmentSheathed's call) */
#define kVrPutAwaySlots 8                   /* (Link's EquipmentUser slots: handles at +0x20, every 0x18) */
#define kVrPutAwayTestNs 2000000000LL
#define kVrPutAwayTestMax 6

static volatile int vr_put_away_asked;   /* asked by hos_vr_put_away_held, not yet made on Link's update */
int hos_vr_put_away_held(void) {
    __atomic_store_n(&vr_put_away_asked, 1, __ATOMIC_RELEASE);
    return 1;
}
void hos_vr_put_away_cancel(void) { __atomic_store_n(&vr_put_away_asked, 0, __ATOMIC_RELEASE); }

/* the EquipmentUser slots the game itself calls drawn (its test, f_804c8a60: enabled, state 1) as bits; name: the first
   drawn one's actor name, for the log */
static uint32_t vr_held_slots(Ctx *c, uint64_t player, char name[64]) {
    name[0] = 0;
    if (!oot_vr_equipment_ptr(player, 0x278)) return 0;
    const uint64_t components = recomp_load64(c, player + 0x270);
    const uint64_t user = oot_vr_equipment_ptr(components, 0x208) ? recomp_load64(c, components + 0x200) : 0;
    if (!oot_vr_equipment_ptr(user, 0x20 + 0x18 * kVrPutAwaySlots)) return 0;
    uint32_t drawn = 0;
    for (int slot = 0; slot < kVrPutAwaySlots; slot++) {
        const uint64_t handle = user + 0x20 + 0x18 * (uint64_t)slot;
        const uint32_t generation = recomp_load32(c, handle + 0x10);
        const uint64_t entry = recomp_load64(c, handle + 8);
        if (generation == UINT32_MAX || !oot_vr_equipment_ptr(entry, 0x50) || recomp_load32(c, entry + 0x48) != generation) continue;
        const uint64_t actor = recomp_load64(c, entry + 0x40);
        if (!oot_vr_equipment_ptr(actor, 0x278)) continue;
        const uint64_t parts = recomp_load64(c, actor + 0x270);
        const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
        if (!oot_vr_equipment_ptr(equipment, 0xa0) || !(recomp_load8(c, equipment + 0x98) & 1) ||
            recomp_load32(c, equipment + 0x9c) != 1) continue;
        drawn |= 1u << slot;
        if (!name[0]) {
            oot_vr_equipment_name(c, actor, 0x260, name);
            if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        }
    }
    return drawn;
}

/* the test's selection: Deku Sticks through the radial menu's own pending selection (RX_VR_PUT_AWAY_TEST=2) */
static void vr_put_away_test_select(void) {
    for (int i = 0; i < VR_RADIAL_ITEMS; i++)
        if (!strcmp(vr_radial_items[i].icon, "Item_DekuStick")) {
            __atomic_store_n(&oot_vr_radial_pending, i, __ATOMIC_RELAXED);
            hos_log("vr: put away test: Deku Sticks selected");
        }
}

/* (host/hos_main.c hos_oot_lift_player, VR HOOK: every actor's movement update) on Link's: a put-away asked is made here,
   on the game's thread, and its result logged on the update after */
void hos_vr_put_away_actor_update(Ctx *c, uint64_t actor) {
    if (!actor || actor != __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED)) return;
    static int test = -1;
    if (test < 0) test = vr_research_int("RX_VR_PUT_AWAY_TEST", 0);
    static int64_t called_at;           /* (the last call, its result not yet logged) */
    static uint32_t drawn_before, action_before;
    const int64_t now = hos_now_ns();
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    const uint32_t action = object ? recomp_load32(c, object + 0x1f60) : 0;
    char name[64];
    const uint32_t drawn = vr_held_slots(c, actor, name);
    if (called_at) {   /* (the update after the call: what it did) */
        hos_log("vr: put away: %s %d ms after the game's call (drawn slots %#x -> %#x, action state %#x -> %#x)",
                drawn & drawn_before ? "still drawn" : "done", (int)((now - called_at) / 1000000), drawn_before, drawn,
                action_before, action);
        called_at = 0;
    }
    if (test) {
        static int64_t first, drawn_since;
        static int selected, asked;
        if (!first) first = now;
        if (test == 2 && !selected && now - first > 16000000000LL) selected = 1, vr_put_away_test_select();
        if (!drawn) drawn_since = 0;
        else if (!drawn_since) drawn_since = now;
        else if (now - drawn_since > kVrPutAwayTestNs && asked < kVrPutAwayTestMax) {
            asked++, drawn_since = 0;
            hos_log("vr: put away test: %s drawn for %d s: asking (%d of %d)", name, (int)(kVrPutAwayTestNs / 1000000000LL),
                    asked, kVrPutAwayTestMax);
            __atomic_store_n(&vr_put_away_asked, 1, __ATOMIC_RELEASE);
        }
    }
    if (!__atomic_exchange_n(&vr_put_away_asked, 0, __ATOMIC_ACQUIRE)) return;
    if (!drawn) return;   /* (nothing out: nothing to do; the item's own reader may lag by an update) */
    hos_log("vr: put away: the game's call for %s (drawn slots %#x, action state %#x)", name[0] ? name : "?", drawn, action);
    vr_bomb_call(c, kVrPutAwayCall, actor, 0, 0, 0, NULL, NULL);
    called_at = now, drawn_before = drawn, action_before = action;
}
