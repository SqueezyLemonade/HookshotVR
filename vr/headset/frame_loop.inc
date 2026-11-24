// Hookshot VR: The headset frame loop: tracking, completed-frame selection and compositor submission.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- a frame with nothing to show: one blank layer, never none (WiVRn showed black and dropped the controllers' input after
// frames with zero layers). A transparent 4 x 4 quad, made once (a static image), head-locked a metre ahead ----
Eye blank_eye;
bool blank_ready = false;
bool blank_layer(XrCompositionLayerQuad& q) {
    if (!blank_ready) {
        if (!headset_gpu_free()) return false;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
        ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = swap_format;
        ci.sampleCount = 1;
        ci.width = ci.height = 4;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &blank_eye.chain))) return false;
        uint32_t n = 0;
        xrEnumerateSwapchainImages(blank_eye.chain, 0, &n, nullptr);
        blank_eye.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(blank_eye.chain, n, &n, (XrSwapchainImageBaseHeader*)blank_eye.images.data());
        blank_eye.w = blank_eye.h = 4;
        uint32_t index = 0;
        SwapTake take;
        const auto drop_chain = [] { xrDestroySwapchain(blank_eye.chain), blank_eye = Eye{}; };
        if (!swap_take(take, {blank_eye.chain}, &index, "blank layer")) return drop_chain(), false;
        if (!headset_record()) return swap_give(take), drop_chain(), false;
        const VkImage img = blank_eye.images[index].image;
        barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue clear{{0, 0, 0, 0}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        blank_ready = headset_submit(take, VK_NULL_HANDLE, 0, "blank layer");
        if (!blank_ready) return false;
        logf("vr: a frame with nothing to show is sent one blank layer (never zero layers)");
    }
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.view;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = blank_eye.chain;
    q.subImage.imageRect.extent = {4, 4};
    q.pose = {{0, 0, 0, 1}, {0, 0, -1}};
    q.size = {0.01f, 0.01f};
    return true;
}

// ---- the end: the game quits (vr::close, from vk_main.cpp mkwvk_close_asked) ----
std::atomic<int> close_step{0};   // 0 running, 1 the end asked, 2 the session and instance destroyed (nothing left to close)
constexpr int64_t kCloseEndNs = 1000000000LL;   // (how long the runtime has to stop a session asked to end; frames still sent)

/// The headset thread, its loop left: the session destroyed, then the instance (its swapchains, spaces and actions with them)
void headset_destroy() {
    std::lock_guard queue_lk(s.queue_mu);
    s.running = false, s.session_running = false;   // (active() false: the game side asks nothing more of the headset)
    for (int i = 0; i < 40 && !headset_gpu_free(); i++)   // (our last submit done first, 0.2 s at most)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (s.session) {
        const XrResult r = xrDestroySession(s.session);
        s.session = XR_NULL_HANDLE;
        logf("vr: closing: the session destroyed (%d)", (int)r);
    }
    if (s.inst) {
        const XrResult r = xrDestroyInstance(s.inst);
        s.inst = XR_NULL_HANDLE;
        logf("vr: closing: the OpenXR instance destroyed (%d)", (int)r);
    }
    close_step.store(2, std::memory_order_release);
}

void headset_thread() {
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = s.family;
    vkCreateCommandPool(dev.device, &pc, nullptr, &cmd_pool);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = cmd_pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev.device, &ca, &cmd);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev.device, &fi, nullptr, &fence);
    int64_t said = 0, close_until = 0;
    int frames = 0, shown = 0;
    for (;;) {
        if (close_step.load(std::memory_order_acquire) == 1) {   // (the game quits: vr::close)
            const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!close_until) {
                close_until = now_ns + kCloseEndNs;
                if (s.session_running && xr_request_exit) {   // (the runtime's STOPPING then ends it: poll_events)
                    std::lock_guard queue_lk(s.queue_mu);
                    logf("vr: closing: the session's end asked of the runtime (%d)", (int)xr_request_exit(s.session));
                }
            }
            if (!s.session_running || !xr_request_exit || now_ns > close_until) break;
        }
        {   // (the runtime may use the headset's queue for a session's start and end: queue_mutex)
            std::lock_guard queue_lk(s.queue_mu);
            poll_events();
        }
        if (!s.session_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(s.session, &wi, &fs))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // (From here to xrEndFrame the headset's queue is this thread's: our submits, and the runtime's own in the swapchain
        // calls and the frame's end. The renderer's vkDeviceWaitIdle (a window resize, frame generation's changes) takes the
        // lock first: vkDeviceWaitIdle needs every queue to itself, and unguarded two threads could be in this one at once (the
        // suspect for the 19 Nov headset run that lost the GPU the moment the window was resized, Xid 32, a corrupted push
        // buffer; not reproduced with the stand-in headset). Not held through xrWaitFrame, so such a wait waits for a frame's
        // work at most)
        std::unique_lock queue_lk(s.queue_mu);
        XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
        if (const XrResult r = xrBeginFrame(s.session, &fb); XR_FAILED(r)) {   // (no xrEndFrame then: the runtime refuses it)
            headset_fail("xrBeginFrame failed (%d): the frame left out", (int)r);
            continue;
        }
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        XrPosef head{{0, 0, 0, 1}, {0, 0, 0}};
        bool head_ok = XR_SUCCEEDED(xrLocateSpace(s.view, s.local, fs.predictedDisplayTime, &loc)) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (head_ok) head = loc.pose, publish_pose(head, fs.predictedDisplayTime);
        const bool head_tracked = head_ok && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);   // (the Window view's eyes)
        XrView eye_view[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};   // (the eyes from the head, as located: the Window view's)
        bool eye_view_ok = false;
        XrView room_view[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};   // (the eyes in the room, as located: a flat frame's views)
        XrViewStateFlags room_flags = 0;
        bool room_view_ok = false;
        flat_screen.display_time = fs.predictedDisplayTime;
        shared_pose = SharedPose{};
        update_controls(fs.predictedDisplayTime);   // (OoT VR: the hands)
        if (ctl.attached && frames % 450 == 0) note_profiles();
        {   // OpenXR's real eye frusta drive the game camera as well as this projection layer.
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = s.view;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView xv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t nv = 0;
            if (XR_SUCCEEDED(xrLocateViews(s.session, &vli, &vs, 2, &nv, xv)) && nv == 2) {
                eye_view[0] = xv[0], eye_view[1] = xv[1], eye_view_ok = true;
                float tx = 0.0f, ty = 0.0f;
                for (int e = 0; e < 2; e++) {
                    tx = std::max(tx, std::max(std::tan(-xv[e].fov.angleLeft), std::tan(xv[e].fov.angleRight)));
                    ty = std::max(ty, std::max(std::tan(xv[e].fov.angleUp), std::tan(-xv[e].fov.angleDown)));
                }
                // (each eye's own FOV, for the projection layer: located_eye_cut. Not for eyes turned from the head (canted
                // displays): their FOV is in a frame of their own, which a cut of the head's picture is not)
                bool own_ok = std::isfinite(tx) && std::isfinite(ty);
                for (int e = 0; e < 2; e++)
                    own_ok = own_ok && std::fabs(xv[e].pose.orientation.w) > 0.99996f && xv[e].fov.angleLeft < xv[e].fov.angleRight &&
                             xv[e].fov.angleDown < xv[e].fov.angleUp;
                static bool eyes_said = false;
                if (!eyes_said) {   // (once, for a player's log: each eye as the runtime locates it)
                    eyes_said = true;
                    XrViewConfigurationView cv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
                    uint32_t ncv = 0;
                    const bool have_cv = XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                                             s.inst, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &ncv, cv)) && ncv == 2;
                    for (int e = 0; e < 2; e++) {
                        const XrView& x = xv[e];
                        logf("vr: eye %s: at %.1f %.1f %.1f mm from the head, turned %.2f deg; fov L/R/U/D %.2f %.2f %.2f %.2f deg; "
                             "recommended %ux%u", e ? "R" : "L", x.pose.position.x * 1000, x.pose.position.y * 1000,
                             x.pose.position.z * 1000, 2 * std::acos(std::min(1.0f, std::fabs(x.pose.orientation.w))) * 57.2958f,
                             x.fov.angleLeft * 57.2958f, x.fov.angleRight * 57.2958f, x.fov.angleUp * 57.2958f,
                             x.fov.angleDown * 57.2958f, have_cv ? cv[e].recommendedImageRectWidth : 0u,
                             have_cv ? cv[e].recommendedImageRectHeight : 0u);
                    }
                    const XrViewStateFlags valid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
                    if ((vs.viewStateFlags & valid) != valid)
                        logf("vr: the eyes' view state %#llx: position or orientation not valid yet", (unsigned long long)vs.viewStateFlags);
                    if (!own_ok) logf("vr: each eye's own FOV not used for the layer (eyes turned from the head, or none located)");
                }
                if (std::isfinite(tx) && std::isfinite(ty) && tx > 0.2f && ty > 0.2f) {
                    std::lock_guard lk(s.pose_mu);
                    s.fov_tan_x = std::min(tx * kCameraGuardBand, kMaxCameraHalfFovTangent);
                    s.fov_tan_y = std::min(ty * kCameraGuardBand, kMaxCameraHalfFovTangent);
                    s.eye_fov[0] = xv[0].fov, s.eye_fov[1] = xv[1].fov, s.eye_fov_ok = own_ok;
                    s.eye_pose[0] = xv[0].pose, s.eye_pose[1] = xv[1].pose;
                    s.eye_pose_ok = eye_pose_usable(xv[0]) && eye_pose_usable(xv[1]);
                }
                if (frames % 90 == 0) {   // the eye distance, now and then
                    const float dx = xv[1].pose.position.x - xv[0].pose.position.x,
                                dy = xv[1].pose.position.y - xv[0].pose.position.y,
                                dz = xv[1].pose.position.z - xv[0].pose.position.z;
                    const float dd = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (dd > 0.04f && dd < 0.09f) s.ipd = dd;
                }
            }
            frames++;
            vli.space = s.local;
            XrViewState rs{XR_TYPE_VIEW_STATE};
            room_view_ok = XR_SUCCEEDED(xrLocateViews(s.session, &vli, &rs, 2, &nv, room_view)) && nv == 2;
            if (room_view_ok) room_flags = rs.viewStateFlags;
        }
        XrCompositionLayerProjectionView views[2];
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad menu_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad flat_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};   // (the world screen: a flat frame's picture)
        XrCompositionLayerQuad hand_layers[2];
        XrCompositionLayerQuad bow_preview{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad radial_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad panel_quad{XR_TYPE_COMPOSITION_LAYER_QUAD}, panel_rays[4];   // (the VR menu, its pointers' rays and dots: vr/menu/vr_menu_layer.inc)
        XrCompositionLayerQuad bomb_quads[kBombMarksMax];   // (the bombs' arc, target and pull ring: vr/items/bomb/bomb_marks.inc)
        XrCompositionLayerQuad vignette_quad;   // (the Comfort Aids' vignette: vr/comfort/vignette_layer.inc)
        WindowLayers window_layer;   // (the Window view's frame in the room: vr/headset/window_view.inc)
        XrCompositionLayerQuad window_rays[2];   // (its Move mode's lasers)
        XrCompositionLayerPassthroughFB room_layer;   // (its room, by XR_FB_passthrough: under every other layer)
        XrCompositionLayerQuad wrist_quad;      // (the wrist check HUD: vr/menu/wrist_draw.inc)
        const XrCompositionLayerBaseHeader* layers[kBombMarkLayersMax];
        uint32_t n_layers = 0;
        window_room_frame();   // (the Window view's room this frame, or none: its blend mode and underlay below)
        if (fs.shouldRender && head_ok) {
            // the newest slot the GPU finished
            uint64_t done = 0;
            {   // (no window: the slot's value signalled here once the replay is past its submit, present_done_at)
                std::lock_guard lk(s.mu);
                if (s.offscreen_value && vk_timeline_done() >= s.offscreen_main) {   // (from the CPU: no submit)
                    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
                    si.semaphore = s.timeline, si.value = s.offscreen_value;
                    vkGetSemaphoreCounterValue(dev.device, s.timeline, &done);
                    if (done < si.value) vkSignalSemaphore(dev.device, &si);
                    s.offscreen_value = 0;
                }
            }
            vkGetSemaphoreCounterValue(dev.device, s.timeline, &done);
            int k = -1, mk = -1;
            int ke[2] = {-1, -1};
            uint64_t best = 0, menu_best = 0;
            {
                std::lock_guard lk(s.mu);
                for (int i = 0; i < kSlots; i++)
                    if (s.slots[i].value && s.slots[i].value <= done && s.slots[i].value > best) best = s.slots[i].value, k = i;
                ke[0] = ke[1] = k;
                s.reading[0] = ke[0];
                s.reading[1] = ke[1];
                if (s.menu_visible.load(std::memory_order_acquire))
                    for (int i = 0; i < kMenuSlots; i++)
                        if (s.menus[i].value && s.menus[i].value <= done && s.menus[i].value > menu_best)
                            menu_best = s.menus[i].value, mk = i;
                s.reading_menu = mk;
            }
            window_edit_frame(head);   // (the Window view's Move mode: the window dragged with a laser)
            if (k >= 0 && window_view()) {   // (Window view: the game through the frame in the room, a flat frame in it too)
                flat_screen.shown = flat_screen.open = false;   // (no world screen: the next flat frame's opens where you are)
                const int n = window_layers(k, head, head_tracked, eye_view_ok ? eye_view : nullptr, window_layer);
                for (int e = 0; e < n; e++) layers[n_layers++] = window_layer.list[e];
            } else if (const int flat = k >= 0 ? flat_eyes_views(k, head, room_view_ok ? room_view : nullptr, room_flags, views) : -1;
                       flat > 0 || (flat == 0 && make_eyes(ke, k, head, views))) {
                // (A flat frame (the title, menus, story cutscenes on the screen, movies): RX_VR_FLAT_LAYER. eyes, the
                // default: the room-fixed screen drawn into each eye's own view (flat_eyes.inc). projection: each eye posed at
                // the screen's fixed heading and Screen size wide, at no distance. quad: the world screen's room-fixed quad (D,
                // 21 Nov), which showed a line along its right and bottom edges that moved with the head on WiVRn (the user's
                // A/B, 22 Nov), the cause not found)
                if (flat_mode() == kFlatQuad && flat_screen_quad(flat_quad)) {   // (a flat frame: the world screen, fixed in the room: eye_rendering.inc)
                    layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&flat_quad;
                } else {
                    layer.space = s.local;
                    layer.viewCount = 2;
                    layer.views = views;
                    layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&layer;
                }
            }
            for (int h = 0; h < 2; h++)   // (the hands' markers, over the world and under the menu)
                if (n_layers && hand_marker(h, hand_layers[h]))
                    layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&hand_layers[h];
            // Use the SAME solved palm/hand line as launch, transformed through the recorded camera, not a second raw-hand ray.
            float aim[12];
            if (n_layers && opts.vr_mode==1 && opts.vr_arms && !s.menu_visible.load() && hookshot_vr_bow(aim)) {
                Pose pose{};
                {
                    std::lock_guard lk(s.pose_mu);
                    const uint32_t seq=(uint32_t)aim[11];
                    const Pose& p=s.poses[seq%kPoses];
                    if (p.seq==seq && p.drawn) pose=p;
                }
                auto local_point=[&](float distance) {
                    float camera[3],local[3];
                    for (int row=0;row<3;row++) {
                        camera[row]=pose.view[row*4+3];
                        for (int j=0;j<3;j++) camera[row]+=pose.view[row*4+j]*(aim[j]+distance*aim[3+j]);
                    }
                    qrot(pose.pose.orientation,camera,local);
                    return XrVector3f{pose.pose.position.x+local[0],pose.pose.position.y+local[1],pose.pose.position.z+local[2]};
                };
                // (only while drawing: the bow in one hand and its string in the other, flag 1 of the bow solution)
                const bool drawing = ((int)aim[10] & 1) != 0;
                if (pose.drawn && opts.vr_bow_preview && drawing && make_marker(2)) {
                    bow_preview.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    bow_preview.space=s.local;
                    bow_preview.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                    bow_preview.subImage.swapchain=ctl.marker[2].chain;
                    bow_preview.subImage.imageRect.extent={64,64};
                    bow_preview.pose.orientation=head.orientation;
                    bow_preview.pose.position=local_point(10);
                    const float size=(.12f-.04f*std::clamp(aim[9]/.5f,0.0f,1.0f))*std::clamp(opts.vr_bow_reticle_size,50,600)*.01f;
                    bow_preview.size={size,size};
                    layers[n_layers++]=(const XrCompositionLayerBaseHeader*)&bow_preview;
                }
            }
#include "../items/boomerang/boomerang_reticle.inc"   // (the Boomerang's reticle while it aims, the bow's artwork)
#include "../items/aim/third_person_reticle.inc"   // (third person: where the aimed shot lands, the bow's artwork)
            if (n_layers) {   // (the bombs' marks, over the world and the bow's and Boomerang's reticles; nine layers kept for the vignette, the wrist HUD and the menus below)
                const int room = std::min<int>(kBombMarksMax, kBombMarkLayersMax - 9 - (int)n_layers);
                const int marks = bomb_mark_layers(bomb_quads, room, head);
                for (int i = 0; i < marks; i++) layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&bomb_quads[i];
            }
            if (n_layers && vignette_layer(vignette_quad))   // VR HOOK (the Comfort Aids' vignette: over the world, under the menus)
                layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&vignette_quad;
            if (n_layers && wrist_layer(wrist_quad, head))   // VR HOOK (the wrist check HUD: vr/menu/wrist_draw.inc)
                layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&wrist_quad;
            if (n_layers && radial_layer(radial_quad, head))   // (the VR radial item menu: vr/menu/radial_draw.inc)
                layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&radial_quad;
            if (n_layers + 2 < kBombMarkLayersMax - 7)   // (Move mode's lasers, over the window)
                for (int r = 0, nr = window_edit_rays(window_rays, head); r < nr; r++)
                    layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&window_rays[r];
            int n_rays = 0;   // (the VR menu, and its pointers' rays and dots over it)
            if (panel_layer(panel_quad, panel_rays, &n_rays, head)) {
                layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&panel_quad;
                for (int r = 0; r < n_rays; r++) layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&panel_rays[r];
            }
            if (mk >= 0 && make_menu_quad(mk, menu_layer))
                layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&menu_layer;
            if (n_layers) shown++;
            if (n_layers < kBombMarkLayersMax && window_underlay(room_layer)) {   // (the room, under every layer)
                std::memmove(layers + 1, layers, n_layers * sizeof *layers);
                layers[0] = (const XrCompositionLayerBaseHeader*)&room_layer, n_layers++;
            }
        }
        bool flat_told = false;
        {   // (bounded: every layer of a flat frame, once as the world screen opens and once 3 s later: what draws the
            // line along its right and bottom edges that moves with the head, headset 22 Nov)
            static bool was_flat = false;
            static int64_t opened = 0;
            static int told = 0, phase = 0;
            const int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (flat_screen.shown && !was_flat) opened = t, phase = 0;
            was_flat = flat_screen.shown;
            if (flat_screen.shown && told < 8 && ((phase == 0) || (phase == 1 && t - opened > 3000))) {
                told++, phase++, flat_told = true;
                logf("vr: [%.1f s] flat frame's layers (%u, %s):", run_s(), n_layers, phase == 1 ? "as it opened" : "3 s later");
                for (uint32_t i = 0; i < n_layers; i++) {
                    const XrCompositionLayerBaseHeader* b = layers[i];
                    const char* space = b->space == s.local ? "room" : b->space == s.view ? "head" : "other";
                    if (b->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                        const auto* q = (const XrCompositionLayerQuad*)b;
                        const float dp[3] = {q->pose.position.x - head.position.x, q->pose.position.y - head.position.y,
                                             q->pose.position.z - head.position.z};
                        float d[3];
                        qrot(qinv(head.orientation), dp, d);
                        logf("vr:   %u: quad in the %s's space, swapchain %p rect %d,%d %dx%d, %.2f x %.2f m, %s%.2f %.2f %.2f from "
                             "the head (its frame), flags %#llx", i, space, (void*)q->subImage.swapchain, q->subImage.imageRect.offset.x,
                             q->subImage.imageRect.offset.y, q->subImage.imageRect.extent.width, q->subImage.imageRect.extent.height,
                             q->size.width, q->size.height, b->space == s.local ? "" : "(pose itself) ", d[0], d[1], d[2],
                             (unsigned long long)b->layerFlags);
                    } else if (b->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                        const auto* pl = (const XrCompositionLayerProjection*)b;
                        logf("vr:   %u: projection in the %s's space, %u views, the first %dx%d", i, space, pl->viewCount,
                             pl->viewCount ? pl->views[0].subImage.imageRect.extent.width : 0,
                             pl->viewCount ? pl->views[0].subImage.imageRect.extent.height : 0);
                    } else logf("vr:   %u: layer type %d in the %s's space", i, (int)b->type, space);
                }
            }
        }
        XrCompositionLayerQuad blank_quad;   // (nothing else this frame: one blank layer, never zero)
        if (!n_layers && blank_layer(blank_quad)) layers[n_layers++] = (const XrCompositionLayerBaseHeader*)&blank_quad;
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = window_blend_mode();   // (opaque, but for the Window view's room by alpha blend)
        ei.layerCount = n_layers;
        ei.layers = layers;
        const XrResult ended = xrEndFrame(s.session, &ei);
        if (XR_FAILED(ended))   // (refused: none of the frame's layers is shown)
            headset_fail("xrEndFrame refused the frame (%d, %u layers): nothing of it shown", (int)ended, n_layers);
        if (flat_told) flat_log(layer.viewCount ? &layer : nullptr, head, room_view_ok ? &room_view[0] : nullptr, room_flags, ended);
        const TesterQuad named[] = {{"the VR menu", &panel_quad}, {"the wrist HUD", &wrist_quad}, {"the radial menu", &radial_quad},
                                    {"Hookshot's own menu", &menu_layer}, {"the vignette", &vignette_quad}, {"a flat frame", &flat_quad},
                                    {"nothing to show (blank)", &blank_quad}};
        tester_frame(layers, n_layers, named, (int)(sizeof named / sizeof named[0]), fs, head, room_view_ok ? &room_view[0] : nullptr, room_flags,
                     ended, flat_told);
        window_frame_ended(ended);   // (the Window view's room: a refused way given up)
        queue_lk.unlock();
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - said >= 10) {
            said = now;
            logf("vr: %d headset frames shown in the last 10 s (eye distance %.1f mm)", shown, s.ipd * 1000);
            {   // (the 3D: how many gameplay frames had the scene's depth, and in true stereo the right eye's own picture)
                const uint32_t play = s.n_play.exchange(0), depth = s.n_depth.exchange(0), eyes = s.n_eyes.exchange(0),
                               eye2 = s.n_eye2.exchange(0), no2 = s.n_no_depth2.exchange(0), not2 = s.n_not_two_layer.exchange(0);
                if (play) {
                    logf("vr: 3D %s: %u of %u gameplay frames with the scene's depth, %u in Stereo with each eye's own picture "
                         "(%u without: %u no second depth, %u not two-layer)", opts.vr_stereo == 0 ? "Mono" : opts.vr_stereo == 4 ?
                         "Stereo" : "From depth", depth, play, eye2, eyes - std::min(eyes, eye2), no2, not2);
                    static bool flat_said = false;
                    if (!depth && play >= 100 && opts.vr_stereo != 0 && !flat_said)
                        flat_said = true, logf("vr: gameplay frames have no scene depth: both eyes see one picture (no stereo)");
                }
            }
            for (int h = 0; h < 2 && ctl.attached; h++) {   // (each hand from the head, in the head's frame: x right, y up, z back)
                const HandState& hs = ctl.now[h];
                if (!hs.valid) {
                    logf("vr:   %s hand not tracked", h ? "right" : "left");
                    continue;
                }
                const float dp[3] = {hs.grip.position.x - head.position.x, hs.grip.position.y - head.position.y,
                                     hs.grip.position.z - head.position.z};
                float d[3];
                qrot(qinv(head.orientation), dp, d);
                logf("vr:   %s hand %.2f %.2f %.2f m from the head, speed %.2f m/s, trigger %.2f, grip %.2f", h ? "right" : "left",
                     d[0], d[1], d[2], std::sqrt(hs.lin_vel[0] * hs.lin_vel[0] + hs.lin_vel[1] * hs.lin_vel[1] +
                                                 hs.lin_vel[2] * hs.lin_vel[2]), hs.trigger, hs.squeeze);
            }
            body_timing_report();   // (vr/body/tracking.inc: each frame posed with its own body, BODY-11)
            body_draw_report();     // (vr/body/vk_vr_body.cpp: Link's draws, a frame without his colour, refused palettes)
            panel_report();         // (vr/menu/vr_menu_layer.inc: the VR menu's frames, while it was open)
            shown = 0;
        }
    }
    headset_destroy();   // (the game quits: the loop left above)
}
