// Hookshot VR: the Boomerang in the VR hand that holds it (first person, VR tab: Boomerang in the hands).
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
// The held model is Item_Boomerang (vk_vr_boomerang.h, generated from the player's own files): one rigid matrix. While a VR
// hand holds it (boomerang_hand(), told by vr/items/boomerang/boomerang_host.c until the game lets it go), the game's own
// draw of it (in Link's game hand) is moved to the VR hand, held by its wing as Link holds it: the VR wrist x his Weapon_L
// socket as his own clip Boomerang_St places it (vr/tools/generate/boomerang.py) in the left hand, its mirror image in the
// right (the fingers are the game's own grip, arms_and_pose.inc, as for the hookshot). Its draws reach here as equipment
// kind 15 (vr/items/gear/equipment_skin.inc equipment_kind: without it they never did, headset 18 Nov). Thrown, it is the
// game's own again: its flight, its return. While it is being put away it is not drawn (it would show in Link's game hand
// for a moment).
namespace {
bool boomerang_model(uint64_t vb0, uint64_t bytes) {
    if (bytes < rig::boomerang_matrices * 48ull) return false;
    for (uint64_t s : rig::boomerang_shapes)
        if (s == vb0) return true;
    return false;
}
std::shared_ptr<const std::vector<float>> held_boomerang(const std::vector<float>& raw, V3 o, uint64_t frame, const LinkBody& body) {
    const int side = boomerang_hand();
    if (side == -2) return std::make_shared<std::vector<float>>(raw.size(), 0.0f);   // (being put away: not drawn)
    if (side < 0 || side > 1 || !body.hand[side].valid) return nullptr;   // (not in a VR hand: the game's own)
    Aff vr{};
    {
        std::lock_guard lk(cache_mu);
        const Entry* rig_now = canonical_rig(frame, body);
        if (!rig_now) return nullptr;
        const rig::Model& M = rig_now->adult ? rig::adult : rig::child;
        const Aff socket = aff(side ? (rig_now->adult ? rig::boomerang_right_adult : rig::boomerang_right_child)
                                    : (rig_now->adult ? rig::boomerang_left_adult : rig::boomerang_left_child));
        vr = mul(shifted(aff(rig_now->out->data() + M.arm[side].wrist * 12), rig_now->origin), socket);
    }
    // (its one matrix: the model's place in the VR hand x the matrix's own place in the model)
    auto out = std::make_shared<std::vector<float>>(raw.begin(), raw.end());
    store(out->data(), shifted(mul(vr, aff(rig::boomerang_first)), mul(o, -1.0f)));
    for (size_t i = 0; i < 12; i++) if (!std::isfinite((*out)[i])) return nullptr;
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) logf("vr: boomerang held in the %s VR hand", side ? "right" : "left");
    return out;
}
}
