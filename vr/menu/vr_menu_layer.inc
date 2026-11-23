// Hookshot VR: the VR menu in the headset. vr/menu/vr_menu.inc draws it (on the present's thread, with Dear ImGui, into an
// image of its own: panel_target, panel_ready); this file shows it as a quad layer floating in the room, works out where each
// controller's pointer meets it, and keeps the controllers from the game while it is open.
//
// Included by vr/headset/vk_vr.cpp after radial_draw.inc, among its private helpers (it uses that file's radial_facing, and
// barrier, Eye, swap_format, cmd_pool, cmd, fence, qrot, s and ctl); vr_menu_api.inc, included after them, has the calls
// vk_vr.h declares (the present's thread's images, the open state, the game side's hands). Not compiled on its own.
//
// Where it floats (VR menu: Menu position, opts.vr_menu_position): Fixed in world, placed once when it opens, 1.2 m ahead
// along the head's level heading, a little below the eyes, turned to face the head, and left there while you look around and
// walk; Follows your head, the same place recomputed every frame from where the head is, its heading eased after the head's
// once the head has turned more than 20 degrees away (a tag-along: a glance to the side does not drag it). The image is copied
// into the layer's swapchain only when a new one is drawn: the compositor keeps showing the last one meanwhile. The pointers'
// dots are layers of their own over it (panel_dot), not drawn into the image: the image changes only when what it shows does
// (a selection, a value), not with every move of a pointer (headset 20 Nov: while a pointer moved, the panel was drawn and copied
// every frame and parts of it flashed).
// While it is open, hookshot_vr_hands and hookshot_vr_hands_world (host_api.inc) tell the game side the buttons, triggers and
// sticks are let go (the left Menu button stays as it is: holding it again closes the menu), and each grip as it was when the
// menu opened, so what a hand holds stays held. After it closes they stay let go until every button and trigger is (the press
// that closed it never reaches the game).
constexpr int kPanelSlots = 3;
constexpr float kPanelWidthMetres = 1.4f, kPanelDistanceMetres = 1.2f, kPanelBelowEyesMetres = .1f;
constexpr float kPanelFollowStartRad = .35f, kPanelFollowSpeedRad = 2.5f;   // (20 degrees off; 140 degrees a second)

struct PanelLayer {
    std::mutex mu;                       // (the slots: the present's thread writes one while the headset thread copies another)
    MenuSlot slots[kPanelSlots];
    int next = 0, writing = -1;
    std::atomic<int> reading{-1};
    std::atomic<bool> visible{false}, open{false};
    // (the headset thread)
    Eye eye;
    uint64_t copied = 0;                 // (the slot value last copied into the swapchain)
    bool anchored = false;
    XrPosef pose{};
    float yaw = 0;                       // (Follows your head: the heading it sits on)
    float width = kPanelWidthMetres, height = kPanelWidthMetres * 0.65f;
    Eye ray, dot;
    bool ray_made = false, ray_failed = false, dot_made = false, dot_failed = false;
    // (the pointers, for the present's thread: pose_mu)
    PanelHand hands[2];
    // (what the game side is told while the menu holds the controllers: pose_mu)
    bool holding = false;
    float grip_held[2]{};
} panel;

// ---- the headset thread ----
bool panel_make_eye(uint32_t w, uint32_t h) {
    Eye& eye = panel.eye;
    if (eye.chain && eye.w == w && eye.h == h) return true;
    if (eye.chain) xrDestroySwapchain(eye.chain);
    eye = Eye{};
    panel.copied = 0;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    ci.width = w, ci.height = h;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &eye.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(eye.chain, 0, &n, nullptr);
    eye.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(eye.chain, n, &n, (XrSwapchainImageBaseHeader*)eye.images.data());
    eye.w = w, eye.h = h;
    logf("vr: VR menu panel: %ux%u, %u images", w, h, n);
    return true;
}
/// The newest finished image into the swapchain, when there is one not copied yet (the headset's submits: headset_submit)
bool panel_copy() {
    if (!headset_gpu_free()) return false;
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(dev.device, s.timeline, &done);
    int k = -1;
    uint64_t best = 0;
    {
        std::lock_guard lk(panel.mu);
        for (int i = 0; i < kPanelSlots; i++)
            if (panel.slots[i].value && panel.slots[i].value <= done && panel.slots[i].value > best) best = panel.slots[i].value, k = i;
        if (k < 0 || best <= panel.copied) return panel.copied != 0;   // (nothing new: the last copy stays shown)
        panel.reading = k;
    }
    MenuSlot& sl = panel.slots[k];
    uint32_t index = 0;
    SwapTake take;
    bool ok = panel_make_eye(sl.color.w, sl.color.h) && swap_take(take, {panel.eye.chain}, &index, "menu panel");
    if (ok && !headset_record()) swap_give(take), ok = false;
    if (!ok) panel.reading = -1;
    if (ok) {
        const VkImage dst = panel.eye.images[index].image;
        barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        // (a blit, not a copy: the swapchain's format may be another of the same size, sRGB or not)
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = b.dstOffsets[1] = {(int32_t)sl.color.w, (int32_t)sl.color.h, 1};
        vkCmdBlitImage(cmd, sl.color.image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       VK_FILTER_NEAREST);
        barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // (the slot read stays the menu's to leave alone until the copy is done, whenever that is)
        ok = headset_submit(take, s.timeline, sl.value, "menu panel", [] { panel.reading = -1; });
    }
    if (ok) panel.copied = best;
    else {   // (the last copy stays shown: no frame without the panel)
        static int told;
        if (told++ < 8) logf("vr: VR menu panel: a new picture was not copied (acquire or submit failed): the last one stays");
    }
    return panel.copied != 0;
}

/// The pointer's thin ray: a small swapchain of one warm colour, made once
bool panel_make_ray() {
    if (panel.ray_made || panel.ray_failed) return panel.ray_made;
    if (!headset_gpu_free()) return false;   // (not a failure: tried again once the headset's last submit is done)
    panel.ray_failed = true;
    Eye& m = panel.ray;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    ci.width = ci.height = 4;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &m.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(m.chain, 0, &n, nullptr);
    m.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(m.chain, n, &n, (XrSwapchainImageBaseHeader*)m.images.data());
    m.w = m.h = 4;
    uint32_t index = 0;
    SwapTake take;
    if (!swap_take(take, {m.chain}, &index, "menu ray")) return false;
    if (!headset_record()) return swap_give(take), false;
    const VkImage img = m.images[index].image;
    barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const float a = .75f;   // (premultiplied: the menu's copper, #F2BC8C)
    const VkClearColorValue colour{{.89f * a, .5f * a, .26f * a, a}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
    barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const bool ok = headset_submit(take, VK_NULL_HANDLE, 0, "menu ray");
    panel.ray_made = ok, panel.ray_failed = !ok;
    return ok;
}

/// The pointer's dot: a small swapchain with a round glow (premultiplied: the menu's copper round a light core), made once
bool panel_make_dot() {
    if (panel.dot_made || panel.dot_failed) return panel.dot_made;
    if (!headset_gpu_free()) return false;   // (not a failure: tried again once the headset's last submit is done)
    panel.dot_failed = true;
    constexpr int kDot = 32;
    Eye& m = panel.dot;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    ci.width = ci.height = kDot;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &m.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(m.chain, 0, &n, nullptr);
    m.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(m.chain, n, &n, (XrSwapchainImageBaseHeader*)m.images.data());
    m.w = m.h = kDot;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)kDot * kDot * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev.device, &bci, nullptr, &staging) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev.device, staging, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    uint8_t* px = nullptr;
    bool ok = mai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(dev.device, &mai, nullptr, &memory) == VK_SUCCESS &&
              vkBindBufferMemory(dev.device, staging, memory, 0) == VK_SUCCESS &&
              vkMapMemory(dev.device, memory, 0, bci.size, 0, (void**)&px) == VK_SUCCESS;
    if (ok) {
        for (int y = 0; y < kDot; y++)
            for (int x = 0; x < kDot; x++) {   // (sRGB values: a white-hot core, the copper #F2BC8C, a soft glow out to the edge)
                const float d = std::sqrt((x + .5f - kDot / 2.0f) * (x + .5f - kDot / 2.0f) + (y + .5f - kDot / 2.0f) * (y + .5f - kDot / 2.0f)) /
                                (kDot / 2.0f);
                const float core = std::clamp((.28f - d) * 12, 0.0f, 1.0f), ring = std::clamp((.55f - d) * 10, 0.0f, 1.0f);
                const float glow = std::clamp(1 - d, 0.0f, 1.0f) * .35f;
                const float a = std::max({core, ring * .9f, glow});
                const float r = core + (1 - core) * .95f, g = core + (1 - core) * .74f, b = core + (1 - core) * .55f;
                uint8_t* o = px + ((size_t)y * kDot + x) * 4;
                o[0] = (uint8_t)std::lround(r * a * 255), o[1] = (uint8_t)std::lround(g * a * 255), o[2] = (uint8_t)std::lround(b * a * 255);
                o[3] = (uint8_t)std::lround(a * 255);
            }
        uint32_t index = 0;
        SwapTake take;
        ok = swap_take(take, {m.chain}, &index, "menu dot");
        if (ok && !headset_record()) swap_give(take), ok = false;
        if (ok) {
            const VkImage img = m.images[index].image;
            barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)kDot, (uint32_t)kDot, 1};
            vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            // (the staging buffer let go once the copy is done, whenever that is: headset_submit runs it then)
            ok = headset_submit(take, VK_NULL_HANDLE, 0, "menu dot", [staging, memory] {
                vkDestroyBuffer(dev.device, staging, nullptr);
                vkFreeMemory(dev.device, memory, nullptr);
            });
            staging = VK_NULL_HANDLE, memory = VK_NULL_HANDLE;   // (now headset_submit's to let go)
        }
    }
    if (staging) vkDestroyBuffer(dev.device, staging, nullptr);   // (not submitted: nothing reads it)
    if (memory) vkFreeMemory(dev.device, memory, nullptr);
    panel.dot_made = ok, panel.dot_failed = !ok;
    return ok;
}

/// Hand h's dot where its pointer meets the panel, 2 mm in front of it, facing as the panel does
bool panel_dot(const PanelHand& p, XrCompositionLayerQuad& q) {
    if (!p.pointing || !panel_make_dot()) return false;
    float right[3], up[3], normal[3];
    const float ax[3] = {1, 0, 0}, ay[3] = {0, 1, 0}, az[3] = {0, 0, 1};
    qrot(panel.pose.orientation, ax, right);
    qrot(panel.pose.orientation, ay, up);
    qrot(panel.pose.orientation, az, normal);
    const float x = (p.u - .5f) * panel.width, y = (.5f - p.v) * panel.height;
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = panel.dot.chain;
    q.subImage.imageRect.extent = {(int32_t)panel.dot.w, (int32_t)panel.dot.h};
    q.pose.orientation = panel.pose.orientation;
    q.pose.position = {panel.pose.position.x + right[0] * x + up[0] * y + normal[0] * .002f,
                       panel.pose.position.y + right[1] * x + up[1] * y + normal[1] * .002f,
                       panel.pose.position.z + right[2] * x + up[2] * y + normal[2] * .002f};
    q.size = {.024f, .024f};
    return true;
}

/// A rotation whose columns are x, y, z (orthonormal)
XrQuaternionf panel_basis(const float x[3], const float y[3], const float z[3]) {
    const float t = x[0] + y[1] + z[2];
    XrQuaternionf q;
    if (t > 0) {
        const float s4 = std::sqrt(t + 1) * 2;
        q = {(y[2] - z[1]) / s4, (z[0] - x[2]) / s4, (x[1] - y[0]) / s4, s4 / 4};
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const float s4 = std::sqrt(1 + x[0] - y[1] - z[2]) * 2;
        q = {s4 / 4, (y[0] + x[1]) / s4, (z[0] + x[2]) / s4, (y[2] - z[1]) / s4};
    } else if (y[1] > z[2]) {
        const float s4 = std::sqrt(1 + y[1] - x[0] - z[2]) * 2;
        q = {(y[0] + x[1]) / s4, s4 / 4, (z[1] + y[2]) / s4, (z[0] - x[2]) / s4};
    } else {
        const float s4 = std::sqrt(1 + z[2] - x[0] - y[1]) * 2;
        q = {(z[0] + x[2]) / s4, (z[1] + y[2]) / s4, s4 / 4, (x[1] - y[0]) / s4};
    }
    return q;
}

/// Where the panel goes for this head: kPanelDistanceMetres ahead along heading yaw (radians, about +y; 0 looks down -z), a
/// little below the eyes, facing the head
XrPosef panel_place(const XrPosef& head, float yaw) {
    XrPosef p;
    p.position = {head.position.x - std::sin(yaw) * kPanelDistanceMetres, head.position.y - kPanelBelowEyesMetres,
                  head.position.z - std::cos(yaw) * kPanelDistanceMetres};
    p.orientation = radial_facing(p.position, {head.position.x, p.position.y, head.position.z});   // (upright, toward the head)
    return p;
}
float panel_head_yaw(const XrPosef& head) {
    float f[3];
    const float ahead[3] = {0, 0, -1};
    qrot(head.orientation, ahead, f);
    return std::atan2(-f[0], -f[2]);
}

/// Each hand's pointer on the panel (its aim pose's -z): where it meets it, in the image's own 0..1 (u right, v down)
void panel_point(PanelHand hands[2]) {
    float right[3], up[3], normal[3];
    const float ax[3] = {1, 0, 0}, ay[3] = {0, 1, 0}, az[3] = {0, 0, 1};
    qrot(panel.pose.orientation, ax, right);
    qrot(panel.pose.orientation, ay, up);
    qrot(panel.pose.orientation, az, normal);
    const float c[3] = {panel.pose.position.x, panel.pose.position.y, panel.pose.position.z};
    for (int h = 0; h < 2; h++) {
        PanelHand& o = hands[h];
        o.pointing = false;
        const HandState& hs = ctl.now[h];
        if (!ctl.attached || !hs.valid) continue;
        float d[3];
        const float fwd[3] = {0, 0, -1};
        qrot(hs.aim.orientation, fwd, d);
        const float org[3] = {hs.aim.position.x, hs.aim.position.y, hs.aim.position.z};
        const float denom = d[0] * normal[0] + d[1] * normal[1] + d[2] * normal[2];
        if (denom > -1e-3f) continue;   // (pointing away from its face, or along it)
        const float t = ((c[0] - org[0]) * normal[0] + (c[1] - org[1]) * normal[1] + (c[2] - org[2]) * normal[2]) / denom;
        if (!(t > .02f && t < 6)) continue;
        float rel[3];
        for (int k = 0; k < 3; k++) rel[k] = org[k] + t * d[k] - c[k];
        const float x = rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2];
        const float y = rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2];
        const float u = x / panel.width + .5f, v = .5f - y / panel.height;
        if (u < -.02f || u > 1.02f || v < -.02f || v > 1.02f) continue;
        o.pointing = true, o.u = u, o.v = v, o.distance = t;
    }
}

/// The ray of hand h to where it points on the panel, as a thin quad turned toward the head
bool panel_ray(int h, const PanelHand& p, const XrPosef& head, XrCompositionLayerQuad& q) {
    if (!p.pointing || !panel_make_ray()) return false;
    const HandState& hs = ctl.now[h];
    float d[3];
    const float fwd[3] = {0, 0, -1};
    qrot(hs.aim.orientation, fwd, d);
    const float len = p.distance;
    const float mid[3] = {hs.aim.position.x + d[0] * len * .5f, hs.aim.position.y + d[1] * len * .5f,
                          hs.aim.position.z + d[2] * len * .5f};
    float z[3] = {head.position.x - mid[0], head.position.y - mid[1], head.position.z - mid[2]};
    const float along = z[0] * d[0] + z[1] * d[1] + z[2] * d[2];
    for (int k = 0; k < 3; k++) z[k] -= along * d[k];   // (toward the head, square to the ray)
    float n = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(n > 1e-4f)) return false;
    for (float& v : z) v /= n;
    const float x[3] = {d[1] * z[2] - d[2] * z[1], d[2] * z[0] - d[0] * z[2], d[0] * z[1] - d[1] * z[0]};   // (y x z)
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = panel.ray.chain;
    q.subImage.imageRect.extent = {4, 4};
    q.pose.orientation = panel_basis(x, d, z);
    q.pose.position = {mid[0], mid[1], mid[2]};
    q.size = {.004f, len};
    return true;
}

/// The VR menu as a quad layer, and the pointers' rays and dots over it (rays[0..*n_rays], four at most); false: not open
/// (the half-vanishing panel, 21 Nov: what the headset log says of it) the VR menu's frames while it was open, every 10 s
struct PanelStats {
    uint64_t open = 0, shown = 0, no_copy = 0, copies = 0, rays = 0, dots = 0;
} panel_stats;
void panel_report() {
    PanelStats& p = panel_stats;
    if (p.open)
        logf("vr: VR menu (10 s): %llu frames open, the panel in %llu (%llu without a picture yet or the headset busy); %llu new "
             "pictures copied; %llu rays, %llu dots", (unsigned long long)p.open, (unsigned long long)p.shown,
             (unsigned long long)p.no_copy, (unsigned long long)p.copies, (unsigned long long)p.rays, (unsigned long long)p.dots);
    p = PanelStats{};
}
bool panel_layer(XrCompositionLayerQuad& q, XrCompositionLayerQuad rays[4], int* n_rays, const XrPosef& head) {
    *n_rays = 0;
    if (!panel.visible.load(std::memory_order_acquire) || !panel_open()) {
        panel.anchored = false;
        std::lock_guard lk(s.pose_mu);
        panel.hands[0].pointing = panel.hands[1].pointing = false;
        return false;
    }
    panel_stats.open++;
    const uint64_t copied_was = panel.copied;
    if (!panel_copy()) return panel_stats.no_copy++, false;
    panel_stats.shown++, panel_stats.copies += panel.copied != copied_was;
    panel.height = panel.width * (float)panel.eye.h / (float)panel.eye.w;
    const float head_yaw = panel_head_yaw(head);
    if (!panel.anchored) panel.yaw = head_yaw, panel.anchored = true, panel.pose = panel_place(head, head_yaw);
    if (opts.vr_menu_position == 1) {   // (Follows your head: eased after the head once it has turned well away)
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        float off = std::remainder(head_yaw - panel.yaw, 6.2831853f);
        static bool easing = false;
        if (std::fabs(off) > kPanelFollowStartRad) easing = true;
        if (easing) {
            const float step = std::min(std::fabs(off), kPanelFollowSpeedRad * dt);
            panel.yaw += off > 0 ? step : -step;
            off = std::remainder(head_yaw - panel.yaw, 6.2831853f);
            if (std::fabs(off) < .02f) easing = false;
        }
        panel.pose = panel_place(head, panel.yaw);
    }
    PanelHand hands[2];
    panel_point(hands);
    {
        std::lock_guard lk(s.pose_mu);
        for (int h = 0; h < 2; h++)
            panel.hands[h].pointing = hands[h].pointing, panel.hands[h].u = hands[h].u, panel.hands[h].v = hands[h].v,
            panel.hands[h].distance = hands[h].distance;
    }
    for (int h = 0; h < 2; h++)
        if (panel_ray(h, hands[h], head, rays[*n_rays])) ++*n_rays, panel_stats.rays++;
    for (int h = 0; h < 2; h++)
        if (panel_dot(hands[h], rays[*n_rays])) ++*n_rays, panel_stats.dots++;
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = panel.eye.chain;
    q.subImage.imageRect.extent = {(int32_t)panel.eye.w, (int32_t)panel.eye.h};
    q.pose = panel.pose;
    q.size = {panel.width, panel.height};
    return true;
}
