// Hookshot VR: a bottle in the VR hand that holds it (first person, VR tab: Bottles in the hands).
//
// A piece of vr/body/vk_vr_body.cpp, included there before vr/items/gear/held_gear.inc, whose anchored_skin hands it the
// bottle's draws: it uses the pieces before it (the rig cache, native_hands; bottle_pose.h, included at the top of
// vk_vr_body.cpp). Not compiled on its own.
// The held bottle is the game's own actor (Weapon_Bottle or Weapon_Bottle_<content>, vr/items/bottle/bottle_host.c), so its
// model already shows what is inside: a bug, a fish, a fairy, a Poe, a potion's colour, milk, the letter (vk_vr_bottle.h,
// made by vr/tools/generate/bottle.py from the player's own files). The game draws it in Link's game hand; every draw of it
// is moved by one change, from where the game draws the model to where the VR hand holds it (vr_bottle_pose in
// vr/items/bottle/bottle_pose.h, the same place the game side puts the actor), so all its parts move together.
// Which draws: the same models are the bottles in the world (a get item, a shop shelf). Only a bottle whose model sits in
// one of Link's own game hands (native_hands: his real uploads of the last two frames) is his held one; any other is
// drawn as the game has it. In first person a held bottle is never drawn in the game's pose: with no VR hand holding it
// (vr::bottle_hand -1, or -2 let go while the game puts it away) it is hidden.
constexpr float kBottleInGameHand = .45f;   // (m: the model's origin this close to a game wrist is in that hand)
const rig::BottleShape* bottle_shape(uint64_t vb0, uint64_t bytes) {
    for (const rig::BottleShape& s : rig::bottle_shapes)
        if (s.vb0 == vb0 && bytes >= s.matrices * 48ull) return &s;
    return nullptr;
}
bool bottle_model(uint64_t vb0, uint64_t bytes) { return bottle_shape(vb0, bytes) != nullptr; }
// A bottle's shape (by its vertex buffer alone): skin_draw.inc tries its palette in vertex uniform buffer 4 first, as for the
// ocarina's and the hookshot chain's colour passes (the glass's liquid did not move with the held bottle, headset 19 Nov);
// held_bottle takes a palette only when it puts the model in one of Link's game hands, so a wrong buffer is passed over
bool held_bottle_draw(uint64_t vb0) {
    for (const rig::BottleShape& s : rig::bottle_shapes)
        if (s.vb0 == vb0) return true;
    return false;
}
// (bounded) each shape's first outcome with each palette size: which draws moved with the hand, which were passed over
void bottle_tell(uint64_t vb0, uint64_t bytes, float nearest, const char* what) {
    static std::mutex mu;
    static std::vector<std::pair<uint64_t, uint64_t>> seen;
    std::lock_guard lk(mu);
    if (seen.size() >= 96 || std::find(seen.begin(), seen.end(), std::make_pair(vb0, bytes)) != seen.end()) return;
    seen.emplace_back(vb0, bytes);
    logf("vr: bottle: shape vb0 %llu, palette %llu bytes: %.2f m from a game wrist: %s", (unsigned long long)vb0,
         (unsigned long long)bytes, nearest, what);
}
// How far the model's origin is from the nearest of Link's own game wrists (native_hands: his real uploads of the last
// two frames), and which (0 left, 1 right; cache_mu held)
float bottle_from_game_hands(V3 at, uint64_t frame, const LinkBody& body, int* hand) {
    float nearest = 1e9f;
    *hand = -1;
    for (const NativeHands& h : native_hands) {
        if (h.frame == ~0ull || h.frame > frame || frame - h.frame > 2 || h.adult == body.child ||
            !(length(sub(h.root, v3(body.root))) < 1.0f)) continue;
        for (int k = 0; k < 2; k++) {
            const float d = length(sub(h.wrist[k], at));
            if (d < nearest) nearest = d, *hand = k;
        }
    }
    return nearest;
}
// The VR hand's grip axes, calibrated as hookshot_vr_hands_world gives them to the game side (vr/headset/host_api.inc):
// x (the palm's normal) and z (the fist runs along -z)
void bottle_grip_axes(const LinkBody& body, int side, float gx[3], float gz[3]) {
    const LinkHand& in = body.hand[side];
    const float d2r = 3.14159265f / 180.0f;
    const float cx = std::cos(body.wrist_deg[side][0] * d2r), sx = std::sin(body.wrist_deg[side][0] * d2r);
    const float cy = std::cos(body.wrist_deg[side][1] * d2r), sy = std::sin(body.wrist_deg[side][1] * d2r);
    const float cz = std::cos(body.wrist_deg[side][2] * d2r), sz = std::sin(body.wrist_deg[side][2] * d2r);
    const float E[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                           {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                           {-sy, cy * sx, cy * cx}};
    for (int k = 0; k < 3; k++) {
        gx[k] = in.basis[k] * E[0][0] + in.basis[3 + k] * E[1][0] + in.basis[6 + k] * E[2][0];
        gz[k] = in.basis[k] * E[0][2] + in.basis[3 + k] * E[1][2] + in.basis[6 + k] * E[2][2];
    }
}
// Where the VR hand holds the bottle's model (world)
Aff bottle_vr_model(const LinkBody& body, int side) {
    float gx[3], gz[3], origin[3], rot[9];
    bottle_grip_axes(body, side, gx, gz);
    vr_bottle_pose(side, body.hand[side].position, gx, gz, origin, rot);
    return {{{{rot[0], rot[1], rot[2]}, {rot[3], rot[4], rot[5]}, {rot[6], rot[7], rot[8]}}}, {origin[0], origin[1], origin[2]}};
}
// The cork, once the game side says the bottle is open (vr::bottle_cork 0: vr/items/bottle/bottle_host.c): its own draw
// (the model's Cap shape, rig::BottleShape::cork) is drawn popping out of the neck, along the glass and up, falling under
// gravity to Link's feet, and gone kCorkGoneS after the pop. The game's own cork stays in its model: the bottle's CapOff does
// not hold while Link holds it out (headless 18 Nov), so only its drawing changes.
constexpr float kCorkPopSpeed = 2.2f;   // (m/s out of the neck, along the glass)
constexpr float kCorkPopUp = 1.2f;      // (m/s upward as well)
constexpr float kCorkGravity = 9.8f;
constexpr float kCorkRest = .07f;       // (m: the model's origin this far below the floor when the cork lies on it)
constexpr float kCorkGoneS = 2.5f;
struct CorkFlight { int was = 1; bool flying = false; Aff start{}; V3 v{}; float floor = 0; std::chrono::steady_clock::time_point at{}; };
std::mutex cork_mu;
CorkFlight cork_flight;   // (cork_mu)
// Where the cork's model is drawn now: the held bottle's place while it is corked, else its flight; false: not drawn
bool cork_place(const Aff& held, const LinkBody& body, Aff* out) {
    const int corked = bottle_cork();   // (-1, not told lately: as corked)
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk(cork_mu);
    CorkFlight& f = cork_flight;
    if (corked != 0) { f.was = 1, f.flying = false, *out = held; return true; }
    if (f.was) {   // (the pop: from where the bottle is now)
        const V3 along = column(held.r, 2);
        f.start = held, f.v = add(mul(along, kCorkPopSpeed), V3{0, kCorkPopUp, 0}), f.floor = body.root[1], f.at = now, f.flying = true;
        logf("vr: bottle: the cork pops out");
    }
    f.was = 0;
    const float t = std::chrono::duration<float>(now - f.at).count();
    if (!f.flying || t > kCorkGoneS) return false;
    V3 p = add(f.start.t, add(mul(f.v, t), V3{0, -.5f * kCorkGravity * t * t, 0}));
    if (p.y < f.floor - kCorkRest) p.y = f.floor - kCorkRest;   // (lying on the floor)
    *out = {f.start.r, p};
    return true;
}
std::shared_ptr<const std::vector<float>> held_bottle(uint64_t vb0, const std::vector<float>& raw, V3 o, uint64_t frame,
                                                      const LinkBody& body) {
    const rig::BottleShape* shape = bottle_shape(vb0, raw.size() * 4ull);
    if (!shape) return nullptr;
    Aff root_inv{};
    invert(aff(rig::bottle_root), &root_inv);
    const Aff game = mul(shifted(aff(raw.data() + shape->root_slot * 12), o), root_inv);
    float nearest = 0;
    int game_hand = -1;
    {
        std::lock_guard lk(cache_mu);
        nearest = bottle_from_game_hands(game.t, frame, body, &game_hand);
    }
    if (!(nearest < kBottleInGameHand)) {   // (not in Link's hands: a bottle in the world, or this palette is not its own)
        bottle_tell(vb0, raw.size() * 4ull, nearest, "not in a game hand: left as the game draws it");
        return nullptr;
    }
    const int held = bottle_hand();
    bottle_tell(vb0, raw.size() * 4ull, nearest, held >= 0 ? "moved to the VR hand" : "hidden (no VR hand holds it)");
    if (held < 0 || !body.hand[held].valid) {   // (no VR hand holds it: never drawn in the game's pose; every vertex at one point)
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 4) logf("vr: bottle: in Link's game hand with no VR hand holding it (%d): hidden", held);
        return std::make_shared<std::vector<float>>(raw.size(), 0.0f);
    }
    Aff inv;
    if (!invert(game, &inv)) return nullptr;
    Aff place = bottle_vr_model(body, held);
    if (shape->cork && !cork_place(place, body, &place)) return std::make_shared<std::vector<float>>(raw.size(), 0.0f);   // (gone)
    const Aff delta = mul(place, inv);
    auto out = std::make_shared<std::vector<float>>(raw);
    for (size_t i = 0; i < shape->matrices; i++)
        store(out->data() + i * 12, shifted(mul(delta, shifted(aff(raw.data() + i * 12), o)), mul(o, -1.0f)));
    for (size_t i = 0; i < shape->matrices * 12ull; i++) if (!std::isfinite((*out)[i])) return nullptr;
    static std::atomic<int> told_side{-1};
    if (told_side.exchange(held) != held)
        logf("vr: bottle held in the %s VR hand (model %u; the game holds it %.2f m from Link's %s wrist)",
             held ? "right" : "left", shape->model, nearest, game_hand == 1 ? "right" : "left");
    return out;
}
