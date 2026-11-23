/* Hookshot VR: a headless research test of the sword's real hits (RX_VR_SWORD_TEST), and what sits on Link.
   Does a hit land where the mod places the sword actor, or does the game put the actor back on Link's hand before its attack
   sensor is tested? With no headset (a headless run) there is no VR hand, so the test makes the blade itself:
   - RX_VR_SWORD_TEST=1: while the game has Link's sword out, every update of the sword actor places it (vr_bomb_place, as
     hos_vr_sword_actor_update does) on a blade swept up and down through the nearest enemy, from beside it, where Link's own
     slash does not reach when the enemy is behind him (the layout puts it there). The input script presses Y (the attack).
   - RX_VR_SWORD_TEST=2: the control. The same enemies watched, the same Y presses, the sword never placed.
   - RX_VR_SWORD_TEST=3: as 1, and placed again in Link's own update (in case the game binds the sword back to his hand
     between the sword's update and its sensor test).
   - RX_VR_SWORD_TEST=4 (VR tab Sword hits: Blade, blade_hits.inc; no attack button): the sword drawn by the game's own draw
     (hos_vr_sword_draw), then a scripted blade swung level through the nearest enemy's stalk (kVrSwordTestStalk above its
     origin) at kVrSwordTestFast, once every kVrSwordTestEvery, back and forth, kVrSwordTestStrokes times, at rest between,
     through the same swing and contact checks as a VR hand's (vr_blade_weapon_update). =5: the same at the head's height
     (kVrSwordTestHead). =6: the control, the stalk's path at kVrSwordTestSlow (a slow wave: no stroke, no hit).
   The enemies (actors named Enemy_*, or RX_VR_SWORD_TEST_TARGET's prefix: Obj_AttentionRock, Kokiri Forest's dummy) are watched in their own updates: where each is, and when one stops updating (killed, or
   gone), logged as "vr: sword test: <name> gone". Every kVrSwordTestLogNs: Link's action state, whether the sword is out, the
   nearest enemy, where the blade was put and where the actor was found at its next update.
   Also logged once (any test): Link's EquipmentUser slots by name, and each actor that sits on Link
   (within kVrBesideNear of him), by name: where the worn Goron Bracelet is to be found.
   A piece of vr/items/sword/sword_host.c (its helpers). Not compiled on its own. */
#define kVrSwordTestLogNs 250000000LL
#define kVrSwordTestGoneNs 1000000000LL   /* (an enemy not updated this long: gone) */
#define kVrSwordTestEnemies 8
#define kVrSwordTestStalk 0.35f       /* (m above the enemy's origin) */
#define kVrSwordTestHead 0.9f
#define kVrSwordTestFast 5.0f         /* (m/s) */
#define kVrSwordTestSlow 0.6f
#define kVrSwordTestSpan 1.4f         /* (m: the grip's path, across the line from Link to the enemy) */
#define kVrSwordTestEvery 4000000000LL
#define kVrSwordTestStrokes 3
typedef struct { uint64_t actor; int64_t seen; float at[3]; char name[48]; } VrSwordTestEnemy;
static VrSwordTestEnemy vr_sword_test_enemies[kVrSwordTestEnemies];
static SRWLOCK vr_sword_test_lock = SRWLOCK_INIT;

static int vr_sword_test(void) {
    static int t = -1;
    if (t < 0) {
        t = vr_research_int("RX_VR_SWORD_TEST", 0);
        if (t) hos_log("vr: sword test %d: %s", t, t == 1 ? "the sword actor swept through the nearest enemy" :
                       t == 3 ? "swept through the nearest enemy, placed in Link's update too" :
                       t == 4 ? "Sword hits Blade: a fast stroke through the nearest enemy's stalk, no attack button" :
                       t == 5 ? "Sword hits Blade: a fast stroke through the nearest enemy's head, no attack button" :
                       t == 6 ? "Sword hits Blade, the control: a slow wave along the stalk's path" : "the control: never placed");
    }
    return t;
}

/* Link's sword actor (EquipmentUser slot 0: the Kokiri Sword, headless 18 Nov) and whether the game has it out (its Equipment
   component enabled, state 1, as vr/items/gear/equipment_host.c reads it); the slots' names logged once */
static uint64_t vr_sword_test_sword(Ctx *c, int *out) {
    *out = 0;
    const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    if (!oot_vr_equipment_ptr(player, 0x278)) return 0;
    const uint64_t components = recomp_load64(c, player + 0x270);
    const uint64_t user = oot_vr_equipment_ptr(components, 0x208) ? recomp_load64(c, components + 0x200) : 0;
    if (!oot_vr_equipment_ptr(user, 0x20 + 0x18 * 8)) return 0;
    static int listed;
    uint64_t sword = 0;
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
        if (!listed) hos_log("vr: sword test: Link's EquipmentUser slot %d: %s %#llx", slot, name[0] ? name : "?", (unsigned long long)actor);
        if (sword || strncmp(name, "Weapon_Sword_", 13)) continue;
        sword = actor;
        const uint64_t parts = recomp_load64(c, actor + 0x270);
        const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
        *out = oot_vr_equipment_ptr(equipment, 0xa0) && (recomp_load8(c, equipment + 0x98) & 1) && recomp_load32(c, equipment + 0x9c) == 1;
    }
    listed = 1;
    return sword;
}

/* (every actor's update) an enemy noted: where it is, when last seen */
static void vr_sword_test_note(Ctx *c, uint64_t actor) {
    char name[64];
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    static char want[48];   /* (RX_VR_SWORD_TEST_TARGET: the targets' name prefix, Enemy_ by default; e.g. Obj_AttentionRock) */
    if (!want[0]) {
        const char *e = vr_research_getenv("RX_VR_SWORD_TEST_TARGET");
        snprintf(want, sizeof want, "%s", e && e[0] ? e : "Enemy_");
    }
    if (strncmp(name, want, strlen(want)) || !hos_guest_range(actor + 796, 12)) return;
    AcquireSRWLockExclusive(&vr_sword_test_lock);
    VrSwordTestEnemy *e = NULL;
    for (int i = 0; i < kVrSwordTestEnemies && !e; i++) if (vr_sword_test_enemies[i].actor == actor) e = &vr_sword_test_enemies[i];
    for (int i = 0; i < kVrSwordTestEnemies && !e; i++)
        if (!vr_sword_test_enemies[i].actor) {
            e = &vr_sword_test_enemies[i], e->actor = actor;
            snprintf(e->name, sizeof e->name, "%s", name);
            hos_log("vr: sword test: enemy %s %#llx seen", name, (unsigned long long)actor);
        }
    if (e) {
        e->seen = hos_now_ns();
        for (int k = 0; k < 3; k++) e->at[k] = vr_bomb_f(c, actor + 796 + 4 * k);
    }
    ReleaseSRWLockExclusive(&vr_sword_test_lock);
}

/* (Link's update) the enemies not seen lately logged as gone; the nearest one still here (its position, the distance) */
static int vr_sword_test_nearest(float at[3], float *dist, char name[48]) {
    const int64_t now = hos_now_ns();
    int found = 0;
    *dist = 1e9f;
    AcquireSRWLockExclusive(&vr_sword_test_lock);
    for (int i = 0; i < kVrSwordTestEnemies; i++) {
        VrSwordTestEnemy *e = &vr_sword_test_enemies[i];
        if (!e->actor) continue;
        if (now - e->seen > kVrSwordTestGoneNs) {
            hos_log("vr: sword test: %s %#llx gone (not updated for %d ms; last at %.2f %.2f %.2f)", e->name,
                    (unsigned long long)e->actor, (int)((now - e->seen) / 1000000), e->at[0], e->at[1], e->at[2]);
            memset(e, 0, sizeof *e);
            continue;
        }
        float d[3];
        for (int k = 0; k < 3; k++) d[k] = e->at[k] - vr_link_pos[k];
        const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (n < *dist) *dist = n, memcpy(at, e->at, 3 * sizeof *at), snprintf(name, 48, "%s", e->name), found = 1;
    }
    ReleaseSRWLockExclusive(&vr_sword_test_lock);
    return found;
}

/* the test's blade through an enemy at e: from kSide beside it (across the line from Link), pointing through it level, its
   height swept from 0.2 to 1.2 m above the enemy's origin twice a second */
static void vr_sword_test_blade(const float e[3], int64_t now, float origin[3], float rot[9]) {
    const float kSide = 0.5f;
    float s[3] = {-(e[2] - vr_link_pos[2]), 0, e[0] - vr_link_pos[0]};   /* (level, across Link -> enemy) */
    const float n = sqrtf(s[0] * s[0] + s[2] * s[2]);
    if (n < 1e-3f) s[0] = 1, s[2] = 0; else s[0] /= n, s[2] /= n;
    const float h = 0.7f + 0.5f * sinf((float)(now % 1000000000LL) * 1e-9f * 6.283185f * 2);
    const float x[3] = {0, 1, 0}, z[3] = {-s[0], 0, -s[2]};
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    for (int k = 0; k < 3; k++) {
        origin[k] = e[k] + s[k] * kSide + x[k] * h;
        rot[3 * k + 0] = x[k], rot[3 * k + 1] = y[k], rot[3 * k + 2] = z[k];
    }
}

/* (tests 4-6) the scripted blade at this moment: level, pointing through the enemy at e from kSide on Link's side, its grip
   moved across, back and forth: at rest, then one stroke every kVrSwordTestEvery (at most kVrSwordTestStrokes), at rest
   again. Returns the stroke under way (1..), 0 at rest */
static int vr_sword_test_stroke(const float e[3], int64_t t, int test, float origin[3], float rot[9]) {
    const float kSide = 0.45f, speed = test == 6 ? kVrSwordTestSlow : kVrSwordTestFast;
    const float h = test == 5 ? kVrSwordTestHead : kVrSwordTestStalk;
    float n[3] = {e[0] - vr_link_pos[0], 0, e[2] - vr_link_pos[2]};   /* (level, Link -> enemy) */
    const float len = sqrtf(n[0] * n[0] + n[2] * n[2]);
    if (len < 1e-3f) n[0] = 0, n[2] = 1; else n[0] /= len, n[2] /= len;
    const float s[3] = {-n[2], 0, n[0]};
    const int64_t stroke_ns = (int64_t)(kVrSwordTestSpan / speed * 1e9f);
    int k = (int)(t / kVrSwordTestEvery);   /* (stroke k from t = k * every + 1 s) */
    int64_t into = t - (int64_t)k * kVrSwordTestEvery - 1000000000LL;
    if (k >= kVrSwordTestStrokes) k = kVrSwordTestStrokes - 1, into = stroke_ns;
    float f = into <= 0 ? 0 : into >= stroke_ns ? 1 : (float)into / (float)stroke_ns;   /* (along this stroke) */
    if (into <= 0 && k > 0) f = 1, k--;   /* (at rest before stroke k: where stroke k - 1 ended) */
    const float x = (k % 2 ? 1 - f : f) * kVrSwordTestSpan - kVrSwordTestSpan / 2;
    const float up[3] = {0, 1, 0}, z[3] = {n[0], 0, n[2]};
    const float y[3] = {z[1] * up[2] - z[2] * up[1], z[2] * up[0] - z[0] * up[2], z[0] * up[1] - z[1] * up[0]};
    for (int j = 0; j < 3; j++) {
        origin[j] = e[j] - n[j] * kSide + s[j] * x + up[j] * h;
        rot[3 * j + 0] = up[j], rot[3 * j + 1] = y[j], rot[3 * j + 2] = z[j];
    }
    return into > 0 && into < stroke_ns ? k + 1 : 0;
}

/* the sword actor placed on the test's blade through the nearest enemy; 0 none placed */
static int vr_sword_test_place(Ctx *c, uint64_t sword, int64_t now, float origin[3]) {
    float at[3], dist, rot[9];
    char name[48];
    if (!vr_sword_test_nearest(at, &dist, name)) return 0;
    vr_sword_test_blade(at, now, origin, rot);
    vr_bomb_place(c, sword, origin, rot);
    return 1;
}

/* (hos_vr_sword_actor_update, every actor's update) the test's part: 1 when it handled this actor */
static int vr_sword_test_update(Ctx *c, uint64_t actor) {
    const int test = vr_sword_test();
    if (!test || !actor) return 0;
    if (actor == __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED)) {
        static int64_t logged;
        const int64_t now = hos_now_ns();
        int out;
        const uint64_t sword = vr_sword_test_sword(c, &out);
        float placed[3];
        if (test == 3 && out && sword) vr_sword_test_place(c, sword, now, placed);
        if (test >= 4 && sword && !out) {   /* (the game's own draw, no attack button: as a VR take asks it) */
            static int64_t drew_at;
            static int draws;
            if (draws < 8 && now - drew_at > 1000000000LL) draws++, drew_at = now, hos_vr_sword_draw();
        }
        if (now - logged < kVrSwordTestLogNs) return 0;
        logged = now;
        float at[3], dist;
        char name[48] = "none";
        const int enemy = vr_sword_test_nearest(at, &dist, name);
        const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
        /* (the game's guard query bit, as vr/items/shield/guard_host.c reads it: Link's guard component +0x34 bit 8) */
        const uint64_t table = oot_vr_guard_ptr(c, actor + 0x270), guard = table ? oot_vr_guard_ptr(c, table + 0x248) : 0;
        const int guarding = guard && hos_guest_range(guard, 0x36) ? (recomp_load16(c, guard + 0x34) & 0x100) != 0 : -1;
        hos_log("vr: sword test: Link at %.2f %.2f %.2f, action %#x, sword %s, guarding %d, locked on %d; nearest enemy %s%s",
                vr_link_pos[0], vr_link_pos[1], vr_link_pos[2], object ? recomp_load32(c, object + 0x1f60) : 0,
                out ? "out" : "away", guarding, oot_vr_lock_point(c, actor, NULL), name, enemy ? "" : " (none)");
        if (enemy) hos_log("vr: sword test:   %s at %.2f %.2f %.2f, %.2f m from Link", name, at[0], at[1], at[2], dist);
        return 0;
    }
    vr_sword_test_note(c, actor);
    int out;
    if (actor != vr_sword_test_sword(c, &out)) return 0;
    static float set_at[3];
    static int have_set;
    static int64_t logged;
    const int64_t now = hos_now_ns();
    if (have_set && now - logged > kVrSwordTestLogNs) {
        logged = now;
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = vr_bomb_f(c, actor + 796 + 4 * k);
        hos_log("vr: sword test: blade placed at %.2f %.2f %.2f, the sword actor found at %.2f %.2f %.2f next update", set_at[0],
                set_at[1], set_at[2], p[0], p[1], p[2]);
    }
    float origin[3];
    if (test >= 4) {   /* (the scripted stroke, through the checks a VR hand's blade goes through) */
        static int64_t out_at;
        static int stroke_was;
        float at[3], dist, rot[9];
        char name[48];
        if (!vr_sword_test_nearest(at, &dist, name)) { have_set = 0, out_at = 0; return 1; }   /* (drawn or not, as a VR hand's) */
        if (!out_at) out_at = now;
        const int stroke = vr_sword_test_stroke(at, now - out_at, test, origin, rot);
        if (stroke && stroke != stroke_was)
            hos_log("vr: sword test: stroke %d of %d through %s at %.1f m/s, %.2f m above its origin", stroke, kVrSwordTestStrokes,
                    name, test == 6 ? kVrSwordTestSlow : kVrSwordTestFast, test == 5 ? kVrSwordTestHead : kVrSwordTestStalk);
        stroke_was = stroke;
        vr_blade_weapon_update(c, actor, -1, origin, rot);   /* (-1: the stroke from the world blade, as scripted) */
        vr_bomb_place(c, actor, origin, rot);
        memcpy(set_at, origin, sizeof set_at), have_set = 1;
        return 1;
    }
    if (test == 2 || !out || !vr_sword_test_place(c, actor, now, origin)) { have_set = 0; return 1; }
    memcpy(set_at, origin, sizeof set_at), have_set = 1;
    return 1;
}

/* (any test) an actor that sits on Link, by name, once each (bounded) */
static void vr_on_link_trace(Ctx *c, uint64_t actor, float dist) {
    static int on = -1;
    if (on < 0) on = vr_sword_test() != 0;
    static uint64_t told[24];
    static unsigned n;
    if (!on || n >= 24) return;
    for (unsigned i = 0; i < n; i++) if (told[i] == actor) return;
    char name[64];
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    told[n++] = actor;
    hos_log("vr: on Link (%.2f m): %s %#llx", dist, name[0] ? name : "?", (unsigned long long)actor);
}
