// Hookshot VR: The exported C bridge used by the game thread to exchange tracking and equipment state.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- the host's calls (host/rx_backend.c) -----------------------------------------------------------------------------------

using namespace mkwvk;

/// (research, headless: RX_VR_HEAD_TEST=1) Without a headset, a still head at the recentred place, level, with the player's
/// headset FOV (109.6 x 111.6 degrees, the "vr: fov asked" log line): the game's camera is built as in the headset (first
/// person when the VR tab's View is), so RX_VR_EYES_TEST's pictures show both eyes as the headset's first person sees them
bool head_test() {
    static const bool on = vr_research_getenv("RX_VR_HEAD_TEST") && atoi(vr_research_getenv("RX_VR_HEAD_TEST")) != 0;
    return on;
}

extern "C" int hookshot_vr_head(HookshotVrHead* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof *out);
    if (!vr::active() && head_test()) {
        static std::atomic<uint32_t> seq{0};
        out->seq = ++seq;
        out->active = 1;
        out->mode = opts.vr_mode;
        out->move_head = opts.vr_move_head;
        out->level = opts.vr_level;
        out->height_cm = opts.vr_height;
        out->body_depth_cm = opts.vr_body_depth;
        out->q[3] = 1.0f;
        out->lean = 1.0f;
        out->tan_half_x = std::tan(109.6f * 0.5f / 57.2958f);
        out->tan_half_y = std::tan(111.6f * 0.5f / 57.2958f);
        out->detail = opts.vr_detail;
        out->foliage = opts.vr_foliage;
        return 1;
    }
    if (!vr::active()) return 0;
    std::lock_guard lk(vr::s.pose_mu);
    if (!vr::s.pose_seq || !vr::s.have_ref) return 0;
    const vr::Pose& p = vr::s.poses[vr::s.pose_seq % vr::kPoses];
    const XrQuaternionf inv = vr::yaw_inverse(vr::s.ref.orientation);
    const XrQuaternionf q = vr::qmul(inv, p.pose.orientation);
    const float dp[3] = {p.pose.position.x - vr::s.ref.position.x, p.pose.position.y - vr::s.ref.position.y,
                         p.pose.position.z - vr::s.ref.position.z};
    float d[3];
    vr::qrot(inv, dp, d);
    out->seq = p.seq;
    out->active = 1;
    out->mode = opts.vr_mode;
    out->move_head = opts.vr_move_head;
    out->level = opts.vr_level;
    out->height_cm = opts.vr_height;
    out->body_depth_cm = opts.vr_body_depth;
    out->q[0] = q.x, out->q[1] = q.y, out->q[2] = q.z, out->q[3] = q.w;
    out->p[0] = d[0], out->p[1] = d[1], out->p[2] = d[2];
    vr::window_head(p.pose, out->q, out->p);   // (Window view: the camera through the frame, vr/headset/window_view.inc)
    out->lean = 1.0f;
    out->tan_half_x = vr::s.fov_tan_x;
    out->tan_half_y = vr::s.fov_tan_y;
    out->detail = opts.vr_detail;
    out->foliage = opts.vr_foliage;
    return 1;
}

extern "C" int hookshot_vr_hands(HookshotVrHands* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof *out);
    if (!vr::active() || !vr::ctl.attached) return 0;
    std::lock_guard lk(vr::s.pose_mu);
    if (!vr::s.have_ref) return 0;
    // (the head's frame of hookshot_vr_head: turned by the recentre's yaw, from the recentred position)
    const XrQuaternionf inv = vr::yaw_inverse(vr::s.ref.orientation);
    out->seq = vr::ctl.seq;
    for (int h = 0; h < 2; h++) {
        const vr::HandState& hs = vr::ctl.shared[h];
        HookshotVrHand& o = out->hand[h];
        o.valid = hs.valid;
        auto put = [&](const XrPosef& p, float q[4], float pos[3]) {
            const XrQuaternionf r = vr::qmul(inv, p.orientation);
            q[0] = r.x, q[1] = r.y, q[2] = r.z, q[3] = r.w;
            const float d[3] = {p.position.x - vr::s.ref.position.x, p.position.y - vr::s.ref.position.y,
                                p.position.z - vr::s.ref.position.z};
            vr::qrot(inv, d, pos);
        };
        put(hs.grip, o.grip_q, o.grip_p);
        put(hs.aim, o.aim_q, o.aim_p);
        vr::qrot(inv, hs.lin_vel, o.lin_vel);
        vr::qrot(inv, hs.ang_vel, o.ang_vel);
        o.trigger = hs.trigger, o.squeeze = hs.squeeze, o.stick[0] = hs.stick[0], o.stick[1] = hs.stick[1];
        o.buttons = hs.buttons;
        vr::panel_mask(h, o.trigger, o.squeeze, o.stick, &o.buttons);   // (the VR menu open: the controllers are its own)
        vr::window_mask(h, o.trigger, o.squeeze, o.stick, &o.buttons);  // (the Window view's Move mode: the same)
    }
    return 1;
}

// The VR menu (vr/menu/vr_menu.inc), from the game side's held left Menu button or Y (vr/input/controllers_host.c): toggle 1 opens
// or closes it, 0 only asks. The answer: open now (1) or not (0)
extern "C" int hookshot_vr_menu(int toggle) {
    if (toggle && !vr::panel_toggle()) logf("vr: VR menu not opened: Hookshot's menu is open");
    return vr::panel_open() ? 1 : 0;
}

// ---- the hookshot (vr/items/hookshot/hookshot_host.c) ---------------------------------------------------------------------
// The controller's line the game's own hookshot aim check follows, in the game world: out = origin xyz, unit direction xyz.
// Along the held hookshot's barrel as drawn (vr/items/hookshot/held_hookshot.inc), from its tip; until it is drawn, along the
// controller's aim pose (OpenXR: its -z, the way a laser pointer points), from 5 cm ahead of it (tracking.inc LinkHand::aim).
extern "C" int hookshot_vr_hook_ray(int hand, float out[6]) {
    vr::LinkBody body;
    if (!out || hand < 0 || hand > 1 || !vr::link_body(body) || !body.hand[hand].valid) return 0;
    if (vr::hook_barrel(out)) {   // (the held hookshot's barrel as drawn: the aim leaves its tip, along it)
        for (int k = 0; k < 3; k++) out[k] += .02f * out[3 + k];
        return 1;
    }
    return hookshot_vr_hand_ray(hand, out);   // (not drawn yet: the controller's pointing line)
}

// A hand's pointing line in the game world, whatever it holds: the controller's aim pose (OpenXR: its -z, the way a laser
// pointer points), from 5 cm ahead of it (tracking.inc LinkHand::aim). out = origin xyz, unit direction xyz. For the bombs'
// throw arc and gravity pull (vr/items/bomb/bomb_host.c) and the hookshot before it is drawn.
extern "C" int hookshot_vr_hand_ray(int hand, float out[6]) {
    vr::LinkBody body;
    if (!out || hand < 0 || hand > 1 || !vr::link_body(body) || !body.hand[hand].valid) return 0;
    const vr::LinkHand& in = body.hand[hand];
    const float n = std::sqrt(in.aim[0] * in.aim[0] + in.aim[1] * in.aim[1] + in.aim[2] * in.aim[2]);
    if (!(n > .5f && n < 2)) return 0;
    for (int k = 0; k < 3; k++) out[3 + k] = in.aim[k] / n, out[k] = in.aim_at[k] + .05f * out[3 + k];
    return 1;
}

// The bombs' marks (vr/items/bomb/bomb_marks.inc), from the game side every game frame: n marks of 5 floats, kind (1 a dot of
// the throw arc, 2 where it lands: Arc to target, parked), where (game world, m), size (m)
extern "C" void hookshot_vr_bomb_marks(const float* marks, int n) { vr::bomb_marks_set(marks, n); }


// The hookshot held in a VR hand in first person, every game frame: the game's own flat screen reticle is hidden then, as
// for the bow (tracking.inc native_bow_reticle_hidden); the game's red laser shows the aim. active: the hand + 1 (0 none):
// the VR hand holds the model then (vr/items/hookshot/held_hookshot.inc)
extern "C" void hookshot_vr_hook_active(int active) {
    if (active > 0) vr::hook_hand_now.store(active <= 2 ? active - 1 : -1, std::memory_order_relaxed);   // (8: out, no hand)
    if (active > 0) vr::hook_seen_at.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());   // (kept after it is put away: tracking.inc hook_out)
    vr::hook_active_at.store(active > 0 ? std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() : 0);
}

// The Comfort Aids' causes now, from the game side every game frame in first person (vr/comfort/comfort_host.c): moving,
// turning, in the air, the game moving Link, each 0 or 1; the vignette's levels are the VR menu's (vr/comfort/vignette_layer.inc)
extern "C" void hookshot_vr_comfort(const float cause[4]) { vr::vignette_set(cause); }

// The VR radial menu's state, from the game side every game frame (vr/menu/radial_host.c): drawn by vr/menu/radial_draw.inc
extern "C" void hookshot_vr_radial(const int32_t* data, int n) { vr::radial_set(data, n); }

// The wrist check HUD's values and block bits, from the game side every game frame (vr/menu/wrist_host.c): drawn by
// vr/menu/wrist_draw.inc when the left wrist is turned to the face
extern "C" void hookshot_vr_wrist(const int32_t* data, int n) {
    vr::wrist_set(data, n);
    vr::wrist_shot();   // (RX_VR_WRIST_SHOT, a test: the panel's picture, headless)
}
// The Bool flags its map pictures are shown by (a later picture of an area), for the game side to read: how many
extern "C" int hookshot_vr_wrist_flags(uint32_t* hashes, int max) { return vr::wrist_flags(hashes, max); }

// (every game frame: 1 the bow is out in first person, 2 the slingshot is: vr/items/slingshot, 0 neither)
extern "C" void hookshot_vr_bow_active(int enabled) {
    if (enabled) vr::bow_is_sling.store(enabled == 2, std::memory_order_relaxed);
    vr::bow_active_at.store(enabled ? std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() : 0);
}

extern "C" int hookshot_vr_bow(float out[12]) {
    vr::LinkBody body;
    return out && vr::link_body(body) && body.bow_equipped && body.hand[0].valid && body.hand[1].valid && vr::bow_solution(out);
}

extern "C" int hookshot_vr_body_heading(float* yaw) {
    return yaw && vr::active() && vr::body_heading(yaw);
}

// Both hands in the game world, for the game side's held items (vr/items/bomb/bomb_host.c): per hand 21 floats, valid (1/0),
// position xyz (m), velocity xyz (m/s), trigger, grip, the grip's axes in the world (x, y, z; 3 floats each: OpenXR's
// grip +x is the way the left palm faces, -x the right's), angular velocity xyz (rad/s). Only with the tracked first-person
// body (link_body). The axes include the saved wrist calibration, as the drawn hand does (arms_and_pose.inc): a bomb seated
// against the palm from the raw axes sat beside the drawn palm and its ball grip stuck the index finger out.
extern "C" int hookshot_vr_hands_world(float out[42]) {
    vr::LinkBody body;
    if (!out || !vr::link_body(body)) return 0;
    for (int h = 0; h < 2; h++) {
        const vr::LinkHand& in = body.hand[h];
        float* o = out + 21 * h;
        o[0] = in.valid ? 1.0f : 0.0f;
        for (int k = 0; k < 3; k++) o[1 + k] = in.position[k], o[4 + k] = in.velocity[k], o[18 + k] = in.angular_velocity[k];
        // basis x calibration (local_euler: roll about z x yaw about y x pitch about x), column by column
        const float d2r = 3.14159265f / 180.0f;
        const float cx = std::cos(body.wrist_deg[h][0] * d2r), sx = std::sin(body.wrist_deg[h][0] * d2r);
        const float cy = std::cos(body.wrist_deg[h][1] * d2r), sy = std::sin(body.wrist_deg[h][1] * d2r);
        const float cz = std::cos(body.wrist_deg[h][2] * d2r), sz = std::sin(body.wrist_deg[h][2] * d2r);
        const float E[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                               {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                               {-sy, cy * sx, cy * cx}};
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++)
                o[9 + 3 * j + k] = in.basis[k] * E[0][j] + in.basis[3 + k] * E[1][j] + in.basis[6 + k] * E[2][j];
        o[7] = in.trigger, o[8] = in.squeeze;
        std::lock_guard lk(vr::s.pose_mu);   // (the VR menu open: the controllers are its own)
        vr::panel_mask(h, o[7], o[8], nullptr, nullptr);
        vr::window_mask(h, o[7], o[8], nullptr, nullptr);
    }
    return 1;
}

// The game side's held items, for the hand's grip (arms_and_pose.inc Grip::ball): a bomb of this radius (m, as drawn) is
// held in this hand (0 left, 1 right); 0 when not. Sent every game frame while it is held, with where the game has its
// centre (world, m): the drawn bomb is found by it even far from the hand (Link's overhead lift at the pull-out).
namespace mkwvk::vr {
std::atomic<float> held_ball[2]{};
std::atomic<int64_t> held_ball_at[2]{};
std::mutex held_ball_mu;
float held_ball_at_game[2][3]{};
bool held_ball_centre(int hand, float out[3]) {
    if (!(held_ball_radius(hand) > 0)) return false;
    std::lock_guard lk(held_ball_mu);
    for (int k = 0; k < 3; k++) out[k] = held_ball_at_game[hand][k];
    return true;
}
float held_ball_radius(int hand) {
    if (hand < 0 || hand > 1) return 0;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - held_ball_at[hand].load(std::memory_order_relaxed) < 250 ? held_ball[hand].load(std::memory_order_relaxed) : 0;
}
}
namespace mkwvk::vr {
// The bombs' state from the game side (hookshot_vr_bomb_state): kept with when it was told, forgotten after 250 ms
std::mutex bomb_state_mu;
float bomb_state[8]{};
int64_t bomb_state_at = 0;
static bool bomb_state_now(float out[8]) {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard lk(bomb_state_mu);
    if (now - bomb_state_at > 250) return false;
    std::memcpy(out, bomb_state, sizeof bomb_state);
    return true;
}
int held_bomb_kind(int hand) {
    float s[8];
    return hand >= 0 && hand <= 1 && bomb_state_now(s) ? (int)s[hand] : 0;
}
bool held_bomb_lifted(int hand) {
    float s[8];
    return hand >= 0 && hand <= 1 && bomb_state_now(s) && s[2 + hand] > .5f;
}
int pull_target(float centre[3]) {
    float s[8];
    if (!bomb_state_now(s) || !(s[4] > .5f) || !std::isfinite(s[5] + s[6] + s[7])) return 0;
    for (int k = 0; k < 3; k++) centre[k] = s[5 + k];
    return (int)s[4];
}
}
// The bombs' state (vr/items/bomb/bomb_host.c, every game frame): per hand what it holds (0 none, 1 a bomb, 2 a Bomb
// Flower), per hand whether Link's lift has it, the bomb the pull would take (kind, centre): equipment_skin.inc bomb_drawn
// A Bomb Flower fruit's body for the game side's hold (vr/items/bomb/bomb_host.c): centre above its origin xyz, radius (m)
extern "C" int hookshot_vr_flower_shape(float out[4]) { return out && vr::flower_shape(out); }

extern "C" void hookshot_vr_bomb_state(const float state[8]) {
    if (!state) return;
    std::lock_guard lk(vr::bomb_state_mu);
    std::memcpy(vr::bomb_state, state, sizeof vr::bomb_state);
    vr::bomb_state_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace mkwvk::vr {
std::atomic<int> sword_hand_now{-1};
std::atomic<int64_t> sword_hand_at{0};
int sword_hand() {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - sword_hand_at.load(std::memory_order_relaxed) < 250 ? sword_hand_now.load(std::memory_order_relaxed) : -1;
}
std::atomic<int> shield_guard_now{0};
std::atomic<int64_t> shield_guard_at{0};
bool shield_guarding() {   // (as sword_hand: forgotten when not told for a quarter of a second)
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - shield_guard_at.load(std::memory_order_relaxed) < 250 && shield_guard_now.load(std::memory_order_relaxed);
}
std::atomic<int> stick_hand_now{-1};
std::atomic<int64_t> stick_hand_at{0};
int stick_hand() {   // (the Deku Stick's hand, as sword_hand: forgotten when not told for a quarter of a second)
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - stick_hand_at.load(std::memory_order_relaxed) < 250 ? stick_hand_now.load(std::memory_order_relaxed) : -1;
}
std::atomic<int> bottle_hand_now{-1};
std::atomic<int64_t> bottle_hand_at{0};
std::atomic<int> bottle_cork_now{-1};
std::atomic<int64_t> bottle_cork_at{0};
int bottle_cork() {   // (as bottle_hand: forgotten when not told for a quarter of a second)
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - bottle_cork_at.load(std::memory_order_relaxed) < 250 ? bottle_cork_now.load(std::memory_order_relaxed) : -1;
}
int bottle_hand() {   // (a bottle's hand, as stick_hand)
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - bottle_hand_at.load(std::memory_order_relaxed) < 250 ? bottle_hand_now.load(std::memory_order_relaxed) : -1;
}
}
namespace mkwvk::vr {
std::atomic<VkImage> hud_images[4]{};
std::atomic<unsigned> hud_next{0};
float hud_scale() {
    static const int test = [] { const char* e = vr_research_getenv("RX_VR_HUD_TEST"); return e ? atoi(e) : 0; }();   // (headless checks)
    if (test > 0) return std::clamp(test, 30, 100) * 0.01f;
    // (a full-screen 2D menu is drawn at the game's 16:9 and shown whole on the world screen: not scaled, menu_169)
    return active() && opts.vr && !menu_169() ? std::clamp(opts.vr_hud_size, 30, 100) * 0.01f : 1.0f;
}
bool hud_presented_recently(VkImage image) {
    for (auto& h : hud_images)
        if (image && h.load(std::memory_order_relaxed) == image) return true;
    return false;
}
void hud_note_presented(VkImage image) {
    if (!image || hud_presented_recently(image)) return;
    hud_images[hud_next.fetch_add(1, std::memory_order_relaxed) % 4].store(image, std::memory_order_relaxed);
}
}
extern "C" void hookshot_vr_sword_hand(int hand) {
    vr::sword_hand_now.store(hand >= -2 && hand <= 1 ? hand : -1, std::memory_order_relaxed);
    vr::sword_hand_at.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}
extern "C" void hookshot_vr_shield_guard(int guarding) {
    vr::shield_guard_now.store(guarding != 0, std::memory_order_relaxed);
    vr::shield_guard_at.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}
extern "C" void hookshot_vr_stick_hand(int hand) {
    vr::stick_hand_now.store(hand >= -2 && hand <= 1 ? hand : -1, std::memory_order_relaxed);
    vr::stick_hand_at.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}
#include "../items/boomerang/boomerang_bridge.inc"   // (the Boomerang's hand and reticle, from vr/items/boomerang)
#include "../items/aim/aim_bridge.inc"   // (the third-person aim's reticle, from vr/items/aim)
// The headset's position in the game world (the first-person body's eye), for the game side's bottle drinking
// (vr/items/bottle/bottle_host.c): 0 while there is no body
extern "C" int hookshot_vr_eye_world(float out[3]) {
    vr::LinkBody body;
    if (!out || !vr::link_body(body) || !(body.eye[0] || body.eye[1] || body.eye[2])) return 0;
    for (int k = 0; k < 3; k++) out[k] = body.eye[k];
    return 1;
}
extern "C" void hookshot_vr_bottle_cork(int corked) {
    vr::bottle_cork_now.store(corked ? 1 : 0, std::memory_order_relaxed);
    vr::bottle_cork_at.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}
extern "C" void hookshot_vr_bottle_hand(int hand) {
    vr::bottle_hand_now.store(hand >= -2 && hand <= 1 ? hand : -1, std::memory_order_relaxed);
    vr::bottle_hand_at.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}
extern "C" void hookshot_vr_hand_holds(int hand, float radius, const float centre[3]) {
    if (hand < 0 || hand > 1 || !(radius >= 0 && radius < 2)) return;
    if (centre && std::isfinite(centre[0] + centre[1] + centre[2])) {
        std::lock_guard lk(vr::held_ball_mu);
        for (int k = 0; k < 3; k++) vr::held_ball_at_game[hand][k] = centre[k];
    }
    vr::held_ball[hand].store(radius, std::memory_order_relaxed);
    vr::held_ball_at[hand].store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}

extern "C" void hookshot_vr_rumble(int hand, float amplitude, float seconds) {
    if (!vr::active() || !vr::ctl.attached || hand < 0 || hand > 1) return;
    vr::haptic_pulse(hand, std::clamp(amplitude, 0.0f, 1.0f), (int64_t)(std::clamp(seconds, 0.0f, 2.0f) * 1e9));
}

/// (host/rx_backend.c host_wide_menus, each present) a full-screen 2D menu is up (vr/headset/bridges_host.c hos_vr_menu_now)
extern "C" void hookshot_vr_menu_screen(int menu) { vr::menu_tick(menu != 0); }
/// (host/rx_backend.c host_wide_menus) VR asked for at start: the menus drawn in the picture's middle 16:9
extern "C" int hookshot_vr_wanted(void) { return vr::wanted() ? 1 : 0; }

extern "C" void hookshot_vr_drawn(uint32_t seq, const float view[12]) {
    if (!seq || !view) {
        vr::bow_active_at.store(0);
        vr::s.camera_owned.store(false, std::memory_order_release);
        return;
    }
    std::lock_guard lk(vr::s.pose_mu);
    vr::Pose& p = vr::s.poses[seq % vr::kPoses];
    if (p.seq != seq) return;
    std::memcpy(p.view, view, sizeof p.view);
    p.drawn = true;
    vr::s.camera_owned.store(true, std::memory_order_release);
}

extern "C" void hookshot_vr_equipment(uint32_t seq, uint32_t known, uint32_t drawn, const uint64_t owners[2]) {
    std::lock_guard lk(vr::s.pose_mu);
    vr::s.equipment_seq = seq;
    vr::s.equipment_known = known & 3;
    vr::s.equipment_drawn = drawn & known & 3;
    for (int k = 0; k < 2; k++) vr::s.equipment_owner[k] = owners ? owners[k] : 0;
    // (BODY-11: kept by pose number too, for the frame drawn with that pose: link_body_for_frame)
    vr::EquipmentSample& e = vr::s.equipment[++vr::s.equipment_gen % vr::kBodies];
    e.gen = vr::s.equipment_gen, e.seq = seq, e.known = vr::s.equipment_known, e.drawn = vr::s.equipment_drawn;
    for (int k = 0; k < 2; k++) e.owner[k] = vr::s.equipment_owner[k];
}

/// (vr/camera/crawl_host.c, every first-person camera update just before hookshot_vr_body) how far into a crawl Link is and
/// how far the view came down for it: kept for the body told next with the same pose
extern "C" void hookshot_vr_crawl(uint32_t seq, float amount, float lowered) {
    std::lock_guard lk(vr::s.pose_mu);
    vr::s.crawl_seq = seq;
    vr::s.crawl_amount = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
    vr::s.crawl_lowered = std::isfinite(lowered) ? std::clamp(lowered, 0.0f, 2.0f) : 0.0f;
}

extern "C" void hookshot_vr_body(uint32_t seq, int active, int child, const float root[3],
                                  const float right[3], const float back[3]) {
    std::lock_guard lk(vr::s.pose_mu);
    vr::s.body_seq = seq;
    vr::s.body_active = active != 0 && root != nullptr && right != nullptr && back != nullptr;
    vr::s.body_child = child != 0;
    if (vr::s.body_active) {
        std::memcpy(vr::s.body_root, root, sizeof vr::s.body_root);
        std::memcpy(vr::s.body_right, right, sizeof vr::s.body_right);
        std::memcpy(vr::s.body_back, back, sizeof vr::s.body_back);
    }
    {   // (BODY-11) every camera update's body, in order, also the frames without one (so the order stays one a frame)
        const uint64_t gen = ++vr::s.body_gen;
        vr::BodySample& b = vr::s.bodies[gen % vr::kBodies];
        b = vr::BodySample{};
        b.seq = seq, b.gen = gen, b.active = vr::s.body_active, b.child = vr::s.body_child;
        // (the equipment the same camera update told, just before: not searched for by pose number later, which a pose
        // used for several game frames (a stalled headset frame) would match in the wrong one)
        if (vr::s.equipment_gen && vr::s.equipment_seq == seq) b.equipment = vr::s.equipment_gen;
        if (vr::s.crawl_seq == seq) b.crawl = vr::s.crawl_amount, b.lowered = vr::s.crawl_lowered;   // (the same way)
        if (b.active) {
            std::memcpy(b.root, root, sizeof b.root);
            std::memcpy(b.right, right, sizeof b.right);
            std::memcpy(b.back, back, sizeof b.back);
        }
    }
    if (vr::s.body_active) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            logf("vr: tracked Link body source active (%s, root %.3f %.3f %.3f)", vr::s.body_child ? "child" : "adult",
                 vr::s.body_root[0], vr::s.body_root[1], vr::s.body_root[2]);
    }
}
#include "../items/ocarina/ocarina_bridge.inc"   // (the ocarina: its holding hand and its place on the VR belt)
#include "../items/item_get/item_get_bridge.inc"   // (an item get plays: the item in front of the headset)
