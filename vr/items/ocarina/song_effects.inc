// Hookshot VR: a song's effects, first person (VR tab: Song effects, with Ocarina in the hands).
//
// A piece of vr/body/skin_draw.inc (so of gpu/vk/vk_draw.cpp, the draw replay), included there before vr_link_skin_pick,
// which asks it about every draw. It uses vr_vertex_reads and vr_read_as_drawn from there. Not compiled on its own.
//
// When a song played with the VR ocarina succeeds, the game plays its effect inside its ocarina event (Dm_SY_Ocarina's
// song sub-flows: Saria's Song swirls green leaves round Link, filling the view from its front camera; headless 18 Nov).
// In first person they are round the invisible game Link, so round the player's head. Shown (the default) keeps them as
// the game draws them. Hidden leaves out, while the game side says a song plays (vr::ocarina_song: the ocarina's event
// goes on with no note wait and no song menu, vr/items/ocarina/ocarina_host.c), every effect draw (its vertex shader
// reads the particle camera, UBO 10, and the effect matrix, UBO 11, as the held arrow's effects do) whose effect sits
// within kSongEffectReach of the eye. Other effects that near (a torch beside Link) are left out for those few seconds too.
constexpr float kSongEffectReach = 4.0f;   // (m from the eye)
static bool vr_song_effect_hidden(NvnState& s, const Program& p) {
    if (opts.vr_song_effects || !opts.vr_ocarina || !vr::ocarina_song()) return false;
    if (!vr_vertex_reads(p, 10 + 3) || !vr_vertex_reads(p, 11 + 3)) return false;
    const nvn_p_bind_buf& u = s.ubo[0][11];
    const nvn_p_bind_buf& cam = s.ubo[0][10];
    if (!u.addr || u.size < 64 || u.size > 0x10000 || !cam.addr || cam.size < 0xc0 || cam.size > 0x10000) return false;
    float m[16], c[0x200 / 4];
    const uint64_t cam_bytes = std::min<uint64_t>(cam.size, sizeof c);
    if (!vr_read_as_drawn(u.addr, m, sizeof m) || !vr_read_as_drawn(cam.addr, c, cam_bytes)) return false;
    vr::arrow_fx::Camera info;
    if (!vr::arrow_fx::inspect_camera(c, cam_bytes, info)) return false;
    const float d[3] = {m[12] - info.eye[0], m[13] - info.eye[1], m[14] - info.eye[2]};
    if (!(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < kSongEffectReach * kSongEffectReach)) return false;
    static std::atomic<int> told{0};
    if (told.fetch_add(1) % 2000 == 0)
        logf("vr: ocarina: a song's effect draw left out (VR tab Song effects: Hidden; %d so far)", told.load());
    return true;
}
