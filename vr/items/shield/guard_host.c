/* Hookshot VR: Untargeted shielding: the guard faces where the headset looks ("Shield follows VR heading").

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* Untargeted shielding, OoT 1.1.1. See technical-reference V-GUARD-NATIVE.
   Native QueryTargetIsGuarding -> 83082c40 -> 82d2b170: component[248],
   concrete guard vtable 84b6a258, halfword +34 bit8. This is NOT "shield drawn".
   The lock-on target: oot_vr_lock_point (component[358], the combat component). */
typedef struct {
    uint64_t actor;
    int guarding, have_last;
    int64_t seen;
    float last_yaw, pinned_yaw, last_turn, facing;
} OotVrGuardView;
static OotVrGuardView oot_vr_guard_view;
static volatile uint64_t oot_vr_guard_actor;
static volatile uint32_t oot_vr_guard_yaw_bits;
static volatile int64_t oot_vr_guard_seen;
/* Source 8f5a7d08 headset log: the native query bit repeatedly disappeared while
   action 408 was unchanged. Retain a CONFIRMED guard only within that exact
   observed action and owned combat instance, never from "shield drawn" alone. */
static volatile uint64_t oot_vr_guard_confirmed_actor, oot_vr_guard_confirmed_combat;

static void oot_vr_guard_forget_native(void) {
    __atomic_store_n(&oot_vr_guard_confirmed_actor,0,__ATOMIC_RELEASE);
    __atomic_store_n(&oot_vr_guard_confirmed_combat,0,__ATOMIC_RELEASE);
}

static uint64_t oot_vr_guard_ptr(Ctx *c, uint64_t addr) {
    if (!addr || !hos_guest_range(addr, 8)) return 0;
    const uint64_t p = recomp_load64(c, addr);
    return p >= HOS_HEAP_BASE && !(p & 7) && hos_guest_range(p, 8) ? p : 0;
}

/* Link's lock-on (Z-targeting): 1 locked on, with the target's point (world) in point[3] if asked; 0 not locked on; -1
   unknown (the chain did not validate). Found headless 18 Nov (a Deku Baba placed on the VR test map, ZL held, the combat
   component's words dumped at each action change): while locked on, +0x1c38 bit 0 is set and +0x1c14/18/1c is a point on
   the target (about 1.7 m up a Deku Baba, swaying with it); without a target its x is a NaN and the bit clear, with ZL held
   or not (the strafe, 0x420, is the same action either way). The ActorLink at +0x1c20/+0x1c28 (QueryPlayerGetLockOnTarget-
   ActorLink, 82ae3890) read -1 throughout, locked on or not: the earlier check on it never saw a target. */
static int oot_vr_lock_point(Ctx *c, uint64_t actor, float point[3]) {
    if (!actor || !hos_guest_range(actor, 0x278) || recomp_load64(c,actor)!=0x84b45e18ULL) return -1;
    const uint64_t table=oot_vr_guard_ptr(c,actor+0x270);
    const uint64_t combat=table ? oot_vr_guard_ptr(c,table+0x358) : 0;
    if (!combat || !hos_guest_range(combat,0x1c40) || recomp_load64(c,combat+0x18)!=actor) return -1;
    float p[3];
    for (int k=0;k<3;k++) { const uint32_t u=recomp_load32(c,combat+0x1c14+4*k); memcpy(&p[k],&u,4); }
    if (!(recomp_load32(c,combat+0x1c38)&1) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return 0;
    if (point) memcpy(point,p,sizeof p);
    return 1;
}

/* Returns 1 only for a validated native guard with an explicitly EMPTY target.
   Once observed, guard intent persists through transient query-bit clears only
   in the SAME 0x408 action. Target presence/unknown, owner/mode changes or action
   transitions revoke it immediately; this is not a time-based sticky guard.
   Unknown, non-player, target present and other equipment all retain native behavior. */
static int oot_vr_guard_native(Ctx *c, uint64_t actor, uint64_t *combat_out) {
    if (!actor || !hos_guest_range(actor, 0x278) || recomp_load64(c,actor)!=0x84b45e18ULL) goto unknown;
    const uint64_t table=oot_vr_guard_ptr(c,actor+0x270);
    const uint64_t combat=table ? oot_vr_guard_ptr(c,table+0x358) : 0;
    const uint64_t guard=table ? oot_vr_guard_ptr(c,table+0x248) : 0;
    if (!combat || !guard || !hos_guest_range(combat,0x1c40) || !hos_guest_range(guard,0x36) ||
        recomp_load64(c,combat+0x18)!=actor || recomp_load64(c,guard)!=0x84b6a258ULL ||
        recomp_load64(c,guard+0x18)!=actor) goto unknown;
    if (combat_out) *combat_out=combat;
    /* Locked on (or unknown): the game's own guard, facing the target */
    if (oot_vr_lock_point(c,actor,NULL)!=0) goto unknown;
    const uint32_t action=recomp_load32(c,combat+0x20); // root Player+1f60, SAME field at its correct subcomponent base
    const int native=(recomp_load16(c,guard+0x34)&0x100)!=0;
    if (action!=0x408u) { oot_vr_guard_forget_native(); return native; }
    if (native) {
        __atomic_store_n(&oot_vr_guard_confirmed_combat,combat,__ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_guard_confirmed_actor,actor,__ATOMIC_RELEASE);
    }
    const int retained=__atomic_load_n(&oot_vr_guard_confirmed_actor,__ATOMIC_ACQUIRE)==actor &&
                       __atomic_load_n(&oot_vr_guard_confirmed_combat,__ATOMIC_RELAXED)==combat;
    static volatile unsigned reports;
    if (!native && retained && __atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<16)
        hos_log("vr: untargeted guard query bit cleared; retaining confirmed action 0x408");
    return native || retained;
unknown:
    oot_vr_guard_forget_native();
    return 0;
}

/* Link has a lock-on target: 1 yes, 0 none, -1 unknown (oot_vr_lock_point) */
static int oot_vr_lock_target(Ctx *c, uint64_t actor) { return oot_vr_lock_point(c, actor, NULL); }

static void oot_vr_guard_reset(void) {
    memset(&oot_vr_guard_view,0,sizeof oot_vr_guard_view);
    __atomic_store_n(&oot_vr_guard_seen,0,__ATOMIC_RELEASE);
    oot_vr_guard_forget_native();
}

/* Pure yaw policy, also used by the recorded/native-memory regression harness.
   Input yaw already includes the existing bow-turn offset. Preserve the last
   rendered base on guard entry (the native camera may already have snapped).
   While guarding, only explicit stick turn changes the pinned base. On release,
   rebase the existing offset once so ending guard does not itself snap the view. */
static float oot_vr_guard_yaw(OotVrGuardView *s, uint64_t actor, int guarding,
                              float incoming, float turn, float head, float *offset) {
    const float tau=6.283185307f;
    if (s->actor!=actor) { memset(s,0,sizeof *s); s->actor=actor; }
    float out=incoming;
    if (guarding) {
        if (!s->guarding) {
            s->pinned_yaw=s->have_last?s->last_yaw:incoming;
            s->last_turn=turn;
            s->facing=remainderf(s->pinned_yaw+head+3.141592654f,tau);
        } else {
            const float delta=remainderf(turn-s->last_turn,tau);
            s->pinned_yaw=remainderf(s->pinned_yaw+delta,tau);
            s->facing=remainderf(s->facing+delta,tau);
            s->last_turn=turn;
        }
        out=s->pinned_yaw;
    } else if (s->guarding) {
        out=s->pinned_yaw;
        *offset=remainderf(*offset+remainderf(out-incoming,tau),tau);
    }
    s->last_yaw=out; s->have_last=1; s->guarding=guarding;
    return out;
}

static void oot_vr_guard_camera(Ctx *c, const HookshotVrHead *h, int guarding, float back[3]) {
    const uint64_t actor=__atomic_load_n(&oot_vr_player_actor,__ATOMIC_RELAXED);
    const int64_t now=hos_now_ns();
    if (h->mode!=1 || !rx_option(903,0) || !actor) { oot_vr_guard_reset(); return; }
    if (oot_vr_guard_view.seen && (now<oot_vr_guard_view.seen || now-oot_vr_guard_view.seen>200000000LL))
        oot_vr_guard_reset();
    const float flat=hypotf(back[0],back[2]);
    const float qx=h->q[0], qy=h->q[1], qz=h->q[2], qw=h->q[3];
    const float hx=2*(qx*qz+qw*qy), hz=1-2*(qx*qx+qy*qy);
    if (!isfinite(flat) || flat<.01f || !isfinite(hx) || !isfinite(hz) || hypotf(hx,hz)<.1f) {
        oot_vr_guard_reset(); return;
    }
    const int was=oot_vr_guard_view.guarding;
    const float view_yaw=atan2f(back[0],back[2]);   /* (the view's heading now, right stick turns included) */
    const float yaw=oot_vr_guard_yaw(&oot_vr_guard_view,actor,guarding,atan2f(back[0],back[2]),
                                    oot_vr_turn_yaw,atan2f(hx,hz),&oot_vr_turn_yaw);
    back[0]=sinf(yaw)*flat; back[2]=cosf(yaw)*flat;
    oot_vr_guard_view.seen=now;
    if (guarding) {
        /* The guard faces the way the stick moves Link: the view's heading, with the head's yaw in Headset mode (the game
           moves Link relative to the view). A guarding Link walks along this facing, so anything else veers: it was Link's
           posed torso, which follows the head only past a dead zone, and the heading pinned at the guard's start, which
           missed right stick turns (headset 18 Nov: "I will hold forward and veer right or left"). */
        const float facing=remainderf(view_yaw+(h->move_head?atan2f(hx,hz):0)+3.141592654f,6.283185307f);
        uint32_t bits; memcpy(&bits,&facing,4);
        __atomic_store_n(&oot_vr_guard_yaw_bits,bits,__ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_guard_actor,actor,__ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_guard_seen,now,__ATOMIC_RELEASE);
    } else __atomic_store_n(&oot_vr_guard_seen,0,__ATOMIC_RELEASE);
    static unsigned reports;
    if (was!=guarding && reports++<64)
        hos_log("vr: untargeted guard heading %s (view %.1f, native facing %.1f degrees)",
                guarding?"latched":"released",yaw*57.2958f,oot_vr_guard_view.facing*57.2958f);
}

/* main+93533c in the shared facing setter 809552f0, AFTER LDR s1,[x1,#8].
   All callers (including strafing) are covered, still only for owned untargeted
   guard. x0=combat; s3=x,s2=y,s1=z,s4=x*x+y*y. Replace these FOUR temporary
   scalars coherently. Never write caller memory: x1 need not be a scratch vector.
   Native s0 turn rate, flags, general registers, actor transforms/collision and
   gameplay target state stay untouched. Next FMADD adds z*z before FSQRT. */
void hos_vr_guard_direction(Ctx *c) {
    const int64_t seen=__atomic_load_n(&oot_vr_guard_seen,__ATOMIC_ACQUIRE), now=hos_now_ns();
    if (!seen || now<seen || now-seen>200000000LL || !rx_option(903,0)) return;
    const uint64_t confirmed=__atomic_load_n(&oot_vr_guard_confirmed_combat,__ATOMIC_ACQUIRE);
    if (confirmed && c->x[0]!=confirmed) return; // most calls belong to other actors
    HookshotVrHead h;
    if (!rx_vr_head(&h) || !h.active || h.mode!=1 || __atomic_load_n(&oot_vr_quick_open,__ATOMIC_ACQUIRE)) return;
    const uint64_t actor=__atomic_load_n(&oot_vr_guard_actor,__ATOMIC_RELAXED);
    uint64_t combat=0;
    if (actor!=__atomic_load_n(&oot_vr_player_actor,__ATOMIC_RELAXED) ||
        !oot_vr_guard_native(c,actor,&combat) || c->x[0]!=combat) return;
    const uint32_t bits=__atomic_load_n(&oot_vr_guard_yaw_bits,__ATOMIC_RELAXED);
    float yaw; memcpy(&yaw,&bits,4);
    if (!isfinite(yaw)) return;
    const float x=sinf(yaw), y=0, z=cosf(yaw), partial=x*x;
    memcpy(c->vreg[3],&x,4); memcpy(c->vreg[2],&y,4);
    memcpy(c->vreg[1],&z,4); memcpy(c->vreg[4],&partial,4);
    static volatile unsigned reports;
    if (__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<32)
        hos_log("vr: shared native untargeted guard facing redirected to %.1f degrees",yaw*57.2958f);
}
