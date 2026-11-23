/* Hookshot VR: the game's events (talks, signs, Navi, chests, doors, arrivals, story cinematics, boss intros) in VR.

   Game-side code, included by host/hos_main.c (its VR HOOK block) in a fixed order: it uses that file's helpers and
   the files included before it (vr/input/stick_host.c: the event's name). Not compiled on its own.

   ---- Research (RX_VR_EVENT_TEST, headless or in the headset) ----------------------------------------------------------
   1: logs what an event does to the cameras and to Link: every update of the gameplay view while an event plays (and a
      second either side), with the event's name, its camera's position and target, the cuts (the camera jumping), Link's
      position and whether his update still runs; and once per event every other camera view the game updates.
   2: as 1, and without a headset the gameplay view is put at Link's eye, looking where he faces (the first-person view,
      flat), so a headless run's shots show what the headset shows during the event.
   3: as 2, and the game camera's own position and target fields are that eye's (World culling's Headset+).
   4: the rules below without a headset: the VR menu's event rows from RX_VR_EVENT_OPTIONS ("912=2,1010=1,1011=0,1012=0"),
      the gameplay view put where they say (your eyes, the director's camera, a theatre seat, Link's eyes), head still.
   (Ported 21 Nov from claude/cutscenes 3b0cc315, made on the old engine, to the v3 nightly: the game side is the same game,
   so its addresses hold; the menu is the VR menu's now, vr/menu/vr_menu.inc, Events -> Cutscenes.) */
static int oot_vr_event_test(void) {
    static int v = -1;
    if (v < 0) { const char *e = vr_research_getenv("RX_VR_EVENT_TEST"); v = e ? atoi(e) : 0; }
    return v;
}

/* the VR tab's event rows (rx_option), or RX_VR_EVENT_OPTIONS' values in a headless RX_VR_EVENT_TEST=4 run */
static int oot_vr_ev_option(int id, int otherwise) {
    const char *e = oot_vr_event_test() >= 4 ? vr_research_getenv("RX_VR_EVENT_OPTIONS") : NULL;
    for (const char *p = e; p && *p;) {
        int k = 0, v = 0;
        if (sscanf(p, "%d=%d", &k, &v) == 2 && k == id) return v;
        p = strchr(p, ',');
        if (p) p++;
    }
    return e ? otherwise : rx_option(id, otherwise);
}

/* a readable name at addr: a char pointer, or a sead SafeString (its text pointer at +8); "" when neither */
static void oot_vr_guess_text(Ctx *c, uint64_t addr, char out[48]) {
    out[0] = 0;
    for (int tries = 0; tries < 2; tries++) {
        if (addr < 0x80000000ULL || !hos_guest_range(addr, 48)) return;
        const uint64_t p = recomp_load64(c, tries ? addr + 8 : addr);
        if (p >= 0x80000000ULL && hos_guest_range(p, 48)) {
            int n = 0;
            for (; n < 47; n++) { const char ch = (char)recomp_load8(c, p + n); if (ch < 32 || ch > 126) break; out[n] = ch; }
            if (n >= 4 && !recomp_load8(c, p + n)) { out[n] = 0; return; }
        }
        out[0] = 0;
    }
}

/* ---- The WorldMgr's demo palette: a story cinematic's lighting, set per cut --------------------------------------------
   A cinematic's _Env flow (e.g. Dm_Kokiri_0030_Env) sets a demo palette for each cut (Event_SetDemoPaletteForTimeline:
   Dm_Kokiri_0030_C05, _C26, ...; the AI node ExecuteWmSetDemoPaletteForTimeline): the shot's own lighting, fog and colour
   grade, which replace the area's for as long as it is set. The node's enter calls the WorldMgr's set (f_83473d50(wm, name),
   f_83473dc0 for a transition to one), its leave the reset (f_83473fd0: the names emptied). The WorldMgr is *(*0x84c9e898)
   (as those nodes load it); its two palette slots are 0x11a0 bytes apart from +0xde8, the one in use (0 or 1) at +0x3128;
   in a slot the palette's name is the text at +0xd88 (its capacity +0xd90), the one a transition goes to at +0xe98. */
#define kVrWorldMgrSlot 0x84c9e898ULL
static uint64_t oot_vr_world_mgr(Ctx *c) {
    const uint64_t holder = recomp_load64(c, kVrWorldMgrSlot);
    const uint64_t wm = holder >= 0x80000000ULL && hos_guest_range(holder, 8) ? recomp_load64(c, holder) : 0;
    return wm >= 0x80000000ULL && hos_guest_range(wm, 0x3130) ? wm : 0;
}
/* the demo palette in use (and the one it changes to, "" none) into out[2][48]; 0 when the WorldMgr cannot be read */
static int oot_vr_demo_palette(Ctx *c, char out[2][48]) {
    out[0][0] = out[1][0] = 0;
    const uint64_t wm = oot_vr_world_mgr(c);
    if (!wm) return 0;
    const uint32_t slot = recomp_load32(c, wm + 0x3128);
    const uint64_t base = wm + 0xde8 + (slot < 2 ? slot * 0x11a0ULL : 0);
    for (int k = 0; k < 2; k++) {
        const uint64_t text = recomp_load64(c, base + (k ? 0xe98 : 0xd88));
        if (text < 0x80000000ULL || !hos_guest_range(text, 48)) continue;
        int n = 0;
        for (; n < 47; n++) { const char ch = (char)recomp_load8(c, text + n); if (ch < 32 || ch > 126) break; out[k][n] = ch; }
        out[k][n] = 0;
    }
    return 1;
}

typedef struct {
    int64_t last_event_at;     /* (ns) an event was last seen playing */
    int64_t began_at;
    uint64_t last_view;
    float last_pos[3], last_look[3], last_link[3];
    int have_last, cuts, lines;
    char event[64];
    uint64_t seen_views[16];   /* other views told this event */
    int seen_n;
} OotVrEventResearch;
static OotVrEventResearch oot_vr_evr;

/* (research) every view the game updates (host/hos_main.c hos_oot_free_view, before it picks the gameplay one): while an
   event plays, each other view is told once with its position, so a cinematic drawn through another camera shows up */
void hos_vr_event_any_view(Ctx *c, uint64_t view) {
    if (!oot_vr_event_test() || !view) return;
    OotVrEventResearch *r = &oot_vr_evr;
    const int64_t now = hos_now_ns();
    if (!r->last_event_at || now - r->last_event_at > 1000000000LL) return;
    if (view == oot_gameplay_view(c)) return;
    for (int i = 0; i < r->seen_n; i++) if (r->seen_views[i] == view) return;
    if (r->seen_n >= 16) return;
    r->seen_views[r->seen_n++] = view;
    hos_log("vr: event test: another view %#llx updated during %s: at %.2f %.2f %.2f looking at %.2f %.2f %.2f",
            (unsigned long long)view, r->event, vr_load(c, view + 0x38), vr_load(c, view + 0x3c), vr_load(c, view + 0x40),
            vr_load(c, view + 0x44), vr_load(c, view + 0x48), vr_load(c, view + 0x4c));
}

/* (research) each gameplay-view update: the event, its camera, Link */
static void oot_vr_event_research(Ctx *c, uint64_t view) {
    if (!oot_vr_event_test()) return;
    OotVrEventResearch *r = &oot_vr_evr;
    const int64_t now = hos_now_ns();
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    char event[64] = "";
    const int playing = events && recomp_load64(c, events + 104);
    if (playing && !oot_vr_event_name(c, events, event)) snprintf(event, sizeof event, "an event");
    if (playing) {
        if (strcmp(r->event, event)) {
            snprintf(r->event, sizeof r->event, "%s", event);
            r->began_at = now, r->cuts = 0, r->seen_n = 0;
            const uint64_t camera = oot_gameplay_camera(c);
            const uint64_t singleton = recomp_load64(c, 0x84ca6bc0ULL);
            const uint64_t manager = singleton ? recomp_load64(c, singleton) : 0;
            hos_log("vr: event test: %s begins (camera zero %#llx, its view %#llx, this view %#llx, %u cameras)", event,
                    (unsigned long long)camera, (unsigned long long)(camera ? recomp_load64(c, camera + 120) : 0),
                    (unsigned long long)view, manager ? recomp_load32(c, manager + 32) : 0);
        }
        r->last_event_at = now;
    } else if (r->event[0] && now - r->last_event_at > 1000000000LL) {
        hos_log("vr: event test: %s over after %.1f s, %d cuts", r->event, (r->last_event_at - r->began_at) * 1e-9f, r->cuts);
        r->event[0] = 0;
    }
    const int near = r->last_event_at && now - r->last_event_at < 1000000000LL;
    float pos[3], at[3], link[3] = {0, 0, 0}, look[3];
    for (int k = 0; k < 3; k++) pos[k] = vr_load(c, view + 0x38 + 4 * k), at[k] = vr_load(c, view + 0x44 + 4 * k);
    for (int k = 0; k < 3; k++) {
        const uint32_t bits = __atomic_load_n(&oot_vr_player_xyz[k], __ATOMIC_RELAXED);
        memcpy(&link[k], &bits, 4);
    }
    const float ln = sqrtf((at[0] - pos[0]) * (at[0] - pos[0]) + (at[1] - pos[1]) * (at[1] - pos[1]) + (at[2] - pos[2]) * (at[2] - pos[2]));
    for (int k = 0; k < 3; k++) look[k] = ln > 1e-4f ? (at[k] - pos[k]) / ln : 0;
    int cut = 0;
    if (r->have_last && r->last_view == view) {
        const float jump = sqrtf((pos[0] - r->last_pos[0]) * (pos[0] - r->last_pos[0]) + (pos[1] - r->last_pos[1]) * (pos[1] - r->last_pos[1]) +
                                 (pos[2] - r->last_pos[2]) * (pos[2] - r->last_pos[2]));
        const float turn = acosf(fmaxf(-1, fminf(1, look[0] * r->last_look[0] + look[1] * r->last_look[1] + look[2] * r->last_look[2])));
        cut = jump > 0.75f || turn > 0.35f;
    }
    const float link_jump = r->have_last ? sqrtf((link[0] - r->last_link[0]) * (link[0] - r->last_link[0]) +
                                                 (link[1] - r->last_link[1]) * (link[1] - r->last_link[1]) +
                                                 (link[2] - r->last_link[2]) * (link[2] - r->last_link[2])) : 0;
    if (near && cut) r->cuts++;
    static int64_t line_at;   /* (the view's matrix is updated several times a frame: a line at most every 40 ms, or at a cut) */
    if (near && r->lines < 12000 && (cut || link_jump > 1.0f || now - line_at > 40000000LL)) {
        r->lines++, line_at = now;
        const int64_t seen = __atomic_load_n(&oot_vr_player_seen, __ATOMIC_ACQUIRE);
        char step[48] = "", step2[48] = "";
        if (events) {
            const uint64_t node = recomp_load64(c, events + 0x78);
            if (node >= 0x80000000ULL && hos_guest_range(node, 0xa0)) {
                oot_vr_guess_text(c, node, step);
                oot_vr_guess_text(c, node + 0x98, step2);
            }
        }
        char palette[2][48];
        oot_vr_demo_palette(c, palette);
        hos_log("vr: event test: %.2f s %s%s step %s/%s palette %s>%s cam %.2f %.2f %.2f look %.2f %.2f %.2f (%.1f m from Link) Link %.2f %.2f "
                "%.2f%s, his update %.0f ms ago",
                (now - r->began_at) * 1e-9f, event[0] ? event : "(none)", cut ? " CUT" : "", step[0] ? step : "-",
                step2[0] ? step2 : "-", palette[0][0] ? palette[0] : "-", palette[1][0] ? palette[1] : "-", pos[0], pos[1], pos[2], look[0], look[1], look[2],
                sqrtf((pos[0] - link[0]) * (pos[0] - link[0]) + (pos[1] - link[1]) * (pos[1] - link[1]) + (pos[2] - link[2]) * (pos[2] - link[2])),
                link[0], link[1], link[2], link_jump > 1.0f ? " LINK MOVED" : "", seen ? (now - seen) * 1e-6f : -1.0f);
    }
    memcpy(r->last_pos, pos, sizeof pos), memcpy(r->last_look, look, sizeof look), memcpy(r->last_link, link, sizeof link);
    r->last_view = view, r->have_last = 1;
}

/* ==== The rules: how each kind of event is shown in first person ==========================================================
   What the game does (headless 19 Nov, RX_VR_EVENT_TEST, the player's save; and the event flows' own steps):
   - Every event moves the same camera: camera zero, the gameplay view (the one the headset replaces). No event drew
     through another camera.
   - A talk (Npc_Kokiri_012_Child_C3_T): EventPlayerConversate eases the camera to 3.2 m over Link's shoulder, no cut;
     Link does not move. An arrival (Dm_SY_BancChangeArrive, 0.6 s): EventChangeTemporarilyToPlayerCamera, the camera
     4.3 m behind Link, no cut. Both look right from Link's eyes.
   - A story cinematic (Dm_Kokiri_0030, 148 s): camera animations (EventMoveCameraAnimationInstructions) 15 to 136 m from
     Link, 15 cuts, and at every cut the _Env flow sets that cut's demo palette (Dm_Kokiri_0030_C27, _C29 ... _C09): the
     lighting, fog and grade change with each shot. Link's own actor stayed where he was the whole time (in his house).
     Boss intros, songs and the sages' awakenings (Dm_Deku_BossIn, Dm_MelodyGet_*, Dm_Awake_*) draw an event model of
     Link (their *_Player_Child model project) and warp the real Link (EventWarpToTargetActor, EventWarpToTargetPosAndRot):
     from Link's eyes the view went wherever the event put the real Link, off to the side or under the floor, while the
     Link in the shot was the event's model. The old rule (Dm_ + area + a number) missed 90 of the 327 Dm_ flows, all the
     boss intros and outros, songs, awakenings, fairy fountains and Inside Jabu-Jabu's among them, and showed them from
     Link's eyes.
   So: a director's cinematic is every Dm_/DmF_ event but the system ones (Dm_SY_*: map changes, item gets, chests,
   bottles, the ocarina). Everything else is an interaction, shown from your eyes unless its camera goes far from Link
   (a shot of something else: a gate opening, a pan to an NPC), which is then shown as a cinematic's shot is.

   Cutscene view (VR menu Events -> Cutscenes, option 912; the user's rules, 21 Nov: "dialogue never puts you in 2D", and
   the big story cutscenes on a screen floating in the world by default):
   - Flat (0): every event's own shots on the world screen (the old Flat screen), the ocarina in the hands excepted.
   - Screen (1): talks, signs, Navi, item gets, doors and arrivals in VR, from your eyes (a talk's shot far from Link as
     3D cutscenes says); story cinematics on the world screen: the game's picture on a screen fixed in the room where it
     opened (vr/headset/eye_rendering.inc, the flat frame's quad), not following your head.
   - In 3D (2, the default since 22 Nov, the user's: "the safest"): as Screen, and story cinematics in the world too, as
     3D cutscenes says (Director by default: a pan through a new area, people talking away from Link, each cut, all in VR).
   Third person: the same, the VR view there being the game's own camera with your head (no far shots: it is the game's
   camera already). Before 21 Nov third person showed every event flat, and that was the "dialogue in 2D" the user met.

   Ways to show a cinematic in 3D (VR menu "3D cutscenes", option 1010):
   - Director's camera: the VR view at the game's camera, level (its turn only: no roll or pitch put on your head), your
     head looking around from there. Cuts are the director's, so the staging (who stands where) is always right.
   - Theatre seat: a seat at the first shot's camera, kept for as long as the director's camera stays within
     kVrTheatreSeatM of it; only a new scene moves you (Dm_Kokiri_0030: 8 seats for 16 shots, headless 19 Nov). Fewer cuts, but things
     the director placed for another angle may be seen popping in. (Not by the camera's target: in a camera animation
     the LookAtCamera's target is only about a metre ahead of it, headless 19 Nov.)
   - Link's spot: Link's eyes where he stood when it began (not where the event warps him), without the VR body.
   - Follow Link: your eyes on the real Link all through it, as in play (boss intros warp him: the view goes with him).
   Lighting (VR menu "Cutscene light", option 1011): Auto keeps the game's per-shot palettes at the director's camera
   (each is lit for its shot) and holds the first one otherwise; Every shot, Held, or The area's (no cinematic palettes).
   Detail (VR menu Debug, "Event culling", option 1012): for a cinematic (or a far shot) in 3D in first person, however it is
   shown, the game camera's position and target fields are the VR eye's for the rest of each frame (World culling's
   Headset+), so what the game decides from them follows where you are and where you look, not the director's camera.
   The user's headset notes (22 Nov), cinematics from Link's eyes on the old rule (Follow Link's view, with no such mode):
   "an invisible box floating around, like a window with different lighting from the area", and the lighting and detail
   changing a lot. What the game fits to its camera's position and target, the box being the region it lit and drew in
   full for the director's shot (its shadow map's reach among them), seen from somewhere else. At the director's camera
   the eye is the camera's already; the head turning off its shot is what the mode then follows. */
enum { kVrEventNone, kVrEventSystem, kVrEventItemGet, kVrEventOcarina, kVrEventCinematic, kVrEventInteraction };
enum { kVrViewEyes, kVrViewFlat, kVrViewDirector, kVrViewTheatre, kVrViewAnchored };
enum { kVrLightAuto, kVrLightShot, kVrLightHeld, kVrLightArea };
#define kVrFarShotM 8.0f          /* an interaction's camera this far from Link: a shot of something else */
#define kVrNearShotM 6.0f         /* ... and back to your eyes under this (no flicker at the edge) */
#define kVrTheatreSeatM 35.0f     /* the director's camera this far from the seat: a new scene, a new seat */
#define kVrAnchorFreshNs 500000000LL   /* Link's eye this recent when a cinematic begins is where it began (else a load) */

/* The kind of event, from its name ("" none) */
static int oot_vr_event_kind(const char *name) {
    if (!name[0]) return kVrEventNone;
    if (!strncmp(name, "Dm_SY_Ocarina", 13)) return kVrEventOcarina;
    if (!strncmp(name, "Dm_SY_Get", 9) || !strncmp(name, "Dm_SY_Treasurebox", 17) || !strcmp(name, "Dm_SY_ShopGet"))
        return kVrEventItemGet;
    if (!strncmp(name, "Dm_SY_", 6) || !strncmp(name, "DmF_SY_", 7)) return kVrEventSystem;
    if (!strncmp(name, "Dm_", 3) || !strncmp(name, "DmF_", 4)) return kVrEventCinematic;
    return kVrEventInteraction;
}
/* An event that sets a cinematic's demo palette (Dm_*_Cnn, not Dm_SY_) plays a cinematic inside it, whatever its own name:
   the new game's Opening_Introduction plays the Deku Tree's opening (Dm_Kokiri_0030's palettes C11, C12...) under its own
   name (the user's headset log, 22 Nov: shown "from your eyes", the first-person body on, the cinematic's Link without his
   hair). So an interaction with such a palette is a cinematic, and stays one until the event ends (seen: the event's
   oot_vr_ev.palette_cine). */
static int oot_vr_palette_kind(int kind, const char *palette, int *seen) {
    if (kind == kVrEventInteraction && (!strncmp(palette, "Dm_", 3) || !strncmp(palette, "DmF_", 4)) &&
        strncmp(palette, "Dm_SY_", 6) && strncmp(palette, "DmF_SY_", 7))
        *seen = 1;
    return kind == kVrEventInteraction && *seen ? kVrEventCinematic : kind;
}
/* How this update is shown. policy: Cutscene view (912: 0 flat, 1 cinematics on the world screen, 2 cinematics in 3D);
   view: 3D cutscenes (1010: 0 director, 1 theatre, 2 Link's spot, 3 follow Link); far: the event's camera is far from Link
   (oot_vr_far_shot); ocarina: the ocarina is played from the VR hands (980), its event stays in your view; first_person:
   0 in third person, where the VR view is the game's own camera (no far shots, no seat) */
static int oot_vr_event_style(int kind, int policy, int view, int far, int ocarina, int first_person) {
    if (kind == kVrEventNone || (kind == kVrEventOcarina && ocarina)) return kVrViewEyes;
    if (policy <= 0) return kVrViewFlat;
    const int shot = view == 1 ? kVrViewTheatre : view == 2 ? kVrViewAnchored : view == 3 ? kVrViewEyes : kVrViewDirector;
    if (kind == kVrEventCinematic) return policy == 1 ? kVrViewFlat : first_person ? shot : kVrViewEyes;
    if (kind == kVrEventInteraction && far && first_person) return view >= 2 ? kVrViewEyes : shot;
    return kVrViewEyes;   /* (a talk, a sign, Navi, an item get, a door, an arrival: never flat but by Flat) */
}
/* A prerecorded movie (romfs Event/Movie, 26 .webm: the opening legend Dm_Kokiri_0000, Kakariko's _0025, the Sun's Song's
   clips, the Temple of Time's...) is a video, not the world: the director's camera cannot show it, and a VR view would
   show it as the game's 2D picture over a stereo world. So while an event plays and the game takes a movie's pictures
   (host/nn_video.c, hos_video_acquired_ns, within kVrMovieNs), it is on the world screen (fixed where it opened) whatever
   the rows say. (The user's wish, later: the world around you at Link, the movie's screen in front of it.) A Dm_ name is
   not a movie (Dm_Kokiri_0030 is real time), hence the decoder, not the name. */
#define kVrMovieNs 300000000LL
static int oot_vr_movie_style(int style, int kind, int movie) { return movie && kind != kVrEventNone ? kVrViewFlat : style; }
/* far (1) or near (0) after this update, with the camera dist metres from Link */
static int oot_vr_far_shot(int far, float dist) { return far ? !(dist < kVrNearShotM) : dist > kVrFarShotM; }
/* The lighting rule for this update (option 1011 and the style): what oot_vr_palette_step does */
static int oot_vr_light_mode(int option, int style) {
    if (style == kVrViewFlat) return kVrLightShot;   /* (a flat screen is the director's own picture) */
    if (option == kVrLightAuto) return style == kVrViewDirector ? kVrLightShot : kVrLightHeld;
    return option;
}

/* The theatre seat: kept while the director's camera stays near it */
typedef struct {
    int have, scenes;
    float seat[3], back[3];   /* (back: the seat's level heading, from where its first shot looked toward the seat) */
} OotVrTheatre;
/* one update with the director's camera at cam, looking at target: 1 when a new scene moved the seat */
static int oot_vr_theatre_step(OotVrTheatre *t, const float cam[3], const float target[3]) {
    float dc = 0;
    for (int k = 0; k < 3; k++) dc += (cam[k] - t->seat[k]) * (cam[k] - t->seat[k]);
    if (t->have && dc < kVrTheatreSeatM * kVrTheatreSeatM) return 0;
    const float bx = cam[0] - target[0], bz = cam[2] - target[2], bl = hypotf(bx, bz);
    if (bl > 1e-3f) t->back[0] = bx / bl, t->back[1] = 0, t->back[2] = bz / bl;
    else if (!t->have) t->back[0] = 0, t->back[1] = 0, t->back[2] = 1;   /* (looking straight down: keep the last heading) */
    memcpy(t->seat, cam, sizeof t->seat);
    t->have = 1, t->scenes++;
    return 1;
}

/* The demo palette held through an event (Held), or kept off (The area's), and the game's own put back at the end */
typedef struct {
    char held[48];   /* the palette held (Held: the event's first) */
    char game[48];   /* the last palette the game itself set while ours stood in for it */
    int touched;     /* the mod changed the WorldMgr's palette in this event */
} OotVrPalette;
enum { kVrPaletteKeep, kVrPaletteSet, kVrPaletteReset };
/* What to do with the WorldMgr's demo palette this update: kVrPaletteSet (to want), kVrPaletteReset or nothing. now: the
   palette in use ("" none); ending: the event is over (or this rule no longer applies): the game's own goes back, once */
static int oot_vr_palette_step(OotVrPalette *p, int mode, const char *now, int ending, char want[48]) {
    want[0] = 0;
    if (ending || mode == kVrLightShot) {
        int action = kVrPaletteKeep;
        if (p->touched && !strcmp(now, p->held))   /* (still ours: the game's last goes back; it may have moved on itself) */
            action = p->game[0] ? kVrPaletteSet : kVrPaletteReset, snprintf(want, 48, "%s", p->game);
        memset(p, 0, sizeof *p);
        return action;
    }
    if (mode == kVrLightHeld) {
        if (!p->held[0]) {
            if (now[0]) snprintf(p->held, sizeof p->held, "%s", now), snprintf(p->game, sizeof p->game, "%s", now);
            return kVrPaletteKeep;
        }
        if (!strcmp(now, p->held)) return kVrPaletteKeep;
        snprintf(p->game, sizeof p->game, "%s", now), p->touched = 1;
        snprintf(want, 48, "%s", p->held);
        return kVrPaletteSet;
    }
    if (mode == kVrLightArea && now[0]) {   /* (held stays "": a later restore compares with none) */
        snprintf(p->game, sizeof p->game, "%s", now), p->touched = 1;
        return kVrPaletteReset;
    }
    return kVrPaletteKeep;
}

/* ---- The game side, each gameplay-view update -------------------------------------------------------------------------- */
typedef struct {
    int style, kind, far, light;
    int palette_cine;         /* (this event set a cinematic's demo palette: oot_vr_palette_kind) */
    char event[64];
    float free_eye[3];        /* Link's eye outside cinematics (Link's eyes in a cinematic stay there) */
    int64_t free_eye_at;      /* (ns) when it was read: an old one (before a map load) is not used */
    float anchor[3];
    int anchored;             /* anchor holds this cinematic's eye */
    OotVrTheatre theatre;
    OotVrPalette palette;
} OotVrEventView;
static OotVrEventView oot_vr_ev;
static volatile int oot_vr_ev_lock;   /* (the view is updated from several of the game's workers) */
static int oot_vr_event_flat(void) { return __atomic_load_n(&oot_vr_ev.style, __ATOMIC_RELAXED) == kVrViewFlat; }

/* the game's own demo palette calls (the WorldMgr takes its own lock in them): set to name, or reset ("" none) */
static void oot_vr_palette_apply(Ctx *c, int action, const char *name) {
    static uint64_t holder;   /* (guest memory: the setter's argument points at a text pointer, as the AI node passes) */
    if (!holder) holder = hos_sys_alloc(80, 16);
    const uint64_t wm = oot_vr_world_mgr(c);
    A64Fn set = a64_lookup(0x83473d50ULL), reset = a64_lookup(0x83473fd0ULL);
    if (!wm || !holder || !set || !reset) return;
    memset(A64_PTR(holder), 0, 80);
    snprintf((char *)A64_PTR(holder + 16), 48, "%s", name);
    recomp_store64(c, holder, holder + 16);
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x100) & ~0xfULL;
    c->x[0] = wm, c->x[1] = holder;
    (action == kVrPaletteSet && name[0] ? set : reset)(c);
    *c = saved;
}

static const char *oot_vr_style_name(int style) {
    static const char *const n[] = {"your eyes", "the world screen (flat)", "the director's camera", "a theatre seat",
                                    "Link's eyes where it began"};
    return (unsigned)style < 5 ? n[style] : "?";
}

/* Each update of the gameplay view, first person or third: which way this update is shown (oot_vr_ev.style), the palette
   rule applied, World culling's event mode set. */
static void oot_vr_event_frame(Ctx *c, uint64_t view, int first_person, int height_cm) {
    while (__atomic_exchange_n(&oot_vr_ev_lock, 1, __ATOMIC_ACQUIRE)) {}
    OotVrEventView *e = &oot_vr_ev;
    char event[64] = "";
    const uint64_t events = oot_singleton(c, OOT_EVENTMGR_SLOT);
    if (events && recomp_load64(c, events + 104) && !oot_vr_event_name(c, events, event)) snprintf(event, sizeof event, "an event");
    if (strcmp(event, e->event)) e->palette_cine = 0;
    char palette_now[2][48];
    const int kind = oot_vr_palette_kind(oot_vr_event_kind(event), oot_vr_demo_palette(c, palette_now) ? palette_now[0] : "",
                                         &e->palette_cine);
    float link[3], cam[3], target[3], dist = 0;
    int child = 1;
    const int link_ok = oot_vr_player_eye(c, height_cm, link, &child);
    for (int k = 0; k < 3; k++) cam[k] = vr_load(c, view + 0x38 + 4 * k), target[k] = vr_load(c, view + 0x44 + 4 * k);
    if (link_ok) dist = sqrtf((cam[0] - link[0]) * (cam[0] - link[0]) + (cam[1] - link[1]) * (cam[1] - link[1]) +
                              (cam[2] - link[2]) * (cam[2] - link[2]));
    const int64_t now = hos_now_ns();
    if (kind == kVrEventNone || !first_person) e->far = 0, e->anchored = 0, memset(&e->theatre, 0, sizeof e->theatre);
    else if (link_ok) e->far = oot_vr_far_shot(e->far, dist);
    if (kind != kVrEventCinematic && link_ok) memcpy(e->free_eye, link, sizeof link), e->free_eye_at = now;
    int style = oot_vr_event_style(kind, oot_vr_ev_option(912, 2), oot_vr_ev_option(1010, 0), e->far, oot_vr_ev_option(980, 0),
                                   first_person);
    {
        extern int64_t hos_video_acquired_ns;   /* (host/nn_video.c) */
        const int64_t shown = __atomic_load_n(&hos_video_acquired_ns, __ATOMIC_RELAXED);
        const int movie = shown && now - shown < kVrMovieNs;
        if (movie && kind != kVrEventNone && style != kVrViewFlat && strcmp(event, e->event))
            hos_log("vr: event %s plays a movie: on the world screen", event);
        style = oot_vr_movie_style(style, kind, movie);
    }
    if (style == kVrViewAnchored && !e->anchored) {   /* (where Link stood before the event: never where it warps him) */
        if (e->free_eye_at && now - e->free_eye_at < kVrAnchorFreshNs) memcpy(e->anchor, e->free_eye, sizeof e->anchor), e->anchored = 1;
        else if (link_ok) memcpy(e->anchor, link, sizeof e->anchor), e->anchored = 1;   /* (straight after a load: where he is) */
        else style = kVrViewDirector;
    }
    if (style == kVrViewTheatre && oot_vr_theatre_step(&e->theatre, cam, target))
        hos_log("vr: %s: theatre seat %d at %.1f %.1f %.1f, the action at %.1f %.1f %.1f", event, e->theatre.scenes, cam[0],
                cam[1], cam[2], target[0], target[1], target[2]);
    if (style != e->style || strcmp(event, e->event)) {
        if (kind != kVrEventNone)
            hos_log("vr: event %s (%s) shown %s %s (its camera %.1f m from Link)", event,
                    kind == kVrEventCinematic ? (e->palette_cine ? "a cinematic, by its demo palette" : "a cinematic") :
                    kind == kVrEventInteraction ? "an interaction" :
                    kind == kVrEventItemGet ? "an item get" : kind == kVrEventOcarina ? "the ocarina" : "a system event",
                    style == kVrViewFlat ? "on" : "from", oot_vr_style_name(style), dist);
        snprintf(e->event, sizeof e->event, "%s", event);
    }
    e->kind = kind;
    __atomic_store_n(&e->style, style, __ATOMIC_RELAXED);
    {   /* the cinematic lighting */
        char now[2][48], want[48];
        if (oot_vr_demo_palette(c, now)) {
            /* (third person: the view is the game's own camera, as at the director's) */
            const int mode = oot_vr_light_mode(oot_vr_ev_option(1011, 0), first_person || style == kVrViewFlat ? style : kVrViewDirector);
            const int action = oot_vr_palette_step(&e->palette, mode, now[0], kind == kVrEventNone, want);
            if (action != kVrPaletteKeep) {
                static unsigned told;
                if (told++ < 200)
                    hos_log("vr: cinematic lighting: the game's %s %s (%s)", now[0][0] ? now[0] : "(none)",
                            action == kVrPaletteSet ? (kind == kVrEventNone ? "put back to" : "held at") : "reset to the area's",
                            action == kVrPaletteSet ? want : "Cinematic lighting");
                oot_vr_palette_apply(c, action, want);
            }
        }
    }
    const int in_3d = style == kVrViewDirector || style == kVrViewTheatre || style == kVrViewAnchored ||
                      (style == kVrViewEyes && kind == kVrEventCinematic);   /* (Follow Link) */
    vr_cull_event_mode = first_person && in_3d && !oot_vr_ev_option(1012, 0) ? 2 : 0;
    __atomic_store_n(&oot_vr_ev_lock, 0, __ATOMIC_RELEASE);
}

/* The eye for Link's eyes in a cinematic (1: eye set to where Link stood when it began) */
static int oot_vr_event_anchor(float eye[3]) {
    if (oot_vr_ev.style != kVrViewAnchored || !oot_vr_ev.anchored) return 0;
    memcpy(eye, oot_vr_ev.anchor, sizeof oot_vr_ev.anchor);
    return 1;
}

/* The VR view's matrix (rows right, up, back, each with -axis.eye) from a level heading frame and the head's rotation q
   (x y z w): axis = the frame turned by the head, as the gameplay view's own (vr/camera/view_host.c) */
static void oot_vr_view_matrix(const float right[3], const float up[3], const float back[3], const float q[4],
                               const float eye[3], float axis[3][3], float m[12]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) axis[a][k] = right[k] * r[0][a] + up[k] * r[1][a] + back[k] * r[2][a];
    for (int row = 0; row < 3; row++) {
        m[row * 4 + 0] = axis[row][0], m[row * 4 + 1] = axis[row][1], m[row * 4 + 2] = axis[row][2];
        m[row * 4 + 3] = -(axis[row][0] * eye[0] + axis[row][1] * eye[1] + axis[row][2] * eye[2]);
    }
}

/* The director's camera or the theatre seat: where the VR view stands and its level heading frame (back from the subject
   toward the camera); 0 when this update is not shown that way */
static int oot_vr_event_vantage(Ctx *c, uint64_t view, float eye[3], float right[3], float up[3], float back[3]) {
    const int style = oot_vr_ev.style;
    if (style != kVrViewDirector && style != kVrViewTheatre) return 0;
    if (style == kVrViewTheatre && oot_vr_ev.theatre.have) {
        memcpy(eye, oot_vr_ev.theatre.seat, 3 * sizeof(float)), memcpy(back, oot_vr_ev.theatre.back, 3 * sizeof(float));
    } else {
        float at[3];
        for (int k = 0; k < 3; k++) eye[k] = vr_load(c, view + 0x38 + 4 * k), at[k] = vr_load(c, view + 0x44 + 4 * k);
        const float bx = eye[0] - at[0], bz = eye[2] - at[2], bl = hypotf(bx, bz);
        if (!(bl > 1e-4f)) return 0;
        back[0] = bx / bl, back[1] = 0, back[2] = bz / bl;
    }
    up[0] = 0, up[1] = 1, up[2] = 0;
    right[0] = back[2], right[1] = 0, right[2] = -back[0];   /* (up x back) */
    return isfinite(eye[0]) && isfinite(eye[1]) && isfinite(eye[2]);
}

/* (research, RX_VR_EVENT_TEST 2 to 4, no headset) the gameplay view put where first person would show it, the head still:
   2 and 3 Link's eye looking level where he faces (3 with World culling's Headset+); 4 by the rules above. 1 when written
   (the caller then applies World culling's after-step). */
static int oot_vr_event_research_view(Ctx *c, uint64_t view) {
    const int test = oot_vr_event_test();
    if (test < 2) return 0;
    float eye[3], right[3], up[3] = {0, 1, 0}, back[3], axis[3][3], m[12];
    const float still[4] = {0, 0, 0, 1};
    if (test >= 4) oot_vr_event_frame(c, view, 1, 160);
    else vr_cull_event_mode = test == 3 ? 2 : 0;
    if (!(test >= 4 && oot_vr_event_vantage(c, view, eye, right, up, back))) {
        const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
        int child = 1;
        if (!actor || !oot_vr_player_eye(c, 160, eye, &child)) return 0;
        if (test >= 4) {
            if (oot_vr_ev.style == kVrViewFlat) return 0;
            oot_vr_event_anchor(eye);
        }
        const float fx = vr_load(c, actor + 816), fz = vr_load(c, actor + 840), fl = hypotf(fx, fz);
        if (!(fl > .5f)) return 0;
        back[0] = -fx / fl, back[1] = 0, back[2] = -fz / fl;
        right[0] = back[2], right[1] = 0, right[2] = -back[0];
    }
    oot_vr_view_matrix(right, up, back, still, eye, axis, m);
    for (int i = 0; i < 12; i++) vr_store(c, view + 8 + 4 * i, m[i]);
    return 1;
}
