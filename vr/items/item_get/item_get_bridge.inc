// Hookshot VR: the game side says when an item get plays (vr/items/item_get/item_get_host.c), for the renderer's
// vr/items/item_get/item_get.inc. Told every 0.1 s, forgotten after a quarter of a second. Back to the game side: an item
// was seen in the last quarter of a second, drawn in front of you or over Link's head with Item shown off (the view's
// heading keeps out of Link's turn, vr/camera/view_host.c).
//
// A piece of vr/headset/host_api.inc (so of vr/headset/vk_vr.cpp), included there with the held items' bridges. Not
// compiled on its own.
namespace mkwvk::vr {
std::atomic<int> item_get_now{0};
std::atomic<int64_t> item_get_at{0};
std::atomic<int64_t> item_get_drawn_at{0};
std::atomic<unsigned> item_get_count{0};   // (item gets begun: a model let go is let go until the next)
static int64_t item_get_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool item_get_live() {
    return item_get_now_ms() - item_get_at.load(std::memory_order_relaxed) < 250 && item_get_now.load(std::memory_order_relaxed);
}
unsigned item_get_number() { return item_get_count.load(std::memory_order_relaxed); }
void item_get_drawn() { item_get_drawn_at.store(item_get_now_ms(), std::memory_order_relaxed); }
}
extern "C" void hookshot_vr_item_get(int playing) {
    if (playing && !vr::item_get_live()) vr::item_get_count.fetch_add(1, std::memory_order_relaxed);
    vr::item_get_now.store(playing ? 1 : 0, std::memory_order_relaxed);
    vr::item_get_at.store(vr::item_get_now_ms(), std::memory_order_relaxed);
}
extern "C" int hookshot_vr_item_get_shown(void) {
    return vr::item_get_live() && vr::item_get_now_ms() - vr::item_get_drawn_at.load(std::memory_order_relaxed) < 250;
}
