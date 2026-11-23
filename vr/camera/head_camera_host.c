/* Hookshot VR: The headset's view of the game camera: the head pose the replay publishes (HookshotVrHead) and the game's
   camera-obstruction fade, kept off in first person.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* (OoT VR) Two camera layers: the game owns orbit/body direction; the headset owns the rendered look. */
typedef struct { uint32_t seq; int active, mode, move_head, level, height_cm, body_depth_cm; float q[4], p[3]; float lean, tan_half_x, tan_half_y; int detail, foliage; } HookshotVrHead;
int rx_vr_head(HookshotVrHead *out);
void rx_vr_drawn(uint32_t seq, const float view[12]);
void rx_vr_body(uint32_t seq, int active, int child, const float root[3], const float right[3], const float back[3]);
void rx_vr_equipment(uint32_t seq, uint32_t known, uint32_t drawn, const uint64_t owners[2]);
volatile float hos_vr_fov_y, hos_vr_aspect;   /* read by nx2/oot_wide.py's perspective update */
static volatile float hos_vr_game_fovy = 0.8726646f;   /* the game camera's own FOV (radians), as last seen */
static volatile uint32_t oot_vr_move_yaw_bits;
static volatile int64_t oot_vr_move_seen;
static volatile int32_t oot_vr_turn_stick;
static volatile int64_t oot_vr_turn_input_seen, oot_vr_turn_bow_seen;
static volatile int64_t oot_vr_bow_out_seen;   /* first person, the bow out in Link's hand (any of its states), as last seen */
int rx_vr_bow(float out[12]);
void rx_vr_rumble(int hand, float amplitude, float seconds);
static volatile uint64_t oot_vr_bow_mapped_buttons;
/* (OoT VR, BODY-13) The game's camera fade on Link himself, kept off in first person (VR tab "Link's camera fade": Off).
   The game fades the actors its camera comes too close to, Link too: its camera's alpha update (the routine at 0x8073e6d0,
   run from the game camera's update at 0x8073cb30) works out, for the actors it follows (Link among them: it compares each
   actor's id with the camera's target list), an alpha clamp(..., 0, 1) from the camera's distance and stores it in the
   actor's ActiveCameraAlpha component (store at 0x807414a8: component + 0x44, a float, 1 opaque); the model code reads that
   component again (0x8051ec60). The component is the actor's component table + 0x108 (actor + 0x270 -> +0x108); its byte
   +0x48 bit 0 is the switch the game's own one-shot OneShotActiveCameraAlphaChangeEnable sets and clears (0x828cfbf0).
   In first person the camera is inside Link's head, so that distance changes as he walks, climbs and the head moves: his
   whole first-person body (and what is drawn with his alpha) could fade or blink.
   First person with the tracked body: the switch is cleared (as the one-shot does) and the alpha held at 1; leaving first
   person, or the row set to Game, puts the switch back as it was. Every change of what the game had there is logged (the
   alpha seen below 1 before the write: the game wrote it since the last frame), so the next headset log shows whether the
   game was fading Link at all.
   Not proven in the headset (18 Nov: static reading of the game's code only); the log lines below settle it.
   What this replaced: the mod cleared bit 0x400 of the halfword at [**0x84c9e818] + 0x250 to stop the fade. That bit is set
   by the camera AI node ExecuteCameraUpdateGameCameraAlpha while it runs (0x82681de0, cleared on its exit at 0x82681e40),
   and its only reader found in the game (0x8073cb64) SKIPS the game camera's own alpha update while it is set: it was never
   seen set in first person (no "camera fade off" line in two headset runs), and clearing it could only have let the game
   camera's fade run while that node wanted it held. */
int rx_option(int id, int otherwise);
int hos_oot_vr_first_person(void);   /* (vr/camera/view_host.c: first person drove the live gameplay view lately) */
#define OOT_VR_ALPHA_COMPONENT 0x108   /* the component table's ActiveCameraAlpha */
#define OOT_VR_ALPHA_VALUE 0x44        /* its alpha (float, 0 faded out .. 1 opaque) */
#define OOT_VR_ALPHA_SWITCH 0x48       /* its byte whose bit 0 the game's OneShotActiveCameraAlphaChangeEnable sets */
static int oot_vr_fade_ptr(uint64_t a, uint64_t n) { return a >= HOS_HEAP_BASE && !(a & 7) && hos_guest_range(a, n) != NULL; }
static void oot_vr_camera_fade(Ctx *c, int off) {
    static uint64_t component;   /* the one whose switch the mod cleared (0: none), and what the switch was */
    static uint32_t switch_was;
    static int64_t told_at, frames, below;   /* the log's count since its last line: frames, and those with alpha below 1 */
    static float lowest = 1.0f;
    static int was_below = -1;
    const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const uint64_t table = oot_vr_fade_ptr(actor, 0x278) ? recomp_load64(c, actor + 0x270) : 0;
    const uint64_t now = oot_vr_fade_ptr(table, OOT_VR_ALPHA_COMPONENT + 8) ? recomp_load64(c, table + OOT_VR_ALPHA_COMPONENT) : 0;
    const uint64_t alpha_component = oot_vr_fade_ptr(now, OOT_VR_ALPHA_SWITCH + 8) ? now : 0;
    /* (first person with the tracked body and the row at Off, while the headset's view drives the game's camera: an event shown
       as a flat screen keeps the game's own camera, and with it the game's fade) */
    const int keep = off && rx_option(920, 0) && hos_oot_vr_first_person();
    if (component && (component != alpha_component || !keep)) {   /* (left first person, Game chosen, or another Link) */
        if (component == alpha_component) {
            const uint32_t b = recomp_load8(c, component + OOT_VR_ALPHA_SWITCH);
            recomp_store8(c, component + OOT_VR_ALPHA_SWITCH, (uint8_t)((b & ~1u) | (switch_was & 1u)));
        }
        hos_log("vr: Link's camera fade: the game's again (%s)", component == alpha_component ? "switch put back" :
                "Link's actor changed");
        component = 0;
    }
    {   /* (once per Link actor: whether he has the component at all; without one the game cannot fade him this way) */
        static uint64_t said_actor;
        if (off && actor && actor != said_actor) {
            said_actor = actor;
            hos_log("vr: Link's camera fade: %s", alpha_component ? "Link has the camera-alpha component" :
                    "Link has no camera-alpha component (component table + 0x108 empty): nothing to keep off");
        }
    }
    if (!alpha_component || !off) return;
    uint32_t bits = recomp_load32(c, alpha_component + OOT_VR_ALPHA_VALUE);
    float alpha;
    memcpy(&alpha, &bits, sizeof alpha);
    const uint32_t sw = recomp_load8(c, alpha_component + OOT_VR_ALPHA_SWITCH);
    {   /* (research, bounded) the game's alpha for Link as it was before this frame's write: a line when it goes below 1 or back
           (at most every 2 s, with the frames since the last line), so the headset log shows whether the game fades him */
        const int is_below = !(alpha >= 0.995f);
        frames++, below += is_below;
        if (is_below && alpha < lowest) lowest = alpha;
        const int64_t t = hos_now_ns();
        if (is_below != was_below && t - told_at > 2000000000LL) {
            hos_log("vr: Link's camera fade: alpha %.2f now (switch %u); since the last line %lld of %lld frames below 1, the "
                    "lowest %.2f%s", alpha, sw & 1u, (long long)below, (long long)frames, lowest,
                    !keep ? " (the game's fade: row Link's camera fade at Game)" :
                    component && is_below ? ": the game faded him again after the mod held him at 1" : "");
            was_below = is_below, told_at = t, frames = below = 0, lowest = 1.0f;
        }
    }
    if (!keep) return;
    if (!(isfinite(alpha) && alpha >= 0.0f && alpha <= 1.0f)) {   /* (not an alpha: an unknown layout, never written) */
        static int warned;
        if (!warned++) hos_log("vr: Link's camera fade: +0x44 holds %#x, not an alpha: left alone", bits);
        return;
    }
    if (!component) {
        component = alpha_component, switch_was = sw;
        recomp_store8(c, component + OOT_VR_ALPHA_SWITCH, (uint8_t)(sw & ~1u));
        hos_log("vr: Link's camera fade: off in first person (switch was %u, alpha %.2f)", sw & 1u, alpha);
    } else if (sw & 1u) {
        recomp_store8(c, component + OOT_VR_ALPHA_SWITCH, (uint8_t)(sw & ~1u));   /* (the game set it again: an event's one-shot) */
    }
    if (alpha != 1.0f) {
        const float one = 1.0f;
        memcpy(&bits, &one, sizeof bits);
        recomp_store32(c, alpha_component + OOT_VR_ALPHA_VALUE, bits);
    }
}
