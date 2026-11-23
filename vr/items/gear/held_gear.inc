// Hookshot VR: swords, sheaths and shields: stowed on the body, or held in a VR hand (the sword's authored
// palm socket).
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
// Swords, sheaths and shields: separate small models the game attaches to Link's own bones: stowed, the upper chest
// (vr-item-dump-155545276157324: within ~4 cm of Spine_2/Neck, the rest belt motion); drawn or guarding, a hand. Left in the
// game's pose they followed the GAME's body, which turns with running while the VR torso faces the view: running in circles
// put them through the body. Each one is moved by the change, from the game's body upload to the VR pose, of the bone it is
// attached to now, so it keeps exactly the game's own attachment: on the VR back when stowed, in the VR hand, held as the
// game holds it, when drawn. All of a model's matrices move together (its belts keep their own motion).
// Which bone: the one it stays rigid to. Each game frame, its transform relative to the game's upper chest and to either
// wrist is compared with the last frame's; the one that holds still (smoothed over a few frames, with hysteresis) is its
// attachment. (A first build chose by distance to the wrists, 22/30 cm: a drawn sword's origin sat beyond it and stayed on
// the torso, so it played the game's hand animation, and the shield hovering at the threshold flipped and blinked. It
// also used the lower spine, which the game's guard and aim stances bend away from the chest: the stowed sword bounced.)
// Of several body uploads in one frame (shadow, depth, colour: not always the same animation snapshot), the one that keeps
// the chosen attachment most continuous is used, so a pass's mismatched snapshot does not jolt it.
// Signatures from the user's own models: vb0 size and palette matrices (Weapon_Sword_*, Weapon_Sheath_*, Weapon_Shield_*).
#include "../deku_stick/held_stick.inc"   // (the Deku Stick: a weapon too, held in a VR hand; anchored_skin hands it its draws)
struct Anchored { uint64_t vb0; uint32_t matrices; };
constexpr Anchored kAnchored[] = {
    {56688, 1}, {87120, 1}, {86376, 1},                 // Master Sword: blade, grip coat, grip
    {146448, 1}, {118392, 1},                           // Master Sword (dark)
    {148632, 1}, {137712, 1}, {86880, 1},               // Biggoron, broken Biggoron, Kokiri swords
    {96600, 1}, {172416, 1}, {484288, 6}, {199276, 10}, // sheaths: Master, Master (dark), Kokiri, Biggoron
    {228696, 2}, {138320, 2}, {118560, 2}, {125916, 2}, {4872, 2},   // shields: Deku, Hylian, Hylian (dark), Mirror (2)
};
bool anchored_model(uint64_t vb0, uint64_t bytes) {
    for (const Anchored& a : kAnchored) if (a.vb0 == vb0 && bytes == a.matrices * 48ull) return true;
    return stick_model(vb0, bytes) || bottle_model(vb0, bytes);
}
#include "shield_grip.inc"   // (the shield's grip, moved across its back for the left hand)
// The held shield while guarding (VR tab Shield when guarding: 0 Arm, 1 Guard turn, 2 Fist). The game wears the shield
// along the forearm, its face to the side, and in its own guard Link raises that forearm across his body: Arm (the default
// since 22 Nov) keeps it so, the shield following the hand as it is held, and the player raises the forearm to guard (the
// game's block faces the body's heading whatever the shield shows: vr/items/shield/guard_host.c). A VR arm held out toward
// an enemy points the forearm forward, so the shield faced sideways (player, headset 19 Nov): Guard turn turns it a quarter
// turn while the game guards, Fist all the time, its face (the shield's +y) to where the fist points (its -x;
// hand_sockets.py checks both), its long side (+z, the thumb's side) still up. The turn is about the grip: the Weapon_R
// mount, the shield's origin, where the palm holds it (the grip's middle within 2 mm of it). It was about the wrist joint,
// 7.5 cm (child) / 9.2 cm (adult) behind the grip, which swung the grip 11-13 cm out of the fingers while the arm held
// still (player, 22 Nov: the shield turned in the hand). The game side says when the game guards: its ZL held
// (vr::shield_guarding, from vr/input/controllers_host.c). The turn eases in and out over kShieldTurnMs. The right hand's
// pose; the left hand's (shield_left_*, the placement mirrored in x) turns the other way.
constexpr float kShieldTurnMs = 120.0f;
static float shield_turn_amount() {   // 0 on the arm .. 1 facing forward, eased (under cache_mu)
    static float amount = 0;
    static int64_t at = 0;
    const int64_t now = now_ms();
    const int mode = opts.vr_shield_pose;
    const float want = mode == 2 || (mode == 1 && shield_guarding()) ? 1.0f : 0.0f;
    const float step = at ? std::clamp((float)(now - at) / kShieldTurnMs, 0.0f, 1.0f) : 1.0f;
    at = now;
    amount += std::clamp(want - amount, -step, step);
    return amount;
}
static Aff shield_guard_pose(bool left, float amount) {   // (about the grip, the shield's origin: it stays in the palm)
    return {axis_angle({0, 0, 1}, (left ? -1.0f : 1.0f) * amount * 1.5707963f), {0, 0, 0}};
}

// The sword's charge glow (and any effect the game emits on its own sword): the game draws it on Link's game sword, at his
// game hand, so in first person it showed there and not on the VR sword (the user, 22 Nov, the spin's charge with magic: a
// blue glow, brighter at stage 2). An effect draw whose effect matrix sits on that sword (within kSwordGlowReach of its
// blade, grip to tip) is drawn on the VR sword, as the Deku Stick's flame is on the VR stick: its particle camera moved by
// the change from the game's sword to the VR one (held_sword_effect_move; the draw side is skin_draw.inc vr_arrow_effect).
constexpr float kSwordGlowReach = .25f;   // (m from the game's blade)
constexpr float kBiggoronGripUp = .15f;   // (m: Biggoron's Sword slid back in the VR hand, the hand further up its hilt)
std::mutex sword_placed_mu;
Aff sword_placed_game{}, sword_placed_vr{};
float sword_placed_tip = .53f;   // (m along the model's z: the Kokiri Sword's tip; the Master Sword's 1.09)
std::chrono::steady_clock::time_point sword_placed_at{};
uint64_t sword_placed_vb0 = 0;   // (which sword: the log's)
bool held_sword_effects_live() {
    std::lock_guard lk(sword_placed_mu);
    return std::chrono::steady_clock::now() - sword_placed_at < std::chrono::milliseconds(150);
}
// (every 2 s while a VR hand holds a sword: the effects near the game's sword, moved and left by their distance, so a
// glow that is not moved shows how far from the blade it is: the bounded lines below run out on other effects first)
static void sword_effect_tally(int what, float near) {   // (what: 0 moved (+z), 3 moved (-z), 1 too far, 2 the camera check failed)
    static std::mutex mu;
    static int moved, moved_back, far_[8], cam;
    static std::chrono::steady_clock::time_point since{};
    std::lock_guard lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (what == 0) moved++;
    else if (what == 3) moved++, moved_back++;
    else if (what == 2) cam++;
    else if (near < .8f) far_[std::min(7, int(near * 10))]++;
    if (now - since < std::chrono::seconds(2)) return;
    static std::atomic<int> lines{0};
    if ((moved || cam || far_[2] || far_[3] || far_[4] || far_[5] || far_[6] || far_[7]) && lines.fetch_add(1) < 120)
        logf("vr: sword: effects near the game's sword (2 s): %d moved (%d of them on its -z side); left: %d 20-30 cm, %d "
             "30-40, %d 40-50, %d 50-60, %d 60-80 cm from its blade, %d failing the camera check", moved, moved_back, far_[2],
             far_[3], far_[4], far_[5], far_[6] + far_[7], cam);
    moved = moved_back = cam = 0;
    for (int& f : far_) f = 0;
    since = now;
}
bool held_sword_effect_move(const float in[16], const float camera[3], float move[12]) {
    Aff game, vr;
    float tip;
    uint64_t which;
    {
        std::lock_guard lk(sword_placed_mu);
        if (std::chrono::steady_clock::now() - sword_placed_at > std::chrono::milliseconds(150)) return false;
        game = sword_placed_game, vr = sword_placed_vr, tip = sword_placed_tip, which = sword_placed_vb0;
    }
    // (the blade both ways from the grip along the model's z: the Kokiri Sword's runs along +z, but the Master Sword's
    // stage-2 glow sat 30-80 cm from a +z blade, its nearest point at the grip, headset 22 Nov 07:55: the other way)
    const V3 grip = game.t, end = add(apply(game.r, V3{0, 0, tip}), game.t), along = sub(end, grip), fx{in[12], in[13], in[14]};
    const float signed_t = std::clamp(dot(sub(fx, grip), along) / std::max(dot(along, along), 1e-6f), -1.0f, 1.0f);
    const float t = std::fabs(signed_t);
    const V3 on = add(grip, mul(along, signed_t));
    const float at[3] = {on.x, on.y, on.z};
    const float near = length(sub(fx, on));
    if (!arrow_fx::source_matches(in, at, camera)) {
        if (near < .8f) sword_effect_tally(2, near);
        static std::atomic<int> told_cam{0};   // (bounded: near the sword, but its camera not the effect layout's check)
        if (near < 1.0f && told_cam.fetch_add(1) < 20)
            logf("vr: sword: an effect %.2f m from the game's sword vb0 %llu (%.0f%% up) failed the camera check: left",
                 near, (unsigned long long)which, t * 100);
        return false;
    }
    if (!(near < kSwordGlowReach)) {
        sword_effect_tally(1, near);
        static std::atomic<int> told_far{0};   // (bounded: an effect near the sword left where it is, should one be its own)
        if (near < 1.0f && told_far.fetch_add(1) < 20)
            logf("vr: sword: an effect %.2f m from the game's sword vb0 %llu (%.0f%% up its blade) left where it is", near,
                 (unsigned long long)which, t * 100);
        return false;
    }
    Aff inv;
    if (!invert(game, &inv)) return false;
    store(move, mul(vr, inv));
    sword_effect_tally(signed_t < 0 ? 3 : 0, near);
    static std::atomic<uint64_t> told_vb0{0};
    static std::atomic<unsigned> told{0};   // (once for each quarter of the blade an effect sits on, each sword)
    if (told_vb0.exchange(which) != which) told = 0;
    const unsigned quarter = 1u << std::min(3, int(t * 4));
    if (!(told.fetch_or(quarter) & quarter))
        logf("vr: sword: an effect %.2f m from the game's sword vb0 %llu (%.0f%% up the blade) drawn on the VR sword", near,
             (unsigned long long)which, t * 100);
    return true;
}

float aff_change(const Aff& a, const Aff& b) {   // metres, plus the largest axis change (about radians for small turns)
    float e = length(sub(a.t, b.t));
    for (int k = 0; k < 3; k++) e = std::max(e, length(sub(column(a.r, k), column(b.r, k))));
    return e;
}
std::shared_ptr<const std::vector<float>> anchored_skin(uint64_t vb0, const std::vector<float>& raw, V3 o, uint64_t frame,
                                                        const LinkBody& body) {
    if (stick_model(vb0, raw.size() * 4ull)) return held_stick(raw, o, frame, body);   // (vr/items/deku_stick/held_stick.inc)
    if (bottle_model(vb0, raw.size() * 4ull)) return held_bottle(vb0, raw, o, frame, body);   // (vr/items/bottle/held_bottle.inc)
    std::lock_guard lk(cache_mu);
    const Entry* vr = canonical_rig(frame, body);
    if (!vr) return nullptr;
    const rig::Model& M = vr->adult ? rig::adult : rig::child;
    const V3 item = add(o, V3{raw[3], raw[7], raw[11]});
    if (length(sub(item, v3(body.root))) > 2.5f) return nullptr;
    // 0 the upper chest (Spine_2: palette slot 2 adult, 7 child; the VR torso is rigid, so its change is the torso's),
    // 1 the left wrist, 2 the right
    const int slots[3] = {vr->adult ? 2 : 7, M.arm[0].wrist, M.arm[1].wrist};
    struct State { uint64_t vb0 = 0, frame = ~0ull, owner = 0; int anchor = 0, streak = 0; bool have = false;
                   bool fixed_have[3]{}; Aff last[3]{}, fixed[3]{}; float still[3] = {1, 1, 1}; int calm_streak = 0; };
    static State states[32];   // (per model, under cache_mu)
    State* st = nullptr;
    for (State& x : states) if (x.vb0 == vb0) { st = &x; break; }
    if (!st) for (State& x : states) if (!x.vb0) { x.vb0 = vb0; st = &x; break; }
    if (!st) return nullptr;
    const bool sheath = (vb0 == 96600 || vb0 == 172416 || vb0 == 484288 || vb0 == 199276);
    const bool shield = (vb0 == 228696 || vb0 == 138320 || vb0 == 118560 || vb0 == 125916 || vb0 == 4872);
    const unsigned kind = shield ? 1u : 0u;
    const bool semantic = !sheath && (body.equipment_known & (1u << kind)) && body.equipment_owner[kind];
    // Stowed gear (a sheath, or a sword/shield the game reports put away): first person leaves it out unless the VR tab's
    // "Stowed gear" shows it. Its back attachment is learned from the game's own sword-to-chest relation, which drifts with
    // the game's belt physics and animations: it floated in front of the VR body (user report, bombs out). The player
    // cannot see their own back anyway. Every vertex at one point: not drawn.
    // A shield the game holds out with the hookshot (adult Link holds both): put away too while a VR hand holds the hookshot,
    // so the hand is not given both (user report, 17 Nov: the shield came out in the hand that took the hookshot). The same
    // beside any other item a VR hand holds (the Deku Stick, the Boomerang: the user, 22 Nov; tracking.inc shield_away).
    if (shield && body.shield_away) return std::make_shared<std::vector<float>>(raw.size(), 0.0f);
    // The sword taken from the back by a VR hand (vr/input/controllers_host.c, vr::sword_hand): the sword in that hand and
    // the shield in the other, drawn or not in the game (its first swing draws it there).
    // Child Link's Hylian Shield stays the game's (worn on his back, his guard a crouch behind it): not put in the other VR
    // hand, and that hand stays free (kept_shield, vr/body/arms_and_pose.inc).
    const bool kept = shield && !vr->adult && vb0 == 138320;
    if (kept) kept_shield_at.store(now_ms(), std::memory_order_relaxed);
    int vr_sword = semantic ? sword_hand() : -1;
    if (kept && vr_sword >= 0) vr_sword = -1;
    // (-2: the sword let go, the game still putting it away (a swing's recovery first, seconds): the sword hidden meanwhile,
    // not shown in the game's own hand. The shield is not part of it: raised meanwhile (the game guards), it is drawn as
    // any shield the game holds out, on the VR wrist; review 18 Nov: it was hidden for the whole put-away wait)
    if (vr_sword == -2 && !shield && !opts.vr_show_stowed) return std::make_shared<std::vector<float>>(raw.size(), 0.0f);
    if (vr_sword == -2) vr_sword = -1;
    if (!opts.vr_show_stowed && vr_sword < 0 && (sheath || (semantic && !(body.equipment_drawn & (1u << kind))))) {
        auto out = std::make_shared<std::vector<float>>(raw.size(), 0.0f);
        return out;
    }
    // A sword's rigid palette (the Master Sword's, the Kokiri Sword's) = VR wrist skin * bind-space Weapon_L socket * sword Skl_Root.
    // The native Weapon_L bone has its own animation (including the draw), independent of Wrist_L. Learning a relative
    // transform from it caused first-draw floating, idle drift and unequal per-shape refinement. Use the authored bind
    // socket immediately and identically for blade/coat/grip and every render pass. The Biggoron Swords keep the learned path.
    if (sheath && vr_sword >= 0 && !opts.vr_show_stowed) return std::make_shared<std::vector<float>>(raw.size(), 0.0f);
    if (semantic && !shield && ((body.equipment_drawn & 1u) || vr_sword >= 0) && raw.size() == 12 &&
        (vb0 == 56688 || vb0 == 87120 || vb0 == 86376 || vb0 == 86880)) {
        const bool right = vr_sword == 1;   // (the game's own draw, and the left VR hand: Link's left hand)
        const bool kokiri = vb0 == 86880;
        const Aff socket = aff(kokiri ? (right ? (vr->adult ? rig::kokiri_sword_right_adult : rig::kokiri_sword_right_child)
                                               : (vr->adult ? rig::kokiri_sword_adult : rig::kokiri_sword_child))
                                      : (right ? (vr->adult ? rig::master_sword_right_adult : rig::master_sword_right_child)
                                               : (vr->adult ? rig::master_sword_adult : rig::master_sword_child)));
        const Aff wrist = shifted(aff(vr->out->data() + M.arm[right ? 1 : 0].wrist * 12), vr->origin);
        auto out = std::make_shared<std::vector<float>>(12);
        store(out->data(), shifted(mul(wrist, socket), mul(o, -1.0f)));
        for (float f : *out) if (!std::isfinite(f)) return nullptr;
        if (vr_sword >= 0) {   // (the game's sword and the VR one, world: for its effects, held_sword_effect_move)
            std::lock_guard lk(sword_placed_mu);
            sword_placed_game = shifted(aff(raw.data()), o), sword_placed_vr = mul(wrist, socket);
            sword_placed_tip = kokiri ? .53f : 1.09f, sword_placed_at = std::chrono::steady_clock::now(), sword_placed_vb0 = vb0;
        }
        return out;
    }
    // A drawn shield: smooth-skinned (Skl_Root, ArmStrap) in model space, so at rest both palette entries are its placement:
    // VR Wrist_R skin * the bind-space Weapon_R socket (rig::shield_*, from the same skeleton data as the sword's), for all
    // passes and every frame alike. Not learned from the game's animated pose: its guard/idle motion and the learned
    // attachment's drift no longer move it. The strap's own animation is dropped (it stays at rest).
    if (semantic && shield && ((body.equipment_drawn & 2u) || vr_sword >= 0) && raw.size() == 24) {
        const bool left = vr_sword == 1;   // (the sword in the right VR hand: the shield in the left)
        const Aff socket = aff(left ? (vr->adult ? rig::shield_left_adult : rig::shield_left_child)
                                    : (vr->adult ? rig::shield_adult : rig::shield_child));
        const Aff wrist = shifted(aff(vr->out->data() + M.arm[left ? 0 : 1].wrist * 12), vr->origin);
        auto out = std::make_shared<std::vector<float>>(24);
        const float turned = shield_turn_amount();   // (facing forward while guarding, VR tab Shield when guarding)
        Aff held = turned > 0 ? mul(socket, shield_guard_pose(left, turned)) : socket;
        // (the left hand's grip moved across the shield's back, for a shield the renderer recognised: the placement moved back
        // by as much, in the shield's own space before the guard turn, so the grip stays in the palm: shield_grip.inc)
        const rig::ShieldGrip* grip = left ? shield_grip_model(vb0) : nullptr;
        const bool grip_moved = grip_recognised(grip);
        if (grip_moved) held = mul(held, Aff{identity(), {grip->place, 0, 0}});
        const Aff placed = shifted(mul(wrist, held), mul(o, -1.0f));
        store(out->data(), placed), store(out->data() + 12, placed);
        for (float f : *out) if (!std::isfinite(f)) return nullptr;
        if (grip_moved) grip_palette_add(out);   // (its draws bind the moved grip: shield_grip_draw.inc)
        return out;
    }
    if (semantic && st->owner != body.equipment_owner[kind]) {
        *st = State{};
        st->vb0 = vb0;
        st->owner = body.equipment_owner[kind];
    }
    const Aff T = shifted(aff(raw.data()), o);
    // the game's body upload: of this frame's (or the canonical rig's source), the one that keeps the attachment continuous
    const Entry* orig = nullptr;
    Aff rel[3];
    float best = 1e9f;
    for (const Entry& e : cache) {
        if (!e.out || e.adult != vr->adult || e.frame > frame || frame - e.frame > 2 || e.raw.size() < (size_t)M.slots * 12) continue;
        Aff r[3], inv;
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            if (!invert(shifted(aff(e.raw.data() + slots[k] * 12), e.origin), &inv)) ok = false;
            else r[k] = mul(inv, T);
        }
        if (!ok) continue;
        const float score = (st->have ? aff_change(r[st->anchor], st->last[st->anchor]) : 0.0f) + (e.frame == frame ? 0.0f : .05f);
        if (score < best) best = score, orig = &e, rel[0] = r[0], rel[1] = r[1], rel[2] = r[2];
    }
    if (!orig) return nullptr;
    // Sheaths never leave the back. Swords and shields go to a hand only when, for 8 frames running, that game hand holds
    // them clearly stiller than the chest and they are within 45 cm of its wrist; back to the chest the same way (or past
    // 55 cm). (vr-item-dump-157743866290805: nothing is rigid to any bone while running, the stowed gear swings ~3.5 cm a
    // frame against the stillest one; stowed gear came within 23-32 cm of a swinging game wrist in other recordings.)
    if (st->frame != frame) {   // (once per game frame)
        if (st->have)
            for (int k = 0; k < 3; k++) st->still[k] += (aff_change(rel[k], st->last[k]) - st->still[k]) * .35f;
        float reach[2];
        for (int k = 0; k < 2; k++)
            reach[k] = length(sub(add(orig->origin, point(orig->raw.data() + slots[k + 1] * 12, v3(M.arm[k].wrist_pos))), item));
        int want = st->anchor;
        if (semantic) want = (body.equipment_drawn & (1u << kind)) ? (shield ? 2 : 1) : 0;
        // (a sword a VR hand holds rides that hand: the learned path kept a drawn one in the left, so Biggoron's Sword taken
        // with the right hand showed in the left, headset 22 Nov)
        if (!shield && !sheath && vr_sword >= 0) want = vr_sword + 1;
        else if (sheath) want = 0;
        else if (st->anchor == 0) {
            for (int k = 1; k < 3; k++)
                if (reach[k - 1] < .45f && st->still[k] < st->still[0] * .4f) want = k;
        } else if (reach[st->anchor - 1] > .55f || st->still[0] < st->still[st->anchor] * .4f) want = 0;
        st->streak = want != st->anchor ? st->streak + 1 : 0;
        if ((semantic && want != st->anchor) || st->streak >= 8 ||
            (!semantic && want == 0 && st->anchor && reach[st->anchor - 1] > .55f)) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 40)
                logf("vr: weapon/shield vb0 %llu now rides the %s", (unsigned long long)vb0,
                     want == 0 ? "chest" : want == 1 ? "left hand" : "right hand");
            st->anchor = want, st->streak = 0;
            if (!semantic) st->fixed_have[want] = false; // heuristic may have learned this socket before it was really held
        }
        // The attachment is FIXED: taken from the game while it holds still against its bone (standing, not swinging),
        // slowly refined then, and kept through the game's running sway, idle animation and swings, so the piece sits
        // still on the VR back or in the VR hand.
        // Taken (whole, not blended: blending rotation matrices scaled them) only after 10 calm frames in a row on this
        // bone, so a draw/sheathe transition is never captured: a stowed sword learned mid-sheathe floated in front of
        // the body (user report, bow out, sword known sheathed). Until then the game's current relation is used.
        const float calm = aff_change(rel[st->anchor], st->last[st->anchor]);
        st->calm_streak = st->have && calm < .006f ? st->calm_streak + 1 : 0;
        // (a sword a VR hand holds keeps the attachment once taken: the game turns Biggoron's Sword about 90 degrees in its
        // hand for the spin's charge, and the VR one turned with it, headset 22 Nov)
        const bool held_sword = !shield && !sheath && vr_sword >= 0 && st->fixed_have[st->anchor];
        if (st->calm_streak >= 10 && !held_sword) st->fixed[st->anchor] = rel[st->anchor], st->fixed_have[st->anchor] = true;
        for (int k = 0; k < 3; k++) st->last[k] = rel[k];
        st->have = true, st->frame = frame;
    }
    const int slot = slots[st->anchor];
    Aff inv;
    if (!invert(T, &inv)) return nullptr;
    // the model's first matrix at the fixed attachment on the VR bone; its other matrices (belts) keep their own motion
    Aff placed = mul(shifted(aff(vr->out->data() + slot * 12), vr->origin),
                     st->fixed_have[st->anchor] ? st->fixed[st->anchor] : rel[st->anchor]);
    if (vb0 == 148632 && vr_sword >= 0 && st->anchor == vr_sword + 1) {
        // (Biggoron's Sword in a VR hand: the game's relation holds it at the end of its long hilt; slid so the hand sits
        // kBiggoronGripUp further up it, toward the guard, the user's ask, headset 22 Nov; RX_VR_BIGGORON_GRIP=<m>)
        static float up = -1;
        if (up < 0) { const char* e = vr_research_getenv("RX_VR_BIGGORON_GRIP"); up = e && e[0] ? (float)atof(e) : kBiggoronGripUp; }
        placed = mul(placed, Aff{identity(), V3{0, 0, -up}});
    }
    const Aff delta = mul(placed, inv);
    auto out = std::make_shared<std::vector<float>>(raw.size());
    for (size_t i = 0; i < raw.size() / 12; i++)
        store(out->data() + i * 12, shifted(mul(delta, shifted(aff(raw.data() + i * 12), o)), mul(o, -1.0f)));
    for (float f : *out) if (!std::isfinite(f)) return nullptr;
    if (!shield && !sheath && vr_sword >= 0 && st->anchor == vr_sword + 1) {   // (for its effects: held_sword_effect_move)
        std::lock_guard lk(sword_placed_mu);
        sword_placed_game = T, sword_placed_vr = placed;
        sword_placed_tip = vb0 == 137712 ? .45f : vb0 == 148632 ? 1.55f : 1.09f;   // (broken, Biggoron's, a Master Sword)
        sword_placed_at = std::chrono::steady_clock::now(), sword_placed_vb0 = vb0;
    }
    {   // (which way a held sword is drawn, once each)
        static std::atomic<uint64_t> told{0};
        if (!shield && !sheath && vr_sword >= 0 && told.exchange(vb0) != vb0)
            logf("vr: sword: vb0 %llu held by the %s VR hand on the learned path (anchor %d)", (unsigned long long)vb0,
                 vr_sword ? "right" : "left", st->anchor);
    }
    return out;
}

// A copy of the game's held sword (a model of one matrix sitting on it: the spin's charge glow at stage 2 is drawn so on the
// Master Sword and Biggoron's, behind the player at Link's game hand, headset 22 Nov) moved onto the VR sword by the same
// change as the sword; nullptr when it is not one. Unknown one-matrix models near the game's sword are logged (bounded).
std::shared_ptr<const std::vector<float>> held_sword_copy(uint64_t vb0, const std::vector<float>& raw, V3 o) {
    Aff game, vr;
    {
        std::lock_guard lk(sword_placed_mu);
        if (std::chrono::steady_clock::now() - sword_placed_at > std::chrono::milliseconds(150)) return nullptr;
        game = sword_placed_game, vr = sword_placed_vr;
    }
    if (raw.size() != 12) return nullptr;
    const Aff T = shifted(aff(raw.data()), o);
    const float off = length(sub(T.t, game.t)), turn = aff_change(T, game);
    if (!(off < .05f && turn < .10f)) {
        static std::mutex mu;
        static std::vector<uint64_t> told;
        std::lock_guard lk(mu);
        if (off < .5f && told.size() < 16 && std::find(told.begin(), told.end(), vb0) == told.end())
            told.push_back(vb0), logf("vr: sword: a model vb0 %llu %.2f m from the game's sword (turned %.2f) left as the game "
                                      "draws it", (unsigned long long)vb0, off, turn);
        return nullptr;
    }
    Aff inv;
    if (!invert(game, &inv)) return nullptr;
    auto out = std::make_shared<std::vector<float>>(12);
    store(out->data(), shifted(mul(mul(vr, inv), T), mul(o, -1.0f)));
    for (float f : *out) if (!std::isfinite(f)) return nullptr;
    static std::mutex mu;
    static std::vector<uint64_t> told;
    std::lock_guard lk(mu);
    if (told.size() < 16 && std::find(told.begin(), told.end(), vb0) == told.end())
        told.push_back(vb0), logf("vr: sword: a copy of the game's sword (vb0 %llu) drawn on the VR sword", (unsigned long long)vb0);
    return out;
}
