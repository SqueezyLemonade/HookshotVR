// Hookshot VR: the Fairy Slingshot drawn in the VR hand, its pouch in the drawing hand.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order (after the bow: it uses bow_pose). Not compiled on its own.
// The held model is Weapon_Range_SlingShot (vk_vr_slingshot.h), always drawn from the game's idle pose for it, whatever the
// game's own draw is doing (the game's ZR, held while the pouch is drawn, is also its own pull, which would show the pouch
// pulled toward Link's game hand). Placed as the hand holds it (sling_place, sling_rig.inc). While the pouch is held (the
// bow string's state, bow_pose), the pouch and the seed in it go to the drawing hand, turned with the line to the fork, and
// each strap is turned and stretched about its fork tip so it still runs from the tip to the pouch.
std::shared_ptr<const std::vector<float>> held_slingshot(const std::vector<float>& raw, V3 o, uint64_t frame, const LinkBody& body) {
    if (!body.sling) return nullptr;   // (not the slingshot out in first person: the game's own pose)
    constexpr unsigned N = rig::sling_matrices;
    // (Link's own: the game's slingshot near Link's root. A slingshot elsewhere, a prize on show, is left alone)
    if (length(sub(add(o, V3{raw[12 * 12 + 3], raw[12 * 12 + 7], raw[12 * 12 + 11]}), v3(body.root))) > 2.5f) return nullptr;
    Aff vr{};
    if (!sling_place(frame, body, &vr)) return nullptr;
    const BowPose b = bow_pose(frame, body);
    if (!b.ok) return nullptr;
    {   // (the model counts as the bow's: drawn this frame)
        uint64_t seen = bow_model_frame.load(std::memory_order_relaxed);
        while (seen < frame && !bow_model_frame.compare_exchange_weak(seen, frame, std::memory_order_relaxed)) {}
    }
    auto out = std::make_shared<std::vector<float>>(raw);
    if (b.hidden) {   // (its grip let go: gone at once, not after the game's put-away; every vertex at one point)
        std::fill(out->begin(), out->end(), 0.0f);
        return out;
    }
    Aff pal[N];
    for (unsigned i = 0; i < N; i++) pal[i] = mul(vr, aff(rig::sling_rest[i]));
    if (b.held) {
        const V3 fork = sling_point(vr, rig::sling_fork), rest = sling_point(vr, rig::sling_pouch);
        const V3 was = sub(rest, fork), now = sub(b.nock, fork);
        const M3 r = length(now) > .03f ? from_to(was, now) : identity();
        const Aff pouch{r, sub(b.nock, apply(r, rest))};   // (rest -> the drawing hand, turned with the line to the fork)
        for (int16_t s : rig::sling_pouch_slots) pal[s] = mul(pouch, pal[s]);
        const auto strap = [&](const float (&tip_at)[3], const float (&anchor_at)[3], const int16_t (&slots)[3]) {
            const V3 tip = sling_point(vr, tip_at), a0 = sling_point(vr, anchor_at);
            const V3 a1 = add(apply(pouch.r, a0), pouch.t);
            const V3 old_line = sub(a0, tip), new_line = sub(a1, tip);
            const float len = length(old_line);
            if (len < .01f) return;
            const M3 change = mul(from_to(old_line, new_line), stretch_along(unit(old_line, {1, 0, 0}),
                                                                             std::clamp(length(new_line) / len, .25f, 5.0f)));
            const Aff about{change, sub(tip, apply(change, tip))};   // (the tip fixed)
            for (int16_t s : slots) pal[s] = mul(about, pal[s]);
        };
        strap(rig::sling_tip_l, rig::sling_anchor_l, rig::sling_strap_l);
        strap(rig::sling_tip_r, rig::sling_anchor_r, rig::sling_strap_r);
    }
    for (unsigned i = 0; i < N; i++) store(out->data() + i * 12, shifted(pal[i], mul(o, -1.0f)));
    for (unsigned i = 0; i < N * 12; i++) if (!std::isfinite((*out)[i])) return nullptr;
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) logf("vr: slingshot held in the %s VR hand", body.bow_hand ? "right" : "left");
    return out;
}
