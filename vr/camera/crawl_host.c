/* Hookshot VR: crawl spaces in first person (VR tab "Crawl spaces", option 990, and "Crawl eye height", option 991).

   Child Link crawls through narrow tunnels on hands and knees: the Kokiri Forest hole to the Kokiri Sword, the ones in the
   Deku Tree, Hyrule Castle, Kakariko, the Bottom of the Well and the Spirit Temple. Each has a CrawlAnchor at both ends and
   an air wall in each mouth (FldObj_AirWall_CrawlHole: a 4 m box scaled 0.2 x 0.16 x 0.2, so 0.8 m wide and 0.64 m tall).
   The game plays it as its own action (Player.action.crawl, ExecutePlayerCrawl; action verb 234) with its own chase camera
   (PlayerCameraParamChase/Crawl). Link's position stays on the tunnel's floor, and the first-person eye stood on it at the
   standing height (child Link 1.25 m): the view looked through the tunnel's roof, at the level outside.

   How the game marks it (headless 18 Nov, Kokiri Forest, RX_VR_CRAWL_TEST and the control bridge's memory reads at the
   mouth, three places in the tunnel and after it): Link's action state (player object +0x1f60) stays 0 all through. Two
   fields of the player object change with it: the word at +0x1f6c gets bit 0x800 from the moment he goes down until he
   has stood up again, and the float at +0x3c90 is 0.91 standing and 0.29 in the crawl, the height his crawl camera sits
   at (it measured 0.28-0.30 m above his position, 0.11 m from it), back to 0.91 as he starts to get up. (His BSA action
   verb, +0x36e8, is 234 meanwhile: the crawl's, as Player.scheduler.crawl sets it.) So he crawls while both say so.

   While he crawls the eye comes down to the crawl eye height above his position, and it goes back up as he stands. Both
   are eased over kVrCrawlEaseS, so going down onto his hands and back up is one short smooth move, never a jump. The
   headset's own movement and look still add to it as standing, so the view moves along the tunnel with Link while you look
   around in it. The renderer is told how far the view came down (rx_vr_crawl), so the first-person body comes down with
   it: it takes the game's own crawl pose under the view (vr/body/arms_and_pose.inc crawl_under_view). Third person keeps
   the game's own crawl camera.
   The crawl eye height (VR tab "Crawl eye height", option 991): Auto is the game's own (+0x3c90, kept between kVrCrawlEyeMin
   and kVrCrawlEyeMax), or a height in centimetres. The tunnels are the same for both ages (the head stays under the
   0.64 m mouth), so it is not scaled by age. */
void rx_vr_crawl(uint32_t seq, float amount, float lowered);   /* (vr/headset/bridges_host.c) */

#define kVrCrawlEaseS .35f        /* seconds to go down onto the hands, or back up */
#define kVrCrawlFlagOffset 0x1f6c /* (player object) the word whose bit kVrCrawlFlag is set all through a crawl */
#define kVrCrawlFlag 0x800u
#define kVrCrawlViewOffset 0x3c90 /* (player object) the game's view height for Link (m): 0.91 standing, 0.29 crawling */
#define kVrCrawlViewLow .6f       /* (m) under this, that view height is a crawl's */
#define kVrCrawlEyeCm 30          /* the crawl eye height when the game's cannot be read (cm above Link's position) */
#define kVrCrawlEyeMin .15f       /* (m) Auto's limits: under the mouth's 0.64 m with the headset's own movement to spare */
#define kVrCrawlEyeMax .50f

/* Link crawls: the player object's crawl bit, and its view height a crawl's (both read by oot_vr_crawl_drop) */
static int oot_vr_crawling(uint32_t flags, float view_height) {
    return (flags & kVrCrawlFlag) && view_height > 0 && view_height < kVrCrawlViewLow;
}

/* How far into the crawl the view is (0 standing, 1 down at the crawl height), one step: towards crawling or standing, at
   most dt / kVrCrawlEaseS. The eye's drop follows oot_vr_crawl_curve of it, so it starts and stops gently. */
static float oot_vr_crawl_step(float amount, int crawling, float dt) {
    const float step = dt / kVrCrawlEaseS;
    return crawling ? fminf(1, amount + step) : fmaxf(0, amount - step);
}
static float oot_vr_crawl_curve(float amount) { return amount * amount * (3 - 2 * amount); }

typedef struct {
    float amount;       /* how far into the crawl (oot_vr_crawl_step) */
    int crawling;       /* the game says Link crawls (and the VR tab's Crawl spaces is on), as last seen */
    int64_t at;         /* the last update (ns) */
    float eye;          /* the crawl eye height (m) as last used: kept while getting up, when the game's is a standing one */
} OotVrCrawl;
static OotVrCrawl oot_vr_crawl;

/* The first-person eye's drop (metres below the standing eye) this view update, and how far into the crawl Link is (0..1).
   standing_m: the standing eye's height above Link's position. Off (option 990), or no crawl seen: the drop eases back to 0,
   so standing up, the option turned off or a map change mid-crawl all give back the normal height. */
static float oot_vr_crawl_drop(Ctx *c, float standing_m, float *amount_out) {
    OotVrCrawl *s = &oot_vr_crawl;
    const int64_t now = hos_now_ns();
    const float dt = s->at && now > s->at ? fminf(.1f, (now - s->at) * 1e-9f) : 0;
    s->at = now;
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    uint32_t flags = 0;
    float view_height = 0;
    if (object && hos_guest_range(object + kVrCrawlViewOffset, 4)) {
        flags = recomp_load32(c, object + kVrCrawlFlagOffset);
        const uint32_t bits = recomp_load32(c, object + kVrCrawlViewOffset);
        memcpy(&view_height, &bits, sizeof view_height);
        if (!isfinite(view_height)) view_height = 0;
    }
    const int crawling = rx_option(990, 0) && oot_vr_crawling(flags, view_height);
    if (crawling) {
        const int setting_cm = rx_option(991, 0);
        s->eye = setting_cm > 0 ? setting_cm * .01f : fminf(kVrCrawlEyeMax, fmaxf(kVrCrawlEyeMin, view_height));
    } else if (s->eye <= 0) s->eye = kVrCrawlEyeCm * .01f;
    if (crawling != s->crawling) {
        hos_log("vr: crawl %s (flags %#x, the game's view height %.2f m): the view %s", crawling ? "begins" : "ends", flags,
                view_height, crawling ? "comes down into the tunnel" : "goes back to the standing height");
        s->crawling = crawling;
    }
    s->amount = oot_vr_crawl_step(s->amount, crawling, dt);
    *amount_out = s->amount;
    return fmaxf(0, standing_m - s->eye) * oot_vr_crawl_curve(s->amount);
}

/* ---- RX_VR_CRAWL_TEST=1 (research, headless): where Link and the game camera are, for the control bridge -----------------
   Logs the player object, Link's actor and the gameplay view when they change, and twice a second (and at each change) Link's
   action state, the two crawl fields, whether oot_vr_crawling calls it a crawl, his place and the game camera's height, in
   either person and without a headset: a headless run steered through the control bridge (docs/control.md) shows the crawl
   found where it is. With RX_VR_PLACE (host/hos_main.c) to start beside a tunnel. */
static void oot_vr_crawl_research(Ctx *c, uint64_t view) {
    static int on = -1;
    if (on < 0) on = vr_research_on("RX_VR_CRAWL_TEST");
    if (!on) return;
    static uint64_t last_object, last_actor, last_view;
    static uint32_t last_state = 0xffffffffu;
    static int64_t told_at;
    static unsigned told;
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    const uint64_t actor = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    if (object != last_object || actor != last_actor || view != last_view)
        hos_log("vr: crawl test: player object %#llx (action state at +0x1f60), actor %#llx (position at +796), gameplay view "
                "%#llx (eye at +0x38)", (unsigned long long)object, (unsigned long long)actor, (unsigned long long)view);
    last_object = object, last_actor = actor, last_view = view;
    if (!object || !actor || !hos_guest_range(actor, 808) || !hos_guest_range(object, kVrCrawlViewOffset + 4)) return;
    const uint32_t state = recomp_load32(c, object + 0x1f60), flags = recomp_load32(c, object + kVrCrawlFlagOffset);
    const float view_height = oot_load_float(c, object + kVrCrawlViewOffset);
    const int crawling = oot_vr_crawling(flags, view_height);
    static int last_crawling = -1;
    const int64_t now = hos_now_ns();
    if (told < 4000 && (state != last_state || crawling != last_crawling || now - told_at > 500000000LL)) {
        told++, told_at = now;
        const float x = oot_load_float(c, actor + 796), y = oot_load_float(c, actor + 800), z = oot_load_float(c, actor + 804);
        const float cx = oot_load_float(c, view + 0x38), cy = oot_load_float(c, view + 0x3c), cz = oot_load_float(c, view + 0x40);
        hos_log("vr: crawl test: action state %#x, crawl flags %#x, view height %.2f: %s%s, Link at %.2f %.2f %.2f, the game "
                "camera %.2f m above him and %.2f m away", state, flags, view_height, crawling ? "CRAWLING" : "standing",
                crawling != last_crawling ? " (changed)" : "", x, y, z, cy - y, hypotf(cx - x, cz - z));
    }
    last_state = state, last_crawling = crawling;
}
