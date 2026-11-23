// Hookshot VR: True stereo: what a draw's textures are for each eye (vr_eyes_inputs: per-eye or shared), and
// which draws are full-screen.
//
// A piece of gpu/vk/vk_draw.cpp (the draw replay), included there at its VR HOOK: it uses that file's types and helpers
// (Targets, NvnState, Program, stream_copy, logf, ...). Not compiled on its own.
/// (OoT VR EYE ROUTING) Check actual current bindings, not a shader-name or vertex-count guess.
bool vr_eyes_samples(const Program& p, const NvnState& s) {
    for (const Stage& st : p.stages) {
        for (const auto& d : st.info.texture_descriptors) {
            const VkImageViewType vt = view_type(d.type);
            for (uint32_t k = 0; k < d.count; ++k) {
                constexpr bool all_stages = false;   // (RX_VK_HANDLE_POINTER_ALL is gone: only fragment stages zero a storage pointer)
                const uint32_t handle = ((all_stages || st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT) &&
                                         handle_is_storage_pointer(st.info, d, k)) ? 0u : handle_of(s, st, d, k);
                Texture* tex = nullptr;
                if (handle && texture_view_of(handle, vt, ViewUse::Sample, VK_FORMAT_UNDEFINED, &tex) && tex &&
                    (tex->vr_eyes || tex->vr_twin))   // (a twin: the right eye reads its own, vr_fog_dispatch)
                    return true;
            }
        }
    }
    return false;
}

// Bindings count descriptor declarations (not array elements), across all stages,
// exactly as build_descriptors does. Never array-convert a mixed descriptor array.
bool vr_eyes_shared(const Program& p, const NvnState& s, Targets& at) {
    static const bool separate = vr_research_on("RX_VR_EYES_SEPARATE");
    if (separate || p.fallback || p.stages.size() > 8) return false;
    uint32_t binding = 0;
    uint64_t masks[8] = {}, signature = 0;
    size_t index = 0;
    for (const Stage& st : p.stages) {
        binding += st.info.constant_buffer_descriptors.size() + st.info.storage_buffers_descriptors.size() +
                   st.info.texture_buffer_descriptors.size() + st.info.image_buffer_descriptors.size();
        for (const auto& d : st.info.texture_descriptors) {
            bool any = false, all = d.count > 0;
            for (uint32_t k = 0; k < d.count; ++k) {
                constexpr bool all_stages = false;   // (RX_VK_HANDLE_POINTER_ALL is gone: only fragment stages zero a storage pointer)
                const uint32_t handle = ((all_stages || st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT) &&
                    handle_is_storage_pointer(st.info, d, k)) ? 0u : handle_of(s, st, d, k);
                Texture* tex = nullptr;
                const bool stereo = handle && texture_view_of(handle, view_type(d.type), ViewUse::Sample,
                    VK_FORMAT_UNDEFINED, &tex) && tex && tex->vr_eyes;
                if (tex && tex->vr_twin) return false;   // (a twin is another image: drawn one eye after the other)
                any |= stereo; all &= stereo;
            }
            if (any) {
                if (!all || binding >= 64 || view_type(d.type) != VK_IMAGE_VIEW_TYPE_2D) return false;
                masks[index] |= uint64_t(1) << binding;
            }
            ++binding;
        }
        binding += st.info.image_descriptors.size();
        if (masks[index]) {
            const bool shift = at.eyes == 2 && (st.vk_stage & (VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_MESH_BIT_EXT));
            if (!vr::eyes_sample_module(st.module, masks[index], shift)) return false;
            signature = mix(signature, mix(masks[index], index + 1));
        }
        ++index;
    }
    for (size_t i = 0; i < 8; ++i) at.eyes_sample_bindings[i] = masks[i];
    at.eyes_sampling = signature ? signature | 1 : 0;
    return true;
}

/// (OoT VR, frame rate) vr_eyes_samples and vr_eyes_shared for a draw, cached. Both resolve every texture the draw binds (as
/// build_descriptors does again): three lookups of each texture of every draw in true stereo, ~3 us a draw. The answer only
/// changes with the program, its texture handles (and what each slot holds: its version), a texture destroyed or the shift,
/// so it is kept by those; most draws repeat their neighbour's bindings
void vr_eyes_inputs(const Program& p, const NvnState& s, Targets& at, bool want_shared, bool& samples, bool& shared) {
    struct Entry {
        bool samples = false, shared = false;
        uint64_t sampling = 0, bindings[8]{};
    };
    static thread_local std::unordered_map<uint64_t, Entry> cache;
    uint64_t key = mix(mix((uint64_t)(uintptr_t)&p, (uint64_t)at.eyes << 1 | (want_shared ? 1 : 0)), texture_destroy_generation());
                constexpr bool all_stages = false;   // (RX_VK_HANDLE_POINTER_ALL is gone: only fragment stages zero a storage pointer)
    for (const Stage& st : p.stages)
        for (const auto& d : st.info.texture_descriptors)
            for (uint32_t k = 0; k < d.count; ++k) {
                const uint32_t handle = ((all_stages || st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT) &&
                                         handle_is_storage_pointer(st.info, d, k)) ? 0u : handle_of(s, st, d, k);
                key = mix(key, (uint64_t)handle << 32 | (handle ? texture_slot_version((int)(handle & 0xFFFFF)) : 0u));
            }
    if (auto it = cache.find(key); it != cache.end()) {
        samples = it->second.samples, shared = it->second.shared;
        if (shared) {
            at.eyes_sampling = it->second.sampling;
            for (int i = 0; i < 8; i++) at.eyes_sample_bindings[i] = it->second.bindings[i];
        }
        return;
    }
    Entry e;
    e.samples = vr_eyes_samples(p, s);
    e.shared = want_shared && e.samples && vr_eyes_shared(p, s, at);   // (nothing to share without a per-eye texture)
    if (e.shared) {
        e.sampling = at.eyes_sampling;
        for (int i = 0; i < 8; i++) e.bindings[i] = at.eyes_sample_bindings[i];
    }
    if (cache.size() > 16384) cache.clear();
    cache.emplace(key, e);
    samples = e.samples, shared = e.shared;
}

/// A small draw (1-6 vertices, one instance): a full-screen triangle or composite, or a tiny world quad (a glow, a light's
/// sprite). Some of the game's screen triangles DO read a position attribute; no generic inputs is not sufficient to
/// identify them. Shifting those triangles moves an entire composite and produces a second silhouette (STE-06).
bool vr_eyes_fullscreen(const Program& p, const DrawArgs& d) {
    (void)p;
    return !d.mesh && !d.indirect && d.instances == 1 && d.count > 0 && d.count <= 6;
}

/// (STE-27) World geometry drawn over the scene that reads the scene's depth or G-buffer (a two-layer texture) without the
/// scene's camera in its vertex stage: the game's deferred light volumes and decals, boxes of 36 vertices (and many lights
/// in one draw: 120-360), positioned by a matrix baked per object; their fragment stage reads the camera. Drawn the same in
/// both eyes, each eye got the light or the decal only inside the middle camera's box: cut off on opposite sides in the two
/// eyes (two shadows, one on either side of a small rock; a dark band on either side of Navi's light), the boxes' edges seen
/// as lit rectangles on the floor at night (headset 20 Nov). They are projected with the scene's own projection, so the
/// same clip-space shift moves them. Not mesh, not small (full-screen passes keep their rule, STE-06), not indirect (its
/// count unknown). The VR menu's Debug row Lights per eye, or RX_VR_EYES_DEBUG 1024: drawn flat, as before
/// (STE-34) Not the actors' shadow boxes: moved, their vertex stage made the depth lookup's screen place and view ray from the
/// position before the eye shift, so each eye read the depth at the box face's middle-camera place. Drawn once for both eyes
/// instead, each pixel reads its own depth, and their view-space matrices are corrected per view (vk_vr_eyes.cpp
/// eyes_actor_shadow_box; the box's footprint stays the middle camera's, a strip as wide as the disparity at its side edges)
/// (STE-35/30, 20 Nov: no longer by default) A moved box's vertex stage makes the depth lookup's screen place and view ray from
/// the position before the eye shift, so each eye read the depth at the box face's middle-camera place: near an edge, another
/// surface's depth. Moved, the light boxes lit the wrong places (red and white flashes with streaks, the headset with Lights
/// per eye On), and the box that builds the volume buffer (25b435fcf3a89ac4) wrote wrong volumes, which the foliage reads
/// for its volume fog: flat white or fog-coloured foliage (headless: clean with the row Off, white with it On: G1 against
/// G2/G3, every pinned-settings run). Drawn once for both eyes instead, each pixel reads its own depth, and the camera reads
/// their fragment stage makes are corrected per view (STE-31 (a), 7116baf1); the box's footprint stays the middle camera's.
/// RX_VR_EYES_DEBUG 524288: moved as STE-27 had it (with the row On), for A/B
bool vr_eyes_world_over_scene(const Program& p, const NvnState& s, const DrawArgs& d) {
    if (!opts.vr_eyes_lights || d.mesh || d.indirect || d.count <= 6 || vr::eyes_debug(1024)) return false;   // (VR menu Debug: Lights per eye)
    if (!vr::eyes_debug(524288)) return false;
    if (vr::eyes_actor_shadow_box(p.key)) return false;
    return vr_eyes_samples(p, s);
}

/// Whether a small draw keeps its eye shift: a blended quad of 4-6 vertices whose vertex stage reads the scene's camera (at.eyes
/// 2, draw_one): a glow, a light's sprite or a sheet of fog, drawn by the game one quad at a time. Drawn flat they sat at
/// infinity in both eyes: Navi's glow as two halos, one each side of her, and the fog sheets cut against each eye's real depth
/// into sharp pieces with square holes (headset, 19 Nov: STE-23). Full-screen triangles (3 vertices) and opaque small draws
/// (the lighting and the composites) stay flat, as STE-06 needs, and the patched vertex stage leaves every vertex with w
/// exactly 1 where it is as well (vk_vr_spirv_eyes.h). RX_VR_EYES_DEBUG 128: every small draw flat, as before
bool vr_eyes_small_moved(const Targets& at, const NvnState& s, const DrawArgs& d) {
    return at.eyes == 2 && d.count >= 4 && (s.color.blend_en & 1) && !vr::eyes_debug(128);
}
/// (research, bounded: 48 programs) a small draw kept moved, once per program: what it is, to tell glows and lights from
/// screen composites in the log
void vr_eyes_small_note(const Program& p, const NvnState& s, const DrawArgs& d, const Targets& at) {
    static vr::OnceLog<48> small_log;   // (no lock once a program is noted or the log is full)
    if (!small_log.first(p.key)) return;
    logf("vr: true stereo: small draw moved for each eye: prog %016llx, %u vertices, into %ux%u (%d targets%s), blend %u",
         (unsigned long long)p.key, d.count, at.width, at.height, at.n, at.depth ? ", depth" : "", s.color.blend_en & 1);
}

/// (STE-28) a dispatch that will run once per eye (it writes a two-layer image) and reads the scene's camera: each run gets its
/// eye's copy of the camera (vr_cam_eye), the screen-space shadows and lighting it makes then fit that eye's depth
bool vr_dispatch_eye_camera(const Program& p, const NvnState& s) {
    if (!vr::eyes_frame() || p.stages.empty() || vr::eyes_debug(2)) return false;
    const int ns = std::clamp(p.stages[0].nvn_stage, 0, kNvnStages - 1);
    bool camera = false;
    for (const nvn_p_bind_buf& u : s.ubo[ns])
        if (u.addr && u.size >= 0x230 && vr::eyes_scene_camera(u.addr, u.size)) { camera = true; break; }
    if (!camera) return false;
    for (const Stage& st : p.stages)
        for (const auto& d : st.info.image_descriptors) {
            if (!d.is_written) continue;
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(s, st, d, k);
                Texture* tex = nullptr;
                if (handle && texture_view_of(handle, view_type(d.type), ViewUse::Storage, image_format(d.format), &tex) && tex &&
                    tex->vr_eyes)
                    return true;
            }
        }
    return false;
}

/// (stereo_draw.inc: vr_fog_now) a dispatch drawn per eye with its own one-layer images
bool vr_fog_dispatch(const Program& p, const NvnState& s) {
    // (opt-in, RX_VR_EYES_FOG_PER_EYE=1: headless it changed nothing measurable while it cost 12% of the frame rate, every
    // draw reading the grid drawn one eye after the other: Kakariko first person 35.2 -> 31.1 fps, 330 -> 691 passes)
    // (VR menu Debug: Fog per eye, read each dispatch; or RX_VR_EYES_FOG_PER_EYE=1 for the whole run)
    static const bool env = vr_research_getenv("RX_VR_EYES_FOG_PER_EYE") && atoi(vr_research_getenv("RX_VR_EYES_FOG_PER_EYE")) != 0;
    if (!(env || opts.vr_eyes_fog) || !vr::eyes_frame() || p.stages.empty()) return false;
    const int ns = std::clamp(p.stages[0].nvn_stage, 0, kNvnStages - 1);
    bool camera = false;
    for (const nvn_p_bind_buf& u : s.ubo[ns])
        if (u.addr && u.size >= 0x230 && vr::eyes_scene_camera(u.addr, u.size)) { camera = true; break; }
    if (!camera) return false;
    for (const Stage& st : p.stages)
        for (const auto& d : st.info.image_descriptors) {
            if (!d.is_written) continue;
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(s, st, d, k);
                Texture* tex = nullptr;
                if (handle && texture_view_of(handle, view_type(d.type), ViewUse::Storage, image_format(d.format), &tex) && tex &&
                    !tex->vr_eyes && tex->type == VK_IMAGE_TYPE_3D && !tex->pool_key && !tex->sparse && tex->samples == VK_SAMPLE_COUNT_1_BIT)
                    return true;
            }
        }
    return false;
}
