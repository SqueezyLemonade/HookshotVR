// (OoT VR, true stereo) The both-eyes check: a routed draw (drawn one eye after the other, vr_split 1 then 2) whose second
// half's pipeline is cold is left out whole at its first half, when the draw may be left out, instead of drawing the left
// eye and then blocking the render thread on the right eye's build (the second half may never be left out alone: STE-03's
// ghost). On the test build the second halves were 89 of a second launch's 94 blocking builds and 541 of a first launch's
// 763 (the pipeline-key stability research). RX_VR_BOTH_EYES=0: off, as before.
//
// The second half's entry: the one that half took the last time this first half's entry was drawn (learned at the second
// half, exact), else the first half's state with view mask 2 (predicted: a routed draw's halves differ by their view mask, and
// by what vr_eyes_inputs and the camera rules decide again for the second; a wrong prediction makes one entry no draw uses and
// costs one block, as before). A learned entry that the prediction did not match is only checked: its state is not the one
// here, so nothing is built for it from here (its own build was queued when its draw made it).
// Included by vk_draw.cpp before draw_inner (after quick_pipeline); the hooks are at the first half's readiness check and at the second's.

struct VrPartner {
    PipeEntry* second = nullptr;
    bool exact = false;   // (the prediction from the first half's state is the entry the second half took)
};
thread_local FlatMap<uint64_t, VrPartner> vr_partner;   // (first half's entry -> its second half's; entries live as long as the cache)
thread_local PipeEntry* vr_half1_pe = nullptr;          // (this draw's first half, drawn: its second half learns the pair)
thread_local PipeEntry* vr_half1_pred = nullptr;        // (the entry predicted for its second half, or null)
std::atomic<int64_t> vr_be_checked{0}, vr_be_learned{0}, vr_be_predicted{0}, vr_be_cold{0}, vr_be_from_caches{0}, vr_be_out{0};
std::atomic<int64_t> vr_be_exact{0}, vr_be_inexact{0};

/// The first half's targets as the second half has them: view mask 2, and no shared sampling (the second half is decided with
/// want_shared off, vr_eyes_inputs, so its signature and bindings stay 0 where a first half's draw that shares its inputs has them)
inline Targets vr_second_half(const Targets& at) {
    Targets two = at;
    two.eyes_mask = 2;
    two.eyes_sampling = 0;
    for (uint64_t& b : two.eyes_sample_bindings) b = 0;
    return two;
}
thread_local uint64_t vr_half1_pred_key = 0;   // (research: the predicted second half's key, for the log of a wrong one)
inline bool vr_both_eyes_on() {
    static const bool on = [] { const char* e = getenv("RX_VR_BOTH_EYES"); return !(e && env_value_off(e)); }();
    return on;
}
void vr_both_eyes_log() {
    logf("vr: both eyes: %lld first halves checked (%lld by the learned pair, %lld predicted); second half cold %lld: %lld then "
         "from the caches, %lld left out with their first; pairs learned exact %lld, other than predicted %lld",
         (long long)vr_be_checked.load(), (long long)vr_be_learned.load(), (long long)vr_be_predicted.load(),
         (long long)vr_be_cold.load(), (long long)vr_be_from_caches.load(), (long long)vr_be_out.load(),
         (long long)vr_be_exact.load(), (long long)vr_be_inexact.load());
}
/// (the first half, its own pipeline there to draw with, the draw one that may be left out) whether the second half's is too
bool vr_both_eyes_ready(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, const uint8_t types[8],
                        PipeEntry* first) {
    vr_half1_pe = first, vr_half1_pred = nullptr;
    vr_be_checked++;
    PipeEntry* second;
    bool exact = true;
    if (const auto it = vr_partner.find((uint64_t)(uintptr_t)first); it != vr_partner.end()) {
        second = it->second.second, exact = it->second.exact;
        vr_be_learned++;
    } else {
        const Targets two = vr_second_half(at);
        second = vr_half1_pred = graphics_pipeline(p, s, two, topo, types);
        vr_half1_pred_key = pipeline_key(p, s, two, topo);
        vr_be_predicted++;
    }
    if (second->ready.load(std::memory_order_acquire) || second->pipeline.load(std::memory_order_acquire)) return true;
    vr_be_cold++;
    if (exact) {   // (from the caches, or quick within the frame's budget for draws that may be left out: as the first half's)
        const Targets two = vr_second_half(at);
        if (vr_pipe_log_on()) vr_pipe_why = 5;   // (research, RX_VR_PIPE_LOG: a build of this check's)
        const bool got = quick_pipeline(*second, p, s, two, topo, false);
        vr_pipe_why = -1;
        if (got) return vr_be_from_caches++, true;
    }
    if (const int64_t n = ++vr_be_out; n <= 3 || n % 500 == 0) vr_both_eyes_log();
    return false;
}
/// (the second half, its entry taken) the pair learned for the next time
inline void vr_both_eyes_learn(PipeEntry* second, uint64_t key) {
    if (!vr_half1_pe) return;
    VrPartner& v = vr_partner[(uint64_t)(uintptr_t)vr_half1_pe];
    if (v.second != second) {
        v.second = second;
        v.exact = !vr_half1_pred || vr_half1_pred == second;
        if (vr_half1_pred) (v.exact ? vr_be_exact : vr_be_inexact)++;
        static std::atomic<int> told{0};
        if (vr_half1_pred && !v.exact && told++ < 40)   // (research: with RX_VR_PIPE_LOG the two keys' state lines say how)
            logf("vr: both eyes: the second half took %016llx, predicted %016llx", (unsigned long long)key,
                 (unsigned long long)vr_half1_pred_key);
    }
    vr_half1_pe = nullptr;
}

// (research, RX_VR_ONE_EYE_LOG=1) the ghost check: a routed draw whose first half reached its pipeline and descriptors and
// whose second did not (or the reverse), counted, with the frames it happened in
thread_local bool vr_half_recorded = false;
std::atomic<int64_t> vr_one_eye_draws{0}, vr_one_eye_frames{0}, vr_two_eye_draws{0};
std::atomic<uint64_t> vr_one_eye_last_frame{~0ull}, vr_one_eye_report{0};
inline bool vr_one_eye_on() {
    static const bool on = vr_research_on("RX_VR_ONE_EYE_LOG");
    return on;
}
void vr_one_eye_note(bool first, bool second, uint64_t prog) {
    const uint64_t frame = texture_frame_no();
    if (first != second) {
        const int64_t n = ++vr_one_eye_draws;
        if (vr_one_eye_last_frame.exchange(frame) != frame) vr_one_eye_frames++;
        if (n <= 20 || n % 200 == 0)
            logf("vr: one eye: program %016llx drawn in the %s eye only (frame %llu; %lld such draws, %lld frames)",
                 (unsigned long long)prog, first ? "left" : "right", (unsigned long long)frame, (long long)n,
                 (long long)vr_one_eye_frames.load());
    } else if (first) {
        vr_two_eye_draws++;
    }
    uint64_t r = vr_one_eye_report.load(std::memory_order_relaxed);
    if (frame >= r + 1200 && vr_one_eye_report.compare_exchange_strong(r, frame)) {
        logf("vr: one eye check: %lld routed draws drawn in one eye only, in %lld frames; %lld drawn in both (frame %llu)",
             (long long)vr_one_eye_draws.load(), (long long)vr_one_eye_frames.load(), (long long)vr_two_eye_draws.load(),
             (unsigned long long)frame);
        if (vr_be_checked.load()) vr_both_eyes_log();
    }
}
