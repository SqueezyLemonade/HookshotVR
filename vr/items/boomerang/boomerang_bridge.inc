// Hookshot VR: the Boomerang's state on the renderer side, as the game side tells it (vr/items/boomerang/boomerang_host.c,
// through rx_vr_boomerang): which VR hand holds its model (vr/items/boomerang/held_boomerang.inc, the fingers in
// vr/body/arms_and_pose.inc) and where its reticle is while it aims (drawn by vr/headset/frame_loop.inc, the bow's).
//
// A piece of vr/headset/host_api.inc, included there: it uses that file's namespace and includes. Not compiled on its own.
namespace mkwvk::vr {
std::atomic<int> boomerang_hand_now{-1};
std::atomic<int64_t> boomerang_hand_at{0};
std::mutex boomerang_mu;
float boomerang_at[3]{};
int64_t boomerang_reticle_at = 0;
int64_t boomerang_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int boomerang_hand() {   // (as stick_hand: forgotten when not told for a quarter of a second)
    return boomerang_now_ms() - boomerang_hand_at.load(std::memory_order_relaxed) < 250
               ? boomerang_hand_now.load(std::memory_order_relaxed) : -1;
}
bool boomerang_reticle(float at[3]) {
    std::lock_guard lk(boomerang_mu);
    if (boomerang_now_ms() - boomerang_reticle_at > 150) return false;
    std::memcpy(at, boomerang_at, sizeof boomerang_at);
    return true;
}
}
// hand: 0 left, 1 right, -1 none, -2 hidden (being put away); reticle: at (the game world) is where it shows
extern "C" void hookshot_vr_boomerang(int hand, int reticle, const float at[3]) {
    vr::boomerang_hand_now.store(hand >= -2 && hand <= 1 ? hand : -1, std::memory_order_relaxed);
    vr::boomerang_hand_at.store(vr::boomerang_now_ms(), std::memory_order_relaxed);
    if (reticle && at && std::isfinite(at[0] + at[1] + at[2])) {
        std::lock_guard lk(vr::boomerang_mu);
        std::memcpy(vr::boomerang_at, at, sizeof vr::boomerang_at);
        vr::boomerang_reticle_at = vr::boomerang_now_ms();
    }
}
