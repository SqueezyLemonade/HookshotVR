/* Hookshot VR: the hookshot and longshot in a VR hand, first person (VR tab: Hookshot in the hands).

   Squeeze a grip with the hookshot (or longshot) selected: the game takes it out in that hand (ZR held until it is out, then
   let go: it rests). That hand's trigger is the game's ZR: held, it aims; let go, it fires. Letting go of the grip while it
   is out puts it away with the game's own call, never B (mid-shot or mid-pull, once that ends). While it is held (as Link holds it: vr/items/hookshot/held_hookshot.inc), the game's own aim
   check runs along its barrel instead of the camera's line, from its tip (hos_vr_hook_aim, hos_vr_hook_start), so what the
   hook can reach, grab or pull is the game's own answer, and the game's own red laser shows it. The game's flat screen
   reticle is hidden then (oot_vr_hook_frame -> the renderer).

   What is known (game 1.1.1; the lifted code, the actor packs, headset logs and headless runs on a copied save, 17 Nov):
   - Weapon_Range_Hookshot / Weapon_Range_Longshot carry a HookshotParam: Range 10 / 20 (m), ShootLineCheckRadius
     0.075, the shot leaves 0.3 / 0.4 m along the Weapon_L bone. The hook itself is a Shootable (Shootable_Hook /
     Shootable_HookLong), the arrows' family.
   - The aim is checked by an object of its own (vtable 0x84b8b440): its update (f_8300c340, slot 8) reads the aim camera
     (its position at +2728, the point it looks at at +2740), aims along at - position, checks the line out to the range
     and keeps the result (+152 the hit or the end, +164 the start, the game's hand, ...). The aim builder f_80629fa0
     copies that into Link's aim (EquipmentUser, actor +624 -> +984, at +48) each frame. Found with HOS_WATCH on the aim
     (written by the memory copy, from f_82e23280 in f_80629fa0) and the object found there.
   - Not this aim: the AI node ExecuteEquipmentUserUpdateHookshotAimInfo's path (f_826c9a80 -> f_82e1f7e0 -> f_82deff10 and
     its line checks f_82bafa80 / f_82bb10b0 / f_82bb2440) never ran with the hookshot out; f_82def960 runs once a shot.
   - The aim at EquipmentUser +48 (words): 2-4 where the check ended (the hit, or the end of the range), 11-13 the shot's
     start, 14-16 the hit's normal, 23-27 the hit surface's collision info, 32 -1 when nothing was hit, 52 (+208) the
     variant's type (1: this aim).
   - The game fires on ZR let go (0x600480 aiming -> 0xe00480 the shot); ZR let go while it comes out (0x80) leaves it
     resting (0x480). OneShotEquipmentUserShootHookShot (f_828f3df0) fires through f_82e1f960.

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/items/bomb/bomb_host.c. Not compiled on its
   own. */
int rx_vr_hook_ray(int hand, float out[6]);
static void oot_vr_radial_frame(Ctx *c);   /* (vr/menu/radial_host.c) */
void rx_vr_hook_active(int active);   /* (the holding hand + 1, 0 none) */
static volatile int oot_vr_hook_hand = -1, oot_vr_hook_owned;
static volatile int64_t oot_vr_hook_out_at;     /* the hookshot last seen out in Link's hand (oot_vr_hook_frame) */
static volatile uint32_t oot_vr_hook_state;     /* Link's action state, as last seen (oot_vr_hook_frame) */

/* first person in the headset, the option on, the hookshot or longshot selected */
static int oot_vr_hook_on(void) {
    int rx_option(int id, int otherwise);
    HookshotVrHead h;
    return hos_vr_selected_is_hookshot && rx_option(911, 0) && rx_vr_head(&h) && h.active && h.mode == 1;
}

/* The hookshot's aim check: an object of its own (vtable 0x84b8b440). The aim builder (f_80629fa0, game hook at main+0x60a074)
   copies its result into Link's aim each frame, with the object in x19: noted here. Its update (f_8300c340, the object in
   x19 there too) reads the aim camera, aims along at - position from the camera, and checks the line out to the range
   (found headless, 17 Nov: HOS_WATCH on the aim, then on the object's line). */
static volatile uint64_t oot_vr_hook_query;
void hos_vr_hook_query(Ctx *c) {
    if (hos_vr_selected_is_hookshot && c->x[19]) __atomic_store_n(&oot_vr_hook_query, c->x[19], __ATOMIC_RELAXED);
}

/* (game hook at main+0x2fec420, in that update right after it took at - position) while a VR hand holds the hookshot in first
   person: that hand's line instead. The registers: the camera's position in s11, s12, s10; s0-s2 the way it looks
   (normalised next). RX_VR_HOOK_TEST=2 (headless): a test line from 1.2 m above Link, along +x. */
void hos_vr_hook_aim(Ctx *c) {
    const uint64_t q = __atomic_load_n(&oot_vr_hook_query, __ATOMIC_RELAXED);
    if (!q || c->x[19] != q || !hos_vr_selected_is_hookshot) return;
    static int test = -1;
    if (test < 0) test = vr_research_int("RX_VR_HOOK_TEST", 0);
    float ray[6];
    if (test == 2) {
        const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
        if (!actor) return;
        for (int k = 0; k < 3; k++) ray[k] = vr_load(c, actor + 796 + 4 * k), ray[3 + k] = k == 0;
        ray[1] += 1.2f;
    } else {
        const int hand = __atomic_load_n(&oot_vr_hook_hand, __ATOMIC_RELAXED);
        if (hand < 0 || !oot_vr_hook_on() || !rx_vr_hook_ray(hand, ray)) return;
    }
    static const int origin[3] = {11, 12, 10};
    for (int k = 0; k < 3; k++) {
        uint32_t o, d;
        memcpy(&o, &ray[k], 4), memcpy(&d, &ray[3 + k], 4);
        a64_vset64(c->vreg[origin[k]], o);
        a64_vset64(c->vreg[k], d);
    }
    static unsigned told;
    if (told++ < 4)
        hos_log("vr: hookshot aims along %s: from %.2f %.2f %.2f toward %.3f %.3f %.3f", test == 2 ? "the test line" : "the VR hand",
                ray[0], ray[1], ray[2], ray[3], ray[4], ray[5]);
}

/* (game hook at main+0x2fed8a4, in the same update right after it stored the line's start at +164: the game's own hand) the
   held VR hookshot's tip instead, where the game's red laser and the shot leave from */
void hos_vr_hook_start(Ctx *c) {
    const uint64_t q = __atomic_load_n(&oot_vr_hook_query, __ATOMIC_RELAXED);
    if (!q || c->x[19] != q || !hos_vr_selected_is_hookshot) return;
    const int hand = __atomic_load_n(&oot_vr_hook_hand, __ATOMIC_RELAXED);
    float ray[6];
    if (hand < 0 || !oot_vr_hook_on() || !rx_vr_hook_ray(hand, ray)) return;
    for (int k = 0; k < 3; k++) vr_store(c, q + 164 + 4 * k, ray[k]);
}

/* (host/nn_hid.c, through hos_oot_vr_bow_controls) the game's buttons while the hookshot is in a VR hand: a grip takes it,
   that hand's trigger is ZR (the real ZR, the right trigger, does nothing then; ZL, the left trigger, neither while the left
   hand holds it), and letting go of the grip while it is out puts it away with the game's own call, never B (B rolls while
   Link moves and talks or opens beside someone: vr/items/gear/put_away_stow.inc). Taken again before it is away, the
   request is withdrawn; if the game put it away all the same, ZR takes it out again. */
#include "../gear/put_away_stow.inc"
#define kVrHookOutFreshNs 100000000LL   /* (out: seen out by the game's frame this recently; it is seen every frame) */
uint64_t hos_vr_hook_buttons(uint64_t buttons) {
    const uint64_t ZL = 1ULL << 8, ZR = 1ULL << 9;
    static int hand = -1, grip_was[2], trigger_was[2], fired, letting_go, regrip;
    static int64_t fired_at;
    static int64_t take_at;   /* (a grip is taking it out, ZR held until it is out) */
    static VrStow away;
    const int64_t now = hos_now_ns();
    const int64_t out_seen = __atomic_load_n(&oot_vr_hook_out_at, __ATOMIC_RELAXED);
    const int out_now = out_seen && now - out_seen < kVrHookOutFreshNs;
    if (!oot_vr_hook_on() || __atomic_load_n(&oot_vr_quick_open, __ATOMIC_ACQUIRE)) {
        if (hand >= 0) hos_log("vr: hookshot: no longer in a VR hand (not selected, the quick select, or not first person)");
        hand = -1, take_at = 0, regrip = 0, grip_was[0] = grip_was[1] = trigger_was[0] = trigger_was[1] = 0;
        away.active = 0;   /* (not selected any more: the item's own put-away is the game's) */
        __atomic_store_n(&oot_vr_hook_hand, -1, __ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_hook_owned, 0, __ATOMIC_RELAXED);
        return buttons;
    }
    uint32_t hw[64] = {0};
    float squeeze[2] = {0, 0}, trigger[2] = {0, 0};
    if (rx_vr_hands(hw))
        for (int k = 0; k < 2; k++) memcpy(&trigger[k], &hw[1 + 26 * k + 21], 4), memcpy(&squeeze[k], &hw[1 + 26 * k + 22], 4);
    int grip[2], trig[2];
    for (int k = 0; k < 2; k++) {
        grip[k] = grip_was[k] ? squeeze[k] > .35f : squeeze[k] > .55f;
        trig[k] = trigger_was[k] ? trigger[k] > .35f : trigger[k] > .55f;
    }
    if (hand < 0) {
        int hos_vr_bomb_in_reach(int hand);   /* (a grip near a bomb takes the bomb) */
        int hos_vr_ocarina_claims(int hand);  /* (a grip at the belt ocarina takes it: vr/items/ocarina/ocarina_host.c) */
        for (int k = 0; k < 2; k++)
            if (grip[k] && !grip_was[k] && !hos_vr_bomb_in_reach(k) && !hos_vr_ocarina_claims(k)) {
                hand = k, regrip = vr_stow_busy(&away);
                vr_stow_cancel(&away, "hookshot");   /* (taken again before it was away) */
                /* not out yet: ZR held until it is (the game takes it out, 0x80), then let go: it rests in the hand, 0x480,
                   without a shot (headset log, 17 Nov: ZR let go while it came out fired nothing) */
                const int64_t seen = __atomic_load_n(&oot_vr_hook_out_at, __ATOMIC_RELAXED);
                if (!seen || now - seen > 300000000LL) take_at = now;
                hos_log("vr: hookshot: in the %s hand%s (its trigger aims, letting go fires)", k ? "right" : "left",
                        take_at == now ? ", taken out" : "");
                break;
            }
    }
    /* ==== RULE: a grip let go mid-shot or mid-pull waits for the shot or pull to finish (player's request, 17 Nov) ===========
       Let go while the hook is out (fired: the trigger let go while aiming, until Link is back to holding it, 0x480) or while
       Link is pulled to the target, the hookshot stays in the VR hand; it goes back to Link (put away) once that ends. Before,
       it snapped back to Link's game hand at once. To change the rule: `busy` below (what counts as mid-shot), or set it to 0
       to let go at once as before. Squeezing the grip again meanwhile keeps holding it. */
    const uint32_t state = __atomic_load_n(&oot_vr_hook_state, __ATOMIC_RELAXED);
    if (hand >= 0 && trigger_was[hand] && !trig[hand] && (state & 0x200000u)) fired = 1, fired_at = now;   /* (the shot) */
    /* (over: back in Link's hand (0x480), nothing in hand (0), put away (0x80000000), or 6 s as a safety) */
    if (state == 0x480u || state == 0 || (state & 0x80000000u) || now - fired_at > 6000000000LL) fired = 0;
    {   /* (the states between a shot and its end, bounded: the pull's own, not yet seen) */
        static uint32_t said;
        static unsigned told;
        if (fired && state != said && told < 40) told++, hos_log("vr: hookshot: mid-shot state %#x", state);
        said = fired ? state : 0;
    }
    const int busy = fired;
    if (hand >= 0 && !grip[hand] && busy) {
        if (!letting_go) hos_log("vr: hookshot: %s grip let go mid-shot (state %#x): kept until the shot ends", hand ? "right" : "left", state);
        letting_go = 1;
    } else if (hand >= 0 && grip[hand]) letting_go = 0;
    if (hand < 0) letting_go = 0;
    else if (!grip[hand] && !busy) {
        letting_go = 0;
        take_at = 0, regrip = 0;
        hos_log("vr: hookshot: %s hand let go%s", hand ? "right" : "left", out_now ? ": put away" : "");
        if (out_now) vr_stow_start(&away, now);
        hand = -1;
    }
    vr_stow_step(&away, out_now, now, "hookshot");   /* (the game's own put-away, asked until it is away; never B) */
    if (regrip && hand >= 0 && !take_at && vr_stow_regrip_lost(&away, out_now, now)) {   /* (the game made the last request) */
        take_at = now, regrip = 0;
        hos_log("vr: hookshot: put away just as it was taken again: the game's ZR takes it out again");
    } else if (regrip && !vr_stow_regrip_lost(&away, 0, now)) regrip = 0;
    for (int k = 0; k < 2; k++) grip_was[k] = grip[k], trigger_was[k] = trig[k];
    __atomic_store_n(&oot_vr_hook_hand, hand, __ATOMIC_RELAXED);
    __atomic_store_n(&oot_vr_hook_owned, 1, __ATOMIC_RELAXED);
    buttons &= ~ZR;
    if (hand == 0) buttons &= ~ZL;
    if (take_at) {   /* (taking it out: until it is seen out, at most 0.7 s) */
        const int64_t seen = __atomic_load_n(&oot_vr_hook_out_at, __ATOMIC_RELAXED);
        if ((seen && seen > take_at) || now - take_at > 700000000LL) {
            if (!(seen && seen > take_at)) hos_log("vr: hookshot: did not come out in 0.7 s");
            take_at = 0;
        }
    }
    if (hand >= 0 && (trig[hand] || take_at)) buttons |= ZR;
    return buttons;
}
int hos_vr_hook_grips(void) { return __atomic_load_n(&oot_vr_hook_owned, __ATOMIC_RELAXED); }

/* The hookshot weapon actor (Weapon_Range_Hookshot / _Longshot) in Link's equipment slots, as vr/items/gear/equipment_host.c
   reads them (EquipmentUser, actor +0x270 -> +0x200, 8 handles from +0x20), or 0 */
static uint64_t oot_vr_hook_weapon(Ctx *c) {
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
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        if (!strcmp(name, "Weapon_Range_Hookshot") || !strcmp(name, "Weapon_Range_Longshot")) return actor;
    }
    return 0;
}

/* (every game frame, from host/hos_main.c oot_vr_note_player) whether the hookshot is out, and whether a VR hand holds it:
   the renderer hides the game's flat screen reticle then (its red laser shows the aim). RX_VR_HOOK_TEST (headless):
   the selection and Link's action states, which the headset's view notes otherwise. */
static void oot_vr_hook_frame(Ctx *c, uint64_t actor) {
    (void)actor;
    static int test = -1;
    if (test < 0) test = vr_research_on("RX_VR_HOOK_TEST");
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    const uint32_t state = object ? recomp_load32(c, object + 0x1f60) : 0;
    if (test) {
        static int64_t noted;
        static uint32_t last = 0xffffffffu;
        const int64_t now = hos_now_ns();
        if (now - noted > 300000000LL) noted = now, oot_vr_note_selected_item(c);
        oot_vr_radial_frame(c);   /* (vr/menu/radial_host.c: its own test, RX_VR_RADIAL_TEST) */
        if (state != last) hos_log("vr: hookshot test: action state %#x -> %#x", last, state), last = state;
    }
    if (!hos_vr_selected_is_hookshot) return;
    /* out in Link's hand: action states (headset log, 17 Nov) 0x80 taking it out, 0x600480 aiming, 0xe00480 the shot,
       0x20000480 the hook coming back, 0x480 out between shots (0x6004xx, 0x4xx the same, some moving), 0x80000000 put away
       by B */
    const int out = !(state & 0x80000000u) && ((state & 0x400u) || (state & 0xffu) == 0x80u);
    if (out) __atomic_store_n(&oot_vr_hook_out_at, hos_now_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&oot_vr_hook_state, state, __ATOMIC_RELAXED);
    const int hand = __atomic_load_n(&oot_vr_hook_hand, __ATOMIC_RELAXED);
    /* the hand + 1 (it holds the model, vr/items/hookshot/held_hookshot.inc), or 8: out but no hand (being put away: the shield
       the game holds out with it stays hidden until it is away; it flashed for a moment after the grip let go, 17 Nov) */
    rx_vr_hook_active(!out ? 0 : hand >= 0 ? hand + 1 : 8);
    if (out && hand >= 0) {   /* (its model, for the near-plane rescue: vr/items/gear/gear_near_host.c; it was culled at some angles) */
        static int64_t looked;
        static uint64_t model;
        const int64_t now = hos_now_ns();
        if (now - looked > 300000000LL) looked = now, model = oot_vr_gear_resolve(c, oot_vr_hook_weapon(c));
        __atomic_store_n(&oot_vr_gear_hook_model, model, __ATOMIC_RELEASE);
        __atomic_store_n(&oot_vr_gear_hook_time, now, __ATOMIC_RELEASE);
    }   /* (the hand + 1: it holds the model, vr/items/hookshot/held_hookshot.inc) */
}
