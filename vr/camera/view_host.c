/* Hookshot VR: The headset camera itself: each frame, the game's gameplay view replaced by the head pose (first person) or turned by
   it (third person). Called from hos_oot_free_view.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */

/* When first person last drove the live gameplay view (ns; 0: not now): the controllers ask it (controllers_host.c) so the
   right stick's jump and the freed upper button apply only while playing, never in menus, text, cutscenes or third person */
static volatile int64_t oot_vr_first_person_seen;
int hos_oot_vr_first_person(void) {
    const int64_t seen = __atomic_load_n(&oot_vr_first_person_seen, __ATOMIC_ACQUIRE);
    return seen && hos_now_ns() - seen < 200000000LL;
}

/* ---- Re-align after events (first person; VR tab "Re-align after events", option 927) ----------------------------------
   The view's heading (oot_vr_base_yaw below) is the mod's own: only the right stick and lock-on turn it, and the game moves
   Link relative to it. The game's events turn Link without either: a talk turns him to the speaker, a door or a chest plays
   its own moves, EventPlayerWarpToTargetActor snaps him to a mark, a map change puts him at the new map's entrance facing
   its own way. The heading stayed where it was, so afterwards Link faced somewhere else than the player (headset 18 Nov:
   "I seem to stay pretty aligned when shielding, but I still get out of alignment during cutscenes"): his first attack,
   jump or roll went his way, and the first push of the stick turned him on the spot.
   The rule: while an event plays (or Link is gone: a map loading), add up how far Link turns by himself (in updates where
   the left stick is not moving him; across a pause in his updates, all of it), then turn the view's heading by as much.
   The view keeps the relation to Link it had before the event, a right-stick turn made while standing included. (Setting
   the heading to Link's facing instead would undo such a turn, and in Headset mode the head's yaw too: Link faces where
   the head looked as he walked.) The turn is made at once where the player sees no turn: when the view comes back after a
   flat stretch (a cinematic), when Link is back after a load (the screen is still dark), or in the update a warp moves
   him (the view jumps with him anyway). Otherwise at the event's end, as a quick turn (kVrRealignSpeed) or a snap, or
   (Follow Link) all through the event, as Link turns. Never while locked on: the lock-on turn owns the heading then.
   Option 927: 0 off (the heading as before: kept through events, taken afresh from the game camera after a flat gap),
   1 a quick turn at the event's end, 2 a snap at the end, 3 follow Link through the event. */
#define kVrRealignMinTurn 0.087f          /* radians (5 degrees): a smaller turn by an event is left alone at its end */
#define kVrRealignSpeed 5.0f              /* radians a second (about 290 degrees): the quick turn's (and following's) speed */
#define kVrRealignWarp 1.0f               /* metres Link moves in one update while an event plays: a warp */
#define kVrRealignGapNs 500000000LL       /* a pause this long in Link's updates (ns): a load, not his own turning (a hitch
                                             is shorter, and the stick may be turning him across it) */
#define kVrRealignFreshNs 150000000LL     /* Link's position this recent (ns) means he is there to be read */
#define kVrLeftStickRecentNs 100000000LL  /* the left stick moved this recently (ns): Link's turn may be the player's own */
enum { kVrRealignOff, kVrRealignQuick, kVrRealignSnap, kVrRealignFollow };
typedef struct {
    int busy;                 /* last update: an event was playing, or Link was gone */
    int have_last;            /* last_*: Link's previous reading */
    int due;                  /* turn the view by `turned`: 1 the event is over; now: 2 a warp moved Link, 3 he is back
                                 after a load */
    int followed;             /* Follow Link has turned the view during this event (logged once) */
    int item_shown;           /* (set by the caller each update) the renderer has an item get's item in front of the headset */
    int item_got;             /* an item get's item was shown in this stretch: all Link's turns in it are the get's */
    int64_t last_at, first_at;   /* (Link's last reading; his first since first person began: 0 none yet) */
    float last_facing, last_pos[3];
    float turned;             /* radians Link has turned by himself since the event began, not yet given to the view */
    float easing;             /* radians of a quick turn still to go */
    char event[64];           /* the event (for the log): its name, or "a load" when Link was only gone */
} OotVrRealign;
static OotVrRealign oot_vr_realign;
/* (vr/input/controllers_host.c: no put-away press while an event plays, a talk among them) */
int hos_vr_event_playing(void) { return __atomic_load_n(&oot_vr_realign.busy, __ATOMIC_RELAXED); }
static float oot_vr_base_yaw;   /* the view's heading: the way its back points (atan2 x, z); it faces this + pi */

/* (VR tab, Item get view, option 982) An item get: Dm_SY_Get (an item picked up, a heart piece, a token), its forms
   Dm_SY_Get_WithoutPlayer, Dm_SY_GetInEvent and Dm_SY_ShopGet, and the chests' Dm_SY_Treasurebox(Small). The game turns
   Link round in it to show the item to its camera (its AS Rotate_GetDemo): headset logs 18 Nov, "Link turned 58.9" and
   "126.4 by it", then the view turned as much at the end: "the VR camera snaps around 180 degrees" (player). The turn is
   for the game's camera, not the player's way, so by default it is not given to the view's heading. Most item gets play
   under other names (a chest's own event, an NPC's talk: vr/items/item_get/item_get_host.c), so the same holds for the
   whole event once the renderer shows an item get's item in front of the headset (item_shown): Link's turns before it
   are dropped too. The game turns him round before he raises the item (headset 21 Nov, the Kokiri Sword's chest: "Link
   turned 173.4 by it", then the view re-aligned from -140.2 to 33.2 degrees: a snap round at the get's end). */
static int oot_vr_event_item_get(const char *event) {
    return !strncmp(event, "Dm_SY_Get", 9) || !strncmp(event, "Dm_SY_Treasurebox", 17) || !strcmp(event, "Dm_SY_ShopGet");
}
/* Every first-person update, flat or not: what the event did to Link. event: the playing event's name ("" none); link_ok:
   Link's facing (radians, atan2 x, z of his forward) and position (metres) were read; stick_moving: the left stick is
   moving him; view_forward: where the view faces now (radians), for the log. */
static void oot_vr_realign_watch(OotVrRealign *s, int64_t now, const char *event, int link_ok, float facing,
                                 const float pos[3], int stick_moving, float view_forward) {
    const float tau = 6.283185307f, deg = 57.2958f;
    const int busy = event[0] || !link_ok;
    if (busy && !s->busy) s->event[0] = 0, s->followed = 0, s->item_got = 0;   /* (a new stretch begins) */
    if (busy && s->item_shown && !s->item_got && rx_option(982, 1))
        s->item_got = 1, s->turned = 0, s->due = s->due == 2 ? 0 : s->due;   /* (the turns so far dropped too) */
    if (event[0] && strcmp(s->event, event)) {
        snprintf(s->event, sizeof s->event, "%s", event);
        if (link_ok) hos_log("vr: event %s begins: Link faces %.1f, the view %.1f degrees", event, facing * deg,
                             remainderf(view_forward, tau) * deg);
    } else if (busy && !s->event[0]) snprintf(s->event, sizeof s->event, "a load");
    if (link_ok) {
        if (s->have_last && (busy || s->busy)) {
            const int gap = now - s->last_at > kVrRealignGapNs;
            /* (Link's turns in an item get are left out of it with Item get view: Keep your view) */
            const int kept = busy && (oot_vr_event_item_get(event) || s->item_got) && rx_option(982, 1);
            if ((gap || !stick_moving) && !kept) s->turned += remainderf(facing - s->last_facing, tau);
            const float moved = hypotf(hypotf(pos[0] - s->last_pos[0], pos[1] - s->last_pos[1]), pos[2] - s->last_pos[2]);
            if (fabsf(s->turned) >= kVrRealignMinTurn && (gap || (busy && moved > kVrRealignWarp))) s->due = gap ? 3 : 2;
        }
        s->last_facing = facing, s->last_at = now, s->have_last = 1;
        if (!s->first_at) s->first_at = now;
        memcpy(s->last_pos, pos, sizeof s->last_pos);
    }
    if (s->busy && !busy) {   /* (the event is over and Link is back) */
        const int load = !strcmp(s->event, "a load");
        if (!load || fabsf(s->turned) >= kVrRealignMinTurn)   /* (not every hitch in Link's updates) */
            hos_log("vr: %s%s over: Link faces %.1f, the view %.1f degrees; Link turned %.1f by it", load ? "" : "event ",
                    s->event, facing * deg, remainderf(view_forward, tau) * deg, s->turned * deg);
        if (!s->due) s->due = 1;   /* (s->event is kept for the re-align's log line) */
    }
    s->busy = busy;
}

/* The turn (radians) to give the view's heading this update. style: option 927; shown: the view was in the headset the
   update before (not after a flat stretch or a load); locked: Link is locked on (the lock-on turn owns the heading);
   base_yaw: the heading now, for the log. */
static float oot_vr_realign_turn(OotVrRealign *s, int style, int shown, int locked, float dt, float base_yaw) {
    if (style <= kVrRealignOff || style > kVrRealignFollow || locked) {   /* (off: still added up for the event's log line) */
        s->easing = 0;
        if (locked || s->due) s->turned = 0, s->due = 0;
        return 0;
    }
    const float deg = 57.2958f, tau = 6.283185307f, pi = 3.141592654f;
    float turn = 0;
    if (style == kVrRealignFollow && shown && s->busy && !s->due && s->turned != 0) {   /* (as Link turns) */
        if (!s->followed && fabsf(s->turned) >= kVrRealignMinTurn * .1f)
            s->followed = 1, hos_log("vr: heading follows Link's turns during %s", s->event);
        s->easing += s->turned, s->turned = 0;
    }
    if (s->due || !shown) {
        if (fabsf(s->turned) >= kVrRealignMinTurn) {
            const int at_once = s->due >= 2 || !shown || style == kVrRealignSnap;
            hos_log("vr: heading re-aligned after %s: from %.1f to %.1f degrees (%s)", s->event[0] ? s->event : "an event",
                    remainderf(base_yaw + pi, tau) * deg, remainderf(base_yaw + s->easing + s->turned + pi, tau) * deg,
                    s->due == 2 ? "at the warp" : s->due == 3 ? "as Link came back" : !shown ? "as the view came back" :
                    at_once ? "snap" : "quick turn");
            if (at_once) turn = s->easing + s->turned, s->easing = 0;
            else s->easing += s->turned;
            s->turned = 0;
        } else if (s->due == 1) s->turned = 0;   /* (the event is over: a smaller turn is left alone) */
        s->due = 0;
    }
    if (s->easing != 0) {
        const float step = copysignf(fminf(fabsf(s->easing), kVrRealignSpeed * dt), s->easing);
        turn += step, s->easing -= step;
        if (fabsf(s->easing) < 1e-4f) s->easing = 0;
    }
    return turn;
}

/* ---- Turning (first person; VR menu Comfort -> Turning: options 1030-1032) -------------------------------------------
   The right stick turns the view's heading. Smooth (option 1030 = 0, as before): as fast as the stick is pushed past a small
   dead zone, a full stick kVrTurnFull radians a second times Smooth turn speed (option 1032: Slow 0.5, Medium 1, the speed
   it always had, Fast 1.5, Very fast 2). Snap (1): a push past kVrSnapPush turns by Snap angle at once (option 1031: 30, 45,
   60, 90 degrees), and the stick must come back under kVrSnapRearm before the next: one push, one turn, so holding it
   never spins. (The right stick's jump counts only a push within 30 degrees of straight up, controllers_host.c: a snap's
   push is sideways.) */
#define kVrTurnDead .15f      /* (a stick under this is at rest: smooth turns) */
#define kVrTurnFull 1.6f      /* radians a second at a full stick, Medium (about 92 degrees) */
#define kVrSnapPush .6f       /* (a snap: the stick pushed this far left or right) */
#define kVrSnapRearm .35f     /* (and back under this before the next) */
typedef struct { int pushed; } OotVrTurning;
static OotVrTurning oot_vr_turning;
/* The turn (radians) taken off the heading's yaw for a right stick at x (-1..1, + right) over dt seconds: + turns right */
static float oot_vr_stick_turn(OotVrTurning *s, int style, int snap_angle, int speed, float x, float dt) {
    if (style == 1) {
        static const float degrees[4] = {30, 45, 60, 90};
        if (fabsf(x) < kVrSnapRearm) s->pushed = 0;
        if (s->pushed || fabsf(x) < kVrSnapPush) return 0;
        s->pushed = 1;
        const float turn = degrees[(unsigned)snap_angle < 4 ? snap_angle : 1] * 0.0174532925f;
        static unsigned told;
        if (told < 40) told++, hos_log("vr: snap turn %s %.0f degrees", x > 0 ? "right" : "left", turn * 57.2958f);
        return x > 0 ? turn : -turn;
    }
    s->pushed = 0;
    static const float factor[4] = {.5f, 1, 1.5f, 2};
    if (fabsf(x) < kVrTurnDead) return 0;
    return x * dt * kVrTurnFull * factor[(unsigned)speed < 4 ? speed : 1];
}
/* (vr/comfort/comfort_host.c, after this file: what moves the view for the Comfort Aids' vignettes) */
static void oot_vr_comfort_frame(Ctx *c, int64_t now, float turned);
static void oot_vr_comfort_vantage(int64_t now, const float eye[3], const float back[3]);

#include "../items/item_get/item_get_host.c"   /* (an item get: the item in front of the headset, the renderer told) */

/* Reads the game for oot_vr_realign_watch: the playing event (vr/input/stick_host.c) and Link's facing (his actor's
   rotation, forward column x +816, z +840: read only; the game turns him through its character controller, so writes are
   lost, headless 16 Nov) and position. First person only. */
static void oot_vr_realign_frame(Ctx *c, const HookshotVrHead *h) {
    if (h->mode != 1) { memset(&oot_vr_realign, 0, sizeof oot_vr_realign); return; }
    const int64_t now = hos_now_ns();
    char event[64] = "";
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    if (events && recomp_load64(c, events + 104) && !oot_vr_event_name(c, events, event))
        snprintf(event, sizeof event, "an event");
    oot_vr_realign.item_shown = oot_vr_item_get_frame(event);   /* (vr/items/item_get/item_get_host.c) */
    const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const int64_t seen = __atomic_load_n(&oot_vr_player_seen, __ATOMIC_ACQUIRE);
    float facing = 0, pos[3] = {0, 0, 0};
    int link_ok = seen && now - seen <= kVrRealignFreshNs && actor && hos_guest_range(actor, 844) &&
                  recomp_load64(c, actor) == 0x84b45e18ULL;   /* (the player's actor class, as oot_vr_lock_point checks) */
    if (link_ok) {
        const float fx = vr_load(c, actor + 816), fz = vr_load(c, actor + 840);
        for (int k = 0; k < 3; k++) {
            const uint32_t bits = __atomic_load_n(&oot_vr_player_xyz[k], __ATOMIC_RELAXED);
            memcpy(&pos[k], &bits, sizeof pos[k]);
        }
        link_ok = isfinite(fx) && isfinite(fz) && hypotf(fx, fz) > .5f &&   /* (upright enough to have a heading) */
                  isfinite(pos[0]) && isfinite(pos[1]) && isfinite(pos[2]);
        if (link_ok) facing = atan2f(fx, fz);
    }
    const int64_t stick = __atomic_load_n(&oot_vr_left_stick_seen, __ATOMIC_ACQUIRE);
    oot_vr_realign_watch(&oot_vr_realign, now, event, link_ok, facing, pos, stick && now - stick < kVrLeftStickRecentNs,
                         oot_vr_base_yaw + 3.141592654f);
}

/* (research, headless benchmarks: vr/tools/bench.py) RX_VR_BENCH_FOV=<width>x<height> in degrees, e.g. 109.6x111.6 (what
   the "vr: fov asked" log line says for the player's headset): without a headset, the game's own camera is widened to the
   headset's FOV anyway, so a run without one draws as much of the world as the headset makes it draw (the wide view
   roughly quadruples the draws). Unset or unreadable: the game's own FOV, as before. Read once. */
static void oot_vr_bench_fov(void) {
    static int read;
    static float fov_y, aspect;
    if (!read) {
        read = 1;
        const char *e = vr_research_getenv("RX_VR_BENCH_FOV");
        float w = 0, h = 0;
        if (e && sscanf(e, "%fx%f", &w, &h) == 2 && w > 10 && w < 170 && h > 10 && h < 170) {
            const float tan_x = tanf(w * 0.5f / 57.2958f), tan_y = tanf(h * 0.5f / 57.2958f);
            fov_y = 2.0f * atanf(tan_y), aspect = tan_x / tan_y;
            hos_log("vr: RX_VR_BENCH_FOV: no headset, the game's camera drawn at %.1f x %.1f deg", w, h);
        }
    }
    hos_vr_fov_y = fov_y, hos_vr_aspect = aspect;
    if (fov_y > 0) hos_vr_detail = hos_vr_foliage = 1;   /* (the VR tab's defaults, Switch 2: the headset path sets them from rx_vr_head) */
}

static int hos_oot_vr_view(Ctx *c, uint64_t view) {
    HookshotVrHead h;
    oot_vr_crawl_research(c, view);   /* (RX_VR_CRAWL_TEST: also without a headset, vr/camera/crawl_host.c) */
    oot_vr_event_research(c, view);   /* (RX_VR_EVENT_TEST: also without a headset, vr/camera/events_host.c) */
    if (!rx_vr_head(&h) || !h.active) {
        oot_vr_camera_fade(c, 0);   /* (the headset gone: the game's camera fade on Link handed back, review 18 Nov) */
        oot_vr_guard_reset();
        oot_vr_bench_fov();   /* (0 and 0, the game's own FOV, unless RX_VR_BENCH_FOV) */
        __atomic_store_n(&hos_vr_cull_first_person, 0, __ATOMIC_RELEASE);
        rx_vr_body(0, 0, 1, NULL, NULL, NULL);
        if (oot_vr_ev.palette.touched && oot_vr_event_test() < 4) oot_vr_event_frame(c, view, 0, 0);   /* (a held palette back) */
        return oot_vr_event_research_view(c, view);   /* (RX_VR_EVENT_TEST 2-4: first person's view, headless) */
    }
    hos_vr_detail = h.detail, hos_vr_foliage = h.foliage;
    oot_vr_note_selected_item(c);
    oot_vr_radial_frame(c);   /* (vr/menu/radial_host.c) */
    oot_vr_wrist_frame(c);    /* (the wrist check HUD's values: vr/menu/wrist_host.c) */
    oot_vr_camera_fade(c, h.mode == 1 && rx_option(902, 0));
    oot_vr_realign_frame(c, &h);   /* (before the flat check: events shown flat count too) */
    oot_vr_event_frame(c, view, h.mode == 1, h.height_cm);   /* (how an event is shown: vr/camera/events_host.c) */
    /* (a full-screen menu, the pause menu among them, vr/headset/bridges_host.c hos_vr_menu_now: flat as the quick select is.
       Its 3D (the Equipment page's Link, the Collection's objects) is drawn at the picture's own camera ratio, not with the
       headset's FOV and shape: with them, Link came out wide and short in the headset, 22 Nov) */
    int hos_vr_menu_now(void);
    if (!oot_vr_player_camera(c, view) || __atomic_load_n(&oot_vr_quick_open, __ATOMIC_ACQUIRE) || hos_vr_menu_now()) {   /* (quick select: flat) */
        oot_vr_guard_reset();
        hos_vr_fov_y = hos_vr_aspect = 0;
        __atomic_store_n(&hos_vr_cull_first_person, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&oot_vr_move_seen, 0, __ATOMIC_RELEASE);
        rx_vr_drawn(0, NULL);   /* the headset presents the untouched scripted shot as a stable cinema view */
        rx_vr_body(0, 0, 1, NULL, NULL, NULL);
        return 0;
    }
    if (h.mode == 1) {   /* (an event shown from the director's camera or a theatre seat: vr/camera/events_host.c) */
        float eye[3], right[3], up[3], back[3], axis[3][3], m[12];
        if (oot_vr_event_vantage(c, view, eye, right, up, back)) {
            for (int k = 0; k < 3; k++) eye[k] += (right[k] * h.p[0] + up[k] * h.p[1] + back[k] * h.p[2]) * h.lean;
            oot_vr_view_matrix(right, up, back, h.q, eye, axis, m);
            for (int i = 0; i < 12; i++) vr_store(c, view + 8 + 4 * i, m[i]);
            if (isfinite(h.tan_half_x) && isfinite(h.tan_half_y) && h.tan_half_x > 0.2f && h.tan_half_y > 0.2f) {
                hos_vr_fov_y = 2.0f * atanf(h.tan_half_y);
                hos_vr_aspect = h.tan_half_x / h.tan_half_y;
            } else hos_vr_fov_y = hos_vr_aspect = 0;
            oot_vr_guard_reset();   /* (as a flat event: no first-person body, hands' controls or movement meanwhile) */
            __atomic_store_n(&hos_vr_cull_first_person, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&oot_vr_first_person_seen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&oot_vr_move_seen, 0, __ATOMIC_RELEASE);
            oot_vr_comfort_vantage(hos_now_ns(), eye, back);   /* (a pan: the Comfort Aids' vignettes) */
            rx_vr_drawn(h.seq, m);
            rx_vr_crawl(h.seq, 0, 0);
            rx_vr_body(h.seq, 0, 1, NULL, NULL, NULL);
            rx_vr_bow_active(0);
            return 1;
        }
    }
    if (h.mode == 0) oot_vr_tp_aim_frame(c, view);   /* (aiming in third person: vr/items/aim/third_person_aim_host.c) */
    float pos[3], look[3], cup[3];
    for (int k = 0; k < 3; k++) {
        pos[k] = vr_load(c, view + 0x38 + 4 * k);
        look[k] = vr_load(c, view + 0x44 + 4 * k);
        cup[k] = vr_load(c, view + 0x50 + 4 * k);
    }
    float back[3] = {pos[0] - look[0], pos[1] - look[1], pos[2] - look[2]};
    const float lb = sqrtf(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
    if (!(lb > 1e-6f)) return 0;
    for (int k = 0; k < 3; k++) back[k] /= lb;
    const int guarding = h.mode == 1 && rx_option(903,0) &&
        oot_vr_guard_native(c,__atomic_load_n(&oot_vr_player_actor,__ATOMIC_RELAXED),NULL);
    {
        const int64_t now = hos_now_ns();
        const uint64_t player = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
        const uint32_t state = player ? recomp_load32(c, player + 0x1f60) : 0;
        const uint32_t equipment = oot_vr_equipment_snapshot(c, h.seq, h.mode == 1);
        /* (the hookshot and longshot use the same states: not the bow while one is selected, vr/items/hookshot. Nor the Deku
           Stick's 0x408: taken for the bow, the bow's grip code tapped B on its let-go and showed the bow's hand,
           headset log 18 Nov) */
        const int bow_out = h.mode == 1 && !hos_vr_selected_is_hookshot && !hos_vr_stick_selected() && !hos_vr_selected_is_boomerang &&
                            oot_vr_bow_state_owned(state, equipment);
        const int aiming = bow_out && (state & 0x2000000u) && ((state & 255u) == 0x42u || (state & 255u) == 0x46u);
        __atomic_store_n(&oot_vr_turn_bow_seen, (aiming || guarding) ? now : 0, __ATOMIC_RELEASE);
        /* The bow out in Link's hand, in any of its states (vr-aim-dump.bin: 0 without it, 0x2000041 taking it out, 0x442/0x446
           raising/drawn, 0x40000443 the shot, 0x441 out between shots until put away): its +0x1f60 nibble 0x400, or taking
           it out. The game's reticle is hidden, the headset's shown and the string owns ZR for all of them, not only the
           aim camera's 0x2000000 states (shots in 0x446 were not hand-aimed before). */
        {   /* (research, bounded) every change of Link's action state in first person, with the buttons the game saw: how the
               game takes the bow out and puts it away (a ZR tap, B, a timeout), before a grip may ask for either */
            static uint32_t last_state = 0xffffffffu;
            static unsigned told;
            if (h.mode == 1 && state != last_state && told < 800) {
                told++;
                hos_log("vr: action state %#x -> %#x (buttons the game sees %#llx)", last_state, state,
                        (unsigned long long)__atomic_load_n(&oot_vr_bow_mapped_buttons, __ATOMIC_RELAXED));
            }
            last_state = state;
        }
        __atomic_store_n(&oot_vr_bow_out_seen, bow_out ? now : 0, __ATOMIC_RELEASE);
        /* First person: the view's heading is the mod's own, not the game camera's. The game's camera swings behind Link as
           he runs once its follow mode starts (after bow aiming, lock-on, a landing), and the headset swung with it. Here
           the heading is taken from the game camera when first person starts (and, with Re-align after events off, after
           a gap: a cutscene, a load), then only the right stick turns it, and while Link is locked on (Z-targeting) it turns
           to keep the target ahead: eased, at most kVrLockTurn radians a second, so it never snaps; the head still looks
           around freely. Events that turn Link turn it by as much (oot_vr_realign_turn, above). Movement follows it:
           the game moves Link relative to the view written below (and the game itself faces Link to his target). */
        static int64_t base_at;
        static int locked;
        float base_yaw = oot_vr_base_yaw;
        const float game_yaw = atan2f(back[0], back[2]);
        oot_vr_turn_yaw = 0;   /* (the old bow/guard turn offset: unused while the heading is owned) */
        if (h.mode != 1) {
            base_at = 0;
            __atomic_store_n(&oot_vr_turn_input_seen, 0, __ATOMIC_RELEASE);
        } else {
            /* (a gap: the view was not shown for a while, a flat cinematic, a load, the quick select. With Re-align after
               events on, the heading is kept and turned by what the event did to Link; off, it is taken afresh from the game
               camera, as before: that camera's yaw after an event is the event's, not the player's) */
            const int realign = rx_option(927, 0);
            const int gap = !base_at || now - base_at > 500000000LL;
            const float dt = gap ? 0 : fmaxf(0, fminf(.05f, (now - base_at) * 1e-9f));
            /* (kept only if Link was read while the view was last shown: else, as on the title screen before a save
               loads, there is no relation to keep) */
            const int related = oot_vr_realign.first_at && oot_vr_realign.first_at <= base_at;
            if (!base_at || (gap && (!realign || !related))) {
                base_yaw = game_yaw;
                oot_vr_realign.turned = oot_vr_realign.easing = 0, oot_vr_realign.due = 0;   /* (a fresh heading) */
                hos_log("vr: first-person heading taken from the game camera (%.1f degrees)", base_yaw * 57.2958f);
            } else {
                if (gap)
                    hos_log("vr: first-person heading kept through a %.1f s gap (%.1f degrees; the game camera's %.1f)",
                            (now - base_at) * 1e-9f, base_yaw * 57.2958f, game_yaw * 57.2958f);
                const int64_t input = __atomic_load_n(&oot_vr_turn_input_seen, __ATOMIC_ACQUIRE);
                float x = __atomic_load_n(&oot_vr_turn_stick, __ATOMIC_RELAXED) / 32767.0f;
                if (!input || now - input > 100000000LL) x = 0;
                base_yaw -= oot_vr_stick_turn(&oot_vr_turning, rx_option(1030, 0), rx_option(1031, 1), rx_option(1032, 1), x, dt);
                const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
                float aim[3];
                const int target = oot_vr_lock_point(c, actor, aim);
                if (oot_vr_ev.style != kVrViewAnchored)   /* (Link's spot in a cinematic: his warps turn the view at its end) */
                    base_yaw += oot_vr_realign_turn(&oot_vr_realign, realign, !gap, target == 1, dt, base_yaw);
                if (target == 1) {
                    const float dx = aim[0] - vr_load(c, actor + 796), dz = aim[2] - vr_load(c, actor + 804);
                    if (dx * dx + dz * dz > .09f) {   /* (not straight above or below Link: no heading there) */
                        const float kVrLockTurn = 2.0f;   /* (radians a second, about 115 degrees) */
                        const float turn = remainderf(atan2f(-dx, -dz) - base_yaw, 6.283185307f) * fminf(1, dt * 6);
                        base_yaw += fmaxf(-kVrLockTurn * dt, fminf(kVrLockTurn * dt, turn));
                    }
                }
                if ((target == 1) != locked) {
                    locked = target == 1;
                    hos_log("vr: lock-on target %s", locked ? "acquired: the view turns to it" : "released: the view stays");
                }
            }
            base_yaw = remainderf(base_yaw, 6.283185307f);
            /* (Comfort Aids: the turn the view made without the head, for Vignette while turning; none at a fresh heading) */
            const int fresh = !base_at || (gap && (!realign || !related));
            oot_vr_comfort_frame(c, now, fresh ? 0 : remainderf(base_yaw - oot_vr_base_yaw, 6.283185307f));
            oot_vr_base_yaw = base_yaw;
            base_at = now;
            const float flat = hypotf(back[0], back[2]);
            back[0] = sinf(base_yaw) * flat, back[2] = cosf(base_yaw) * flat;
        }
        oot_vr_turn_at = now;
    }
    {   /* (the untargeted guard's facing: published from the view's heading and Link's torso; the view itself is not changed) */
        float guard_back[3] = {back[0], back[1], back[2]};
        oot_vr_guard_camera(c, &h, guarding, guard_back);
    }
    const float world_up[3] = {0, 1, 0};
    const float *u = cup;
    /* Third person keeps the game's downward orbit pitch so Link stays in frame. First person may level its body heading. */
    if (h.mode == 1 && h.level) {
        const float flat = sqrtf(back[0] * back[0] + back[2] * back[2]);
        if (flat > 1e-3f) back[0] /= flat, back[1] = 0, back[2] /= flat, u = world_up;
    }
    float right[3] = {u[1] * back[2] - u[2] * back[1], u[2] * back[0] - u[0] * back[2], u[0] * back[1] - u[1] * back[0]};
    const float lr = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (!(lr > 1e-6f)) return 0;
    for (int k = 0; k < 3; k++) right[k] /= lr;
    const float up[3] = {back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                         back[0] * right[1] - back[1] * right[0]};
    const float x = h.q[0], y = h.q[1], z = h.q[2], w = h.q[3];
    float eye[3] = {pos[0], pos[1], pos[2]}, body_root[3] = {};
    float crawl = 0, lowered = 0;   /* (how far into a crawl Link is, and how far the eye came down for it: crawl_host.c) */
    int body_child = 1, body_ok = 0;
    if (h.mode == 1) {
        body_ok = oot_vr_player_root(c, body_root, &body_child);
        if (!oot_vr_player_eye(c, h.height_cm, eye, &body_child)) {
            static int64_t last_missing;
            const int64_t now = hos_now_ns();
            if (now - last_missing > 5000000000LL)
                hos_log("vr: first-person waiting for Link's position"), last_missing = now;
        } else if (body_ok) {   /* (crawling: the eye comes down into the tunnel, vr/camera/crawl_host.c) */
            lowered = oot_vr_crawl_drop(c, eye[1] - body_root[1], &crawl);
            eye[1] -= lowered;
        }
        for (int k = 0; k < 3; k++) eye[k] += back[k] * h.body_depth_cm * 0.01f;
        /* (Link's spot in a cinematic: where he stood when it began, without the VR body; the event's own Link model is the
           one in the shots, and the real Link may be warped away: vr/camera/events_host.c) */
        if (oot_vr_event_anchor(eye)) body_ok = 0, crawl = lowered = 0;
    }
    for (int k = 0; k < 3; k++) eye[k] += (right[k] * h.p[0] + up[k] * h.p[1] + back[k] * h.p[2]) * h.lean;
    float axis[3][3], m[12];
    oot_vr_view_matrix(right, up, back, h.q, eye, axis, m);
    for (int i = 0; i < 12; i++) {
        uint32_t bits;
        memcpy(&bits, &m[i], sizeof bits);
        recomp_store32(c, view + 8 + 4 * i, bits);
    }
    /* The engine's own perspective update consumes these next frame. Updating its FOV and aspect together keeps CPU culling,
       shadows and the GPU projection consistent; writing only the finished matrix made area loading crash. */
    if (isfinite(h.tan_half_x) && isfinite(h.tan_half_y) && h.tan_half_x > 0.2f && h.tan_half_y > 0.2f) {
        hos_vr_fov_y = 2.0f * atanf(h.tan_half_y);
        hos_vr_aspect = h.tan_half_x / h.tan_half_y;
    } else hos_vr_fov_y = hos_vr_aspect = 0;
    /* The game derives movement from the view matrix, which already includes head yaw. Counter-rotate when movement should
       stay with the game camera: always in third person, and for first-person Camera mode. */
    if (h.mode == 0 || !h.move_head) {
        const float yaw = atan2f(2 * (w * y + x * z), 1 - 2 * (y * y + z * z));
        uint32_t bits;
        memcpy(&bits, &yaw, sizeof bits);
        __atomic_store_n(&oot_vr_move_yaw_bits, bits, __ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_move_seen, hos_now_ns(), __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&oot_vr_move_seen, 0, __ATOMIC_RELEASE);
    }
    /* Hand-line aiming is visual until the projectile launch is hooked. Do not steer the gameplay camera from the hands. */
    for (int k = 0; k < 3; k++) hos_vr_cull_eye[k] = eye[k];
    __atomic_store_n(&hos_vr_cull_first_person, h.mode == 1 && body_ok, __ATOMIC_RELEASE);   /* (hos_vr_cull_snapshot) */
    __atomic_store_n(&oot_vr_first_person_seen, h.mode == 1 && body_ok ? hos_now_ns() : 0, __ATOMIC_RELEASE);
    rx_vr_drawn(h.seq, m);
    if (h.mode == 1 && body_ok) oot_vr_boom_note_view(eye, axis[2]);   /* (the Boomerang's eyes' line: vr/items/boomerang) */
    rx_vr_crawl(h.seq, crawl, lowered);   /* (before the body it goes with: the renderer pairs them by h.seq) */
    rx_vr_body(h.seq, h.mode == 1 && body_ok, body_child, body_root, right, back);
    /* (2: the bow that is out is the slingshot, drawn and aimed as one: vr/items/slingshot) */
    rx_vr_bow_active(h.mode == 1 && body_ok && __atomic_load_n(&oot_vr_bow_out_seen, __ATOMIC_ACQUIRE) != 0 ?
                     (hos_vr_selected_is_slingshot ? 2 : 1) : 0);
    static int last_mode = -1;
    if (last_mode != h.mode) hos_log("vr: %s camera", h.mode ? "first-person" : "third-person"), last_mode = h.mode;
    return 1;
}
