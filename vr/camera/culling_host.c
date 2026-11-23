/* Hookshot VR: First-person culling: the game's culling planes moved out to a ball around the eye, so Link's own models (gauntlets,
   the held arrow, tunics) are not culled where the game's animation put them. And the projection's locked update.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it. Not compiled on its own. */
/* main+0x33b674, the view snapshot's last world plane stored (str s0, [x19, #0x24c]). The game culls each of Link's separate
   models (gauntlets, the held arrow, tunics) where ITS animation put them, not where the VR pose draws them. In first person
   the native hands hang at the hips, below the headset's view: vr-item-dump-132852667698202.bin has every gold-gauntlet and
   arrow colour draw missing for exactly the 292 frames the game's wrists were at 0.6-0.85 m, and back the frame its bow
   raise lifted them to 1.4 m (the depth/shadow passes, with their own snapshots, drew them all along). So for the headset
   camera's snapshot only, in first person only, every culling plane is moved out until a ball around the eye is inside it, and
   the frustum's bounding box grows to hold the same ball. The picture's projection, near/far values and detail are untouched;
   far from the eye the frustum is only this margin wider. The snapshot keeps (+0x08) four view-space side planes n.p = w,
   (+0x1f0) six world planes n.p + d = 0, (+0x250) the world box's min and (+0x25c) max, (+0xd8) camera-to-world rows. */
static volatile int hos_vr_cull_first_person;
static volatile float hos_vr_cull_eye[3];
static void hos_vr_cull_note_view(Ctx *c, uint64_t snap);   /* (the world-culling watch, below: the headset's view as built) */
int rx_option(int id, int otherwise);   /* (gpu/vk/vk_replay.cpp: the menu's values) */

/* The VR tab's "Headset frustum culling" (option 931), a TEST for objects that vanish (CAM-11): Off makes the same ball
   200 m wide, so the headset view's planes and box cull nothing near. The per-model view test (f_8009bb80) checks each
   model's box against these planes first (8009dd50..8009de54) and its sphere after (the hooks of
   vr/items/gear/gear_near_host.c); a model the box check drops never reaches those hooks. If a vanishing object comes
   back with Off, this view test dropped it; if it still vanishes, something else does. Costs frame rate (everything
   within 200 m is drawn, behind you too): a test, not a setting to keep.
   Wider (2): the four side planes turned 10 degrees outward about the eye (the picture is unchanged). The 16:54 headset log
   (18 Nov): with Off, objects that vanished at some angles came back; one reading is that the game culls a frame with an
   older head pose than the one that frame is shown at, so the side you turn toward loses objects first (the frustum is
   only 12% wider in tan than the picture, vr/camera/detail_host.c). The lag is measured below (vr_cull_lag_note). */
#define VR_CULL_TEST_MARGIN 200.0f
#define VR_CULL_WIDER (10.0f / 57.2958f)   /* radians: Wider's extra half-angle on each side */
static void vr_cull_lag_note(Ctx *c, uint64_t snap);   /* (below: how far this snapshot's look is from the newest headset view) */

/* Turns a side plane outward by `by` about the eye: n is its inward normal (any length), fw the view's forward (unit). The
   plane's inward normal leans forward by the frustum's half-angle a (n.fw = |n| sin a); the new one leans by a + by. Near and
   far planes (n along fw) and planes already wider than 80 degrees are left. Returns 0 when it left it. */
static int vr_cull_widen(float n[3], const float fw[3], float by) {
    const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 1e-6f)) return 0;
    const float sa = (n[0] * fw[0] + n[1] * fw[1] + n[2] * fw[2]) / len;
    if (!(sa > 0.05f && sa < 0.95f)) return 0;   /* (not a side plane) */
    const float ca = sqrtf(1.0f - sa * sa), a = asinf(sa);
    if (a + by > 80.0f / 57.2958f) return 0;
    const float keep = cosf(a + by) / ca, lean = sinf(a + by);
    for (int j = 0; j < 3; j++) {
        const float across = n[j] / len - sa * fw[j];   /* (the part of the unit normal across the view) */
        n[j] = len * (keep * across + lean * fw[j]);
    }
    return 1;
}

void hos_vr_cull_snapshot(Ctx *c, uint64_t snap) {
    static int told = -1;
    const int frustum_mode = rx_option(931, 0), frustum_off = frustum_mode == 1, wider = frustum_mode == 2;
    /* metres: Link's own reach from the eye, native hands at his sides included (or the test's 200 m) */
    const float margin = frustum_off ? VR_CULL_TEST_MARGIN : 1.5f;
    hos_vr_cull_note_view(c, snap);
    {   /* (research, bounded: the blinking) every view snapshot: its FOV, eye, near/far and whether the margin applies. A
           main-view snapshot built from another eye (the game's own camera) would be culled without the margin. */
        static unsigned seen;
        if (seen < 400 && __atomic_load_n(&hos_vr_cull_first_person, __ATOMIC_ACQUIRE)) {
            seen++;
            hos_log("vr: snapshot %#llx fov %.3f eye %.2f %.2f %.2f (VR eye %.2f %.2f %.2f) near %.3f far %.1f ours %d",
                    (unsigned long long)snap, vr_load(c, snap + 0x1d0), vr_load(c, snap + 0xe4), vr_load(c, snap + 0xf4),
                    vr_load(c, snap + 0x104), hos_vr_cull_eye[0], hos_vr_cull_eye[1], hos_vr_cull_eye[2],
                    vr_load(c, snap + 0x1c8), vr_load(c, snap + 0x1cc), hos_vr_snapshot_ours(c, snap));
        }
    }
    if (!__atomic_load_n(&hos_vr_cull_first_person, __ATOMIC_ACQUIRE) || !hos_vr_snapshot_ours(c, snap)) return;
    float e[3], f[3];
    for (int k = 0; k < 3; k++)
        e[k] = vr_load(c, snap + 0xe4 + 16 * k), f[k] = -vr_load(c, snap + 0xe0 + 16 * k);
    const float ex = e[0] - hos_vr_cull_eye[0], ey = e[1] - hos_vr_cull_eye[1], ez = e[2] - hos_vr_cull_eye[2];
    if (!(ex * ex + ey * ey + ez * ez < 0.25f)) return;   /* (another camera at the same FOV: a reflection, a portrait) */
    vr_cull_lag_note(c, snap);
    const float zn = vr_load(c, snap + 0x1c8), zf = vr_load(c, snap + 0x1cc);
    if (!(zn > 0.0f && zf > zn && isfinite(zf))) return;
    const float mid = 0.5f * (zn + fminf(zf, zn + 20.0f));   /* (a point surely inside, to tell each plane's inside) */
    float plane[6][4], side[6];
    for (int k = 0; k < 6; k++) {
        float q = 0;
        for (int j = 0; j < 4; j++) plane[k][j] = vr_load(c, snap + 0x1f0 + 16 * k + 4 * j);
        for (int j = 0; j < 3; j++) q += plane[k][j] * (e[j] + mid * f[j]);
        q += plane[k][3];
        if (!isfinite(q) || fabsf(q) < 1e-4f) return;
        side[k] = q > 0 ? 1.0f : -1.0f;
        if (side[k] != side[0]) return;   /* (not a frustum this understands: leave it as the game made it) */
    }
    float view[4][4], vside[4];
    for (int k = 0; k < 4; k++) {
        for (int j = 0; j < 4; j++) view[k][j] = vr_load(c, snap + 0x08 + 16 * k + 4 * j);
        const float q = -mid * view[k][2] - view[k][3];   /* (view space: the eye at 0, looking down -z) */
        if (!isfinite(q) || fabsf(q) < 1e-4f) return;
        vside[k] = q > 0 ? 1.0f : -1.0f;
    }
    if (wider) {   /* (before the ball: each turned plane still passes where it did at the eye) */
        const float fv[3] = {0, 0, -1};   /* (view space looks down -z) */
        for (int k = 0; k < 6; k++) {
            float n[3];
            for (int j = 0; j < 3; j++) n[j] = side[k] * plane[k][j];
            const float at_eye = plane[k][0] * e[0] + plane[k][1] * e[1] + plane[k][2] * e[2] + plane[k][3];
            if (!vr_cull_widen(n, f, VR_CULL_WIDER)) continue;
            for (int j = 0; j < 3; j++) plane[k][j] = side[k] * n[j], vr_store(c, snap + 0x1f0 + 16 * k + 4 * j, plane[k][j]);
            plane[k][3] = at_eye - (plane[k][0] * e[0] + plane[k][1] * e[1] + plane[k][2] * e[2]);
            vr_store(c, snap + 0x1f0 + 16 * k + 12, plane[k][3]);
        }
        for (int k = 0; k < 4; k++) {   /* (n.v = w: the eye at 0 keeps w) */
            float n[3];
            for (int j = 0; j < 3; j++) n[j] = vside[k] * view[k][j];
            if (!vr_cull_widen(n, fv, VR_CULL_WIDER)) continue;
            for (int j = 0; j < 3; j++) view[k][j] = vside[k] * n[j], vr_store(c, snap + 0x08 + 16 * k + 4 * j, view[k][j]);
        }
    }
    for (int k = 0; k < 6; k++) {
        const float inside = side[k] * (plane[k][0] * e[0] + plane[k][1] * e[1] + plane[k][2] * e[2] + plane[k][3]);
        if (inside < margin) vr_store(c, snap + 0x1f0 + 16 * k + 12, plane[k][3] + side[k] * (margin - inside));
    }
    for (int k = 0; k < 4; k++) {
        const float inside = -vside[k] * view[k][3];
        if (inside < margin) vr_store(c, snap + 0x08 + 16 * k + 12, view[k][3] - vside[k] * (margin - inside));
    }
    for (int k = 0; k < 3; k++) {
        const float lo = vr_load(c, snap + 0x250 + 4 * k), hi = vr_load(c, snap + 0x25c + 4 * k);
        if (e[k] - margin < lo) vr_store(c, snap + 0x250 + 4 * k, e[k] - margin);
        if (e[k] + margin > hi) vr_store(c, snap + 0x25c + 4 * k, e[k] + margin);
    }
    if (told != frustum_mode) {
        told = frustum_mode;
        if (frustum_off) hos_log("vr: headset frustum culling Off (test): nothing within %.0f m of the eye is culled by the "
                                 "headset view's planes", margin);
        else if (wider) hos_log("vr: headset frustum culling Wider (test): its side planes %.0f degrees wider each side",
                                VR_CULL_WIDER * 57.2958f);
        else hos_log("vr: first-person culling keeps %.1f m around the eye (Link's own equipment)", margin);
    }
}

/* (a full-screen menu, vr/headset/bridges_host.c hos_vr_menu_now) Its 3D (the pause's Link and item models, the field behind
   the title and the file select) is drawn over the whole headset-shaped picture, which the headset crops to its middle 16:9,
   where the menus are. At the game's own vertical FOV that middle showed it 1.42 times too big and pushed out from the middle
   (the Collection's and the Items' models off their places, Link too big for his oval; headset 22 Nov). So while the menu is
   up each 16:9 camera keeps the game's horizontal view instead: its vertical tan times 16:9 over the picture's ratio, so the
   middle 16:9 is the game's own 16:9 frame. The game's angle is kept, a projection at a time, and put back once the menu is
   gone (a projection still at ours) */
static struct { uint64_t proj; float game, ours; } vr_menu_fov[32];
static void vr_set_fov(Ctx *c, uint64_t proj, float angle) {
    vr_store(c, proj + 0xa0, angle);
    vr_store(c, proj + 0xa4, sinf(0.5f * angle));
    vr_store(c, proj + 0xa8, cosf(0.5f * angle));
    vr_store(c, proj + 0xac, tanf(0.5f * angle));
}
static int hos_vr_menu_projection(Ctx *c, uint64_t proj, float ratio, float *tan_out) {
    extern volatile float hos_wide_ratio;
    int hos_vr_menu_now(void);
    const int menu = hos_vr_menu_now();
    static int was_menu;
    /* (a menu opened: what is left from the last one is a projection not updated since (its camera gone, the pause's
       previews), forgotten) */
    if (menu && !was_menu) memset(vr_menu_fov, 0, sizeof vr_menu_fov);
    was_menu = menu;
    const float angle = vr_load(c, proj + 0xa0), r = hos_wide_ratio;
    if (menu) {   /* (research, bounded: each camera a menu's frames update, its ratio and angle) */
        static uint64_t said[8];
        static int n_said;
        int known = 0;
        for (int i = 0; i < n_said && !known; i++) known = said[i] == proj;
        if (!known && n_said < 8)
            said[n_said++] = proj, hos_log("vr: menu 3D: camera %#llx, ratio %.4f, angle %.1f degrees", (unsigned long long)proj,
                                           ratio, angle * 57.2958f);
    }
    int k = -1, free_k = -1;
    for (int i = 0; i < 32; i++) {
        if (vr_menu_fov[i].proj == proj) k = i;
        else if (!vr_menu_fov[i].proj && free_k < 0) free_k = i;
    }
    const int ours = k >= 0 && fabsf(angle - vr_menu_fov[k].ours) < 1e-5f;
    if (!menu) {   /* (the menu gone: the game's own angle back, once) */
        if (k < 0) return 0;
        const float game = vr_menu_fov[k].game;
        vr_menu_fov[k].proj = 0;
        if (!ours) return 0;
        vr_set_fov(c, proj, game);
        *tan_out = tanf(0.5f * game);
        return 1;
    }
    /* (a 16:9 camera: its ratio still the game's, or already the picture's from the camera update, nx2/oot_wide.py _ratio(0):
       the pause's are 45 degrees at 1.25 here; the square shadow and probe cameras are left alone) */
    if (!(r > 0.2f && r < 16.0f / 9.0f * 0.999f) ||
        !((ratio > 16.0f / 9.0f - 0.01f && ratio < 16.0f / 9.0f + 0.01f) || fabsf(ratio - r) < 0.01f))
        return 0;
    const float game = ours ? vr_menu_fov[k].game : angle;
    if (!(isfinite(game) && game > 0.05f && game < 3.0f) || (k < 0 && free_k < 0)) return 0;
    const float tn = tanf(0.5f * game) * (16.0f / 9.0f) / r, a = 2.0f * atanf(tn);
    if (k < 0) k = free_k;
    vr_menu_fov[k].proj = proj, vr_menu_fov[k].game = game, vr_menu_fov[k].ours = a;
    vr_set_fov(c, proj, a);
    static int told;
    if (told < 8) told++, hos_log("vr: menu 3D: projection %#llx at %.1f degrees (vertical) for the game's %.1f: the game's own "
                                  "16:9 view in the picture's middle", (unsigned long long)proj, a * 57.2958f, game * 57.2958f);
    *tan_out = tn;
    return 1;
}

#define kVrNearM 0.1f   /* (m) the headset camera's near plane at most (gameplay's own) */
static int hos_vr_projection_locked(Ctx *c, uint64_t proj, float ratio, float *tan_out) {
    static int told_ok, told_bad;
    if (proj && hos_vr_menu_projection(c, proj, ratio, tan_out)) return 1;
    static uint64_t first;   /* (the first camera seen: the gameplay one, whose own FOV is the Switch 2 reference) */
    const float fov = hos_vr_fov_y;
    if (!(fov > 0.2f && fov < 3.0f) || !proj) return 0;
    const float va = hos_vr_aspect;
    if (!(ratio > 16.0f / 9.0f - 0.01f && ratio < 16.0f / 9.0f + 0.01f) && !(va > 0.0f && fabsf(ratio - va) < 0.01f)) return 0;
    const float angle = vr_load(c, proj + 0xa0), s = vr_load(c, proj + 0xa4), co = vr_load(c, proj + 0xa8),
                t = vr_load(c, proj + 0xac);
    const float h = 0.5f * fov, ns = sinf(h), nc = cosf(h), nt = tanf(h), half = 0.5f * angle;
    const int fin = isfinite(angle) && isfinite(s) && isfinite(co) && isfinite(t);
    /* the game's own four fields (it set its FOV again), or ours with only the tan set again by the game (one camera does that) */
    const int game = fin && angle > 0.05f && angle < 3.1f && fabsf(sinf(half) - s) < 0.01f && fabsf(cosf(half) - co) < 0.01f &&
                     fabsf(tanf(half) - t) < 0.01f * (1.0f + t);
    const int ours = fin && fabsf(s - ns) < 1e-5f && fabsf(co - nc) < 1e-5f;
    if (!game && !ours) {
        if (!told_bad) told_bad = 1, hos_log("vr: projection %#llx: angle %.4f sin %.4f cos %.4f tan %.4f is not sead's layout: "
                                             "FOV left alone", (unsigned long long)proj, angle, s, co, t);
        return 0;
    }
    if (!first) first = proj;
    {   /* (research, bounded) the projection's near and far (sead's PerspectiveProjection: +0x98, +0x9c, before the angle) as
           they change: a cutscene's camera may clip what is near the eye (headset 22 Nov, a band missing at the view's bottom
           in Link's house at the director's camera) */
        static float said_near = -1, said_far = -1;
        static unsigned lines;
        const float n = vr_load(c, proj + 0x98), fa = vr_load(c, proj + 0x9c);
        if (lines < 40 && (fabsf(n - said_near) > 0.01f || fabsf(fa - said_far) > 1.0f)) {
            lines++, said_near = n, said_far = fa;
            hos_log("vr: projection %#llx near %.3f far %.1f (angle %.3f, the game's %d)", (unsigned long long)proj, n, fa, angle,
                    game);
        }
    }
    /* (the game's own FOV: never ours, which is itself consistent) */
    if (game && proj == first && fabsf(angle - fov) > 0.01f && angle < 2.0f) hos_vr_game_fovy = angle;
    /* (the near plane no further than gameplay's own 0.1 m: a cutscene's camera sets its own per shot, 0.5 to 1 m in the new
       game's opening, and with the headset's wide view what is that near (the floor below you, Link's bed) was cut away, a
       band missing at the view's bottom that followed the head; headset 22 Nov) */
    if (vr_load(c, proj + 0x98) > kVrNearM) vr_store(c, proj + 0x98, kVrNearM);
    vr_store(c, proj + 0xa0, fov);
    vr_store(c, proj + 0xa4, ns);
    vr_store(c, proj + 0xa8, nc);
    vr_store(c, proj + 0xac, nt);
    if (!told_ok) told_ok = 1, hos_log("vr: game FOV %.1f -> %.1f degrees (vertical), through sead's projection",
                                       hos_vr_game_fovy * 57.2958f, fov * 57.2958f);
    *tan_out = nt;
    return 1;
}

/* ---- (OoT VR) World culling in VR: a watch for the next headset run, and the VR tab's "World culling" switch ---------------
   Headset 18 Nov (child Link, first person): torches (Obj_CandleStick_A_01/_A_02/_TimeExtinguish_A_01, Obj_SwitchCandleStick),
   the Deku Tree's spinning spike log (Obj_InsideTheDekuTree_ThornStick_A_01) and the boulder before Dodongo's Cavern
   (Obj_DeathMtTrail_BrokenRock_A_01) vanished while the terrain stayed. What reading game 1.1.1 established (addresses and
   names only, no game data):
   - The per-model view test (f_8009bb80) culls against the view snapshot, and the headset camera's snapshot is the widened
     one (vr/camera/detail_host.c, the margin above). Per view it first checks the model's box against the snapshot's
     world box (+0x250/+0x25c) and six world planes (+0x1f0..; 8009dd50..8009de54, a drop jumps to 8009e360), then its
     sphere (near + the four view planes). It has 14 near tests in all (8009df54, 8009ed1c, 8009f47c, 8009f664, 8009fc04,
     800a07ac, 800a41a8, 800a559c, 800a5984, 800a5b84, 800a6474, 800a7240, 800a7994, 800a7b6c); the census sees only the
     two the gear hooks sit on (8009df54, 8009f47c). Each view's result is kept on the stack (sp+0x188 + 0x38 a view), so
     nothing outside can read it afterwards.
   - The precomputed culling (PreComputedCulling/<area>/Boxes.bbox: 44-byte boxes, id, centre, rotation, half size;
     RuntimeId/<box id>.bcul: the rooms and the pre-actor-rendered pieces seen from that box) takes its base position from
     the gameplay LookAtCamera's matrix (-R^T t of its +8 matrix, near main+0x2bbedc0), which the mod writes with the
     headset's eye. And none of the four is in those lists, from the boxes they stand in: they are whole actors. Not this.
   - The LOD component (actor +0x270 -> +0xb0) keeps an in-view state at +0xb5 (what QueryLODGetAreaView reads). In the
     15:41 log its bit 0x02 followed the HEADSET's view at every change (e.g. the actor at -2.5 106.0 4.4: 15 changes,
     each with the headset view). So it is made from the headset: not this.
   - Left: (a) the per-model test after all, its box check or a branch the census misses (the first census could not
     tell: it saw no test at all near the stationary actors at wall height, and its "kept" pooled every model in a 6 m
     block); (b) the effect limiter's directional culling (LimiterParam IsEnabledDirectionalCulling +0x74,
     DirectionalCullingFunDot +0x58, FunRad +0x5c, sphere radius +0x64; parsed by f_83ba8630) for the torches' flames,
     whose camera is not established; (c) anything made from the LookAtCamera's own fields (+0x38 position, +0x44 target,
     +0x50 up; doubles at +0x60 position, +0x78 target), which stay the orbit camera's (3.3 to 4.3 m from the eye in the
     15:41 log); (d) the renderer. (The 90-degree views at the game camera in the log are six faces of one point, a cube map
     probe, not a culling view.)
   So: (1) the watch below logs, for actors near the eye inside the headset's view, by name, whether THEIR OWN model reached
   the sphere test and what it did; (2) the VR tab's "Headset frustum culling" Off (option 931, above) turns the headset
   view's planes off near the eye: if the objects come back, (a) it is; (3) the VR tab's "World culling" (option 923) gives
   the game camera's fields the headset's eye and look for the rest of each frame, and the game's own back before the mod
   reads them next frame: if the objects come back with it, (c) it is. Both are tests, off ("Game") by default. */
#define VR_SEEN_CELL 2.0f                 /* metres: a tested model is noted in the cell of its sphere's centre */
#define VR_SEEN_SLOTS 8192                /* (a power of two) the cells and models noted, one hash table */
#define VR_SEEN_PROBES 8                  /* slots tried per cell */
#define VR_SEEN_FRESH_NS 150000000LL      /* a model tested this recently was tested "now" (several frames at 72-120 Hz) */
#define VR_WATCH_RADIUS 30.0f             /* metres from the eye: the actors watched */
#define VR_WATCH_ACTORS 256               /* actors remembered at once (the nearest are what matter) */
#define VR_WATCH_PERIOD_NS 250000000LL    /* each watched actor looked at this often */
#define VR_WATCH_LOG_GAP_NS 1000000000LL  /* at most one line a second per actor */
#define VR_WATCH_SUMMARY_NS 10000000000LL /* a summary every 10 s */
#define VR_WATCH_ACTOR_RADIUS 0.5f        /* metres: an actor counts as in a view when a ball this big around its origin is */
#define VR_VIEW_FRESH_NS 200000000LL      /* the headset's culling planes are used while this recent */
#define VR_GAME_ASPECT (16.0f / 9.0f)     /* the game camera's own picture shape */

/* RX_VR_CULL_WATCH=1 turns the watch on (it only reads and logs). Off by default: on, every update of an actor near the eye
   spun on the watch's lock and walked its 256 places, a cost in every frame for research only (the stereo performance
   reviews, 20 Nov). The torch and vanishing-object research sets it */
static int vr_cull_watch_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = vr_research_getenv("RX_VR_CULL_WATCH");
        on = e && e[0] && e[0] != '0';
    }
    return on;
}

/* -- the models the per-model test saw, for the headset's view (called from vr/items/gear/gear_near_host.c's two hooks) -- */
static volatile uint64_t vr_seen_key[VR_SEEN_SLOTS];
static volatile int64_t vr_seen_time[VR_SEEN_SLOTS];
static volatile int32_t vr_seen_pass[VR_SEEN_SLOTS];   /* VR_SEEN_KEPT / VR_SEEN_CULLED: what the tests there did lately */
enum { VR_SEEN_KEPT = 1, VR_SEEN_CULLED = 2 };   /* (a model is tested once per view a frame: both can happen) */
static uint64_t vr_seen_cell(int64_t qx, int64_t qy, int64_t qz) {
    return (1ULL << 63) | ((uint64_t)(qx & 0x1fffff) << 42) | ((uint64_t)(qy & 0x1fffff) << 21) | (uint64_t)(qz & 0x1fffff);
}
static unsigned vr_seen_slot(uint64_t key) {
    key ^= key >> 33, key *= 0xff51afd7ed558ccdULL, key ^= key >> 33;
    return (unsigned)key & (VR_SEEN_SLOTS - 1);
}
/* The game's own view test, as f_8009bb80 makes it after these hooks: the sphere in view space by the snapshot's rows
   (+0xa8 right, +0xb8 up, +0xc8 back, x y z w each), culled if behind the near distance (near - r > -z) or out of any of
   the four view planes (+0x08, n.v - w > r is out) */
static int vr_seen_kept(Ctx *c, uint64_t snap, const float p[3], float r) {
    float v[3];
    for (int k = 0; k < 3; k++) {
        const uint64_t row = snap + 0xa8 + 16 * k;
        v[k] = vr_load(c, row) * p[0] + vr_load(c, row + 4) * p[1] + vr_load(c, row + 8) * p[2] + vr_load(c, row + 12);
    }
    if (vr_load(c, snap + 0x1c8) - r > -v[2]) return 0;
    for (int k = 0; k < 4; k++) {
        const uint64_t plane = snap + 0x08 + 16 * k;
        const float out = vr_load(c, plane) * v[0] + vr_load(c, plane + 4) * v[1] + vr_load(c, plane + 8) * v[2] -
                          vr_load(c, plane + 12);
        if (!(out < r)) return 0;
    }
    return 1;
}
/* a test's result noted under key (a cell's, or a model's own address) */
static void vr_seen_note(uint64_t key, int kept, int64_t now) {
    const unsigned home = vr_seen_slot(key);
    for (unsigned i = 0; i < VR_SEEN_PROBES; i++) {
        const unsigned s = (home + i) & (VR_SEEN_SLOTS - 1);
        uint64_t was = __atomic_load_n(&vr_seen_key[s], __ATOMIC_ACQUIRE);
        if (was != key) {   /* (an empty slot, or one not noted for a second, is taken for this cell) */
            const int64_t t = __atomic_load_n(&vr_seen_time[s], __ATOMIC_RELAXED);
            if (was && now - t <= 1000000000LL) continue;
            if (__atomic_compare_exchange_n(&vr_seen_key[s], &was, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                __atomic_store_n(&vr_seen_time[s], 0, __ATOMIC_RELAXED);   /* (new here: no earlier "kept" counts) */
            else if (was != key) continue;   /* (another thread took it for another cell) */
        }
        /* (several models share a cell, and a model has a test per view a frame: what each did lately is kept) */
        const int bit = kept ? VR_SEEN_KEPT : VR_SEEN_CULLED;
        if (now - __atomic_load_n(&vr_seen_time[s], __ATOMIC_RELAXED) > VR_SEEN_FRESH_NS) __atomic_store_n(&vr_seen_pass[s], bit, __ATOMIC_RELAXED);
        else __atomic_fetch_or(&vr_seen_pass[s], bit, __ATOMIC_RELAXED);
        __atomic_store_n(&vr_seen_time[s], now, __ATOMIC_RELEASE);
        return;
    }
}
/* what was noted under key lately: -1 nothing, else VR_SEEN_KEPT and/or VR_SEEN_CULLED */
static int vr_seen_get(uint64_t key, int64_t now) {
    const unsigned home = vr_seen_slot(key);
    for (unsigned i = 0; i < VR_SEEN_PROBES; i++) {
        const unsigned s = (home + i) & (VR_SEEN_SLOTS - 1);
        if (__atomic_load_n(&vr_seen_key[s], __ATOMIC_ACQUIRE) != key) continue;
        if (now - __atomic_load_n(&vr_seen_time[s], __ATOMIC_ACQUIRE) > VR_SEEN_FRESH_NS) return -1;
        return __atomic_load_n(&vr_seen_pass[s], __ATOMIC_RELAXED);
    }
    return -1;
}
/* the models tested near p (their centres within one cell of p's) lately: -1 none, else VR_SEEN_* bits of them all.
   (For the RX_VR_CULL_AT places, which have no actor. It pools a 6 m block: an actor's own model is told apart by
   vr_seen_get on the model's address instead.) */
static int vr_seen_near(const float p[3], int64_t now) {
    const int64_t qx = (int64_t)floorf(p[0] / VR_SEEN_CELL), qy = (int64_t)floorf(p[1] / VR_SEEN_CELL),
                  qz = (int64_t)floorf(p[2] / VR_SEEN_CELL);
    int bits = -1;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                const int got = vr_seen_get(vr_seen_cell(qx + dx, qy + dy, qz + dz), now);
                if (got >= 0) bits = (bits < 0 ? 0 : bits) | got;
            }
    return bits;
}
/* Every model's sphere test in the headset's view (the two hooks of vr/items/gear/gear_near_host.c, model = the ModelNW in
   x19): noted by its place, and by the model itself. These two are only the sphere tests that come after the box test;
   f_8009bb80 has 14 near tests in all, and a model its box check drops reaches none. So "not seen here" means "dropped
   before the sphere test, or tested in a branch the watch does not see", not "never tested". */
void hos_vr_cull_model_seen(Ctx *c, uint64_t snap, uint64_t sphere, uint64_t model) {
    if (!vr_cull_watch_on() || !(hos_vr_fov_y > 0.2f) || !hos_vr_snapshot_ours(c, snap) || !hos_guest_range(sphere, 16)) return;
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = vr_load(c, sphere + 4 * k);
    const float r = vr_load(c, sphere + 12);
    if (!(isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && isfinite(r) && r >= 0)) return;
    float d2 = 0;
    for (int k = 0; k < 3; k++) d2 += (p[k] - hos_vr_cull_eye[k]) * (p[k] - hos_vr_cull_eye[k]);
    if (!(d2 < (VR_WATCH_RADIUS + 10.0f) * (VR_WATCH_RADIUS + 10.0f))) return;
    const int64_t now = hos_now_ns();
    const int kept = vr_seen_kept(c, snap, p, r);
    vr_seen_note(vr_seen_cell((int64_t)floorf(p[0] / VR_SEEN_CELL), (int64_t)floorf(p[1] / VR_SEEN_CELL),
                              (int64_t)floorf(p[2] / VR_SEEN_CELL)), kept, now);
    if (model) vr_seen_note(model, kept, now);   /* (a guest address: never bit 63, so never a cell's key) */
}

/* -- the headset's culling planes, as the game built them (before the first-person margin), from hos_vr_cull_snapshot -- */
static float vr_view_plane[6][4];   /* world planes, signed so that inside is >= 0 */
static volatile int64_t vr_view_time;
static void hos_vr_cull_note_view(Ctx *c, uint64_t snap) {
    if (!vr_cull_watch_on() || !hos_vr_snapshot_ours(c, snap)) return;
    float e[3], f[3], d2 = 0;
    for (int k = 0; k < 3; k++) {
        e[k] = vr_load(c, snap + 0xe4 + 16 * k), f[k] = -vr_load(c, snap + 0xe0 + 16 * k);
        d2 += (e[k] - hos_vr_cull_eye[k]) * (e[k] - hos_vr_cull_eye[k]);
    }
    if (!(d2 < 0.25f)) return;   /* (the headset's own eye only, as hos_vr_cull_snapshot) */
    float pl[6][4];
    for (int k = 0; k < 6; k++) {
        float q = 0;
        for (int j = 0; j < 4; j++) pl[k][j] = vr_load(c, snap + 0x1f0 + 16 * k + 4 * j);
        for (int j = 0; j < 3; j++) q += pl[k][j] * (e[j] + 1.0f * f[j]);   /* (a point 1 m ahead of the eye: inside) */
        q += pl[k][3];
        if (!isfinite(q) || fabsf(q) < 1e-6f) return;
        if (q < 0)
            for (int j = 0; j < 4; j++) pl[k][j] = -pl[k][j];
    }
    memcpy(vr_view_plane, pl, sizeof pl);   /* (several threads: a torn copy only skews one look of the watch) */
    __atomic_store_n(&vr_view_time, hos_now_ns(), __ATOMIC_RELEASE);
}
static int vr_in_headset_view(const float p[3]) {
    for (int k = 0; k < 6; k++)
        if (vr_view_plane[k][0] * p[0] + vr_view_plane[k][1] * p[1] + vr_view_plane[k][2] * p[2] + vr_view_plane[k][3] <
            -VR_WATCH_ACTOR_RADIUS) return 0;
    return 1;
}

/* -- the game camera's own fields (the gameplay LookAtCamera's +0x38 position, +0x44 target, +0x50 up), as the game made
   them this frame, from hos_vr_cull_camera before the mod's view -- */
static float vr_game_cam[9];
static volatile int64_t vr_game_cam_time;
/* p within a view from pos toward at, up, of half-angle tangents tx (across) and ty (up) */
static int vr_in_camera_view(const float cam[9], float tx, float ty, const float p[3]) {
    float fw[3], d[3], fl = 0;
    for (int k = 0; k < 3; k++) fw[k] = cam[3 + k] - cam[k], fl += fw[k] * fw[k], d[k] = p[k] - cam[k];
    fl = sqrtf(fl);
    if (!(fl > 1e-4f)) return 0;
    for (int k = 0; k < 3; k++) fw[k] /= fl;
    const float *up = cam + 6;
    float rt[3] = {fw[1] * up[2] - fw[2] * up[1], fw[2] * up[0] - fw[0] * up[2], fw[0] * up[1] - fw[1] * up[0]};
    const float rl = sqrtf(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
    if (!(rl > 1e-4f)) return 0;
    for (int k = 0; k < 3; k++) rt[k] /= rl;
    const float u[3] = {rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2], rt[0] * fw[1] - rt[1] * fw[0]};
    const float z = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2], x = d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2],
                y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    return z > -VR_WATCH_ACTOR_RADIUS && fabsf(x) <= z * tx + VR_WATCH_ACTOR_RADIUS && fabsf(y) <= z * ty + VR_WATCH_ACTOR_RADIUS;
}

/* -- the actors (host/hos_main.c hos_oot_lift_player: every actor's movement update, VR HOOK) -- */
typedef struct {
    uint64_t actor, text;   /* (text: its name's address when the name was read: another actor at a reused address has another) */
    uint64_t model;         /* its ModelNW, as vr_actor_model found it (0: none) */
    int64_t looked, logged;
    int state;       /* VR_WATCH_* bits of the last look, -1 none yet */
    const char *no_model;   /* why it has no model (vr_actor_model), for the log */
    char name[48];
} VrWatch;
enum {
    VR_WATCH_HEADSET = 1,      /* inside the headset's culling planes */
    VR_WATCH_TESTED = 2,       /* its own model reached the per-model sphere test lately (in the headset's view) */
    VR_WATCH_KEPT = 4,         /* ... and that test kept it */
    VR_WATCH_GAME_VIEW = 8,    /* inside the game camera's view, at the game's own field of view */
    VR_WATCH_GAME_WIDE = 16,   /* inside the game camera's view at the headset's field of view */
    VR_WATCH_MODEL = 32,       /* its model was found (vr_actor_model) */
    VR_WATCH_CULLED = 64,      /* ... and a sphere test (of another of the headset's views) culled it too */
};
static VrWatch vr_watch[VR_WATCH_ACTORS];
static volatile int vr_watch_lock;
static unsigned vr_watch_lines;   /* (bounded: the log stays readable) */
static volatile unsigned vr_watch_dropped;   /* actor updates not looked at: another thread held the lock (no spin there) */
/* An actor's name (Obj_CandleStick_A_01). The actors this hook is given are ActorAccessors (vtable 0x84b45e18 for Link's,
   others their own), whose name is a SafeString pointer at +0x260, else +0x20, as the game's QueryGenericGetActorName reads
   it (81016f6c..81016f84; vr/items/gear/equipment_host.c oot_vr_equipment_name reads it the same way). The first watch read
   +16 -> +88, which is the Player object's own record (hos_oot_is_player) and no other actor's: every line of the
   18 Nov 15:41 log named its actor "?". Returns the name's address (0: none). */
static uint64_t vr_actor_name(Ctx *c, uint64_t actor, char out[48]) {
    static const uint64_t at[2] = {0x260, 0x20};
    for (int i = 0; i < 2; i++) {
        const uint64_t text = hos_guest_range(actor + at[i], 8) ? recomp_load64(c, actor + at[i]) : 0;
        if (text < 0x80000000ULL || !hos_guest_range(text, 48)) continue;
        int n = 0;
        for (; n < 47; n++) {
            const unsigned char ch = recomp_load8(c, text + n);
            if (!ch) break;
            if (ch < 32 || ch > 126) { n = 0; break; }   /* (not text: try the other) */
            out[n] = (char)ch;
        }
        if (n) {
            out[n] = 0;
            return text;
        }
    }
    memcpy(out, "?", 2);
    return 0;
}
/* An actor's main model, the ModelNW the per-model view test (f_8009bb80) is given in x19, found the way
   vr/items/gear/gear_near_host.c oot_vr_gear_resolve finds a held item's: actor +0x270 -> component table, +8 -> its
   ModelComponent (vtable 0x84c81020), +0x18 -> resource (0x84c288a0), +0x18 -> a node whose first word is the ModelNW
   (0x84c28cc8). 0 when a link is not that, and *why names the link (for the log). An actor with more models is told by its
   first. */
static uint64_t vr_ptr(Ctx *c, uint64_t p) {
    return p >= HOS_HEAP_BASE && !(p & 7) && hos_guest_range(p, 8) ? recomp_load64(c, p) : 0;
}
static uint64_t vr_actor_model(Ctx *c, uint64_t actor, const char **why) {
    const uint64_t table = vr_ptr(c, actor + 0x270), component = table ? vr_ptr(c, table + 8) : 0;
    if (!component) return *why = "no model component", 0;
    if (vr_ptr(c, component) != 0x84c81020ULL) return *why = "its component +8 is not a ModelComponent", 0;
    const uint64_t resource = vr_ptr(c, component + 0x18);
    if (vr_ptr(c, resource) != 0x84c288a0ULL) return *why = "its model resource is another kind", 0;
    const uint64_t node = vr_ptr(c, resource + 0x18), model = node ? vr_ptr(c, node) : 0;
    if (vr_ptr(c, model) != 0x84c28cc8ULL) return *why = "its model is not a ModelNW", 0;
    return *why = NULL, model;
}
/* the actor's LOD component (actor +0x270 -> +0xb0): its bytes +0xb4..+0xb7 (+0xb5: the in-view state the game's
   QueryLODGetAreaView reads; in the 18 Nov 15:41 log its bit 0x02 followed the HEADSET's view, every change), or ~0 when
   there is none */
static uint32_t vr_actor_lod(Ctx *c, uint64_t actor) {
    const uint64_t table = hos_guest_range(actor + 0x270, 8) ? recomp_load64(c, actor + 0x270) : 0;
    const uint64_t lod = table >= HOS_HEAP_BASE && hos_guest_range(table + 0xb0, 8) ? recomp_load64(c, table + 0xb0) : 0;
    return lod >= HOS_HEAP_BASE && hos_guest_range(lod + 0xb4, 4) ? recomp_load32(c, lod + 0xb4) : 0xffffffffu;
}
static const char *vr_model_said(int state) {   /* (the log's words for the per-model sphere test of an actor's own model) */
    if (!(state & VR_WATCH_MODEL)) return "no model found";
    if (!(state & VR_WATCH_TESTED)) return "did NOT reach the sphere test (dropped before it: box test or view mask)";
    if (!(state & VR_WATCH_KEPT)) return "culled by the sphere test";
    return state & VR_WATCH_CULLED ? "kept by one view's sphere test, culled by another's" : "kept by the sphere test";
}
static const char *vr_frustum_said(void) {
    const int m = rx_option(931, 0);
    return m == 1 ? "Off (test)" : m == 2 ? "Wider (test)" : "Game";
}
void hos_vr_cull_actor(Ctx *c, uint64_t actor) {
    if (!vr_cull_watch_on() || !actor || !(hos_vr_fov_y > 0.2f) || !hos_guest_range(actor + 796, 12)) return;
    float p[3], d2 = 0;
    for (int k = 0; k < 3; k++) p[k] = vr_load(c, actor + 796 + 4 * k), d2 += (p[k] - hos_vr_cull_eye[k]) * (p[k] - hos_vr_cull_eye[k]);
    if (!(d2 < VR_WATCH_RADIUS * VR_WATCH_RADIUS) || !(d2 > 0.04f)) return;   /* (and NaN; Link himself is at the eye) */
    const int64_t now = hos_now_ns();
    if (now - __atomic_load_n(&vr_view_time, __ATOMIC_ACQUIRE) > VR_VIEW_FRESH_NS) return;
    if (__atomic_exchange_n(&vr_watch_lock, 1, __ATOMIC_ACQUIRE)) {
        /* (held by another worker: this update is dropped and counted, not waited for. The actor updates again next frame,
           and is looked at only every VR_WATCH_PERIOD_NS anyway) */
        __atomic_add_fetch(&vr_watch_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    VrWatch *w = NULL, *oldest = &vr_watch[0];
    for (int i = 0; i < VR_WATCH_ACTORS && !w; i++) {
        if (vr_watch[i].actor == actor) w = &vr_watch[i];
        else if (vr_watch[i].looked < oldest->looked) oldest = &vr_watch[i];
    }
    if (!w && oldest->actor && now - oldest->looked < 4 * VR_WATCH_PERIOD_NS) {
        /* (every place holds an actor still updating: this one is not watched. Taking the oldest place anyway made each
           update of more than VR_WATCH_ACTORS actors in range a new one, every one a first-sight line) */
        __atomic_store_n(&vr_watch_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (w && now - w->looked < VR_WATCH_PERIOD_NS) {
        __atomic_store_n(&vr_watch_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    char name[48];
    const uint64_t text = vr_actor_name(c, actor, name);
    if (!w || w->text != text) {   /* (a new one takes the place of one gone or far; or another actor at this address) */
        if (!w) w = oldest;
        memset(w, 0, sizeof *w), w->actor = actor, w->text = text, w->state = -1;
        memcpy(w->name, name, sizeof name);
    }
    w->looked = now;
    w->model = vr_actor_model(c, actor, &w->no_model);   /* (each look: a model can be made after the actor) */
    float cam[9];
    memcpy(cam, vr_game_cam, sizeof cam);
    const int cam_ok = now - __atomic_load_n(&vr_game_cam_time, __ATOMIC_ACQUIRE) < VR_VIEW_FRESH_NS;
    const float ty_game = tanf(0.5f * hos_vr_game_fovy), ty_vr = tanf(0.5f * hos_vr_fov_y);
    const int seen = w->model ? vr_seen_get(w->model, now) : -1;
    int state = vr_in_headset_view(p) ? VR_WATCH_HEADSET : 0;
    if (w->model) state |= VR_WATCH_MODEL;
    if (seen >= 0)
        state |= VR_WATCH_TESTED | (seen & VR_SEEN_KEPT ? VR_WATCH_KEPT : 0) | (seen & VR_SEEN_CULLED ? VR_WATCH_CULLED : 0);
    if (cam_ok && vr_in_camera_view(cam, ty_game * VR_GAME_ASPECT, ty_game, p)) state |= VR_WATCH_GAME_VIEW;
    if (cam_ok && hos_vr_aspect > 0 && vr_in_camera_view(cam, ty_vr * hos_vr_aspect, ty_vr, p)) state |= VR_WATCH_GAME_WIDE;
    const uint32_t lod = vr_actor_lod(c, actor);
    /* a line when what matters changes: in the headset's view and what the sphere test did with its model, or the game
       camera's view of it, or its LOD in-view byte, while it is in the headset's view */
    const int was = w->state;
    w->state = state | (int)(((lod >> 8) & 0xffu) << 8);
    const int changed = was != w->state && ((state & VR_WATCH_HEADSET) || (was >= 0 && (was & VR_WATCH_HEADSET)));
    const int log_it = changed && now - w->logged >= VR_WATCH_LOG_GAP_NS && vr_watch_lines < 3000;
    if (log_it) w->logged = now, vr_watch_lines++;
    const uint64_t model = w->model;
    const char *no_model = w->no_model;
    __atomic_store_n(&vr_watch_lock, 0, __ATOMIC_RELEASE);
    float gd = 0;
    for (int k = 0; k < 3; k++) gd += (cam[k] - hos_vr_cull_eye[k]) * (cam[k] - hos_vr_cull_eye[k]);
    if (log_it)
        hos_log("vr: cull watch %s %#llx at %.1f %.1f %.1f, %.1f m: headset view %d, its model %#llx %s%s%s, game camera "
                "view %d (at the headset's FOV %d), LOD %08x, game camera %.1f m from the eye", name,
                (unsigned long long)actor, p[0], p[1], p[2], sqrtf(d2), !!(state & VR_WATCH_HEADSET),
                (unsigned long long)model, vr_model_said(state), no_model ? ": " : "", no_model ? no_model : "",
                !!(state & VR_WATCH_GAME_VIEW), !!(state & VR_WATCH_GAME_WIDE), lod, cam_ok ? sqrtf(gd) : -1.0f);
    {   /* every 10 s: the actors in the headset's view whose own model did not reach the sphere test, by name, and what the
           game camera saw */
        static volatile int64_t last;
        int64_t at = __atomic_load_n(&last, __ATOMIC_RELAXED);
        if (now - at < VR_WATCH_SUMMARY_NS || !__atomic_compare_exchange_n(&last, &at, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return;
        char list[600];
        int n = 0, in_view = 0, with_model = 0, missing = 0, missing_game = 0, len = 0;
        list[0] = 0;
        while (__atomic_exchange_n(&vr_watch_lock, 1, __ATOMIC_ACQUIRE)) {}
        for (int i = 0; i < VR_WATCH_ACTORS; i++) {
            const VrWatch *e = &vr_watch[i];
            if (!e->actor || e->state < 0 || now - e->looked > 1000000000LL) continue;   /* (looked at in the last second) */
            n++;
            if (!(e->state & VR_WATCH_HEADSET)) continue;
            in_view++;
            if (!(e->state & VR_WATCH_MODEL)) continue;
            with_model++;
            if (e->state & VR_WATCH_TESTED) continue;
            missing++;
            if (!(e->state & VR_WATCH_GAME_VIEW)) missing_game++;
            if (len < (int)sizeof list - 70)
                len += snprintf(list + len, sizeof list - len, "%s%s(game view %d)", len ? ", " : "", e->name,
                                !!(e->state & VR_WATCH_GAME_VIEW));
        }
        __atomic_store_n(&vr_watch_lock, 0, __ATOMIC_RELEASE);
        hos_log("vr: cull watch: %d actors within %.0f m, %d in the headset's view, %d of those with a model, %d of which did "
                "not reach the sphere test (%d of them outside the game camera's view): %s; game camera %.1f m from the eye; "
                "world culling %s; headset frustum culling %s; %u updates dropped (lock busy)", n, VR_WATCH_RADIUS, in_view,
                with_model, missing, missing_game, list[0] ? list : "-", cam_ok ? sqrtf(gd) : -1.0f,
                rx_option(923, 0) == 2 ? "Headset+" : rx_option(923, 0) == 1 ? "Headset" : "Game", vr_frustum_said(),
                __atomic_load_n(&vr_watch_dropped, __ATOMIC_RELAXED));
    }
}

/* -- places to watch (RX_VR_CULL_AT="x,y,z;x,y,z", up to 8: an object's placement from your own game files): for an
   object whose actor is not among those the movement update passes (above), what the census says there, once a frame
   from hos_vr_cull_camera, a line on each change while the place is near and in the headset's view -- */
#define VR_WATCH_POINTS 8
static float vr_watch_at[VR_WATCH_POINTS][3];
static int vr_watch_at_n = -1, vr_watch_at_state[VR_WATCH_POINTS];
static void vr_cull_watch_points(void) {
    if (vr_watch_at_n < 0) {
        vr_watch_at_n = 0;
        const char *e = vr_research_getenv("RX_VR_CULL_AT");
        while (e && *e && vr_watch_at_n < VR_WATCH_POINTS) {
            float *q = vr_watch_at[vr_watch_at_n];
            int used = 0;
            if (sscanf(e, " %f , %f , %f%n", &q[0], &q[1], &q[2], &used) != 3) break;
            vr_watch_at_state[vr_watch_at_n++] = -1;
            e += used;
            while (*e == ';' || *e == ' ') e++;
        }
        if (vr_watch_at_n) hos_log("vr: cull watch: %d places from RX_VR_CULL_AT", vr_watch_at_n);
    }
    if (!vr_watch_at_n) return;
    static int64_t looked;
    static unsigned told;
    const int64_t now = hos_now_ns();
    if (now - looked < VR_WATCH_PERIOD_NS || now - __atomic_load_n(&vr_view_time, __ATOMIC_ACQUIRE) > VR_VIEW_FRESH_NS) return;
    looked = now;
    const float ty_game = tanf(0.5f * hos_vr_game_fovy);
    for (int i = 0; i < vr_watch_at_n; i++) {
        const float *q = vr_watch_at[i];
        float d2 = 0;
        for (int k = 0; k < 3; k++) d2 += (q[k] - hos_vr_cull_eye[k]) * (q[k] - hos_vr_cull_eye[k]);
        if (!(d2 < VR_WATCH_RADIUS * VR_WATCH_RADIUS)) { vr_watch_at_state[i] = -1; continue; }
        const int seen = vr_seen_near(q, now);
        int state = vr_in_headset_view(q) ? VR_WATCH_HEADSET : 0;
        if (seen >= 0) state |= VR_WATCH_TESTED | (seen & VR_SEEN_KEPT ? VR_WATCH_KEPT : 0);
        if (vr_in_camera_view(vr_game_cam, ty_game * VR_GAME_ASPECT, ty_game, q)) state |= VR_WATCH_GAME_VIEW;
        const int was = vr_watch_at_state[i];
        vr_watch_at_state[i] = state;
        if (was == state || !((state | (was < 0 ? 0 : was)) & VR_WATCH_HEADSET) || told >= 600) continue;
        told++;
        hos_log("vr: cull watch place %d (%.1f %.1f %.1f), %.1f m: headset view %d, models within 6 m %s, game camera view %d",
                i, q[0], q[1], q[2], sqrtf(d2), !!(state & VR_WATCH_HEADSET),
                !(state & VR_WATCH_TESTED) ? "never reached the sphere test" : (state & VR_WATCH_KEPT) ? "kept (one at least)" :
                "culled by the sphere test", !!(state & VR_WATCH_GAME_VIEW));
    }
}

/* -- the VR tab's "World culling" (option 923): 0 Game; 1 Headset: the gameplay LookAtCamera's float fields (+0x38 position,
   +0x44 target, +0x50 up) are the headset's eye, look and up for the rest of each frame; 2 Headset+: its double position
   and target (+0x60, +0x78) too. Called by host/hos_main.c hos_oot_free_view with after 0 before the mod's view (the game's
   own values put back where the game did not write new ones: the mod's view reads them, and the game's camera keeps its
   own), and after 1 once the mod wrote the headset's matrix. -- */
/* -- the culling's lag (CAM-11): each headset view the mod writes (hos_vr_cull_camera, after 1) is kept with the one before
   it; each headset culling snapshot (hos_vr_cull_snapshot) is compared with both. A snapshot made from the newest view is
   0 degrees from it. One made from the view before is as far from the newest as the head turned in a frame, and the side
   you turn toward is culled by the older pose. Told every 10 s while first person, at most 120 times. -- */
static float vr_head_look[2][3];   /* [0] the newest headset view's forward, [1] the one before */
static volatile int vr_head_looks;  /* views written (0, 1: not enough yet) */
static volatile int vr_head_lock;
static void vr_cull_note_head(const float back[3]) {
    while (__atomic_exchange_n(&vr_head_lock, 1, __ATOMIC_ACQUIRE)) {}
    const float l = sqrtf(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
    if (l > 1e-4f) {
        memcpy(vr_head_look[1], vr_head_look[0], sizeof vr_head_look[0]);
        for (int k = 0; k < 3; k++) vr_head_look[0][k] = -back[k] / l;
        if (vr_head_looks < 2) vr_head_looks++;
    }
    __atomic_store_n(&vr_head_lock, 0, __ATOMIC_RELEASE);
}
static float vr_angle_deg(const float a[3], const float b[3]) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return acosf(fmaxf(-1.0f, fminf(1.0f, d))) * 57.2958f;
}
static void vr_cull_lag_note(Ctx *c, uint64_t snap) {
    static volatile int lock;
    static int64_t since;
    static unsigned n, older, told;
    static float sum, worst, sum_turn;
    float f[3], l = 0;
    for (int k = 0; k < 3; k++) f[k] = -vr_load(c, snap + 0xe0 + 16 * k), l += f[k] * f[k];
    l = sqrtf(l);
    if (!(l > 1e-4f) || __atomic_load_n(&vr_head_looks, __ATOMIC_ACQUIRE) < 2) return;
    for (int k = 0; k < 3; k++) f[k] /= l;
    while (__atomic_exchange_n(&vr_head_lock, 1, __ATOMIC_ACQUIRE)) {}
    const float to_new = vr_angle_deg(f, vr_head_look[0]), to_old = vr_angle_deg(f, vr_head_look[1]),
                turn = vr_angle_deg(vr_head_look[0], vr_head_look[1]);
    __atomic_store_n(&vr_head_lock, 0, __ATOMIC_RELEASE);
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {}
    const int64_t now = hos_now_ns();
    if (!since) since = now;
    n++, sum += to_new, sum_turn += turn, worst = fmaxf(worst, to_new);
    if (turn > 0.5f && to_old < to_new) older++;   /* (only frames where the head turned enough to tell the two apart) */
    if (now - since >= 10000000000LL && told < 120) {
        told++;
        hos_log("vr: culling vs headset (10 s): %u headset culling views, %.2f degrees from the newest headset view on "
                "average (worst %.1f), %u of them nearer the view before it; the head turned %.2f degrees a frame on average",
                n, sum / n, worst, older, sum_turn / n);
        since = now, n = older = 0, sum = worst = sum_turn = 0;
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

static float vr_cull_saved[9], vr_cull_wrote_f[9];
static double vr_cull_saved_d[6], vr_cull_wrote_d[6];
static int vr_cull_wrote;   /* bit 1: the floats are ours, bit 2: the doubles */
static double vr_load_f64(Ctx *c, uint64_t a) { const uint64_t b = recomp_load64(c, a); double d; memcpy(&d, &b, 8); return d; }
static void vr_store_f64(Ctx *c, uint64_t a, double d) { uint64_t b; memcpy(&b, &d, 8); recomp_store64(c, a, b); }
static int vr_same_f32(Ctx *c, uint64_t a, float f) {   /* (bit for bit: a NaN the mod wrote is still the mod's) */
    uint32_t b;
    memcpy(&b, &f, 4);
    return recomp_load32(c, a) == b;
}
static int vr_same_f64(Ctx *c, uint64_t a, double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return recomp_load64(c, a) == b;
}
/* (vr/camera/events_host.c) World culling's mode while an event plays in first person (0: the VR menu's alone) */
static volatile int vr_cull_event_mode;
static int vr_cull_camera_mode(int asked, float eye[3], float back[3], float up[3], Ctx *c, uint64_t view) {
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = vr_load(c, view + 8 + 4 * i);
    for (int k = 0; k < 3; k++) {   /* (the view matrix's rows: right, up, back, each with -axis.eye) */
        eye[k] = -(m[k] * m[3] + m[4 + k] * m[7] + m[8 + k] * m[11]);
        up[k] = m[4 + k], back[k] = m[8 + k];
    }
    for (int k = 0; k < 3; k++)
        if (!isfinite(eye[k]) || !isfinite(up[k]) || !isfinite(back[k])) return 0;
    return asked;
}
void hos_vr_cull_camera(Ctx *c, uint64_t view, int after) {
    static volatile int lock;
    if (!hos_guest_range(view + 0x38, 0x58)) return;
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {}
    if (!after) {
        /* each field (position, target, up) still what the mod wrote: the game made none this frame, its own goes back. Field
           by field: a game update that set only some of them must not keep the mod's in the others (the view below reads
           position and target together for the heading) */
        if (vr_cull_wrote & 1)
            for (int f = 0; f < 3; f++) {
                int same = 1;
                for (int k = 3 * f; k < 3 * f + 3; k++) same &= vr_same_f32(c, view + 0x38 + 4 * k, vr_cull_wrote_f[k]);
                if (same)
                    for (int k = 3 * f; k < 3 * f + 3; k++) vr_store(c, view + 0x38 + 4 * k, vr_cull_saved[k]);
            }
        if (vr_cull_wrote & 2)
            for (int f = 0; f < 2; f++) {
                int same = 1;
                for (int k = 3 * f; k < 3 * f + 3; k++) same &= vr_same_f64(c, view + 0x60 + 8 * k, vr_cull_wrote_d[k]);
                if (same)
                    for (int k = 3 * f; k < 3 * f + 3; k++) vr_store_f64(c, view + 0x60 + 8 * k, vr_cull_saved_d[k]);
            }
        vr_cull_wrote = 0;
        for (int k = 0; k < 9; k++) vr_game_cam[k] = vr_load(c, view + 0x38 + 4 * k);
        __atomic_store_n(&vr_game_cam_time, hos_now_ns(), __ATOMIC_RELEASE);
        __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
        return;
    }
    float eye[3], back[3], up[3];
    const int asked = rx_option(923, 0);
    const int mode = vr_cull_camera_mode(vr_cull_event_mode > asked ? vr_cull_event_mode : asked, eye, back, up, c, view);
    if (isfinite(back[0]) && isfinite(back[1]) && isfinite(back[2])) vr_cull_note_head(back);   /* (the culling's lag) */
    static int told = -1;
    if (mode != told) {
        told = mode;
        hos_log("vr: world culling: %s", mode == 2 ? "Headset+ (test): the game camera's position and target, floats and "
                "doubles, are the headset's eye and look for the rest of each frame" : mode == 1 ? "Headset (test): the game "
                "camera's float position, target and up are the headset's for the rest of each frame" : "Game (the game "
                "camera's own fields)");
    }
    if (vr_cull_watch_on()) vr_cull_watch_points();
    if (mode >= 1) {
        float pos[3], at[3], dist2 = 0;
        for (int k = 0; k < 3; k++) {
            pos[k] = vr_load(c, view + 0x38 + 4 * k), at[k] = vr_load(c, view + 0x44 + 4 * k);
            dist2 += (at[k] - pos[k]) * (at[k] - pos[k]);
        }
        const float dist = dist2 > 0.01f && isfinite(dist2) ? sqrtf(dist2) : 1.0f;   /* (the target stays as far ahead) */
        for (int k = 0; k < 9; k++) vr_cull_saved[k] = vr_load(c, view + 0x38 + 4 * k);
        for (int k = 0; k < 3; k++) {
            vr_cull_wrote_f[k] = eye[k], vr_cull_wrote_f[3 + k] = eye[k] - back[k] * dist, vr_cull_wrote_f[6 + k] = up[k];
        }
        for (int k = 0; k < 9; k++) vr_store(c, view + 0x38 + 4 * k, vr_cull_wrote_f[k]);
        vr_cull_wrote = 1;
        if (mode == 2) {
            int sane = 1;   /* (doubles that match the floats the game made: its own position and target) */
            for (int k = 0; k < 6; k++) {
                vr_cull_saved_d[k] = vr_load_f64(c, view + 0x60 + 8 * k);
                sane &= isfinite(vr_cull_saved_d[k]) && fabs(vr_cull_saved_d[k] - vr_cull_saved[k]) < 1.0;
            }
            if (sane) {
                for (int k = 0; k < 6; k++) vr_cull_wrote_d[k] = vr_cull_wrote_f[k], vr_store_f64(c, view + 0x60 + 8 * k, vr_cull_wrote_d[k]);
                vr_cull_wrote |= 2;
            } else {
                static int told_bad;
                if (!told_bad) told_bad = 1, hos_log("vr: world culling: the camera's doubles do not match its floats: left alone");
            }
        }
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}
