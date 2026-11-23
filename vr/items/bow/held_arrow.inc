// Hookshot VR: the arrow nocked on the VR bow (the game's own arrow model, moved onto the string).
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
// The game's standalone normal arrow (Item_Arrow_Normal, vb0 113976, one rigid matrix) is the held arrow AND every flying or
// stuck one, and its object/palette buffers alternate frame to frame, so neither the model nor its address says which arrow
// it is. The one the game has nocked on Link's OWN bow, this frame, is told apart by how it sits on that bow (the game's bow
// root as uploaded, before the VR pose): in every recording (vr-item-dump-131150747377220, -132852667698202) the nocked
// arrow's shaft is the bow's -x within 15 degrees, beside the string's centre (|y|, |z| under 8 cm), sliding from -0.34 to
// +0.24 m along x as the game draws. A flying arrow leaves that within a frame; a stuck one is not on Link's bow. Only that
// arrow moves: onto the committed solution, nocked (the notch at its tail, 0.41 m behind its origin; the mesh ends at 0.423)
// on the string's rest, or on the drawing hand while held, its shaft in the recorded drawn orientation, beside the handle as
// the game's is (the drawn string centre and the arrow agree to the millimetre across it).
// The fire, ice and light arrows are their own standalone models (vk_vr_arrows.h, from the player's files), also one rigid
// matrix: the same arrow body plus a crystal (and, for light, a god-ray cone) on that matrix. They take the same path. Nocked,
// though, the game draws a fire, ice or light arrow with the normal arrow's model plus effects (flames, frost, light): those
// are drawn on the VR arrow by moving their draws' camera (arrow_effect_move below; the draw side in skin_draw.inc and
// palette_draw.inc; the camera maths in arrow_effect_math.h). (Researched 16 Nov.)
// The game's held arrow and the VR one, as last placed (world): for its effects (arrow_effect_move). The game places a held
// arrow's effects (the Pouch_Arrow_* sets) at its own arrow in Link's game hand, behind the first-person eye. Each effect
// draw carries an effect matrix in its vertex uniform buffer 11 (C14) that sits on that arrow; its particles keep their own
// world positions (in a storage buffer), so moving that matrix moves nothing: the draw's particle camera is moved instead,
// by the change that moves the arrow.
struct ArrowPlaced { Aff game{}, vr{}; uint64_t frame = ~0ull; };
std::mutex arrow_placed_mu;
ArrowPlaced arrow_placed;
std::atomic<uint64_t> arrow_placed_frame{~0ull};   // (arrow_placed.frame, read without the lock)
}   // namespace (held_arrow's file-local pieces end here: the two below are called from the draw replay)

bool arrow_effects_live() {
    const uint64_t f = arrow_placed_frame.load(std::memory_order_relaxed);
    return f != ~0ull && texture_frame_no() <= f + 2;
}

bool arrow_effect_move(const float in[16], const float camera[3], float move[12]) {
    ArrowPlaced a;
    {
        std::lock_guard lk(arrow_placed_mu);
        a = arrow_placed;
    }
    const uint64_t now = texture_frame_no();
    if (a.frame == ~0ull || now > a.frame + 2) return false;
    const float game[3] = {a.game.t.x, a.game.t.y, a.game.t.z};
    if (!arrow_fx::source_matches(in, game, camera)) return false;
    Aff inv;
    if (!invert(a.game, &inv)) return false;
    store(move, mul(a.vr, inv));
    return true;
}

namespace {
const rig::ArrowShape* arrow_model(uint64_t vb0) {
    for (const rig::ArrowShape& a : rig::arrow_shapes)
        if (a.vb0 == vb0) return &a;
    return nullptr;
}
std::shared_ptr<const std::vector<float>> held_arrow(const float* raw, V3 o, uint64_t frame, const LinkBody& body) {
    Aff bow{};
    bool fresh_bow = false, recent_bow = false;
    {
        std::lock_guard lk(bow_mu);
        if (native_bow.frame != ~0ull && frame >= native_bow.frame) {
            fresh_bow = frame - native_bow.frame <= 1;
            recent_bow = frame - native_bow.frame <= 3;
            bow = native_bow.world;
        }
    }
    const Aff native_arrow = shifted(aff(raw), o);
    Aff inv, rel{};
    if (fresh_bow && !invert(bow, &inv)) fresh_bow = false;
    if (fresh_bow) rel = mul(inv, native_arrow);
    const V3 shaft = unit(column(rel.r, 2), {0, 0, 0});
    const bool nocked = fresh_bow && rel.t.x > -.45f && rel.t.x < .40f && std::fabs(rel.t.y) < .12f &&
                        std::fabs(rel.t.z) < .12f && -shaft.x > .9f;
    if (!nocked) {
        // Carry/roll poses need not align with the native bow. Check the TAIL,
        // the point held by the native hand, not just the origin 41 cm ahead.
        // Also allow arrow-before-bow submission: no fresh bow upload is needed
        // to identify an arrow positively carried by Link's own native hand.
        if (!body.bow_equipped && !recent_bow) return nullptr;
        std::lock_guard lk(cache_mu);
        const V3 tail = add(native_arrow.t, apply(native_arrow.r, {0, 0, -.41f}));
        const unsigned match = native_arrow_hand_match(tail,native_arrow.t,frame,body);
        if (!(match & 1)) {
            // Preserve the old suppression for other native hand-transition poses;
            // do not broaden it to arbitrary nearby world/projectile arrows.
            if (match & 2) return std::make_shared<std::vector<float>>(12, 0.0f);
            return nullptr;
        }
    }
    const BowPose b = bow_pose(frame, body);
    if (!b.ok) return nullptr;
    if (b.hidden) return std::make_shared<std::vector<float>>(12, 0.0f);   // (with the bow: gone at once)
    const M3 hold = columns({0, 1, 0}, {0, 0, -1}, {-1, 0, 0});   // (the recorded nocked orientation, in bow model axes)
    const V3 nock_local{0, 0, -.41f}, beside{0, .034f, .011f};
    const float s2 = body.bow_scale * body.bow_scale;
    const V3 nock_bow = add(add(kBowGrip, mul(apply(transpose(b.target), sub(b.nock, b.palm)), 1 / s2)), beside);
    const V3 at = sub(nock_bow, apply(hold, nock_local));
    auto out = std::make_shared<std::vector<float>>(12);
    const Aff placed{mul(b.target, hold), sub(add(b.palm, apply(b.target, sub(at, kBowGrip))), o)};
    store(out->data(), placed);
    {   // (for the arrow's effects: the game's arrow and the VR one, world)
        std::lock_guard lk(arrow_placed_mu);
        arrow_placed = {native_arrow, shifted(placed, o), frame};
        arrow_placed_frame.store(frame, std::memory_order_relaxed);
    }
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) logf("vr: held arrow nocked on the VR bow (%s, game offset %.3f %.3f %.3f)",
                                 nocked ? "native bow match" : "native hand carries tail", rel.t.x, rel.t.y, rel.t.z);
    return out;
}
