/* Hookshot VR: The sticks in first person: the gameplay-camera check, the left stick turned to the headset's heading, and the bow's
   aim pitch made from the line between the hands.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* The caller already selected the live gameplay view. An active EventMgr entry is the reliable cutscene boundary.
   Do not gate this on the player camera's internal state: ordinary traversal can use more states than mouse-look does.
   (VR menu, Cutscene view: during an event the headset may keep the view (the event moves the same camera, camera zero, the
   player camera's view: headless 17 Nov, a chest's event; 19 Nov, a talk, an arrival and a story cinematic). How each kind
   of event is shown is decided in vr/camera/events_host.c.) */
int rx_option(int id, int otherwise);
/* The playing event's name into name[64] ("" when it cannot be read): EventMgr +0x68, its +0x8 (headless 17 Nov:
   Dm_SY_BancChangeArrive on arriving, a chest's BottomOfTheWell_GetGlassTruth). Each new name is logged once. Also read by
   the heading's re-align after events (vr/camera/view_host.c). */
static int oot_vr_event_name(Ctx *c, uint64_t events, char name[64]) {
    name[0] = 0;
    const uint64_t holder = recomp_load64(c, events + 0x68);
    if (holder < 0x80000000ULL || !hos_guest_range(holder, 16)) return 0;
    const uint64_t text = recomp_load64(c, holder + 8);
    if (text < 0x80000000ULL || !hos_guest_range(text, 64)) return 0;
    int n = 0;
    for (; n < 63; n++) { const char ch = (char)recomp_load8(c, text + n); if (ch < 32 || ch > 126) break; name[n] = ch; }
    name[n] = 0;
    static char told[64];
    if (n && strcmp(told, name)) snprintf(told, sizeof told, "%s", name), hos_log("vr: event %s", name);
    return n > 0;
}
/* A story cinematic's name: Dm_ (or DmF_), an area, then a number (Dm_Kokiri_0030). The item get's test
   (vr/items/item_get/item_get_host.c). */
static int oot_vr_cinematic_name(const char *name) {
    const char *p = !strncmp(name, "Dm_", 3) ? name + 3 : !strncmp(name, "DmF_", 4) ? name + 4 : NULL;
    if (!p) return 0;
    const char *u = strchr(p, '_');
    return u && u[1] >= '0' && u[1] <= '9';
}
/* (vr/camera/events_host.c) the playing event's update is shown as a flat screen (the world screen): Cutscene view, the kind
   of event and its camera decide it there (oot_vr_event_frame, each update before this check) */
static int oot_vr_event_flat(void);
static int oot_vr_player_camera(Ctx *c, uint64_t view) {
    const uint64_t camera = oot_gameplay_camera(c);
    if (!camera || recomp_load64(c, camera + 120) != view) return 0;
    return !oot_vr_event_flat();
}

/* Called after the ordinary pad merge. It changes only left-stick direction, never the right-stick camera. */
/* (OoT VR, bow) While Link holds the bow raised in first person, the right stick's up/down is made from the line between the
   two hands: it tilts the camera (the game's aim pitch) until it matches the line from the rear hand to the front one. Left/right
   stays the player's stick (the game aims sideways by turning Link). The game then aims, places its reticle and fires along it as usual.
   (Found from vr-aim-dump.bin: the player object's +0x1f60 holds bit 0x2000000 with the bow up and 0x46 in its low byte while
   aiming; a full right stick turns the aim about 60 degrees a second.) */
int rx_vr_hands(void *out);
static volatile int32_t oot_vr_aim_stick[2];
static volatile int64_t oot_vr_aim_seen;
static void oot_vr_aim_steer(Ctx *c, const float pos[3], const float look[3], const float right[3], const float up[3],
                             const float back[3]) {
    const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const uint64_t player = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    if (!actor || !player) return;
    const uint32_t state = recomp_load32(c, player + 0x1f60);
    if (!(state & 0x2000000u) || (state & 0xffu) != 0x46u) return;
    uint32_t w[64] = {0};
    if (!rx_vr_hands(w)) return;
    float p[2][3];
    for (int h = 0; h < 2; h++) {
        if (!w[1 + 26 * h]) return;   /* (both hands tracked) */
        memcpy(p[h], &w[1 + 26 * h + 5], sizeof p[h]);   /* (grip_p: after valid and grip_q) */
    }
    /* the front hand is the bow hand (either way round): more forward is more negative z in the recentred frame */
    const int front = p[0][2] < p[1][2] ? 0 : 1;
    const float dl[3] = {p[front][0] - p[!front][0], p[front][1] - p[!front][1], p[front][2] - p[!front][2]};
    if (dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2] < 0.15f * 0.15f) return;
    float dw[3];
    for (int k = 0; k < 3; k++) dw[k] = right[k] * dl[0] + up[k] * dl[1] + back[k] * dl[2];
    const float n = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
    const float want_yaw = atan2f(dw[0], dw[2]), want_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, dw[1] / n)));
    const float yaw = atan2f(vr_load(c, actor + 816), vr_load(c, actor + 840));
    float d[3] = {look[0] - pos[0], look[1] - pos[1], look[2] - pos[2]};
    const float dn = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dn > 1e-4f)) return;
    const float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, d[1] / dn)));
    float ey = want_yaw - yaw;
    while (ey > 3.14159265f) ey -= 6.2831853f;
    while (ey < -3.14159265f) ey += 6.2831853f;
    const float ep = want_pitch - pitch;
    const float full = 15.0f * 0.0174533f;   /* (a full stick from 15 degrees off: proportional inside that) */
    float sx = -ey / full, sy = ep / full;   /* (yaw grows toward +x: turning right lowers it) */
    sx = fmaxf(-1.0f, fminf(1.0f, sx)), sy = fmaxf(-1.0f, fminf(1.0f, sy));
    if (fabsf(ey) < 0.006f) sx = 0;
    if (fabsf(ep) < 0.006f) sy = 0;
    __atomic_store_n(&oot_vr_aim_stick[0], (int32_t)(sx * 32767.0f), __ATOMIC_RELAXED);
    __atomic_store_n(&oot_vr_aim_stick[1], (int32_t)(sy * 32767.0f), __ATOMIC_RELAXED);
    __atomic_store_n(&oot_vr_aim_seen, hos_now_ns(), __ATOMIC_RELEASE);
    static int64_t told;
    const int64_t now = hos_now_ns();
    if (now - told > 1000000000LL)
        told = now, hos_log("vr: bow aimed by the hands: off by %.1f deg sideways, %.1f deg up (stick %.2f %.2f)",
                            ey * 57.2958f, ep * 57.2958f, sx, sy);
}

int hos_vr_radial_open(void);   /* (vr/menu/radial_host.c: 0 closed, 1 the item menu, 2 the equipment menu) */
/* When the left stick last asked Link to move (ns; 0 never): the heading's re-align after events counts only the turns Link
   makes without it (vr/camera/view_host.c). A push past kVrLeftStickMoving of full counts. */
#define kVrLeftStickMoving (0.2f * 32767.0f)
static volatile int64_t oot_vr_left_stick_seen;
void hos_oot_vr_input(int32_t stick[4]) {
    const int64_t now = hos_now_ns();
    const int radial = hos_vr_radial_open();
    if (radial == 1) stick[0] = stick[1] = 0;   /* (the item menu is open: the left stick picks, Link stands) */
    if (radial == 2) stick[2] = stick[3] = 0;   /* (the equipment menu: the right stick picks, the view does not turn) */
    if (hypotf((float)stick[0], (float)stick[1]) > kVrLeftStickMoving)
        __atomic_store_n(&oot_vr_left_stick_seen, now, __ATOMIC_RELEASE);
    const int64_t bow = __atomic_load_n(&oot_vr_turn_bow_seen, __ATOMIC_ACQUIRE);
    HookshotVrHead head;
    if (rx_vr_head(&head) && head.active && head.mode == 1) {   /* (first person: the right stick turns the view's heading) */
        __atomic_store_n(&oot_vr_turn_stick, stick[2], __ATOMIC_RELAXED);
        __atomic_store_n(&oot_vr_turn_input_seen, now, __ATOMIC_RELEASE);
        if (bow && now-bow < 100000000LL) stick[2] = 0; // (aiming or guarding: not the game's legacy bow yaw either)
        /* (the game never gets the right stick's up and down: the headset is the view's pitch. It tilted the game camera,
           most of all with Jump on right stick up, each jump tilting it further until the game culled the VR body, 17 Nov) */
        stick[3] = 0;
    }
    const int64_t seen = __atomic_load_n(&oot_vr_move_seen, __ATOMIC_ACQUIRE);
    if (!seen || hos_now_ns() - seen > 200000000LL) return;
    const uint32_t bits = __atomic_load_n(&oot_vr_move_yaw_bits, __ATOMIC_RELAXED);
    float yaw;
    memcpy(&yaw, &bits, sizeof yaw);
    const float co = cosf(yaw), si = sinf(yaw);
    const float x = co * stick[0] + si * stick[1];
    const float y = -si * stick[0] + co * stick[1];
    stick[0] = (int32_t)fmaxf(-32767.0f, fminf(32767.0f, x));
    stick[1] = (int32_t)fmaxf(-32767.0f, fminf(32767.0f, y));
}
