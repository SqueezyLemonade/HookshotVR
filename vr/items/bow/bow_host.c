/* Hookshot VR: The first-person bow on the game side: its controls owned by the hands (ZR raise, string draw, release), and the
   probe that steers the game's arrow to the VR bow (called from vr/items/bow/vr_bow_patch.py's patch).

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* (OoT VR) The first-person bow's controls, owned by the hands. ZR (mapped bit 9) is the game's bow button: held raises,
   nocks and draws; let go fires (every logged shot followed a ZR release while aiming; a quick tap is a quick shot). The game's
   B (bit 1) puts the bow away, but the VR controls never press it (see below). Holding ZR also puts Link in the game's aiming stance (0x2000442 raising, 0x2000446 drawn, about a
   second in), which changes how he moves and what the game culls; out of it the bow rests in his hand (0x441, seen after a
   shot with ZR let go).
   "Grip holds the bow" (VR tab, with string firing): once ZR is known to hold the bow this session (a ZR press brought the
   bow out; the pause menu forgets it), squeezing either grip takes the bow out in that hand: ZR is held until the bow is
   out, then let go, so the bow rests. ZR is held again only while the string is drawn past its commit (flag 2 of the bow
   solution, vk_vr_body.cpp bow_pose), so the game aims only while you draw, and the string's firing let-go lets it go for
   the game's shot; letting go of the bow's grip (not while drawing) puts it away with the game's own call, never B (B rolls
   while Link moves, talks or opens beside someone: vr/items/gear/put_away_stow.inc). While the bow is the ZR item
   the grips are not the game's shoulder buttons (hos_oot_vr_bow_grips: as ZL they locked on and strafed) and the real ZR
   does nothing. Without that option, "Bow fires on string release": while the bow is out the real ZR does nothing and
   the string's committed draw holds it (the press that brought the bow out passes until let go). A dropout of the
   string's solution under 1.5 s keeps the last answer, so it is never a release. */
static volatile int oot_vr_bow_grips_owned;
#include "../gear/put_away_stow.inc"   /* (the put-away: the game's own call, never B) */
int rx_vr_hands(void *out);
int hos_oot_vr_bow_grips(void) { return __atomic_load_n(&oot_vr_bow_grips_owned, __ATOMIC_RELAXED); }
static volatile int oot_vr_bow_hand_now = -1;
/* (vr/input/controllers_host.c) the hand that holds the bow or slingshot, or takes it out or puts it away; -1 none */
int hos_oot_vr_bow_hand(void) { return __atomic_load_n(&oot_vr_bow_hand_now, __ATOMIC_RELAXED); }
int hos_vr_sword_claims(int hand);   /* (a grip behind the shoulder is the sword's, vr/input/controllers_host.c) */
int hos_vr_ocarina_claims(int hand);   /* (a grip at the belt ocarina, or the hand holding it: vr/items/ocarina/ocarina_host.c) */
uint64_t hos_oot_vr_bow_controls(uint64_t buttons) {
    {   /* (bombs in the VR hands, vr/items/bomb/bomb_host.c: their ZR taps, and the triggers kept from the game) */
        extern uint64_t hos_vr_bomb_buttons(uint64_t), hos_vr_bomb_test_buttons(uint64_t);
        buttons = hos_vr_bomb_test_buttons(hos_vr_bomb_buttons(buttons));
        extern uint64_t hos_vr_hook_buttons(uint64_t);   /* (the hookshot in a VR hand, vr/items/hookshot/hookshot_host.c) */
        buttons = hos_vr_hook_buttons(buttons);
        extern uint64_t hos_vr_boomerang_buttons(uint64_t);   /* (the boomerang in a VR hand, vr/items/boomerang/boomerang_host.c) */
        buttons = hos_vr_boomerang_buttons(buttons);
    }
    const uint64_t ZR = 1ULL << 9, PLUS = 1ULL << 10;
    enum { IDLE, TAKING_OUT, HOLDING, PUTTING_AWAY };
    static int passthrough, holding, flags, phase = IDLE, bow_on_zr, take_hand = -1, grip_was[2], real_take, regrip;
    static VrStow away;
    static int64_t solution_at, phase_at, zr_press_at;
    const int64_t now = hos_now_ns();
    const int64_t out_at = __atomic_load_n(&oot_vr_bow_out_seen, __ATOMIC_ACQUIRE);
    const int bow_out = out_at && now - out_at < 150000000LL;
    float aim[12];
    if (bow_out && rx_vr_bow(aim)) {
        flags = (int)aim[10], solution_at = now;
    }
    const int fresh = bow_out && solution_at && now - solution_at < 1500000000LL && (flags & 8);
    uint64_t result = buttons;
    /* ---- learning which item ZR holds: a ZR press followed by the bow coming out; the pause menu forgets it */
    static int zr_was, out_was, selected_was, selected_grip_pending;
    if ((buttons & ZR) && !zr_was) zr_press_at = now;
    zr_was = (buttons & ZR) != 0;
    /* the selection, when it can be read and has not been shown wrong (a grip that brought nothing out) */
    static int distrusted = -1;
    const int selection = __atomic_load_n(&oot_vr_selection, __ATOMIC_ACQUIRE);
    const int selected_bow = selection != distrusted ? __atomic_load_n(&oot_vr_selected_bow, __ATOMIC_ACQUIRE) : -1;
    const int selection_changed = selection != selected_was;
    selected_was = selection;
    if (selection_changed) selected_grip_pending = selected_bow == 1;
    if (selected_bow >= 0 && bow_on_zr != selected_bow) {
        bow_on_zr = selected_bow;
        hos_log("vr: the selected item %s the bow: the grips %s", selected_bow ? "is" : "is not", selected_bow ? "take it" : "are the game's");
    } else if (selected_bow < 0 && (buttons & PLUS)) {
        if (bow_on_zr) hos_log("vr: pause menu: ZR's item is asked again (bring the bow out with ZR once)");
        bow_on_zr = 0;
    }
    if (selected_bow < 0 && bow_out && !out_was && zr_press_at && now - zr_press_at < 600000000LL && !bow_on_zr) {
        bow_on_zr = 1;
        hos_log("vr: ZR holds the bow: the grips take it out from now on");
    }
    out_was = bow_out;
    HookshotVrHead h;
    const int grip_mode = rx_option(900, 0) && rx_vr_head(&h) && h.active && h.mode == 1;
    if (grip_mode) {
        uint32_t hw[64] = {0};
        float sq[2] = {0, 0};
        if (rx_vr_hands(hw)) memcpy(&sq[0], &hw[1 + 22], 4), memcpy(&sq[1], &hw[1 + 26 + 22], 4);
        int grip[2];
        for (int k = 0; k < 2; k++) grip[k] = grip_was[k] ? sq[k] > .35f : sq[k] > .55f;
        const int bow_hand = fresh ? ((flags & 4) ? 1 : 0) : take_hand;
        int zr = 0;
        switch (phase) {
        case IDLE:
            /* (brought out by the player's own ZR: that press stays theirs until let go, as it always was, unless a grip
               is holding the bow by then) */
            if (bow_out && (buttons & ZR) && !real_take && now - zr_press_at < 600000000LL) real_take = 1;
            if (real_take && !(buttons & ZR)) real_take = 0;
            zr = real_take;
            if (bow_out) {   /* (out some other way: held from here as soon as a grip takes it) */
                for (int k = 0; k < 2; k++)
                    if (grip[k] && (!grip_was[k] || real_take) && !hos_vr_sword_claims(k) && !hos_vr_ocarina_claims(k)) {
                        take_hand = k, phase = HOLDING, phase_at = now, zr = 0, real_take = 0;
                        break;
                    }
            } else if (bow_on_zr && !__atomic_load_n(&oot_vr_quick_open, __ATOMIC_ACQUIRE)) {
                int hos_vr_bomb_in_reach(int hand);   /* (a grip near a bomb takes the bomb, not the bow) */
                for (int k = 0; k < 2; k++)
                    if (grip[k] && (!grip_was[k] || selected_grip_pending) && !hos_vr_bomb_in_reach(k) && !hos_vr_sword_claims(k) &&
                        !hos_vr_ocarina_claims(k)) {
                        selected_grip_pending = 0;
                        take_hand = k, phase = TAKING_OUT, phase_at = now, zr = 1;
                        hos_log("vr: %s grip takes the bow out (the game's ZR held)", k ? "right" : "left");
                        break;
                    }
            }
            break;
        case TAKING_OUT:   /* ZR held until the bow is out, then let go: it rests in the hand, not aimed */
            zr = 1;
            if (bow_out) {
                phase = HOLDING, phase_at = now, zr = 0;
                hos_log("vr: the bow is out: ZR let go, the bow rests until the string is drawn");
            } else if (now - phase_at > 700000000LL) {
                hos_log("vr: bow take-out timed out: %s", selected_bow == 1 ? "selection still confirms bow; release and retry grip" : "unknown ZR item; grips released");
                if (selected_bow != 1) bow_on_zr = 0, distrusted = selection;
                phase = IDLE;
                zr = 0;
            }
            break;
        case HOLDING:   /* ZR only while the string is drawn past its commit: the game aims, and the let-go is its shot */
            if (!bow_out && regrip && bow_hand >= 0 && grip[bow_hand] && vr_stow_regrip_lost(&away, 0, now)) {
                /* (taken again as it was being put away, and the game's last request put it away all the same) */
                phase = TAKING_OUT, phase_at = now, zr = 1, regrip = 0;
                hos_log("vr: bow: put away just as it was taken again: the game's ZR takes it out again");
                break;
            }
            if (!bow_out) { phase = IDLE; zr = 0; regrip = 0; break; }   /* (put away by the game: a cutscene, a ledge) */
            if (regrip && !vr_stow_regrip_lost(&away, 0, now)) regrip = 0;
            zr = fresh && (flags & 2);
            if (bow_hand >= 0 && !grip[bow_hand] && !(fresh && (flags & 3))) {
                phase = PUTTING_AWAY, phase_at = now, zr = 0, regrip = 0;
                hos_log("vr: bow grip let go: put away");
                vr_stow_start(&away, now);
                vr_stow_step(&away, bow_out, now, "bow");   /* (asked at once) */
            }
            break;
        case PUTTING_AWAY:   /* the game's own put-away, asked until the bow is away (vr_stow_step); never B */
            if (bow_hand >= 0 && grip[bow_hand]) {   /* (taken again before it went away) */
                regrip = vr_stow_busy(&away);
                vr_stow_cancel(&away, "bow");
                phase = HOLDING;
                break;
            }
            if (vr_stow_step(&away, bow_out, now, "bow") != kVrStowWaiting) phase = IDLE;   /* (away, or failed: logged) */
            break;
        }
        for (int k = 0; k < 2; k++) grip_was[k] = grip[k];
        __atomic_store_n(&oot_vr_bow_hand_now, phase != IDLE ? take_hand : -1, __ATOMIC_RELAXED);
        const int owned = bow_on_zr || bow_out || phase != IDLE;
        __atomic_store_n(&oot_vr_bow_grips_owned, owned, __ATOMIC_RELAXED);
        if (owned) result = (buttons & ~ZR) | (zr ? ZR : 0);   /* (not owned: not learned yet, the player's ZR teaches it) */
        if (zr != holding) hos_log("vr: bow %s the game's ZR", zr ? "holds" : "lets go of"), holding = zr;
        passthrough = 0;
    } else {
        __atomic_store_n(&oot_vr_bow_grips_owned, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_bow_hand_now, -1, __ATOMIC_RELAXED);
        phase = IDLE;
        if (!fresh) {
            passthrough = (buttons & ZR) != 0;   /* (the press that brings the bow out stays the player's) */
            if (holding) hos_log("vr: bow string ZR let go (the string's solution or the bow went away)");
            holding = 0;
        } else {
            if (passthrough && !(buttons & ZR)) passthrough = 0;
            const int want = passthrough ? (buttons & ZR) != 0 : (flags & 2) != 0;
            if (want != holding && !passthrough)
                hos_log("vr: bow string %s the game's ZR", want ? "holds" : "lets go of");
            holding = !passthrough && want;
            result = (buttons & ~ZR) | (want ? ZR : 0);
        }
    }
    const uint64_t was = __atomic_exchange_n(&oot_vr_bow_mapped_buttons,result,__ATOMIC_RELAXED);
    /* (observation only) every mapped button edge the game sees while the bow is out in first person. Bounded. */
    static unsigned edges;
    if (result != was && bow_out && __atomic_fetch_add(&edges, 1, __ATOMIC_RELAXED) < 400)
        hos_log("vr: bow aim input edge %#llx -> %#llx (pressed %#llx released %#llx)", (unsigned long long)was,
                (unsigned long long)result, (unsigned long long)(result & ~was), (unsigned long long)(was & ~result));
    return result;
}
static float oot_vr_turn_yaw;
static int64_t oot_vr_turn_at;

static float vr_load(Ctx *c, uint64_t addr) {
    const uint32_t bits = recomp_load32(c, addr);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void vr_store(Ctx *c, uint64_t addr, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    recomp_store32(c, addr, bits);
}

int rx_vr_bow(float out[12]);
void rx_vr_bow_active(int active);
static void oot_vr_tp_aim_shot(Ctx *c, uint64_t address, int phase);   /* (vr/items/aim/third_person_aim_host.c) */
static _Thread_local Ctx *oot_vr_bow_context;
static _Thread_local int oot_vr_bow_player, oot_vr_bow_rumbled;
/* Scope ownership to the actual shoot call, not a time window shared with unrelated projectiles. */
void hos_oot_vr_bow_probe(Ctx *c, uint64_t address, int phase) {
    if (phase == 3) { oot_vr_bow_context = NULL; oot_vr_bow_player = 0; return; }
    if (!phase) { oot_vr_bow_context = NULL; oot_vr_bow_player = 0; }
    HookshotVrHead h;
    oot_vr_tp_aim_shot(c, address, phase);   /* (third person: vr/items/aim/third_person_aim_host.c) */
    if (!address || !rx_vr_head(&h) || !h.active || h.mode != 1) return;
    const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const uint64_t player = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    if (!actor || !player) return;
    const uint32_t state = recomp_load32(c,player+0x1f60);
    if (phase == 0 && recomp_load64(c,address+0x18) != actor) return;
    if (phase && !((state & 0x2000000u) || (state & 0xffu)==0x43u || (state & 0xf00u)==0x400u)) return;   /* (0x446: a shot drawn without the aim camera) */
    if (!phase) { oot_vr_bow_context = c; oot_vr_bow_player = 1; oot_vr_bow_rumbled = 0; }
    else if (oot_vr_bow_context != c || !oot_vr_bow_player) return;
#ifdef HOOKSHOT_VR_RESEARCH
    static unsigned count;
    const int report = __atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<96;   /* (the launch probe's report: research builds) */
#else
    enum { report = 0 };
#endif
    if (!phase) {
        if (report) hos_log("vr: bow launch probe shoot owner %#llx player %#llx state %#x pose %u",(unsigned long long)address,
                (unsigned long long)player,state,h.seq);
        if (report) hos_log("vr: bow fire input mapped buttons %#llx ZR %u",(unsigned long long)__atomic_load_n(&oot_vr_bow_mapped_buttons,__ATOMIC_RELAXED),
                           (unsigned)(__atomic_load_n(&oot_vr_bow_mapped_buttons,__ATOMIC_RELAXED)>>9&1));
        return;
    }
    float a[3],b[3],v[3];
    for (int k=0;k<3;k++) {
        a[k]=vr_load(c,address+0x1948+4*k);
        b[k]=vr_load(c,address+0x1954+4*k);
        v[k]=vr_load(c,address+0x193c+4*k);
    }
    if (report) hos_log("vr: bow launch probe phase %d state %#x pose %u basis %#llx A(%.5f %.5f %.5f) B(%.5f %.5f %.5f) C(%.5f %.5f %.5f)",
            phase,state,h.seq,(unsigned long long)address,a[0],a[1],a[2],b[0],b[1],b[2],v[0],v[1],v[2]);
    float aim[12];
    if (!rx_vr_bow(aim)) return; // bow state, not a hard-coded physical grip, owns hand-directed aim
    /* The nine recorded shots have |A|=1.875, B-C parallel A. The descriptor copies A to launch record +0x7c,
       C to position +0xa0 and B to the optional forward target +0xac before projectile initialization.
       Preserve the game's scalar speed and target distance; rotate its coherent launch frame together. */
    const float speed = sqrtf(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
    const float span = sqrtf((b[0]-v[0])*(b[0]-v[0])+(b[1]-v[1])*(b[1]-v[1])+(b[2]-v[2])*(b[2]-v[2]));
    const float norm = sqrtf(aim[3]*aim[3]+aim[4]*aim[4]+aim[5]*aim[5]);
    if (!isfinite(speed+span+norm) || speed<.01f || speed>1000 || span<.01f || fabsf(norm-1)>.01f) return;
    /* The arrow flies along the headset reticle's own line: from the VR bow's handle (aim[0..2]) in the solved direction, so
       reticle and shot agree at every distance (gravity aside). Before, it started at the game's muzzle (where the GAME's
       bow is) and converged on the reticle's 10 m point; past 10 m it kept drifting to the far side of that offset (shots
       wide to the right at long range). The start moves only within 2 m of the game's muzzle (the first string shots were
       1.1-1.4 m from it: the game holds its bow out on its own side), so it is
       never placed across a room; otherwise the old convergence stays. Speed and the game's target distance are kept.
       (Not collision-checked: a handle pushed through a wall can start the arrow beyond it.) */
    float start[3], direction[3];
    const float dx=aim[0]-v[0], dy=aim[1]-v[1], dz=aim[2]-v[2];
    const float moved=sqrtf(dx*dx+dy*dy+dz*dz);
    const int from_hand = isfinite(moved) && moved < 2.0f;
    if (from_hand) {
        for (int k=0;k<3;k++) start[k]=aim[k], direction[k]=aim[3+k]/norm;
    } else {
        for (int k=0;k<3;k++) start[k]=v[k], direction[k]=aim[k]+10.0f*aim[3+k]-v[k];
        const float reach=sqrtf(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]);
        if (!isfinite(reach) || reach<.1f || reach>100) return;
        for (int k=0;k<3;k++) direction[k]/=reach;
    }
    for (int k=0;k<3;k++) {
        vr_store(c,address+0x1948+4*k,speed*direction[k]);
        vr_store(c,address+0x1954+4*k,start[k]+span*direction[k]);
        if (from_hand) vr_store(c,address+0x193c+4*k,start[k]);
    }
    /* The bow hand feels the game's shot: a strong short buzz, once per shot (the first launch basis of this shoot call) */
    if (!oot_vr_bow_rumbled) oot_vr_bow_rumbled = 1, rx_vr_rumble(((int)aim[10] & 4) ? 1 : 0, 1.0f, 0.09f);
    if (report) hos_log("vr: hand-directed bow shot pose %.0f direction %.4f %.4f %.4f speed %.4f (%s, %.2f m from the game's muzzle)",
                       aim[11],direction[0],direction[1],direction[2],speed,from_hand ? "from the VR bow" : "converging from the game's muzzle",moved);
}
