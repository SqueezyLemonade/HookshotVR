/* Hookshot VR: bombs in the VR hands (game side). "Bombs in the hands" (VR tab, option 904).

   Grip means "take hold": a grip closed near a bomb (on the ground, in the air, in the other hand) takes it, holding it
   exactly as it was gripped: upside down, by the fuse, wherever the hand closed on it. With bombs selected, a grip closed
   anywhere with no bomb or Bomb Flower within BOMB_REACH of the hand takes a new bomb out (nearest first: one within reach,
   on its plant, on the ground or in the air, is taken instead, so a catch is a catch): the mod taps the game's item button
   (ZR) so Link takes one out, the game's own lifter call ends his hold on it (OneShotLifterTryRequestFall, vr_bomb_lifter_
   fall), and the bomb is put in the hand from the moment it appears (the renderer draws it in the palm while his lift has
   it: no detour over his head). A held bomb is placed every frame with
   the hand (position and rotation, as the game's ExecuteActorForceSetPos does), moving with the hand's velocity. Letting go hands it the hand's velocity (averaged over
   the last 3 samples, times the VR tab's Bomb throw strength), and it flies on an arc of the VR tab's Bomb gravity (real
   gravity by default; the game's is nearly three times that) until it hits something: from there it is the game's physics
   (bounces, rolling).

   A bomb is unlit until the trigger of the hand holding it is pulled. The game's fuse is its Bomb_Blink animation
   (Item_Bomb.root.ainb: play Bomb_Blink, then request death, which is the explosion); unlit, that animation is held at one
   frame (the Debug tab's Unlit bomb frame) before frame 20, where the animation's own events start the fuse's sound, flame
   and sparks. Lit, it plays: the game's own blinking, fuse and about 4 s to the explosion. (Switching the bomb's effects off
   with OneShotXLinkSetCalcAll's f_8297bb40 stops them updating rather than removing them: emitted puffs froze in the world.) Arrows and fire set bombs off as they always do (the game's chemistry).

   Game 1.1.1, found 16 November 2026: the bomb AI node ExecuteBarrelBombAI's update
   (f_826584b0, main+0x26384b0) names the bomb (node +8 -> +8 -> +496); every actor's movement update (host/hos_main.c
   hos_oot_lift_player) is where a bomb is steered. On the actor: the physics component +624 -> +80; f_804e6630(physics,
   &position, &rotation 3x3, 1) places it (rotation NaN: kept); the main rigid body +80 -> +32 -> +400, f_8014dbe0(body, &v, 0)
   its linear velocity, f_8014b790(body, &w) its angular velocity; the animation controller
   +624 -> +64 -> +24 -> +24, f_837d5d70(controller, slot, frame) an animation's frame.

   An unlit bomb is quiet (VR tab Unlit bomb fire: None): its fuse fire, sparks, smoke and fuse sound are its XLink (the
   ELink and SLink users Item_Bomb: action and property triggers, not the animation's events, which only hold AtCollision),
   and that XLink is put to sleep while the bomb is the mod's and unlit, as the game's own OneShotXLinkSleep does
   (f_80994100(xlink, reason bits): the XLink component is actor +624 -> +104; a reason bit of the mod's own, so the game's
   own wake for its reasons does not wake it). Asleep, the effects are taken away (not frozen, as SetCalcAll left them) and
   no new ones start; the bomb's logic (its AI, its physics, the explosion) runs on. Lit, it wakes (f_80994060, the game's
   OneShotXLinkCancelSleep). It also wakes the moment its animation leaves Bomb_Blink: set off unlit (an arrow, fire,
   another explosion), its explosion shows.

   The throw arc (Bomb aim: Arc to target; PARKED 19 Nov, and its options 951/952 out of the menu and the options file for
   the release: it ended on a flat plane at Link's feet, not on the ground it would hit; only RX_VR_BOMB_AIM in a research
   build, or a check's own option 951, still reaches it): while a hand holds a bomb, a dotted arc from it along the way that hand
   points, at the speed that reaches Bomb arc reach at 45 degrees, under the VR tab's Bomb gravity, and a ring where it
   comes down to the height of Link's feet (the ground is taken as flat there: the game is not asked where the ground is),
   or on Link's Z-target, when he has one (thrown at the angle the hand points, or the nearest that reaches it). A throw
   (let go, or squeeze again, faster than BOMB_AIM_THROW) leaves along that arc instead of the arm's own velocity. The
   arithmetic is bomb_math.h; the renderer draws the marks (vr/items/bomb/bomb_marks.inc).

   Bomb Flowers (Obj_BombFlower_*, actor BombFruit: Goron City, Death Mountain, Dodongo's Cavern) are found by name near
   Link as well as by their bomb AI. A VR grip picks one whatever is selected (VR tab Bomb Flowers in the hands), once the
   Goron's Bracelet (or a later gauntlet) is owned, as the game's own pick asks (QueryBombFruitCanPlayerPickNoGetGoron-
   Gauntlet). Picked by a hand, it is quiet and unlit like a bomb until that hand's trigger.

   The gravity pull (VR tab Bomb pull): an open hand whose palm faces a bomb out of reach (within Bomb pull reach, and Bomb
   pull aim either side of the line straight out of the palm) chooses it, and the renderer draws it pulsing; squeezing that
   grip flies it into the hand (placed along the way each update), where it is held as if gripped there. A Bomb Flower
   still on its plant is not flown (its body is fixed there): the game picks it, with Link beside it.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
#include "bomb_math.h"

#define BOMB_MAX 32                 /* (bombs tracked at once; full, the oldest loose one is let go of) */
#define BOMB_RADIUS_GAME 0.22f      /* (m) the bomb's radius (Item_Bomb_Body_0: 0.45 m across) */
#define BOMB_RADIUS (BOMB_RADIUS_GAME * rx_option(909, 100) * 0.01f)   /* (as drawn: the VR tab's Bomb size) */
#define BOMB_CENTRE 0.2187f         /* (m) the bomb's centre above its origin (Item_Bomb_Body_0's centre) */
/* (a Bomb Flower's fruit has its own: its body's centre above its origin and its radius, from your game's model through the
   renderer, rx_vr_flower_shape: vr/tools/generate/bomb_flower.py. Until it answers, a bomb's) */
#define BOMB_REACH 0.25f            /* (m) a grip this close to a bomb's surface takes it, before any new one is taken out */
#define BOMB_PALM 0.03f             /* (m) the palm's surface from the grip pose, along the palm's direction */
#define BOMB_SEAT_NS 100000000LL   /* (ns) a gripped bomb slides into its seat against the palm over this long */
#define BOMB_MAX_SPEED 30.0f        /* (m/s) no thrown bomb is sent faster */
/* collision layers (the game's layer entities, in its own order: Object 0, ... HitOnlyGround 10, ... NoHit 14) */
#define LAYER_OBJECT 0              /* a bomb's own (Item_Bomb's RigidBodyEntityParam LayerEntity) */
#define LAYER_HIT_ONLY_GROUND 10    /* just let go: it lands and bounces, but cannot touch Link */
#define LAYER_NO_HIT 14             /* held: it collides with nothing (it is placed with the hand) */
#define BOMB_CLEAR_LINK 1.2f        /* (m) let go, it is Object again this far from Link, or after a second */
#define BOMB_REAL_G 9.81f           /* (m/s2) real gravity: the VR tab's Bomb gravity is a percent of it (the game's is about 28) */
#define BOMB_UNLIT_FRAME 18.0f      /* the Bomb_Blink frame an unlit bomb is held at (the Debug tab's Unlit bomb frame) */
/* unlit bombs' effects asleep: the mod's own reason bit in the XLink's sleep reasons (the game's AI uses bit 0) */
#define BOMB_SLEEP_REASON 0x40000000u
/* the throw arc (VR tab Bomb aim) */
#define BOMB_AIM_THROW 1.5f         /* (m/s) a let-go faster than this throws along the arc; slower drops the bomb */
#define BOMB_AIM_DOWN -0.75f        /* the arc hides while the hand points more steeply down than this (sine: about 50 degrees) */
#define BOMB_ARC_DOTS 8             /* the arc's dots */
#define BOMB_MARKS_MAX 12           /* (marks sent to the renderer at most: vr/items/bomb/bomb_marks.inc kBombMarksMax) */
/* the gravity pull (VR tab Bomb pull) */
#define BOMB_PULL_SPEED 9.0f        /* (m/s) a pulled bomb flies to the hand this fast... */
#define BOMB_PULL_MIN_S 0.25f       /* ...taking at least this long */
#define BOMB_PULL_MAX_S 1.5f        /* (s) a pull that has not arrived by now is given up (stuck on something) */
#define BOMB_PULL_KEEP 1.4f         /* a ringed bomb stays chosen until it is this many times the aim angle off */
/* Bomb Flowers found near Link by name (m: within the pull's reach of him, and this much more) */
#define BOMB_FLOWER_LOOK 1.0f
#define BOMB_FLOWER_PICK 1.6f      /* (m) a fruit on its plant is picked (by the game: Link beside it) only this near Link */
#define BOMB_FLOWER_PICK_NS 1500000000LL   /* (ns) a pick the game has not made by then is given up */
#define BOMB_SETTLE_NS 700000000LL /* (ns) a bomb's animation may change this long after a pick without counting as set off */

int rx_vr_hands_world(float out[42]);
int rx_vr_body_heading(float *yaw);
void rx_vr_hand_holds(int hand, float radius, const float centre[3]);
int rx_vr_hand_ray(int hand, float out[6]);
void rx_vr_bomb_marks(const float *marks, int n);
void rx_vr_bomb_state(const float state[8]);
int rx_vr_flower_shape(float out[4]);
int hos_vr_ocarina_claims(int hand);   /* (a grip at the belt ocarina takes it, not a new bomb: vr/items/ocarina) */
int hos_vr_sword_claims(int hand);     /* (the sword's hand, the shield's beside it, a grip behind the shoulder: vr/input) */
extern int hos_vr_selected_is_bomb;

typedef struct {
    uint64_t actor;          /* (first, and stored whole: vr_bomb_find reads it without the lock, vr_bomb_track clears the rest) */
    int64_t born, seen, released_at;
    int holder;              /* the hand holding it (0 left, 1 right), or -1 */
    int lit;
    int owned;               /* the mod's: taken from the bag, or gripped. Until then (a bomb flower, any bomb the hands never
                                had) the game's own: no unlit hold, so explosions, arrows and fire set it off as the game does */
    int release;             /* frames left to send release_v (a body still attached ignores one) */
    float release_v[3];
    int grip_set;            /* how it sits in the hand is known: rel_p, rel_r */
    float centre[3];         /* where its centre was at its last update (for reach checks off its update) */
    int layer;               /* its collision layer as last set (-1: the game's own) */
    int flight;              /* thrown or dropped, flying on the mod's arc (the VR tab's Bomb gravity) until it hits something */
    float flight_v[3], flight_last[3];
    int64_t flight_at, flight_since;
    float rel_p[3], rel_r[9];   /* its origin and rotation in the hand's axes (rel_p: where it is seated) */
    float seat_from[3];         /* (rel_p as gripped, sliding to rel_p from seat_at) */
    int64_t seat_at;
    int asleep;              /* its effects (XLink) put to sleep by the mod: unlit and quiet */
    uint64_t asleep_cmd;     /* its animation's command when they were (another one now: it is exploding, wake it) */
    int flower;              /* a Bomb Flower's fruit (by its actor name) */
    int pull_hand;           /* the hand pulling it to itself (the gravity pull), or -1 */
    int64_t pull_at, pull_last;
    int on_plant;            /* a Bomb Flower's fruit the game has not picked: fixed on its plant, its fuse not running */
    int64_t picking_at;      /* (on_plant) a hand asked the game to pick it then (the game's A) */
    int64_t settle_until;    /* its animation may change until then without counting as set off (a pick, a pull-out) */
} VrBomb;
static VrBomb vr_bombs[BOMB_MAX];

typedef struct {
    int valid;
    float p[3], v[3], w[3], trigger, grip, axis[3][3];   /* (axis: the grip's x, y, z in the world; w its angular velocity) */
    int aims;                /* the pointing line is known: */
    float aim_from[3], aim[3];   /* the controller's aim pose (where it points from, and the way: unit) */
} VrBombHand;
static float vr_bomb_link_v[3];              /* Link's own velocity (his root's, smoothed): held and thrown bombs move with him */
static VrBombHand vr_bomb_hand[2];           /* (written by the player's update, read by the bombs') */
static float vr_bomb_link_at[3];             /* Link's root, as at his last update (Bomb Flowers are looked for near it) */
static uint32_t vr_bomb_link_bits[3];        /* (the same, as atomic words: the lock-free look for Bomb Flowers reads these) */
static float vr_bomb_vel_hist[2][3][3];      /* the last 3 velocity samples per hand */
static int vr_bomb_vel_n[2];

/* the pull-out: a ZR tap for the game, the bomb in the hand from when it appears */
enum { PULL_IDLE, PULL_TAKE, PULL_THROW, PULL_CATCH };
static int vr_bomb_pull = PULL_IDLE, vr_bomb_pull_hand;
static int64_t vr_bomb_pull_at, vr_bomb_zr_until, vr_bomb_a_until;   /* (the game's ZR, the take-out, and A, a pick, pressed until then) */
static uint64_t vr_bomb_pulled;
static int vr_bomb_owned;                    /* bombs selected and the feature on: the grips and triggers are the bombs' */

static int vr_bomb_test(void) {
#ifndef HOOKSHOT_VR_RESEARCH
    return 0;   /* (the headless test and its simulated hands: research builds only) */
#endif
    static int t = -1;
    if (t < 0) { const char *e = vr_research_getenv("RX_VR_BOMB_TEST"); t = e ? atoi(e) : 0; if (t) hos_log("vr: bombs: headless test %d (simulated hands)", t); }
    return t;
}
static int vr_bomb_enabled(void) { return vr_bomb_test() || rx_option(904, 0); }

/* ---- one lock for the bombs' records (the game side) ----------------------------------------------------------------------
   The game runs actors' updates on several worker threads (BaseProcExecutor0 to 5 in the log), so Link's update (the hands,
   the grips, the pull-out) and a bomb's (its placement, its fuse, its flight) can run on different threads, both reading and
   writing the same record. Every game-side entry that touches the records holds this lock (vr_bomb_enter, vr_bomb_leave):
   Link's update, a tracked bomb's update, the bomb AI's hook. Actors that are not bombs (nearly all) never take it: their
   check reads only atomic words. The same thread may take it again without waiting (a game call made while it is held
   could reach another actor's update). An entry that had to wait is counted, and the count logged every 10 s, so whether
   two updates really run at the same time shows in any run's log (review finding 6, 20 Nov: not known then).
   The pad read (the input side) never takes it: it reads vr_bomb_input, below. */
static SRWLOCK vr_bomb_lock = SRWLOCK_INIT;
static _Thread_local int vr_bomb_lock_depth;
static uint32_t vr_bomb_lock_waits, vr_bomb_lock_waits_total;   /* (atomic: entries that found it held) */
static void vr_bomb_enter(void) {
    if (vr_bomb_lock_depth++) return;
    if (!TryAcquireSRWLockExclusive(&vr_bomb_lock)) {
        __atomic_add_fetch(&vr_bomb_lock_waits, 1, __ATOMIC_RELAXED);
        AcquireSRWLockExclusive(&vr_bomb_lock);
    }
}
static void vr_bomb_leave(void) {
    if (!--vr_bomb_lock_depth) ReleaseSRWLockExclusive(&vr_bomb_lock);
}

/* ---- what the input side reads (the pad read: hos_vr_bomb_buttons, hos_vr_bomb_in_reach, hos_vr_bomb_grips) ----------------
   Written once at the end of each of Link's updates (vr_bomb_publish), read by the pad read on its own thread: one snapshot,
   so a read sees one update's state or the next's, never a mix, and never a bomb record (the records, the actors, the hands'
   positions stay with the game side). A seqlock: its count is odd while it is written; every field is written and read whole
   (relaxed atomics), the count's release and acquire order them. One writer: Link's update, under vr_bomb_lock. */
typedef struct {
    int32_t owned;               /* bombs selected and the feature on: the grips and the right trigger are the bombs' */
    int32_t holds[2];            /* a bomb or a Bomb Flower in this hand: its trigger lights it, not the game's */
    int32_t claims[2];           /* this hand's grip is the bombs': a bomb held, pulled or pointed at by it, or within its reach */
    int64_t zr_until, a_until;   /* the game's ZR and A pressed until then (ns): the take-out's tap, a pick */
    int64_t at;                  /* when it was published (ns): an old one (Link not updated: a map load, the feature off) is
                                    nothing (vr_bomb_input_now) */
} VrBombInput;
#define BOMB_INPUT_STALE_NS 500000000LL
static uint32_t vr_bomb_input_seq;
static VrBombInput vr_bomb_input;
static void vr_bomb_publish(const VrBombInput *in) {
    const uint32_t s = __atomic_load_n(&vr_bomb_input_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_bomb_input_seq, s + 1, __ATOMIC_RELAXED);   /* (odd: being written) */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&vr_bomb_input.owned, in->owned, __ATOMIC_RELAXED);
    for (int h = 0; h < 2; h++) {
        __atomic_store_n(&vr_bomb_input.holds[h], in->holds[h], __ATOMIC_RELAXED);
        __atomic_store_n(&vr_bomb_input.claims[h], in->claims[h], __ATOMIC_RELAXED);
    }
    __atomic_store_n(&vr_bomb_input.zr_until, in->zr_until, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_bomb_input.a_until, in->a_until, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_bomb_input.at, in->at, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_bomb_input_seq, s + 2, __ATOMIC_RELEASE);   /* (even: whole) */
}
/* (the pad read's copy: a few tries while it is being written, then nothing claimed, nothing pressed) */
static void vr_bomb_read_input(VrBombInput *out) {
    for (int tries = 0; tries < 64; tries++) {
        const uint32_t s = __atomic_load_n(&vr_bomb_input_seq, __ATOMIC_ACQUIRE);
        if (s & 1) continue;
        out->owned = __atomic_load_n(&vr_bomb_input.owned, __ATOMIC_RELAXED);
        for (int h = 0; h < 2; h++) {
            out->holds[h] = __atomic_load_n(&vr_bomb_input.holds[h], __ATOMIC_RELAXED);
            out->claims[h] = __atomic_load_n(&vr_bomb_input.claims[h], __ATOMIC_RELAXED);
        }
        out->zr_until = __atomic_load_n(&vr_bomb_input.zr_until, __ATOMIC_RELAXED);
        out->a_until = __atomic_load_n(&vr_bomb_input.a_until, __ATOMIC_RELAXED);
        out->at = __atomic_load_n(&vr_bomb_input.at, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vr_bomb_input_seq, __ATOMIC_RELAXED) == s) return;
    }
    memset(out, 0, sizeof *out);
}
/* (what the input side acts on: the snapshot, or nothing when it is old or the feature is off. The deadlines are times, so
   an old one has passed anyway) */
static void vr_bomb_input_now(VrBombInput *out) {
    vr_bomb_read_input(out);
    if (!vr_bomb_enabled() || hos_now_ns() - out->at > BOMB_INPUT_STALE_NS) memset(out, 0, sizeof *out);
}

static float vr_bomb_f(Ctx *c, uint64_t a) { const uint32_t w = recomp_load32(c, a); float f; memcpy(&f, &w, 4); return f; }
static void vr_bomb_put_f(Ctx *c, uint64_t a, float f) { uint32_t w; memcpy(&w, &f, 4); recomp_store32(c, a, w); }

/* (a record's actor is the one field read without vr_bomb_lock, by vr_bomb_tracked: it is only ever stored whole) */
static VrBomb *vr_bomb_find(uint64_t actor) {
    for (int i = 0; i < BOMB_MAX; i++) if (__atomic_load_n(&vr_bombs[i].actor, __ATOMIC_ACQUIRE) == actor) return &vr_bombs[i];
    return NULL;
}

/* A Bomb Flower's fruit, by its actor's name: Obj_BombFlower_A_01 and _A_04, and the fruit by Death Mountain Trail's
   breakable rocks (Obj_BombFlower_Fruit_*). Not its plant, the leaves the fruit grows on: Obj_BombFlower_A_02, _A_03 and
   Obj_BombFlower_Grass_* (their actor packs: the fruits' model is Obj_BombFlower_A_01, the plants' Obj_BombFlower_A_03,
   18 Nov; a grip took the plant in a dungeon before) */
static int vr_bomb_is_flower(Ctx *c, uint64_t actor) {
    char name[64];
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    return !strcmp(name, "Obj_BombFlower_A_01") || !strcmp(name, "Obj_BombFlower_A_04") ||
           !strncmp(name, "Obj_BombFlower_Fruit", 20);
}

static void vr_bomb_centre(Ctx *c, uint64_t actor, float out[3]);

/* a bomb tracked from now on (none: the table is full of held ones). flower: 1 a Bomb Flower whose bomb AI runs (picked: its
   fuse is the game's), 2 one found by its name near Link (waiting on its plant) */
static VrBomb *vr_bomb_track(Ctx *c, uint64_t actor, int flower) {
    VrBomb *slot = NULL;
    for (int i = 0; i < BOMB_MAX && !slot; i++) if (!vr_bombs[i].actor) slot = &vr_bombs[i];
    for (int i = 0; i < BOMB_MAX && !slot; i++)   /* (full: the oldest loose bomb is no longer tracked, as the game left it) */
        if (vr_bombs[i].holder < 0 && vr_bombs[i].pull_hand < 0 && (!slot || vr_bombs[i].born < slot->born)) slot = &vr_bombs[i];
    if (!slot) return NULL;
    const int64_t now = hos_now_ns();
    __atomic_store_n(&slot->actor, 0, __ATOMIC_RELEASE);   /* (the rest cleared after: the actor is stored whole, alone) */
    memset((char *)slot + sizeof slot->actor, 0, sizeof *slot - sizeof slot->actor);
    slot->holder = slot->pull_hand = -1, slot->born = slot->seen = now, slot->layer = -1;
    slot->flower = flower, slot->on_plant = flower == 2;
    __atomic_store_n(&slot->actor, actor, __ATOMIC_RELEASE);
    vr_bomb_centre(c, actor, slot->centre);
    char name[64];
    oot_vr_equipment_name(c, actor, 0x260, name);
    hos_log("vr: bombs: a %s appeared (%#llx, %s, %s), unlit", flower ? "Bomb Flower" : "bomb", (unsigned long long)actor,
            name[0] ? name : "?", flower == 2 ? "found near Link: on its plant" : "its bomb AI runs");
    return slot;
}

/* main+0x26384b0: the bomb AI's update. The first time a bomb's AI runs, it is one of ours */
void hos_vr_bomb_ai(Ctx *c) {
    if (!vr_bomb_enabled()) return;
    const uint64_t node = c->x[0];
    const uint64_t a = hos_guest_range(node + 8, 8) ? recomp_load64(c, node + 8) : 0;
    const uint64_t b = a >= HOS_HEAP_BASE && hos_guest_range(a + 8, 8) ? recomp_load64(c, a + 8) : 0;
    const uint64_t actor = b >= HOS_HEAP_BASE && hos_guest_range(b + 496, 8) ? recomp_load64(c, b + 496) : 0;
    if (actor < HOS_HEAP_BASE) return;
    vr_bomb_enter();
    VrBomb *seen = vr_bomb_find(actor);
    if (seen) {   /* (a Bomb Flower found on its plant: its bomb AI running now, the game has picked it) */
        if (seen->on_plant) seen->on_plant = 0, seen->settle_until = hos_now_ns() + BOMB_SETTLE_NS,
                            hos_log("vr: bombs: a Bomb Flower's bomb AI runs: picked, its fuse the game's");
        vr_bomb_leave();
        return;
    }
    vr_bomb_track(c, actor, vr_bomb_is_flower(c, actor));
    vr_bomb_leave();
}

/* ---- calling the game -------------------------------------------------------------------------------------------------- */
/* a game function called from here, on a scratch stack below the caller's: x0, x2, x3, s0, and up to 12 floats copied to
   scratch memory (3 floats, their address in x1; 9 floats, their address in x2) */
static uint64_t vr_bomb_call(Ctx *c, uint64_t fn, uint64_t x0, uint64_t x2, uint64_t x3, float s0, const float *a, const float *b9) {
    A64Fn f = a64_lookup(fn);
    if (!f) return 0;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x200) & ~0xfULL;
    const uint64_t scratch = c->x[31] + 0x100;
    uint64_t x1 = 0;
    if (a) { x1 = scratch; for (int i = 0; i < 3; i++) vr_bomb_put_f(c, scratch + 4 * i, a[i]); recomp_store32(c, scratch + 12, 0); }
    if (b9) { x2 = scratch + 0x20; for (int i = 0; i < 9; i++) vr_bomb_put_f(c, x2 + 4 * i, b9[i]); }
    c->x[0] = x0, c->x[1] = x1, c->x[2] = x2, c->x[3] = x3;
    uint32_t sw; memcpy(&sw, &s0, 4); a64_vset64(c->vreg[0], sw);
    f(c);
    const uint64_t answer = c->x[0];   /* (what it returned) */
    *c = saved;
    return answer;
}

static uint64_t vr_bomb_chain(Ctx *c, uint64_t p, const int *offs, int n) {
    for (int i = 0; i < n; i++) {
        if (p < HOS_HEAP_BASE || !hos_guest_range(p + offs[i], 8)) return 0;
        p = recomp_load64(c, p + offs[i]);
    }
    return p >= HOS_HEAP_BASE ? p : 0;
}

static void vr_bomb_set_velocity(Ctx *c, uint64_t actor, const float v[3], int spin_stop) {
    static const int body_path[] = {624, 80, 32, 400};
    const uint64_t body = vr_bomb_chain(c, actor, body_path, 4);
    if (!body) return;
    float w[3];
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int k = 0; k < 3; k++) w[k] = n > BOMB_MAX_SPEED ? v[k] * BOMB_MAX_SPEED / n : v[k];
    vr_bomb_call(c, 0x8014dbe0ULL, body, 0, 0, 0, w, NULL);
    if (spin_stop) { const float zero[3] = {0, 0, 0}; vr_bomb_call(c, 0x8014b790ULL, body, 0, 0, 0, zero, NULL); }
}

/* its collision layer (as OneShotPhysicsChangeRigidBodyLayerEntity does: f_82922bb0 calls f_80147840(body, layer)) */
static void vr_bomb_layer(Ctx *c, VrBomb *b, int layer) {
    if (b->layer == layer) return;
    static const int body_path[] = {624, 80, 32, 400};
    const uint64_t body = vr_bomb_chain(c, b->actor, body_path, 4);
    A64Fn f = a64_lookup(0x80147840ULL);
    if (!body || !f) return;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x200) & ~0xfULL;
    c->x[0] = body, c->x[1] = (uint64_t)layer;
    f(c);
    *c = saved;
    static unsigned told;
    if (told++ < 12) hos_log("vr: bombs: collision layer %d (%s)", layer,
                             layer == LAYER_NO_HIT ? "held: no collision" : layer == LAYER_HIT_ONLY_GROUND ? "let go: the ground only" : "its own");
    b->layer = layer;
}

/* the bomb placed: its origin and rotation (3x3, as the actor keeps it at +0x328: row major) */
void hos_vr_held_keep(Ctx *c, uint64_t actor);   /* (vr/items/gear/gear_near_host.c: kept from the near test while held) */
static void vr_bomb_place(Ctx *c, uint64_t actor, const float origin[3], const float rot[9]) {
    static const int physics_path[] = {624, 80};
    const uint64_t physics = vr_bomb_chain(c, actor, physics_path, 2);
    if (physics) vr_bomb_call(c, 0x804e6630ULL, physics, 0, 1, 0, origin, rot);
    hos_vr_held_keep(c, actor);   /* (every actor the mod places on a VR hand: at the face it is closer than the near plane) */
}

/* ---- an unlit bomb's effects: its XLink asleep (the game's OneShotXLinkSleep / OneShotXLinkCancelSleep) ----------------
   The XLink component (actor +624 -> +104, f_80994040) keeps its sleep reasons at +200 and its two users at +112 (ELink:
   the effects) and +120 (SLink: the sounds). f_80994100(xlink, reasons) adds reasons and, the first time, puts both users to
   sleep (their effects ended, no new ones); f_80994060(xlink, reasons) takes them away and wakes them. */
static uint64_t vr_bomb_xlink(Ctx *c, uint64_t actor) {
    static const int xlink_path[] = {624, 104};
    const uint64_t x = vr_bomb_chain(c, actor, xlink_path, 2);
    return x && hos_guest_range(x + 200, 8) ? x : 0;
}
/* the animation's current command in slot 0 (f_837d5d00: the fuse is Bomb_Blink's), to see it change */
static uint64_t vr_bomb_command(Ctx *c, uint64_t actor) {
    static const int as_path[] = {624, 64, 24, 24};
    const uint64_t as = vr_bomb_chain(c, actor, as_path, 4);
    return as ? vr_bomb_call(c, 0x837d5d00ULL, as, 0, 0, 0, NULL, NULL) : 0;
}
static void vr_bomb_quiet(Ctx *c, VrBomb *b, int quiet) {
    if (quiet == b->asleep) return;
    const uint64_t x = vr_bomb_xlink(c, b->actor);
    A64Fn f = a64_lookup(quiet ? 0x80994100ULL : 0x80994060ULL);
    if (!x || !f) return;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x200) & ~0xfULL;
    c->x[0] = x, c->x[1] = BOMB_SLEEP_REASON;
    f(c);
    *c = saved;
    b->asleep = quiet;
    if (quiet) b->asleep_cmd = vr_bomb_command(c, b->actor);
    static unsigned told;
    if (told++ < 16) hos_log("vr: bombs: %s effects %s (XLink reasons now %#x)", b->flower ? "a Bomb Flower's" : "a bomb's",
                             quiet ? "asleep: unlit and quiet" : "awake", recomp_load32(c, x + 200));
}

/* its origin and rotation, and its centre (the origin plus the rotated centre offset) */
static void vr_bomb_pose(Ctx *c, uint64_t actor, float origin[3], float rot[9]) {
    for (int k = 0; k < 3; k++) origin[k] = vr_bomb_f(c, actor + 796 + 4 * k);
    for (int k = 0; k < 9; k++) rot[k] = vr_bomb_f(c, actor + 0x328 + 4 * k);
}
/* a bomb's centre above its origin and its radius (m): a bomb's, or a Bomb Flower fruit's own (asked of the renderer once) */
static float vr_bomb_flower_geometry(int radius) {
    static float shape[4];
    static int known;
    if (!known && rx_vr_flower_shape(shape) && shape[1] > 0.05f && shape[3] > 0.05f) {
        known = 1;
        hos_log("vr: bombs: a Bomb Flower's fruit: centre %.3f m above its origin, radius %.3f m (your game's model)", shape[1], shape[3]);
    }
    return known ? (radius ? shape[3] : shape[1]) : (radius ? BOMB_RADIUS_GAME : BOMB_CENTRE);
}
static float vr_bomb_up(const VrBomb *b) { return b && b->flower ? vr_bomb_flower_geometry(0) : BOMB_CENTRE; }
static float vr_bomb_radius(const VrBomb *b) { return b && b->flower ? vr_bomb_flower_geometry(1) : BOMB_RADIUS; }
static void vr_bomb_centre(Ctx *c, uint64_t actor, float out[3]) {
    float o[3], r[9];
    vr_bomb_pose(c, actor, o, r);
    const float up = vr_bomb_up(vr_bomb_find(actor));
    for (int k = 0; k < 3; k++) out[k] = o[k] + r[3 * k + 1] * up;
}

/* how a bomb sits in the hand: its origin and rotation in the hand's axes (rel_p[a] = axis a . (origin - hand), rel_r = the
   hand's axes transposed times its rotation). It keeps the rotation it was gripped at, but always sits at one spot: its
   centre BOMB_PALM + its radius out from the grip along the palm's direction (grip +x for the left hand, -x for the right),
   so it rests against the palm wherever the hand met it, even inside it, and the renderer's ball grip (arms_and_pose.inc)
   is always the same pose. It slides there over BOMB_SEAT_NS */
static void vr_bomb_grip_here(VrBomb *b, const VrBombHand *h, int hand, const float origin[3], const float rot[9]) {
    for (int a = 0; a < 3; a++) {
        b->rel_p[a] = 0;
        for (int k = 0; k < 3; k++) b->rel_p[a] += h->axis[a][k] * (origin[k] - h->p[k]);
        for (int j = 0; j < 3; j++) {
            b->rel_r[3 * a + j] = 0;
            for (int i = 0; i < 3; i++) b->rel_r[3 * a + j] += h->axis[a][i] * rot[3 * i + j];
        }
    }
    memcpy(b->seat_from, b->rel_p, sizeof b->seat_from);
    for (int a = 0; a < 3; a++)
        b->rel_p[a] = (a == 0 ? (hand ? -1.0f : 1.0f) * (BOMB_PALM + vr_bomb_radius(b)) : 0) - b->rel_r[3 * a + 1] * vr_bomb_up(b);
    b->seat_at = hos_now_ns();
    b->grip_set = 1;
}
/* a new bomb: upright, its side against the palm */
static void vr_bomb_grip_palm(VrBomb *b, const VrBombHand *h, int hand) {
    float palm[3], origin[3];
    const float up[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int k = 0; k < 3; k++) palm[k] = h->axis[0][k] * (hand ? -1.0f : 1.0f);
    for (int k = 0; k < 3; k++) origin[k] = h->p[k] + palm[k] * (BOMB_PALM + vr_bomb_radius(b)) - (k == 1 ? vr_bomb_up(b) : 0);
    vr_bomb_grip_here(b, h, hand, origin, up);
}

/* Link can lift a bomb flower (or any bomb the hands never had): the Goron Bracelet, or a gauntlet after it, owned.
   Bombs from the bag need neither. (Read from the inventory at most twice a second) */
static int vr_bomb_strong_seen;   /* (the last answer, atomic: for callers without the game's context or the lock) */
static int vr_bomb_strong(Ctx *c) {
    static int64_t at;
    int strong;
    const int64_t now = hos_now_ns();
    if (now - at > 500000000LL) {
        at = now;
        strong = oot_item_count(c, "Item_Gauntlet_Goron") > 0 || oot_item_count(c, "Armor_Child_Gauntlet_Goron") > 0 ||
                 oot_item_count(c, "Armor_Adult_Gauntlet_Silver") > 0 || oot_item_count(c, "Armor_Adult_Gauntlet_Gold") > 0;
        __atomic_store_n(&vr_bomb_strong_seen, strong, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&vr_bomb_strong_seen, __ATOMIC_RELAXED);
}

/* ---- the hands, once a frame from the player's own update -------------------------------------------------------------- */
static void vr_bomb_fake_hands(Ctx *c, uint64_t player, int64_t now, VrBombHand out[2]);


/* ---- the throw arc (VR tab Bomb aim: Arc to target; PARKED, its rows hidden: see the top of this file) -------------- */
static int vr_bomb_test_aim(void) {   /* (headless: RX_VR_BOMB_AIM=1 throws along the arc, as the VR tab's Arc to target) */
    static int t = -1;
    if (t < 0) { const char *e = vr_research_getenv("RX_VR_BOMB_AIM"); t = e ? atoi(e) : 0; }
    return vr_bomb_test() && t;
}
static int vr_bomb_aiming(void) { return vr_bomb_test_aim() || rx_option(951, 0) == 1; }
static float vr_bomb_gravity(void) { return BOMB_REAL_G * rx_option(907, 100) * 0.01f; }

typedef struct {
    float from[3], v[3];     /* where it leaves (the held bomb's centre) and its velocity */
    float land[3], t_land;   /* where it comes down, and when */
    int target;              /* onto Link's Z-target */
} VrBombArc;

/* The arc a throw from this hand would follow now: from its bomb's centre, along the hand's pointing line at the arc's
   speed (a 45-degree throw at it lands Bomb arc reach away, on level ground), down to the height of Link's feet; or, with
   a Z-target, onto it at the angle the hand points (or the nearest that reaches it at that speed). 0: no arc (the hand
   points too far down, or the target is out of reach). */
static int vr_bomb_arc(Ctx *c, const VrBomb *b, const VrBombHand *h, VrBombArc *out) {
    memset(out, 0, sizeof *out);
    if (!h->aims || h->aim[1] < BOMB_AIM_DOWN) return 0;
    const float g = vr_bomb_gravity(), reach = rx_option(952, 80) * 0.1f;
    const float speed = fminf(BOMB_MAX_SPEED, sqrtf(g * reach));
    memcpy(out->from, b->centre, sizeof out->from);
    const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    float lock[3];
    if (player && oot_vr_lock_point(c, player, lock) == 1) {
        const float pitch = asinf(fmaxf(-1, fminf(1, h->aim[1])));
        if (!bomb_arc_onto(out->from, lock, pitch, g, speed * 1.05f, out->v)) return 0;
        const float dx = lock[0] - out->from[0], dz = lock[2] - out->from[2];
        const float level = sqrtf(out->v[0] * out->v[0] + out->v[2] * out->v[2]);
        out->t_land = level > 0.05f ? sqrtf(dx * dx + dz * dz) / level : bomb_arc_land_time(out->from, out->v, g, lock[1]);
        memcpy(out->land, lock, sizeof out->land);
        out->target = 1;
    } else {
        for (int k = 0; k < 3; k++) out->v[k] = h->aim[k] * speed;
        const float ground = player ? vr_bomb_f(c, player + 800) : out->from[1] - 1;
        out->t_land = bomb_arc_land_time(out->from, out->v, g, ground);
        if (out->t_land > 0) bomb_arc_at(out->from, out->v, g, out->t_land, out->land);
    }
    return out->t_land > 0.05f;
}

/* ---- the gravity pull (VR tab Bomb pull) ------------------------------------------------------------------------------- */
static int vr_bomb_flowers_on(void) { return vr_bomb_test() >= 3 || rx_option(956, 1); }
/* (a bomb these hands may take: their own, or a bomb flower or any other the hands never had, with the Goron's Bracelet) */
static int vr_bomb_takeable(Ctx *c, const VrBomb *b) {
    if (b->flower && !vr_bomb_flowers_on()) return 0;
    return b->owned || vr_bomb_strong(c);
}
/* how far a bomb is from Link (m, level) */
static float vr_bomb_from_link(const VrBomb *b) {
    const float dx = b->centre[0] - vr_bomb_link_at[0], dz = b->centre[2] - vr_bomb_link_at[2];
    return sqrtf(dx * dx + dz * dz);
}
static VrBomb *vr_bomb_ringed[2];   /* the bomb each open hand points at (its ring shows; a squeeze pulls it), or none */
/* the bomb this open hand points at, for the pull: the best aimed of those out of reach, within the pull's reach and aim
   (the one already ringed is kept a little longer: BOMB_PULL_KEEP) */
/* The pull's pointing line: out of the palm of the hand as drawn (the grip's calibrated axes: +x is the way the left palm
   faces, -x the right's), from its surface. Not the controller's aim pose, which points another way than the drawn hand
   (player report, 19 Nov: "can't tell whether it's relative to the hand or the headset"). Headless tests aim their own. */
static void vr_bomb_palm_ray(const VrBombHand *h, int hand, float from[3], float dir[3]) {
    for (int k = 0; k < 3; k++) {
        dir[k] = vr_bomb_test() ? h->aim[k] : h->axis[0][k] * (hand ? -1.0f : 1.0f);
        from[k] = h->p[k] + dir[k] * BOMB_PALM;
    }
}
static VrBomb *vr_bomb_pull_pick(Ctx *c, const VrBombHand *h, int hand, const VrBomb *was) {
    if (!h->valid || !(vr_bomb_test() >= 2 || rx_option(953, 1))) return NULL;
    const float reach = rx_option(954, 40) * 0.1f, cone = rx_option(955, 20) * 0.0174533f;
    float from[3], dir[3];
    vr_bomb_palm_ray(h, hand, from, dir);
    VrBomb *best = NULL;
    float best_off = 0;
    for (int i = 0; i < BOMB_MAX; i++) {
        VrBomb *b = &vr_bombs[i];
        if (!b->actor || b->holder >= 0 || b->pull_hand >= 0 || (vr_bomb_pull != PULL_IDLE && b->actor == vr_bomb_pulled)) continue;
        if (!vr_bomb_takeable(c, b)) continue;
        if (b->on_plant && vr_bomb_from_link(b) > BOMB_FLOWER_PICK) continue;   /* (on its plant: the game picks it, Link beside it) */
        float off = bomb_pull_aim(from, dir, b->centre, vr_bomb_radius(b), BOMB_REACH + vr_bomb_radius(b), reach,
                                  b == was ? cone * BOMB_PULL_KEEP : cone);
        if (off < 0) continue;
        if (b == was) off *= 0.8f;   /* (the ringed one wins a near tie: the ring does not flicker between two) */
        if (!best || off < best_off) best = b, best_off = off;
    }
    return best;
}

/* ---- the marks in the headset: the arc's dots and its target, parked (vr/items/bomb/bomb_marks.inc draws them) -------- */
static void vr_bomb_mark(float *m, int *n, int kind, const float at[3], float size) {
    if (*n >= BOMB_MARKS_MAX) return;
    float *f = m + 5 * (*n)++;
    f[0] = (float)kind, f[1] = at[0], f[2] = at[1], f[3] = at[2], f[4] = size;
}
/* Every game frame, to the renderer: what each hand holds and whether Link's lift has it (the renderer then draws it in
   the palm wherever the lift has it: no detour over his head), and the bomb the pull would take (drawn pulsing, the bomb
   itself: vr/items/gear/equipment_skin.inc); and the arc of the bomb in hand last taken (Bomb aim Arc to target: parked,
   its row hidden) */
static void vr_bomb_send_marks(Ctx *c) {
    float state[8] = {0};
    const uint64_t po = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    const int carrying = po && (recomp_load32(c, po + 0x1f60) & 0x06000000u) == 0x06000000u;
    for (int i = 0; i < BOMB_MAX; i++) {
        const VrBomb *b = &vr_bombs[i];
        if (!b->actor || b->holder < 0 || !b->grip_set) continue;
        state[b->holder] = b->flower ? 2 : 1;   /* (kind: 1 a bomb, 2 a Bomb Flower) */
        state[2 + b->holder] = carrying || (b->actor == vr_bomb_pulled && (vr_bomb_pull == PULL_TAKE || vr_bomb_pull == PULL_THROW));
    }
    const VrBomb *aimed = vr_bomb_ringed[1] ? vr_bomb_ringed[1] : vr_bomb_ringed[0];
    if (aimed) {
        state[4] = aimed->flower ? 2 : 1;
        memcpy(state + 5, aimed->centre, 3 * sizeof *state);
    }
    rx_vr_bomb_state(state);
    float m[BOMB_MARKS_MAX * 5];
    int n = 0;
    const VrBomb *held = NULL;
    for (int i = 0; i < BOMB_MAX; i++) {
        const VrBomb *b = &vr_bombs[i];
        if (b->actor && b->holder >= 0 && b->grip_set && !b->on_plant && (!held || b->seat_at > held->seat_at)) held = b;   /* (not while the game picks it) */
    }
    VrBombArc arc;
    if (vr_bomb_aiming() && held && vr_bomb_pull == PULL_IDLE && vr_bomb_hand[held->holder].valid &&
        vr_bomb_arc(c, held, &vr_bomb_hand[held->holder], &arc)) {
        const float g = vr_bomb_gravity();
        for (int i = 1; i <= BOMB_ARC_DOTS; i++) {   /* (evenly in time, the first a little out of the hand) */
            float at[3];
            bomb_arc_at(arc.from, arc.v, g, arc.t_land * i / (BOMB_ARC_DOTS + 1), at);
            const float dx = at[0] - arc.from[0], dy = at[1] - arc.from[1], dz = at[2] - arc.from[2];
            vr_bomb_mark(m, &n, 1, at, 0.035f + 0.006f * sqrtf(dx * dx + dy * dy + dz * dz));   /* (further: a little larger) */
        }
        const float dx = arc.land[0] - arc.from[0], dz = arc.land[2] - arc.from[2];
        vr_bomb_mark(m, &n, 2, arc.land, (arc.target ? 0.5f : 0.7f) + 0.03f * sqrtf(dx * dx + dz * dz));
    }
    rx_vr_bomb_marks(m, n);
    static int64_t said;
    const int64_t now = hos_now_ns();
    if (vr_bomb_test() && n && now - said > 500000000LL) {
        said = now;
        hos_log("vr: bombs: marks: %d (the throw arc)", n);
    }
}

/* Link's carry ended by the game's own call, not a button: OneShotLifterTryRequestFall's execute (f_82908700) is
   f_82e70db0(f_80993230(actor), 1, 1.0, 0.0), f_80993230 reading the actor's Lifter (+0x270 -> +0x2c8). It does
   nothing unless the Lifter holds something (its state +0x2b0 is 2 or 3), and answers whether it let go. (ZR in a carry is
   Link's throw, A his put-down or a talk beside someone: the input calls audit, its #7-#10.) */
static int vr_bomb_lifter_fall(Ctx *c, uint64_t player) {
    static const int path[] = {624, 712};
    const uint64_t lifter = vr_bomb_chain(c, player, path, 2);
    A64Fn f = lifter && hos_guest_range(lifter + 0x2b0, 4) ? a64_lookup(0x82e70db0ULL) : NULL;
    if (!f) {
        hos_log("vr: bombs: no lifter on Link to end his carry");
        return 0;
    }
    const uint32_t state = recomp_load32(c, lifter + 0x2b0);
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x200) & ~0xfULL;
    c->x[0] = lifter, c->x[1] = 1;
    const float one = 1, zero = 0;
    uint32_t w;
    memcpy(&w, &one, 4); a64_vset64(c->vreg[0], w);
    memcpy(&w, &zero, 4); a64_vset64(c->vreg[1], w);
    f(c);
    const int fell = (c->x[0] & 0xff) != 0;
    *c = saved;
    hos_log("vr: bombs: the game's lifter call (state %u): %s", state, fell ? "Link lets go" : "nothing to let go");
    return fell;
}

/* A hand takes a fruit still on its plant: the game picks it (its own pick, the A button: Link must be beside it and the
   game's own reach and facing rules apply), and once Link's lift has it (his carry stance) the hand holds it, as a bomb
   from the bag (the lifter call that ends his carry: vr_bomb_player). Until then it stays where it is. (The pick stays on
   A: the beside check that keeps the put-away's B quiet, hos_vr_link_beside, counts the fruit itself, and no pick call
   is found yet.) */
static void vr_bomb_ask_pick(VrBomb *b, const VrBombHand *hand, int h, int64_t now) {
    vr_bomb_grip_palm(b, hand, h);
    b->holder = h, b->owned = 1, b->release = 0, b->pull_hand = -1, b->picking_at = now;
    vr_bomb_a_until = now + 150000000LL;
    hos_log("vr: bombs: the %s hand asks the game to pick a Bomb Flower (its A)", h ? "right" : "left");
}

/* (the input side's copy, made at the end of Link's update: vr_bomb_publish) whether this hand's grip is the bombs': a bomb
   held or being pulled by it, pointed at for its pull, or within its reach (one the hands may take) */
static int vr_bomb_claims(int hand) {
    if (!vr_bomb_enabled() || !vr_bomb_hand[hand].valid) return 0;
    if (vr_bomb_ringed[hand]) return 1;
    const float *p = vr_bomb_hand[hand].p;
    for (int i = 0; i < BOMB_MAX; i++) {
        const VrBomb *b = &vr_bombs[i];
        if (!b->actor) continue;
        if (b->holder == hand || b->pull_hand == hand) return 1;
        if (b->flower && !vr_bomb_flowers_on()) continue;
        if (!b->owned && !vr_bomb_strong_seen) continue;   /* (a bomb flower without the Goron Bracelet: not the hands') */
        const float dx = b->centre[0] - p[0], dy = b->centre[1] - p[1], dz = b->centre[2] - p[2];
        if (sqrtf(dx * dx + dy * dy + dz * dz) - vr_bomb_radius(b) < BOMB_REACH) return 1;
    }
    return 0;
}
static void vr_bomb_publish_input(void) {
    VrBombInput in;
    memset(&in, 0, sizeof in);
    in.owned = vr_bomb_owned;
    for (int i = 0; i < BOMB_MAX; i++) {
        const int h = vr_bombs[i].holder;   /* (read once, used only as 0 or 1) */
        if (vr_bombs[i].actor && (h == 0 || h == 1)) in.holds[h] = 1;
    }
    for (int h = 0; h < 2; h++) in.claims[h] = vr_bomb_claims(h);
    in.zr_until = vr_bomb_zr_until, in.a_until = vr_bomb_a_until;
    in.at = hos_now_ns();
    vr_bomb_publish(&in);
}

static void vr_bomb_player(Ctx *c, uint64_t player) {
    const int64_t now = hos_now_ns();
    static int64_t noted;
    if (vr_bomb_test() && now - noted > 300000000LL) noted = now, oot_vr_note_selected_item(c);   /* (VR's view does it otherwise) */
    vr_bomb_strong(c);   /* (kept current for the bow's reach check) */
    const int owned = vr_bomb_enabled() && hos_vr_selected_is_bomb;
    if (owned != vr_bomb_owned) hos_log("vr: bombs: %s", owned ? "bombs selected: a grip takes a bomb" : "bombs not selected");
    vr_bomb_owned = owned;

    VrBombHand hands[2];
    memset(hands, 0, sizeof hands);
    if (vr_bomb_test()) vr_bomb_fake_hands(c, player, now, hands);
    else {
        float w[42];
        if (rx_vr_hands_world(w))
            for (int h = 0; h < 2; h++) {
                const float *o = w + 21 * h;
                hands[h].valid = o[0] > .5f;
                for (int k = 0; k < 3; k++) hands[h].p[k] = o[1 + k], hands[h].v[k] = o[4 + k], hands[h].w[k] = o[18 + k];
                for (int k = 0; k < 9; k++) hands[h].axis[k / 3][k % 3] = o[9 + k];
                hands[h].trigger = o[7], hands[h].grip = o[8];
                float ray[6];   /* (where it points: the throw arc and the pull) */
                if (hands[h].valid && rx_vr_hand_ray(h, ray))
                    hands[h].aims = 1, memcpy(hands[h].aim_from, ray, sizeof hands[h].aim_from), memcpy(hands[h].aim, ray + 3, sizeof hands[h].aim);
            }
    }
    memcpy(vr_bomb_hand, hands, sizeof hands);
    for (int k = 0; k < 3; k++) {
        vr_bomb_link_at[k] = vr_bomb_f(c, player + 796 + 4 * k);
        uint32_t w;
        memcpy(&w, &vr_bomb_link_at[k], 4);
        __atomic_store_n(&vr_bomb_link_bits[k], w, __ATOMIC_RELAXED);
    }
    {   /* Link's own velocity: his root's change between his updates, smoothed (about 0.1 s) */
        static float last[3]; static int64_t last_at;
        float root[3];
        for (int k = 0; k < 3; k++) root[k] = vr_bomb_f(c, player + 796 + 4 * k);
        const float dt = last_at ? (now - last_at) * 1e-9f : 0;
        if (dt > 0.001f && dt < 0.2f)
            for (int k = 0; k < 3; k++) {
                const float v = (root[k] - last[k]) / dt;
                vr_bomb_link_v[k] += (fabsf(v) < 30 ? v - vr_bomb_link_v[k] : 0) * fminf(1, dt * 12);
            }
        else memset(vr_bomb_link_v, 0, sizeof vr_bomb_link_v);
        memcpy(last, root, sizeof last), last_at = now;
    }
    static int grip_was[2], trigger_was[2];
    for (int h = 0; h < 2; h++) {
        VrBombHand *hand = &hands[h];
        const int grip = hand->valid && (grip_was[h] ? hand->grip > .35f : hand->grip > .55f);
        const int trigger = hand->valid && (trigger_was[h] ? hand->trigger > .4f : hand->trigger > .6f);
        VrBomb *held = NULL, *pulling = NULL;
        for (int i = 0; i < BOMB_MAX; i++) if (vr_bombs[i].actor && vr_bombs[i].holder == h) held = &vr_bombs[i];
        for (int i = 0; i < BOMB_MAX; i++) if (vr_bombs[i].actor && vr_bombs[i].pull_hand == h) pulling = &vr_bombs[i];
        if (pulling && !grip) {   /* (the pull let go of on the way: it flies on as it was going, the game's physics) */
            pulling->pull_hand = -1, pulling->released_at = now;
            hos_log("vr: bombs: the %s hand let go of its pull on the way", h ? "right" : "left");
            pulling = NULL;
        }
        int grabbed_now = 0;
        if (grip && !held && !pulling && (rx_option(908, 0) != 1 || !grip_was[h])) {   /* a bomb within reach (loose, or in the other hand on a new grip): held as gripped */
            VrBomb *best = NULL; float best_d = BOMB_REACH;
            for (int i = 0; i < BOMB_MAX; i++) {
                VrBomb *b = &vr_bombs[i];
                if (!b->actor || (b->holder >= 0 && grip_was[h]) || (vr_bomb_pull != PULL_IDLE && b->actor == vr_bomb_pulled)) continue;
                if (b->pull_hand >= 0 && b->pull_hand != h) continue;   /* (the other hand is pulling it) */
                if (b->on_plant && grip_was[h]) continue;   /* (the game's pick: asked once a squeeze, never again while held) */
                if (!vr_bomb_takeable(c, b)) continue;   /* (a bomb flower is lifted only with the Goron Bracelet) */
                float centre[3]; vr_bomb_centre(c, b->actor, centre);
                const float d = sqrtf((centre[0] - hand->p[0]) * (centre[0] - hand->p[0]) + (centre[1] - hand->p[1]) * (centre[1] - hand->p[1]) +
                                      (centre[2] - hand->p[2]) * (centre[2] - hand->p[2])) - vr_bomb_radius(b);
                if (d < best_d) best = b, best_d = d;
            }
            if (best && best->on_plant) {
                vr_bomb_ask_pick(best, hand, h, now);
                held = best, grabbed_now = 1;
            } else if (best) {
                float origin[3], rot[9];
                vr_bomb_pose(c, best->actor, origin, rot);
                hos_log("vr: bombs: the %s hand %s a bomb (%.2f m from its surface)", h ? "right" : "left",
                        best->holder >= 0 ? "takes" : "picks up", best_d > 0 ? best_d : 0);
                vr_bomb_grip_here(best, hand, h, origin, rot);
                best->owned = 1, best->pull_hand = -1;
                best->holder = h, best->release = 0, held = best, grabbed_now = 1;
            } else if (!grip_was[h] && vr_bomb_ringed[h] && vr_bomb_ringed[h]->on_plant) {   /* (a fruit on its plant: the game picks it) */
                vr_bomb_ask_pick(vr_bomb_ringed[h], hand, h, now);
                held = vr_bomb_ringed[h], grabbed_now = 1;
            } else if (!grip_was[h] && vr_bomb_ringed[h] && !hos_vr_ocarina_claims(h)) {   /* the bomb this hand points at: pulled
                                                                                         into it (the actor update flies it) */
                VrBomb *b = vr_bomb_ringed[h];
                b->pull_hand = h, b->pull_at = b->pull_last = now, b->flight = 0, b->released_at = 0;
                grabbed_now = 1;
                const float d[3] = {b->centre[0] - hand->p[0], b->centre[1] - hand->p[1], b->centre[2] - hand->p[2]};
                hos_log("vr: bombs: the %s hand pulls a %s %.2f m away", h ? "right" : "left", b->flower ? "Bomb Flower" : "bomb",
                        sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
            } else if (!grip_was[h] && owned && vr_bomb_pull == PULL_IDLE && !hos_vr_ocarina_claims(h) && !hos_vr_sword_claims(h)) {
                /* Bombs selected, a grip anywhere with nothing within reach takes a new one: the mod taps the game's ZR,
                   Link takes one out, and the hand has it from the moment it appears. (At the hips only from 19 Nov, when a
                   missed catch took a new bomb out; grip anywhere again 22 Nov, the hub's call, with the reach widened to
                   BOMB_REACH so a catch finds the bomb first.) */
                grabbed_now = 1;
                vr_bomb_pull = PULL_TAKE, vr_bomb_pull_hand = h, vr_bomb_pull_at = now, vr_bomb_zr_until = now + 150000000LL;
                hos_log("vr: bombs: the %s hand takes a bomb out (the game's ZR)", h ? "right" : "left");
            }
        }
        /* the throw: letting go of the grip (VR tab Bomb throw: Let go), or squeezing it again (Squeeze again; the bomb stays
           in the hand without holding the grip) */
        const int squeeze_mode = rx_option(908, 0) == 1 && !vr_bomb_test();
        const int throw_now = held && (squeeze_mode ? grip && !grip_was[h] && !grabbed_now : !grip);
        if (throw_now && held->on_plant) {   /* (let go before the game picked it: it stays on its plant, the game's) */
            held->holder = -1, held->owned = 0, held->grip_set = 0, held->picking_at = 0;
            hos_log("vr: bombs: the %s hand let go before the game picked the Bomb Flower", h ? "right" : "left");
            held = NULL;
        } else if (throw_now) {   /* dropped or thrown, the bomb's own velocity */
            float avg[3] = {0, 0, 0};
            const int n = vr_bomb_vel_n[h] < 3 ? vr_bomb_vel_n[h] : 3;
            for (int s = 0; s < n; s++) for (int k = 0; k < 3; k++) avg[k] += vr_bomb_vel_hist[h][s][k] / n;
            const float scale = vr_bomb_test() ? 1.1f : rx_option(905, 150) * 0.01f;   /* (the VR tab's Bomb throw strength) */
            for (int k = 0; k < 3; k++) held->release_v[k] = avg[k] * scale + vr_bomb_link_v[k];
            VrBombArc arc;   /* (Arc to target: a throw, not a drop, leaves along the arc shown) */
            const float thrown = sqrtf(avg[0] * avg[0] + avg[1] * avg[1] + avg[2] * avg[2]) * scale;
            if (vr_bomb_aiming() && thrown > BOMB_AIM_THROW && vr_bomb_arc(c, held, hand, &arc)) {
                memcpy(held->release_v, arc.v, sizeof held->release_v);
                hos_log("vr: bombs: thrown along the arc%s: lands at %.2f %.2f %.2f in %.2f s", arc.target ? " onto the Z-target" : "",
                        arc.land[0], arc.land[1], arc.land[2], arc.t_land);
            }
            held->release = 0, held->holder = -1, held->released_at = now;
            held->flight = 1, held->flight_at = held->flight_since = 0;   /* (its arc starts at its next update) */
            memcpy(held->flight_v, held->release_v, sizeof held->flight_v);
            hos_log("vr: bombs: the %s hand lets go (%.2f %.2f %.2f m/s)", h ? "right" : "left",
                    held->release_v[0], held->release_v[1], held->release_v[2]);
            held = NULL;
        }
        if (held && trigger && !trigger_was[h] && !held->lit) {
            held->lit = 1;
            hos_log("vr: bombs: lit by the %s trigger: the fuse runs", h ? "right" : "left");
        }
        /* the bomb this hand would pull: only an open, empty hand points (a squeeze pulls it, above) */
        vr_bomb_ringed[h] = !held && !pulling && !grip ? vr_bomb_pull_pick(c, hand, h, vr_bomb_ringed[h]) : NULL;
        grip_was[h] = grip, trigger_was[h] = trigger;
        /* (the held bomb's own velocity, averaged over the last 3 samples, this one counted from the next frame: a release
           uses the motion that led to it, not the frame it ended on. A bomb is held away from the grip, so a flick of the
           wrist moves it faster than the hand: the hand's velocity plus its angular velocity across that distance) */
        if (hand->valid) {
            float v[3];
            memcpy(v, hand->v, sizeof v);
            if (held) {
                float r[3];
                for (int k = 0; k < 3; k++) r[k] = held->centre[k] - hand->p[k];
                v[0] += hand->w[1] * r[2] - hand->w[2] * r[1];
                v[1] += hand->w[2] * r[0] - hand->w[0] * r[2];
                v[2] += hand->w[0] * r[1] - hand->w[1] * r[0];
            }
            memcpy(vr_bomb_vel_hist[h][vr_bomb_vel_n[h]++ % 3], v, sizeof v);
        } else vr_bomb_vel_n[h] = 0;
    }

    /* the pull-out: the new bomb is the hand's from when it appears (Link's lift lets go of it at the throw tap) */
    switch (vr_bomb_pull) {
    case PULL_TAKE: {
        VrBomb *fresh = NULL;
        for (int i = 0; i < BOMB_MAX; i++)   /* (a bomb, not a Bomb Flower just found nearby) */
            if (vr_bombs[i].actor && !vr_bombs[i].flower && vr_bombs[i].born > vr_bomb_pull_at) fresh = &vr_bombs[i];
        if (fresh) {
            const int h = vr_bomb_pull_hand;
            vr_bomb_pulled = fresh->actor, vr_bomb_pull = PULL_THROW, vr_bomb_pull_at = now;
            vr_bomb_grip_palm(fresh, &vr_bomb_hand[h], h);
            fresh->holder = h, fresh->release = 0, fresh->owned = 1;
        } else if (now - vr_bomb_pull_at > 1200000000LL) {
            hos_log("vr: bombs: no bomb came out (none left, or Link cannot take one out here)");
            vr_bomb_pull = PULL_IDLE;
        }
        break;
    }
    case PULL_THROW:   /* (Link has it: the game's lifter call ends his hold on it) */
        if (now - vr_bomb_pull_at > 600000000LL) {   /* (after Link's lift; if he still carries it, the carry's end below calls again) */
            vr_bomb_pull = PULL_CATCH, vr_bomb_pull_at = now;
            vr_bomb_lifter_fall(c, player);
            /* The hand places it from now on (hos_vr_bomb_actor_update). Until now the renderer drew it in the palm turned as Link's
               lift has it; held from then on as it is now, so it does not turn at the handover. (Held upright as at the grip,
               taken at the hip, it turned over as the hand came up: "correct for a split second, then upside down", user
               report 17 Nov.) Already in its seat (no slide). */
            VrBomb *b = vr_bomb_find(vr_bomb_pulled);
            if (b && b->holder >= 0 && vr_bomb_hand[b->holder].valid) {
                float origin[3], rot[9];
                vr_bomb_pose(c, b->actor, origin, rot);
                vr_bomb_grip_here(b, &vr_bomb_hand[b->holder], b->holder, origin, rot);
                memcpy(b->seat_from, b->rel_p, sizeof b->seat_from);
            }
        }
        break;
    case PULL_CATCH:
        if (now - vr_bomb_pull_at > 200000000LL) {
            VrBomb *b = vr_bomb_find(vr_bomb_pulled);
            if (b && b->holder < 0) hos_log("vr: bombs: the new bomb was let go before Link's throw ended");
            else if (b) hos_log("vr: bombs: the new bomb is in the hand");
            vr_bomb_pull = PULL_IDLE;
        }
        break;
    }
    /* Link lifting a bomb the hand holds (his carry stance, action bits 0x06000000: 0x1f060000, 0x17060000 seen): the game's
       own pick-up of a liftable near him. The lifter call makes him let go, as at the pull-out; the hand keeps it */
    if (vr_bomb_pull == PULL_IDLE) {
        static int64_t tapped;
        static int taps;
        const uint64_t po = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
        const uint32_t action = po ? recomp_load32(c, po + 0x1f60) : 0;
        const int carrying = (action & 0x06000000u) == 0x06000000u;
        int holding = 0, flower = 0;
        for (int i = 0; i < BOMB_MAX; i++) {
            VrBomb *b = &vr_bombs[i];
            if (!b->actor || b->holder < 0) continue;
            holding = 1, flower |= b->flower;
            if (b->on_plant && carrying) {   /* (the game picked it: Link holds it up; free of its plant, its fuse the game's) */
                /* (the game picks the fruit in front of Link, not always the one the hand asked for: the one his lift
                   raised off its plant, over his head, is the hand's) */
                VrBomb *up = b;
                float best = 0;
                for (int j = 0; j < BOMB_MAX; j++) {
                    VrBomb *o = &vr_bombs[j];
                    if (!o->actor || !o->flower || o->holder >= 0 || o == b) continue;
                    const float dx = o->centre[0] - vr_bomb_link_at[0], dz = o->centre[2] - vr_bomb_link_at[2];
                    const float high = o->centre[1] - vr_bomb_link_at[1];
                    if (dx * dx + dz * dz < 0.8f * 0.8f && high > 0.7f && high > best) best = high, up = o;
                }
                if (up != b) {
                    up->holder = b->holder, up->owned = 1, up->release = 0, up->pull_hand = -1, up->grip_set = 1;
                    memcpy(up->rel_p, b->rel_p, sizeof up->rel_p), memcpy(up->rel_r, b->rel_r, sizeof up->rel_r);
                    memcpy(up->seat_from, b->rel_p, sizeof up->seat_from), up->seat_at = b->seat_at;
                    b->holder = -1, b->owned = 0, b->grip_set = 0, b->picking_at = 0;
                    hos_log("vr: bombs: the game picked another Bomb Flower than the hand asked for: the hand has that one");
                    b = up;
                }
                b->on_plant = 0, b->picking_at = 0, b->settle_until = now + BOMB_SETTLE_NS;
                hos_log("vr: bombs: the game picked the Bomb Flower (action %#x)", action);
            } else if (b->on_plant && b->picking_at && now - b->picking_at > BOMB_FLOWER_PICK_NS) {
                b->holder = -1, b->owned = 0, b->grip_set = 0, b->picking_at = 0;
                hos_log("vr: bombs: the game did not pick the Bomb Flower (Link not beside it, or not facing it)");
            }
        }
        if (!carrying) taps = 0;
        /* (a bomb's carry ends only once Link's lift is done: a call every 0.6 s; a fruit's carry, picked by the game, is
           ended as soon as the game takes it, a call every 0.2 s: it shows over his head meanwhile) */
        if (holding && carrying && now - tapped > (flower ? 200000000LL : 600000000LL) && taps < 10) {
            tapped = now, taps++;
            hos_log("vr: bombs: Link lifted a %s the hand holds (action %#x)", flower ? "Bomb Flower" : "bomb", action);
            vr_bomb_lifter_fall(c, player);
        }
    }
    /* the hands' grips (the renderer's Grip::ball): which hand holds a bomb, its drawn radius, and where the game has it
       (the renderer draws that bomb in the palm even while Link's pull-out lift has it over his head) */
    for (int h = 0; h < 2; h++) {
        const VrBomb *holds = NULL;
        for (int i = 0; i < BOMB_MAX; i++) if (vr_bombs[i].actor && vr_bombs[i].holder == h && vr_bombs[i].grip_set) holds = &vr_bombs[i];
        rx_vr_hand_holds(h, holds ? vr_bomb_radius(holds) : 0, holds ? holds->centre : NULL);   /* (the fingers' ball) */
    }
    vr_bomb_send_marks(c);
    /* bombs no longer updated: exploded or removed */
    for (int i = 0; i < BOMB_MAX; i++)
        if (vr_bombs[i].actor && now - vr_bombs[i].seen > 400000000LL) {
            hos_log("vr: bombs: a %s is gone (%s, %.1f s after it appeared)", vr_bombs[i].flower ? "Bomb Flower" : "bomb",
                    vr_bombs[i].lit ? "lit" : "unlit", (vr_bombs[i].seen - vr_bombs[i].born) * 1e-9);
            for (int h = 0; h < 2; h++) if (vr_bomb_ringed[h] == &vr_bombs[i]) vr_bomb_ringed[h] = NULL;
            __atomic_store_n(&vr_bombs[i].actor, 0, __ATOMIC_RELEASE);
        }
    vr_bomb_publish_input();   /* (the pad read's copy of this update: vr_bomb_input) */
    {   /* (whether two updates ran at the same time: entries that found vr_bomb_lock held, every 10 s when any did, and a
           one-line answer after the first minute either way) */
        static int64_t said, started;
        static int answered;
        if (!started) started = now;
        if (now - said > 10000000000LL) {
            said = now;
            const uint32_t n = __atomic_exchange_n(&vr_bomb_lock_waits, 0, __ATOMIC_RELAXED);
            vr_bomb_lock_waits_total += n;
            if (n) hos_log("vr: bombs: %u updates waited for another's in 10 s (Link's and a bomb's ran at the same time)", n);
            if (!answered && now - started > 60000000000LL) {
                answered = 1;
                hos_log("vr: bombs: in the first minute %u updates waited for another's%s", vr_bomb_lock_waits_total,
                        vr_bomb_lock_waits_total ? "" : " (none ran at the same time)");
            }
        }
    }
}

/* ---- each actor's movement update (host/hos_main.c hos_oot_lift_player, VR HOOK) ------------------------------------------- */
/* Bomb Flowers near Link, found by name (a fruit waiting on its plant runs no bomb AI for hos_vr_bomb_ai to see): an actor
   within reach of him is looked at once; one that is not a flower is remembered for a few seconds (a small table by its
   address) so the names are not read every frame. Without vr_bomb_lock, from atomic words only (Link's place, the
   bracelet, the table), so the actors that are not flowers never take it: 1 a fruit not tracked yet */
static int vr_bomb_flower_near(Ctx *c, uint64_t actor) {
    if (!vr_bomb_flowers_on() || !__atomic_load_n(&vr_bomb_strong_seen, __ATOMIC_RELAXED) || !actor) return 0;
    float p[3], link[3];
    for (int k = 0; k < 3; k++) {
        p[k] = vr_bomb_f(c, actor + 796 + 4 * k);
        const uint32_t w = __atomic_load_n(&vr_bomb_link_bits[k], __ATOMIC_RELAXED);
        memcpy(&link[k], &w, 4);
    }
    const float look = vr_bomb_test() == 3 ? 40 : fmaxf(rx_option(954, 40) * 0.1f, 1.5f) + BOMB_FLOWER_LOOK;   /* (the flower test: any in sight) */
    const float dx = p[0] - link[0], dy = p[1] - link[1], dz = p[2] - link[2];
    if (!(dx * dx + dy * dy + dz * dz < look * look)) return 0;
    static struct { uint64_t actor; int64_t at; } not_flower[128];   /* (each word atomic: several threads look) */
    const int64_t now = hos_now_ns();
    const unsigned slot = (unsigned)((actor >> 4) * 2654435761u) % 128;
    if (__atomic_load_n(&not_flower[slot].actor, __ATOMIC_RELAXED) == actor &&
        now - __atomic_load_n(&not_flower[slot].at, __ATOMIC_RELAXED) < 5000000000LL) return 0;
    if (!vr_bomb_is_flower(c, actor)) {
        __atomic_store_n(&not_flower[slot].at, now, __ATOMIC_RELAXED);
        __atomic_store_n(&not_flower[slot].actor, actor, __ATOMIC_RELAXED);
        return 0;
    }
    return 1;
}

static void vr_bomb_actor_locked(Ctx *c, uint64_t actor);
void hos_vr_bomb_actor_update(Ctx *c, uint64_t actor) {
    if (!vr_bomb_enabled() || !actor) return;
    const int link = actor == __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    if (!link && !vr_bomb_find(actor) && !vr_bomb_flower_near(c, actor)) return;   /* (nearly every actor: no lock) */
    vr_bomb_enter();
    if (link) vr_bomb_player(c, actor);
    else vr_bomb_actor_locked(c, actor);
    vr_bomb_leave();
}
/* (a tracked bomb's update, or a Bomb Flower's first: under vr_bomb_lock) */
static void vr_bomb_actor_locked(Ctx *c, uint64_t actor) {
    VrBomb *b = vr_bomb_find(actor);
    if (!b) b = vr_bomb_track(c, actor, 2);   /* (a fruit just found near Link: vr_bomb_flower_near) */
    if (!b) return;
    const int64_t now = hos_now_ns();
    b->seen = now;
    {   /* unlit and the mod's: its effects asleep (VR tab Unlit bomb fire: None); lit, or exploding (its animation left the
           fuse's: set off unlit), awake */
        const int quiet = b->owned && !b->lit && (vr_bomb_test() || rx_option(950, 1));
        if (b->asleep && (now < b->settle_until || b->on_plant || (b->actor == vr_bomb_pulled && vr_bomb_pull != PULL_IDLE)))
            b->asleep_cmd = vr_bomb_command(c, actor);   /* (a pick or a pull-out changes it: not set off) */
        else if (b->asleep && quiet && vr_bomb_command(c, actor) != b->asleep_cmd) {
            b->lit = 1;   /* (set off: its fuse is no longer held either) */
            hos_log("vr: bombs: an unlit %s's animation changed (set off?): its effects wake", b->flower ? "Bomb Flower" : "bomb");
        }
        vr_bomb_quiet(c, b, quiet && !b->lit);
    }
    if (!b->lit && b->owned) {   /* unlit: the fuse (Bomb_Blink, slot 0) held at one frame */
        static const int as_path[] = {624, 64, 24, 24};
        const float hold_frame = (float)rx_option(906, (int)BOMB_UNLIT_FRAME);   /* (the Debug tab's Unlit bomb frame) */
        const uint64_t as = vr_bomb_chain(c, actor, as_path, 4);
        if (as) vr_bomb_call(c, 0x837d5d70ULL, as, 0, 0, hold_frame, NULL, NULL);
    }
    vr_bomb_centre(c, actor, b->centre);
    if (b->holder >= 0) b->flight = 0, hos_vr_held_keep(c, actor);   /* (in a VR hand, placed or carried) */
    {   /* its collision with Link: none while held or pulled, the ground only just after it is let go, its own once clear of him */
        int layer = LAYER_OBJECT;
        if (b->holder >= 0 || b->pull_hand >= 0) layer = LAYER_NO_HIT;
        else if (b->released_at && now - b->released_at < 1000000000LL) {
            const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
            float d2 = 0;
            for (int k = 0; k < 3; k++) { const float d = b->centre[k] - (player ? vr_bomb_f(c, player + 796 + 4 * k) : 0); d2 += k == 1 ? 0 : d * d; }
            if (!player || d2 < BOMB_CLEAR_LINK * BOMB_CLEAR_LINK) layer = LAYER_HIT_ONLY_GROUND;
        }
        if (b->holder >= 0 || b->pull_hand >= 0 || b->layer >= 0) vr_bomb_layer(c, b, layer);
    }
    if (b->holder >= 0 && b->grip_set) {   /* held: placed with the hand, moving with it */
        const VrBombHand *h = &vr_bomb_hand[b->holder];
        if (!h->valid) return;
        /* Not while Link's carry still has it (the pull-out until the game's second ZR, or his lift of a held bomb: action
           bits 0x06000000). It is attached to him then, and placing it at the hand dragged Link after it: pulling out a
           bomb shoved him forward (user report). The renderer draws it in the palm meanwhile (equipment_skin.inc
           bomb_drawn); it is placed again once he lets go of it. */
        const uint64_t po = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
        const uint32_t action = po ? recomp_load32(c, po + 0x1f60) : 0;
        if ((action & 0x06000000u) == 0x06000000u || b->on_plant ||   /* (on its plant: until the game picks it) */
            (b->actor == vr_bomb_pulled && (vr_bomb_pull == PULL_TAKE || vr_bomb_pull == PULL_THROW))) return;
        float origin[3], rot[9], rel[3];
        const float t = fminf(1, (float)(hos_now_ns() - b->seat_at) / BOMB_SEAT_NS);   /* (sliding into its seat) */
        for (int a = 0; a < 3; a++) rel[a] = b->seat_from[a] + (b->rel_p[a] - b->seat_from[a]) * t * t * (3 - 2 * t);
        for (int k = 0; k < 3; k++) {
            origin[k] = h->p[k];
            for (int a = 0; a < 3; a++) origin[k] += h->axis[a][k] * rel[a];
        }
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                rot[3 * i + j] = 0;
                for (int a = 0; a < 3; a++) rot[3 * i + j] += h->axis[a][i] * b->rel_r[3 * a + j];
            }
        vr_bomb_place(c, actor, origin, rot);
        float v[3];
        for (int k = 0; k < 3; k++) v[k] = h->v[k] + vr_bomb_link_v[k];   /* (with Link as he runs: it does not trail the hand) */
        vr_bomb_set_velocity(c, actor, v, 1);
    } else if (b->pull_hand >= 0) {
        /* Pulled (the gravity pull): flown to the hand's palm seat, placed along the way each update (a Bomb Flower's body,
           fixed on its plant, follows too) with the matching velocity, faster the further it has to go. There, it is held as
           if gripped where it arrived; a pull that has not arrived in BOMB_PULL_MAX_S (stuck behind something) is let go. */
        const int h = b->pull_hand;
        const VrBombHand *hand = &vr_bomb_hand[h];
        float seat[3], d[3], origin[3], rot[9];
        for (int k = 0; k < 3; k++) seat[k] = hand->p[k] + hand->axis[0][k] * (h ? -1.0f : 1.0f) * (BOMB_PALM + vr_bomb_radius(b));
        for (int k = 0; k < 3; k++) d[k] = seat[k] - b->centre[k];
        const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float dt = fminf(0.05f, fmaxf(0, (now - b->pull_last) * 1e-9f));
        b->pull_last = now;
        vr_bomb_pose(c, actor, origin, rot);
        if (!hand->valid || now - b->pull_at > (int64_t)(BOMB_PULL_MAX_S * 1e9)) {
            hos_log("vr: bombs: a pull gave up (%s)", hand->valid ? "it did not arrive: stuck on something?" : "the hand is not tracked");
            b->pull_hand = -1, b->released_at = now;
        } else if (dist < BOMB_REACH) {
            vr_bomb_grip_here(b, hand, h, origin, rot);
            b->holder = h, b->owned = 1, b->release = 0, b->pull_hand = -1;
            hos_log("vr: bombs: a pulled %s arrived in the %s hand after %.2f s", b->flower ? "Bomb Flower" : "bomb",
                    h ? "right" : "left", (now - b->pull_at) * 1e-9);
        } else {
            const float speed = fmaxf(BOMB_PULL_SPEED, dist / BOMB_PULL_MIN_S), step = fminf(dist, speed * dt);
            float v[3];
            for (int k = 0; k < 3; k++) v[k] = d[k] / dist * speed, origin[k] += d[k] / dist * step;
            vr_bomb_place(c, actor, origin, rot);
            vr_bomb_set_velocity(c, actor, v, 1);
        }
    } else if (b->flight) {
        /* In flight on the mod's arc: the game's gravity is nearly three times real gravity, so a toss is over before it
           can be judged. Each update the velocity is the arc's (the VR tab's Bomb gravity); where the bomb went instead
           (a wall, the floor, Link, an arrow) is a hit, and the game's physics has it from there (bounces, rolling) */
        if (b->flight_at) {
            const float dt = (now - b->flight_at) * 1e-9f;
            float miss = 0, expect = 0;
            for (int k = 0; k < 3; k++) {
                const float want = b->flight_v[k] * dt, moved = b->centre[k] - b->flight_last[k];
                miss += (moved - want) * (moved - want), expect += want * want;
            }
            const int settled = now - b->flight_since > 100000000LL;   /* (its first moves are still the hand's: not a hit) */
            if (dt > 0.2f || (settled && sqrtf(miss) > fmaxf(0.04f, 0.35f * sqrtf(expect))) || now - b->flight_since > 4000000000LL) {
                b->flight = 0;
                hos_log("vr: bombs: the thrown bomb hit something after %.2f s: the game's physics from here",
                        (now - b->flight_since) * 1e-9);
            } else b->flight_v[1] -= BOMB_REAL_G * rx_option(907, 100) * 0.01f * dt;
        } else b->flight_since = now;
        if (b->flight) {
            vr_bomb_set_velocity(c, actor, b->flight_v, 0);
            memcpy(b->flight_last, b->centre, sizeof b->flight_last);
            b->flight_at = now;
        }
    }
    if (vr_bomb_test() && b->released_at && now - b->released_at < 900000000LL) {   /* (test: its height each frame after a release) */
        float centre[3]; vr_bomb_centre(c, actor, centre);
        hos_log("vr: bombs: flight t=%.4f y=%.4f", (now - b->released_at) * 1e-9, centre[1]);
    }
}

/* ---- the game's buttons, from hos_oot_vr_bow_controls ---------------------------------------------------------------------
   With bombs owned: the real ZR (the right trigger) does nothing, the trigger lights; ZL (the left trigger) does nothing
   while the left hand holds a bomb; the take-out's own ZR tap is added. Whatever is selected, a hand holding a bomb (a Bomb
   Flower picked with a sword out) keeps its trigger from the game: it lights the bomb (ZR is the right trigger, ZL the left). */
uint64_t hos_vr_bomb_buttons(uint64_t buttons) {
    const uint64_t ZL = 1ULL << 8, ZR = 1ULL << 9;
    VrBombInput in;
    vr_bomb_input_now(&in);
    const int64_t now = hos_now_ns();
    if (in.holds[0]) buttons &= ~ZL;
    if (in.holds[1]) buttons &= ~ZR;
    if (now < in.a_until) buttons |= 1ULL << 0;   /* (A: the game's own pick of a Bomb Flower) */
    if (now < in.zr_until) buttons |= ZR;         /* (the take-out's tap) */
    if (vr_bomb_test() == 0 && !in.owned) return buttons;
    if (in.owned) buttons &= ~ZR;
    if (now < in.zr_until) buttons |= ZR;
    return buttons;
}
int hos_vr_bomb_grips(void) {
    VrBombInput in;
    vr_bomb_input_now(&in);
    return in.owned;
}
/* (other items' grips: the bow, the hookshot, the boomerang, the Deku Stick) a bomb is within this hand's reach, held or
   being pulled by it, or pointed at for its pull: its grip takes the bomb (vr_bomb_claims, as of Link's last update) */
int hos_vr_bomb_in_reach(int hand) {
    if (hand < 0 || hand > 1) return 0;
    VrBombInput in;
    vr_bomb_input_now(&in);
    return in.claims[hand];
}

/* ---- the headless test: a simulated right hand on a timeline (read the run's "vr: bombs:" lines) -----------------------
   RX_VR_BOMB_TEST=1 lit, 2 unlit: from when bombs are selected (the test script steps the game's quick select; L is blocked
   once bombs are selected): the grip at the right hip at 1 s (a bomb from the bag), the hand ahead of Link from 2.5 s, the
   trigger at 6 s (test 1), a throw at 8 s (RX_VR_BOMB_AIM=1: along the arc, as Bomb aim Arc to target). Test 2 then
   points the open hand at the thrown bomb from 11 s and squeezes at 12 s: the gravity pull, the bomb in the hand, let go
   at 15 s.
   RX_VR_BOMB_TEST=3, a Bomb Flower (teleport near some: RX_VR_TELEPORT_*): from 3 s after one is first within 6 m of
   Link, the open hand at the nearest, pointing at it; squeezed at 1 s (the game asked to pick it), lifted
   half a metre at 3 s, the trigger at 5 s (RX_VR_BOMB_TEST_LIGHT=1 only), a throw at 7 s; then pointing at it from 10 s
   and squeezed at 11 s (the pull), let go at 14 s. Its effects, animation and whether it explodes are in the log. --- */
static int64_t vr_bomb_test_t0;
uint64_t hos_vr_bomb_test_buttons(uint64_t buttons) {
    if (vr_bomb_test() && hos_vr_selected_is_bomb) buttons &= ~(1ULL << 6);   /* (keep the selection) */
    return buttons;
}
/* (test: how many effects and sounds a bomb's XLink has going: its ELink and SLink users' event counts) */
#ifdef HOOKSHOT_VR_RESEARCH   /* (the simulated hand: research builds only) */
static void vr_bomb_test_effects(Ctx *c, const VrBomb *b, unsigned out[3]) {
    out[0] = out[1] = out[2] = 0;
    const uint64_t x = vr_bomb_xlink(c, b->actor);
    if (!x) return;
    out[2] = recomp_load32(c, x + 200);
    for (int u = 0; u < 2; u++) {
        const uint64_t inst = recomp_load64(c, x + 112 + 8 * u);
        if (inst >= HOS_HEAP_BASE && hos_guest_range(inst + 56, 4)) out[u] = recomp_load32(c, inst + 56);
    }
}
static void vr_bomb_fake_hands(Ctx *c, uint64_t player, int64_t now, VrBombHand out[2]) {
    const int flower_test = vr_bomb_test() == 3;
    VrBomb *flower = NULL;
    if (flower_test) {
        static uint64_t chosen;   /* (the flower nearest Link when the timeline starts, kept) */
        for (int i = 0; i < BOMB_MAX; i++)
            if (vr_bombs[i].actor && vr_bombs[i].flower && (chosen ? vr_bombs[i].actor == chosen :
                                                            !flower || vr_bomb_from_link(&vr_bombs[i]) < vr_bomb_from_link(flower)))
                flower = &vr_bombs[i];
        if (!flower || (!chosen && vr_bomb_from_link(flower) > 6)) return;
        if (!chosen) chosen = flower->actor, hos_log("vr: bombs: test: the Bomb Flower %.2f m from Link", vr_bomb_from_link(flower));
        if (!vr_bomb_test_t0) vr_bomb_test_t0 = now + 3000000000LL, hos_log("vr: bombs: test: a Bomb Flower found, its timeline starts in 3 s");
        if (now < vr_bomb_test_t0) return;
    } else {
        if (!hos_vr_selected_is_bomb) return;
        /* (the timeline starts 15 s after bombs are first seen selected: a save may have them selected from the title screen on) */
        static int64_t selected_at;
        if (!selected_at) selected_at = now;
        if (now - selected_at < 15000000000LL) return;
        if (!vr_bomb_test_t0) vr_bomb_test_t0 = now, hos_log("vr: bombs: test timeline starts");
    }
    const float t = (now - vr_bomb_test_t0) * 1e-9f;
    const float px = vr_bomb_f(c, player + 796), py = vr_bomb_f(c, player + 800), pz = vr_bomb_f(c, player + 804);
    const float fx = vr_bomb_f(c, player + 816), fz = vr_bomb_f(c, player + 840);
    const float fn = sqrtf(fx * fx + fz * fz) > 1e-3f ? sqrtf(fx * fx + fz * fz) : 1;
    const float F[3] = {fx / fn, 0, fz / fn}, R[3] = {fz / fn, 0, -fx / fn}, U[3] = {0, 1, 0};
    VrBombHand *h = &out[1];
    h->valid = 1;
    /* (the right grip: +x = R (its palm, -x, faces left), -z up the fist, y = z x x) */
    for (int k = 0; k < 3; k++) h->axis[0][k] = R[k], h->axis[2][k] = -U[k];
    h->axis[1][0] = h->axis[2][1] * h->axis[0][2] - h->axis[2][2] * h->axis[0][1];
    h->axis[1][1] = h->axis[2][2] * h->axis[0][0] - h->axis[2][0] * h->axis[0][2];
    h->axis[1][2] = h->axis[2][0] * h->axis[0][1] - h->axis[2][1] * h->axis[0][0];
    /* (it points ahead and 30 degrees up, from 5 cm ahead of the hand: as the controller's aim pose) */
    for (int k = 0; k < 3; k++) h->aim[k] = F[k] * 0.866f + U[k] * 0.5f;
    h->aims = 1;
    static float flower_at[3];   /* (test 3: where the flower was when the timeline started) */
    if (flower_test) {
        static int fixed;
        if (!fixed) memcpy(flower_at, flower->centre, sizeof flower_at), fixed = 1;
        float toward[3], n = 0;   /* (from the flower toward Link, level) */
        for (int k = 0; k < 3; k++) toward[k] = k == 1 ? 0 : (k ? pz : px) - flower_at[k], n += toward[k] * toward[k];
        n = sqrtf(n) > 1e-3f ? sqrtf(n) : 1;
        for (int k = 0; k < 3; k++) {
            toward[k] /= n;
            h->p[k] = flower_at[k] + toward[k] * 0.1f + (t > 3 ? U[k] * 0.5f : 0);   /* (at it: within a grip's reach) */
            h->aim[k] = -toward[k];
        }
        h->grip = (t > 1 && t < 7.05f) || (t > 11 && t < 14) ? 1 : 0;
        h->trigger = vr_research_on("RX_VR_BOMB_TEST_LIGHT") && t > 5 && t < 5.3f ? 1 : 0;
        if (t > 6.8f && t < 7.05f) for (int k = 0; k < 3; k++) h->v[k] = -toward[k] * 4 + U[k] * 2;
        if (t > 10) {   /* (the pull: pointing at where it lies now) */
            float d[3], n2 = 0;
            for (int k = 0; k < 3; k++) d[k] = flower->centre[k] - h->p[k], n2 += d[k] * d[k];
            n2 = sqrtf(n2);
            if (n2 > 0.01f) for (int k = 0; k < 3; k++) h->aim[k] = d[k] / n2;
        }
    } else {
        const char *reach_env = vr_research_getenv("RX_VR_BOMB_TEST_REACH");
        const float reach = reach_env ? (float)atof(reach_env) : 0.45f;   /* (m ahead of Link) */
        const int at_hip = t < 2.5f;
        for (int k = 0; k < 3; k++) {
            const float base = k == 0 ? px : k == 1 ? py : pz;
            h->p[k] = at_hip ? base + R[k] * 0.3f + U[k] * 0.8f : base + F[k] * reach + R[k] * 0.2f + U[k] * 1.15f;
        }
        h->grip = (t > 1 && t < 8.05f) || (vr_bomb_test() == 2 && t > 12 && t < 15) ? 1 : 0;
        h->trigger = vr_bomb_test() == 1 && t > 6 && t < 6.3f ? 1 : 0;
        if (t > 7.8f && t < 8.05f) {   /* (a throw: RX_VR_BOMB_TEST_THROW=x,y,z in world axes, else forward and up) */
            const char *tv = vr_research_getenv("RX_VR_BOMB_TEST_THROW");
            float w[3] = {F[0] * 5, 3, F[2] * 5};
            if (tv) sscanf(tv, "%f,%f,%f", &w[0], &w[1], &w[2]);
            memcpy(h->v, w, sizeof w);
        }
        if (vr_bomb_test() == 2 && t > 11) {   /* (the pull: pointing at the thrown bomb, wherever it lies) */
            for (int i = 0; i < BOMB_MAX; i++)
                if (vr_bombs[i].actor && vr_bombs[i].holder < 0 && vr_bombs[i].owned) {
                    float d[3], n = 0;
                    for (int k = 0; k < 3; k++) d[k] = vr_bombs[i].centre[k] - h->p[k], n += d[k] * d[k];
                    n = sqrtf(n);
                    if (n > 0.01f) for (int k = 0; k < 3; k++) h->aim[k] = d[k] / n;
                    break;
                }
        }
    }
    for (int k = 0; k < 3; k++) h->aim_from[k] = h->p[k] + 0.05f * h->aim[k];
    static int64_t said;
    if (now - said > 500000000LL) {
        said = now;
        for (int i = 0; i < BOMB_MAX; i++) if (vr_bombs[i].actor) {
            const VrBomb *b = &vr_bombs[i];
            const uint64_t po = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
            unsigned fx3[3];
            vr_bomb_test_effects(c, b, fx3);
            hos_log("vr: bombs: test t=%.1f action %#x %s held by %d%s, %s, centre %.2f m from the hand (%.2f %.2f %.2f), Link at %.2f %.2f %.2f; "
                    "effects %u, sounds %u, sleep reasons %#x, animation %#llx",
                    t, po ? recomp_load32(c, po + 0x1f60) : 0, b->flower ? "flower" : "bomb", b->holder,
                    b->pull_hand >= 0 ? " (being pulled)" : "", b->lit ? "lit" : "unlit",
                    sqrtf((b->centre[0] - h->p[0]) * (b->centre[0] - h->p[0]) + (b->centre[1] - h->p[1]) * (b->centre[1] - h->p[1]) +
                          (b->centre[2] - h->p[2]) * (b->centre[2] - h->p[2])),
                    b->centre[0], b->centre[1], b->centre[2], px, py, pz, fx3[0], fx3[1], fx3[2],
                    (unsigned long long)vr_bomb_command(c, b->actor));
        }
    }
}
#else
static void vr_bomb_fake_hands(Ctx *c, uint64_t player, int64_t now, VrBombHand out[2]) { (void)c, (void)player, (void)now, (void)out; }
#endif
