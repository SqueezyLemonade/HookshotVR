/* Hookshot VR: the Comfort Aids, the game side: what moves the first-person view without the player's own head, for the
   vignette round both eyes (the renderer draws it: vr/comfort/vignette_layer.inc; the VR menu's Comfort -> Comfort Aids
   rows say how strong for each). Four causes, each 0 or 1, told every game frame in first person (rx_vr_comfort):
   - moving: the left stick pushes Link (oot_vr_left_stick_seen, vr/input/stick_host.c) and he moves (kVrComfortMove);
   - turning: the view's heading turns (the right stick, a snap, lock-on, a re-align after an event: view_host.c says how
     far each update), faster than kVrComfortTurnRate, or by a snap's worth at once;
   - in the air: a jump or a fall, both, as an OoT jump off a ledge is mostly a fall. No in-air flag of the game's is known,
     so it is read from Link's height: falling under gravity (kVrComfortAirAccel, once falling kVrComfortPullFall) or
     falling fast (kVrComfortAirFall), each kVrComfortRunning readings running. The vertical speeds are over
     kVrComfortSpan readings and the pull over twice as many, so the readings' uneven timing (they are timed as the host sees
     them) does not read as a pull on a slope; a stair's step, up or down, reads as a pull for two readings at most. (No
     rule for rising fast: running up stairs rises nearly 3 m/s);
   - the game moving you: Link moving with the left stick at rest a while (kVrComfortCoastNs): a hookshot pull,
     knockback, a current, a moving platform, an event walking him; or faster than he can run whatever the stick (Epona,
     a hookshot pull) (kVrComfortRide).
   A cinematic in 3D at the director's camera or a theatre seat (vr/camera/events_host.c) is the same to them, the view
   standing at the camera in place of Link (oot_vr_comfort_vantage): a pan or a dolly is the game moving you (Carry
   vignette), the camera's heading turning is turning (Turn vignette); a cut is a warp, no motion.
   Each is held kVrComfortHoldNs after it was last seen. Only what a row is on for is told or logged (option 1033: a bit
   each); a warp or a load (kVrComfortWarp) is no motion.

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/camera/view_host.c, whose first-person view
   update calls oot_vr_comfort_frame. Not compiled on its own. */
void rx_vr_comfort(const float cause[4]);   /* (vr/headset/bridges_host.c) */

#define kVrComfortMove .3f            /* m/s: Link moving this fast while the left stick pushes him: moving */
#define kVrComfortTurnRate .2f        /* radians a second: the heading turning this fast (a smooth turn's slowest is 0.24) */
#define kVrComfortTurnSnap .2f        /* radians in one update: a snap or a re-align's snap, whatever the time */
#define kVrComfortAirAccel 12.0f      /* m/s/s down, kVrComfortRunning readings running: falling under gravity (a hill's
                                         crest 3 m round at a full run is 9; the game's gravity is about 3 times real's,
                                         vr_options.inc vr_bomb_gravity) */
#define kVrComfortAirFall 4.5f        /* m/s down, kVrComfortRunning readings running: a fall (down a 45 degree slope at a
                                         run is 3.7) */
#define kVrComfortRunning 3           /* readings running for the air's rules (a stair's step makes two) */
#define kVrComfortPullFall 3.0f       /* m/s down: the pull counts only once falling this fast (running over Hyrule Field's
                                         humps pulled as hard, falling at most 2.6: headless 21 Nov) */
#define kVrComfortCarried 1.0f        /* m/s: Link moving this fast, the left stick at rest: the game moves him */
#define kVrComfortRide 6.0f           /* m/s over the ground: faster than Link runs (5.3), the game moves him whatever the stick */
#define kVrComfortWarp 30.0f          /* m/s: faster is a warp or a load, not motion */
#define kVrComfortSpan 2              /* readings: the vertical speed over this many, the pull over twice as many */
#define kVrComfortHistory 5           /* (2 x kVrComfortSpan + 1 readings kept) */
#define kVrComfortSameNs 12000000LL   /* a reading this soon after the last is the same game frame's: left out */
#define kVrComfortStillNs 100000000LL /* Link's place read again unchanged: a reading only this long after the last (he is
                                         read several times a game frame, from several threads, and moves once in it: headless
                                         21 Nov, a run at 5 m/s read as 0.4 between stops of 0) */
#define kVrComfortStickNs 150000000LL /* the left stick pushed this recently: the player moves Link */
#define kVrComfortCoastNs 400000000LL /* and at rest this long before his motion is the game's: he slows from a run after the
                                         stick is let go (headless 21 Nov: 3.9 m/s just after, read as the game moving him) */
#define kVrComfortHoldNs 150000000LL  /* each cause kept this long after it was last seen (a stride's or a frame's gap) */
enum { kVrComfortMoving, kVrComfortTurning, kVrComfortAir, kVrComfortGameMoves };
typedef struct {
    int have;                 /* readings of Link kept, up to kVrComfortHistory */
    int64_t at, view_at;      /* (the last one's time, oot_vr_player_seen; the view's last update) */
    float pos[3], vy;         /* (where Link was, and his vertical speed then: m/s, up) */
    int64_t past_at[kVrComfortHistory];   /* (the readings' times and heights, the newest first) */
    float past_y[kVrComfortHistory];
    int falling, fast_down;   /* (readings running) */
    float speed, ground;      /* (his speed at the last reading, and over the ground: m/s) */
    int64_t until[4];         /* each cause on until then (ns) */
    int shown[4];             /* what was last told, for the log */
    unsigned told;
} OotVrComfort;
static OotVrComfort oot_vr_comfort;

/* One update of the causes. now: ns; turned: radians the view's heading turned this update (0 at a fresh heading); stick:
   when the left stick last pushed Link; seen, pos: Link's last reading (its time, where); on: option 1033's bits. Fills
   cause[4]. (Its own function, apart from the game's memory, so the tester runs it: vr/tools/test.py comfort) */
static void oot_vr_comfort_step(OotVrComfort *s, int64_t now, float turned, int64_t stick, int64_t seen, const float pos[3],
                                int on, float cause[4]) {
    int is[4] = {0, 0, 0, 0};
    const float view_dt = s->view_at && now > s->view_at ? (now - s->view_at) * 1e-9f : 0;
    s->view_at = now;
    if (fabsf(turned) >= kVrComfortTurnSnap || (view_dt > 0 && view_dt < .25f && fabsf(turned) / view_dt > kVrComfortTurnRate))
        is[kVrComfortTurning] = 1;
    const int moved = pos && (!s->have || fabsf(pos[0] - s->pos[0]) + fabsf(pos[1] - s->pos[1]) + fabsf(pos[2] - s->pos[2]) > 1e-4f);
    if (seen && (!s->have || seen - s->at >= (moved ? kVrComfortSameNs : kVrComfortStillNs)) && pos && isfinite(pos[0]) &&
        isfinite(pos[1]) && isfinite(pos[2])) {   /* (a new reading) */
        const float dt = s->have && seen > s->at ? (seen - s->at) * 1e-9f : 0;
        const float v[3] = {dt > 0 ? (pos[0] - s->pos[0]) / dt : 0, dt > 0 ? (pos[1] - s->pos[1]) / dt : 0,
                            dt > 0 ? (pos[2] - s->pos[2]) / dt : 0};
        const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(dt > 0 && dt < .25f) || speed > kVrComfortWarp) s->have = 0;   /* (the first, after a gap, or a warp: anew) */
        for (int k = kVrComfortHistory - 1; k > 0; k--) s->past_at[k] = s->past_at[k - 1], s->past_y[k] = s->past_y[k - 1];
        s->past_at[0] = seen, s->past_y[0] = pos[1];
        s->have = s->have < kVrComfortHistory ? s->have + 1 : kVrComfortHistory;
        s->speed = s->have > 1 ? speed : 0, s->ground = s->have > 1 ? hypotf(v[0], v[2]) : 0;
        const int n = kVrComfortSpan;
        /* (the vertical speed over the last span, and the one before it: the pull between them) */
        const float span0 = s->have > n ? (s->past_at[0] - s->past_at[n]) * 1e-9f : 0;
        const float span1 = s->have > 2 * n ? (s->past_at[n] - s->past_at[2 * n]) * 1e-9f : 0;
        const float vy = span0 > 0 ? (s->past_y[0] - s->past_y[n]) / span0 : 0, last_vy = s->vy;
        s->vy = vy;
        s->fast_down = span0 > 0 && vy < -kVrComfortAirFall ? s->fast_down + 1 : 0;
        if (span0 > 0 && span1 > 0) {
            const float before = (s->past_y[n] - s->past_y[2 * n]) / span1, ay = (vy - before) / ((span0 + span1) * .5f);
            /* (and slower up, or faster down, than at the reading before: in a fall it is every reading; over a step up
               and then down, as at a stair's top, one reading in between is level) */
            s->falling = ay < -kVrComfortAirAccel && vy < last_vy - kVrComfortAirAccel * dt * .5f ? s->falling + 1 : 0;
        } else s->falling = 0;
        s->at = seen;
        memcpy(s->pos, pos, sizeof s->pos);
    }
    const int fresh = s->have && now - s->at < 150000000LL;   /* (Link read lately: else no motion of his is known) */
    const int pushed = stick && now - stick < kVrComfortStickNs, coasting = stick && now - stick < kVrComfortCoastNs;
    if (fresh) {
        if (pushed && s->speed > kVrComfortMove) is[kVrComfortMoving] = 1;
        if ((s->falling >= kVrComfortRunning && s->vy < -kVrComfortPullFall) || s->fast_down >= kVrComfortRunning)
            is[kVrComfortAir] = 1;
        if ((!coasting && s->speed > kVrComfortCarried) || s->ground > kVrComfortRide) is[kVrComfortGameMoves] = 1;
    }
    static const char *const names[4] = {"moving", "turning", "in the air", "the game moves Link"};
    for (int i = 0; i < 4; i++) {
        if (!(on >> i & 1)) is[i] = 0, s->until[i] = 0;
        if (is[i]) s->until[i] = now + kVrComfortHoldNs;
        const int now_on = s->until[i] > now;
        cause[i] = now_on ? 1.0f : 0.0f;
        if (now_on != s->shown[i]) {
            s->shown[i] = now_on;
            if (s->told < 300)
                s->told++, hos_log("vr: comfort: %s %s (speed %.1f m/s, over the ground %.1f, up %.1f; turned %.1f degrees)",
                                   names[i], now_on ? "begins" : "ends", s->speed, s->ground, s->vy, turned * 57.2958f);
        }
    }
}

/* (view_host.c, each first-person update of the view: turned, the radians its heading turned) */
static void oot_vr_comfort_frame(Ctx *c, int64_t now, float turned) {
    (void)c;
    const int on = rx_option(1033, 0);
    if (!on) {
        if (oot_vr_comfort.have) memset(&oot_vr_comfort, 0, sizeof oot_vr_comfort);   /* (every row Off: nothing kept) */
        return;
    }
    float pos[3];
    for (int k = 0; k < 3; k++) {
        const uint32_t bits = __atomic_load_n(&oot_vr_player_xyz[k], __ATOMIC_RELAXED);
        memcpy(&pos[k], &bits, sizeof pos[k]);
    }
    float cause[4];
    const int64_t stick = __atomic_load_n(&oot_vr_left_stick_seen, __ATOMIC_ACQUIRE);
    const int64_t seen = __atomic_load_n(&oot_vr_player_seen, __ATOMIC_ACQUIRE);
    oot_vr_comfort_step(&oot_vr_comfort, now, turned, stick, seen, pos, on, cause);
    rx_vr_comfort(cause);
    {   /* (research, bounded: what the rules read, every 2 s, for tuning them in the headset) */
        static int64_t said;
        static unsigned lines;
        if (lines < 60 && now - said > 2000000000LL) {
            said = now, lines++;
            hos_log("vr: comfort: Link at %.2f %.2f %.2f, read %.0f ms ago; speed %.1f m/s (over the ground %.1f, up %.1f); "
                    "the left stick %s; air readings %d %d", pos[0], pos[1], pos[2], seen ? (now - seen) * 1e-6 : -1.0,
                    oot_vr_comfort.speed, oot_vr_comfort.ground, oot_vr_comfort.vy,
                    stick && now - stick < kVrComfortStickNs ? "pushed" : "at rest", oot_vr_comfort.falling, oot_vr_comfort.fast_down);
        }
    }
}

/* (view_host.c, each update of a cinematic shown from the director's camera or a theatre seat: the vantage's eye and its
   level heading's back) the camera's own motion, as if it were Link's with the stick at rest. A cut (the camera jumping)
   reads as a warp, and its turn as none. */
static void oot_vr_comfort_vantage(int64_t now, const float eye[3], const float back[3]) {
    static float last_eye[3], last_yaw;
    static int64_t last_at;
    const int on = rx_option(1033, 0);
    const float yaw = atan2f(back[0], back[2]);
    const float dt = last_at && now > last_at ? (now - last_at) * 1e-9f : 0;
    const float jump = sqrtf((eye[0] - last_eye[0]) * (eye[0] - last_eye[0]) + (eye[1] - last_eye[1]) * (eye[1] - last_eye[1]) +
                             (eye[2] - last_eye[2]) * (eye[2] - last_eye[2]));
    const int cut = !(dt > 0 && dt < .25f) || jump > kVrComfortWarp * dt;
    const float turned = cut ? 0 : remainderf(yaw - last_yaw, 6.283185307f);
    memcpy(last_eye, eye, sizeof last_eye), last_yaw = yaw, last_at = now;
    if (!on) return;
    float cause[4];
    oot_vr_comfort_step(&oot_vr_comfort, now, turned, 0, now, eye, on, cause);
    rx_vr_comfort(cause);
}
