/* Hookshot VR: aiming in third person (the release's package B). In third person the headset's view is the game's own
   camera turned by the head (vr/camera/view_host.c), but what decides a shot is the game's camera alone: so the game's flat
   reticle, drawn at the middle of the picture, sits where the head looks, not where the shot goes. Here, every third-person
   frame while Link aims the slingshot, the bow or the Boomerang: where the game's own line along its camera ends
   (oot_vr_tp_aim_point), told to the renderer, which draws the headset's reticle there in the world
   (vr/items/aim/third_person_reticle.inc, VR tab Aim reticle) and hides the game's flat one meanwhile (vr/body/tracking.inc
   native_bow_reticle_hidden). First person keeps its own aim (the hands): nothing here runs there.

   RESEARCH (RX_VR_TP_AIM_TEST=1, headless, on a COPIED save): RX_VR_TP_AIM_ITEM (slingshot, boomerang, bow, hookshot) given
   and selected once a save is loaded; then, in third person, Link's action state as it changes, and every 100 ms while an
   item is out: the game camera's line and the reticle, each shot's launch (the arrow-launch probe, vr/items/bow/bow_host.c),
   and the shot's flight (hos_vr_tp_aim_actor_update: what leaves the launch point, followed for 3 s).

   Game-side code, included by host/hos_main.c (its VR HOOK block) before vr/camera/view_host.c; it uses
   vr/items/bow/bow_host.c's (vr_load), vr/items/gear/equipment_host.c's and vr/input/quick_select_host.c's readers. Not
   compiled on its own. */

static int oot_vr_tp_aim_test(void) {
    static int test = -1;
    if (test < 0) test = vr_research_int("RX_VR_TP_AIM_TEST", 0);
    return test;
}

static float oot_vr_tp_aim_last[3];   /* (the research: the last reticle, beside each shot's launch) */
static int oot_vr_tp_aim_last_kind;

#if defined(_WIN32) || defined(__linux__)   /* (oot_pouch_change: the debug tools' pouch edit, host/hos_main.c) */
/* the research run's item: given and selected once a save has settled */
static void oot_vr_tp_aim_give(Ctx *c) {
    static int given, selected;
    static int64_t loaded_at;
    const int64_t now = hos_now_ns();
    const char *want = vr_research_getenv("RX_VR_TP_AIM_ITEM");
    if (!want || !*want || selected) return;
    static const struct { const char *key, *item, *bag, *ammo, *menu; } kItems[] = {   /* (the bag first: without it, no ammo) */
        {"slingshot", "Weapon_Range_SlingShot", "Item_BulletBag_Small", "Item_DekuSeed", "slingshot"},
        {"boomerang", "Item_Boomerang", NULL, NULL, "boomerang"},
        {"bow", "Weapon_Range_Bow", "Item_Quiver_Small", "Item_Arrow_Normal", "bow"},
        {"hookshot", "Item_Hookshot", NULL, NULL, "hookshot"},
    };
    int which = -1;
    for (int i = 0; i < 4; i++) if (!strcmp(want, kItems[i].key)) which = i;
    if (which < 0) return;
    if (!given) {
        const int have = oot_pouch_count(c, kItems[which].item);
        if (have < 0) return;
        if (!loaded_at) loaded_at = now;
        if (now - loaded_at < 5000000000LL) return;   /* (the save settled) */
        given = 1;
        if (have == 0) hos_log("vr: tp aim test: %s given (status %d)", kItems[which].item, oot_pouch_change(c, kItems[which].item, 1));
        if (kItems[which].bag && oot_pouch_count(c, kItems[which].bag) <= 0)
            hos_log("vr: tp aim test: %s given (status %d)", kItems[which].bag, oot_pouch_change(c, kItems[which].bag, 1));
        if (kItems[which].ammo) hos_log("vr: tp aim test: %s +30 (status %d, now %d)", kItems[which].ammo,
                                        oot_pouch_change(c, kItems[which].ammo, 30), oot_pouch_count(c, kItems[which].ammo));
    }
    if (now - loaded_at < 7000000000LL) return;
    selected = 1;
    /* (the assignable item's number, by its name in the game's table, as vr/input/quick_select_host.c reads them) */
    const uint64_t table = recomp_load64(c, 0x84ca1140ULL);
    int index = -1;
    for (int i = 0; i <= 0x35 && table >= 0x80000000ULL; i++) {
        const int32_t off = (int32_t)recomp_load32(c, table + 0x250 + 4 * (uint64_t)(i + 1));
        char name[64] = {0};
        if (off >= 0)
            for (int k = 0; k < 63; k++) { name[k] = (char)recomp_load8(c, table + (uint64_t)off + k); if (!name[k]) break; }
        for (int k = 0; name[k]; k++) if (name[k] >= 'A' && name[k] <= 'Z') name[k] = (char)(name[k] + 32);
        hos_log("vr: tp aim test: assignable item %d %s", i, name);
        if (index < 0 && strstr(name, kItems[which].menu) && !strstr(name, "rainbow")) index = i;
    }
    const uint64_t slot = recomp_load64(c, 0x84c9e0d0ULL);
    const uint64_t pouch = slot >= 0x80000000ULL && !(slot & 7) && hos_guest_range(slot, 8) ? recomp_load64(c, slot) : 0;
    A64Fn change = a64_lookup(0x81231c00ULL);
    if (index >= 0 && pouch >= HOS_HEAP_BASE && change) {
        int child = 1;
        if (!oot_gamedata_bool(c, 0xcea220d3u, &child)) child = 1;
        const Ctx saved = *c;
        c->x[31] = (c->x[31] - 0x100) & ~0xfULL;
        c->x[0] = pouch, c->x[1] = child ? 1 : 0, c->x[2] = (uint64_t)index, c->x[3] = 0, c->x[4] = 1;
        change(c);
        *c = saved;
        hos_log("vr: tp aim test: item %d selected (%s)", index, child ? "child" : "adult");
    } else hos_log("vr: tp aim test: %s not selected (item %d)", want, index);
}
#endif

/* (research) the shot's launch, from the arrow-launch probe in third person (vr/items/bow/bow_host.c): A its velocity, B the
   point it aims for, C where it starts */
static volatile int64_t oot_vr_tp_shot_at;   /* (the research: the last shot's launch, its start; the flight followed) */
static float oot_vr_tp_shot_from[3];
static void oot_vr_tp_aim_shot(Ctx *c, uint64_t address, int phase) {
    if (!oot_vr_tp_aim_test() || phase < 1 || phase > 2 || !address) return;
    for (int k = 0; k < 3; k++) oot_vr_tp_shot_from[k] = vr_load(c, address + 0x193c + 4 * k);
    __atomic_store_n(&oot_vr_tp_shot_at, hos_now_ns(), __ATOMIC_RELEASE);
    static unsigned told;
    if (told++ > 40) return;
    float a[3], b[3], v[3];
    for (int k = 0; k < 3; k++)
        a[k] = vr_load(c, address + 0x1948 + 4 * k), b[k] = vr_load(c, address + 0x1954 + 4 * k), v[k] = vr_load(c, address + 0x193c + 4 * k);
    const float *r = oot_vr_tp_aim_last;
    hos_log("vr: tp aim test: shot (phase %d) velocity %.3f %.3f %.3f, aims for %.2f %.2f %.2f, from %.2f %.2f %.2f; the reticle "
            "(kind %d) at %.2f %.2f %.2f", phase, a[0], a[1], a[2], b[0], b[1], b[2], v[0], v[1], v[2], oot_vr_tp_aim_last_kind,
            r[0], r[1], r[2]);
}

void rx_vr_aim_mark(int kind, const float at[3]);   /* (vr/headset/bridges_host.c) */
extern int hos_vr_selected_is_boomerang;   /* (vr/input/quick_select_host.c) */
#define kVrTpAimReach 60.0f           /* (m: how far the slingshot's and bow's line is checked; past it, its end) */
#define kVrTpBoomReach 10.0f          /* (m: the Boomerang's Range, GameParameter/Boomerang: it turns back past it) */

/* Where the shot the game is aiming will land, in the game world: 1, or 0 not aiming (or no answer to believe).
   What decides it (third person): the game's camera, never the headset. The slingshot's and bow's shot, aimed with the aim
   camera (Link's action state 0x2000000, the bow's 0x44x with the item bit 0x400): the point the game's own launch would fly
   to now (cast: oot_vr_tp_aim_answer; a test passes a fake), believed within 2 m of the game camera's line, ahead of it and
   within kVrTpAimReach. The Boomerang raised (0x180400): its throw's TargetPos, 10 m beyond Link along the camera's line
   (vr/items/boomerang). The hookshot is not here: the game's own red laser shows its aim (the user, headset 22 Nov). */
typedef int (*VrTpAimCast)(Ctx *c, uint64_t actor, float reach, float out[3]);
static int oot_vr_tp_aim_point(Ctx *c, uint64_t view, uint32_t state, float at[3], VrTpAimCast cast) {
    float eye[3], look[3], d[3], n = 0;
    for (int k = 0; k < 3; k++) eye[k] = vr_load(c, view + 0x38 + 4 * k), look[k] = vr_load(c, view + 0x44 + 4 * k);
    for (int k = 0; k < 3; k++) d[k] = look[k] - eye[k], n += d[k] * d[k];
    n = sqrtf(n);
    if (!isfinite(n) || n < 1e-4f) return 0;
    for (int k = 0; k < 3; k++) d[k] /= n;
    if (!(state & 0x400u) || (state & 0x80000000u)) return 0;   /* (the item out in Link's hand, not being put away) */
    const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const float reach = kVrTpAimReach;
    if (__atomic_load_n(&hos_vr_selected_is_boomerang, __ATOMIC_RELAXED)) {
        if ((state & 0x180000u) != 0x180000u) return 0;   /* (raised, ready to throw: not at rest, not in flight) */
        /* (its throw's TargetPos, f_82bb4c50: kVrTpBoomReach beyond Link along the camera's forward; headless 22 Nov, the
           throws turned back 11 m out, past it) */
        float along = 0;
        for (int k = 0; k < 3; k++) along += (vr_load(c, player + 796 + 4 * k) - eye[k]) * d[k];
        if (!isfinite(along)) return 0;
        for (int k = 0; k < 3; k++) at[k] = eye[k] + (fmaxf(along, 0) + kVrTpBoomReach) * d[k];
        return 1;
    }
    if (!hos_vr_selected_is_slingshot && __atomic_load_n(&oot_vr_selected_bow, __ATOMIC_RELAXED) != 1) return 0;
    if (!(state & 0x2000000u)) return 0;   /* (aiming: the aim camera's states, 0x2000442 raising, 0x2000446 drawn) */
    float e[3];
    if (!cast(c, player, reach, e)) return 0;
    /* (believed only near the camera's line (within 2 m: the check's starts at Link), ahead of it and within the reach of
       where Link stands) */
    float ahead = 0, off = 0;
    for (int k = 0; k < 3; k++) ahead += (e[k] - eye[k]) * d[k];
    for (int k = 0; k < 3; k++) { const float r = e[k] - eye[k] - ahead * d[k]; off += r * r; }
    if (!isfinite(ahead + off) || ahead < 0 || ahead > reach + 10 || off > 2.0f * 2.0f) {
        static unsigned told;
        if (told++ < 20)
            hos_log("vr: third-person aim: the game's line ended at %.2f %.2f %.2f, %.2f m along the camera's line and %.2f m off "
                    "it: not believed", e[0], e[1], e[2], ahead, sqrtf(off));
        return 0;
    }
    memcpy(at, e, sizeof e);
    return 1;
}

/* Link's aim check object, the hookshot's (vtable 0x84b8b440), found as the aim builder f_80629fa0 finds it: in
   Link's list (components +824: a count at +44, 16-byte entries from +32, a key and the object), by the key 0xfddbd693, then
   its two calls the builder makes (vtable +56, +64, its +148 bit 0 set first); its answer at +152 (the hit or the end), its
   start at +164 (Link's hand). The builder runs it only while the hookshot aims; asked here, it gives the slingshot's and
   bow's aim too: the line from Link along the aim camera's forward, out to about 22 m (headless 22 Nov). It does not stop at
   everything a seed hits (roots 4.5 m ahead: the check went on to its end). Asked on the game's thread; its frames below
   the caller's. 0 when not found. */
static int oot_vr_tp_aim_check(Ctx *c, uint64_t player, float end[3], float start[3]) {
    if (!oot_vr_equipment_ptr(player, 0x278)) return 0;
    const uint64_t parts = recomp_load64(c, player + 624);
    const uint64_t list = oot_vr_equipment_ptr(parts, 832) ? recomp_load64(c, parts + 824) : 0;
    if (!oot_vr_equipment_ptr(list, 48)) return 0;
    const uint32_t count = recomp_load32(c, list + 44);
    const uint64_t items = recomp_load64(c, list + 32);
    if (!count || count > 4096 || !oot_vr_equipment_ptr(items, 16ull * count)) return 0;
    uint64_t q = 0;
    for (uint32_t i = 0; i < count && !q; i++)
        if (recomp_load32(c, items + 16ull * i) == 0xfddbd693u) q = recomp_load64(c, items + 16ull * i + 8);
    if (!oot_vr_equipment_ptr(q, 192)) return 0;
    const uint64_t vt = recomp_load64(c, q);
    A64Fn first = vt >= 0x80000000ULL ? a64_lookup(recomp_load64(c, vt + 56)) : NULL;
    A64Fn second = vt >= 0x80000000ULL ? a64_lookup(recomp_load64(c, vt + 64)) : NULL;
    static unsigned told;
    if (told++ < 4) hos_log("vr: tp aim test: Link's aim check %#llx (vtable %#llx)", (unsigned long long)q, (unsigned long long)vt);
    if (!first || !second) return 0;
    const Ctx saved = *c;
    recomp_store16(c, q + 148, (recomp_load16(c, q + 148) | 1) & 0xffff);
    c->x[31] = (saved.x[31] - 0x200) & ~0xfULL;
    c->x[0] = q, c->x[30] = 0;
    first(c);
    c->x[31] = (saved.x[31] - 0x200) & ~0xfULL;
    c->x[0] = q, c->x[30] = 0;
    second(c);
    *c = saved;
    for (int k = 0; k < 3; k++) end[k] = vr_load(c, q + 152 + 4 * k), start[k] = vr_load(c, q + 164 + 4 * k);
    return isfinite(end[0] + end[1] + end[2]);
}

static float oot_vr_tp_aim_from[3];   /* (the shot's start, as the game would launch it now) */
/* The launch's answer, asked in Link's own update (hos_vr_tp_aim_actor_update: the aimer answers only on the actors' threads)
   while the view wants it, kept for the view (a sequence lock: odd while written) */
static volatile int64_t oot_vr_tp_aim_wanted_at;
static volatile uint32_t oot_vr_tp_launch_seq;
static struct { float to[3], from[3]; int64_t at; } oot_vr_tp_launch;
static int oot_vr_tp_aim_launched(float to[3], float from[3]) {
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t seq = __atomic_load_n(&oot_vr_tp_launch_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) continue;
        memcpy(to, oot_vr_tp_launch.to, 12), memcpy(from, oot_vr_tp_launch.from, 12);
        const int64_t at = oot_vr_tp_launch.at;
        if (__atomic_load_n(&oot_vr_tp_launch_seq, __ATOMIC_ACQUIRE) != seq) continue;
        return at && hos_now_ns() - at < 150000000LL;
    }
    return 0;
}
/* The launch the game would make now: what the arrow shoot (f_82e1f2d0) takes, from its aimer (its descriptor's +0x1968,
   the held weapon's shooter +376: its vtable +16, as f_82e23560 calls it: x1 the velocity, x2 the point it flies to, x3 where it starts, x4 a
   flag byte, all out). The point is where the shot meets the world (headless 22 Nov: the seeds stopped at it, the cliff at
   17.9 m and a root at 10.3 m, where Link's aim check went on to its 22 m end). Asked on the game's thread, after Link's aim
   check (the aimer reads it); its frames below the caller's. 0 when it cannot be asked or its answer is not a point. */
static int oot_vr_tp_aim_launch(Ctx *c, uint64_t player, float to[3], float from[3]) {
    if (!oot_vr_equipment_ptr(player, 0x278)) return 0;
    const uint64_t components = recomp_load64(c, player + 0x270);
    const uint64_t user = oot_vr_equipment_ptr(components, 0x208) ? recomp_load64(c, components + 0x200) : 0;
    /* (the aimer, as the shoot finds it, f_82e1f8c0: the held weapon's actor named by the handle at EquipmentUser +104 (its entry
       +8, generation +16), that actor's components +384 (its shooter), +376 there) */
    uint64_t aimer = 0;
    if (oot_vr_equipment_ptr(user, 128)) {
        const uint64_t entry = recomp_load64(c, user + 104 + 8);
        const uint32_t generation = recomp_load32(c, user + 104 + 16);
        const uint64_t weapon = generation != UINT32_MAX && oot_vr_equipment_ptr(entry, 0x50) && recomp_load32(c, entry + 0x48) == generation
                                ? recomp_load64(c, entry + 0x40) : 0;
        const uint64_t parts = oot_vr_equipment_ptr(weapon, 0x278) ? recomp_load64(c, weapon + 624) : 0;
        const uint64_t shooter = oot_vr_equipment_ptr(parts, 392) ? recomp_load64(c, parts + 384) : 0;
        if (oot_vr_equipment_ptr(shooter, 376 + 16)) aimer = shooter + 376;
    }
    const uint64_t vt = oot_vr_equipment_ptr(aimer, 16) ? recomp_load64(c, aimer) : 0;
    const uint64_t fn = vt >= 0x80000000ULL && hos_guest_range(vt + 16, 8) ? recomp_load64(c, vt + 16) : 0;
    A64Fn launch = fn ? a64_lookup(fn) : NULL;
    if (!launch) {
        static unsigned told;
        if (told++ < 6)
            hos_log("vr: third-person aim: no launch to ask (EquipmentUser %#llx owner %#llx, aimer %#llx, vtable %#llx, %#llx)",
                    (unsigned long long)user, (unsigned long long)(oot_vr_equipment_ptr(user, 0x20) ? recomp_load64(c, user + 0x18) : 0),
                    (unsigned long long)aimer, (unsigned long long)vt, (unsigned long long)fn);
        return 0;
    }
    const Ctx saved = *c;
    const uint64_t base = (saved.x[31] - 0x100) & ~0xfULL;   /* (the velocity at base, the point at +0x10, the start at +0x20) */
    for (uint64_t k = 0; k < 0x40; k += 4) recomp_store32(c, base + k, 0);
    c->x[31] = (base - 0x100) & ~0xfULL;
    c->x[0] = aimer, c->x[1] = base, c->x[2] = base + 0x10, c->x[3] = base + 0x20, c->x[4] = base + 0x30, c->x[30] = 0;
    launch(c);
    *c = saved;
    for (int k = 0; k < 3; k++) to[k] = vr_load(c, base + 0x10 + 4 * k), from[k] = vr_load(c, base + 0x20 + 4 * k);
    if (oot_vr_tp_aim_test()) {
        static unsigned told;
        if (told++ < 12)
            hos_log("vr: tp aim test: the launch asked (aimer %#llx, vtable %#llx, its +16 %#llx): velocity %.3f %.3f %.3f, to %.2f %.2f "
                    "%.2f, from %.2f %.2f %.2f, flag %u", (unsigned long long)aimer, (unsigned long long)vt, (unsigned long long)fn,
                    vr_load(c, base), vr_load(c, base + 4), vr_load(c, base + 8), to[0], to[1], to[2], from[0], from[1], from[2],
                    (unsigned)recomp_load8(c, base + 0x30));
    }
    return isfinite(to[0] + to[1] + to[2] + from[0] + from[1] + from[2]) && (to[0] != 0 || to[1] != 0 || to[2] != 0);
}
/* Where the slingshot's or bow's shot would land now, as oot_vr_tp_aim_point asks it: the point the game's own launch flies to
   (oot_vr_tp_aim_launch, asked in Link's update), after Link's aim check (whose end the shot is aimed along; the reticle there
   until a launch has been asked). Headless 22 Nov: four seeds stopped within 2 cm of the line through it and one update
   short of it (cliff 17.8 m, roots 10.4, 6.7 and 4.5 m); an adult arrow's launch aimed exactly at it. */
static int oot_vr_tp_aim_answer(Ctx *c, uint64_t player, float reach, float out[3]) {
    (void)reach;
    float from[3], to[3], start[3];
    if (!oot_vr_tp_aim_check(c, player, out, from)) return 0;   /* (the check's end, if no launch was asked lately) */
    __atomic_store_n(&oot_vr_tp_aim_wanted_at, hos_now_ns(), __ATOMIC_RELEASE);   /* (Link's next update asks the launch) */
    if (oot_vr_tp_aim_launched(to, start)) memcpy(out, to, sizeof to), memcpy(from, start, sizeof start);
    else {
        static unsigned told;
        if (told++ < 8) hos_log("vr: third-person aim: no launch asked lately: the reticle at the aim check's end");
    }
    memcpy(oot_vr_tp_aim_from, from, sizeof from);
    return 1;
}

/* (vr/camera/view_host.c, each third-person view, before the head turns it) the game camera at view +0x38 (its eye) and
   +0x44 (the point it looks at): the reticle told to the renderer (vr/items/aim/third_person_reticle.inc), then the research */
static void oot_vr_tp_aim_frame(Ctx *c, uint64_t view) {
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    {
        const uint32_t now_state = object ? recomp_load32(c, object + 0x1f60) : 0;
        float at[3] = {0, 0, 0};
        const int kind = oot_vr_tp_aim_point(c, view, now_state, at, oot_vr_tp_aim_answer);
        if (kind) memcpy(oot_vr_tp_aim_last, at, sizeof at);
        oot_vr_tp_aim_last_kind = kind;
        static int was;
        if (kind || was) rx_vr_aim_mark(kind, at);   /* (every frame while it shows; once more as it goes) */
        if (kind != was) {
            static unsigned told;
            if (told++ < 200)
                hos_log("vr: third-person aim: %s", kind ? "the reticle where the game's line along its camera ends" :
                        "no reticle (not aiming)");
            was = kind;
        }
    }
    const int test = oot_vr_tp_aim_test();
    if (!test) return;
#if defined(_WIN32) || defined(__linux__)
    oot_vr_tp_aim_give(c);
#endif
    const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const uint32_t state = object ? recomp_load32(c, object + 0x1f60) : 0;
    static uint32_t said = 0xffffffffu;
    static unsigned told;
    if (state != said && told < 400) told++, hos_log("vr: tp aim test: Link's action state %#x -> %#x", said, state);
    if (state != said && (state & 0x40000000u) && (state & 0x400u) && actor) {   /* (the shot: 0x40000443; followed from Link) */
        for (int k = 0; k < 3; k++) oot_vr_tp_shot_from[k] = vr_load(c, actor + 796 + 4 * k);
        oot_vr_tp_shot_from[1] += 0.9f;
        __atomic_store_n(&oot_vr_tp_shot_at, hos_now_ns(), __ATOMIC_RELEASE);
        hos_log("vr: tp aim test: the shot (state %#x); the reticle (kind %d) at %.2f %.2f %.2f", state, oot_vr_tp_aim_last_kind,
                oot_vr_tp_aim_last[0], oot_vr_tp_aim_last[1], oot_vr_tp_aim_last[2]);
    }
    said = state;
    const int64_t now = hos_now_ns();
    static int64_t logged;
    if (!state || now - logged < 100000000LL) return;
    logged = now;
    float eye[3], at[3], d[3], n = 0;
    for (int k = 0; k < 3; k++) eye[k] = vr_load(c, view + 0x38 + 4 * k), at[k] = vr_load(c, view + 0x44 + 4 * k);
    for (int k = 0; k < 3; k++) d[k] = at[k] - eye[k], n += d[k] * d[k];
    n = sqrtf(n);
    static unsigned lines;
    if (lines++ > 3000) return;
    const float *r = oot_vr_tp_aim_last;
    float in_view[3];   /* (the reticle in the view drawn last frame, the 3x4 at view +8: x right, y up, -z ahead) */
    for (int row = 0; row < 3; row++) {
        in_view[row] = vr_load(c, view + 8 + 4 * (row * 4 + 3));
        for (int j = 0; j < 3; j++) in_view[row] += vr_load(c, view + 8 + 4 * (row * 4 + j)) * r[j];
    }
    hos_log("vr: tp aim test: state %#x camera %.2f %.2f %.2f dir %.4f %.4f %.4f | reticle %d at %.2f %.2f %.2f, in the view %.3f %.3f "
            "%.3f (fov y %.4f, aspect %.4f), the shot from %.2f %.2f %.2f | Link at %.2f %.2f %.2f", state, eye[0], eye[1], eye[2], n > 0 ? d[0] / n : 0,
            n > 0 ? d[1] / n : 0, n > 0 ? d[2] / n : 0, oot_vr_tp_aim_last_kind, r[0], r[1], r[2], in_view[0], in_view[1],
            in_view[2], hos_vr_fov_y, hos_vr_aspect, oot_vr_tp_aim_from[0], oot_vr_tp_aim_from[1], oot_vr_tp_aim_from[2],
            vr_load(c, actor + 796), vr_load(c, actor + 800), vr_load(c, actor + 804));
}

/* (host/hos_main.c hos_oot_lift_player: every actor's movement update) in Link's, while the view wants it, the game's launch
   (oot_vr_tp_aim_launch); (research) the shot's flight: an actor other than Link
   within 2.5 m of the launch's start in the 0.5 s after it, followed for 3 s (its name once, then where it is each update) */
void hos_vr_tp_aim_actor_update(Ctx *c, uint64_t actor) {
    const int64_t wanted = __atomic_load_n(&oot_vr_tp_aim_wanted_at, __ATOMIC_ACQUIRE);
    if (wanted && hos_now_ns() - wanted < 100000000LL && actor == __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED)) {
        float to[3], from[3];
        if (oot_vr_tp_aim_launch(c, actor, to, from)) {   /* (the slingshot or bow aims in third person: the game's launch now) */
            __atomic_add_fetch(&oot_vr_tp_launch_seq, 1, __ATOMIC_ACQ_REL);
            memcpy(oot_vr_tp_launch.to, to, 12), memcpy(oot_vr_tp_launch.from, from, 12), oot_vr_tp_launch.at = hos_now_ns();
            __atomic_add_fetch(&oot_vr_tp_launch_seq, 1, __ATOMIC_ACQ_REL);
        }
    }
    if (!oot_vr_tp_aim_test()) return;
    const int64_t shot = __atomic_load_n(&oot_vr_tp_shot_at, __ATOMIC_ACQUIRE), now = hos_now_ns();
    if (!shot || now - shot > 3000000000LL || !oot_vr_equipment_ptr(actor, 0x328)) return;
    static int64_t for_shot;
    static uint64_t followed;
    static unsigned lines;
    if (for_shot != shot) for_shot = shot, followed = 0;
    if (actor == __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED)) return;
    float p[3], d = 0;
    for (int k = 0; k < 3; k++) p[k] = vr_load(c, actor + 796 + 4 * k), d += (p[k] - oot_vr_tp_shot_from[k]) * (p[k] - oot_vr_tp_shot_from[k]);
    if (!followed) {
        if (now - shot > 500000000LL || !(d < 2.5f * 2.5f)) return;
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        if (!strcmp(name, "PlayerCamera") || !strncmp(name, "Npc_", 4) || !strcmp(name, "Weapon_Range_SlingShot") || !strcmp(name, "Weapon_Range_Bow")) return;
        followed = actor;
        hos_log("vr: tp aim test: the shot is %s %#llx, %.2f m from the launch", name[0] ? name : "?", (unsigned long long)actor, sqrtf(d));
    }
    if (actor != followed || lines++ > 1500) return;
    hos_log("vr: tp aim test: flight %.3f s at %.2f %.2f %.2f", (now - shot) * 1e-9, p[0], p[1], p[2]);
}
