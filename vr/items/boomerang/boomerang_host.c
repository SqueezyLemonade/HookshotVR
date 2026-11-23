/* Hookshot VR: child Link's Boomerang in a VR hand, first person (VR tab: Boomerang in the hands, Boomerang throw).

   The boomerang stays the game's own item, actor and flight: the mod only decides when the game's item button (ZR) and B
   are pressed, where the game's throw aims, and the renderer draws the game's own boomerang model in the VR hand while it
   is held (vr/items/boomerang/held_boomerang.inc). Grip holds it, in either hand; letting go of the grip puts it away.

   What is known (game 1.1.1; the player's own actor packs, the lifted code, headless runs on a copied save, 18 Nov):
   - The thrown boomerang is the actor Weapon_Boomerang (model Item_Boomerang, a Shootable like the hookshot's hook), made
     once and kept in reserve by Link's ShooterParam (Player_Child: IsMakeReserveActor). Link throws it from the ranged
     attack state (AI Player.action.rangedattack: ExecutePlayerRangedAttack, then OneShotShooterShoot Item_Boomerang on an
     animation event), as he throws a Deku Nut.
   - Link's action state (player object +0x1f60), headless 18 Nov (RX_VR_BOOMERANG_TEST=1, scripted presses, two runs):
     ZR pressed with it not out: 0 -> 0x2100000 -> 0x2180400, raised over his head, as long as ZR is held (4 s tried; he
     walks meanwhile, slower: 3.8 m/s); ZR let go: 0x180400 -> 0x400, thrown; it flies about 10 m and back, and once caught
     he holds it at rest, 0x100400 (at his hand, until the next press). From rest a ZR tap throws it at once
     (0x100400 -> 0x180400 -> 0x400). B while it is raised lowers it again, not thrown (0x180400 -> 0x100400); letting go of
     ZR after that throws nothing; raised from not out, B lowers it to rest too (0x2180400 -> 0x2100400 -> 0x100400). B at
     rest puts it away (0x100400 -> 0x80100400 -> 0). A ZR tap with it not out takes it out to rest (0 -> 0x2100000 ->
     0x100000 -> 0x100400). So: bit 0x100000 it is in his hand, 0x80000 raised, 0x400 the item's state; 0x400 alone the
     boomerang in flight (kVrBoomInHand, kVrBoomRaised, kVrBoomItemOut below).
   - Its own parameters (GameParameter/Boomerang): Range 10 m (ExtendRange 13, ShortRange 9), MoveSpeed 10 m/s; a homing
     sphere of 6 m (Item_Boomerang_Homing_Area).
   - Its flight (AI Item_Boomerang.root), decided once as it leaves the hand: locked on (QueryGenericIsGameCameraLockOn),
     it homes on the lock-on target (ExecuteSetBoomerangLinearVelocityLockOn, the game's own); otherwise it takes the game
     camera's position (QueryCameraGetGameCameraWorldPosition, FirstPlayerCameraPos) and a point straight ahead of the
     camera (QueryGetCameraForwardDir, TargetPos), and ExecuteBoomerangCalcAndSetLinaerVelocityReticleDirection (enter
     f_82672110, update f_826721d0) flies it from the hand toward TargetPos, bending over its Range toward the direction
     TargetPos - camera, then toward TargetPos again. It comes back to Link (its creator's position) and is caught within
     1 m of him.
   - QueryGetCameraForwardDir's calc (f_82a2b220; the node's actor in x21) works TargetPos out with f_82bb4c50 (a line
     along the camera's forward, 10 m) into sp+0x14, then hands it on. The game hook at main+0x2a0b2a8, right after that
     (hos_vr_boomerang_target), puts the VR aim there instead (see vr_boom_target for where).

   The controls, from these: a grip takes it out to rest in that hand (if it is not out: ZR held until Link raises it, then
   B lowers it again, then ZR let go); the trigger raises it (ZR held) and aims it, its reticle 10 m along the aim's line;
   letting go throws it (the game's own throw, aimed by the hook). The VR tab's Boomerang throw (option 961; the player's
   choice, 19 Nov: three, of the six first tried) picks the aim's line:
     0 Aim with Boomerang Hand: the holding hand's trigger; the controller's pointing line (the hookshot's,
       hookshot_vr_hook_ray).
     1 Aim with Empty Hand: the free hand points; its trigger aims and throws.
     2 Aim with Headset: the holding hand's trigger; the eyes' line, the headset's eye and forward as vr/camera/view_host.c
       builds the view (not the game's own aim: in first person it still comes from its orbit camera; headset 18 Nov).
   (A value saved from the six, 3-5, is 0.) Caught: the game's own catch, back at rest in Link's hand; with the grip still
   held it is in the VR hand again; let go meanwhile, it is put away once back. Letting go of the grip puts it away
   (raised: B lowers it first, ZR held, so it is not thrown): the game's own put-away call (vr_boomerang_put_away).

   Headset 18 Nov (the vr-items builds): Aim with Boomerang Hand works; the placement at the VR hand as it leaves holds
   (vr: boomerang: placed at ... found at ... next update, 0.3 m on: one update's flight).

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/menu/radial_host.c; it uses
   vr/items/bomb/bomb_host.c's (vr_bomb_place), vr/items/bow/bow_host.c's (vr_load, vr_store) and
   vr/items/gear/equipment_host.c's readers. Not compiled on its own. */
#define kVrBoomActorName "Weapon_Boomerang"
#define kVrBoomItemName "Item_Boomerang"      /* (its ActorName and pouch name: accepted too) */
#define kVrBoomInHand 0x100000u               /* (Link's action state: it is in his hand, rest or raised) */
#define kVrBoomRaised 0x80000u                /* (... raised, ready to throw) */
#define kVrBoomItemOut 0x400u                 /* (... the item's state: with kVrBoomInHand, at rest; alone, in flight) */
#define kVrBoomReach 10.0f                    /* (m: the game's Range; the reticle is this far along the aim's line) */
#define kVrBoomFar 60.0f                      /* (m: the throw's TargetPos this far along the line through the reticle, so
                                                 the game's camera term vanishes: vr_boom_target) */
#define kVrBoomTakeNs 700000000LL             /* (taking it out: ZR held at most this long for Link to raise it) */
#define kVrBoomLowerNs 1000000000LL           /* (... and B at most this long for him to lower it to rest) */
#define kVrBoomLaunchNs 1500000000LL          /* (a throw's aim waits this long for the game to let it go) */
#define kVrBoomFlightLimitNs 8000000000LL     /* (a throw not back after this is given up for, as a safety) */
#define kVrBoomViewNs 200000000LL             /* (the headset's view, told every frame, is fresh this long) */
#define kVrBoomPressNs 100000000LL            /* (a press of B) */
#define kVrBoomPressGapNs 300000000LL         /* (B pressed again this often while Link has not lowered it) */
#define kVrBoomTellNs 100000000LL             /* (the renderer is told this often: it forgets after 250 ms) */
enum { kVrBoomAimHand, kVrBoomAimOtherHand, kVrBoomAimHead, kVrBoomStyles };
enum { kVrBoomIdle, kVrBoomTaking, kVrBoomLowering, kVrBoomHeld, kVrBoomThrown, kVrBoomAway };

int rx_vr_hook_ray(int hand, float out[6]);   /* (vr/headset/bridges_host.c: a controller's pointing line, the hookshot's) */
int rx_vr_hands_world(float out[42]);
void rx_vr_boomerang(int hand, int reticle, const float at[3]);   /* (vr/headset/bridges_host.c) */
int hos_vr_sword_claims(int hand);
int hos_vr_bomb_in_reach(int hand);
extern int hos_vr_selected_is_boomerang;   /* (vr/input/quick_select_host.c sets it) */

static float vr_boom_bits_f(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static uint32_t vr_boom_f_bits(float f) { uint32_t bits; memcpy(&bits, &f, 4); return bits; }

/* ---- What the game thread knows (Link's and the actors' updates, the headset's view), for the controls and the hook --- */
static volatile uint32_t oot_vr_boom_state;         /* Link's action state, as last updated (see the top) */
static volatile uint64_t oot_vr_boom_actor;         /* the boomerang actor, once seen */
static volatile int64_t oot_vr_boom_launched_at;    /* the hook aimed a throw */
static volatile int oot_vr_boom_place_hand = -1;    /* the hand a throw left: the boomerang placed there as it leaves */
static volatile int oot_vr_boom_held_hand = -1;     /* the hand that holds its model now (the renderer's), or -1 */

/* (vr/camera/view_host.c, each first-person view it builds) the headset's eye and its forward in the game world, as the
   view was built: the eyes' line of Aim with Headset, where its reticle is drawn */
static volatile uint32_t oot_vr_boom_view[6];
static volatile int64_t oot_vr_boom_view_at;
static void oot_vr_boom_note_view(const float eye[3], const float back[3]) {
    for (int k = 0; k < 3; k++)
        __atomic_store_n(&oot_vr_boom_view[k], vr_boom_f_bits(eye[k]), __ATOMIC_RELAXED),
        __atomic_store_n(&oot_vr_boom_view[3 + k], vr_boom_f_bits(-back[k]), __ATOMIC_RELAXED);
    __atomic_store_n(&oot_vr_boom_view_at, hos_now_ns(), __ATOMIC_RELEASE);
}
/* the eyes' line: 0 while the headset's view is not fresh */
static int vr_boom_head_ray(float ray[6], int64_t now) {
    const int64_t at = __atomic_load_n(&oot_vr_boom_view_at, __ATOMIC_ACQUIRE);
    if (!at || now - at > kVrBoomViewNs) return 0;
    for (int k = 0; k < 6; k++) ray[k] = vr_boom_bits_f(__atomic_load_n(&oot_vr_boom_view[k], __ATOMIC_RELAXED));
    return 1;
}
/* the aim's line for a style: the holding hand's or the other's pointing line, or the eyes' */
static int vr_boom_aim_ray(int style, int aim_hand, float ray[6], int64_t now) {
    return style == kVrBoomAimHead ? vr_boom_head_ray(ray, now) : rx_vr_hook_ray(aim_hand, ray);
}

/* ---- A throw's aim, frozen by the controls when the throw is asked for, used by the hook when the game lets go -------- */
static volatile uint32_t oot_vr_boom_aim_seq;       /* (odd while being written) */
static struct { float ray[6]; int64_t at; } oot_vr_boom_aim;
static void oot_vr_boom_aim_set(const float ray[6], int64_t now) {
    __atomic_add_fetch(&oot_vr_boom_aim_seq, 1, __ATOMIC_ACQ_REL);
    oot_vr_boom_aim.at = now;
    memcpy(oot_vr_boom_aim.ray, ray, sizeof oot_vr_boom_aim.ray);
    __atomic_add_fetch(&oot_vr_boom_aim_seq, 1, __ATOMIC_ACQ_REL);
}
static int oot_vr_boom_aim_take(float ray[6], int64_t *at) {
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t seq = __atomic_load_n(&oot_vr_boom_aim_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) continue;
        *at = oot_vr_boom_aim.at;
        memcpy(ray, oot_vr_boom_aim.ray, sizeof oot_vr_boom_aim.ray);
        if (__atomic_load_n(&oot_vr_boom_aim_seq, __ATOMIC_ACQUIRE) == seq) return 1;
    }
    return 0;
}

/* first person in the headset, the VR tab's row on, the boomerang selected */
static int oot_vr_boom_on(void) {
    HookshotVrHead h;
    return __atomic_load_n(&hos_vr_selected_is_boomerang, __ATOMIC_RELAXED) && rx_option(960, 0) && rx_vr_head(&h) &&
           h.active && h.mode == 1;
}

/* ==== THE PUT-AWAY: the game's own call, one function ===================================================================
   hos_vr_put_away_held (vr/items/gear/put_away_host.c) asks for the game's own put-away (f_804c8690, what events use to
   sheathe Link's gear: every slot the game has drawn, the held item's slot 1 among them), made on Link's next update. It
   is asked when the put-away begins, and again every kVrBoomAwayRetryNs while the game still has the boomerang in Link's
   hand (kVrBoomInHand), at most kVrBoomAwayTries times; then, or when the VR tab's Put away says Button (it returns 0), the
   game's B is pressed for kVrBoomPressNs instead, as the hookshot's put-away does. w: this put-away's state (zeroed when it
   begins), out: the game still has it in Link's hand. Returns the game buttons to add now. */
#define kVrBoomAwayRetryNs 300000000LL
#define kVrBoomAwayTries 4
int hos_vr_put_away_held(void);
typedef struct { int tries, button; int64_t next, button_at; } VrBoomAway;
static uint64_t vr_boomerang_put_away(VrBoomAway *w, int out, int64_t now) {
    const uint64_t B = 1ULL << 1;
    if (!w->button && (!w->tries || (out && now >= w->next))) {
        if (w->tries < kVrBoomAwayTries && hos_vr_put_away_held()) {
            w->tries++, w->next = now + kVrBoomAwayRetryNs;
            if (w->tries > 1) hos_log("vr: boomerang: still out: the put-away asked again (%d)", w->tries);
        } else {
            w->button = 1, w->button_at = now;
            hos_log("vr: boomerang: put away with the game's B (%s)", w->tries ? "the game's call did not put it away" :
                    "VR tab Put away: Button");
        }
    }
    return w->button && now - w->button_at < kVrBoomPressNs ? B : 0;
}


/* ---- The controls: the game's buttons while the boomerang is selected in first person --------------------------------
   (host/nn_hid.c, through vr/items/bow/bow_host.c hos_oot_vr_bow_controls) ZR is the game's item button, B lowers it (see
   the top). The real ZR (the right trigger) does not reach the game meanwhile, nor ZL (the left trigger: lock-on) while the
   left hand's trigger is the boomerang's. The states: kVrBoomTaking (ZR held until Link raises it), kVrBoomLowering (B,
   ZR still held, until it is at rest), kVrBoomHeld (at rest in the VR hand; ZR held while the trigger aims), kVrBoomThrown
   (until it is back in Link's hand), kVrBoomAway (being put away). */
static volatile int oot_vr_boom_grips_now;   /* which grips are the boomerang's (bit 0 left, bit 1 right) */
int hos_vr_boomerang_grips(void) { return __atomic_load_n(&oot_vr_boom_grips_now, __ATOMIC_RELAXED); }
static int vr_boom_at_rest(uint32_t g) {
    return (g & (kVrBoomInHand | kVrBoomItemOut)) == (kVrBoomInHand | kVrBoomItemOut) && !(g & kVrBoomRaised);
}
/* the VR tab's Boomerang throw: one of the three styles (a value saved when there were six, 3-5, is the first) */
static int vr_boom_style(void) {
    const int style = rx_option(961, 0);
    return style >= 0 && style < kVrBoomStyles ? style : kVrBoomAimHand;
}
uint64_t hos_vr_boomerang_buttons(uint64_t buttons) {
    const uint64_t ZL = 1ULL << 8, ZR = 1ULL << 9, B = 1ULL << 1;
    static int state = kVrBoomIdle, hand = -1, grip_was[2], trigger_was[2], aiming, flown, away_from_aim;
    static int64_t state_at, told_at, pressed_at;
    static VrBoomAway away;
    static int told_hand = -100;
    const int64_t now = hos_now_ns();
    if (!oot_vr_boom_on() || __atomic_load_n(&oot_vr_quick_open, __ATOMIC_ACQUIRE)) {
        if (state != kVrBoomIdle) hos_log("vr: boomerang: no longer in a VR hand (not selected, the quick select, or not first person)");
        state = kVrBoomIdle, hand = -1, aiming = 0, grip_was[0] = grip_was[1] = trigger_was[0] = trigger_was[1] = 0;
        __atomic_store_n(&oot_vr_boom_grips_now, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_boom_held_hand, -1, __ATOMIC_RELAXED);
        return buttons;
    }
    const int style = vr_boom_style();
    const uint32_t g = __atomic_load_n(&oot_vr_boom_state, __ATOMIC_RELAXED);   /* (Link's action state) */
    uint32_t hw[64] = {0};
    float squeeze[2] = {0, 0}, trigger[2] = {0, 0};
    if (rx_vr_hands(hw))
        for (int k = 0; k < 2; k++) memcpy(&trigger[k], &hw[1 + 26 * k + 21], 4), memcpy(&squeeze[k], &hw[1 + 26 * k + 22], 4);
    int grip[2], trig[2];
    for (int k = 0; k < 2; k++) {
        grip[k] = grip_was[k] ? squeeze[k] > .35f : squeeze[k] > .55f;
        trig[k] = trigger_was[k] ? trigger[k] > .35f : trigger[k] > .55f;
    }
    const int other = hand >= 0 ? 1 - hand : -1;
    const int aim_hand = style == kVrBoomAimOtherHand ? other : hand;   /* (whose trigger aims and throws, whose line) */
    int throw_now = 0, away_now = 0;
    switch (state) {
    case kVrBoomIdle:
        for (int k = 0; k < 2; k++)
            if (grip[k] && !grip_was[k] && !hos_vr_bomb_in_reach(k) && !hos_vr_sword_claims(k)) {
                hand = k, state_at = now, aiming = 0;
                state = vr_boom_at_rest(g) ? kVrBoomHeld : kVrBoomTaking;
                hos_log("vr: boomerang: in the %s hand (style %d)%s", k ? "right" : "left", style,
                        state == kVrBoomTaking ? ", taken out (ZR until Link raises it, then B to rest)" : "");
                break;
            }
        break;
    case kVrBoomTaking:   /* (ZR until raised; the game took it to rest by itself: held) */
        if (vr_boom_at_rest(g) && now - state_at > kVrBoomPressNs) state = kVrBoomHeld;
        else if (g & kVrBoomRaised) state = kVrBoomLowering, state_at = now, pressed_at = 0;
        else if (now - state_at > kVrBoomTakeNs) {
            hos_log("vr: boomerang: did not come out in %d ms (state %#x)", (int)(kVrBoomTakeNs / 1000000), g);
            state = kVrBoomIdle, hand = -1;
        }
        break;
    case kVrBoomLowering:   /* (B, ZR still held, until it is at rest; then ZR is let go: nothing thrown) */
        if (vr_boom_at_rest(g)) state = kVrBoomHeld, state_at = now;
        else if (now - state_at > kVrBoomLowerNs) {
            hos_log("vr: boomerang: not lowered to rest in %d ms (state %#x)", (int)(kVrBoomLowerNs / 1000000), g);
            state = kVrBoomHeld, state_at = now;
        }
        break;
    case kVrBoomHeld:   /* (the trigger raises it, ZR held; let go, the game throws it; the grip let go puts it away) */
        if (aim_hand >= 0) {
            if (trig[aim_hand] && !trigger_was[aim_hand]) aiming = 1;
            if (aiming && !trig[aim_hand]) throw_now = 1;
        }
        if (!grip[hand] && !throw_now) away_now = 1;
        break;
    case kVrBoomThrown:   /* (back: in Link's hand again after it flew, or the throw never left, or the flight's limit) */
        if (!(g & kVrBoomInHand)) flown = 1;
        if ((flown && vr_boom_at_rest(g)) || (!flown && now - state_at > kVrBoomLaunchNs) || now - state_at > kVrBoomFlightLimitNs) {
            if (grip[hand]) {
                state = kVrBoomHeld, state_at = now, aiming = 0;
                hos_log("vr: boomerang: back, in the %s hand again%s", hand ? "right" : "left", flown ? "" : " (it never left)");
            } else {
                hos_log("vr: boomerang: back, the grip let go meanwhile: put away");
                away_now = 1;
            }
        }
        break;
    case kVrBoomAway:   /* (raised: B first, ZR held, until it is at rest; then the put-away; over once it is away) */
        if (!(g & kVrBoomInHand) || now - state_at > kVrBoomLowerNs + kVrBoomAwayRetryNs * (kVrBoomAwayTries + 1))
            state = kVrBoomIdle, hand = -1;
        break;
    }
    if (away_now) {
        away_from_aim = aiming;   /* (ZR just pressed: Link may not show it raised yet; it is lowered all the same) */
        state = kVrBoomAway, state_at = now, aiming = 0, pressed_at = 0;
        memset(&away, 0, sizeof away);
        hos_log("vr: boomerang: let go: put away");
    }
    if (throw_now) {   /* (the aim's line frozen now, as the reticle showed it; the hook uses it when the game lets it go) */
        float ray[6];
        if (vr_boom_aim_ray(style, aim_hand, ray, now)) {
            oot_vr_boom_aim_set(ray, now);
            hos_log("vr: boomerang: thrown along the %s line", style == kVrBoomAimHead ? "eyes'" : "controller's");
        } else {   /* (an earlier throw's line must not aim this one) */
            const float none[6] = {0, 0, 0, 0, 0, 0};
            oot_vr_boom_aim_set(none, 0);
            hos_log("vr: boomerang: thrown with no VR line (the game's own aim)");
        }
        __atomic_store_n(&oot_vr_boom_place_hand, hand, __ATOMIC_RELAXED);
        state = kVrBoomThrown, state_at = now, aiming = 0, flown = 0;
    }
    for (int k = 0; k < 2; k++) grip_was[k] = grip[k], trigger_was[k] = trig[k];
    /* (the grips: none held yet, both take it; one held, only that one is the boomerang's (the other locks on and guards
       again, as beside the Deku Stick)) */
    __atomic_store_n(&oot_vr_boom_grips_now, hand < 0 ? 3 : 1 << hand, __ATOMIC_RELAXED);
    buttons &= ~ZR;
    if (hand == 0 || aim_hand == 0) buttons &= ~ZL;
    {   /* (the game's buttons for the state) */
        const int lowering = state == kVrBoomLowering ||
                             (state == kVrBoomAway && ((g & kVrBoomRaised) || (away_from_aim && !vr_boom_at_rest(g) &&
                                                                               now - state_at < kVrBoomLowerNs)));
        if (state == kVrBoomTaking || lowering || (state == kVrBoomHeld && aiming)) buttons |= ZR;
        if (lowering && (!pressed_at || now - pressed_at > kVrBoomPressGapNs)) pressed_at = now;
        if (lowering && now - pressed_at < kVrBoomPressNs) buttons |= B;
        if (state == kVrBoomAway && !lowering) buttons |= vr_boomerang_put_away(&away, (g & kVrBoomInHand) != 0, now);
    }
    {   /* (the renderer: the hand that holds the model (until the game lets it go), -2 hidden while being put away; the
           reticle while aiming, kVrBoomReach along the aim's line) */
        const int leaving = state == kVrBoomThrown && !flown && now - state_at < kVrBoomLaunchNs;
        const int shown = state == kVrBoomTaking || state == kVrBoomLowering || state == kVrBoomHeld || leaving ? hand :
                          state == kVrBoomAway ? -2 : -1;
        float at[3] = {0, 0, 0}, ray[6];
        int reticle = 0;
        if (state == kVrBoomHeld && aiming && vr_boom_aim_ray(style, aim_hand, ray, now))
            for (int k = 0; k < 3; k++) at[k] = ray[k] + kVrBoomReach * ray[3 + k], reticle = 1;
        __atomic_store_n(&oot_vr_boom_held_hand, shown, __ATOMIC_RELAXED);
        if (shown != told_hand || reticle || now - told_at > kVrBoomTellNs)
            rx_vr_boomerang(shown, reticle, at), told_hand = shown, told_at = now;
    }
    return buttons;
}

/* ---- The aim: the game hook at main+0x2a0b2a8 ---------------------------------------------------------------------------
   In QueryGetCameraForwardDir's calc (f_82a2b220), right after f_82bb4c50 left the point ahead of the camera (TargetPos) at
   sp+0x14; x21 is the node's actor. For a boomerang a VR hand has just thrown, its TargetPos is vr_boom_target's.

   Where (headset 19 Nov: "the throw goes down and to the right of the reticle"): the flight heads for TargetPos from where
   it starts, and bends on the way toward TargetPos - FirstPlayerCameraPos, the game camera's eye, which in first person is
   its orbit camera's, behind and above Link. TargetPos at the reticle (10 m) made that bend a few degrees down and aside.
   So TargetPos is kVrBoomFar along the line from where the boomerang starts (the VR hand, where it is placed as it
   leaves) through the reticle's point: seen from that far, the orbit camera and the hand are in the same direction, so
   the flight keeps to the line through the reticle. A far TargetPos changes neither its range nor its return (the flight
   turns back past 10-11 m from where it was thrown, Item_Boomerang.root): headless 19 Nov, Hyrule Field,
   RX_VR_BOOMERANG_TEST=3 along the game's own aim, four throws flew the full 10 m within 0.07 m of the line, then curved
   back as the game's own do. (Headless the game camera is the orbit camera looking along the throw, so the bend itself is
   small there: a throw turned 31 degrees, level, missed by 0.12-0.23 m at 5 m with the old aim, 0.00-0.11 m with this
   one; turned 32 degrees with its climb, both were stopped at 5-6 m by the ground and differed by little.)
   start: where it starts; ray: the aim's line (the reticle kVrBoomReach along it); out: TargetPos. */
static void vr_boom_target(const float start[3], const float ray[6], float out[3]) {
    float d[3], n = 0;
    for (int k = 0; k < 3; k++) d[k] = ray[k] + kVrBoomReach * ray[3 + k] - start[k], n += d[k] * d[k];
    n = sqrtf(n);
    for (int k = 0; k < 3; k++) out[k] = n > 0.1f ? start[k] + kVrBoomFar * d[k] / n : ray[k] + kVrBoomFar * ray[3 + k];
}
/* RX_VR_BOOMERANG_TEST (headless; 1 the research run below): 2 a test reticle 10 m from the boomerang, along the game's
   own aim turned RX_VR_BOOMERANG_TURN degrees (default 30) about the vertical, as TargetPos (the old aim); 3 the same reticle
   through vr_boom_target (the aim now). (Turned, so the game's camera term bends the old aim; along the game's own aim,
   where run 1 found 10 m of open space, so no wall stops it.) */
static int oot_vr_boom_test(void) {
    static int test = -1;
    if (test < 0) test = vr_research_int("RX_VR_BOOMERANG_TEST", 0);
    return test;
}
static int oot_vr_boom_is_boomerang(Ctx *c, uint64_t actor) {
    if (!actor) return 0;
    if (actor == __atomic_load_n(&oot_vr_boom_actor, __ATOMIC_RELAXED)) return 1;
    if (!oot_vr_equipment_ptr(actor, 0x328)) return 0;
    char name[64];
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    return !strcmp(name, kVrBoomActorName) || !strcmp(name, kVrBoomItemName);
}
void hos_vr_boomerang_target(Ctx *c) {
    /* (nothing at all unless a VR hand has just thrown the boomerang: every other actor's use of this query, and every
       other moment, is left alone without even a read) */
    const int test = oot_vr_boom_test();
    const int64_t now = hos_now_ns();
    float ray[6], start[3], at[3];
    int64_t asked = 0;
    if (test < 2 && !(oot_vr_boom_on() && oot_vr_boom_aim_take(ray, &asked) && asked && now - asked < kVrBoomLaunchNs))
        return;
    const uint64_t actor = c->x[21];
    if (!oot_vr_boom_is_boomerang(c, actor)) return;
    __atomic_store_n(&oot_vr_boom_actor, actor, __ATOMIC_RELAXED);
    const uint64_t sp = c->x[31];
    float game[3];
    for (int k = 0; k < 3; k++) game[k] = vr_load(c, sp + 0x14 + 4 * k), start[k] = vr_load(c, actor + 796 + 4 * k);
    if (test >= 2) {   /* (the test reticle, from the boomerang where the game has it) */
        const float turn = (float)vr_research_int("RX_VR_BOOMERANG_TURN", 30) * 0.0174533f;
        const float gx = game[0] - start[0], gy = game[1] - start[1], gz = game[2] - start[2];
        const float gn = sqrtf(gx * gx + gy * gy + gz * gz);
        if (gn < 0.5f) return;
        memcpy(ray, start, sizeof start);   /* (the game's own aim's climb kept: a level throw met rising ground at 5 m) */
        ray[3] = (gx * cosf(turn) - gz * sinf(turn)) / gn, ray[4] = gy / gn, ray[5] = (gx * sinf(turn) + gz * cosf(turn)) / gn;
    } else {   /* (from the VR hand: it is placed there as it leaves, hos_vr_boomerang_actor_update) */
        const int hand = __atomic_load_n(&oot_vr_boom_place_hand, __ATOMIC_RELAXED);
        float w[42];
        if (hand >= 0 && rx_vr_hands_world(w) && w[21 * hand] > .5f) memcpy(start, w + 21 * hand + 1, sizeof start);
    }
    if (test == 2) for (int k = 0; k < 3; k++) at[k] = ray[k] + kVrBoomReach * ray[3 + k];
    else vr_boom_target(start, ray, at);
    /* (the throw's aim is not used up: the game may ask more than once as it lets go; it holds for kVrBoomLaunchNs) */
    __atomic_store_n(&oot_vr_boom_launched_at, now, __ATOMIC_RELAXED);
    for (int k = 0; k < 3; k++) vr_store(c, sp + 0x14 + 4 * k, at[k]);
    static unsigned told;
    if (told++ < 40)
        hos_log("vr: boomerang thrown: the game aims at %.2f %.2f %.2f; the VR aim: from %.2f %.2f %.2f through the reticle at "
                "%.2f %.2f %.2f, TargetPos %.2f %.2f %.2f%s", game[0], game[1], game[2], start[0], start[1], start[2],
                ray[0] + kVrBoomReach * ray[3], ray[1] + kVrBoomReach * ray[4], ray[2] + kVrBoomReach * ray[5], at[0],
                at[1], at[2], test == 2 ? " (test: the old aim, TargetPos at the reticle)" : test == 3 ? " (test)" : "");
}

/* ---- The research run, headless (RX_VR_BOOMERANG_TEST, on a COPIED save): the boomerang given and selected once a save
   is loaded, then the boomerang's place and distance from Link logged as they change (his action state is logged by his
   update) */
#if defined(_WIN32) || defined(__linux__)   /* (oot_pouch_change: the debug tools' pouch edit, host/hos_main.c) */
static void vr_boom_research(Ctx *c, uint64_t player, int64_t now) {
    static int given, selected;
    static int64_t loaded_at;
    if (!given) {
        const int have = oot_pouch_count(c, kVrBoomItemName);
        if (have < 0) return;
        if (!loaded_at) loaded_at = now;
        if (now - loaded_at < 5000000000LL) return;   /* (the save settled) */
        given = 1;
        if (have == 0) hos_log("vr: boomerang test: given (status %d)", oot_pouch_change(c, kVrBoomItemName, 1));
        else hos_log("vr: boomerang test: owned already");
    }
    if (!selected && now - loaded_at > 7000000000LL) {   /* (the game's own selection, as the VR radial selects) */
        selected = 1;
        const uint64_t slot = recomp_load64(c, 0x84c9e0d0ULL);
        const uint64_t pouch = slot >= 0x80000000ULL && !(slot & 7) && hos_guest_range(slot, 8) ? recomp_load64(c, slot) : 0;
        A64Fn change = a64_lookup(0x81231c00ULL);
        if (pouch >= HOS_HEAP_BASE && change) {
            const Ctx saved = *c;
            c->x[31] = (c->x[31] - 0x100) & ~0xfULL;
            c->x[0] = pouch, c->x[1] = 1, c->x[2] = 3, c->x[3] = 0, c->x[4] = 1;   /* (child, item 3: the Boomerang) */
            change(c);
            *c = saved;
            hos_log("vr: boomerang test: selected");
        }
        oot_vr_note_selected_item(c);
    }
    const uint32_t state = __atomic_load_n(&oot_vr_boom_state, __ATOMIC_RELAXED);   /* (logged by Link's update) */
    const uint64_t boomerang = __atomic_load_n(&oot_vr_boom_actor, __ATOMIC_RELAXED);
    static int64_t logged;
    static float last[3];
    if (boomerang && now - logged > 100000000LL) {
        float p[3], q[3], moved = 0;
        for (int k = 0; k < 3; k++) p[k] = vr_load(c, boomerang + 796 + 4 * k), q[k] = vr_load(c, player + 796 + 4 * k);
        for (int k = 0; k < 3; k++) moved += fabsf(p[k] - last[k]);
        if (moved > 0.02f) {
            const float d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
            hos_log("vr: boomerang test: at %.2f %.2f %.2f, %.2f m from Link (%.2f %.2f %.2f up), state %#x", p[0], p[1], p[2],
                    sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), d[0], d[1], d[2], state);
            memcpy(last, p, sizeof last);
        }
        logged = now;
    }
}
#endif


/* the boomerang actor among the actors' updates: its name read once per actor (a small cache keyed by the actor and its
   name's pointer) */
typedef struct { uint64_t actor, text; int is; } VrBoomName;
static VrBoomName vr_boom_names[256];
static int vr_boom_is_it(Ctx *c, uint64_t actor) {
    VrBoomName *n = &vr_boom_names[(actor >> 4) & 255];
    const uint64_t text = recomp_load64(c, actor + 0x260);
    if (n->actor == actor && n->text == text) return n->is;
    char name[64];
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    n->actor = actor, n->text = text, n->is = !strcmp(name, kVrBoomActorName) || !strcmp(name, kVrBoomItemName);
    return n->is;
}

/* ---- (host/hos_main.c hos_oot_lift_player, VR HOOK: every actor's movement update) ------------------------------------ */
void hos_vr_boomerang_actor_update(Ctx *c, uint64_t actor) {
    if (!actor) return;
    const int test = oot_vr_boom_test();
    const int selected = __atomic_load_n(&hos_vr_selected_is_boomerang, __ATOMIC_RELAXED);
    if (!test && !(selected && rx_option(960, 0))) return;
    const uint64_t player = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const int64_t now = hos_now_ns();
    if (actor == player) {   /* (once a frame: Link's action state, the research run) */
        const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
        const uint32_t g = object ? recomp_load32(c, object + 0x1f60) : 0;
        static uint32_t said = 0xffffffffu;
        static unsigned told;
        if (g != said && told < 400) told++, hos_log("vr: boomerang: Link's action state %#x -> %#x", said, g);
        said = g;
        __atomic_store_n(&oot_vr_boom_state, g, __ATOMIC_RELAXED);
#if defined(_WIN32) || defined(__linux__)
        if (test) vr_boom_research(c, actor, now);
#endif
        return;
    }
    if (!player || !oot_vr_equipment_ptr(actor, 0x328) || !vr_boom_is_it(c, actor)) return;
    __atomic_store_n(&oot_vr_boom_actor, actor, __ATOMIC_RELAXED);
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = vr_load(c, actor + 796 + 4 * k);
    if (__atomic_load_n(&oot_vr_boom_held_hand, __ATOMIC_RELAXED) >= 0) {
        /* (its model kept from the game's near-plane test while a VR hand holds it: the hookshot's slot, never both at once
           (one item is selected), vr/items/gear/gear_near_host.c; the game's own sits in Link's game hand, which the headset
           can be in front of) */
        static int64_t looked;
        static uint64_t model;
        if (now - looked > 300000000LL) looked = now, model = oot_vr_gear_resolve(c, actor);
        __atomic_store_n(&oot_vr_gear_hook_model, model, __ATOMIC_RELEASE);
        __atomic_store_n(&oot_vr_gear_hook_time, now, __ATOMIC_RELEASE);
    }
    /* (as it leaves the hand, in the first update after the game let it go: placed at the VR hand once, so its flight starts
       there and not at Link's game hand; the log says whether the placement held) */
    static int64_t placed_for;
    static float placed_at[3];
    static int placed_check;
    const int64_t launched = __atomic_load_n(&oot_vr_boom_launched_at, __ATOMIC_RELAXED);
    if (placed_check) {
        hos_log("vr: boomerang: placed at %.2f %.2f %.2f, found at %.2f %.2f %.2f next update", placed_at[0], placed_at[1],
                placed_at[2], p[0], p[1], p[2]);
        placed_check = 0;
    }
    const int hand = __atomic_load_n(&oot_vr_boom_place_hand, __ATOMIC_RELAXED);
    if (launched && launched != placed_for && now - launched < 100000000LL && hand >= 0 && oot_vr_boom_on()) {
        placed_for = launched;
        float w[42], rot[9];
        if (rx_vr_hands_world(w) && w[21 * hand] > .5f) {
            for (int k = 0; k < 9; k++) rot[k] = vr_load(c, actor + 0x328 + 4 * k);
            vr_bomb_place(c, actor, w + 21 * hand + 1, rot);
            memcpy(placed_at, w + 21 * hand + 1, sizeof placed_at), placed_check = 1;
        }
    }
}
