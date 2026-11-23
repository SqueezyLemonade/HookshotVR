// Hookshot VR: the Comfort Aids' vignette in the headset: a soft dark ring round both eyes while the view moves in a way the
// head did not (the VR menu's Comfort -> Comfort Aids rows, each Off / Low / Medium / High: Vignette while moving, while
// turning, in the air, when the game moves you). The game side says which of those is happening now, every game frame
// (vr/comfort/comfort_host.c, through hookshot_vr_comfort); the strongest level among them is shown, faded in and out over
// kVignetteFade.
//
// Included by vr/headset/vk_vr.cpp after bomb_marks.inc, among its private helpers (it uses barrier, memory_type, cmd, s,
// swap_format, Eye and the headset's submit helpers: headset_gpu_free, swap_take, headset_record, headset_submit);
// vr/headset/frame_loop.inc adds its layer over the world, under the menus. Not compiled on its own.
//
// Why a quad layer: one picture, drawn once on the CPU into a static swapchain at the start and never again, and one
// compositor layer while it shows: nothing is added to the eyes' own passes and nothing is drawn per frame. The quad sits in
// the head's own space (s.view), kVignetteDistance ahead, so it stays put on the eyes however the head moves. Each step of
// the fade is its own copy of the ring at that opacity, side by side in the one image (kVignetteSteps of them, 4 x 4), and
// the layer shows the one for the fade's step; the level (how far in the dark comes) is the quad's size: the ring's clear
// middle is a fixed part of the picture, so a smaller quad brings the dark closer to the middle of the view. The quad is
// never smaller than the eyes' view (s.fov_tan_*), so its own edge is never seen.
constexpr int kVignettePx = 128;                          // (one ring's picture, a side)
constexpr int kVignetteSteps = 16;                        // (the fade's steps: 4 x 4 rings in the one image)
constexpr int kVignetteAtlasPx = kVignettePx * 4;
constexpr float kVignetteFade = .15f;                     // (s: none to full, and back)
constexpr float kVignetteClear = .3f, kVignetteDark = .6f;   // (the ring: clear inside this part of the picture's half-width,
                                                          // dark from this, smooth between)
constexpr float kVignetteDistance = 1.0f;                 // (m ahead of the head)
// The levels Low, Medium, High: how far off straight ahead the darkening starts, as the angle's tangent (about 44, 35 and 24
// degrees); it is full at twice that (kVignetteDark / kVignetteClear)
constexpr float kVignetteInner[3] = {.95f, .7f, .45f};
constexpr float kVignetteEase = .1f;                      // (s: a change of level eases the ring in or out this fast)

struct VignetteState {
    std::mutex mu;
    float cause[4]{};                                     // (mu) moving, turning, in the air, the game moving Link: 0 or 1
    std::chrono::steady_clock::time_point at{};           // (mu) when the game side last said
    // (the headset thread)
    Eye art;
    bool ready = false, failed = false;
    float amount = 0;                                     // (0..1: the fade now)
    float inner = 0;                                      // (the darkening's start now, eased: a tangent)
    int level = 0;                                        // (1-3: the level fading, kept while it fades out)
    std::chrono::steady_clock::time_point last{};
    int told = 0, told_level = 0;                         // (the log: how many times, and the level it last said)
} vignette;

// (the game side, every game frame in first person: vr/comfort/comfort_host.c)
void vignette_set(const float* cause) {
    std::lock_guard lk(vignette.mu);
    for (int i = 0; i < 4; i++) vignette.cause[i] = cause && cause[i] > .5f ? 1.0f : 0.0f;
    vignette.at = std::chrono::steady_clock::now();
}

// The ring's darkness at a point of its picture (x, y: -1..1 across it)
float vignette_alpha(float x, float y) {
    const float t = std::clamp((std::sqrt(x * x + y * y) - kVignetteClear) / (kVignetteDark - kVignetteClear), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

bool vignette_make() {
    if (vignette.ready || vignette.failed) return vignette.ready;
    if (!headset_gpu_free()) return false;   // (not a failure: tried again once the headset's last submit is done)
    vignette.failed = true;
    Eye& m = vignette.art;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    ci.width = ci.height = kVignetteAtlasPx;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &m.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(m.chain, 0, &n, nullptr);
    m.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(m.chain, n, &n, (XrSwapchainImageBaseHeader*)m.images.data());
    m.w = m.h = kVignetteAtlasPx;
    // (the picture into a host-visible buffer, copied into the image once: black, premultiplied, so only the alpha counts;
    // the same bytes in RGBA8 sRGB or UNORM, swap_format's two)
    const VkDeviceSize bytes = (VkDeviceSize)kVignetteAtlasPx * kVignetteAtlasPx * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* px = nullptr;
    const auto let_go = [](VkBuffer b, VkDeviceMemory mem) {
        if (mem) vkFreeMemory(dev.device, mem, nullptr);
        if (b) vkDestroyBuffer(dev.device, b, nullptr);
    };
    bool ok = vkCreateBuffer(dev.device, &bci, nullptr, &staging) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev.device, staging, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ok = ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(dev.device, &ai, nullptr, &memory) == VK_SUCCESS &&
             vkBindBufferMemory(dev.device, staging, memory, 0) == VK_SUCCESS &&
             vkMapMemory(dev.device, memory, 0, bytes, 0, (void**)&px) == VK_SUCCESS;
    }
    if (!ok) return let_go(staging, memory), logf("vr: comfort vignette: no staging buffer"), false;
    for (int y = 0; y < kVignetteAtlasPx; y++)
        for (int x = 0; x < kVignetteAtlasPx; x++) {
            const int cell = (y / kVignettePx) * 4 + x / kVignettePx;
            const float fx = ((x % kVignettePx) + .5f) * 2 / kVignettePx - 1, fy = ((y % kVignettePx) + .5f) * 2 / kVignettePx - 1;
            uint8_t* p = px + ((size_t)y * kVignetteAtlasPx + x) * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = (uint8_t)std::lround(vignette_alpha(fx, fy) * (cell + 1) / kVignetteSteps * 255);
        }
    uint32_t index = 0;
    SwapTake take;
    if (!swap_take(take, {m.chain}, &index, "comfort vignette")) return let_go(staging, memory), false;
    if (!headset_record()) return swap_give(take), let_go(staging, memory), false;
    const VkImage img = m.images[index].image;
    barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)kVignetteAtlasPx, (uint32_t)kVignetteAtlasPx, 1};
    vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // (the buffer let go once the copy is done, whenever that is)
    ok = headset_submit(take, VK_NULL_HANDLE, 0, "comfort vignette", [=] { let_go(staging, memory); });
    vignette.ready = ok, vignette.failed = !ok;
    logf(ok ? "vr: comfort vignette ready (%d px, %d steps)" : "vr: comfort vignette could not be made (%d px, %d steps)",
         kVignetteAtlasPx, kVignetteSteps);
    return ok;
}

/// The level (0 none, 1 Low, 2 Medium, 3 High) for what the game side said: the strongest row among what is happening now
int vignette_want() {
    float cause[4];
    {
        std::lock_guard lk(vignette.mu);
        if (std::chrono::steady_clock::now() - vignette.at > std::chrono::milliseconds(250)) return 0;   // (not said lately)
        std::memcpy(cause, vignette.cause, sizeof cause);
    }
    if (opts.vr_mode != 1 || s.menu_visible.load()) return 0;
    const int rows[4] = {opts.vr_vignette_move, opts.vr_vignette_turn, opts.vr_vignette_air, opts.vr_vignette_carried};
    int want = 0;
    for (int i = 0; i < 4; i++)
        if (cause[i] > .5f) want = std::max(want, std::clamp(rows[i], 0, 3));
    return want;
}

/// The vignette as a quad layer for this headset frame (false: none to show)
bool vignette_layer(XrCompositionLayerQuad& q) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = vignette.last.time_since_epoch().count()
                         ? std::clamp(std::chrono::duration<float>(now - vignette.last).count(), 0.0f, .1f) : 0;
    vignette.last = now;
    const int want = vignette_want();
    if (want) vignette.level = want;   // (fading out, the level it had stays)
    vignette.amount = std::clamp(vignette.amount + (want ? dt : -dt) / kVignetteFade, 0.0f, 1.0f);
    if (vignette.amount <= 0) {
        vignette.level = 0, vignette.inner = 0, vignette.told_level = 0;
        return false;
    }
    const float inner = kVignetteInner[std::clamp(vignette.level, 1, 3) - 1];
    vignette.inner = vignette.inner > 0 ? vignette.inner + (inner - vignette.inner) * std::min(1.0f, dt / kVignetteEase) : inner;
    if (!vignette_make()) return false;
    float cover;
    {
        std::lock_guard lk(s.pose_mu);
        cover = std::max(s.fov_tan_x, s.fov_tan_y) * 1.05f;
    }
    const float half = std::max(vignette.inner / kVignetteClear, cover);   // (the quad's half-width, as a tangent)
    const int cell = std::clamp((int)std::lround(vignette.amount * kVignetteSteps) - 1, 0, kVignetteSteps - 1);
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.view;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = vignette.art.chain;
    q.subImage.imageRect.offset = {(cell % 4) * kVignettePx, (cell / 4) * kVignettePx};
    q.subImage.imageRect.extent = {kVignettePx, kVignettePx};
    q.pose = {{0, 0, 0, 1}, {0, 0, -kVignetteDistance}};
    q.size = {2 * half * kVignetteDistance, 2 * half * kVignetteDistance};
    if (want && vignette.amount >= 1 && want != vignette.told_level && vignette.told < 30) {   // (each new full level, bounded)
        vignette.told++, vignette.told_level = want;
        logf("vr: comfort vignette shown: level %d (dark from %.0f degrees off straight ahead)", want,
             std::atan(vignette.inner) * 57.2958f);
    }
    return true;
}
