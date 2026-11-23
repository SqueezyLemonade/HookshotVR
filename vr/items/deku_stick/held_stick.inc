// Hookshot VR: the Deku Stick in the VR hand that holds it (first person, VR tab: Deku Sticks in the hands).
//
// A piece of vr/items/gear/held_gear.inc (so of vr/body/vk_vr_body.cpp), included there before anchored_skin, which hands
// it the stick's draws: it uses the pieces before it (the rig cache, native_hands, canonical_rig). Not compiled on its own.
// The held model is Item_DekuStick (Weapon_DekuStick's): 3 smooth matrices for its leaf's cloth and one rigid matrix, the
// stick (rig::deku_stick_*, made by vr/tools/generate/hand_sockets.py from the player's own files). It is held as the game
// holds a sword: the VR wrist x the game's Weapon_L mount x (its grip back to its origin) x the stick bone, in the left
// hand; in the right on the mirrored mount, as the swords are. The leaf's matrices move with the stick, so its cloth keeps the game's own motion.
// Which draws: the same model is the stick lying in the world (Item_DekuStick, a pickup). Only a stick whose origin is in
// one of Link's own game hands (native_hands: his real uploads of the last two frames) is his held one; any other is
// drawn as the game has it.
// Where it goes: the VR hand that holds it (vr::stick_hand, from vr/input/controllers_host.c vr_stick); hidden while it is
// let go and the game still puts it away (-2); and when no VR hand holds it (the game's own buttons, or the VR tab's row
// off) in the VR hand the game holds it in: first person never draws it in the game's pose.
// Its flame: an effect the game emits on its own stick's head (Effect_0, rig::deku_stick_head), in Link's game hand, so it
// burnt at the hips (headset 18 Nov). An effect draw whose effect matrix sits on that head is drawn on the VR stick's head,
// as a held fire arrow's flames are on the VR arrow: its particle camera moved by the change from the game's stick to the
// VR one (held_stick_effect_move; the draw side is skin_draw.inc vr_arrow_effect).
// Where the VR hand holds it: near its butt (rig::deku_stick_grip, vr/tools/generate/hand_sockets.py STICK_GRIP), which
// the sockets already hold; the game side places the stick's actor the same way (deku_stick_host.c kVrStickGripZ).
constexpr float kStickInGameHand = .35f;   // (m: the stick's origin this close to a game wrist is in that hand)
constexpr float kStickFlameReach = .25f;   // (m: an effect this close to the game's stick, grip to head, is its own (its
                                           // flame at the head, its embers along it, headset 18 Nov); a torch's flame
                                           // further from it is left where it is)
// The game's stick and the VR one, as last placed (world), for its flame
std::mutex stick_placed_mu;
Aff stick_placed_game{}, stick_placed_vr{};
std::chrono::steady_clock::time_point stick_placed_at{};
bool held_stick_effects_live() {
    std::lock_guard lk(stick_placed_mu);
    return std::chrono::steady_clock::now() - stick_placed_at < std::chrono::milliseconds(150);
}
bool held_stick_effect_move(const float in[16], const float camera[3], float move[12]) {
    Aff game, vr;
    {
        std::lock_guard lk(stick_placed_mu);
        if (std::chrono::steady_clock::now() - stick_placed_at > std::chrono::milliseconds(150)) return false;
        game = stick_placed_game, vr = stick_placed_vr;
    }
    // (the game's stick as a segment, from where the VR hand holds it to its head; the effect's nearest point on it)
    const V3 grip = add(apply(game.r, v3(rig::deku_stick_grip)), game.t);
    const V3 head = add(apply(game.r, v3(rig::deku_stick_head)), game.t), along = sub(head, grip), fx{in[12], in[13], in[14]};
    const float t = std::clamp(dot(sub(fx, grip), along) / std::max(dot(along, along), 1e-6f), 0.0f, 1.0f);
    const V3 on = add(grip, mul(along, t));
    const float at[3] = {on.x, on.y, on.z};
    if (!arrow_fx::source_matches(in, at, camera)) return false;
    const float near = length(sub(fx, on));
    if (!(near < kStickFlameReach)) {
        static std::atomic<int> told_far{0};   // (bounded: an effect near the stick left where it is, should one be its own)
        if (near < 1.0f && told_far.fetch_add(1) < 8)
            logf("vr: deku stick: an effect %.2f m from the game's stick (%.0f%% up it) left where it is", near, t * 100);
        return false;
    }
    Aff inv;
    if (!invert(game, &inv)) return false;
    store(move, mul(vr, inv));
    static std::atomic<unsigned> told{0};   // (once for each quarter of the stick an effect sits on)
    const unsigned quarter = 1u << std::min(3, int(t * 4));
    if (!(told.fetch_or(quarter) & quarter))
        logf("vr: deku stick: an effect %.2f m from the game's stick (%.0f%% up it) drawn on the VR stick", near, t * 100);
    return true;
}
bool stick_model(uint64_t vb0, uint64_t bytes) {
    if (bytes < rig::deku_stick_matrices * 48ull) return false;   // (some passes bind a larger buffer: only the palette moves)
    for (uint64_t s : rig::deku_stick_shapes)
        if (s == vb0) return true;
    return false;
}
// The game's stick (world): its model's origin, from its rigid matrix
Aff stick_game_model(const std::vector<float>& raw, V3 o) {
    Aff root_inv{};
    invert(aff(rig::deku_stick_root), &root_inv);
    return mul(shifted(aff(raw.data() + rig::deku_stick_rigid_slot * 12), o), root_inv);
}
// The game hand (0 left, 1 right) whose wrist holds the stick at this place, or -1 (cache_mu held)
int stick_game_hand(V3 at, uint64_t frame, const LinkBody& body, float* nearest) {
    int hand = -1;
    *nearest = 1e9f;
    for (const NativeHands& h : native_hands) {
        if (h.frame == ~0ull || h.frame > frame || frame - h.frame > 2 || h.adult == body.child ||
            !(length(sub(h.root, v3(body.root))) < 1.0f)) continue;
        for (int k = 0; k < 2; k++) {
            const float d = length(sub(h.wrist[k], at));
            if (d < *nearest) *nearest = d, hand = k;
        }
    }
    return *nearest < kStickInGameHand ? hand : -1;
}
std::shared_ptr<const std::vector<float>> held_stick(const std::vector<float>& raw, V3 o, uint64_t frame, const LinkBody& body) {
    const Aff game = stick_game_model(raw, o);
    Aff vr_stick{};
    int side = -1, game_hand = -1;
    float nearest = 0;
    {
        std::lock_guard lk(cache_mu);
        game_hand = stick_game_hand(game.t, frame, body, &nearest);
        if (game_hand < 0) {   // (not in Link's hands: a stick in the world, drawn as the game has it)
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 8)
                logf("vr: deku stick: a stick %.2f m from Link's game hands left as the game draws it", nearest);
            return std::make_shared<std::vector<float>>(raw);
        }
        const int held = stick_hand();
        if (held == -2) return std::make_shared<std::vector<float>>(raw.size(), 0.0f);   // (let go: hidden, every vertex at one point)
        side = held >= 0 ? held : game_hand;
        if (!body.hand[side].valid) return nullptr;
        const Entry* rig_now = canonical_rig(frame, body);
        if (!rig_now) return nullptr;
        const rig::Model& M = rig_now->adult ? rig::adult : rig::child;
        const Aff socket = aff(side ? (rig_now->adult ? rig::deku_stick_right_adult : rig::deku_stick_right_child)
                                    : (rig_now->adult ? rig::deku_stick_adult : rig::deku_stick_child));
        // (the stick bone's place in the VR hand, then its model's origin, as stick_game_model has the game's)
        Aff root_inv{};
        invert(aff(rig::deku_stick_root), &root_inv);
        vr_stick = mul(mul(shifted(aff(rig_now->out->data() + M.arm[side].wrist * 12), rig_now->origin), socket), root_inv);
    }
    Aff inv;
    if (!invert(game, &inv)) return nullptr;
    const Aff delta = mul(vr_stick, inv);
    auto out = std::make_shared<std::vector<float>>(raw);
    for (size_t i = 0; i < rig::deku_stick_matrices; i++)
        store(out->data() + i * 12, shifted(mul(delta, shifted(aff(raw.data() + i * 12), o)), mul(o, -1.0f)));
    for (size_t i = 0; i < rig::deku_stick_matrices * 12; i++) if (!std::isfinite((*out)[i])) return nullptr;
    {
        std::lock_guard lk(stick_placed_mu);
        stick_placed_game = game, stick_placed_vr = vr_stick, stick_placed_at = std::chrono::steady_clock::now();
    }
    static std::atomic<int> told_side{-1};
    if (told_side.exchange(side) != side)
        logf("vr: deku stick held in the %s VR hand%s (the game holds it in Link's %s hand, %.2f m from its wrist: the "
             "socket assumes his left, Weapon_L)", side ? "right" : "left", stick_hand() < 0 ? " (the game's own, no VR grip)" : "",
             game_hand ? "right" : "left", nearest);
    return out;
}
