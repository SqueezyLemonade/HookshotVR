/* Hookshot VR: the wrist check HUD, its game side (VR menu: General, Headset, Wrist HUD).

   Turn the inside of the left wrist to the face and a small panel shows on it: hearts, magic, rupees and, in a dungeon, its
   small keys, boss key, map and compass. The panel is drawn by the renderer (vr/menu/wrist_draw.inc) from the game's
   VALUES, read here from the save's data (GameDataMgr, the same flags the save holds), never from the game's 2D HUD: so
   Hookshot's master mode HUD (all the game's 2D UI hidden) stays on.

   What is read (every 200 ms, on the game's thread, from hos_oot_vr_view):
     PlayerStatus.Life, MaxLife (quarter hearts), MP, MPMax (0: no magic yet), CurrentRupee: Int flags, plain reads
       (host/oot_state_tap.c hos_oot_gamedata_int);
     the wallet: oot_wallet_capacity (200, 500, 1000);
     Sequence_CurrentBanc, the map: inside a dungeon, its DungeonInfo.<Banc>.SmallKeyNum (Int) and IsGetBossKey, IsGetMap,
       IsGetCompass (Bool: the game's own getter, oot_gamedata_bool, game thread only).
   For Full's minimap: the map's name (Sequence_CurrentBanc, as a hash), the flags the renderer's map pictures name (a
   later picture of an area, shown once its event has played: rx_vr_wrist_flags), and every frame Link's place and facing.
   And every game frame, what keeps the panel away in a fight (the block bits, kVrWrist*): the sword, the shield or the bow
   out, a VR hand holding the sword or a Deku Stick, a lock-on target, an event playing, the radial menu open, a hurt in
   the last 3 s. The renderer adds its own (the hands moving fast, the VR menu open, a flat frame).

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/items/shield/guard_host.c, vr/items/sword/
   sword_host.c, vr/input/controllers_host.c's users and vr/menu/radial_host.c (it calls their getters). Not compiled on its
   own. */

int rx_vr_wrist(const int32_t *data, int n);
int rx_vr_wrist_flags(uint32_t *hashes, int max);   /* (the Bool flags the renderer's map pictures name, at most max) */
int hos_oot_gamedata_int(uint32_t hash, int64_t *v);   /* (host/oot_state_tap.c) */
int hos_vr_sword_holder(void);    /* (vr/input/controllers_host.c: the VR hand holding the sword, or -1) */
int hos_vr_stick_holder(void);    /* (the same, a Deku Stick) */
int hos_vr_event_playing(void);   /* (vr/camera/view_host.c) */

#define kVrWristLife 0x31ab5580u       /* PlayerStatus.Life (Int, quarter hearts) */
#define kVrWristMaxLife 0xfbe01da1u    /* PlayerStatus.MaxLife */
#define kVrWristMp 0x76b18200u         /* PlayerStatus.MP */
#define kVrWristMpMax 0x12d63a97u      /* PlayerStatus.MPMax (0: no magic) */
#define kVrWristRupees 0xa77921d7u     /* PlayerStatus.CurrentRupee */
#define kVrWristReadNs 200000000LL     /* the values read this often */
#define kVrWristHurtNs 3000000000LL    /* a hurt this recent: in a fight */
#define kVrWristFlagsMax 32            /* map picture flags asked at most */
#define kVrWristLinkFreshNs 200000000LL   /* Link's place this recent: known */
enum {   /* (the block bits: any set, no panel) */
    kVrWristSword = 1, kVrWristShield = 2, kVrWristBow = 4, kVrWristHeld = 8, kVrWristLocked = 16, kVrWristEvent = 32,
    kVrWristRadial = 64, kVrWristHurt = 128,
};
enum { kVrWristBossKey = 1, kVrWristMap = 2, kVrWristCompass = 4 };
/* Each dungeon's flags: DungeonInfo.<banc>.SmallKeyNum, .IsGetBossKey, .IsGetMap, .IsGetCompass, their MurmurHash3s (seed 0,
   matched against the game's GameDataList: vr/tools/test.py check_wrist recomputes them from the names),
   and what each has (its keys, boss key, map and compass, as kVrWrist* bits: the game keeps all four flags for every one) */
enum { kVrWristHasMaps = kVrWristMap | kVrWristCompass, kVrWristHasKeys = 8, kVrWristHasAll = 15 };
typedef struct { const char *banc; uint32_t keys, boss, map, compass; int has; } VrWristDungeon;
static const VrWristDungeon vr_wrist_dungeons[] = {
    {"InsideTheDekuTree", 0x0914ba07u, 0xf6995ceeu, 0x89787395u, 0x06c51ff2u, kVrWristHasMaps},
    {"DodongoCavern", 0x3fdbc3d0u, 0xfaabc084u, 0xa52c3a19u, 0x8b93edf9u, kVrWristHasMaps},
    {"InsideJabuJabuBelly", 0x1470a816u, 0x656bd5eeu, 0xfcadd210u, 0x9a2218dbu, kVrWristHasMaps},
    {"ForestTemple", 0x90237092u, 0x427333a0u, 0xe704f9b4u, 0xf240eeb2u, kVrWristHasAll},
    {"FireTemple", 0x1df11487u, 0xb72d73dfu, 0x5b3ac0bau, 0x0aa068b6u, kVrWristHasAll},
    {"WaterTemple", 0x877bdceeu, 0x79daf80au, 0x4a4402abu, 0x2e82c8feu, kVrWristHasAll},
    {"ShadowTemple", 0x5edda9e3u, 0xc33c66cfu, 0xf6177b93u, 0x1e2feec5u, kVrWristHasAll},
    {"SpiritTemple", 0x79eeec8eu, 0x1984c03fu, 0xb85002bau, 0x9bd1181cu, kVrWristHasAll},
    {"BottomOfTheWell", 0x7dd79d10u, 0x9402715cu, 0x77c47ec9u, 0x9d6efaacu, kVrWristHasMaps | kVrWristHasKeys},
    {"IceCavern", 0xfa5d133du, 0x275f4512u, 0x322f2a94u, 0xea1846e6u, kVrWristHasMaps},
    {"GerudoTrainingGround", 0xea9786f5u, 0x6d155841u, 0x4b36041eu, 0x3d9a2750u, kVrWristHasKeys},
    {"InsideGanonCastle", 0xfbfd2910u, 0x2fd59293u, 0x9bbc0c4au, 0xc4d4734eu, kVrWristHasKeys | kVrWristBossKey},
    {"GerudoFortress", 0x3a28b6a4u, 0x95fbcb7cu, 0xbcaad6ceu, 0x5eb80679u, kVrWristHasKeys},
};
#define VR_WRIST_DUNGEONS (int)(sizeof vr_wrist_dungeons / sizeof vr_wrist_dungeons[0])
/* The dungeon a map is in (its banc name begins with the dungeon's: a boss room's own banc counts), or -1 */
static int vr_wrist_dungeon_of(const char *banc) {
    for (int i = 0; i < VR_WRIST_DUNGEONS; i++) {
        const size_t n = strlen(vr_wrist_dungeons[i].banc);
        if (!strncmp(banc, vr_wrist_dungeons[i].banc, n)) return i;
    }
    return -1;
}

/* The map's name as the renderer's map table has it (vr/tools/generate/wrist_maps.py fnv1a) */
static uint32_t vr_wrist_banc_hash(const char *s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x01000193u;
    return h;
}

/* The payload (vr/menu/wrist_draw.inc wrist_set): what the panel shows, why it may not, then the minimap's: the map's name
   hash, Link's place (mm) and facing (hundredths of a degree, atan2 of his forward's x and z), whether those are known, the
   asked flags set (bit k: rx_vr_wrist_flags' k-th) */
enum { kVrWristShown, kVrWristLifeAt, kVrWristMaxLifeAt, kVrWristMpAt, kVrWristMpMaxAt, kVrWristRupeesAt, kVrWristWalletAt,
       kVrWristKeysAt, kVrWristDungeonAt, kVrWristBlockAt, kVrWristBancAt, kVrWristXAt, kVrWristYAt, kVrWristZAt,
       kVrWristFacingAt, kVrWristLinkAt, kVrWristFlagsAt, kVrWristN };
typedef struct {
    int32_t v[kVrWristN];
    int64_t read_at, hurt_at;
    int logged;
} VrWrist;
static VrWrist oot_vr_wrist;

/* Why the panel may not show now (kVrWrist* bits), from what the game has: drawn (oot_vr_equipment_drawn_now: 1 the sword,
   2 the shield, 4 the bow), the VR hands' holds, the lock-on, an event, the radial menu, a recent hurt */
static int32_t vr_wrist_block(uint32_t drawn, int sword_holder, int stick_holder, int locked, int event, int radial,
                              int64_t now, int64_t hurt_at) {
    int32_t b = 0;
    if (drawn & 1) b |= kVrWristSword;
    if (drawn & 2) b |= kVrWristShield;
    if (drawn & 4) b |= kVrWristBow;
    if (sword_holder >= 0 || stick_holder >= 0) b |= kVrWristHeld;
    if (locked == 1) b |= kVrWristLocked;
    if (event) b |= kVrWristEvent;
    if (radial) b |= kVrWristRadial;
    if (hurt_at && now - hurt_at < kVrWristHurtNs) b |= kVrWristHurt;
    return b;
}

/* The values into w->v (shown 0: no save loaded). Life falling since the last read: a hurt (hurt_at) */
static void vr_wrist_values(VrWrist *w, int64_t now, const int64_t ints[5], int ints_ok, int wallet, int dungeon,
                            int64_t keys, int bits) {
    const int32_t was_life = w->v[kVrWristShown] ? w->v[kVrWristLifeAt] : -1;
    if (!ints_ok || ints[1] <= 0 || ints[1] > 400) {
        w->v[kVrWristShown] = 0;
        return;
    }
    w->v[kVrWristShown] = 1;
    w->v[kVrWristLifeAt] = (int32_t)(ints[0] < 0 ? 0 : ints[0] > ints[1] ? ints[1] : ints[0]);
    w->v[kVrWristMaxLifeAt] = (int32_t)ints[1];
    w->v[kVrWristMpMaxAt] = (int32_t)(ints[3] < 0 ? 0 : ints[3] > 1000 ? 1000 : ints[3]);
    w->v[kVrWristMpAt] = (int32_t)(ints[2] < 0 ? 0 : ints[2] > w->v[kVrWristMpMaxAt] ? w->v[kVrWristMpMaxAt] : ints[2]);
    w->v[kVrWristRupeesAt] = (int32_t)(ints[4] < 0 ? 0 : ints[4] > 9999 ? 9999 : ints[4]);
    w->v[kVrWristWalletAt] = wallet;
    w->v[kVrWristKeysAt] = dungeon < 0 ? -1 : (int32_t)(keys < 0 ? 0 : keys > 99 ? 99 : keys);
    w->v[kVrWristDungeonAt] = dungeon < 0 ? 0 : bits | vr_wrist_dungeons[dungeon].has << 4;   /* (owned, then what it has) */
    if (was_life >= 0 && w->v[kVrWristLifeAt] < was_life) w->hurt_at = now;
}

/* Every game frame with the headset on (hos_oot_vr_view): the values now and then, the block bits always, sent */
static void oot_vr_wrist_frame(Ctx *c) {
    VrWrist *w = &oot_vr_wrist;
    const int64_t now = hos_now_ns();
    if (!w->read_at || now - w->read_at >= kVrWristReadNs) {
        w->read_at = now;
        static const uint32_t hashes[5] = {kVrWristLife, kVrWristMaxLife, kVrWristMp, kVrWristMpMax, kVrWristRupees};
        int64_t ints[5] = {0, 0, 0, 0, 0};
        int ok = 1;
        for (int i = 0; i < 5; i++) {
            int64_t v = 0;
            if (hos_oot_gamedata_int(hashes[i], &v)) ints[i] = v;
            else if (i < 2) ok = 0;   /* (no life: no save loaded) */
        }
        int dungeon = -1, bits = 0, wallet = 200;
        int64_t keys = 0;
        uint32_t banc_hash = 0, flags = 0;
        if (ok && ints[1] > 0) {
            char banc[64];
            oot_gamedata_string(c, 0x1d6189dau, banc, sizeof banc);   /* Sequence_CurrentBanc (its return is always 0: the text) */
            dungeon = banc[0] ? vr_wrist_dungeon_of(banc) : -1;
            banc_hash = banc[0] ? vr_wrist_banc_hash(banc) : 0;
            uint32_t asked[kVrWristFlagsMax];
            const int n = rx_vr_wrist_flags(asked, kVrWristFlagsMax);
            for (int k = 0; k < n && k < kVrWristFlagsMax; k++) {
                int b = 0;
                if (oot_gamedata_bool(c, asked[k], &b) && b) flags |= 1u << k;
            }
            if (dungeon >= 0) {
                const VrWristDungeon *d = &vr_wrist_dungeons[dungeon];
                int b = 0;
                if (!hos_oot_gamedata_int(d->keys, &keys)) keys = 0;
                if (oot_gamedata_bool(c, d->boss, &b) && b) bits |= kVrWristBossKey;
                if (oot_gamedata_bool(c, d->map, &b) && b) bits |= kVrWristMap;
                if (oot_gamedata_bool(c, d->compass, &b) && b) bits |= kVrWristCompass;
            }
            wallet = oot_wallet_capacity(c);
        }
        int32_t before[kVrWristN];
        memcpy(before, w->v, sizeof before);
        vr_wrist_values(w, now, ints, ok, wallet, dungeon, keys, bits);
        w->v[kVrWristBancAt] = (int32_t)banc_hash, w->v[kVrWristFlagsAt] = (int32_t)flags;
        if (w->v[kVrWristShown] && memcmp(before, w->v, kVrWristBlockAt * sizeof before[0]) && w->logged < 400) {
            w->logged++;   /* (bounded: a line a change, the headset's checklist reads them) */
            char k[16] = "-";
            if (w->v[kVrWristKeysAt] >= 0) snprintf(k, sizeof k, "%d", (int)w->v[kVrWristKeysAt]);
            hos_log("vr: wrist: life %d/%d mp %d/%d rupees %d/%d keys %s%s%s%s", (int)w->v[kVrWristLifeAt],
                    (int)w->v[kVrWristMaxLifeAt], (int)w->v[kVrWristMpAt], (int)w->v[kVrWristMpMaxAt],
                    (int)w->v[kVrWristRupeesAt], (int)w->v[kVrWristWalletAt], k,
                    w->v[kVrWristDungeonAt] & kVrWristBossKey ? " boss key" : "",
                    w->v[kVrWristDungeonAt] & kVrWristMap ? " map" : "",
                    w->v[kVrWristDungeonAt] & kVrWristCompass ? " compass" : "");
        }
    }
    const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    {   /* (Link's place, as the view reads it: view_host.c oot_vr_realign_frame) */
        const int64_t seen = __atomic_load_n(&oot_vr_player_seen, __ATOMIC_ACQUIRE);
        float pos[3], fx = 0, fz = 0;
        int known = seen && now - seen <= kVrWristLinkFreshNs && actor && hos_guest_range(actor, 844);
        for (int k = 0; k < 3; k++) {
            const uint32_t bits = __atomic_load_n(&oot_vr_player_xyz[k], __ATOMIC_RELAXED);
            memcpy(&pos[k], &bits, sizeof pos[k]);
            known = known && isfinite(pos[k]) && fabsf(pos[k]) < 1e5f;
        }
        if (known) fx = vr_load(c, actor + 816), fz = vr_load(c, actor + 840);
        known = known && isfinite(fx) && isfinite(fz) && hypotf(fx, fz) > .1f;
        w->v[kVrWristLinkAt] = known;
        if (known) {
            for (int k = 0; k < 3; k++) w->v[kVrWristXAt + k] = (int32_t)lrintf(pos[k] * 1000);
            w->v[kVrWristFacingAt] = (int32_t)lrintf(atan2f(fx, fz) * 5729.578f);
        }
    }
    w->v[kVrWristBlockAt] = vr_wrist_block(__atomic_load_n(&oot_vr_equipment_drawn_now, __ATOMIC_RELAXED), hos_vr_sword_holder(),
                                           hos_vr_stick_holder(), actor ? oot_vr_lock_point(c, actor, NULL) : 0,
                                           hos_vr_event_playing(), hos_vr_radial_open(), now, w->hurt_at);
    rx_vr_wrist(w->v, kVrWristN);
}
