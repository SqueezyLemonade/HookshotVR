// Hookshot VR: the ocarina on the VR belt, first person (VR tab: Ocarina in the hands, option 980).
//
// A piece of vr/body/vk_vr_body.cpp, included there after vr/items/gear/held_gear.inc (it uses the rig cache,
// canonical_rig and aff_change) and before vr/items/gear/equipment_skin.inc, which hands it the ocarina's draws. Not
// compiled on its own.
//
// Link carries his ocarina at the back of his belt once he owns one: a holder (Item_OcarinaHolder_Fairy or _Time, its band
// of belt leather round it) with the ocarina in it (Item_Ocarina_Fairy or _Time). Which ones the game draws is its own
// choice (none, the Fairy Ocarina, the Ocarina of Time); their draws are recognised by their vertex buffer sizes
// (rig::ocarina_shapes, rig::ocarina_holder_shapes: vk_vr_ocarina.h, made by vr/tools/generate/ocarina.py). Drawn where the
// game wears them, they floated off the VR body; before this they were hidden in first person (on worn_items.py's list; now,
// whenever they have no VR pose, link_worn_hidden hides them by their equipment kind).
//
// Where they go: on the VR body's belt, at its front left (kOcarinaBeltAngle), where the player sees it and can reach it.
// - Each one's attachment to Link's waist bone (rig::Model waist: his hips) is taken from the game, in Link's own model
//   space (his bind pose: +y up, +z his front, +x his left): the game's waist palette matrix inverted, times the model's
//   place. It is taken once it has held still for kOcarinaCalmFrames frames on the belt (within kOcarinaOnBelt of the waist),
//   and then kept: whatever the game does with its own ocarina afterwards (takes it to his game hands to play), the VR belt
//   keeps it where it was worn.
// - Then both are turned about the waist's upright axis, from where the game wears the holder (at his back) to the front
//   left: the same turn for the ocarina, so it stays in its holder. Placed on the frame's one VR pose of the waist, every
//   pass of a frame agrees.
// - Until an attachment is taken (the first frames after a load), or with the row off, the draw is left out near Link, as
//   before (link_worn_hidden: never in the game's pose).
// In a VR hand: while the game side says a VR hand holds it (ocarina_hand: vr/items/ocarina/ocarina_host.c), the ocarina is
// drawn in that hand; its holder stays on the belt. How the hand holds it is the game's own grip, learned the first time
// the game has its ocarina in Link's hands (playing it: within kOcarinaInHand of both his game wrists and away from its
// holder) and held there still for kOcarinaCalmFrames frames: the ocarina's place against each game wrist, kept for that age
// and hand (logged once, so it can be made a constant). Until then it sits on the hand's Weapon_L mount (the hookshot's,
// rig::hookshot_left_* / _right_*). Each draw on the belt also tells the game side where it is (ocarina_belt_drawn), for
// the grip that takes it.
// RX_VR_NO_OCARINA=1 turns this off (hidden, as before), for comparison.
constexpr float kOcarinaBeltAngle = 0.70f;   // (radians from his front toward his left: 40 degrees, the front left)
constexpr float kOcarinaOnBelt = .35f;       // (m: a model's origin this close to the waist joint, sideways, and within
                                             // this above or below it, is worn on the belt; in his hands it is not)
constexpr float kOcarinaCalm = .004f;        // (m, and about radians: a frame's change of its attachment that is holding still)
constexpr int kOcarinaCalmFrames = 20;       // (frames in a row holding still before the attachment is taken)
constexpr float kOcarinaInHolder = .25f;     // (m: the ocarina this close to the holder, both the game's, is in it)
constexpr uint64_t kOcarinaForgetFrames = 300;   // (a gap this long without a draw: a scene change; taken again)
constexpr float kOcarinaInHand = .20f;       // (m: the game's ocarina this close to both his game wrists is in his hands)

bool ocarina_model(uint64_t vb0, uint64_t bytes) {
    if (!vb0 || bytes != rig::ocarina_matrices * 48ull) return false;
    for (uint64_t s : rig::ocarina_shapes) if (s == vb0) return true;
    return false;
}
bool ocarina_holder_model(uint64_t vb0, uint64_t bytes) {
    if (!vb0 || bytes != rig::ocarina_holder_matrices * 48ull) return false;
    for (uint64_t s : rig::ocarina_holder_shapes) if (s == vb0) return true;
    return false;
}
// (skin_draw.inc) an ocarina or holder draw: its colour pass binds the palette in vertex uniform buffer 4, not 3
bool held_ocarina_draw(uint64_t vb0) {
    for (uint64_t s : rig::ocarina_shapes) if (s && s == vb0) return true;
    for (uint64_t s : rig::ocarina_holder_shapes) if (s && s == vb0) return true;
    return false;
}
static bool ocarina_enabled() {
    static const bool off = env_on("RX_VR_NO_OCARINA");
    return !off && opts.vr_ocarina;
}

// One worn model's attachment to the game's waist, in Link's model space (cache_mu held)
struct OcarinaWorn {
    bool have = false, last_have = false;
    int calm = 0;
    uint64_t frame = ~0ull;   // (the game frame its calm was counted for: every pass of a frame counts once)
    Aff rel{}, last{};
};
struct OcarinaBelt {
    bool adult = false;
    uint64_t seen = ~0ull;        // (the last frame either model was drawn)
    OcarinaWorn holder, ocarina;
    Aff turn{};                   // (model space: the turn about the waist's upright axis to the front left; with holder.have)
    V3 holder_game{};             // (where the game has the holder this frame, world, for "the ocarina is in it")
    uint64_t holder_game_frame = ~0ull;
};
static OcarinaBelt ocarina_belt;
// The game's own grip: the ocarina against each game wrist while Link plays it, per age (0 child, 1 adult) and hand (0
// left, 1 right); kept for the whole run once taken (cache_mu held)
static OcarinaWorn ocarina_grip[2][2];

// Counts a frame of the attachment holding still, and takes it after kOcarinaCalmFrames; true when it was just taken
static bool ocarina_attach(OcarinaWorn& w, const Aff& rel, uint64_t frame) {
    if (w.have || w.frame == frame) return false;
    w.calm = w.last_have && aff_change(rel, w.last) < kOcarinaCalm ? w.calm + 1 : 0;
    w.last = rel, w.last_have = true, w.frame = frame;
    if (w.calm < kOcarinaCalmFrames) return false;
    w.rel = rel, w.have = true;
    return true;
}

// The turn about the waist joint's upright axis (model space) that takes the holder from where the game wears it to
// kOcarinaBeltAngle; the angle it was worn at into *worn (radians from his front toward his left)
static Aff ocarina_belt_turn(const Aff& holder_rel, V3 waist, float* worn) {
    const V3 d = sub(holder_rel.t, waist);
    *worn = std::atan2(d.x, d.z);
    const M3 r = axis_angle({0, 1, 0}, kOcarinaBeltAngle - *worn);
    return {r, sub(waist, apply(r, waist))};
}

// The ocarina's or the holder's draw on the VR belt or in a VR hand: its palette moved from the game's place to the VR
// one, or nullptr (not placed this frame: left out near Link by link_worn_hidden)
std::shared_ptr<const std::vector<float>> held_ocarina(const std::vector<float>& raw, V3 o, uint64_t frame,
                                                       const LinkBody& body, bool holder) {
    if (!ocarina_enabled() || raw.size() < 12) return nullptr;
    std::lock_guard lk(cache_mu);
    const Entry* vr = canonical_rig(frame, body);
    if (!vr) return nullptr;
    const rig::Model& M = vr->adult ? rig::adult : rig::child;
    if (vr->raw.size() < (size_t)M.slots * 12 || vr->out->size() < (size_t)M.slots * 12) return nullptr;
    OcarinaBelt& st = ocarina_belt;
    if (st.seen != ~0ull && (frame < st.seen || frame - st.seen > kOcarinaForgetFrames || st.adult != vr->adult)) {
        logf("vr: ocarina: belt attachments forgotten (%s): taken again", st.adult != vr->adult ? "Link's age changed" :
             "a gap in its draws, a scene change");
        st = OcarinaBelt{};
    }
    st.adult = vr->adult, st.seen = frame;
    const Aff T = shifted(aff(raw.data()), o);   // (the model's place in the world: its matrix 0)
    const Aff game_waist = shifted(aff(vr->raw.data() + M.waist * 12), vr->origin);
    Aff inv;
    if (!invert(game_waist, &inv)) return nullptr;
    const Aff rel = mul(inv, T);
    const V3 waist = v3(M.waist_pos), d = sub(rel.t, waist);
    const bool on_belt = std::hypot(d.x, d.z) < kOcarinaOnBelt && std::fabs(d.y) < kOcarinaOnBelt;
    if (holder) {
        st.holder_game = T.t, st.holder_game_frame = frame;
        if (on_belt && ocarina_attach(st.holder, rel, frame)) {
            float worn = 0;
            st.turn = ocarina_belt_turn(st.holder.rel, waist, &worn);
            logf("vr: ocarina: holder attached to the %s waist (%.2f m from its joint, worn %.0f degrees from his front, "
                 "turned to %.0f: the front left)", vr->adult ? "adult" : "child", length(d), worn * 57.2958f,
                 kOcarinaBeltAngle * 57.2958f);
        }
    } else {
        const bool in_holder = st.holder_game_frame != ~0ull && frame - st.holder_game_frame <= 1 &&
                               length(sub(T.t, st.holder_game)) < kOcarinaInHolder;
        if (on_belt && in_holder && ocarina_attach(st.ocarina, rel, frame))
            logf("vr: ocarina: the ocarina in its holder attached (%.2f m from the holder)", length(sub(T.t, st.holder_game)));
    }
    const int age = vr->adult ? 1 : 0;
    if (!holder) {   // (the game's own grip, while the game has its ocarina in Link's hands: away from its holder)
        const bool off_holder = st.holder_game_frame != ~0ull && frame - st.holder_game_frame <= 1 &&
                                !(length(sub(T.t, st.holder_game)) < kOcarinaInHolder);
        Aff by_wrist[2];
        bool in_hands = off_holder;
        for (int k = 0; k < 2 && in_hands; k++) {
            Aff wrist_inv;
            if (!invert(shifted(aff(vr->raw.data() + M.arm[k].wrist * 12), vr->origin), &wrist_inv)) in_hands = false;
            else by_wrist[k] = mul(wrist_inv, T), in_hands = length(by_wrist[k].t) < kOcarinaInHand;
        }
        for (int k = 0; k < 2 && in_hands; k++)
            if (ocarina_attach(ocarina_grip[age][k], by_wrist[k], frame)) {
                const Aff& g = ocarina_grip[age][k].rel;
                logf("vr: ocarina: the game's grip in %s Link's %s hand taken: {%.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, "
                     "%.4f, %.4f, %.4f, %.4f} (its place against the wrist, rows of 3x4)", age ? "adult" : "child",
                     k ? "right" : "left", g.r.v[0][0], g.r.v[0][1], g.r.v[0][2], g.t.x, g.r.v[1][0], g.r.v[1][1],
                     g.r.v[1][2], g.t.y, g.r.v[2][0], g.r.v[2][1], g.r.v[2][2], g.t.z);
            }
    }
    const int hand = holder ? -1 : ocarina_hand();
    const OcarinaWorn& w = holder ? st.holder : st.ocarina;
    // its place: in the VR hand that holds it (the frame's VR wrist x the game's grip, or the Weapon_L mount), or on the
    // belt (the frame's VR waist x the turn to the front left x its attachment)
    const Aff vr_waist = shifted(aff(vr->out->data() + M.waist * 12), vr->origin);
    const bool on_vr_belt = w.have && st.holder.have;
    Aff placed{};
    if (on_vr_belt) placed = mul(mul(vr_waist, st.turn), w.rel);
    if (!holder && on_vr_belt) {
        const float at[3] = {placed.t.x, placed.t.y, placed.t.z};
        ocarina_belt_drawn(at);
    }
    if (hand >= 0) {
        const OcarinaWorn& g = ocarina_grip[age][hand];
        const Aff mount = aff(hand ? (vr->adult ? rig::hookshot_right_adult : rig::hookshot_right_child)
                                   : (vr->adult ? rig::hookshot_left_adult : rig::hookshot_left_child));
        placed = mul(shifted(aff(vr->out->data() + M.arm[hand].wrist * 12), vr->origin), g.have ? g.rel : mount);
        static std::atomic<int> told{-1};
        if (told.exchange(hand * 2 + (g.have ? 1 : 0)) != hand * 2 + (g.have ? 1 : 0))
            logf("vr: ocarina: in the %s VR hand (%s)", hand ? "right" : "left",
                 g.have ? "the game's own grip" : "on the Weapon_L mount until the game's grip is seen: play it once");
    } else if (!on_vr_belt) return nullptr;
    Aff game_inv;
    if (!invert(T, &game_inv)) return nullptr;
    const Aff delta = mul(placed, game_inv);
    auto out = std::make_shared<std::vector<float>>(raw.size());
    for (size_t i = 0; i < raw.size() / 12; i++)
        store(out->data() + i * 12, shifted(mul(delta, shifted(aff(raw.data() + i * 12), o)), mul(o, -1.0f)));
    for (float f : *out) if (!std::isfinite(f)) return nullptr;
    return out;
}
