/* Hookshot VR: the ocarina in the VR hands, first person (VR tab: Ocarina in the hands, option 980). Game side: the grip,
   holding it to the face, the game's ocarina mode, the notes and the cancel.

   A piece of vr/input/controllers_host.c (so of host/nn_hid.c), included there before vr_pad_read: it uses that file's
   hand type, head type, kVrPressNs and the sword's and stick's holders, and nn_hid.c's note wait (hos_oc_wait). Not
   compiled on its own.

   The ocarina hangs at the front left of the VR belt (vr/items/ocarina/held_ocarina.inc draws it there and says where).
   - Take: a grip closed within kVrOcarinaReach of it, either hand: the ocarina is in that VR hand (the renderer is told,
     rx_vr_ocarina_hand). Other items' grips leave that hand alone meanwhile (hos_vr_ocarina_claims: the bombs' bag, the
     bow, the hookshot and the Deku Stick ask it). Letting go puts it back on the belt.
   - Play: the holding hand brought to the mouth (within kVrOcarinaFace of it) presses the game's D-pad Left once, which
     starts the game's own ocarina mode: its event Dm_SY_Ocarina, so the game freezes the other actors and Link stands, as
     it always does. The view stays the headset's: the event only moves camera zero, which first person replaces
     (vr/camera/events_host.c keeps it so for Dm_SY_Ocarina* whatever Cutscene view says). Found headless 18 Nov.
   - Notes, while it is at the mouth and the game waits for notes (hos_oc_wait 1: Hookshot's own patch of the game's note
     wait, nx2/oot_patches.py): the game's five note buttons are L = the left trigger, R = the right trigger, X = the right
     stick up, Y = the right stick left, A = the right stick right; D-pad Up (a half step up) = the left controller's upper
     button (Y), D-pad Down = its lower button (X); the left stick bends the pitch, as it does in the game. Meanwhile the
     triggers, those two buttons and the grips are not their usual buttons, and the right stick neither turns nor jumps.
   - The ocarina's presses (the notes, D-pad Left, B) are the game's own Switch buttons, added after the controller mapping
     (hos_vr_ocarina_controls, called by host/nn_hid.c with the bow's and the menus' controls), never pad positions: the
     player's mapping moves those (headset 18 Nov, the vr-items build: no note played and nothing stopped it, because L and R
     came from the stick clicks there and A and B were swapped, so the "B" pressed was the A note).
   - Cancel (the game's B): letting go of the grip, or the ocarina held away from the mouth (past kVrOcarinaAway) for
     kVrOcarinaAwayNs, while the game waits for notes on the field (wait kinds 1 and 3; a song lesson is the lesson's). After a
     song the game ends its event itself; the ocarina has to leave the mouth before it can start again (one start each
     time it is brought up).
   - Song menu, the radial wheel and song effects: see vr_ocarina below. */
#define kVrOcarinaReach 0.15f            /* (m: a grip this close to the belt ocarina takes it) */
#define kVrOcarinaFace 0.17f             /* (m: the holding hand this close to the mouth is at the face) */
#define kVrOcarinaAway 0.27f             /* (m: and this far is away from it again) */
#define kVrOcarinaAwayNs 250000000LL     /* (away this long cancels: a wobble does not) */
#define kVrOcarinaStartNs 1500000000LL   /* (D-pad Left pressed: the game's note wait expected within this) */
#define kVrOcarinaTellNs 100000000LL     /* (the renderer told again this often: it forgets after 250 ms) */
#define kVrOcarinaNote 0.6f              /* (the right stick this far plays a note; back under kVrOcarinaNoteOff lets it go) */
#define kVrOcarinaNoteOff 0.4f
#define kVrOcarinaRadialNs 2000000000LL  /* (chosen on the radial wheel: this long for the grip to close on it) */
void rx_vr_ocarina_hand(int hand);       /* (vr/items/ocarina/ocarina_bridges_host.c) */
void rx_vr_ocarina_song(int playing);
int rx_vr_ocarina_belt(float out[3]);
int rx_vr_hands_world(float out[42]);
int hos_vr_stick_holder(void);
int hos_vr_event_playing(void);

static volatile int vr_ocarina_holder = -1;   /* (the hand holding it, -1 none; and each hand within reach of the belt one) */
static volatile int vr_ocarina_reach[2];
static volatile uint64_t vr_ocarina_switch;   /* (the game's buttons it presses now: 1 << HOS_SW_*) */
static volatile int vr_ocarina_rearm;          /* (the hands were away: the next read takes the grips as they are) */

/* (vr/input/controllers_host.c vr_pad_read, when the controllers cannot be read) the ocarina presses nothing until they
   are back: its last button word would otherwise stay added to the game's input (hos_vr_ocarina_controls), a note or the
   song menu's ZL held for as long as the hands are gone (review finding 9, 20 Nov). When they are back, the grips are
   taken as they are then: one closed meanwhile is not a new squeeze (it takes nothing), the hand holding the ocarina keeps
   it while its grip is still closed, and the notes and the song menu are read afresh from the controls. */
static void vr_ocarina_hands_lost(void) {
    if (__atomic_exchange_n(&vr_ocarina_switch, 0, __ATOMIC_RELAXED))
        hos_log("vr: ocarina: the controllers cannot be read: its buttons let go");
    __atomic_store_n(&vr_ocarina_rearm, 1, __ATOMIC_RELAXED);
}
/* (the bombs' bag, the bow, the hookshot, the Deku Stick) this hand's grip is the ocarina's: it holds it, or it is at the belt
   ocarina, where a grip takes the ocarina and nothing else */
int hos_vr_ocarina_claims(int hand) {
    return hand >= 0 && hand < 2 && (__atomic_load_n(&vr_ocarina_holder, __ATOMIC_RELAXED) == hand ||
                                     __atomic_load_n(&vr_ocarina_reach[hand], __ATOMIC_RELAXED));
}

/* the mouth, in the headset's frame (OpenXR: x right, y up, z back): a little below and ahead of the head's pose */
static void vr_ocarina_mouth(const VrSwordHead *h, float out[3]) {
    const float x = h->q[0], y = h->q[1], z = h->q[2], w = h->q[3];
    const float v[3] = {0, -0.08f, -0.09f};
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int k = 0; k < 3; k++) out[k] = h->p[k] + r[k][0] * v[0] + r[k][1] * v[1] + r[k][2] * v[2];
}

/* the right stick as a note: 0 none, else the game's button it plays, plus 1 (X up, Y left, A right; down plays nothing). Out
   past kVrOcarinaNote it is the direction's note; between that and kVrOcarinaNoteOff the note held stays, so it does not
   stutter at the edge */
static int vr_ocarina_stick_note(float x, float y) {
    static int held;
    const float m = sqrtf(x * x + y * y);
    if (m >= kVrOcarinaNote)
        held = y > fabsf(x) ? HOS_SW_X + 1 : x < -fabsf(y) ? HOS_SW_Y + 1 : x > fabsf(y) ? HOS_SW_A + 1 : 0;
    else if (m < kVrOcarinaNoteOff) held = 0;
    return held;
}

/* The VR radial wheel's ocarina (vr/menu/radial_host.c, the equipment menu): into the right hand, the wheel's, as if taken
   from the belt; held there for kVrOcarinaRadialNs for the grip to close on it, then an ordinary hold (let go: the belt) */
static volatile int vr_ocarina_radial;
void hos_vr_ocarina_take_from_radial(void) { __atomic_store_n(&vr_ocarina_radial, 1, __ATOMIC_RELAXED); }
int hos_vr_ocarina_held(void) { return __atomic_load_n(&vr_ocarina_holder, __ATOMIC_RELAXED) >= 0; }

/* The ocarina's state for this read: which hand holds it (-1 none) into the return, whether it is being played (at the
   mouth while the game waits for notes, or its song menu open: the notes own the triggers, the left controller's buttons,
   the grips and the right stick, which are taken out of *raw) into *playing; the game's buttons it presses (the start, the
   notes, the song menu's ZL, the stop) are kept for hos_vr_ocarina_controls. on: first person with the VR tab's row on;
   free: not in a menu.
   - The game's ocarina is "open" (session) from the note wait seen with the ocarina held at the mouth until the game's
     event and its note wait are over (a song plays its effect inside the same event).
   - Stopping: letting go or holding it away marks it stopped; the game's B is pressed once the game waits for notes on the
     field (at once, or when the song menu closes); brought back to the mouth first, it plays on.
   - The song menu (VR tab: Ocarina song menu, option 981): while it plays at the mouth, the other hand's grip held is the
     game's ZL, which opens the game's song list (its note wait stops meanwhile: the menu stays open while that grip is
     held and the game's ocarina is open); the left stick picks a song and A (the right stick right) plays it, as in the
     game.
   - While a song plays (the event goes on with no note wait and no menu) the renderer is told (rx_vr_ocarina_song), for
     the VR tab's Song effects (vr/items/ocarina/song_effects.inc). */
static int vr_ocarina(const HookshotVrHand hands[2], int on, int free, uint64_t *raw, int *playing, int64_t now) {
    static int holder = -1, grip_was[2], at_face, armed = 1, radial_hold, session, stop, away_stopped, menu, told = -100,
               song_told = -1;
    static int64_t start_until, start_by, cancel_until, away_at, radial_until, told_at, song_told_at;
    VrSwordHead head;
    const int tracked = on && rx_vr_head(&head) && head.active;
    float belt[3], world[42];
    const int have_belt = on && rx_vr_ocarina_belt(belt), have_world = have_belt && rx_vr_hands_world(world);
    const int wait = hos_oc_wait == 1, field = hos_oc_last_kind == 1 || hos_oc_last_kind == 3;
    const int event = hos_vr_event_playing();
    if (__atomic_exchange_n(&vr_ocarina_rearm, 0, __ATOMIC_RELAXED)) {   /* (back after the hands were away) */
        for (int k = 0; k < 2; k++) grip_was[k] = hands[k].valid && hands[k].squeeze > 0.35f;
        menu = 0;
        vr_ocarina_stick_note(0, 0);
    }
    if (__atomic_exchange_n(&vr_ocarina_radial, 0, __ATOMIC_RELAXED) && holder < 0 && on && hos_vr_stick_holder() != 1 &&
        hos_vr_sword_holder() != 1) {
        holder = 1, radial_hold = 1, radial_until = now + kVrOcarinaRadialNs;
        hos_log("vr: ocarina: chosen on the radial wheel: in the right hand (squeeze its grip to hold it)");
    }
    for (int k = 0; k < 2; k++) {
        const int grip = hands[k].valid && hands[k].squeeze > (grip_was[k] ? 0.35f : 0.55f);
        int reach = 0;
        if (have_world && world[21 * k] > .5f) {
            const float *p = world + 21 * k + 1;
            const float dx = p[0] - belt[0], dy = p[1] - belt[1], dz = p[2] - belt[2];
            reach = dx * dx + dy * dy + dz * dz < kVrOcarinaReach * kVrOcarinaReach;
        }
        __atomic_store_n(&vr_ocarina_reach[k], holder < 0 && reach, __ATOMIC_RELAXED);
        if (holder < 0 && on && free && grip && !grip_was[k] && reach && hos_vr_sword_holder() != k &&
            hos_vr_stick_holder() != k) {
            holder = k, radial_hold = 0;
            hos_log("vr: ocarina: taken from the belt by the %s hand", k ? "right" : "left");
        }
        if (holder == k && radial_hold) {   /* (from the wheel: the grip closes on it in time, or it goes to the belt) */
            if (grip) radial_hold = 0;
            else if (now >= radial_until || !on) {
                holder = -1, radial_hold = 0;
                hos_log("vr: ocarina: not gripped in %d ms after the wheel: back on the belt", (int)(kVrOcarinaRadialNs / 1000000));
            }
        } else if (holder == k && (!grip || !on)) {
            holder = -1;
            hos_log("vr: ocarina: let go: back on the belt%s", session ? " (the game's ocarina stops: B)" : "");
            if (session) stop = 1;
        }
        grip_was[k] = grip;
    }
    /* at the mouth: the holding hand near it (with a margin to leave it again) */
    int face = 0;
    if (holder >= 0 && tracked && hands[holder].valid) {
        float m[3];
        vr_ocarina_mouth(&head, m);
        const float *g = hands[holder].grip_p;
        const float d = sqrtf((g[0] - m[0]) * (g[0] - m[0]) + (g[1] - m[1]) * (g[1] - m[1]) + (g[2] - m[2]) * (g[2] - m[2]));
        face = at_face ? d < kVrOcarinaAway : d < kVrOcarinaFace;
    }
    if (face != at_face) {
        hos_log("vr: ocarina: %s the mouth", face ? "brought to" : "taken away from");
        at_face = face, away_at = face ? 0 : now;
    }
    if (wait && holder >= 0 && face && !session)
        session = 1, hos_log("vr: ocarina: the game waits for notes (kind %d)", hos_oc_last_kind);
    if (session && !wait && !event && !start_by) session = 0, stop = 0, menu = 0;   /* (the game's ocarina is over) */
    if (!face && !wait && !start_by) armed = 1;   /* (away and the game no longer waiting: ready to start again) */
    if (face && holder >= 0) stop = 0, away_stopped = 0;   /* (back at the mouth before it stopped: it plays on) */
    /* the start: D-pad Left, once, when the game is not playing it already and no event plays */
    if (face && armed && free && !wait && !start_by && !event) {
        armed = 0, start_until = now + kVrPressNs, start_by = now + kVrOcarinaStartNs;
        hos_log("vr: ocarina: at the mouth: the game's ocarina (D-pad Left)");
    }
    if (start_by && wait) start_by = 0;
    else if (start_by && now >= start_by) {
        hos_log("vr: ocarina: the game's ocarina did not start in %d ms (Link busy, swimming or in the air?): take it away "
                "from the mouth and bring it back to try again", (int)(kVrOcarinaStartNs / 1000000));
        start_by = 0;
    }
    /* the stop: held away from the mouth too long; the game's B once it waits for notes on the field */
    if (holder >= 0 && !face && away_at && now - away_at >= kVrOcarinaAwayNs && session && !away_stopped) {
        hos_log("vr: ocarina: held away from the mouth: the game's ocarina stops (B)");
        stop = 1, away_stopped = 1;   /* (once each time it is held away) */
    }
    if (stop && wait && field && !menu && now >= cancel_until) cancel_until = now + kVrPressNs, stop = 0;
    /* the song menu: the other hand's grip, while it plays at the mouth */
    const int other = holder >= 0 ? !holder : 0;
    const int menu_grip = holder >= 0 && hands[other].valid && hands[other].squeeze > (menu ? 0.35f : 0.55f);
    const int menu_now = rx_option(981, 1) && holder >= 0 && face && session && menu_grip && (wait || (menu && event));
    if (menu_now != menu) hos_log("vr: ocarina: song menu %s", menu_now ? "opened (the game's ZL)" : "closed");
    menu = menu_now;
    *playing = holder >= 0 && face && (wait || menu);
    uint64_t sw = 0;   /* (the game's own buttons) */
    if (*playing) {
        /* (the controllers' own triggers and the left controller's buttons do not reach the game as what the mapping makes
           of them: they are the notes) */
        *raw &= ~((1ULL << HOS_PAD_LTRIGGER) | (1ULL << HOS_PAD_RTRIGGER) | (1ULL << HOS_PAD_WEST) | (1ULL << HOS_PAD_NORTH));
        if (hands[0].trigger > 0.55f) sw |= 1ULL << HOS_SW_L;
        if (hands[1].trigger > 0.55f) sw |= 1ULL << HOS_SW_R;
        const int note = vr_ocarina_stick_note(hands[1].stick[0], hands[1].stick[1]);
        if (note) sw |= 1ULL << (note - 1);
        if (hands[0].buttons & VR_BTN_UPPER) sw |= 1ULL << HOS_SW_UP;    /* a half step up */
        if (hands[0].buttons & VR_BTN_LOWER) sw |= 1ULL << HOS_SW_DOWN;  /* a half step down */
        if (menu) sw |= 1ULL << HOS_SW_ZL;
    } else vr_ocarina_stick_note(0, 0);
    if (now < start_until) sw |= 1ULL << HOS_SW_LEFT;
    if (now < cancel_until) sw |= 1ULL << HOS_SW_B;
    __atomic_store_n(&vr_ocarina_switch, sw, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_ocarina_holder, holder, __ATOMIC_RELAXED);
    if (holder != told || now - told_at >= kVrOcarinaTellNs) told = holder, told_at = now, rx_vr_ocarina_hand(holder);
    const int song = session && event && !wait && !menu && !start_by && !stop;   /* (not a menu closed by letting go) */
    if (song != song_told || now - song_told_at >= kVrOcarinaTellNs) {
        if (song != song_told && song_told >= 0)
            hos_log("vr: ocarina: %s", song ? "a song plays (its effects: VR tab Song effects)" : "the song is over");
        song_told = song, song_told_at = now, rx_vr_ocarina_song(song);
    }
    return holder;
}

/* (host/nn_hid.c, the game's button word after the controller mapping, with the bow's and the menus' controls) the
   ocarina's presses added: the notes, D-pad Left to start, B to stop. RX_VR_OCARINA_TEST (headless, no headset): once the
   game waits for notes on the field (D-pad Left from the test's script), 1 s later 1 plays Saria's Song (R A Y R A Y,
   0.15 s each, 0.35 s apart) through this path, 2 presses B, to show that these buttons reach the game. */
uint64_t hos_vr_ocarina_controls(uint64_t buttons) {
    static int test = -1;
    static int64_t waited_at;
    if (test < 0) {
        const char *e = vr_research_getenv("RX_VR_OCARINA_TEST");
        test = e ? atoi(e) : 0;
    }
    uint64_t sw = __atomic_load_n(&vr_ocarina_switch, __ATOMIC_RELAXED);
    if (test) {
        const int64_t now = hos_now_ns();
        if (hos_oc_wait != 1 || (hos_oc_last_kind != 1 && hos_oc_last_kind != 3)) waited_at = 0;
        else if (!waited_at) waited_at = now, hos_log("vr: ocarina test %d: the game waits for notes", test);
        const int64_t t = waited_at ? now - waited_at - 1000000000LL : -1;
        static const int song[6] = {HOS_SW_R, HOS_SW_A, HOS_SW_Y, HOS_SW_R, HOS_SW_A, HOS_SW_Y};
        if (t >= 0 && test == 1 && t / 350000000LL < 6 && t % 350000000LL < 150000000LL) sw |= 1ULL << song[t / 350000000LL];
        if (t >= 0 && test == 2 && t < 150000000LL) sw |= 1ULL << HOS_SW_B;
    }
    return buttons | sw;
}
