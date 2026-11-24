// Hookshot VR: The renderer-to-headset handoff: captured cameras, scene depth, both eyes and timeline signals.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// (Threaded rendering's recorder thread, gpu/vk/vk_recorder.h) The replay's vkCmd* calls go through vk_work.h into the
// recorder's ring and reach their command buffer later, on the recorder thread; this file's go straight to the driver. Each
// function below that records into the replay's command buffer, or destroys what its commands use, first lets the recorder
// run what this thread put in the ring (B_DIRECT, as vk_dlss.cpp does before NGX records): the recorder is then idle while
// ours are made, so two threads never write one command buffer at once, and ours land after the replay's commands before
// them. Without it, scene_done's depth copy was written into the command buffer the recorder was writing the game's
// dispatches into: on 19 Nov the GPU was lost a minute into Kakariko, 2 of 2 runs (Xid 13
// SKEDCHECK36_DEPENDENCE_COUNTER_UNDERFLOW, a compute fault, the last checkpoint the dispatches just before the gameplay
// network scene_done runs at). The recorder off: a relaxed load and a branch. The replay waits for the recorder ~2 us on
// average at a boundary (the recorder's stats line), the longest 1.4 ms; these are three a frame at most.

VkImage menu_target(uint32_t width, uint32_t height, VkImageView* view) {
    if (view) *view = VK_NULL_HANDLE;
    if (!active() || !view || !width || !height || dev.swap_view_format == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (make_image may destroy a slot's image the ring's commands still name)
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    std::lock_guard lk(s.mu);
    int k = s.next_menu;
    if (k == s.reading_menu.load(std::memory_order_acquire)) k = (k + 1) % kMenuSlots;
    s.next_menu = (k + 1) % kMenuSlots;
    MenuSlot& sl = s.menus[k];
    if (!make_image(sl.color, dev.swap_view_format, usage, width, height)) return VK_NULL_HANDLE;
    sl.value = 0;
    s.writing_menu = k;
    *view = sl.color.view;
    return sl.color.image;
}

void menu_ready(VkCommandBuffer cmd) {
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (records into the present's command buffer directly)
    std::lock_guard lk(s.mu);
    if (s.writing_menu < 0) return;
    MenuSlot& sl = s.menus[s.writing_menu];
    barrier(cmd, sl.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    sl.value = ++s.timeline_value;
    s.pending_value = sl.value;
    s.writing_menu = -1;
    s.menu_visible.store(true, std::memory_order_release);
}

void menu_hidden() {
    std::lock_guard lk(s.mu);
    s.menu_visible.store(false, std::memory_order_release);
    s.writing_menu = -1;
}

float eyes_ipd() { return s.ipd; }
std::atomic<uint64_t> scene_size_now{0};   // (true stereo: the scene's size, width << 32 | height, for eyes_scene_sized)
/// (vk_texture.cpp create) whether a texture is the scene's size or a half or quarter of it: the screen's own buffers, made in
/// whatever order (a copy or a read first), never one of the game's pictures
bool eyes_scene_sized(uint32_t w, uint32_t h) {
    const uint64_t s = scene_size_now.load(std::memory_order_relaxed);
    const uint32_t sw = (uint32_t)(s >> 32), sh = (uint32_t)s;
    for (const uint32_t k : {1u, 2u, 4u})
        if (sw && w * k == sw && h * k == sh) return true;
    return false;
}
/// (STE-32/35, vk_vr_eyes.cpp eyes_target) whether a texture is the scene's size divided by 3, 6, 8, 12 or 16 each way (either
/// rounding): the steps of a screen downsample chain, the glow's (180x144, 240x192 and 480x384 of a 2880x2304 scene)
bool eyes_scene_chain(uint32_t w, uint32_t h) {
    const uint64_t s = scene_size_now.load(std::memory_order_relaxed);
    const uint32_t sw = (uint32_t)(s >> 32), sh = (uint32_t)s;
    if (!sw || !sh) return false;
    auto part = [](uint32_t whole, uint32_t k, uint32_t v) { return v == whole / k || v == (whole + k - 1) / k; };
    for (const uint32_t k : {3u, 6u, 8u, 12u, 16u})
        if (part(sw, k, w) && part(sh, k, h)) return true;
    return false;
}
float eyes_headset_p00() {
    std::lock_guard lk(s.pose_mu);
    return s.fov_tan_x > 0.2f ? 1.0f / s.fov_tan_x : 0.0f;
}

void note_frame_camera(uint64_t frame, const float vp[16], const float proj[16]);   // (vr/body/tracking.inc, BODY-11)
void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                     uint64_t camera_size) {
    // (true stereo's test without the headset needs the scene's camera too: eyes_note_projection)
    if ((!s.running && !eyes_wanted()) || !color0 || !depth || !camera || camera_size < 0x230 || width < 640 || height < 360)
        return;
    if (color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return;
    const uint64_t now = texture_frame_no();
    float vp[16], proj[16];
    {
        std::lock_guard lk(s.mu);
        if (s.scene_frame == now && (uint64_t)width * height <= (uint64_t)s.scene_w * s.scene_h) return;   // (the largest wins)
        float c[0x230 / 4];
        if (!memory_read_as_submitted(camera, c, sizeof c)) return;
        if (c[0x120 / 4] != (float)width || c[0x124 / 4] != (float)height || c[0xa0 / 4] != 0.0f || c[0xa4 / 4] != 0.0f ||
            c[0xa8 / 4] != -1.0f || c[0xac / 4] != 0.0f)
            return;
        std::memcpy(s.scene_vp, c + 0x30 / 4, sizeof s.scene_vp);
        eyes_note_camera(c);   // (research: RX_VR_EYES_CAM_LOG's search for its copies, STE-29)
        std::memcpy(s.scene_proj, c + 0x70 / 4, sizeof s.scene_proj);
        eyes_note_projection(s.scene_proj);
        s.scene_color = color0, s.scene_depth = depth, s.scene_w = width, s.scene_h = height;
        scene_size_now.store((uint64_t)width << 32 | height, std::memory_order_relaxed);
        s.scene_frame = now, s.scene_gen = texture_destroy_generation();
        std::memcpy(vp, s.scene_vp, sizeof vp);
        std::memcpy(proj, s.scene_proj, sizeof proj);
    }
    note_frame_camera(now, vp, proj);   // (BODY-11: which game frame's body this frame is posed with; s.mu not held)
}

/// (vr/body/skin_draw.inc, Link's shadow) whether a depth target is the scene's own depth (as its draws of the last few frames
/// used it): a depth-only pass into it is the scene's depth prepass, never a shadow map. Only the pointers are compared (the
/// texture is never touched), so a texture gone since cannot be read.
constexpr uint64_t kSceneDepthFrames = 8;   // (frames the scene's depth stays known without a scene draw: a load, a menu)
bool scene_depth_is(const Texture* depth) {
    if (!depth) return false;
    std::lock_guard lk(s.mu);
    return s.scene_depth == depth && s.scene_frame != ~0ull && texture_frame_no() - s.scene_frame <= kSceneDepthFrames;
}
/// (vk_draw.cpp vr_scene_network) whether a texture is this frame's scene colour, as its largest draw used it: the input of
/// the game's gameplay upscale at every Render resolution (the shares below 100% each have a network of their own)
bool scene_color_is(const Texture* color) {
    if (!color) return false;
    std::lock_guard lk(s.mu);
    return s.scene_color == color && s.scene_frame != ~0ull && texture_frame_no() - s.scene_frame <= 1 &&
           s.scene_gen == texture_destroy_generation();
}
/// Whether the scene's depth has been seen lately (scene_depth_is can answer)
bool scene_depth_known() {
    std::lock_guard lk(s.mu);
    return s.scene_depth && s.scene_frame != ~0ull && texture_frame_no() - s.scene_frame <= kSceneDepthFrames;
}

/// (true stereo) The scene's end, before the game's first network of the frame (they work in the scene's colour in place):
/// both eyes' scene copied out (and, with RX_VR_EYES_TEST, as pictures)
void eyes_scene_end(VkCommandBuffer cmd) {
    (void)cmd;
    eyes_frame_complete();
}

/// (true stereo) A pass drew both eyes since the last copy: the next dispatch copies the scene first (the game rewrites its
/// colour in place with compute work before its networks, layer 0 only: what the passes drew is copied before any of it)
std::atomic<bool> eyes_drawn_since{false};
void eyes_pass_drawn() { eyes_drawn_since.store(true, std::memory_order_relaxed); }
bool eyes_copy_due() { return eyes_drawn_since.load(std::memory_order_relaxed); }
void eyes_before_dispatch(VkCommandBuffer cmd) {
    if (!eyes_drawn_since.exchange(false, std::memory_order_relaxed)) return;
    std::lock_guard lk(s.mu);
    if (s.scene_frame != texture_frame_no() || s.scene_gen != texture_destroy_generation()) return;
    eyes_scene_done(cmd, s.scene_color, s.scene_depth);
}

void scene_done(VkCommandBuffer cmd) {
    if (eyes_wanted()) eyes_frame_end();   // (true stereo: the frame's counts)
    if (!active()) return;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (records the depth copy into the replay's command buffer directly)
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    static uint64_t done_frame = ~0ull;   // (once a frame: in true stereo the network's dispatch runs for both eyes)
    if (done_frame == now) return;
    done_frame = now;
    if (!s.scene_depth || now - s.scene_frame > 1 || s.scene_gen != texture_destroy_generation()) return;
    Texture* d = s.scene_depth;
    if (!(d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || !(d->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || d->samples != VK_SAMPLE_COUNT_1_BIT)
        return;
    if (!make_image(s.depth_snap, VK_FORMAT_R32_SFLOAT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, d->width, d->height))
        return;
    const VkImageView dv = depth_view(d);
    if (!dv) return;
    VkDescriptorImageInfo ii[2] = {{s.point, dv, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, s.depth_snap.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    memory_barrier(cmd);
    barrier(cmd, s.depth_snap.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.depth_copy);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.depth_layout, 0, 2, w);
    const uint32_t size[2] = {d->width, d->height};
    vkCmdPushConstants(cmd, s.depth_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, size);
    vkCmdDispatch(cmd, (d->width + 7) / 8, (d->height + 7) / 8, 1);
    memory_barrier(cmd);
    s.depth_snap2_valid = false;
    if (opts.vr_stereo == 4 && eyes_frame() && d->vr_eyes) {   // (true stereo: the right eye's depth, the second layer, the same way)
        const VkImageView dv1 = depth_view1(d);
        if (dv1 && make_image(s.depth_snap2, VK_FORMAT_R32_SFLOAT,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, d->width,
                              d->height)) {
            ii[0].imageView = dv1, ii[1].imageView = s.depth_snap2.view;
            barrier(cmd, s.depth_snap2.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.depth_layout, 0, 2, w);
            vkCmdDispatch(cmd, (d->width + 7) / 8, (d->height + 7) / 8, 1);
            memory_barrier(cmd);
            s.depth_snap2_valid = true;
        }
    }
    std::memcpy(s.snap_vp, s.scene_vp, sizeof s.snap_vp);
    std::memcpy(s.snap_proj, s.scene_proj, sizeof s.snap_proj);
    s.depth_snap_valid = true;
    static bool said;
    if (!said) said = true, logf("vr: scene depth %ux%u (format %d) copied once the world is drawn", d->width, d->height, (int)d->format);
}

/// Each present (host/rx_backend.c host_wide_menus, through hookshot_vr_menu_screen): whether a full-screen 2D menu is up,
/// the title, the file select or the pause menu (vr/headset/bridges_host.c hos_vr_menu_now: their screens' updates). Its
/// frame is shown flat on the world screen as the game's own 16:9 picture: the middle 16:9 of the headset-shaped picture,
/// where VR has the game draw every menu (host_wide_menus: no screen kept whole but the event fade), without the HUD
/// size (hud_scale). Before, the file select's background screens were stretched over the whole 5:4 picture while its
/// panels were fitted to its width, and the pause menu was a gameplay frame across the whole view (headset 22 Nov).
/// RX_VR_MENU_169=0: as before
void menu_tick(bool menu) {
    static const bool off = [] { const char* e = getenv("RX_VR_MENU_169"); return e && *e && atoi(e) == 0; }();
    const bool now = !off && menu && active() && opts.vr;
    if (s.menu169.exchange(now, std::memory_order_relaxed) != now) {
        static int told = 0;
        if (told < 40) told++, logf("vr: 2D menu %s at present %lld", now ? "up: its middle 16:9 on the world screen" : "gone",
                                    (long long)presents_now);
    }
}
bool menu_169() { return s.menu169.load(std::memory_order_relaxed); }

void present(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], bool head_locked) {
    if (!active() || !src) return;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (records the slot's copies into the present's command buffer directly)
    std::lock_guard lk(s.mu);
    int32_t x0 = 0, y0 = 0, w = (int32_t)sw, h = (int32_t)sh;
    if (crop && crop[2] > 0 && crop[3] > 0) x0 = crop[0], y0 = crop[1], w = crop[2], h = crop[3];
    // (a full-screen 2D menu: only the middle 16:9, where the game drew it at square pixels; menu_tick)
    const bool menu = !head_locked && s.menu169.load(std::memory_order_relaxed);
    if (menu && (int64_t)w * 9 < (int64_t)h * 16) {
        const int32_t band = std::max<int32_t>(2, (int32_t)((int64_t)w * 9 / 16) & ~1);
        y0 += (h - band) / 2, h = band;
    }
    int k = s.next_slot;
    for (int tries = 0; tries < kSlots && (k == s.reading[0].load() || k == s.reading[1].load()); tries++) k = (k + 1) % kSlots;
    s.next_slot = (k + 1) % kSlots;
    Slot& sl = s.slots[k];
    if (!make_image(sl.color, VK_FORMAT_R8G8B8A8_UNORM,   // (TRANSFER_SRC: 3D Flat copies it into the eyes as it is)
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, (uint32_t)w, (uint32_t)h))
        return;
    sl.value = 0;   // (not usable until this present's submit is done)
    memory_barrier(cmd);
    barrier(cmd, sl.color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[0] = {x0, y0, 0}, b.srcOffsets[1] = {x0 + w, y0 + h, 1};
    b.dstOffsets[1] = {w, h, 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, sl.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    barrier(cmd, sl.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    sl.has_depth = false;
    sl.drawn_ok = false;
    sl.has_eye2 = sl.eyes_mode = false;
    sl.menu169 = menu;
    sl.head_locked = head_locked || !s.camera_owned.load(std::memory_order_acquire) || sl.menu169;
    {   // (whether a flat frame (title, cutscene) is a live 3D scene: its depth and projection were seen this frame)
        static int told = -1;
        const int live = sl.head_locked && !head_locked ? (s.depth_snap_valid ? 1 : 0) : -1;
        if (live >= 0 && live != told) {
            told = live;
            logf("vr: a flat (non-gameplay) frame %s", live ? "is a live 3D scene: it has the game's depth and camera"
                                                         : "has no 3D scene depth (2D, a movie or a menu)");
        }
    }
    if (!sl.head_locked && s.depth_snap_valid &&
        make_image(sl.depth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   s.depth_snap.w, s.depth_snap.h)) {
        barrier(cmd, sl.depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {s.depth_snap.w, s.depth_snap.h, 1};
        vkCmdCopyImage(cmd, s.depth_snap.image, VK_IMAGE_LAYOUT_GENERAL, sl.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, sl.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        std::memcpy(sl.vp, s.snap_vp, sizeof sl.vp);
        std::memcpy(sl.proj, s.snap_proj, sizeof sl.proj);
        sl.has_depth = true;
        sl.drawn_ok = drawn_pose(sl.vp, sl.proj, &sl.drawn, sl.view);
        // (true stereo: the game's picture is the left eye's; its second layer, and the depth's, the right eye's)
        sl.eyes_mode = opts.vr_stereo == 4 && eyes_frame();
        if (sl.eyes_mode) {
            const float h = eyes_half();
            sl.eye_shift[0] = -h, sl.eye_shift[1] = h;
        }
        if (sl.eyes_mode && s.depth_snap2_valid && eyes_two_layer(src) &&
            make_image(sl.color2, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       (uint32_t)w, (uint32_t)h) &&
            make_image(sl.depth2, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       s.depth_snap2.w, s.depth_snap2.h)) {
            barrier(cmd, sl.color2.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit b2 = b;
            b2.srcSubresource.baseArrayLayer = 1;
            vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, sl.color2.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b2,
                           VK_FILTER_NEAREST);
            barrier(cmd, sl.color2.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            barrier(cmd, sl.depth2.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy c2{};
            c2.srcSubresource = c2.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c2.extent = {s.depth_snap2.w, s.depth_snap2.h, 1};
            vkCmdCopyImage(cmd, s.depth_snap2.image, VK_IMAGE_LAYOUT_GENERAL, sl.depth2.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &c2);
            barrier(cmd, sl.depth2.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            sl.has_eye2 = true;
            static bool said;
            if (!said) said = true, logf("vr: true stereo: each eye its own picture (%dx%d), drawn by the game", w, h);
        } else if (sl.eyes_mode) {
            (!s.depth_snap2_valid ? s.n_no_depth2 : s.n_not_two_layer).fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // A menu frame is deliberately flat, but keeps the current camera's FOV. Non-gameplay frames fall back safely in make_eyes.
        std::memcpy(sl.vp, s.depth_snap_valid ? s.snap_vp : s.scene_vp, sizeof sl.vp);
        std::memcpy(sl.proj, s.depth_snap_valid ? s.snap_proj : s.scene_proj, sizeof sl.proj);
        if (!sl.depth.image) make_image(sl.depth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 16, 16);
    }
    if (!sl.head_locked) {   // (the 10 s 3D line, frame_loop.inc)
        s.n_play.fetch_add(1, std::memory_order_relaxed);
        if (sl.has_depth) s.n_depth.fetch_add(1, std::memory_order_relaxed);
        if (sl.eyes_mode) s.n_eyes.fetch_add(1, std::memory_order_relaxed);
        if (sl.has_eye2) s.n_eye2.fetch_add(1, std::memory_order_relaxed);
    }
    s.depth_snap_valid = false;
    s.depth_snap2_valid = false;
    sl.value = ++s.timeline_value;
    s.pending_value = sl.value;
}

void present_done_at(uint64_t main_value) {
    VkSemaphoreSubmitInfo sig;
    if (!present_signal(sig)) return;
    std::lock_guard lk(s.mu);
    s.offscreen_main = main_value, s.offscreen_value = sig.value;   // (a newer one stands for the one before: the timeline only rises)
}

bool present_signal(VkSemaphoreSubmitInfo& out) {
    if (!s.running) return false;
    std::lock_guard lk(s.mu);
    if (!s.pending_value) return false;
    out = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    out.semaphore = s.timeline;
    out.value = s.pending_value;
    out.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    s.pending_value = 0;
    return true;
}
