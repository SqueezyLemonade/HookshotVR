/* Hookshot VR: the sword's hits where the VR blade is (game side). VR tab "Sword from the back" (option 913).
   The equipped sword's actor (the Master Sword's, the Kokiri Sword's: any Weapon_Sword_*) carries its own attack sensor (its pack: Phive/RigidBodySensorParam/WeaponAtk, a capsule
   WeaponSwordAtkPlayerMetal 0.15 m thick along the actor's own z, 0 to 1.5 m), tracking the actor itself
   (RigidBodyControllerSensorParam IsTrackingActor), switched on by the game during an attack. So while a VR hand holds the
   sword (vr/input/controllers_host.c) and the game has it out, the sword actor is placed every update on the VR blade: its
   origin at the hand's grip, its z along the blade (the fist's axis, leaned forward by the VR tab's Sword angle, as
   vr/body/arms_and_pose.inc leans the drawn hand), the same placement call as a held bomb's (bomb_host.c vr_bomb_place).
   VR tab Sword hits: Blade (the default, blade_hits.inc), the VR blade's own swing and contact set the game's attack sensor
   up; Button, a swing presses the attack button, the game's attack switches the sensor on, and what the blade passes
   through is hit.
   The game binds the actor back to Link's hand between updates (vr: sword actor: placed at ..., found at ... 0.2-0.9 m
   off), but only after its sensor was tested where it was placed: headless 18 Nov (RX_VR_SWORD_TEST, sword_test.inc), the
   Kokiri Sword placed through a Deku Baba 3.9 m behind Link killed it in about 1.8 s of Y presses; never placed, it lived.
   Game-side code, included by host/hos_main.c (its VR HOOK block) after bomb_host.c (its helpers). Not compiled on its own. */
int hos_vr_sword_holder(void);                  /* (vr/input/controllers_host.c: the VR hand holding it, or -1) */

/* ---- Link standing or moving, for a put-away's button (vr/input/controllers_host.c vr_put_away_may_press) --------------
   From Link's own updates: his ground speed (his position, actor +796, between updates, smoothed over about 50 ms) and his
   action state (player object +0x1f60). Standing: under kVrLinkStillSpeed for kVrLinkStillNs. Not known when he has not
   updated for kVrLinkFreshNs (paused, a menu, a load): then no put-away press. */
#define kVrLinkStillSpeed 0.3f          /* (m/s: child Link walks about 1.5, runs about 4) */
#define kVrLinkStillNs 150000000LL
#define kVrLinkFreshNs 200000000LL
static volatile int64_t vr_link_seen_at, vr_link_still_since;
static volatile uint32_t vr_link_action_now;
int hos_vr_link_standing(void) {
    const int64_t now = hos_now_ns(), seen = __atomic_load_n(&vr_link_seen_at, __ATOMIC_ACQUIRE);
    if (!seen || now - seen > kVrLinkFreshNs) return -1;
    const int64_t still = __atomic_load_n(&vr_link_still_since, __ATOMIC_RELAXED);
    return still && now - still >= kVrLinkStillNs;
}
uint32_t hos_vr_link_action(void) { return __atomic_load_n(&vr_link_action_now, __ATOMIC_RELAXED); }

/* ---- Something beside Link, which his put-away button would act on instead (vr_put_away_may_press) -------------------
   The game's B puts a drawn sword or stick away, but with a person, a chest, a door or a sign beside Link the same press
   talks, opens or reads (headset 18 Nov: letting go of the Deku Stick beside a Kokiri talked to them, and the repeated
   presses went through their text). Which of these B will do is not known here, so no put-away press is made while any
   other actor's update puts it within kVrBesideFar of Link. Closer than kVrBesideNear is Link's own gear (the sword and
   shield on his back, the stick or a bomb in his hand): those actors sit on him. The worn Goron Bracelet rides his arm,
   0.59-0.74 m from his position (headless 18 Nov), and is his too (vr_on_link_note). Grass and rocks count: the put-away
   then waits (the weapon hidden) until Link stands clear of them. */
#define kVrBesideNear 0.45f             /* (m, from Link's position) */
#define kVrBesideFar 1.6f
#define kVrBesideHigh 1.2f              /* (m: higher or lower than this is not beside him: a ledge above, a floor below) */
#define kVrBesideFreshNs 300000000LL    /* (an actor seen there this recently: still beside him) */
static volatile int64_t vr_beside_at;
static float vr_link_pos[3];
static volatile float vr_link_vy;   /* (Link's vertical speed, m/s, smoothed over about 50 ms: vr_link_track) */
int hos_vr_link_beside(void) {
    const int64_t at = __atomic_load_n(&vr_beside_at, __ATOMIC_ACQUIRE);
    return at && hos_now_ns() - at < kVrBesideFreshNs;
}
#define kVrOnLinkFlat 0.8f              /* (m, level: Link's own worn and held items are looked for among the actors this close, */
#define kVrOnLinkHigh 2.0f              /*  and up to this far above or below his position: an adult's ocarina at the mouth) */
static int vr_on_link_note(Ctx *c, uint64_t actor, float dist);
static void vr_beside_note(Ctx *c, uint64_t actor) {
    const int64_t now = hos_now_ns(), seen = __atomic_load_n(&vr_link_seen_at, __ATOMIC_ACQUIRE);
    if (!seen || now - seen > kVrLinkFreshNs || !hos_guest_range(actor + 796, 12)) return;
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = vr_bomb_f(c, actor + 796 + 4 * k) - vr_link_pos[k];
    const float flat = sqrtf(d[0] * d[0] + d[2] * d[2]), dist = sqrtf(flat * flat + d[1] * d[1]);
    if (flat < kVrOnLinkFlat && fabsf(d[1]) < kVrOnLinkHigh && vr_on_link_note(c, actor, dist)) return;   /* (his own: not beside him) */
    if (!(dist > kVrBesideNear && flat < kVrBesideFar && fabsf(d[1]) < kVrBesideHigh)) return;
    __atomic_store_n(&vr_beside_at, now, __ATOMIC_RELEASE);
    static uint64_t told_actor;   /* (the log: each new one beside him, bounded) */
    static unsigned told;
    if (actor != told_actor && told < 40) {
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        told_actor = actor, told++;
        hos_log("vr: beside Link (no put-away press): %s %#llx, %.2f m", name[0] ? name : "?", (unsigned long long)actor, dist);
    }
}

#include "blade_hits.inc"   /* (VR tab Sword hits: Blade, the game's attack sensor set up by the VR blade's own swing and contact) */
#ifdef HOOKSHOT_VR_RESEARCH
#include "sword_test.inc"   /* (RX_VR_SWORD_TEST: the headless hit test, and the trace of what sits on Link; research builds) */
#else
static int vr_sword_test_update(Ctx *c, uint64_t actor) { (void)c, (void)actor; return 0; }
static void vr_on_link_trace(Ctx *c, uint64_t actor, float dist) { (void)c, (void)actor, (void)dist; }
#endif

/* ---- Child Link's worn Goron Bracelet, found by its own update on him (vr/items/gear/gear_near_host.c keeps it from the
   game's near test) -------------------------------------------------------------------------------------------------------
   The near test's rescue looked for the bracelet among Link's EquipmentUser slots (vr/items/gear/equipment_host.c), and it
   is not there: the headset run of 18 Nov 20:13 (child Link with the bracelet all run) never logged "vr: Goron Bracelet actor
   ... worn", so the rescue never ran and the bracelet on the VR wrist vanished whenever the game's own arm (where the game
   tests it) went behind the headset's near plane: sword swings, the spin attack, Deku Stick swings. It is an actor of its own,
   Armor_Child_Gauntlet_Goron, riding Link's arm 0.59-0.74 m from his position (headless 18 Nov, RX_VR_SWORD_TEST: his
   EquipmentUser held only slot 0 the Kokiri Sword and slot 4 the Deku Shield), so it is found here by name among the
   actors on him (within kVrOnLinkFlat level, kVrOnLinkHigh up or down). Returns whether this actor is Link's own (the
   bracelet, the ocarina or its holder): not beside him. */
#define kVrBraceletFreshNs 300000000LL   /* (seen this recently: still worn) */
static volatile uint64_t vr_bracelet_actor;
static volatile int64_t vr_bracelet_seen_at;
static int vr_is_bracelet_name(const char *name) {
    return !strcmp(name, "Armor_Child_Gauntlet_Goron") || !strcmp(name, "Item_Gauntlet_Goron");
}
#define kVrNotBracelet 32                /* (actors near Link already read and not the bracelet: each name read once) */
/* The ocarina and its holder on the belt are found the same way: the renderer draws them on the VR belt or in a VR hand
   (vr/items/ocarina/held_ocarina.inc), the game tests them at its own (at Link's mouth while he plays: closer than the near
   plane), so each update of theirs keeps them from the near test (vr/items/gear/gear_near_host.c hos_vr_held_keep) */
#define kVrOcarinas 4
void hos_vr_held_keep(Ctx *c, uint64_t actor);
static int vr_is_ocarina_name(const char *name) { return !strncmp(name, "Item_Ocarina_", 13); }
static int vr_on_link_note(Ctx *c, uint64_t actor, float dist) {
    if (dist < kVrBesideNear) vr_on_link_trace(c, actor, dist);
    static volatile uint64_t not_it[kVrNotBracelet], ocarinas[kVrOcarinas];
    static volatile unsigned next, next_ocarina;
    if (actor == __atomic_load_n(&vr_bracelet_actor, __ATOMIC_RELAXED)) {
        __atomic_store_n(&vr_bracelet_seen_at, hos_now_ns(), __ATOMIC_RELEASE);
        return 1;
    }
    for (int i = 0; i < kVrOcarinas; i++)
        if (__atomic_load_n(&ocarinas[i], __ATOMIC_RELAXED) == actor) { hos_vr_held_keep(c, actor); return 1; }
    for (int i = 0; i < kVrNotBracelet; i++) if (__atomic_load_n(&not_it[i], __ATOMIC_RELAXED) == actor) return 0;
    char name[64];
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    if (vr_is_ocarina_name(name)) {   /* (his own: not beside him either) */
        __atomic_store_n(&ocarinas[__atomic_fetch_add(&next_ocarina, 1, __ATOMIC_RELAXED) % kVrOcarinas], actor, __ATOMIC_RELAXED);
        hos_vr_held_keep(c, actor);
        return 1;
    }
    if (!vr_is_bracelet_name(name)) {
        __atomic_store_n(&not_it[__atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % kVrNotBracelet], actor, __ATOMIC_RELAXED);
        return 0;
    }
    if (actor != __atomic_exchange_n(&vr_bracelet_actor, actor, __ATOMIC_RELAXED))
        hos_log("vr: Goron Bracelet: its actor %s %#llx found on Link (%.2f m)", name, (unsigned long long)actor, dist);
    __atomic_store_n(&vr_bracelet_seen_at, hos_now_ns(), __ATOMIC_RELEASE);
    return 1;
}
/* (vr/items/gear/equipment_host.c) the bracelet actor on Link, seen lately; 0 none */
uint64_t hos_vr_bracelet_on_link(void) {
    const int64_t at = __atomic_load_n(&vr_bracelet_seen_at, __ATOMIC_ACQUIRE);
    return at && hos_now_ns() - at < kVrBraceletFreshNs ? __atomic_load_n(&vr_bracelet_actor, __ATOMIC_RELAXED) : 0;
}
static void vr_link_track(Ctx *c, uint64_t actor) {
    static float was[3], speed;
    static int64_t then;
    const int64_t now = hos_now_ns();
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = vr_bomb_f(c, actor + 796 + 4 * k);
    if (!isfinite(p[0] + p[1] + p[2])) return;
    const float dt = (float)(now - then) * 1e-9f;
    if (then && dt <= 0.001f) return;   /* (a second update in the same moment: nothing to measure) */
    if (then && dt < 0.25f) {
        const float dx = p[0] - was[0], dz = p[2] - was[2];
        speed += (sqrtf(dx * dx + dz * dz) / dt - speed) * fminf(1, dt / 0.05f);
        vr_link_vy += ((p[1] - was[1]) / dt - vr_link_vy) * fminf(1, dt / 0.05f);   /* (for blade_hits.inc: in the air) */
    } else speed = kVrLinkStillSpeed, vr_link_vy = 0;   /* (after a gap: not known to stand until measured) */
    memcpy(was, p, sizeof was), then = now;
    memcpy(vr_link_pos, p, sizeof vr_link_pos);   /* (for vr_beside_note) */
    if (speed >= kVrLinkStillSpeed) __atomic_store_n(&vr_link_still_since, 0, __ATOMIC_RELAXED);
    else if (!__atomic_load_n(&vr_link_still_since, __ATOMIC_RELAXED)) __atomic_store_n(&vr_link_still_since, now, __ATOMIC_RELAXED);
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_link_action_now, object ? recomp_load32(c, object + 0x1f60) : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_link_seen_at, now, __ATOMIC_RELEASE);
}

/* Where a weapon held in this VR hand is, as the game places its actor (the sword's, the Deku Stick's: both carry their
   attack sensor along their own z from the origin): the origin at the hand's grip, z along the fist's axis leaned forward
   by the VR tab's Sword angle (as vr/body/arms_and_pose.inc leans the hand that holds it), x the palm's normal. rot: row
   major, the actor's local axes as columns (rot[3 k + j] = axis j's component k). False with no tracked hand. */
static int vr_held_blade_pose(int hand, float origin[3], float rot[9]) {
    float w[42];
    if (hand < 0 || hand > 1 || !rx_vr_hands_world(w)) return 0;
    const float *o = w + 21 * hand;
    if (o[0] < .5f) return 0;
    /* (the grip's axes in the world: o[9..11] its x (the palm's normal), o[15..17] its z (the fist's axis runs along -z)) */
    float a[3], x[3], f[3], z[3], y[3];
    for (int k = 0; k < 3; k++) a[k] = -o[15 + k], x[k] = o[9 + k];
    f[0] = a[1] * x[2] - a[2] * x[1], f[1] = a[2] * x[0] - a[0] * x[2], f[2] = a[0] * x[1] - a[1] * x[0];   /* (a x c) */
    const float t = rx_option(914, 35) * 0.0174533f;
    for (int k = 0; k < 3; k++) z[k] = a[k] * cosf(t) + f[k] * sinf(t);   /* (the blade) */
    y[0] = z[1] * x[2] - z[2] * x[1], y[1] = z[2] * x[0] - z[0] * x[2], y[2] = z[0] * x[1] - z[1] * x[0];
    for (int k = 0; k < 3; k++) rot[3 * k + 0] = x[k], rot[3 * k + 1] = y[k], rot[3 * k + 2] = z[k];
    memcpy(origin, o + 1, 3 * sizeof *origin);
    return 1;
}

void hos_vr_sword_actor_update(Ctx *c, uint64_t actor) {
    static int64_t logged;
    static float set_at[3];
    static int have_set;
    if (actor && actor == __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED)) {
        vr_link_track(c, actor);
        vr_sword_test_update(c, actor);   /* (headless only: RX_VR_SWORD_TEST) */
        vr_blade_link_update(c, actor);
        return;
    }
    if (actor) vr_beside_note(c, actor), vr_blade_target_note(c, actor);
    if (vr_sword_test_update(c, actor)) return;   /* (headless only: RX_VR_SWORD_TEST) */
    if (!actor || actor != __atomic_load_n(&oot_vr_sword_actor_seen, __ATOMIC_RELAXED)) return;
    const int64_t now = hos_now_ns();
    const int hand = hos_vr_sword_holder();
    /* (Sword hits: Blade: placed and checked whether or not the game keeps it drawn: the game's own draw does not hold while
       Link stands in no drawn stance, headset 22 Nov "drawn 0x1" then "drawn 0" two frames later, and the sensor is the
       sword actor's either way) */
    if (hand < 0 || (!hos_vr_sword_drawn() && !vr_blade_on()) || !rx_option(913, 0)) { have_set = 0; return; }
    if (have_set && now - logged > 1000000000LL) {   /* (once a second: did the last placement hold until this update?) */
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = vr_bomb_f(c, actor + 796 + 4 * k);
        hos_log("vr: sword actor: placed at %.2f %.2f %.2f, found at %.2f %.2f %.2f next update", set_at[0], set_at[1],
                set_at[2], p[0], p[1], p[2]);
        logged = now;
    }
    float origin[3], rot[9];
    if (!vr_held_blade_pose(hand, origin, rot)) return;
    vr_blade_weapon_update(c, actor, hand, origin, rot);   /* (Sword hits: Blade: the swing and the contact; held where it touched) */
    vr_bomb_place(c, actor, origin, rot);
    memcpy(set_at, origin, sizeof set_at), have_set = 1;
}
