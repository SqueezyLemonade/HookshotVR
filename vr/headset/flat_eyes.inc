// Hookshot VR: a flat frame (the title, menus, movies) drawn into each eye at that eye's own located pose and FOV
// (RX_VR_FLAT_LAYER=eyes, the default since v1.0.2).
//
// The world screen (flat_place: fixed in the room where it opened, Screen size wide, kFlatScreenMetres ahead) is drawn as each
// eye sees it at the frame's display time, and the projection layer's views are those poses and FOVs as xrLocateViews gave
// them: what OpenXR says a view "should almost always derive from", and what every runtime is made for. Both other ways rest
// on the compositor: projection (before v1.0.2) sends both eyes the picture at one pose that is neither eye's, with one narrow
// FOV, legal ("the runtime will map the view and FOV to the system display appropriately") but at no distance and as each
// runtime sees fit; quad sends it on a quad, whose right and bottom edges moved with the head on WiVRn (issue 873).
// Players on Meta's PC runtime see flat frames doubled with projection; why is not known (never run on it here).
//
// Included by vr/headset/vk_vr.cpp after window_view.inc (its window_pipe), before frame_loop.inc. Not compiled on its own.

struct FlatEyes {
    Eye eye[2];                    // (their own swapchains, the runtime's recommended size: the game's eyes' stay as they are)
    Image out;                     // (one eye's picture, drawn and copied one after the other)
    WindowState::Pipe pipe;
    bool failed = false;           // (could not be made: projection for the run)
    bool known = false;            // an eye pose and FOV located this run (held while they are not valid)
    XrPosef pose[2]{};
    XrFovf fov[2]{};
    bool sent = false;             // views[] as last sent (again when a frame could not be drawn)
    XrCompositionLayerProjectionView views[2];
} flat_eyes;

/// One eye's picture size: the runtime's recommended, at most opts.vr_eye_height tall
void flat_eyes_size(uint32_t* w, uint32_t* h) {
    static uint32_t rw = 0, rh = 0;
    if (!rw) {
        uint32_t n = 0;
        XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(s.inst, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)) &&
            n >= 1 && v[0].recommendedImageRectWidth && v[0].recommendedImageRectHeight)
            rw = v[0].recommendedImageRectWidth, rh = v[0].recommendedImageRectHeight;
        else
            rw = 1800, rh = 1800;
    }
    const uint32_t cap = opts.vr_eye_height > 0 ? std::max(720u, (uint32_t)opts.vr_eye_height) : rh;
    *w = rw, *h = rh;
    if (*h > cap) *w = (uint32_t)((uint64_t)rw * cap / rh) & ~1u, *h = cap;
}

/// The eyes' swapchains, the storage image and the pipeline, made the first time (and again when the size changes)
bool flat_eyes_ready(uint32_t w, uint32_t h) {
    if (!window_pipe(flat_eyes.pipe, 1, sizeof(FlatEyePush), VR_FLAT_EYES_COMP_SPV, sizeof VR_FLAT_EYES_COMP_SPV, "a flat frame's eyes")) return false;
    for (int e = 0; e < 2; e++) {
        Eye& eye = flat_eyes.eye[e];
        if (eye.chain && eye.w == w && eye.h == h) continue;
        if (eye.chain) xrDestroySwapchain(eye.chain);
        eye = Eye{};
        flat_eyes.sent = false;   // (the views last sent name the swapchain destroyed)
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = swap_format;
        ci.sampleCount = 1;
        ci.width = w, ci.height = h;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &eye.chain))) return eye = Eye{}, false;
        uint32_t n = 0;
        xrEnumerateSwapchainImages(eye.chain, 0, &n, nullptr);
        eye.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(eye.chain, n, &n, (XrSwapchainImageBaseHeader*)eye.images.data());
        eye.w = w, eye.h = h;
        logf("vr: flat frames: eye %d: %ux%u, %u images", e, w, h, n);
    }
    return make_image(flat_eyes.out, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, w, h);
}

/// One view recorded (its pipeline and set bound): drawn into out, copied to its swapchain image
void flat_view_record(const WindowState::Pipe& pipe, VkImage out, const FlatEyePush& push, VkImage dst, uint32_t w, uint32_t h) {
    barrier(cmd, out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPushConstants(cmd, pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push.v);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    barrier(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {w, h, 1};
    vkCmdCopyImage(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

/// Slot k's picture on the world screen as each eye sees it, drawn into the eyes' swapchains
bool flat_eyes_draw(int k, uint32_t w, uint32_t h) {
    Slot& sl = s.slots[k];
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, flat_eyes.out.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {s.linear, sl.color.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2];
    for (int i = 0; i < 2; i++) {   // (re-pointed every frame: no submit of ours reads the set now, headset_gpu_free)
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[i].dstSet = flat_eyes.pipe.set;
        wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev.device, 2, wr, 0, nullptr);
    uint32_t index[2];
    SwapTake take;
    if (!swap_take(take, {flat_eyes.eye[0].chain, flat_eyes.eye[1].chain}, index, "flat frame")) return false;
    if (!headset_record()) return swap_give(take), false;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, flat_eyes.pipe.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, flat_eyes.pipe.layout, 0, 1, &flat_eyes.pipe.set, 0, nullptr);
    for (int e = 0; e < 2; e++) {
        const FlatEyePush push = flat_eye_push(flat_screen.pose, flat_screen.width, flat_screen.height, flat_eyes.pose[e],
                                               flat_eyes.fov[e], (float)w, (float)h, 2);
        flat_view_record(flat_eyes.pipe, flat_eyes.out.image, push, flat_eyes.eye[e].images[index[e]].image, w, h);
    }
    return headset_submit(take, s.timeline, sl.value, "flat frame");
}

/// A flat frame's views (slot k): 1 filled, to be sent; 0 not this way (a gameplay frame, another RX_VR_FLAT_LAYER, or it
/// cannot be made here: make_eyes'); -1 nothing to send now. located: the eyes in the room (null: not located), flags theirs
int flat_eyes_views(int k, const XrPosef& head, const XrView* located, XrViewStateFlags flags, XrCompositionLayerProjectionView views[2]) {
    Slot& sl = s.slots[k];
    if (!sl.head_locked || flat_mode() != kFlatEyes || flat_eyes.failed) return 0;
    if (!sl.color.view || !sl.color.w || !sl.color.h) return 0;
    if (located && (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        const bool at = (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        for (int e = 0; e < 2; e++) {
            flat_eyes.pose[e].orientation = located[e].pose.orientation;
            if (at) flat_eyes.pose[e].position = located[e].pose.position;
            else if (!flat_eyes.known) flat_eyes.pose[e].position = head.position;
            flat_eyes.fov[e] = located[e].fov;
        }
        flat_eyes.known = true;
    }
    if (!flat_eyes.known) return 0;   // (never located: as before)
    const auto again = [&] {   // (not drawn now: the last one again, posed as it was drawn)
        if (!flat_eyes.sent) return -1;
        views[0] = flat_eyes.views[0], views[1] = flat_eyes.views[1];
        return 1;
    };
    flat_place(head);
    flat_screen_set(sl.color.w, sl.color.h);
    if (!headset_gpu_free()) return again();
    uint32_t w = 0, h = 0;
    flat_eyes_size(&w, &h);
    if (!flat_eyes_ready(w, h)) {
        flat_eyes.failed = true;
        logf("vr: flat frames: the eyes' own pictures could not be made: RX_VR_FLAT_LAYER=projection for this run");
        return 0;
    }
    if (!flat_eyes_draw(k, w, h)) return again();
    for (int e = 0; e < 2; e++) {
        XrCompositionLayerProjectionView& v = views[e];
        v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        v.pose = flat_eyes.pose[e];
        v.fov = flat_eyes.fov[e];
        v.subImage.swapchain = flat_eyes.eye[e].chain;
        v.subImage.imageRect.extent = {(int32_t)w, (int32_t)h};
        flat_eyes.views[e] = v;
    }
    flat_eyes.sent = true;
    return 1;
}

/// (a player's log) a projection layer's views as sent, and each eye as located for the frame
void views_log(const XrCompositionLayerProjection* sent, const XrPosef& head, const XrView* located, XrViewStateFlags flags) {
    const auto deg = [](float a) { return a * 57.2958f; };
    char at[160];
    const auto pose = [&at](const XrPosef& p) {
        snprintf(at, sizeof at, "at %.4f %.4f %.4f turn %.5f %.5f %.5f %.5f", p.position.x, p.position.y, p.position.z, p.orientation.x,
                 p.orientation.y, p.orientation.z, p.orientation.w);
        return at;
    };
    logf("vr:   head %s; views located %s (state %#llx)", pose(head), located ? "yes" : "no", (unsigned long long)flags);
    for (uint32_t e = 0; sent && e < sent->viewCount && e < 2; e++) {
        const XrCompositionLayerProjectionView& v = sent->views[e];
        logf("vr:   sent %s: %s; fov L/R/U/D %.2f %.2f %.2f %.2f deg; swapchain %p rect %d,%d %dx%d", e ? "R" : "L", pose(v.pose),
             deg(v.fov.angleLeft), deg(v.fov.angleRight), deg(v.fov.angleUp), deg(v.fov.angleDown), (void*)v.subImage.swapchain,
             v.subImage.imageRect.offset.x, v.subImage.imageRect.offset.y, v.subImage.imageRect.extent.width,
             v.subImage.imageRect.extent.height);
    }
    for (int e = 0; located && e < 2; e++) {
        const XrView& v = located[e];
        logf("vr:   located %s: %s; fov L/R/U/D %.2f %.2f %.2f %.2f deg", e ? "R" : "L", pose(v.pose), deg(v.fov.angleLeft),
             deg(v.fov.angleRight), deg(v.fov.angleUp), deg(v.fov.angleDown));
    }
}

/// (bounded: as the world screen opens and 3 s later, at most 8 times) a flat frame as it was sent and as the eyes are: the
/// mode, each view's submitted pose, FOV and rect, each eye's located pose and FOV, and the frame end's result
void flat_log(const XrCompositionLayerProjection* sent, const XrPosef& head, const XrView* located, XrViewStateFlags flags,
              XrResult ended) {
    const XrPosef& p = flat_screen.pose;
    logf("vr:   flat mode %s; screen at %.4f %.4f %.4f turn %.5f %.5f %.5f %.5f, %.3f x %.3f m, picture %ux%u; frame end %d",
         flat_eyes.failed ? "projection (eyes could not be made)" : flat_mode_name(), p.position.x, p.position.y, p.position.z,
         p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, flat_screen.width, flat_screen.height, flat_screen.w,
         flat_screen.h, (int)ended);
    views_log(sent, head, located, flags);
}

// ---- a gameplay picture both eyes share, for eyes turned from the head (eye_rendering.inc make_eyes) ----
WindowState::Pipe turned_pipe;

/// The pipeline and its set for drawing a picture into s.eye_out[0] (no submit of ours reads the set now). False: not made
bool turned_eye_ready(const Image& picture) {
    if (!window_pipe(turned_pipe, 1, sizeof(FlatEyePush), VR_FLAT_EYES_COMP_SPV, sizeof VR_FLAT_EYES_COMP_SPV, "the turned eyes' picture"))
        return false;
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, s.eye_out[0].view, VK_IMAGE_LAYOUT_GENERAL}, {s.linear, picture.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2];
    for (int i = 0; i < 2; i++) {
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[i].dstSet = turned_pipe.set;
        wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev.device, 2, wr, 0, nullptr);
    return true;
}

/// One eye's view recorded: drawn into s.eye_out[0] (the two eyes one after the other), copied to its swapchain image
void turned_eye_record(const FlatEyePush& push, VkImage dst, uint32_t w, uint32_t h) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, turned_pipe.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, turned_pipe.layout, 0, 1, &turned_pipe.set, 0, nullptr);
    flat_view_record(turned_pipe, s.eye_out[0].image, push, dst, w, h);
}
