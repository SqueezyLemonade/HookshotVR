// Hookshot VR: the hookshot or longshot in the VR hand that holds it (first person, VR tab: Hookshot in the hands).
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
// The held model is Item_Hookshot_Body (vk_vr_hookshot.h, generated from the player's own files): 13 rigid matrices, its
// barrel along -X. While a VR hand holds it (body.hook_hand, sent by vr/items/hookshot/hookshot_host.c), it is held as Link
// holds it: the VR wrist x the game's own Weapon_L socket in the left hand, its mirror image in the right (the fingers are
// the game's own grip, arms_and_pose.inc). All 13 matrices move together, so the claws and the chain stub keep the game's
// own animation. The barrel's line, as drawn, is what the game's aim and laser follow (hook_barrel, host_api.inc).
// (The first build turned the barrel along the controller's pointing line instead: the model sat at an odd turn in the
// hand, the fingers not on it; headset test 17 Nov.)
namespace {
constexpr float kHookTip = -.335f;   // (Tip_HookShot along the barrel, m: where the laser leaves)
std::mutex hook_barrel_mu;
float hook_barrel_line[6]{};
V3 hook_game_tip{}, hook_vr_tip{};   // (the game's own muzzle and the VR hookshot's, world: the fired chain's hand end)
Aff hook_game_model{}, hook_vr_model{};   // (the model's place in the game's hand and in the VR hand: its effects moved by)
std::chrono::steady_clock::time_point hook_barrel_at{};
// (Some passes bind a larger buffer: only the palette is moved, the rest kept.)
bool hookshot_model(uint64_t vb0, uint64_t bytes) {
    if (bytes < rig::hookshot_matrices * 48ull) return false;
    for (uint64_t s : rig::hookshot_shapes)
        if (s == vb0) return true;
    return false;
}
std::shared_ptr<const std::vector<float>> held_hookshot(const std::vector<float>& raw, V3 o, uint64_t frame, const LinkBody& body) {
    const int side = body.hook_hand;
    if (side < 0 || side > 1 || !body.hand[side].valid) return nullptr;   // (not in a VR hand: the game's own pose)
    Aff vr{};
    {
        std::lock_guard lk(cache_mu);
        const Entry* rig_now = canonical_rig(frame, body);
        if (!rig_now) return nullptr;
        const rig::Model& M = rig_now->adult ? rig::adult : rig::child;
        const Aff socket = aff(side ? (rig_now->adult ? rig::hookshot_right_adult : rig::hookshot_right_child)
                                    : (rig_now->adult ? rig::hookshot_left_adult : rig::hookshot_left_child));
        vr = mul(shifted(aff(rig_now->out->data() + M.arm[side].wrist * 12), rig_now->origin), socket);
    }
    Aff first_inv;
    if (!invert(aff(rig::hookshot_first), &first_inv)) return nullptr;
    const Aff game = mul(shifted(aff(raw.data()), o), first_inv);
    Aff inv;
    if (!invert(game, &inv)) return nullptr;
    const Aff delta = mul(vr, inv);
    auto out = std::make_shared<std::vector<float>>(raw.size());
    std::copy(raw.begin(), raw.end(), out->begin());
    for (size_t i = 0; i < rig::hookshot_matrices; i++)
        store(out->data() + i * 12, shifted(mul(delta, shifted(aff(raw.data() + i * 12), o)), mul(o, -1.0f)));
    for (size_t i = 0; i < rig::hookshot_matrices * 12; i++) if (!std::isfinite((*out)[i])) return nullptr;
    {
        const V3 ahead = unit(mul(column(vr.r, 0), -1.0f), {0, 0, -1}), tip = add(vr.t, mul(ahead, -kHookTip));
        std::lock_guard lk(hook_barrel_mu);
        const float line[6] = {tip.x, tip.y, tip.z, ahead.x, ahead.y, ahead.z};
        std::memcpy(hook_barrel_line, line, sizeof line);
        hook_vr_tip = tip;
        hook_game_model = game, hook_vr_model = vr;
        hook_game_tip = add(game.t, mul(column(game.r, 0), kHookTip));
        hook_barrel_at = std::chrono::steady_clock::now();
    }
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) logf("vr: hookshot held in the %s VR hand", side ? "right" : "left");
    return out;
}
// The fired chain (Item_Hookshot_Chain), drawn by the flying hook from its head back to the game's own muzzle: turned and
// stretched about the head so that end meets the VR hookshot's tip instead. The head (and the hook, rigid on it) stays where
// the game has it; the chain's extending, retracting and pulling stay the game's.
bool hookchain_model(uint64_t vb0, uint64_t bytes) {
    if (bytes != rig::hookchain_matrices * 48ull) return false;
    for (uint64_t s : rig::hookchain_shapes)
        if (s == vb0) return true;
    return false;
}
std::shared_ptr<const std::vector<float>> held_hookchain(const std::vector<float>& raw, V3 o, const LinkBody& body) {
    if (body.hook_hand < 0) return nullptr;
    V3 game_tip, vr_tip;
    {
        std::lock_guard lk(hook_barrel_mu);
        if (std::chrono::steady_clock::now() - hook_barrel_at > std::chrono::milliseconds(150)) return nullptr;
        game_tip = hook_game_tip, vr_tip = hook_vr_tip;
    }
    const V3 head = add(o, V3{raw[3], raw[7], raw[11]});
    const V3 was = sub(game_tip, head), now = sub(vr_tip, head);
    Aff a{identity(), {}};
    if (length(was) < .3f) {   // (the hook close to the hand: moved with it)
        a.t = sub(vr_tip, game_tip);
    } else {
        const V3 u = unit(was, {0, 0, 1});
        const float k = std::clamp(length(now) / length(was), .2f, 5.0f);
        const M3 stretch = stretch_along(u, k);
        a.r = mul(from_to(was, now), stretch);
        a.t = sub(head, apply(a.r, head));   // (the head fixed)
    }
    auto out = std::make_shared<std::vector<float>>(raw.size());
    for (size_t i = 0; i < rig::hookchain_matrices; i++)
        store(out->data() + i * 12, shifted(mul(a, shifted(aff(raw.data() + i * 12), o)), mul(o, -1.0f)));
    for (float f : *out) if (!std::isfinite(f)) return nullptr;
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) logf("vr: hookshot chain bent to the VR hookshot (head %.2f m from the game's muzzle)", length(was));
    return out;
}
// The hookshot's effects (its red aiming laser: small effect draws that appear only while it aims, headset log 17 Nov): an
// effect whose matrix sits on the game's hookshot is drawn through a camera moved by the change from the game's hookshot to
// the VR one (as the elemental arrows' effects, skin_draw.inc vr_arrow_effect)
// The chain's draws (its colour pass binds its palette in uniform buffer 4, not 3, with the usual object buffer: headset log
// 17 Nov; skin_draw.inc gives that buffer to the same placement)
bool held_hookchain_draw(uint64_t vb0) {
    for (uint64_t s : rig::hookchain_shapes)
        if (s == vb0) return true;
    return false;
}
bool held_hook_effects_live() {
    std::lock_guard lk(hook_barrel_mu);
    return std::chrono::steady_clock::now() - hook_barrel_at < std::chrono::milliseconds(150);
}
bool held_hook_effect_move(const float in[16], const float camera[3], float move[12]) {
    Aff game, vr;
    {
        std::lock_guard lk(hook_barrel_mu);
        if (std::chrono::steady_clock::now() - hook_barrel_at > std::chrono::milliseconds(150)) return false;
        game = hook_game_model, vr = hook_vr_model;
    }
    const float at[3] = {hook_game_tip.x, hook_game_tip.y, hook_game_tip.z};
    if (!arrow_fx::source_matches(in, at, camera)) return false;
    Aff inv;
    if (!invert(game, &inv)) return false;
    store(move, mul(vr, inv));
    return true;
}
bool held_hook_barrel(float out[6]) {   // (vr::hook_barrel, vk_vr_body.cpp)
    std::lock_guard lk(hook_barrel_mu);
    if (std::chrono::steady_clock::now() - hook_barrel_at > std::chrono::milliseconds(150)) return false;
    std::memcpy(out, hook_barrel_line, sizeof hook_barrel_line);
    return true;
}
}
