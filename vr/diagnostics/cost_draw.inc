// Hookshot VR: Research (RX_VR_PROG_COST=1): the replay's CPU time by program, the 30 costliest every 600
// frames. To be removed once the frame-rate work is done.
//
// A piece of gpu/vk/vk_draw.cpp (the draw replay), included there at its VR HOOK: it uses that file's types and helpers
// (Targets, NvnState, Program, stream_copy, logf, ...). Not compiled on its own.
// (OoT VR, research: RX_VR_PROG_COST=1) the replay's CPU time by program, the 30 that cost the most logged every 600 frames:
// what in an area (foliage, fences, fog, water) costs the frame rate
#ifdef HOOKSHOT_VR_RESEARCH   // (research builds only: a player's build has the stubs at the end)
thread_local uint64_t vr_cost_key = 0;
thread_local uint32_t vr_cost_w = 0, vr_cost_h = 0;
thread_local bool vr_cost_split = false;   // (the draw or dispatch was split for the eyes)
bool vr_cost_on() { static const bool on = vr_research_on("RX_VR_PROG_COST"); return on; }
void vr_cost_note(uint64_t key, double ms, const DrawArgs* d, bool compute, uint32_t w, uint32_t h, bool split) {
    struct Cost { double ms = 0; int64_t n = 0, count = 0, inst = 0, split = 0; bool mesh = false, indirect = false, compute = false; uint32_t w = 0, h = 0; };
    static std::mutex mu;
    static std::unordered_map<uint64_t, Cost> costs;
    static uint64_t from = 0;
    std::lock_guard lk(mu);
    Cost& c = costs[key];
    c.ms += ms, c.n++, c.split += split, c.compute = compute, c.w = w, c.h = h;
    if (d) c.count += d->count, c.inst += d->instances, c.mesh = d->mesh, c.indirect = d->indirect != 0;
    const uint64_t now = texture_frame_no();
    if (!from) from = now;
    if (now - from < 600) return;
    const double frames = (double)(now - from);
    std::vector<std::pair<uint64_t, Cost>> v(costs.begin(), costs.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double all = 0;
    int64_t draws = 0;
    for (const auto& e : v) all += e.second.ms, draws += e.second.n;
    logf("vr: cost: %.2f ms a frame of replayed draws and dispatches, %.0f a frame, %zu programs; the 30 that cost the most:", all / frames,
         draws / frames, v.size());
    for (size_t i = 0; i < v.size() && i < 30; i++) {
        const Cost& c = v[i].second;
        logf("vr: cost:  %5.2f ms %4.1f%%  %6.1f a frame  prog %016llx  %s%s%s count %lld x%lld inst  %ux%u%s", c.ms / frames,
             100.0 * c.ms / all, c.n / frames, (unsigned long long)v[i].first, c.compute ? "dispatch" : c.mesh ? "mesh" : "draw",
             c.indirect ? " indirect" : "", c.split ? " (split for the eyes)" : "", (long long)(c.n ? c.count / c.n : 0),
             (long long)(c.n ? c.inst / c.n : 0), c.w, c.h, v[i].first ? "" : " (left out before its program: skipped)");
    }
    costs.clear();
    from = now;
}
#else
thread_local uint64_t vr_cost_key = 0;
thread_local uint32_t vr_cost_w = 0, vr_cost_h = 0;
thread_local bool vr_cost_split = false;
constexpr bool vr_cost_on() { return false; }
inline void vr_cost_note(uint64_t, double, const DrawArgs*, bool, uint32_t, uint32_t, bool) {}
#endif
