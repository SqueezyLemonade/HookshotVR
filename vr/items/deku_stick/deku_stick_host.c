/* Hookshot VR: Deku Sticks in the VR hands (game side), first person (VR tab: Deku Sticks in the hands, option 930).

   Child Link's Deku Stick is the game's own item and actor, held and burnt by the game's own logic; the VR mod only moves it
   to the VR hand, as it does the sword (vr/items/sword/sword_host.c). The controls (a grip takes it out, a swing attacks,
   letting go puts it away) are vr/input/controllers_host.c vr_stick; its model in the VR hand is
   vr/items/deku_stick/held_stick.inc.

   What is known (game 1.1.1; the player's own Weapon_DekuStick actor pack, read-only, and headset logs, 18 Nov):
   - Taken out, the stick is the actor Weapon_DekuStick (ModelInfo Item_DekuStick, its WeaponParam a long sword's: the
     Weapon_Sword_Wood_Base family), one of Link's EquipmentUser slots like the sword (vr/items/gear/equipment_host.c reads
     them): its Equipment component's state 1 while it is out. The selected stick comes out with the item button ZR (headset
     log: "DekuStick" selected, ZR -> action state 0 -> 0x8 -> 0x408, the sword's own out state), and SOUTH standing puts it
     away (0x408 -> 0x80000000 with SOUTH).
   - Its attack sensor (Phive RigidBodySensorParam WeaponAtk, shape Item_DekuStick_Atk: a capsule of radius 0.2 m along
     the actor's z from 0 to 1.2 m) and its chemistry sensor (WeaponChemical, layer ChemicalObject, shape
     Item_DekuStick_Chemical: radius 0.1 m along z from 0.65 to 1.0 m: the head) both track the actor itself
     (RigidBodyControllerSensorParam IsTrackingActor). Its chemistry (ChemicalParam Weapon_Chemical_Wood: material
     weapon_wood, burns, FixedBurningDuration 1440, RequestDeathIfBurned) is what catches fire from a flame and sets fire
     to what the burning head touches (webs, torches): the game's own fire, nothing faked here.
   So, as the sword: while a VR hand holds the stick and the game has it out, the stick actor is placed every update on the
   VR hand (vr_held_blade_pose: origin at the grip, z along the fist leaned by the Sword angle), and its sensors follow.
   The VR hand holds the stick near its butt, kVrStickGripZ behind its origin (the drawn stick: STICK_GRIP in
   vr/tools/generate/hand_sockets.py), so the actor is placed that far up the fist, its sensors along the drawn stick.
   Not yet known: whether the game's own attachment to Link's hand, every frame too, puts it back before its sensors and
   chemistry are tested (the sword's log shows the game's place at its next update); the log says (vr: deku stick actor:
   placed at ..., found at ... next update), and whether it lights (vr: deku stick: chemistry ...).
   - Lit or not: the stick's AI (Weapon_DekuStick.root.ainb) asks QueryChecmicalGetState (sic; its calc f_808f7df0) for
     IsBurning / IsBurnt / IsExplode: actor +0x270 (components) +0x198 (the chemical component), a list of chemical objects
     (head +0x40, first node +0x48, the node's offset in its element at +0x54, the object at element +0x10, the next node at
     node +8); an object counts when +0x268 bit 2 is set and +0x44 has neither 0x4 nor 0x10, and its state is 1 burning,
     2 burnt, 3 exploded. The query takes that state from f_808f80f0, which answers the stored byte +0x35 when it has
     nothing newer (0xff): this reads +0x35, so it may lag the query by a frame. Read for the log only; nothing depends on it.

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/items/sword/sword_host.c (vr_held_blade_pose)
   and vr/items/hookshot/hookshot_host.c; it uses vr/items/gear/equipment_host.c's readers and gear_near_host.c's near-plane
   slot. Not compiled on its own. */
#define kVrStickActorName "Weapon_DekuStick"
#define kVrStickItemName "Item_DekuStick"   /* (its ActorName: accepted too, in case the name read is that one) */
#define kVrStickChemicalComponent 0x198   /* (components table +0x198: the chemical component, f_808f7df0) */
#define kVrStickChemicalObjectsMax 8      /* (a stick has one: walk at most this many) */
#define kVrStickGripZ 0.22f               /* (m: the stick's origin this far up the fist from the VR hand: -STICK_GRIP[2]) */
int hos_vr_stick_holder(void);            /* (vr/input/controllers_host.c: the VR hand holding the stick, or -1) */

static volatile uint64_t oot_vr_stick_actor;   /* the game's stick actor in Link's equipment slots (0: none) */
static volatile int oot_vr_stick_out_now;      /* the game has it out (its Equipment state 1) */
static volatile int oot_vr_stick_lit_now = -1; /* its chemistry: 0 unlit, 1 burning, 2 burnt, 3 exploded, -1 not read */
int hos_vr_stick_out(void) { return __atomic_load_n(&oot_vr_stick_out_now, __ATOMIC_RELAXED); }
int hos_vr_stick_lit(void) { return __atomic_load_n(&oot_vr_stick_lit_now, __ATOMIC_RELAXED); }

/* The stick actor in Link's EquipmentUser slots (as equipment_host.c and hookshot_host.c read them), or 0; *out: its
   Equipment component says it is out (state 1, or the slot's drawn flag) */
static uint64_t oot_vr_stick_find(Ctx *c, int *out, int list) {
    *out = 0;
    const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    if (!oot_vr_equipment_ptr(player, 0x278)) return 0;
    const uint64_t components = recomp_load64(c, player + 0x270);
    const uint64_t user = oot_vr_equipment_ptr(components, 0x208) ? recomp_load64(c, components + 0x200) : 0;
    if (!oot_vr_equipment_ptr(user, 0x13c4)) return 0;
    for (int slot = 0; slot < 8; slot++) {
        const uint64_t handle = user + 0x20 + 0x18 * (uint64_t)slot;
        const uint32_t generation = recomp_load32(c, handle + 0x10);
        const uint64_t entry = recomp_load64(c, handle + 8);
        if (generation == UINT32_MAX || !oot_vr_equipment_ptr(entry, 0x50) || recomp_load32(c, entry + 0x48) != generation) continue;
        const uint64_t actor = recomp_load64(c, entry + 0x40);
        if (!oot_vr_equipment_ptr(actor, 0x278)) continue;
        const uint32_t life = recomp_load32(c, actor + 0x2c);
        if (life >= 7 && life < 12) continue;   /* (being destroyed: as the equipment reader) */
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        if (list) hos_log("vr: deku stick: not found; Link's equipment slot %d holds %s", slot, name[0] ? name : "(no name)");
        if (strcmp(name, kVrStickActorName) && strcmp(name, kVrStickItemName)) continue;
        const uint64_t parts = recomp_load64(c, actor + 0x270);
        const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
        if (oot_vr_equipment_ptr(equipment, 0xa0))
            *out = (recomp_load8(c, equipment + 0x98) & 1) &&
                   (recomp_load32(c, equipment + 0x9c) == 1 || (recomp_load8(c, user + 0x13bc + slot) & 2));
        return actor;
    }
    return 0;
}

/* The stick's chemistry state (1 burning, 2 burnt, 3 exploded, 0 none of these), read as QueryChecmicalGetState reads it
   (see the top), or -1 when its chemical component cannot be read */
static int oot_vr_stick_chemistry(Ctx *c, uint64_t actor) {
    const uint64_t components = recomp_load64(c, actor + 0x270);
    if (!oot_vr_equipment_ptr(components, kVrStickChemicalComponent + 8)) return -1;
    const uint64_t chem = recomp_load64(c, components + kVrStickChemicalComponent);
    if (!oot_vr_equipment_ptr(chem, 0x58)) return -1;
    const uint64_t head = chem + 0x40;
    const int64_t offset = (int32_t)recomp_load32(c, chem + 0x54);
    int state = 0;
    uint64_t node = recomp_load64(c, chem + 0x48);
    for (int n = 0; node != head && n < kVrStickChemicalObjectsMax; n++) {
        if (!oot_vr_equipment_ptr(node, 0x10) || !oot_vr_equipment_ptr(node - offset, 0x18)) return -1;
        const uint64_t object = recomp_load64(c, node - offset + 0x10);
        if (oot_vr_equipment_ptr(object, 0x270) && (recomp_load8(c, object + 0x268) & 4) &&
            !((recomp_load32(c, object + 0x44) & 0xffff) & 0x14)) {
            const int s = (int8_t)recomp_load8(c, object + 0x35);
            if (s == 1 || (s > 1 && state != 1)) state = s;   /* (burning wins: any object alight is a lit stick) */
        }
        node = recomp_load64(c, node + 8);
    }
    return state;
}

/* (host/hos_main.c hos_oot_lift_player, VR HOOK: every actor's movement update) Link's update: which stick actor is out,
   lit or not, logged once per change. The stick's update: placed on the VR hand while one holds it. */
void hos_vr_stick_actor_update(Ctx *c, uint64_t actor) {
    if (!actor) return;
    if (actor == __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED)) {
        int out = 0;
        uint64_t stick = rx_option(930, 0) ? oot_vr_stick_find(c, &out, 0) : 0;
        {   /* (bounded: the stick selected and Link's weapon out, but no stick in his slots: what the slots hold, once a
               second at most, 4 times a run, so a different actor name shows in the log) */
            int hos_vr_stick_selected(void);
            static int64_t listed_at;
            static unsigned lists;
            const int64_t now = hos_now_ns();
            if (!stick && rx_option(930, 0) && hos_vr_stick_selected() && hos_vr_link_action() == 0x408u && lists < 4 &&
                now - listed_at > 1000000000LL)
                lists++, listed_at = now, stick = oot_vr_stick_find(c, &out, 1);
        }
        const int lit = stick ? oot_vr_stick_chemistry(c, stick) : -1;
        static uint64_t stick_was;
        static int out_was, lit_was = -2;
        if (stick != stick_was || out != out_was) {
            const int held = hos_vr_stick_holder();
            hos_log("vr: deku stick: %s%s (actor %#llx)%s", stick ? "the game's stick " : "no stick",
                    stick ? (out ? "out in Link's hand" : "not out") : "", (unsigned long long)stick,
                    !out && out_was && held >= 0 ? ": gone while a VR hand holds it (burnt up, put away or broken)" : "");
        }
        if (stick && lit != lit_was)
            hos_log("vr: deku stick: chemistry %s%s", lit == 1 ? "BURNING (lit)" : lit == 2 ? "burnt" : lit == 3 ? "exploded" :
                                                      lit == 0 ? "unlit" : "not readable", out ? "" : " (not out)");
        if (stick && !out && out_was && lit == 1)   /* (put away, but its fire not out with it: what the put-away left) */
            hos_log("vr: deku stick: put away still BURNING: its fire was not put out with it");
        stick_was = stick, out_was = out, lit_was = stick ? lit : -2;
        __atomic_store_n(&oot_vr_stick_lit_now, lit, __ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_stick_out_now, out, __ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_stick_actor, out ? stick : 0, __ATOMIC_RELAXED);
        /* (its model kept from the game's near-plane test while a VR hand holds it: the hookshot's slot, never both at once
           (one item is selected), vr/items/gear/gear_near_host.c; the game's own stick sits in Link's game hand, which the
           headset can be in front of) */
        if (out && hos_vr_stick_holder() >= 0) {
            static int64_t looked;
            static uint64_t model;
            const int64_t now = hos_now_ns();
            if (now - looked > 300000000LL) looked = now, model = oot_vr_gear_resolve(c, stick);
            __atomic_store_n(&oot_vr_gear_hook_model, model, __ATOMIC_RELEASE);
            __atomic_store_n(&oot_vr_gear_hook_time, now, __ATOMIC_RELEASE);
        }
        return;
    }
    if (actor != __atomic_load_n(&oot_vr_stick_actor, __ATOMIC_RELAXED)) return;
    static int64_t logged;
    static float set_at[3];
    static int have_set;
    const int hand = hos_vr_stick_holder();
    if (hand < 0 || !hos_vr_stick_out() || !rx_option(930, 0)) { have_set = 0; return; }
    const int64_t now = hos_now_ns();
    if (have_set && now - logged > 1000000000LL) {   /* (once a second: did the last placement hold until this update?) */
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = vr_bomb_f(c, actor + 796 + 4 * k);
        hos_log("vr: deku stick actor: placed at %.2f %.2f %.2f, found at %.2f %.2f %.2f next update (%s)", set_at[0],
                set_at[1], set_at[2], p[0], p[1], p[2], hos_vr_stick_lit() == 1 ? "lit" : "unlit");
        logged = now;
    }
    float origin[3], rot[9];
    if (!vr_held_blade_pose(hand, origin, rot)) return;
    for (int k = 0; k < 3; k++) origin[k] += rot[3 * k + 2] * kVrStickGripZ;   /* (up the fist: rot's third column, the stick) */
    vr_blade_weapon_update(c, actor, hand, origin, rot);   /* (Sword hits: Blade, vr/items/sword/blade_hits.inc: its swings hit too) */
    vr_bomb_place(c, actor, origin, rot);
    memcpy(set_at, origin, sizeof set_at), have_set = 1;
}
