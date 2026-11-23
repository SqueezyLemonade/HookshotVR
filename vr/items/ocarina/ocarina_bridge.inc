// Hookshot VR: the ocarina's two messages between the game side and the renderer (first person, VR tab: Ocarina in the hands).
//
// A piece of vr/headset/host_api.inc (so of vr/headset/vk_vr.cpp), included there with the other held items' bridges. Not
// compiled on its own.
// - The game side tells which VR hand holds the ocarina (vr/items/ocarina/ocarina_host.c, through rx_vr_ocarina_hand in
//   vr/items/ocarina/ocarina_bridges_host.c): 0 left, 1 right, -1 on the belt. Told when it changes and every 0.1 s; forgotten
//   (back on the belt) when not told for a quarter of a second.
// - The renderer tells where it drew the ocarina on the VR belt (vr/items/ocarina/held_ocarina.inc), in the game world, so
//   a grip there takes it: fresh for a quarter of a second.
// - The game side tells when a song it played is being played by the game (its effect on screen), for the VR tab's Song
//   effects (vr/items/ocarina/song_effects.inc): told every 0.1 s, forgotten after a quarter of a second.
namespace mkwvk::vr {
std::atomic<int> ocarina_hand_now{-1};
std::atomic<int64_t> ocarina_hand_at{0};
std::mutex ocarina_belt_mu;
float ocarina_belt_at[3]{};
int64_t ocarina_belt_when = 0;
static int64_t ocarina_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<int> ocarina_song_now{0};
std::atomic<int64_t> ocarina_song_at{0};
bool ocarina_song() {
    return ocarina_now_ms() - ocarina_song_at.load(std::memory_order_relaxed) < 250 && ocarina_song_now.load(std::memory_order_relaxed);
}
int ocarina_hand() {
    return ocarina_now_ms() - ocarina_hand_at.load(std::memory_order_relaxed) < 250 ? ocarina_hand_now.load(std::memory_order_relaxed) : -1;
}
void ocarina_belt_drawn(const float at[3]) {
    std::lock_guard lk(ocarina_belt_mu);
    for (int k = 0; k < 3; k++) ocarina_belt_at[k] = at[k];
    ocarina_belt_when = ocarina_now_ms();
}
}
extern "C" void hookshot_vr_ocarina_hand(int hand) {
    vr::ocarina_hand_now.store(hand == 0 || hand == 1 ? hand : -1, std::memory_order_relaxed);
    vr::ocarina_hand_at.store(vr::ocarina_now_ms(), std::memory_order_relaxed);
}
extern "C" int hookshot_vr_ocarina_belt(float out[3]) {
    if (!out) return 0;
    std::lock_guard lk(vr::ocarina_belt_mu);
    if (vr::ocarina_now_ms() - vr::ocarina_belt_when >= 250) return 0;
    for (int k = 0; k < 3; k++) out[k] = vr::ocarina_belt_at[k];
    return 1;
}
extern "C" void hookshot_vr_ocarina_song(int playing) {
    vr::ocarina_song_now.store(playing ? 1 : 0, std::memory_order_relaxed);
    vr::ocarina_song_at.store(vr::ocarina_now_ms(), std::memory_order_relaxed);
}
