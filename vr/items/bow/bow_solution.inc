// Hookshot VR: the frame's bow solution, for the game side (the arrow's launch).
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
bool bow_solution(float out[12]) {
    std::lock_guard lk(bow_solution_mu);
    const int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!latest_bow_at || now-latest_bow_at>100) return false;
    std::memcpy(out,latest_bow,sizeof latest_bow);
    return true;
}
