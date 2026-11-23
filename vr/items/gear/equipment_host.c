/* Hookshot VR: Link's equipment as the game owns it (sword, shield, bow: drawn or sheathed), read from the game's memory, never
   guessed from rendering, and the first-person show/hide of equipment models.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */

/* Read-only OoT 1.1.1 equipment ownership/state. Embedded by apply-hookshot-vr.py.
   QueryEquipmentUserIsStateDrawnDynamicEquipmentSlot (82a126d0 -> 804c8a60) resolves the eight handles at user+20,
   stride 18, and tests the owned actor's Equipment component: enabled +98 bit 0, current state +9c == 1.
   QueryEquipmentGetCurrentEquipState (82a0e8e0) independently confirms those component offsets.
   No guest functions, inventory writes, actor creation or refcount manipulation are needed here. */
static volatile uint32_t oot_vr_equipment_drawn_now;
static volatile uint64_t oot_vr_sword_actor_seen;   /* (the equipped sword actor: vr/items/sword/sword_host.c places it) */   /* (the last snapshot's drawn bits: 1 sword, 2 shield, 4 bow) */
int hos_vr_sword_drawn(void) { return __atomic_load_n(&oot_vr_equipment_drawn_now, __ATOMIC_RELAXED) & 1; }
static int oot_vr_equipment_ptr(uint64_t a, uint64_t n) {
    return a >= HOS_HEAP_BASE && !(a & 7) && hos_guest_range(a, n) != NULL;
}

static void oot_vr_equipment_name(Ctx *c, uint64_t actor, uint64_t offset, char name[64]) {
    /* QueryGenericGetActorName (81016f6c..81016f84): ActorAccessor SafeString pointers at +20 and +260.
       +10 is NOT the Player object's inline ActorInfo record used by hos_oot_is_player. */
    const uint64_t text = recomp_load64(c, actor + offset);
    memset(name, 0, 64);
    if (text < 0x80000000ULL || !hos_guest_range(text, 64)) return;
    for (int i=0;i<63;i++) {
        const unsigned char ch=recomp_load8(c,text+i);
        if (!ch) break;
        if (ch<32 || ch>126) { name[0]=0; break; }
        name[i]=(char)ch;
    }
}

static uint32_t oot_vr_equipment_snapshot(Ctx *c, uint32_t seq, int active) {
    uint32_t known = 0, drawn = 0, ambiguous = 0;
    uint64_t identity[3] = {0, 0, 0};   /* sword, shield, bow; unknown is NOT a sheathed item */
    uint64_t bracelet = 0;   /* child Link's worn Goron Bracelet (its actor), for the near test (gear_near_host.c) */
    const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    /* ActorAccessor (84b45e18) and Player object (84b6c460) are different objects. The existing player validator
       accepts only the latter. Follow the same ownership relationship as hos_oot_lift_player before reading the actor's table. */
    if (active && oot_vr_equipment_ptr(player, 0x278) && oot_vr_equipment_ptr(object, 0x1d70) && hos_oot_is_player(c, object) &&
        recomp_load64(c, object + 0x1d50 + 0x18) == player) {
        const uint64_t components = recomp_load64(c, player + 0x270);
        const uint64_t user = oot_vr_equipment_ptr(components, 0x208) ? recomp_load64(c, components + 0x200) : 0;
        if (oot_vr_equipment_ptr(user, 0x13c4)) for (int slot = 0; slot < 8; slot++) {
            const uint64_t handle = user + 0x20 + 0x18 * (uint64_t)slot;
            const uint32_t generation = recomp_load32(c, handle + 0x10);
            const uint64_t entry = recomp_load64(c, handle + 8);
            if (generation == UINT32_MAX || !oot_vr_equipment_ptr(entry, 0x50) ||
                recomp_load32(c, entry + 0x48) != generation) continue;
            const uint64_t actor = recomp_load64(c, entry + 0x40);
            if (!oot_vr_equipment_ptr(actor, 0x278)) continue;
            const uint32_t life = recomp_load32(c, actor + 0x2c);
            if (life >= 7 && life < 12) continue;   /* native resolver rejects these lifecycle states */
            char name[64] = {0};
            oot_vr_equipment_name(c,actor,0x260,name);
            if (!name[0]) oot_vr_equipment_name(c,actor,0x20,name);
            int kind = !strncmp(name, "Weapon_Sword_", 13) ? 0 : !strncmp(name, "Weapon_Shield_", 14) ? 1 :
                       !strcmp(name, "Weapon_Range_Bow") || !strcmp(name, "Weapon_Range_SlingShot") ? 2 : -1;   /* (the slingshot is a bow to the game) */
            if (kind < 0 && (!strcmp(name, "Armor_Child_Gauntlet_Goron") || !strcmp(name, "Item_Gauntlet_Goron"))) {
                bracelet = actor;   /* (worn whenever owned: no drawn state) */
                continue;
            }
            if (kind < 0) continue;   /* excludes sheaths, pickups, enemies' equipment and world props */
            const uint64_t parts = recomp_load64(c, actor + 0x270);
            const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
            if (!oot_vr_equipment_ptr(equipment, 0xa0)) continue;
            const uint32_t enabled = recomp_load8(c, equipment + 0x98);
            const uint32_t state = recomp_load32(c, equipment + 0x9c);
            if (!(enabled & 1) || state > 2 || recomp_load32(c, entry + 0x48) != generation ||
                recomp_load64(c, entry + 0x40) != actor) continue;
            /* Ambiguous duplicate owners are not authoritative. Keep native render fallback for that kind. */
            if (identity[kind] && identity[kind] != actor) ambiguous |= 1u << kind;
            if (ambiguous & (1u << kind)) { known &= ~(1u << kind); continue; }
            identity[kind] = actor;
            known |= 1u << kind;
            if (state == 1 || (recomp_load8(c, user + 0x13bc + slot) & 2)) drawn |= 1u << kind;
        }
    }
    static volatile uint64_t last_signature = UINT64_MAX;
    static volatile unsigned reports;
    const uint64_t signature = identity[0] ^ (identity[1] << 1) ^ ((uint64_t)known << 60) ^ ((uint64_t)(drawn & known) << 56);
    if (__atomic_exchange_n(&last_signature, signature, __ATOMIC_RELAXED) != signature &&
        __atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) < 128)
        hos_log("vr: native equipment pose %u known %#x drawn %#x sword %#llx shield %#llx", seq, known, drawn & known,
                (unsigned long long)identity[0], (unsigned long long)identity[1]);
    __atomic_store_n(&oot_vr_equipment_drawn_now, drawn & known, __ATOMIC_RELAXED);
    __atomic_store_n(&oot_vr_sword_actor_seen, (known & 1) ? identity[0] : 0, __ATOMIC_RELAXED);
    oot_vr_gear_cull_publish(c, known, drawn & known, identity);
    static int bracelet_off = -1;   /* (RX_VR_NO_BRACELET=1: the bracelet left to the game, as vr/items/gear/equipment_skin.inc) */
    if (bracelet_off < 0) bracelet_off = env_on("RX_VR_NO_BRACELET");
    /* (not among the slots in the 18 Nov headset run: found by its own update on Link, vr/items/sword/sword_host.c) */
    uint64_t hos_vr_bracelet_on_link(void);
    if (active && !bracelet) bracelet = hos_vr_bracelet_on_link();
    /* (the bow or slingshot out: drawn in a VR hand, tested by the game at its own: gear_near_host.c hos_vr_held_keep) */
    void hos_vr_held_keep(Ctx *c, uint64_t actor);
    if (active && (drawn & known & 4)) hos_vr_held_keep(c, identity[2]);
    oot_vr_gear_worn_publish(c, bracelet_off ? 0 : bracelet);
    rx_vr_equipment(seq, known & 3, drawn & known & 3, identity);
    return known | ((drawn & known) << 8); // bow identity is needed by host state gating, not the rig's two-item ABI
}

/* Action nibble 0x400 is shared with sword/shield actions. Prefer actual owned bow state; if unknown, retain the old
   compatibility path only when no native drawn sword rules it out. Selection alone is not proof that a bow is held. */
static int oot_vr_bow_state_owned(uint32_t action, uint32_t equipment) {
    /* (0x441 at rest, 0x442/0x446 raising and drawn, 0x2000041 taking it out; 0x41 is the same take-out without the aiming
       stance, the grip's ZR let go as soon as the bow is out) */
    const int candidate = (action & 0xf00u) == 0x400u || (action & 0xfffu) == 0x041u;
    if (!candidate) return 0;
    if (equipment & 4u) return !!(equipment & 0x400u);
    return !(equipment & 0x100u);
}

/* (OoT VR) main+0x2dfe260 = f_82e1e260(EquipmentUser *user, bool *visible, int slot): the request behind the event node
   OneShotEquipmentUserRequestSetVisible (execute 0x828f3700, params DynamicEquipmentSlot / IsVisible). Called after its
   first instruction (nx2/oot_wide.py site). In first person ("Keep Link's gear visible"), a request to HIDE a drawn
   sword or shield of Link's own EquipmentUser is turned into "visible" and logged: the swing animations hide the real
   blade/shield for a few frames (vr-item-dump-170228872042332: one colour-less run per swing, inside the attack window),
   which in first person is a blink in the VR hand. Every other request is untouched. */
void hos_vr_equipment_visible(Ctx *c) {
    HookshotVrHead h;
    const uint64_t user = c->x[0], flag = c->x[1];
    const uint32_t slot = (uint32_t)c->x[2];
    if (!flag || slot >= 8 || !rx_option(902, 0) || !rx_vr_head(&h) || !h.active || h.mode != 1) return;
    const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    if (!oot_vr_equipment_ptr(player, 0x278)) return;
    const uint64_t components = recomp_load64(c, player + 0x270);
    if (!oot_vr_equipment_ptr(components, 0x208) || recomp_load64(c, components + 0x200) != user) return;
    const uint64_t handle = user + 0x20 + 0x18 * (uint64_t)slot;
    const uint32_t generation = recomp_load32(c, handle + 0x10);
    const uint64_t entry = recomp_load64(c, handle + 8);
    if (generation == UINT32_MAX || !oot_vr_equipment_ptr(entry, 0x50) || recomp_load32(c, entry + 0x48) != generation) return;
    const uint64_t actor = recomp_load64(c, entry + 0x40);
    if (!oot_vr_equipment_ptr(actor, 0x278)) return;
    char name[64] = {0};
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    if (strncmp(name, "Weapon_Sword_", 13) && strncmp(name, "Weapon_Shield_", 14)) return;
    const uint64_t parts = recomp_load64(c, actor + 0x270);
    const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
    const int drawn = (oot_vr_equipment_ptr(equipment, 0xa0) && recomp_load32(c, equipment + 0x9c) == 1) ||
                      (recomp_load8(c, user + 0x13bc + slot) & 2);
    const int visible = recomp_load8(c, flag) & 1;
    static unsigned told;
    if (told < 200) told++, hos_log("vr: equipment visibility request slot %u %s %s%s", slot, name, visible ? "show" : "HIDE",
                                    !visible && drawn ? " -> kept visible (first person, drawn)" : drawn ? "" : " (not drawn: untouched)");
    if (!visible && drawn) recomp_store8(c, flag, 1);
}
