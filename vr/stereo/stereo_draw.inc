// Hookshot VR: True stereo in the draw replay: which passes draw both eyes (vr_eyes_mark), the draw split
// per eye (vr_split), and the per-eye camera copies for full-screen passes (vr_cam_eye, RX_VR_EYES_DEBUG 64).
//
// A piece of gpu/vk/vk_draw.cpp (the draw replay), included there at its VR HOOK: it uses that file's types and helpers
// (Targets, NvnState, Program, stream_copy, logf, ...). Not compiled on its own.
/// (research, RX_VR_EYES_CAM_LOG=1, STE-30) A pass in a two-eye frame that draws one view while one of its targets has the
/// right eye's layer: that layer is not drawn, and the right eye later samples what was left in it (an object white in the
/// right eye only, for a whole run, in some runs: whether a texture got its second layer depends on its first use). Its
/// targets, once per first target (64 at most)
void vr_eyes_half_note(const Targets& at) {
    static const bool on = vr_research_getenv("RX_VR_EYES_CAM_LOG") && atoi(vr_research_getenv("RX_VR_EYES_CAM_LOG")) != 0;
    if (!on) return;
    bool any = at.depth && at.depth->vr_eyes;
    for (int i = 0; i < at.n; i++) any |= at.color[i] && at.color[i]->vr_eyes;
    if (!any) return;
    static std::mutex mu;
    static std::unordered_set<uint32_t> seen;
    const Texture* first = at.n && at.color[0] ? at.color[0] : at.depth;
    std::lock_guard lk(mu);
    if (!first || seen.size() >= 64 || !seen.insert(first->id).second) return;
    char line[600];
    int n = snprintf(line, sizeof line, "vr: true stereo: a pass drawn one view into a two-layer target (%ux%u, %d samples):", at.width,
                     at.height, (int)at.samples);
    auto one = [&](const char* what, const Texture* t, uint32_t layer) {
        if (t && n < (int)sizeof line - 96)
            n += snprintf(line + n, sizeof line - n, " %s %#llx %ux%u x%u vk %d%s%s;", what, (unsigned long long)t->gpu, t->width,
                          t->height, t->layers,
                          (int)t->format, t->vr_eyes ? " two-layer" : " one layer", layer ? " (a layer of it)" : "");
    };
    for (int i = 0; i < at.n; i++) one("colour", at.color[i], at.color_layer[i]);
    one("depth", at.depth, at.depth_layer);
    logf("%s", line);
}
/// (OoT VR, true stereo) Whether a pass into these targets draws both eyes: all of them made with the second layer
/// (vk_vr_eyes.cpp), the frame drawing both eyes. Kept for the targets' binding (serial) while the eyes' shift is the same
Targets vr_eyes_mark(Targets at, uint64_t serial) {
    at.eyes = 0;
    if (!vr::eyes_frame()) return at;
    static thread_local uint64_t mark_serial = ~0ull, mark_key = 0;
    static thread_local uint8_t mark_eyes = 0;
    const uint64_t key = vr::eyes_key();
    if (serial == mark_serial && key == mark_key) return at.eyes = mark_eyes, at;
    bool ok = (at.n || at.depth) && at.samples == VK_SAMPLE_COUNT_1_BIT;
    for (int i = 0; i < at.n && ok; i++)
        if (at.color_view[i] && (!at.color[i] || !at.color[i]->vr_eyes || at.color_layer[i] ||
                                 !vr::eyes_view(at.color[i], at.color_format[i], at.color_level[i])))
            ok = false;
    if (ok && at.depth && (!at.depth->vr_eyes || at.depth_layer || !vr::eyes_view(at.depth, at.depth_format, at.depth_level)))
        ok = false;
    mark_serial = serial, mark_key = key, mark_eyes = ok ? 1 : 0;
    if (!ok) vr_eyes_half_note(at);   // (research, STE-30: a two-layer target drawn one view: its right-eye layer not drawn)
    at.eyes = mark_eyes;
    return at;
}
/// (OoT VR, true stereo) what a pipeline made for both eyes differs by (0: one view)
thread_local uint64_t vr_array_sampling = 0;
inline uint32_t vr_eyes_mask(const Targets& at) { return at.eyes ? at.eyes_mask : 0u; }
inline uint64_t vr_eyes_salt(const Targets& at) {
    return at.eyes ? (vr::eyes_key() ^ (uint64_t)at.eyes * 0x5e7e5ull ^ (uint64_t)vr_eyes_mask(at) << 48 ^ at.eyes_sampling ^
                      at.eyes_cam * 0x9e3779b97f4a7c15ull) : 0;   // (eyes_cam: STE-31's fragment patch)
}
/// (OoT VR, true stereo) A full-screen draw into both eyes' targets (the lighting, fog and composites of a deferred scene) reads
/// textures the eyes' passes drew in both layers: drawn once per layer as one view (draw), the second time with layer 1's views of
/// its targets and of the two-layer textures it samples. vr_split: 0 a draw; 2 the second, layer 1's
thread_local int vr_split = 0;
thread_local bool vr_split_asked = false;
thread_local bool vr_route_inputs = true;   // (OoT VR: the routed draw's first half read per-eye textures)
/// (OoT VR, true stereo, STE-28/29) The eye a pass is drawn or dispatched for, whose copies of the camera its buffers get
/// (vk_draw.cpp uniform_range, vk_vr_eyes.cpp eyes_move_camera_copies): 0 none, 1 the left eye's copies, 2 the right's
thread_local int vr_cam_eye = 0;
/// (OoT VR, research: the ghost) RX_VR_SKIP_PROG=<hex>,<hex>...: those programs' draws and dispatches left out, to find the pass
bool vr_dbg_skipped(uint64_t key) {
    static const std::vector<uint64_t> keys = [] {
        std::vector<uint64_t> k;
        if (const char* e = vr_research_getenv("RX_VR_SKIP_PROG"))
            for (const char* q = e; *q;) {
                char* end = nullptr;
                const uint64_t v = strtoull(q, &end, 16);
                if (end == q) break;
                k.push_back(v);
                q = *end ? end + 1 : end;
            }
        return k;
    }();
    for (const uint64_t k : keys)
        if (k == key) return true;
    return false;
}
thread_local uint32_t vr_cam_w = 0, vr_cam_h = 0;
/// (research, RX_VR_EYES_CAM_LOG) the program being drawn or dispatched (kind 1 a draw, 2 a dispatch) and its targets' size
thread_local uint64_t vr_log_prog = 0;
thread_local int vr_log_kind = 0;
thread_local uint32_t vr_log_w = 0, vr_log_h = 0;
/// whether any stage of a draw reads a buffer laid out as the scene's camera, by the camera's own size field (a quarter-size
/// lighting pass reads the full-size camera). Stages 0 and 1 only, before STE-28: the lighting passes read it in their fragment
/// stage, so RX_VR_EYES_DEBUG 64 never gave them their eye's copy
bool vr_reads_camera(const NvnState& s) {
    for (int st = 0; st < kNvnStages; st++)
        for (const nvn_p_bind_buf& u : s.ubo[st])
            if (u.addr && u.size >= 0x230 && vr::eyes_scene_camera(u.addr, u.size)) return true;
    return false;
}
/// (STE-28) a draw moved for each eye (its vertex stage shifted) gets its eye's camera copy in its other stages only: the
/// copy's view-projection in the vertex stage would move it a second time
thread_local bool vr_cam_frag_only = false;
/// (STE-28) the lighting and shadow passes rebuild each pixel's place in the world from the depth with the camera: with the
/// middle camera, each eye's place was ~3 cm off sideways, opposite ways, and the shadow map was read there. A soft soil mound
/// got a hatched shadow on one side in one eye and the other side in the other; small rocks two shadows; Link's shadow one
/// each side of him (headset 20 Nov; From depth showed none). Now the full-screen passes and the light boxes that read the
/// camera are drawn one eye after the other, each with its eye's copy, and the per-eye compute passes get it too. The VR
/// menu's Debug row Shadows per eye; RX_VR_EYES_DEBUG 2048: the middle camera, as before
bool vr_eyes_camera_per_eye() {
    return opts.vr_eyes_camera && !vr::eyes_debug(2048);
}
/// (STE-31) whether a draw's fragment stage reads a buffer that holds copies of the camera (vk_vr_eyes.cpp
/// eyes_camera_copies_learn). STE-29 gave each eye its copies in the full-screen passes and light boxes only, so geometry
/// that lights itself as it is drawn (characters, foliage, small props, enemies) still worked out its shadows and light from
/// the middle of the head in both eyes: the terrain's shadows came right in the headset, theirs stayed mirrored per eye
/// (Death Mountain Trail, 20 Nov). Headless, 15 half-size programs read the scene camera in their fragment stage in one draw
/// for both eyes. A draw drawn one eye after the other anyway now gets that eye's copies in its fragment stage. Learned once
/// per program, kept per thread: no lock on the draw path once known. RX_VR_EYES_DEBUG 4096: not for these draws
/// (and its signature, vr_frag_camera_signature: what its fragment patch is keyed by, option (a))
/// (STE-33: decided again once when the shadow-cascade check decides: a program drawn once for both eyes, first drawn before
/// the check passed, got its cascades neither in its copies nor in its patched shader, and kept mirrored shadows all session)
uint64_t vr_frag_camera_signature(const Program& p, const NvnState& s) {
    if (vr::eyes_debug(4096)) return 0;
    struct Known { uint64_t signature; int cascades; };
    thread_local std::unordered_map<uint64_t, Known> known;
    const int cascades = vr::eyes_cascades_state();
    if (auto it = known.find(p.key); it != known.end() && (it->second.cascades == cascades || !it->second.signature))
        return it->second.signature;
    bool copies = false, decided = true;
    for (const Stage& st : p.stages) {
        if (!(st.vk_stage & VK_SHADER_STAGE_FRAGMENT_BIT)) continue;
        const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        for (const auto& d : st.info.constant_buffer_descriptors)
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t binding = d.index + k;   // (a UBO's number plus 3; 0 and 2 are the driver's banks)
                if (binding < 3 || binding - 3 >= std::size(s.ubo[ns])) continue;
                const nvn_p_bind_buf& u = s.ubo[ns][binding - 3];
                if (!u.addr || u.size < 48) continue;
                const int r = vr::eyes_camera_copies_learn(p.key, (int)binding, u.addr, u.size);
                copies |= r > 0, decided &= r >= 0;
            }
    }
    const uint64_t signature = copies ? vr::eyes_camera_signature(p.key) : 0;
    if (copies || decided) known[p.key] = {signature, cascades};
    return signature;
}
bool vr_frag_reads_camera(const Program& p, const NvnState& s) { return vr_frag_camera_signature(p, s) != 0; }
/// (STE-31, option (a)) a draw drawn once for both eyes whose fragment stage reads camera copies: that stage patched to correct
/// them by view index (vk_vr_spirv_camera.h), no second draw. RX_VR_EYES_DEBUG 32768: not patched, as before
uint64_t vr_frag_camera_patch(const Program& p, const NvnState& s) {
    return vr::eyes_debug(32768) ? 0 : vr_frag_camera_signature(p, s);
}
/// (research, RX_VR_EYES_CAM_LOG=1) each program drawn for each eye by that rule, once: how many there are and what they draw into
void vr_frag_camera_note(const Program& p, const Targets& at, bool routed_anyway, int count) {
    static const bool on = vr_research_getenv("RX_VR_EYES_CAM_LOG") && atoi(vr_research_getenv("RX_VR_EYES_CAM_LOG")) != 0;
    if (!on) return;
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() >= 160 || !seen.insert(p.key).second) return;
    logf("vr: true stereo: program %016llx reads the camera in its fragment stage (%u), into %ux%u, %s: now with each eye's "
         "copies (count %d)", (unsigned long long)p.key, (unsigned)seen.size(), at.width, at.height,
         routed_anyway ? "already drawn per eye" : "was one draw for both eyes", count);
}
/// (research, RX_VR_EYES_CAM_LOG=1) a compute program's resources, once per program (96 at most): its storage images and
/// textures (size, NVN format) and storage buffers, to tell which dispatches make screen buffers both eyes then read
void vr_dispatch_note(const NvnState& s, uint64_t key) {
    static const bool on = vr_research_getenv("RX_VR_EYES_CAM_LOG") && atoi(vr_research_getenv("RX_VR_EYES_CAM_LOG")) != 0;
    if (!on || !key || !vr::eyes_frame()) return;
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() >= 96 || !seen.insert(key).second) return;
    char line[1400];
    int n = snprintf(line, sizeof line, "vr: true stereo: dispatch %016llx grid %ux%ux%u:", (unsigned long long)key, s.grid[0],
                     s.grid[1], s.grid[2]);
    auto desc = [&](const char* what, uint32_t h) {
        nvn_tex t;
        nvn_view v;
        bool hv = false;
        if (!h || !texture_of((int)(h & 0xFFFFF), &t, &v, &hv)) return;
        if (n < (int)sizeof line - 96)
            n += snprintf(line + n, sizeof line - n, " %s %ux%ux%u fmt %#x;", what, t.d.width, t.d.height, t.d.depth,
                          (unsigned)t.d.format);
    };
    for (int i = 0; i < 8; i++) desc("img", (uint32_t)s.img[5][i]);
    for (int i = 0; i < 16; i++) desc("tex", (uint32_t)s.tex[5][i]);
    for (int i = 0; i < 16; i++)
        if (s.ssbo[5][i].addr && n < (int)sizeof line - 64)
            n += snprintf(line + n, sizeof line - n, " ssbo%d %#llx KB;", i, (unsigned long long)(s.ssbo[5][i].size >> 10));
    logf("%s", line);
    for (int i = 0; i < 16; i++) {   // (STE-29: which of its buffers keeps a copy of the camera)
        vr::eyes_find_camera(key, "ubo", i, s.ubo[5][i].addr, s.ubo[5][i].size);
        vr::eyes_find_camera(key, "ssbo", i, s.ssbo[5][i].addr, s.ssbo[5][i].size);
    }
}
/// (research, RX_VR_EYES_FROXEL_EYE=1|2) compute dispatches that read the scene's camera get that eye's copy of it, the left's
/// (1) or the right's (2), instead of the middle camera: which eye the game's volumetric fog grid is then built for
std::atomic<int64_t> vr_froxel_moved{0};   // (research: how many dispatch cameras were given an eye's copy)
int vr_froxel_eye() {
    static const int eye = vr_research_getenv("RX_VR_EYES_FROXEL_EYE") ? atoi(vr_research_getenv("RX_VR_EYES_FROXEL_EYE")) : 0;
    return eye == 1 || eye == 2 ? eye : 0;
}
/// (OoT VR, true stereo) A dispatch drawn per eye with its own one-layer images: one that reads the scene's camera and writes a
/// 3D image of one layer. The game's volumetric fog is such a grid (64x36x96 and 128x72x96 cells over the camera's view, from
/// the shadow maps and the scene's depth), made by four programs once a frame. Run once, it was the middle camera's and the
/// left eye's depth's, and both eyes sampled it at their own pixels: each eye's fog, fog shadows and Navi's light in the air
/// might come out wrong in its own way (the suspect for the wet look and one-sided shadows on soft ground in first person:
/// headset 19 Nov, STE-25; not confirmed headless). With RX_VR_EYES_FOG_PER_EYE=1 it runs twice (dispatch_inner): the
/// left eye's camera copy, then the right eye's with layer 1 of the depth and a twin of each such image (texture_vr_twin),
/// which later passes read for the right eye
thread_local bool vr_fog_now = false;   // (the dispatch being recorded is one, both halves)
/// (inputs_draw.inc vr_fog_dispatch: whether a dispatch is one)
/// layer 1's targets of a split draw (false: a view not made, the draw left out)
bool vr_eyes_layer1(Targets& at) {
    for (int i = 0; i < at.n; i++)
        if (at.color_view[i] && !(at.color_view[i] = vr::eyes_layer_view(at.color[i], at.color_format[i], at.color_level[i], 1)))
            return false;
    if (at.depth && !(at.depth_view = vr::eyes_layer_view(at.depth, at.depth_format, at.depth_level, 1))) return false;
    at.eyes = 0;
    return true;
}
