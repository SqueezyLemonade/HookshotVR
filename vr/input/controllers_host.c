/* Hookshot VR: The VR controllers as an ordinary, remappable gamepad: buttons, sticks, grips and triggers into the
   game's pad (Controller mapping: the VR menu's Body, Controls, which opens Hookshot's Controls page).

   A piece of host/nn_hid.c, included there at its VR HOOK (it uses that file's helpers). Not compiled on its own. */
/* ---- (OoT VR) Quest/OpenXR controllers as an ordinary remappable gamepad ----------------------------------------- */
enum {
    VR_BTN_LOWER = 1, VR_BTN_UPPER = 2, VR_BTN_STICK = 4, VR_BTN_MENU = 8,
};
typedef struct {
    int valid;
    float grip_q[4], grip_p[3], aim_q[4], aim_p[3], lin_vel[3], ang_vel[3];
    float trigger, squeeze, stick[2];
    uint32_t buttons;
} HookshotVrHand;
typedef struct { uint32_t seq; HookshotVrHand hand[2]; } HookshotVrHands;
int rx_vr_hands(HookshotVrHands *out);

static int16_t vr_pad_axis(float x) {
    if (!isfinite(x)) return 0;
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    return (int16_t)lrintf(x * 32767.0f);
}

/* (OoT VR, first person) The right stick pushed up jumps: it presses the pad button the left controller's upper button
   presses (Y), so the game's own controls and any remapping decide what it does. Only a clear push up counts: the stick
   at least kVrJumpPush up and within about 30 degrees of straight up. Turning (left and right) never jumps: once the
   stick has gone sideways it must come back toward the middle first, as it must after every jump, so one push is one
   jump. The button is held while the stick stays up, at least kVrJumpHoldNs so the game sees the press.
   Returns whether the jump button is down. */
#define kVrJumpPush 0.7f
#define kVrJumpSideways 0.5f      /* (a stick this far left or right is turning) */
#define kVrJumpCentre 0.3f        /* (back within this of the middle: ready again) */
#define kVrJumpHoldNs 100000000LL
static int vr_stick_jump(float x, float y, int64_t now) {
    static int ready = 1, held;
    static int64_t until;
    if (x * x + y * y < kVrJumpCentre * kVrJumpCentre) ready = 1;
    if (fabsf(x) > kVrJumpSideways) ready = 0;   /* (turning) */
    if (ready && y > kVrJumpPush && fabsf(x) < y * 0.577f) {   /* (tan 30 degrees) */
        ready = 0, held = 1, until = now + kVrJumpHoldNs;
    } else if (held && y < kVrJumpPush - 0.2f && now >= until) {
        held = 0;
    }
    return held;
}

/* (OoT VR, first person, VR tab's Sword from the back) The sword taken from behind the shoulder by either hand, swung to
   attack, let go to put away (the player's design, 17 Nov; Link is left-handed, the player need not be).
   - Take: a grip squeezed with that hand behind its own shoulder (vr_sword_reach) takes the sword. The game is not told:
     the sword shows in that VR hand (vr::sword_hand, vr/items/gear/held_gear.inc) and the shield in the other.
   - Sword hits: Blade (VR tab, option 1005, the default): no button at all. The take asks the game's own draw (no attack),
     and the VR blade's own swing and contact set the game's attack sensor up (vr/items/sword/blade_hits.inc). Button: as
     follows.
   - Swing (VR tab Sword attacks: Swing, or Swing or trigger): the holding hand moving faster than the swing speed (VR tab
     Swing sensitivity, vr_swing_speed) presses the
     attack button (the left controller's lower button, the pad's WEST: it draws the sword in the game and slashes, found
     headless 17 Nov), once per swing: the hand must slow below kVrSwingRest and kVrSwingGapNs pass before the next. The
     first swing may come straight from the take, the pull from the shoulder itself (headset 18 Nov: the swings logged as
     child Link were 2.4-3.4 m/s, barely over the old 2.4 m/s, and the first one often missed: drawing with the button
     first was the way around it).
   - Trigger (Hold trigger, or Swing or trigger): while that hand's trigger is held, the attack is pressed again every
     kVrSwingGapNs as long as the hand moves faster than kVrSwingRest, so the game's slash (its sensor on only after the
     wind-up, by when a quick VR swing has passed the enemy, headset 18 Nov) is already live when the blade gets there.
   - Put away: letting go of the grip; if the game has the sword out, the game's own put-away call
     (vr/items/gear/put_away_stow.inc), never its button: SOUTH rolled while Link ran (headset 18 Nov) and talks or opens
     beside someone. Meanwhile the sword stays hidden from the VR hand. Taken again before it is away: the put-away is
     withdrawn; if the game puts it away all the same, the next swing's attack button draws it again.
   The hand's speed is measured from its tracked position (not every runtime reports velocity). */
#define kVrSwingRest 1.0f
#define kVrPressNs 100000000LL
#define kVrSwingGapNs 300000000LL
typedef struct { uint32_t seq; int active, mode, move_head, level, height_cm, body_depth_cm; float q[4], p[3]; float lean,
                 tan_half_x, tan_half_y; int detail, foliage; } VrSwordHead;   /* (HookshotVrHead, vr/vk_vr.h) */
int rx_vr_head(VrSwordHead *out);
void rx_vr_sword_hand(int hand);
int hos_vr_sword_drawn(void);
int hos_vr_sword_draw(void);   /* (vr/items/sword/blade_hits.inc: the game's own draw asked, made on Link's next update) */
void hos_vr_sword_trigger(int held);   /* (vr/items/sword/blade_hits.inc: the spin's charge, held) */
void rx_vr_rumble(int hand, float amplitude, float seconds);   /* (vr/headset/bridges_host.c) */
#define kVrChargePulseNs 100000000LL   /* (the charge's buzz: one pulse this long, again and again while held) */
#define kVrDrawGapNs 300000000LL   /* (Sword hits: Blade: the draw asked again this often while it is not out) */
#define kVrDrawTries 2

/* is this hand behind its own shoulder: near the head's level, at or behind the face, on its own side (head frame:
   OpenXR x right, y up, z back) */
static int vr_sword_reach(const HookshotVrHand *h, const VrSwordHead *head, int hand) {
    const float *q = head->q;   /* (the head's forward, -z, flattened: the body's way) */
    float fx = -(2 * (q[0] * q[2] + q[3] * q[1])), fz = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
    const float n = sqrtf(fx * fx + fz * fz);
    if (n < 1e-3f) return 0;
    fx /= n, fz /= n;
    const float d[3] = {h->grip_p[0] - head->p[0], h->grip_p[1] - head->p[1], h->grip_p[2] - head->p[2]};
    const float ahead = d[0] * fx + d[2] * fz, side = -d[0] * fz + d[2] * fx, up = d[1];
    const float own = hand ? side : -side;   /* (toward this hand's own side) */
    return up > -0.30f && up < 0.40f && ahead < 0.08f && own > -0.12f && own < 0.45f;
}

/* a tracked point's speed (m/s), from its position between reads, smoothed over about 30 ms */
typedef struct { int have; int64_t then; float was[3], v; } VrSpeed;
static float vr_speed_step(VrSpeed *s, int valid, const float p[3], int64_t now) {
    if (!valid) { s->have = 0; return 0; }
    const float dt = (float)(now - s->then) * 1e-9f;
    if (s->have && dt > 0.002f && dt < 0.1f) {
        const float d[3] = {p[0] - s->was[0], p[1] - s->was[1], p[2] - s->was[2]};
        s->v += (sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / dt - s->v) * (dt / (dt + 0.03f));
    }
    if (!s->have || dt > 0.002f) memcpy(s->was, p, sizeof s->was), s->then = now, s->have = 1;
    return s->v;
}
/* the hand's speed (m/s), from its grip position */
static float vr_hand_speed(const HookshotVrHand *h, int hand, int64_t now) {
    static VrSpeed s[2];
    return vr_speed_step(&s[hand], h->valid, h->grip_p, now);
}
/* the speed (m/s) of the held blade's middle, kVrBladeMiddle along it from the grip (the blade leaned forward by the VR tab's
   Sword angle, as vr/items/sword/sword_host.c vr_held_blade_pose places it: in the grip's frame (0, -sin a, -cos a)). A swing
   is mostly the wrist turning: the blade moves much faster than the hand that holds it */
#define kVrBladeMiddle 0.35f
static float vr_blade_speed(const HookshotVrHand *h, int hand, int64_t now) {
    static VrSpeed s[2];
    const float a = rx_option(914, 35) * 0.0174533f, b[3] = {0, -sinf(a) * kVrBladeMiddle, -cosf(a) * kVrBladeMiddle};
    const float *q = h->grip_q;   /* (x y z w: b turned by q, b + 2w (q x b) + 2 q x (q x b)) */
    const float c[3] = {q[1] * b[2] - q[2] * b[1], q[2] * b[0] - q[0] * b[2], q[0] * b[1] - q[1] * b[0]};
    const float cc[3] = {q[1] * c[2] - q[2] * c[1], q[2] * c[0] - q[0] * c[2], q[0] * c[1] - q[1] * c[0]};
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = h->grip_p[k] + b[k] + 2 * (q[3] * c[k] + cc[k]);
    return vr_speed_step(&s[hand], h->valid, p, now);
}

/* ---- Putting a weapon away (the sword, the Deku Stick): the game's own call, never its button --------------------------- */
#include "../items/gear/put_away_stow.inc"

/* ---- A swing of a held weapon (the sword, the Deku Stick): the attack button (WEST) pressed, as the VR tab's Sword attacks
   says: a swing faster than the hand's swing speed (once per swing: the hand slows below kVrSwingRest before the next), or
   (Hold trigger) the hand's trigger held while it moves faster than kVrSwingRest, again every kVrSwingGapNs.
   VR tab Swing detection (option 1001): Hand, as above (one press per swing, from the hand's speed alone); Blade, the
   default: a swing also starts when the blade's middle (vr_blade_speed) passes the blade's swing speed, and while the swing
   goes on fast (the hand past kVrSwingOnFraction of its swing speed, or the blade of its) the attack is pressed again every
   kVrSwingGapNs, at most kVrSwingRepeats times a swing.
   VR tab Swing sensitivity (option 1003): the two swing speeds, vr_swing_speed. Headset 19 Nov: hand 1.8 / blade 2.8 m/s was
   "very sensitive", 2.0 / 3.4 "a little bit better", still sensitive; the repeats then went on until the hand nearly
   stopped (1.0 / 1.6 m/s), so ordinary motion after a swing kept attacking: now only while it is still a fast swing.
   Why (headset 18 Nov, child Link: real swings did not hit, the attack button did): the hits themselves land where the VR
   blade is (headless 18 Nov, RX_VR_SWORD_TEST: the sword actor swept through a Deku Baba 3.9 m behind Link killed it in
   about 1.8 s of Y presses; the control, never placed, left it alive all run), so a swing missed because of its one press:
   given at the swing's start, the game's slash switches the blade's sensor on only after its wind-up, by when a quick VR
   swing has passed the enemy, and a swing that is mostly the wrist (the blade fast, the hand slow) was never counted
   ("vr: sword: a move peaked at ... no attack" now says how often). */
#define kVrBladeRest 1.6f
#define kVrSwingOnFraction 0.6f   /* (the swing goes on while this fast, of its swing speed) */
#define kVrSwingRepeats 2         /* (attacks again at most this many times a swing) */
/* the swing speeds (m/s: the hand's, the blade middle's) for the VR tab's Swing sensitivity: 0 Low, 1 Medium, 2 High */
static void vr_swing_speed(float *hand, float *blade) {
    static const float kHand[3] = {2.5f, 2.2f, 2.0f}, kBlade[3] = {4.8f, 4.0f, 3.4f};
    int k = rx_option(1003, 1);
    if (k < 0 || k > 2) k = 1;
    *hand = kHand[k], *blade = kBlade[k];
}
typedef struct { int ready, held_was, attacked, swinging; int64_t attack_until, last_attack; float peak, blade_peak; } VrSwing;   /* (swinging: the repeats left) */
/* VR tab Sword hits (option 1005): 1 Blade, the game's sensor set up by the VR blade (vr/items/sword/blade_hits.inc), no
   attack button; 0 Button, as below */
static int vr_sword_hits_blade(void) { return rx_option(1005, 1) == 1; }
static void vr_swing_step(VrSwing *s, const HookshotVrHand *h, int hand, int free, int64_t now, const char *what) {
    if (vr_sword_hits_blade()) { s->attack_until = 0, s->peak = s->blade_peak = 0, s->attacked = 0; return; }
    const float v = vr_hand_speed(h, hand, now), blade = vr_blade_speed(h, hand, now);
    const int mode = rx_option(915, 2);   /* (0 swing, 1 trigger held, 2 either) */
    const int by_blade = rx_option(1001, 1);   /* (Swing detection: 0 Hand, 1 Blade) */
    const int held = mode != 0 && h->trigger > 0.55f;
    float hand_speed, blade_speed;
    vr_swing_speed(&hand_speed, &blade_speed);
    const int moving = v >= kVrSwingRest || (by_blade && blade >= kVrBladeRest);
    const int fast = v > hand_speed || (by_blade && blade > blade_speed);
    const int going_on = v > hand_speed * kVrSwingOnFraction || blade > blade_speed * kVrSwingOnFraction;
    /* (each move past rest: its peak, logged when it ends without an attack (bounded), so a headset log shows the swings that
       were too slow to count) */
    if (moving) s->peak = fmaxf(s->peak, v), s->blade_peak = fmaxf(s->blade_peak, blade);
    else if (s->peak > 0) {
        static unsigned told;
        if (!s->attacked && told < 300)
            told++, hos_log("vr: %s: a move peaked at %.2f m/s (the blade's middle %.2f m/s): no attack", what, s->peak, s->blade_peak);
        s->peak = s->blade_peak = 0, s->attacked = 0;
    }
    if (!moving) s->ready = 1;
    if (!moving || !going_on) s->swinging = 0;   /* (slowed down: the swing is over, even before the hand rests) */
    if (free && now - s->last_attack > kVrSwingGapNs) {
        if (mode != 1 && s->ready && fast) {
            s->ready = 0, s->swinging = by_blade ? kVrSwingRepeats : 0, s->last_attack = now, s->attack_until = now + kVrPressNs;
            s->attacked = 1;
            hos_log("vr: %s: swing at %.2f m/s (the blade's middle %.2f m/s)", what, v, blade);
        } else if (mode != 1 && s->swinging && going_on) {   /* (Blade: the swing goes on, and so does the attack) */
            s->last_attack = now, s->attack_until = now + kVrPressNs, s->swinging--;
            static unsigned told;
            if (told++ < 200) hos_log("vr: %s: the swing goes on (%.2f m/s, the blade's middle %.2f m/s): attack again", what, v, blade);
        } else if (held && v > kVrSwingRest) {
            s->ready = 0, s->last_attack = now, s->attack_until = now + kVrPressNs, s->attacked = 1;
            if (!s->held_was) hos_log("vr: %s: trigger attack at %.2f m/s", what, v);
            s->held_was = 1;
        }
    }
    if (!held) s->held_was = 0;
}

static volatile int vr_sword_holder = -1;
int hos_vr_sword_holder(void) { return __atomic_load_n(&vr_sword_holder, __ATOMIC_RELAXED); }   /* (vr/items/sword/sword_host.c) */
/* ---- Guarding with the sword in hand (VR tab Guard with the sword out, option 1000) -----------------------------------
   While a VR hand holds the sword, the other hand holds the shield (vr/items/gear/held_gear.inc), and its grip or its
   trigger raises it: the left trigger's pad input is pressed (the game's ZL: the shield, and the lock-on when a target is
   near, in the game's own controls and the player's). That hand is the sword's then (hos_vr_sword_claims): its grip takes
   no bow, slingshot, Boomerang, bottle or Deku Stick, and its own trigger and shoulder button do not reach the game.
   Why (headset 18 Nov, child Link, the slingshot on ZR): with the sword in the right hand, the left grip took the
   slingshot out (the game saw ZL+ZR, "action state 0x408 -> 0x2000041"), which put the sword and shield away; and with the
   sword in the left hand nothing could guard at all (its trigger is the sword's in Sword attacks: Hold trigger). Off: the
   hands as before, the guard only on the left trigger. */
static volatile int vr_sword_guard_hand = -1;
static int vr_sword_guard_of(int holder) { return holder >= 0 && rx_option(1000, 1) ? 1 - holder : -1; }
/* (vr/items/bow/bow_host.c) this hand's grip is the sword's: it holds the sword, or it is behind its shoulder with the VR
   tab's row on, where a grip takes the sword and not the bow or slingshot that is selected (headset 18 Nov: the grip took
   both, or with the slingshot selected the sword could not be taken at all), or it holds the shield beside the sword */
static volatile int vr_sword_shoulder[2];
int hos_vr_sword_claims(int hand) {
    return hand >= 0 && hand < 2 && (hos_vr_sword_holder() == hand || __atomic_load_n(&vr_sword_shoulder[hand], __ATOMIC_RELAXED) ||
                                     __atomic_load_n(&vr_sword_guard_hand, __ATOMIC_RELAXED) == hand);
}

/* the sword's state for this read: which hand holds it (-1 none), and the button it presses (attack, WEST) into
   *raw. on: first person with the VR tab's row on; free: not in a menu; busy: the hand that holds the bow or
   slingshot (-1 none), which does not take the sword. Returns the holding hand. */
static int vr_sword(const HookshotVrHand hands[2], int on, int free, int busy, uint64_t *raw, int64_t now) {
    static int holder = -1, grip_was[2];
    static VrSwing swing = {1, 0, 0, 0};
    static VrStow away;
    VrSwordHead head;
    const int tracked = on && rx_vr_head(&head) && head.active;
    for (int k = 0; k < 2; k++) {
        const int grip = hands[k].valid && hands[k].squeeze > 0.55f;
        const int reach = tracked && free && k != busy && hands[k].valid && vr_sword_reach(&hands[k], &head, k);
        __atomic_store_n(&vr_sword_shoulder[k], reach, __ATOMIC_RELAXED);
        if (holder < 0 && reach && grip && !grip_was[k]) {
            holder = k, swing.ready = 1;
            vr_stow_cancel(&away, "sword");   /* (taken again before it was away) */
            hos_log("vr: sword: taken by the %s hand", k ? "right" : "left");
        }
        if (holder == k && (!grip || !on)) {
            holder = -1;
            if (hos_vr_sword_drawn()) vr_stow_start(&away, now);
            hos_log("vr: sword: put away%s", hos_vr_sword_drawn() ? " (the game's too)" : "");
        }
        grip_was[k] = grip;
    }
    if (holder >= 0) vr_swing_step(&swing, &hands[holder], holder, free, now, "sword");
    /* (Sword hits: Blade: the holding hand's trigger, the spin's charge, vr/items/sword/blade_hits.inc) */
    const int charging = holder >= 0 && free && vr_sword_hits_blade() && hands[holder].trigger > 0.55f;
    hos_vr_sword_trigger(charging);
    {   /* (the charge felt in the holding hand, the user's ask, 22 Nov: light, then medium at stage 1, strong at stage 2; by
           the time held until the game's own charge stage is read: RX_VR_CHARGE_BUZZ=<stage 1 s>,<stage 2 s>) */
        static int64_t since, pulsed;
        static float t1 = -1, t2;
        if (t1 < 0) {
            const char *e = vr_research_getenv("RX_VR_CHARGE_BUZZ");
            t1 = 1.0f, t2 = 2.0f;
            if (e) sscanf(e, "%f,%f", &t1, &t2);
            else t1 = 1e9f;   /* (off unless set: the user, 22 Nov, until its timing follows the game's own stage) */
        }
        if (!charging) since = 0;
        else {
            if (!since) since = now, pulsed = 0;
            const float held = (float)(now - since) * 1e-9f;
            if (t1 < 1e8f && now - pulsed >= kVrChargePulseNs && rx_option(1006, 1)) {   /* (with Hit buzz on) */
                pulsed = now;
                rx_vr_rumble(holder, held >= t2 ? 0.9f : held >= t1 ? 0.5f : 0.15f, (float)kVrChargePulseNs * 1e-9f);
            }
        }
    }
    {   /* (Sword hits: Blade: the game's sword drawn at the take, while it is not, by the game's own draw call (no button:
           the player's rule, a pressed attack button conflicted with the game's own uses of it). Hits land only while Link
           stands in his drawn stance (action state 0x410, headset 22 Nov 04:46: after an X press the VR strokes hit the
           dummy; let go and taken again they did not), and this call does not put him there yet: drawn, then away two
           frames later. The call that does is being looked for) */
        static int64_t draw_at;
        static int draws;
        if (holder < 0) draws = 0;   /* (per take) */
        else if (free && vr_sword_hits_blade() && !hos_vr_sword_drawn() && draws < kVrDrawTries && now - draw_at > kVrDrawGapNs) {
            draws++, draw_at = now, hos_vr_sword_draw();
            hos_log("vr: sword: the game's own draw asked (%d of %d)", draws, kVrDrawTries);
        }
    }
    vr_stow_step(&away, hos_vr_sword_drawn(), now, "sword");
    if (now < swing.attack_until) *raw |= 1ULL << HOS_PAD_WEST;
    /* (Sword hits: Blade: while the sword hand's trigger is held, the game sees its attack button held: its own charge, its
       glow and stage 2 build only so (headset 22 Nov 07:21: the charge asked by request alone stopped at stage 1, the
       game's button in third person reached stage 2). The player's own press, mapped, only while held: the user's call,
       22 Nov; driving the charge by a call is left for later research. Link's slash from it is refused in flight
       (blade_hits.inc: 0x70 made his movement), and let go his spin is asked (0x77)) */
    if (charging) *raw |= 1ULL << HOS_PAD_WEST;
    __atomic_store_n(&vr_sword_holder, holder, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_sword_guard_hand, vr_sword_guard_of(holder), __ATOMIC_RELAXED);
    rx_vr_sword_hand(holder >= 0 ? holder : vr_stow_busy(&away) ? -2 : -1);
    return holder;
}

/* (OoT VR, first person, VR tab's Deku Sticks in the hands, option 930) child Link's Deku Stick in a VR hand, the game's own
   stick (vr/items/deku_stick/deku_stick_host.c places its actor, with its attack and fire sensors, on that hand; its model
   is drawn there, vr/items/deku_stick/held_stick.inc). With the stick selected (the VR radial menu, or the game's own):
   - Take: a grip, either hand (grip holds, as the bow, hookshot and slingshot), not one behind the shoulder that is the
     sword's (Sword from the back) or one near a bomb. If the game does not have the stick out, its item button (ZR: the
     right trigger's pad button) is held until it is out (headset log 18 Nov: ZR, then 0x8 -> 0x408), at most
     kVrStickTakeNs.
   - Swing: as the sword's (vr_swing_step, the VR tab's Sword attacks: Swing, Hold trigger, or either), the stick's own item
     button (ZR) while the game has the stick out, let go between swings. Never WEST: with the stick out, WEST puts it away
     and draws the sword and shield (headset log 18 Nov: every swing at WEST, then the stick gone, the sword drawn).
   - Put away: letting go of the grip; the game's own put-away call as the sword's (put_away_stow.inc), never its SOUTH,
     the stick hidden from the VR hand meanwhile (a lit stick's fire goes with it; the log says if not: "put away still
     BURNING"). Taken again before it is away: withdrawn; if the game puts it away all the same, ZR takes it out again.
   - The real right trigger (ZR) does not reach the game while the stick is selected: a grip takes the stick; the holding
     hand's trigger is the attack's in Hold trigger (as the sword's).
   The renderer is told the holding hand (vr::stick_hand: 0 left, 1 right, -1 none, -2 let go and being put away) through
   rx_vr_stick_hand (vr/headset/bridges_host.c), when it changes and every kVrStickTellNs. */
#define kVrStickTakeNs 700000000LL
#define kVrStickTellNs 100000000LL      /* (told again this often: the renderer forgets it after 250 ms) */
void rx_vr_stick_hand(int hand);       /* (vr/headset/bridges_host.c) */
int hos_vr_stick_out(void);             /* (vr/items/deku_stick/deku_stick_host.c: the game has the stick out) */
int hos_vr_stick_draw(void);            /* (vr/items/sword/blade_hits.inc: the game's own draw of the stick's slot asked) */
static volatile int vr_stick_holder_now = -1;
int hos_vr_stick_holder(void) { return __atomic_load_n(&vr_stick_holder_now, __ATOMIC_RELAXED); }
static void vr_stick_tell(int hand, int64_t now) {
    static int told = -100;
    static int64_t told_at;
    if (hand == told && now - told_at < kVrStickTellNs) return;
    told = hand, told_at = now;
    rx_vr_stick_hand(hand);
}
/* the stick's state for this read: which hand holds it (-1 none), and the buttons it presses (ZR, take and attack) into
   *raw. on: first person, the VR tab's row on, the stick selected; free: not in a menu. Returns the holding hand. */
static int vr_stick(const HookshotVrHand hands[2], int on, int free, uint64_t *raw, int64_t now) {
    int hos_vr_bomb_in_reach(int hand);   /* (vr/items/bomb/bomb_host.c: a grip near a bomb takes the bomb) */
    int hos_vr_ocarina_claims(int hand);  /* (vr/items/ocarina/ocarina_host.c: a grip at the belt ocarina takes it) */
    static int holder = -1, grip_was[2];
    static int64_t take_until;
    static VrSwing swing = {1, 0, 0, 0};
    static VrStow away;
    static int regrip;   /* (taken again while being put away: the game may still put it away just after) */
    const int out = hos_vr_stick_out();
    for (int k = 0; k < 2; k++) {
        const int grip = hands[k].valid && hands[k].squeeze > (grip_was[k] ? 0.35f : 0.55f);
        if (holder < 0 && on && free && grip && !grip_was[k] && hos_vr_sword_holder() < 0 && !hos_vr_sword_claims(k) &&
            !hos_vr_bomb_in_reach(k) && !hos_vr_ocarina_claims(k)) {
            holder = k, swing.ready = 1, regrip = vr_stow_busy(&away);
            vr_stow_cancel(&away, "deku stick");
            take_until = out ? 0 : now + kVrStickTakeNs;
            hos_log("vr: deku stick: taken by the %s hand%s", k ? "right" : "left",
                    out ? " (the game's already out)" : ", the game's ZR held until it is out");
        }
        if (holder == k && (!grip || !on)) {
            holder = -1, take_until = 0;
            if (out) vr_stow_start(&away, now);
            hos_log("vr: deku stick: let go%s", out ? ": put away" : on ? "" : " (not selected or not first person)");
        }
        grip_was[k] = grip;
    }
    if (regrip && holder >= 0 && !take_until && vr_stow_regrip_lost(&away, out, now)) {   /* (the game made the last request) */
        take_until = now + kVrStickTakeNs, regrip = 0;
        hos_log("vr: deku stick: put away just as it was taken again: the game's ZR takes it out again");
    } else if (regrip && (holder < 0 || !vr_stow_regrip_lost(&away, 0, now))) regrip = 0;
    if (take_until && (out || now >= take_until)) {
        if (!out) hos_log("vr: deku stick: did not come out in %d ms (none left?)", (int)(kVrStickTakeNs / 1000000));
        take_until = 0;
        swing.last_attack = now;   /* (the take's ZR let go for a swing gap first: a swing right away is a new press) */
    }
    if (holder >= 0 && out && !take_until) vr_swing_step(&swing, &hands[holder], holder, free, now, "deku stick");
    else swing.attack_until = 0;   /* (no attack before the stick is out: that ZR would be the take's) */
    vr_stow_step(&away, out, now, "deku stick");
    if (on) *raw &= ~(1ULL << HOS_PAD_RTRIGGER);   /* (the real ZR: the grips take the stick) */
    if (holder >= 0 && rx_option(915, 2) != 0) *raw &= ~(1ULL << (holder ? HOS_PAD_RTRIGGER : HOS_PAD_LTRIGGER));
    {   /* (Sword hits: Blade: the stick taken out by the game's own draw of its slot, no button; RX_VR_STICK_TAKE_ZR=1 the
           item button as before, a test) */
        static int by_zr = -1;
        static int64_t asked_at;
        if (by_zr < 0) { const char *e = vr_research_getenv("RX_VR_STICK_TAKE_ZR"); by_zr = e && atoi(e) != 0; }
        if (take_until && vr_sword_hits_blade() && !by_zr) {
            if (now - asked_at > kVrDrawGapNs) asked_at = now, hos_vr_stick_draw();
        } else if (take_until) *raw |= 1ULL << HOS_PAD_RTRIGGER;
    }
    if (now < swing.attack_until) *raw |= 1ULL << HOS_PAD_RTRIGGER;
    __atomic_store_n(&vr_stick_holder_now, holder, __ATOMIC_RELAXED);
    vr_stick_tell(holder >= 0 ? holder : vr_stow_busy(&away) ? -2 : -1, now);
    return holder;
}
#include "../items/ocarina/ocarina_host.c"   /* (the ocarina in the VR hands: vr_ocarina) */
#include "../items/bottle/bottle_controls.inc"   /* (a bottle in the VR hands: vr_bottle) */

/* The left Menu button held kVrMenuHoldNs opens the VR menu, or closes it (vr/menu/vr_menu.inc). Let go sooner, with no
   chord meanwhile (Menu+LS, Menu+RS, Menu+X), it is a tap: the game's Plus, sent once it is let go. A hold that has opened or
   closed the menu sends nothing when it is let go, and a chord never opens it, however long it is held. */
#define kVrMenuHoldNs 500000000LL
typedef struct { int was, chord, fired; int64_t since; } VrMenuHold;
/* One reading of the Menu button (down), whether a chord button is pressed with it now (chord): 1 the hold has just reached
   kVrMenuHoldNs (open or close the VR menu), 2 a tap has just ended (the game's Plus), else 0 */
static int vr_menu_hold_step(VrMenuHold *m, int down, int chord, int64_t now) {
    int out = 0;
    if (down && !m->was) m->since = now, m->chord = 0, m->fired = 0;
    if (down && chord) m->chord = 1;
    if (down && !m->chord && !m->fired && now - m->since >= kVrMenuHoldNs) m->fired = 1, out = 1;
    if (!down && m->was && !m->chord && !m->fired) out = 2;
    m->was = down;
    return out;
}

/* Quest layout:
   - face buttons, sticks, triggers and grips are the corresponding gamepad controls;
   - tap the left Menu button for Plus; hold it half a second for the VR menu; Menu+LS is Minus; Menu+RS opens Hookshot's
     Insert menu; Menu+X marks a place for the dev map editor (vr/devmap/devmap_host.c);
   - in Hookshot's menu, the left stick is the D-pad; in the game, hold LS and move the right stick for D-pad directions.
   Menu is emitted on release so a chord never leaks a Plus press into the game. While the VR menu is open, the controllers
   are its own: the hands come here let go (vr/menu/vr_menu_layer.inc panel_mask), so Link stands still. */
int rx_vr_menu(int toggle);
static void vr_pad_read(uint64_t *raw, int32_t stick[4], int zone) {
    HookshotVrHands v;
    if (!rx_vr_hands(&v)) {
        vr_ocarina_hands_lost();   /* (vr/items/ocarina/ocarina_host.c: its buttons let go meanwhile) */
        return;
    }
    const HookshotVrHand *l = &v.hand[0], *r = &v.hand[1];
    const int lb = l->buttons, rb = r->buttons;
    const int menu = (lb & VR_BTN_MENU) != 0;
    const int ls = (lb & VR_BTN_STICK) != 0, rs = (rb & VR_BTN_STICK) != 0;
    static VrMenuHold menu_hold;
    static int64_t plus_until;
    const int64_t now = hos_now_ns();

    /* (first person, VR tab's Jump on right stick up: the stick jumps, and the upper button is left free) */
    int hos_oot_vr_first_person(void);
    const int stick_jump = rx_option(910, 0) && hos_oot_vr_first_person() && !rx_option(1, 0);
    if (lb & VR_BTN_LOWER) *raw |= 1ULL << HOS_PAD_WEST;    /* X */
    if ((lb & VR_BTN_UPPER) && !stick_jump) *raw |= 1ULL << HOS_PAD_NORTH;   /* Y */
    if (rb & VR_BTN_LOWER) *raw |= 1ULL << HOS_PAD_SOUTH;   /* A */
    if (rb & VR_BTN_UPPER) *raw |= 1ULL << HOS_PAD_EAST;    /* B */
    if (l->trigger > 0.55f) *raw |= 1ULL << HOS_PAD_LTRIGGER;
    if (r->trigger > 0.55f) *raw |= 1ULL << HOS_PAD_RTRIGGER;
    /* (first person, while the bow owns them: host/hos_main.c hos_oot_vr_bow_grips) the grips hold the bow and draw its
       string; as shoulder buttons they lock on (ZL here), whose strafing stance bounced the arms while drawing */
    int hos_oot_vr_bow_grips(void), hos_vr_bomb_grips(void), hos_vr_hook_grips(void), hos_vr_stick_selected(void);
    int hos_vr_boomerang_grips(void);   /* (vr/items/boomerang: bit 0 the left grip is the boomerang's, bit 1 the right) */
    /* (first person, VR tab's Deku Sticks in the hands, the stick selected: the grips take it, vr_stick) */
    const int stick_on = rx_option(930, 0) && hos_oot_vr_first_person() && hos_vr_stick_selected();
    const int grips_owned = hos_oot_vr_bow_grips() || hos_vr_bomb_grips() || hos_vr_hook_grips();   /* (the bow's, the bombs', the hookshot's) */
    /* (first person, VR tab's Sword from the back: a grip behind the shoulder takes the sword; that grip is the sword's then) */
    int hos_vr_radial_open(void), hos_oot_vr_bow_hand(void);
    const int free = !menu && !rx_option(1, 0) && !hos_vr_radial_open();
    /* (not while a VR hand holds the Deku Stick: the game has one weapon out at a time) */
    const int sword_hand = vr_sword(v.hand, rx_option(913, 0) && hos_oot_vr_first_person() && hos_vr_stick_holder() < 0, free,
                                    hos_oot_vr_bow_hand(), raw, now);
    /* (Sword attacks with the trigger: the sword hand's trigger is the sword's, not the game's lock-on or item) */
    if (sword_hand >= 0 && rx_option(915, 2) != 0) *raw &= ~(1ULL << (sword_hand ? HOS_PAD_RTRIGGER : HOS_PAD_LTRIGGER));
    /* (Guard with the sword out: the shield hand's grip or trigger is the game's ZL, the left trigger's pad input) */
    const int guard_hand = vr_sword_guard_of(sword_hand);
    if (guard_hand >= 0) {
        static int guard_was;
        const HookshotVrHand *g = &v.hand[guard_hand];
        const int guard = free && g->valid && (g->squeeze > 0.55f || g->trigger > 0.55f);
        *raw &= ~(1ULL << (guard_hand ? HOS_PAD_RTRIGGER : HOS_PAD_LTRIGGER));   /* (its own trigger: the guard's, not an item's) */
        if (guard) *raw |= 1ULL << HOS_PAD_LTRIGGER;
        if (guard != guard_was) hos_log("vr: sword: the %s hand %s the shield", guard_hand ? "right" : "left", guard ? "raises" : "lowers");
        guard_was = guard;
    }
    const int stick_hand = vr_stick(v.hand, stick_on, free, raw, now);
    /* (first person, VR tab's Ocarina in the hands: a grip at the belt takes it, held to the mouth it plays; while it plays,
       the notes own the triggers, the left controller's buttons, the grips and the right stick) */
    int ocarina_playing = 0;
    const int ocarina_hand = vr_ocarina(v.hand, rx_option(980, 0) && hos_oot_vr_first_person(), free, raw, &ocarina_playing, now);
    /* (first person, VR tab's Bottles in the hands, a bottle selected: the grips take it, vr_bottle; not while the ocarina is
       in a hand or plays) */
    const int bottle_on = rx_option(970, 0) && hos_oot_vr_first_person() && hos_vr_bottle_selected() && ocarina_hand < 0 &&
                          !ocarina_playing;
    const int bottle_hand = vr_bottle(v.hand, bottle_on, free, raw, now);
    /* (the stick or a bottle selected: a grip takes it, so no grip is a shoulder button while no hand holds it; once one
       does, the other hand's grip is its shoulder button again (lock-on, shield), as beside the sword) */
    const int stick_grip[2] = {(stick_on && stick_hand != 1) || (bottle_on && (bottle_hand != 1 || hos_vr_bottle_at_cork() == 0)),
                               (stick_on && stick_hand != 0) || (bottle_on && (bottle_hand != 0 || hos_vr_bottle_at_cork() == 1))};
    /* (the hand holding the ocarina, and both while it plays: L and R are notes there; the hands holding the Boomerang) */
    const int ocarina_grip[2] = {ocarina_playing || ocarina_hand == 0, ocarina_playing || ocarina_hand == 1};
    const int boomerang_grips = hos_vr_boomerang_grips();
    if (l->squeeze > 0.55f && !grips_owned && !stick_grip[0] && sword_hand != 0 && guard_hand != 0 && !ocarina_grip[0] &&
        !(boomerang_grips & 1))
        *raw |= 1ULL << HOS_PAD_LSHOULDER;
    if (r->squeeze > 0.55f && !grips_owned && !stick_grip[1] && sword_hand != 1 && guard_hand != 1 && !ocarina_grip[1] &&
        !(boomerang_grips & 2))
        *raw |= 1ULL << HOS_PAD_RSHOULDER;

    /* (the game's ZL, its guard, held now: the renderer turns a held shield to face forward meanwhile, VR tab Shield when
       guarding, vr/items/gear/held_gear.inc) */
    void rx_vr_shield_guard(int guarding);
    rx_vr_shield_guard((*raw >> HOS_PAD_LTRIGGER) & 1);

    int16_t a[4] = {vr_pad_axis(l->stick[0]), vr_pad_axis(l->stick[1]),
                    vr_pad_axis(r->stick[0]), vr_pad_axis(r->stick[1])};
    const int hookshot_menu = rx_option(1, 0) != 0;
    if (hookshot_menu && (abs(a[0]) > 18000 || abs(a[1]) > 18000)) {
        if (abs(a[0]) > abs(a[1])) *raw |= 1ULL << (a[0] < 0 ? HOS_PAD_LEFT : HOS_PAD_RIGHT);
        else *raw |= 1ULL << (a[1] < 0 ? HOS_PAD_DOWN : HOS_PAD_UP);
        a[0] = a[1] = 0;   /* the menu consumes the left stick as discrete navigation */
    }
    const int dpad = !hookshot_menu && !menu && !ocarina_playing && ls && (abs(a[2]) > 18000 || abs(a[3]) > 18000);
    /* (hos_vr_radial_open, vr/menu/radial_host.c: 2, the equipment menu: the right stick picks, no jump) */
    const int jump = vr_stick_jump(r->stick[0], r->stick[1], now);
    if (jump && stick_jump && !dpad && !menu && !ocarina_playing && hos_vr_radial_open() != 2) *raw |= 1ULL << HOS_PAD_NORTH;
    if (ocarina_playing) a[2] = a[3] = 0;   /* (the right stick plays notes: the view does not turn) */
    if (dpad) {
        /* (with the VR ocarina, its D-pad Left is not the chord's: the ocarina starts at the mouth, never by a stick held
           while the left stick is clicked, headset 19 Nov) */
        const int ocarina_on = rx_option(980, 0) && hos_oot_vr_first_person();
        if (abs(a[2]) > abs(a[3]) && a[2] < 0 && ocarina_on) {
            static int64_t told;
            if (now - told > 2000000000LL)
                told = now, hos_log("vr: ocarina: the D-pad chord's Left left out (the VR ocarina is on)");
        } else if (abs(a[2]) > abs(a[3])) *raw |= 1ULL << (a[2] < 0 ? HOS_PAD_LEFT : HOS_PAD_RIGHT);
        else *raw |= 1ULL << (a[3] < 0 ? HOS_PAD_DOWN : HOS_PAD_UP);
        a[2] = a[3] = 0;   /* no camera movement while choosing a D-pad direction */
    } else if (!menu) {
        /* (first person with "Stick click menus: VR radial": the stick clicks are the radial menus', read from the
           controllers themselves, vr/menu/radial_host.c; the game never gets them: through a layout the left one opened the
           game's own menu) */
        const int radial = rx_option(901, 0) == 1 && hos_oot_vr_first_person();
        if (ls && !radial) *raw |= 1ULL << HOS_PAD_LSTICK;
        if (rs && !radial) *raw |= 1ULL << HOS_PAD_RSTICK;
    }

    int chord = 0;
    if (menu) {
        if (ls) *raw |= 1ULL << HOS_PAD_BACK, chord = 1;       /* Minus */
        if (rs) *raw |= 1ULL << HOS_PAD_GUIDE, chord = 1;      /* Hookshot menu */
        {   /* Menu + X: a dev map mark, where Link stands (vr/devmap/devmap_host.c, research builds); X does not reach the
               game meanwhile */
            static int x_was;
            const int x = (lb & VR_BTN_LOWER) != 0;
#ifdef HOOKSHOT_VR_RESEARCH
            void hos_vr_devmap_mark(void);
            if (x && !x_was) hos_vr_devmap_mark(), chord = 1;
#endif
            if (x) *raw &= ~(1ULL << HOS_PAD_WEST), chord = 1;
            x_was = x;
        }
    }
    const int hold = vr_menu_hold_step(&menu_hold, menu, chord, now);
    if (hold == 1) {   /* (held long enough: the VR menu opens or closes, vr/menu/vr_menu.inc) */
        const int open = rx_vr_menu(1);
        hos_log("vr: left Menu held %d ms: the VR menu %s", (int)(kVrMenuHoldNs / 1000000), open ? "opens" : "closes");
    } else if (hold == 2 && !rx_vr_menu(0)) {
        plus_until = now + 120000000LL;                        /* a clean Plus tap (not while the VR menu is open) */
    }
    if (!menu && now < plus_until) *raw |= 1ULL << HOS_PAD_START;

    int32_t q[4];
    hos_filter_stick(a[0], a[1], zone, q);       /* (OoT VR: 2.2's paired dead zone, as its pads) */
    hos_filter_stick(a[2], a[3], zone, q + 2);
    if (rx_option(16, 0)) q[2] = -q[2];
    if (rx_option(17, 0)) q[3] = -q[3];
    stick_max(stick, q);
}
