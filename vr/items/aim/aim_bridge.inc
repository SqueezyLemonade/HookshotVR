// Hookshot VR: the third-person aim's reticle on the renderer side, as the game side tells it
// (vr/items/aim/third_person_aim_host.c, through rx_vr_aim_mark): where the shot will land in the game world while an item
// aims in third person, drawn by vr/items/aim/third_person_reticle.inc.
//
// A piece of vr/headset/host_api.inc, included there: it uses that file's namespace and includes. Not compiled on its own.
namespace mkwvk::vr {
std::mutex aim_mark_mu;
float aim_mark_at[3]{};
int aim_mark_kind = 0;
int64_t aim_mark_told = 0;
int64_t aim_mark_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int third_person_aim(float at[3]) {   // (forgotten 150 ms after the game side last told: it tells every game frame)
    std::lock_guard lk(aim_mark_mu);
    if (!aim_mark_kind || aim_mark_now_ms() - aim_mark_told > 150) return 0;
    std::memcpy(at, aim_mark_at, sizeof aim_mark_at);
    return aim_mark_kind;
}
}
// kind: 0 none, 1 the reticle at at (the game world)
extern "C" void hookshot_vr_aim_mark(int kind, const float at[3]) {
    std::lock_guard lk(vr::aim_mark_mu);
    const bool ok = kind > 0 && at && std::isfinite(at[0] + at[1] + at[2]);
    vr::aim_mark_kind = ok ? kind : 0;
    if (ok) std::memcpy(vr::aim_mark_at, at, sizeof vr::aim_mark_at);
    vr::aim_mark_told = vr::aim_mark_now_ms();
}
