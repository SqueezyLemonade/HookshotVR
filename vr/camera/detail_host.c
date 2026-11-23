/* Hookshot VR: The headset's field of view in the game's projection, and model and foliage detail at the true distance from the eye
   (the presets of the VR tab). Called from nx2/oot_wide.py's hook sites.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* (OoT VR) Called by nx2/oot_wide.py inside sead's perspective-projection matrix update, after ldp s0, s<ratio>, [proj, #0xac].
   sead keeps the vertical FOV as four fields: angle +0xa0, sin(angle/2) +0xa4, cos(angle/2) +0xa8, tan(angle/2) +0xac (the
   aspect at +0xb0). The matrix reads the tan; culling reads the angle, sin and cos. All four get the headset's, together: the
   picture, culling, lighting and shadows all see the one real headset camera. Only a 16:9 camera (the gameplay one; with the
   headset's aspect already in, nx2/oot_wide.py's first site), only when the four fields really look like sead's. */
static int hos_vr_projection_locked(Ctx *c, uint64_t proj, float ratio, float *tan_out);
/* (the game's worker threads update their cameras at the same time: one at a time) */
int hos_vr_projection(Ctx *c, uint64_t proj, float ratio, float *tan_out) {
    static volatile int lock;
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {}
    const int r = hos_vr_projection_locked(c, proj, ratio, tan_out);
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    return r;
}

/* ---- (OoT VR) model and foliage detail ----------------------------------------------------------------------------------
   The game picks a model's detail from its size on screen: radius * 1/tan(FOV/2) / -depth, the depth along the view's forward
   axis, at 193 places (nx2/oot_wide.py _VR_DEPTH, one per kind of object). With the headset's wide FOV everything looked small,
   and with the head turning the view, depth (and detail) changed while nothing moved: models flickered between levels. So, for
   the headset camera's views: the depth becomes the true distance from the eye (detail follows where you are, never where you
   look), and the view snapshot (main+0x33a8c0, rebuilt every frame) keeps 1/tan(FOV/2) of a reference FOV, the VR tab's Detail
   distance (Switch 2: the game's own FOV). Foliage reads the snapshot's tan(FOV/2) (through copies of it, on the GPU too): it
   gets the Foliage distance's; the two readers of it that light and shadow the scene (main+0x6bc924, 0x335ad0) get the
   headset's back. Culling builds its frustum 12% wider than the picture (in tan), so fast head turns never outrun it. */
static int hos_vr_detail, hos_vr_foliage;   /* the VR tab's Detail / Foliage distance (0 Low .. 3 Ultra), from rx_vr_head */
static float hos_vr_preset_fovy(int preset) {   /* the reference FOV of a preset (radians): Switch 2 is the game camera's own */
    switch (preset) {
    case 0: return 70.0f / 57.2958f;
    case 2: return 35.0f / 57.2958f;
    case 3: return 25.0f / 57.2958f;
    case 4: return hos_vr_fov_y;   /* (Off: the game's own calculation with the headset's FOV, as without the fix) */
    default: return hos_vr_game_fovy;
    }
}
/* a view snapshot of the headset camera: its FOV is the headset's (or the guard band's) */
static int hos_vr_snapshot_ours(Ctx *c, uint64_t snap) {
    const float fov = hos_vr_fov_y, f = snap ? vr_load(c, snap + 0x1d0) : 0.0f;
    return fov > 0.2f && f > hos_vr_game_fovy + 0.1f && fabsf(f - fov) < 0.2f;
}
/* main+0x33ab44, the view snapshot's ldr s0, [x2, #0xa0]: the FOV its frustum is built from (the guard band) */
int hos_vr_frustum_fovy(Ctx *c, uint64_t proj, float in, float *out) {
    static int told;
    const float fov = hos_vr_fov_y;
    (void)proj;
    if (!(fov > 0.2f) || fabsf(in - fov) > 0.002f) return 0;
    *out = 2.0f * atanf(tanf(0.5f * fov) * 1.12f);
    if (!told) told = 1, hos_log("vr: culling frustum %.1f degrees for the %.1f degree picture", *out * 57.2958f, fov * 57.2958f);
    return 1;
}
/* main+0x33af7c, after the frustum is made: the snapshot's FOV values the picture's own (lighting and shadows read them with the
   real projection), 1/tan(FOV/2) the Detail distance's, tan(FOV/2) the Foliage distance's */
void hos_vr_lod_snapshot(Ctx *c, uint64_t snap) {
    static int told;
    if (!hos_vr_snapshot_ours(c, snap)) return;
    const float fov = hos_vr_fov_y, d = hos_vr_preset_fovy(hos_vr_detail), f = hos_vr_preset_fovy(hos_vr_foliage);
    vr_store(c, snap + 0x1d0, fov);
    vr_store(c, snap + 0x1d8, tanf(fov));
    vr_store(c, snap + 0x1dc, tanf(0.5f * f));
    vr_store(c, snap + 0x1e0, 1.0f / tanf(0.5f * d));
    if (!told) told = 1, hos_log("vr: detail as a %.1f degree camera, foliage as a %.1f degree one", d * 57.2958f, f * 57.2958f);
}
/* main+0x6bc924 and 0x335ad0, the readers of the snapshot's tan(FOV/2) that light and shadow the scene: the headset's */
int hos_vr_tan_reader(Ctx *c, uint64_t snap, int reader, float in, float *out) {
    (void)reader, (void)in;
    if (!hos_vr_snapshot_ours(c, snap)) return 0;
    *out = tanf(0.5f * hos_vr_fov_y);
    return 1;
}
/* the 193 places: the true distance from the eye for the depth along the view, in front of a headset camera's view */
int hos_vr_lod_depth(Ctx *c, uint64_t snap, uint64_t obj, float z, float *out) {
    static int told;
    if (!(z < 0.0f) || !obj || !hos_vr_snapshot_ours(c, snap)) return 0;
    float r[12];
    for (int i = 0; i < 12; i++) r[i] = vr_load(c, snap + 0xa8 + 4 * i);   /* the view's rows: right, up, back (x y z w each) */
    const float ex = -(r[0] * r[3] + r[4] * r[7] + r[8] * r[11]), ey = -(r[1] * r[3] + r[5] * r[7] + r[9] * r[11]),
                ez = -(r[2] * r[3] + r[6] * r[7] + r[10] * r[11]);
    const float dx = vr_load(c, obj) - ex, dy = vr_load(c, obj + 4) - ey, dz = vr_load(c, obj + 8) - ez;
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(dist) || dist < -z * 0.999f) return 0;   /* (never less than the depth: anything else is not this view) */
    *out = -dist;
    if (!told) told = 1, hos_log("vr: model detail by distance from the eye");
    return 1;
}
