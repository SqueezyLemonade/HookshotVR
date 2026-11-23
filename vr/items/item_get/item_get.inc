// Hookshot VR: an item get's item in front of the headset, first person (VR tab: Item get).
//
// A piece of vr/body/vk_vr_body.cpp, included there with the held items (it uses the rig cache and canonical_rig) and
// before vr/items/gear/equipment_skin.inc, which hands it the draws no other item takes. Not compiled on its own.
//
// In an item get the game binds the item's model to Link above his head and turns him to show it to its camera. In first
// person his head is the headset's: the item was over the player's head (player, 18 Nov). Most item gets play under an
// event of their own (a chest's KokiriForest_TreasureBox_NaviChatOff, an NPC's talk calling Dm_SY_GetInEvent: romfs Banc
// and EventFlow, 21 Nov), so the item is known by where it is, not by the event's name: while an event other than a story
// cinematic plays (vr/items/item_get/item_get_host.c says so, through item_get_live), a small model (at most kItemGetMatrices palette matrices: not Navi's 13, not Link's) whose origin is within kItemGetReach
// of the spot kItemGetAbove over Link's game head joint is the item: it is drawn kItemGetAhead in front of the headset,
// kItemGetBelow under the eyes, at a spot placed once per item get and kept still in the world, and turned about the
// upright so the side it showed Link's front (the game's camera) faces the player; the game's own spin of it stays. A
// shop's item (seen on its shelf first) is drawn where the game has it. The game side is told it was (item_get_drawn): the view's heading then keeps
// out of Link's turn whatever the event is called. Each model is logged once ("vr: item get: ..."), as are models near the
// spot that were not taken, so a headset log says what an item get draws.
// Not moved yet: the item's sparkle (its effect, EventEffectStartGetItem) stays where the game has the item.
constexpr float kItemGetAhead = .5f;       // (m in front of the eyes: past the hands held at rest)
constexpr float kItemGetBelow = .08f;      // (m under the eyes: in view, the eyes a little down)
constexpr float kItemGetAbove = .25f;      // (m: the game's item spot above Link's game head joint)
constexpr float kItemGetReach = .55f;      // (m: a model's origin this close to that spot is the item)
constexpr float kItemGetGearOver = .3f;    // (m: one of Link's gear models is the item only this far over his head joint)
constexpr float kItemGetFar = .8f;        // (m: a model first seen this far from the eyes, and
constexpr float kItemGetShelf = .2f;       //  this far over his head joint, is a shop's on its shelf: it grows no bigger)
constexpr float kItemGetFarMost = 3.0f;    // (m: no farther, and no more than kItemGetShelfMost over his head joint: world
constexpr float kItemGetShelfMost = 1.0f;  //  models tens of metres away were limited too, headset 22 Nov)
static uint64_t item_get_glide_frames = 20;   // (frames, about 0.2 s: a model taken glides from where the game had it to
                                              // the spot; counted in the game's frames, so every pass of a frame, depth
                                              // and colour, both eyes, draws it at the same place)
constexpr float kItemGetOther = .3f;       // (m: a draw of the same model this far from the one taken is another copy)
constexpr int64_t kItemGetGoneMs = 500;    // (ms: a model not drawn this long is a new one, a second item bought)
constexpr int64_t kItemGetQuietMs = 1000;  // (ms: no item drawn this long, the spot is placed afresh for the next)
constexpr float kItemGetMoved = 1.0f;     // (m: the eyes moved this far in an item get, a warp: its spot placed again)
constexpr float kItemGetGearAhead = -.05f;  // (m: a gear model near the eyes is the item unless behind his head, as his
                                           //  sheath is, about 0.18 m; a shop's shield flown to the eyes is level with it)
constexpr unsigned kItemGetMatrices = 8;   // (a model with more palette matrices is never the item: Navi has 13)

static int64_t item_get_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Logs a model once (bounded): what an item get draws near Link's head, taken or not
static void item_get_note(uint64_t vb0, uint64_t bytes, float from_spot, float over_head, bool taken) {
    static std::mutex mu;
    static std::vector<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() >= 64 || std::find(seen.begin(), seen.end(), vb0 * 2 + taken) != seen.end()) return;
    seen.push_back(vb0 * 2 + taken);
    logf("vr: item get: a model (vb0 %llu, %llu matrices) %.2f m from the item spot over Link's head, %.2f m over his head "
         "joint, %s", (unsigned long long)vb0, (unsigned long long)(bytes / 48), from_spot, over_head,
         taken ? "drawn in front of you" : "left as it is");
}

// The spot in front of the eyes for this item get (target) and the level way it faces (f): set the first time it is
// asked in an item get, kItemGetAhead along where the view faced then and kItemGetBelow under the eyes, and kept still
// in the world for the rest of it (headset 21 Nov: it followed the head as it turned; "make it world-locked"). Set again
// if the eyes are kItemGetMoved from where they were (a warp), and for each item after kItemGetQuietMs with none (a
// second item bought in the same shop talk).
static void item_get_anchor(V3 eye, V3 view_front, V3* target, V3* f) {
    static std::mutex mu;
    static unsigned number;
    static bool set;
    static int64_t used;
    static V3 at_eye, at, way;
    const int64_t now = item_get_ms();
    std::lock_guard lk(mu);
    if (number != item_get_number() || now - used > kItemGetQuietMs) number = item_get_number(), set = false;
    used = now;
    if (!set || length(sub(eye, at_eye)) > kItemGetMoved) {
        way = unit(flat(view_front), {0, 0, 1});
        at = add(add(eye, mul(way, kItemGetAhead)), {0, -kItemGetBelow, 0}), at_eye = eye, set = true;
    }
    *target = at, *f = way;
}

// The size an item-get model may be drawn at (k, at most 1, times its size now): never bigger than it was when first
// seen up on a shelf (kItemGetFar or more from the eyes and kItemGetShelf over Link's head joint) in this item get. A
// shop's item is flown from its shelf to the camera and grows on the way (it took the whole view, headset 21 Nov); a
// chest's or a pickup's item is first seen low, in the chest or on the ground, and never limited (it may grow in from
// nothing: limited at 1 m, items from chests and a heart stayed over the head, headset 22 Nov).
// Also its place on the shelf (*shelf, when shelf is given and it was seen there): a shop's item, which item_get_skin
// draws where the game has it. gear: one of Link's gear models (the shop's Deku Shield): its place is kept, its size never
// limited.
static float item_get_size(uint64_t vb0, float size, float from_eye, float over, V3 at, bool gear, V3* shelf = nullptr) {
    struct Far { uint64_t vb0; float size; V3 at; };
    static std::mutex mu;
    static unsigned number;
    static std::vector<Far> far;
    std::lock_guard lk(mu);
    if (number != item_get_number()) number = item_get_number(), far.clear();
    for (const Far& f : far)
        if (f.vb0 == vb0) {
            if (shelf) *shelf = f.at;
            if (gear) return 1.0f;
            if (size > f.size * 1.05f) {
                static std::atomic<int> told{0};
                if (told.fetch_add(1, std::memory_order_relaxed) < 20)
                    logf("vr: item get %u: vb0 %llu limited to its shelf size (%.2f of %.2f)", number, (unsigned long long)vb0,
                         f.size, size);
            }
            return size > f.size * 1.05f ? f.size / size : 1.0f;
        }
    if (shelf) *shelf = {NAN, NAN, NAN};
    if (from_eye >= kItemGetFar && from_eye <= kItemGetFarMost && over >= kItemGetShelf && over <= kItemGetShelfMost &&
        far.size() < 64) {
        far.push_back({vb0, size, at});
        if (shelf) *shelf = at;
        logf("vr: item get %u: vb0 %llu seen on a shelf: %.2f m from the eyes, %.2f over his head joint, size %.2f", number,
             (unsigned long long)vb0, from_eye, over, size);
    }
    return 1.0f;
}

// The place an item-get model is drawn at, from where the game has it (world): in front of the eyes, its front to the
// player. game_head: Link's game head joint; link_front: the way his game body faces; eye, view_front: the headset's
// eyes and the way the view faces (level). False when it is not the item (too far from the spot over his head; for one
// of Link's gear models, also not kItemGetGearOver over his head joint: his own is lower. Headless 21 Nov, the Kokiri
// Sword's chest: the sword's sheath, on his back once he has it, came within 0.45 m of the spot, about at his head joint;
// headset 22 Nov, every chest: it rose 0.11 m over it as Link bent to open the chest, and was taken at 0.1 m).
// Also the item: a small model (not gear) nearer the eyes than the spot in front of them (a shop's item, which the game
// flies off the shelf to the camera: headset 21 Nov, "it comes too close and sometimes takes the whole view"); it is
// stopped there. The spot is world-locked (item_get_anchor): placed once, where the head faced as the get began.
// yaw: the turn about the upright in placed (for item_get_skin's glide in).
static bool item_get_place(const Aff& T, V3 game_head, V3 link_front, V3 eye, V3 view_front, bool gear, Aff* placed,
                           float* from_spot, float* yaw) {
    const V3 spot = add(game_head, {0, kItemGetAbove, 0});
    *from_spot = length(sub(T.t, spot));
    const float over = T.t.y - game_head.y;
    const V3 lf = unit(flat(link_front), {0, 0, 1});
    // (a gear model near the eyes only over his head joint and not behind it: his own sheath, on his back, is within
    // reach of the eyes and about at his head joint: 0.04 m over it, headless 21 Nov; a shop's Deku Shield flown to the
    // camera, the eyes, is about level with his head joint and was refused at 0.1 m in front: drawn full size at the
    // face, headset 22 Nov)
    const bool near_eye = length(sub(T.t, eye)) < kItemGetAhead &&
                          (!gear || (over > 0 && dot(flat(sub(T.t, game_head)), lf) > kItemGetGearAhead));
    if (!near_eye && (!(*from_spot < kItemGetReach) || (gear && !(over > kItemGetGearOver)))) return false;
    V3 f, target;
    item_get_anchor(eye, view_front, &target, &f);
    // (its front showed Link's front: turned so that side looks back at the eyes, -f)
    *yaw = std::remainder(std::atan2(-f.x, -f.z) - std::atan2(lf.x, lf.z), 6.283185307f);
    placed->r = mul(axis_angle({0, 1, 0}, *yaw), T.r), placed->t = target;
    return true;
}

// What became of a model in this item get: 0 nothing yet, 1 taken (drawn at the spot), 2 let go. One taken and then
// refused under Link's head joint and near him is let go for the rest of it: the game took it down into his hand
// (headset 21 Nov, the Kokiri Sword admired in his hand after the get snapped between the spot and his hand: 0.70 m,
// then -0.14 m over his head joint). Refused anywhere else, it is the game's to draw meanwhile and taken again when it
// comes back: a shop's item put back on its shelf when the purchase is cancelled, and flown to the camera again at the
// next confirm (headset 21 Nov: let go then, the game flew the Deku Shield to its camera, the headset, round the head as
// it turned). A model not drawn for kItemGetGoneMs starts afresh. A refused draw kItemGetOther or more from where the
// model was last taken is another copy of it and changes nothing (headset 22 Nov: a rupee or heart picked up with another
// rupee in view was refused every frame through the other, its glide began again each frame and it stayed over the head,
// drawn white). what: 0 ask, 1 just taken, 2 just refused under his head joint and near him, 3 just refused otherwise;
// at: where the game has it; frame: the game's frame. Taken, *ease (0 to 1) says how far it is into its glide from *from
// (where the game had it when taken this time) to the spot.
static int item_get_track(uint64_t vb0, int what, V3 at = {}, uint64_t frame = 0, float* ease = nullptr, V3* from = nullptr,
                          bool* was_on = nullptr) {
    struct Seen { uint64_t vb0; int state; int64_t at; bool on; uint64_t since, taken; V3 from, last; };
    static std::mutex mu;
    static unsigned number;
    static std::vector<Seen> seen;
    const int64_t now = item_get_ms();
    std::lock_guard lk(mu);
    if (number != item_get_number()) number = item_get_number(), seen.clear();
    Seen* m = nullptr;
    for (Seen& e : seen) if (e.vb0 == vb0) m = &e;
    if (m && now - m->at > kItemGetGoneMs) *m = {vb0, 0, now, false, 0, {}, {}};
    // (only a model taken has an entry: every small model drawn in an event is asked about, more than 32 a frame in the
    // forest, and with entries for them all the taken one was pushed out each frame and taken afresh, its glide begun
    // again: headless 22 Nov, "vb0 1440 taken" every frame, the rupee over the head)
    if (!m && what != 1) return 0;
    if (!m) {
        if (seen.size() < 32) seen.push_back({vb0, 0, now, false, 0, {}, {}}), m = &seen.back();
        else {   // (the stalest makes room)
            m = &seen[0];
            for (Seen& e : seen) if (e.at < m->at) m = &e;
            *m = {vb0, 0, now, false, 0, {}, {}};
        }
    }
    if (what == 2 || what == 3) {   // (another copy, or this one where it is now: followed, down into his hand)
        // (another copy: drawn far from it while it is taken, in this frame or the one before. Not so after that: the
        // game moved it, the Kokiri Sword from over his head into his hand in one frame, headset 22 Nov)
        if (m->state == 1 && frame <= m->taken + 1 && length(sub(at, m->last)) >= kItemGetOther) return m->state;
        m->last = at;
    }
    if (was_on) *was_on = m->on;
    m->at = now;
    if (what == 1) {
        if (m->state == 0) m->state = 1;
        m->last = at, m->taken = frame;
        if (!m->on) {
            m->on = true, m->since = frame, m->from = at;
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 300)
                logf("vr: item get %u: vb0 %llu taken at %.2f %.2f %.2f", number, (unsigned long long)vb0, at.x, at.y, at.z);
        }
        if (ease)
            *ease = item_get_glide_frames ? std::min(1.0f, (float)(frame - m->since) / (float)item_get_glide_frames) : 1.0f;
        if (from) *from = m->from;
    }
    if (what == 3) m->on = false;
    if (what == 2 && m->state == 1) {
        m->state = 2, m->on = false;
        logf("vr: item get: the model vb0 %llu let go and hidden for the rest of this item get (the game took it down into his hand)", (unsigned long long)vb0);
    }
    return m->state;
}

// Logs a model caught or let go of (bounded): where the game had it, so a headset log says which rule did it (the shop's
// first look, headset 22 Nov: the item came to the face, the second look not)
static void item_get_moment(uint64_t vb0, const char* what, V3 at, V3 head, V3 link_front, V3 eye, float from_spot, bool gear) {
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) >= 400) return;
    const V3 lf = unit(flat(link_front), {0, 0, 1});
    logf("vr: item get %u: %s vb0 %llu at %.2f %.2f %.2f: %.2f m from the spot over his head (reach %.2f), %.2f m from the "
         "eyes (near %.2f), %.2f over his head joint, %.2f in front of it, gear %d", item_get_number(), what,
         (unsigned long long)vb0, at.x, at.y, at.z, from_spot, kItemGetReach, length(sub(at, eye)), kItemGetAhead,
         at.y - head.y, dot(flat(sub(at, head)), lf), (int)gear);
}

// Logs once each reason a small model's draw in an item get was not looked at (bounded): a headset log says which check
// kept the item over Link's head
static void item_get_refused(const char* why) {
    static std::mutex mu;
    static std::vector<const char*> told;
    std::lock_guard lk(mu);
    if (told.size() >= 16 || std::find(told.begin(), told.end(), why) != told.end()) return;
    told.push_back(why);
    logf("vr: item get: a model's draw not looked at: %s", why);
}

// gear: the model is one of Link's gear kinds (the Kokiri Sword, the slingshot, the boomerang: a chest's item is the same
// model; vr/items/gear/equipment_skin.inc asks here first during an item get). Headless 21 Nov, the Kokiri Sword's chest:
// without it the sword stayed over Link's head.
std::shared_ptr<const std::vector<float>> item_get_skin(uint64_t bones, uint64_t bytes, uint64_t object, uint64_t object_size,
                                                        uint64_t vb0, uint64_t frame, bool gear = false) {
    if (!item_get_live() || !bones || !bytes || bytes % 48 || bytes > kItemGetMatrices * 48ull) return nullptr;
    if (!object || object_size < 0x4c) return item_get_refused("no object buffer"), nullptr;
    LinkBody body;
    if (!frame_body(frame, body)) return item_get_refused("no VR body for the frame"), nullptr;
    float o[3];
    std::vector<float> raw(bytes / 4);
    if (!ub_read(object + 0x40, o, sizeof o) || !ub_read(bones, raw.data(), bytes)) return item_get_refused("its buffers unread"), nullptr;
    for (float f : raw) if (!std::isfinite(f)) return nullptr;
    if (!std::isfinite(o[0] + o[1] + o[2])) return nullptr;
    const V3 back = v3(body.head_back);
    if (!(length(flat(back)) > .1f)) return item_get_refused("the headset's way not known"), nullptr;
    V3 head, front;
    {
        std::lock_guard lk(cache_mu);
        const Entry* vr = canonical_rig(frame, body);
        if (!vr) return item_get_refused("no rig of Link's for the frame"), nullptr;
        const rig::Model& M = vr->adult ? rig::adult : rig::child;
        if (vr->raw.size() < (size_t)M.slots * 12) return nullptr;
        head = add(vr->origin, point(vr->raw.data() + M.neck * 12, v3(M.neck_pos)));
        front = apply(rotation(vr->raw.data() + M.waist * 12), {0, 0, 1});   // (his model faces +z)
    }
    const Aff T = shifted(aff(raw.data()), v3(o));   // (the model's place: its first matrix)
    // (the model's matrices moved so the first is at `to`: the others keep their place relative to it)
    auto redraw = [&](const Aff& to) -> std::shared_ptr<const std::vector<float>> {
        Aff inv;
        if (!invert(T, &inv)) return nullptr;
        const Aff delta = mul(to, inv);
        auto out = std::make_shared<std::vector<float>>(raw.size());
        for (size_t i = 0; i < raw.size() / 12; i++)
            store(out->data() + i * 12, shifted(mul(delta, shifted(aff(raw.data() + i * 12), v3(o))), mul(v3(o), -1.0f)));
        for (float f : *out) if (!std::isfinite(f)) return nullptr;
        return out;
    };
    V3 shelf;
    const float k = item_get_size(vb0, length(column(T.r, 0)), length(sub(T.t, v3(body.eye))), T.t.y - head.y, T.t, gear, &shelf);
    const M3 shrink = {{{k, 0, 0}, {0, k, 0}, {0, 0, k}}};
    const Aff as_is = {mul(T.r, shrink), T.t};
    auto left = [&]() { return k < 1 ? redraw(as_is) : nullptr; };
    Aff placed;
    float from_spot = 0;
    float yaw = 0;
    // (let go: the game took it down into his hand. Hidden for the rest of the item get: the game's own animation after
    // the Kokiri Sword's get swings it round in front of the face, "the sword cutscene still fights me", headset 22 Nov)
    if (item_get_track(vb0, 0) == 2) return redraw({M3{}, T.t});
    // (a shop's item, seen on its shelf in this item get: drawn where the game has it, its own close-up too. Moved, it
    // came too close at the spot, or was hidden; and the Deku Shield, Link's own shield model, was taken by the gear code
    // onto the VR body's shield place when left alone ("flew off to the right", headset 22 Nov): given the game's own
    // pose here, so nothing moves it)
    if (std::isfinite(shelf.x)) return gear ? redraw(T) : nullptr;
    if (!item_get_place(T, head, front, v3(body.eye), mul(back, -1.0f), gear, &placed, &from_spot, &yaw)) {
        if (from_spot < 1.2f) item_get_note(vb0, bytes, from_spot, T.t.y - head.y, false);
        const bool down = T.t.y < head.y && from_spot < kItemGetReach;   // (in his hand)
        bool was_on = false;
        item_get_track(vb0, down ? 2 : 3, T.t, frame, nullptr, nullptr, &was_on);
        if (was_on) item_get_moment(vb0, "let go of", T.t, head, front, v3(body.eye), from_spot, gear);
        return left();
    }
    float ease = 1;
    V3 from = T.t;
    bool was_on = true;
    item_get_track(vb0, 1, T.t, frame, &ease, &from, &was_on);
    if (!was_on) item_get_moment(vb0, "caught", T.t, head, front, v3(body.eye), from_spot, gear);
    // (with Item shown: Over your head the item stays where the game has it, but the game side is still told it was seen:
    // the view's heading keeps out of Link's turn either way)
    if (!opts.vr_item_get) return item_get_drawn(), left();
    if (ease < 1) {   // (gliding in: no jump from the game's flight, or from over his head, to the spot)
        const float e = ease * ease * (3 - 2 * ease);
        placed.t = add(from, mul(sub(placed.t, from), e));
        placed.r = mul(axis_angle({0, 1, 0}, yaw * e), T.r);
    }
    placed.r = mul(placed.r, shrink);
    auto out = redraw(placed);
    if (!out) return nullptr;
    item_get_note(vb0, bytes, from_spot, T.t.y - head.y, true);
    item_get_drawn();
    return out;
}
