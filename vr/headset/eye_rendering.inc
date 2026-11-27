// Hookshot VR: OpenXR eye swapchains, depth reprojection and the flat menu layer.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// Flat screens keep their own aspect ratio. These values control only their apparent size/distance, not world scale.
constexpr float kFlatWidthDeg = 70.0f;       // title/cutscene screen width, degrees (the VR menu's Screen size: opts.vr_cutscene_screen)
constexpr float kFlatScreenMetres = 3.5f;    // the world screen: this far ahead of where you were when it opened
constexpr uint32_t kFlatPictureH = 1440;      // the world screen's picture at most this tall when the headset's density is not known
constexpr float kMenuDistanceMetres = 1.35f; // Hookshot menu, forward from the headset
constexpr float kMenuWidthMetres = 1.55f;

// ---- the headset thread -----------------------------------------------------------------------------------------------------

struct Eye {
    XrSwapchain chain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
};
Eye eyes[2];
Eye menu_eye;
// The world screen (a flat frame: story cutscenes, the title, menus, the quick select): the picture on a quad fixed in the room
// in front of where you were and faced when the flat view began, Screen size wide from there (the user's rule, 21 Nov: big
// cutscenes "on a 2D screen floating in the world", not following the head). make_eyes fills it; the frame loop shows it
// in place of the projection layer. (Before 21 Nov the flat frame was the projection layer at that FOV: its turn fixed,
// but at no distance, so it moved with every step of the head.)
struct FlatScreen {
    bool shown = false;          // this frame's eyes are a flat frame
    bool open = false;           // the last eyes were a flat frame too (the screen stays where it opened); the Window view's
                                 // gameplay frames close it
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    float width = 0, height = 0;
    uint32_t w = 0, h = 0;       // the picture's size in eyes[0]'s image
    XrTime display_time = 0;     // this frame's
    XrTime reopen_at = 0;        // (a recenter by the headset: opened again where you are from then on; 0 none, flat_place)
    bool reopen = false;         // (the headset was taken off: the same)
} flat_screen;

/// RX_VR_FLAT_LAYER: how a flat frame (the title, menus, movies) goes to the headset. eyes (the default): drawn into each eye
/// at its own located pose and FOV (flat_eyes.inc); projection: one picture for both eyes at the screen's fixed pose, as
/// before v1.0.2; quad: a quad fixed in the room
enum FlatMode { kFlatEyes, kFlatProjection, kFlatQuad };
const char* flat_mode_name(FlatMode m) { return m == kFlatEyes ? "eyes" : m == kFlatProjection ? "projection" : "quad"; }
FlatMode flat_mode() {
    static const FlatMode mode = [] {
        const char* e = getenv("RX_VR_FLAT_LAYER");
        const FlatMode m = e && !std::strcmp(e, "projection") ? kFlatProjection : e && !std::strcmp(e, "quad") ? kFlatQuad : kFlatEyes;
        logf("vr: flat frames: RX_VR_FLAT_LAYER=%s%s", flat_mode_name(m), !e || !*e ? " (the default)"
             : std::strcmp(e, flat_mode_name(m)) ? " (the value set is none of eyes, projection, quad: the default)" : "");
        return m;
    }();
    return mode;
}
const char* flat_mode_name() { return flat_mode_name(flat_mode()); }
int64_t swap_format = 0;
VkCommandPool cmd_pool = VK_NULL_HANDLE;
VkCommandBuffer cmd = VK_NULL_HANDLE;
VkFence fence = VK_NULL_HANDLE;

// ---- the headset thread's GPU work: one command pool, one command buffer and one fence for every layer ----------------------
// Each layer goes the same way: its swapchain images acquired and waited for (swap_take), the shared command buffer recorded
// (headset_record), submitted and waited for at most a second (headset_submit), the images released. One submit is in
// flight at most, so the pool, the buffer and the fence are free again once its fence signals. When it has not signalled in
// the second (a hung, lost or starved GPU, a process frozen across the wait), nothing of it is let go: its images stay
// acquired (released, the compositor would show them half written), the pool and the fence are not reset (Vulkan forbids it
// with the work pending), and what was to be destroyed after it waits too. Every layer is then left out (headset_gpu_free:
// each site asks first, before it touches a descriptor, an image or staging memory that work may still read) until the
// fence signals; then the images are released and the rest destroyed. Each failure is logged. (Before, eye 0 stayed acquired
// when eye 1's acquire failed, seven waits' results were dropped, and after a timeout the images were released and the pool
// and fence reset at once, by the next layer of the same frame: the outside review's finding 1, 20 Nov.)
constexpr uint64_t kHeadsetWaitNs = 1000000000ull;
/// The swapchain images one submit draws into: each acquired and waited for
struct SwapTake {
    XrSwapchain chain[2]{};
    int n = 0;
};
struct HeadsetPending {   // (a submit whose wait gave up: what is let go once its fence signals)
    bool on = false;
    SwapTake images;
    std::function<void()> after;
    const char* what = "";
    std::chrono::steady_clock::time_point since;
} headset_pending;
/// Logs a failure: the first few, then one in 600 (a failing session fails every frame)
void headset_fail(const char* fmt, ...) {
    static std::atomic<uint64_t> count{0};
    const uint64_t n = ++count;
    if (n > 8 && n % 600) return;
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    logf("vr: %s (headset failure %llu)", line, (unsigned long long)n);
}
void swap_give(SwapTake& t) {
    for (int i = 0; i < t.n; i++) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult r = xrReleaseSwapchainImage(t.chain[i], &ri);
        if (XR_FAILED(r)) headset_fail("a swapchain image's release failed (%d)", (int)r);
    }
    t.n = 0;
}
/// Whether the headset thread may record and submit again: no submit of its own is still pending. One whose wait gave up is
/// looked at without waiting; once its fence has signalled its images are released and its after-work done
bool headset_gpu_free() {
    HeadsetPending& p = headset_pending;
    if (!p.on) return true;
    const VkResult r = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, 0);   // (a look, not a wait)
    if (r == VK_TIMEOUT) return false;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.since).count();
    logf("vr: the headset's %s submit %s %.1f s after it was given: its images released, layers drawn again", p.what,
         r == VK_SUCCESS ? "finished" : "ended with the device lost", secs);
    swap_give(p.images);
    if (p.after) p.after();
    p = HeadsetPending{};
    return r == VK_SUCCESS;
}
/// Acquires and waits for one image of each chain (index[i] the image of chain i). False: none is kept (each one taken and
/// waited for released again), logged. One that was acquired but whose wait failed cannot be released (OpenXR releases only
/// a waited image): it is logged as such
bool swap_take(SwapTake& t, std::initializer_list<XrSwapchain> chains, uint32_t* index, const char* what) {
    t = SwapTake{};
    int i = 0;
    for (const XrSwapchain chain : chains) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult r = xrAcquireSwapchainImage(chain, &ai, &index[i]);
        if (XR_FAILED(r)) {
            headset_fail("%s: a swapchain image's acquire failed (%d): the layer left out", what, (int)r);
            return swap_give(t), false;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        r = xrWaitSwapchainImage(chain, &wi);
        if (r != XR_SUCCESS) {   // (XR_TIMEOUT_EXPIRED succeeds, but the image is not ours yet)
            headset_fail("%s: a swapchain image's wait failed (%d): the layer left out, that image still acquired", what, (int)r);
            return swap_give(t), false;
        }
        t.chain[t.n++] = chain;
        i++;
    }
    return true;
}
/// The shared command buffer reset and begun. False: logged (the caller gives its images back)
bool headset_record() {
    VkResult r = vkResetCommandPool(dev.device, cmd_pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (r == VK_SUCCESS) r = vkBeginCommandBuffer(cmd, &bi);
    if (r != VK_SUCCESS) headset_fail("the headset's command buffer could not be begun (%d)", (int)r);
    return r == VK_SUCCESS;
}
/// Ends the recorded command buffer and submits it to the headset's queue (after wait_on reaches wait_value, when given),
/// waiting for it at most a second. Done: t's images released, `after` run, true. Not submitted: the same, false. Its wait
/// given up: t's images and `after` kept for headset_gpu_free, false
bool headset_submit(SwapTake& t, VkSemaphore wait_on, uint64_t wait_value, const char* what, std::function<void()> after = {}) {
    VkResult r = vkEndCommandBuffer(cmd);
    if (r == VK_SUCCESS) r = vkResetFences(dev.device, 1, &fence);
    if (r == VK_SUCCESS) {
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = wait_on, wait.value = wait_value, wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cb.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = wait_on ? 1 : 0;
        si.pWaitSemaphoreInfos = &wait;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cb;
        r = vkQueueSubmit2(s.queue, 1, &si, fence);
    }
    if (r != VK_SUCCESS) {
        headset_fail("%s: the headset's submit failed (%d)", what, (int)r);
        swap_give(t);
        if (after) after();
        return false;
    }
    r = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, kHeadsetWaitNs);
    if (r == VK_SUCCESS) {
        swap_give(t);
        if (after) after();
        return true;
    }
    // (always said, past headset_fail's limit: rare, and the line the headset logs are read for)
    logf("vr: %s: the headset's submit did not finish in %.0f s (%d): its images, the command pool and the fence held until it "
         "does, every layer left out meanwhile", what, kHeadsetWaitNs / 1e9, (int)r);
    headset_pending = HeadsetPending{true, t, std::move(after), what, std::chrono::steady_clock::now()};
    t = SwapTake{};
    return false;
}

bool make_eye(int e, uint32_t w, uint32_t h) {
    Eye& eye = eyes[e];
    if (eye.chain && eye.w == w && eye.h == h) return true;
    if (eye.chain) xrDestroySwapchain(eye.chain);
    eye = Eye{};
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
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
    logf("vr: eye %d: %ux%u, %u images", e, w, h, n);
    return true;
}

bool make_menu_eye(uint32_t w, uint32_t h) {
    Eye& eye = menu_eye;
    if (eye.chain && eye.w == w && eye.h == h) return true;
    if (eye.chain) xrDestroySwapchain(eye.chain);
    eye = Eye{};
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
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
    logf("vr: native menu quad: %ux%u, %u images", w, h, n);
    return true;
}

void note_profiles();   // (vr/input/openxr_controllers.inc)

void poll_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* e = (const XrEventDataSessionStateChanged*)&ev;
            logf("vr: session state %d (at %.1f s)", (int)e->state, run_s());
            if (e->state == XR_SESSION_STATE_SYNCHRONIZED) flat_screen.reopen = true;
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult br = xrBeginSession(s.session, &bi);   // (its answer, once, in the log)
                if (XR_SUCCEEDED(br)) s.session_running = true;
                logf("vr: xrBeginSession -> %d (%s)", (int)br, XR_SUCCEEDED(br) ? "session running" : "FAILED");
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(s.session);
                s.session_running = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {   // (a controller connected or changed: the
            // profile the runtime now binds, logged)
            note_profiles();
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {   // (a recenter by the runtime: the Window
            // view's frame stays where the room's space now puts it; the menu's Place window moves it)
            const auto* e = (const XrEventDataReferenceSpaceChangePending*)&ev;
            logf("vr: [%.1f s] the runtime's %s space is changing (a recenter by the headset)", run_s(),
                 e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? "room (LOCAL)" : e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? "head" : "other");
            if (e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) flat_screen.reopen_at = std::max<XrTime>(e->changeTime, 1);
        } else {   // (another event: said for a player's log, bounded)
            static int told = 0;
            if (told++ < 20) logf("vr: [%.1f s] runtime event %d", run_s(), (int)ev.type);
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

/// The world screen's width, degrees seen from where it opened (the VR menu's Screen size)
int flat_width_deg() { return std::clamp(opts.vr_cutscene_screen > 0 ? opts.vr_cutscene_screen : (int)kFlatWidthDeg, 30, 120); }

/// How tall a flat picture of w x h need be on the world screen: about as many pixels across as the headset shows across
/// the screen (Screen size wide), from the runtime's recommended eye width over the eye's view, with a quarter more for
/// sharpness; at least 720, at most h; kFlatPictureH when the runtime does not say. (Any headset: a Quest 3, an Index, a
/// Pimax each get their own)
uint32_t flat_picture_h(uint32_t w, uint32_t h) {
    static uint32_t rec_w = ~0u;   // (the recommended eye width, asked once; 0: not known)
    if (rec_w == ~0u) {
        rec_w = 0;
        uint32_t n = 0;
        XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (s.inst && XR_SUCCEEDED(xrEnumerateViewConfigurationViews(s.inst, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                                     &n, v)) && n >= 1)
            rec_w = v[0].recommendedImageRectWidth;
        logf("vr: world screen: the headset's recommended eye width %u px", rec_w);
    }
    if (!w || !h) return 0;
    if (!rec_w || !(s.fov_tan_x > 0.1f)) return std::min(h, kFlatPictureH);
    const float px_per_tan = rec_w / (2.0f * s.fov_tan_x);   // (at the view's middle, where the screen is)
    const float across = 1.25f * px_per_tan * 2.0f * std::tan(flat_width_deg() * 0.5f / 57.2958f);
    const uint32_t want = (uint32_t)(across * (float)h / (float)w);
    return std::clamp(want, std::min(h, 720u), h);
}

constexpr float kWarpNear = 0.35f;   // (From depth: nearer than this, the eyes' shift eases off: vr_warp_splat.comp distance_at)
// (v1.0.2, the VR menu's Near depth: the warp's near distance, Low 1 m, Medium 0.6 m, Full kWarpNear. At 3D strength 100% a
// hand at 0.3 m before the ground at 1.5 m leaves a gap of about 12, 27 and 65 px beside it in each eye. Every distance is
// eased the same way, so near things keep their order and stay whole: a cap on each pixel's shift against its local
// background (a max-distance pyramid) would keep a large hand's middle at its full shift and its edges capped, tearing it)
inline float kWarpNearOf(int near_depth) { return near_depth >= 2 ? kWarpNear : near_depth == 1 ? 0.6f : 1.0f; }
// Extra camera coverage accommodates motion between rendering and display. Tangents are dimensionless, not degrees.
constexpr float kCameraGuardBand = 1.03f;
constexpr float kMaxCameraHalfFovTangent = 3.0f;

/// (v1.0.2) RX_VR_LAYER_FOV: located (the default) submits each eye at its own located FOV, cut from the picture;
/// symmetric, every eye at the picture's whole symmetric FOV, as before v1.0.2
bool layer_fov_located() {
    static const bool located = [] {
        const char* e = getenv("RX_VR_LAYER_FOV");
        const bool symmetric = e && !std::strcmp(e, "symmetric");
        if (e && *e) logf("vr: RX_VR_LAYER_FOV=%s: each eye's layer at %s", e,
                          symmetric ? "the picture's whole symmetric FOV (as before v1.0.2)" : "its own located FOV");
        return !symmetric;
    }();
    return located;
}

/// (v1.0.2) An eye's projection view at its own located FOV, widened by the guard band and kept inside the picture: the rect of
/// the eye's w x h picture (drawn symmetric, |P00| p00 and |P11| p11, so it covers tangents -1/p00..1/p00 across) that holds
/// that frustum, a tangent t at pixel x = (t p00 + 1) / 2 w (y the same way from the top), and the FOV of the whole pixels
/// cut. Before v1.0.2 each eye was the whole picture at its symmetric FOV, legal but not what a runtime expects (the spec's
/// fov "should almost always derive from" xrLocateViews): the suspect for flat, far-away stereo on Meta's PC runtime (Rift S,
/// CV1) while WiVRn and SteamVR looked right (research/oculus-stereo.md R1). False: the picture's whole FOV stays
bool located_eye_cut(const XrFovf& eye, float guard, float p00, float p11, uint32_t w, uint32_t h, XrFovf* fov, XrRect2Di* rect) {
    if (!(p00 > 1e-3f) || !(p11 > 1e-3f) || w < 16 || h < 16) return false;
    const float tx = 1.0f / p00, ty = 1.0f / p11;   // (the picture's half extent, in tangents)
    const float l = std::min(std::tan(-eye.angleLeft) * guard, tx), r = std::min(std::tan(eye.angleRight) * guard, tx);
    const float u = std::min(std::tan(eye.angleUp) * guard, ty), d = std::min(std::tan(-eye.angleDown) * guard, ty);
    if (!std::isfinite(l + r + u + d) || l + r < 0.2f || u + d < 0.2f) return false;
    const int32_t x0 = std::clamp((int32_t)std::floor((1.0f - l * p00) * 0.5f * (float)w), 0, (int32_t)w);
    const int32_t x1 = std::clamp((int32_t)std::ceil((1.0f + r * p00) * 0.5f * (float)w), 0, (int32_t)w);
    const int32_t y0 = std::clamp((int32_t)std::floor((1.0f - u * p11) * 0.5f * (float)h), 0, (int32_t)h);
    const int32_t y1 = std::clamp((int32_t)std::ceil((1.0f + d * p11) * 0.5f * (float)h), 0, (int32_t)h);
    if (x1 - x0 < 16 || y1 - y0 < 16) return false;
    rect->offset = {x0, y0}, rect->extent = {x1 - x0, y1 - y0};
    fov->angleLeft = std::atan((2.0f * (float)x0 / (float)w - 1.0f) / p00);
    fov->angleRight = std::atan((2.0f * (float)x1 / (float)w - 1.0f) / p00);
    fov->angleUp = std::atan((1.0f - 2.0f * (float)y0 / (float)h) / p11);
    fov->angleDown = std::atan((1.0f - 2.0f * (float)y1 / (float)h) / p11);
    return true;
}

// ---- the world screen's place (every RX_VR_FLAT_LAYER) ----
constexpr float kFlatLeftMetres = 2.0f;   // (the room-fixed screens: opened again once the head is this far from where it opened)
XrPosef flat_pose{{0, 0, 0, 1}, {0, 0, 0}};   // where the screen opened: your place and heading then, level

/// The world screen opened for a flat frame when it is not open: in front of where you are and face now, then fixed in the
/// room. Opened again after a recenter by the headset, its being put back on, or your walking off (not for projection, at no
/// distance, which stays as before)
void flat_place(const XrPosef& head) {
    const float away[3] = {head.position.x - flat_pose.position.x, head.position.y - flat_pose.position.y, head.position.z - flat_pose.position.z};
    const bool recentred = flat_screen.reopen_at && flat_screen.display_time >= flat_screen.reopen_at;
    const bool left = away[0] * away[0] + away[1] * away[1] + away[2] * away[2] > kFlatLeftMetres * kFlatLeftMetres;
    const bool again = flat_mode() != kFlatProjection && (flat_screen.reopen || recentred || left);
    if (recentred) flat_screen.reopen_at = 0;
    flat_screen.reopen = false;
    if (!flat_screen.open || again) {   // (where it opens: your place and heading then, level; then fixed in the room)
        const XrQuaternionf q = head.orientation;
        const float yaw = std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.x * q.x + q.y * q.y));
        flat_pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
        flat_pose.position = head.position;
        logf("vr: [%.1f s] world screen opened %.0f deg wide, %.1f m ahead (heading %.0f deg)", run_s(), (float)flat_width_deg(),
             kFlatScreenMetres, yaw * 57.2958f);
    }
    flat_screen.open = true;
}

/// The world screen for a flat picture of w x h: kFlatScreenMetres ahead of where it opened, Screen size wide from there
void flat_screen_set(uint32_t w, uint32_t h) {
    const float ahead[3] = {0, 0, -kFlatScreenMetres};
    float world[3];
    qrot(flat_pose.orientation, ahead, world);
    flat_screen.pose = flat_pose;
    flat_screen.pose.position.x += world[0], flat_screen.pose.position.y += world[1], flat_screen.pose.position.z += world[2];
    flat_screen.width = 2 * kFlatScreenMetres * std::tan(flat_width_deg() * 0.5f / 57.2958f);
    flat_screen.height = flat_screen.width * (float)h / (float)w;
    flat_screen.w = w, flat_screen.h = h;
    flat_screen.shown = true;
}

/// vr_flat_eyes.comp's push constants: a screen (its middle and turn, its size in metres) seen from an eye (pose, fov, in the
/// same space) in a view of px_w x px_h pixels
struct FlatEyePush { float v[24]; };
FlatEyePush flat_eye_push(const XrPosef& screen, float width, float height, const XrPosef& eye, const XrFovf& fov, float px_w, float px_h,
                          float samples) {
    FlatEyePush p{};
    const XrQuaternionf to_screen = qinv(screen.orientation);
    const float dp[3] = {eye.position.x - screen.position.x, eye.position.y - screen.position.y, eye.position.z - screen.position.z};
    qrot(to_screen, dp, p.v);
    p.v[4] = std::tan(fov.angleLeft), p.v[5] = std::tan(fov.angleRight), p.v[6] = std::tan(fov.angleDown), p.v[7] = std::tan(fov.angleUp);
    for (int i = 0; i < 3; i++) {
        const float unit[3] = {i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
        float world[3];
        qrot(eye.orientation, unit, world);
        qrot(to_screen, world, p.v + 8 + 4 * i);
    }
    p.v[11] = px_w, p.v[15] = px_h;
    p.v[20] = 0.5f * width, p.v[21] = 0.5f * height, p.v[22] = samples;
    return p;
}

// ---- a gameplay frame both eyes share one picture of (3D Mono, or no scene depth) ----
/// (v1.0.2) RX_VR_MONO_POSE: where its views are. eyes (the default): each eye's own pose, from the head the picture was drawn
/// for and the eye's located place and turn from the head; head: both at that head, as before
bool mono_pose_eyes() {
    static const bool eyes_pose = [] {
        const char* e = getenv("RX_VR_MONO_POSE");
        const bool head = e && !std::strcmp(e, "head");
        if (e && *e) logf("vr: RX_VR_MONO_POSE=%s: a picture both eyes share is sent at %s", e, head ? "the drawn head for both" : "each eye's own pose");
        return !head;
    }();
    return eyes_pose;
}
struct SharedPose {   // (this frame's gameplay picture, for the log: tester_log.inc)
    bool shown = false, own = false, turned = false;   // one picture for both eyes; at each eye's own pose; resampled for its turn
    bool eye2 = false;
    XrPosef drawn{};
    uint32_t w = 0, h = 0;
} shared_pose;
bool turned_eye_ready(const Image& picture);   // (flat_eyes.inc)
void turned_eye_record(const FlatEyePush& push, VkImage dst, uint32_t w, uint32_t h);

/// An eye located from the head (finite, a turn, a place within reach)
bool eye_pose_usable(const XrView& v) {
    const XrQuaternionf& q = v.pose.orientation;
    const XrVector3f& p = v.pose.position;
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, d = p.x * p.x + p.y * p.y + p.z * p.z;
    return std::isfinite(n) && std::fabs(n - 1) < 0.01f && std::isfinite(d) && d < 0.04f && std::isfinite(v.fov.angleLeft + v.fov.angleRight +
           v.fov.angleUp + v.fov.angleDown) && v.fov.angleLeft < v.fov.angleRight && v.fov.angleDown < v.fov.angleUp;
}
/// Whether an eye is turned from the head (a canted display): by more than 0.1 degrees
bool eye_turned(const XrQuaternionf& q) { return q.x * q.x + q.y * q.y + q.z * q.z > 0.000873f * 0.000873f; }

/// An eye's pose for a picture drawn for a head: the head's, with the eye's place from it, and its turn when the picture is
/// resampled for it
XrPosef shared_eye_pose(const XrPosef& drawn, const XrPosef& eye, bool turned) {
    XrPosef p = drawn;
    const float at[3] = {eye.position.x, eye.position.y, eye.position.z};
    float world[3];
    qrot(drawn.orientation, at, world);
    p.position.x += world[0], p.position.y += world[1], p.position.z += world[2];
    if (turned) p.orientation = qmul(drawn.orientation, eye.orientation);
    return p;
}

/// A turned eye's view of a picture (w x h, symmetric, |P00| p00 and |P11| p11) drawn along the head: the eye's own FOV widened by
/// guard (or the picture's whole, whole), at the picture's own pixels to a tangent, and the push constants that draw it. The
/// picture is at infinity (both eyes see one direction at one place): only the turn moves it, and past its edge is black
FlatEyePush turned_eye_view(const XrPosef& eye, const XrFovf& own, bool whole, float guard, float p00, float p11, uint32_t w, uint32_t h,
                            XrFovf* fov, XrRect2Di* rect) {
    const float tx = 1 / p00, ty = 1 / p11;
    const auto t = [&](float angle, float all) { return whole ? all : std::clamp(std::tan(angle) * guard, -3.0f, 3.0f); };
    const float l = t(own.angleLeft, -tx), r = t(own.angleRight, tx), u = t(own.angleUp, ty), d = t(own.angleDown, -ty);
    rect->offset = {0, 0};
    rect->extent = {std::clamp((int32_t)std::lround((r - l) * p00 * 0.5f * (float)w), 16, (int32_t)w),
                    std::clamp((int32_t)std::lround((u - d) * p11 * 0.5f * (float)h), 16, (int32_t)h)};
    *fov = {std::atan(l), std::atan(r), std::atan(u), std::atan(d)};
    return flat_eye_push(XrPosef{{0, 0, 0, 1}, {0, 0, -1}}, 2 * tx, 2 * ty, XrPosef{eye.orientation, {0, 0, 0}}, *fov,
                         (float)rect->extent.width, (float)rect->extent.height, 1);
}

/// Each eye from its slot, for the head now (ke[0] the left eye's, ke[1] the right's: the same slot); true when the views were
/// filled
bool make_eyes(const int ke[2], int kn, const XrPosef& head, XrCompositionLayerProjectionView views[2]) {
    if (!headset_gpu_free()) return false;   // (before the eye images and the descriptor sets below: a pending submit reads them)
    // (Flat frames (title, cutscenes, other non-gameplay views) are a screen fixed in the room, in front of where you faced when
    // the flat view began: no longer stuck to the head, and kFlatWidthDeg wide instead of the whole view. Gameplay frames that
    // match no drawn pose stay centred on the head. A flat frame is always both eyes' slot.)
    const bool flat = s.slots[ke[0]].head_locked;
    if (flat) flat_place(head);
    else flat_screen.open = false;
    flat_screen.shown = false;
    struct EyeFrame {
        XrPosef drawn{};
        float shift = 0;          // (true stereo) the eye's picture's camera, along the drawn head's right (metres)
        uint32_t w = 0, h = 0;
        bool projection_ok = false;
        float proj_x = 1, proj_y = 1;
        bool own = false, turned = false;   // (one picture for both eyes: the view at the eye's own pose, the picture resampled for its turn)
        float layer[2]{};         // where the eye's picture is shown from the drawn head (x right, y up)
    } ef[2];
    WarpPush push[2];
    const uint32_t cap = opts.vr_eye_height > 0 ? (uint32_t)opts.vr_eye_height : 0;
    const float scale = opts.vr_depth_scale / 100.0f;
    XrPosef eye_pose[2];
    XrFovf eye_fov[2];
    bool eye_pose_ok;
    {
        std::lock_guard lk(s.pose_mu);
        eye_pose_ok = s.eye_pose_ok;
        for (int e = 0; e < 2; e++) eye_pose[e] = s.eye_pose[e], eye_fov[e] = s.eye_fov[e];
    }
    for (int e = 0; e < 2; e++) {
        Slot& sl = s.slots[ke[e]];
        EyeFrame& f = ef[e];
        if (sl.head_locked) f.drawn = flat_pose;
        else if (sl.drawn_ok) f.drawn = sl.drawn;
        else if (!drawn_pose(sl.vp, sl.proj, &f.drawn)) f.drawn = head;
        if (!sl.head_locked && sl.eyes_mode) f.shift = sl.eye_shift[sl.has_eye2 ? e : 0];   // (true stereo: where each picture was drawn)
        // the eye's size: the picture's, at most opts.vr_eye_height tall
        f.w = sl.color.w, f.h = sl.color.h;
        // (a flat frame: about the size the world screen is in the headset (flat_picture_h). The title's picture is the
        // headset-shaped 2880x2304; shown on a quad fixed in the room it was minified about twice over (a Quest 3), without
        // mipmaps, and shimmered as the head moved (most along its edges, headset 22 Nov); the warp's copy scales it down
        // once instead, the same every frame)
        const uint32_t fh = sl.head_locked ? flat_picture_h(f.w, f.h) : 0;
        const uint32_t pc = fh ? (cap ? std::min(cap, fh) : fh) : cap;
        if (pc && f.h > pc) f.w = (uint32_t)((uint64_t)f.w * pc / f.h) & ~1u, f.h = pc;
        // (a flat frame's smaller picture goes in the top left of the eye's swapchain when it fits, the quad showing just that
        // rectangle: no new swapchain at each change between a flat frame and play, a quick select's opening among them)
        const bool fits = sl.head_locked && eyes[e].chain && eyes[e].w >= f.w && eyes[e].h >= f.h;
        if (!fits && !make_eye(e, f.w, f.h)) return false;
        if (!make_image(s.eye_out[e], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, f.w,
                        f.h) ||
            !make_image(s.landed[e], VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, f.w, f.h))
            return false;
        // the head's movement since the picture was drawn, in the picture's own frame
        const float dp[3] = {head.position.x - f.drawn.position.x, head.position.y - f.drawn.position.y,
                             head.position.z - f.drawn.position.z};
        float d[3];
        qrot(qinv(f.drawn.orientation), dp, d);
        // Before gameplay (logos, title and movies) there may be no scene projection yet. A zero projection made each eye's FOV
        // approach 180 degrees. Use a stable 90-degree horizontal flat view until a real game camera is available.
        f.projection_ok = std::isfinite(sl.proj[0]) && std::isfinite(sl.proj[5]) && std::isfinite(sl.proj[10]) &&
                          std::isfinite(sl.proj[11]) && std::fabs(sl.proj[0]) > 1e-5f && std::fabs(sl.proj[5]) > 1e-5f &&
                          sl.proj[14] == -1.0f;
        f.proj_x = f.projection_ok ? std::fabs(sl.proj[0]) : 1.0f;
        f.proj_y = f.projection_ok ? std::fabs(sl.proj[5]) : (f.h ? (float)f.w / (float)f.h : 1.0f);
        // the depth's distances (fsr3_math.h: d = a + b / v with a = (1 - p22) / 2, b = p23 / 2, the game's -1..1 clip depth).
        // The game's depth is 0 at the near plane and 1 at the far plane, so the near plane is v at d = 0, -b / a =
        // p23 / (p22 - 1), and the far plane v at d = 1, b / (1 - a). (Before v1.0.2 the near plane was taken as b / (1 - a):
        // the far plane, 24672 m for the game's 0.1 to 25000 m, and the warp clamped every distance to it, so From depth
        // moved nothing on any headset)
        const float a = f.projection_ok ? (1.0f - sl.proj[10]) * 0.5f : 0.0f;
        const float b = f.projection_ok ? sl.proj[11] * 0.5f : 1.0f;
        const float near_plane = f.projection_ok && std::fabs(sl.proj[10] - 1.0f) > 1e-9f ? sl.proj[11] / (sl.proj[10] - 1.0f) : 0.05f;
        const float far_plane = f.projection_ok && std::fabs(1.0f - a) > 1e-9f ? b / (1.0f - a) : 0.0f;
        if (e == 0 && !sl.head_locked && f.projection_ok) {   // (bounded: the planes the picture was drawn with)
            static float said = -1;
            static int lines = 0;
            if (lines < 40 && std::fabs(near_plane - said) > 0.001f)
                lines++, said = near_plane,
                    logf("vr: the eyes' picture: near %.3f m, far %.1f m", near_plane, far_plane);
        }
        // (a flat frame is shown as it was drawn: on the world screen, a quad fixed in the room, which the compositor turns for
        // the head itself. The depth warp's stereo shift (the eye's half and the head's movement since the screen opened)
        // slid the title's movie inside its own picture, and its uncovered rim moved with the head, headset 22 Nov)
        const bool stereo = !sl.head_locked && opts.vr_stereo != 0 && sl.has_depth;
        f.own = !sl.head_locked && !stereo && eye_pose_ok && mono_pose_eyes();
        f.turned = f.own && f.projection_ok && eye_turned(eye_pose[e].orientation);
        // this eye's place from the drawn head (true stereo: from where the eye's own picture was drawn, f.shift)
        const float half = (e ? 0.5f : -0.5f) * s.ipd * (stereo ? scale : 0.0f);
        WarpPush& p = push[e];
        std::memset(&p, 0, sizeof p);
        p.out_size[0] = (float)f.w, p.out_size[1] = (float)f.h;
        p.uv_scale[0] = p.uv_scale[1] = 1;
        p.proj[0] = f.proj_x, p.proj[1] = f.proj_y, p.proj[2] = -a, p.proj[3] = b;
        // (RX_VR_WARP_HEAD=0: the head's movement since the picture was drawn left to the compositor, the warp makes the eyes'
        // offset only. With From depth's real near plane the warp moves near things by that movement too, every frame, so
        // the gaps beside them change every frame: the suspect for a haze round Link's body, 23 Nov)
        static const bool head_motion = [] {
            const char* v = getenv("RX_VR_WARP_HEAD");
            const bool off = v && *v == '0';
            if (off) logf("vr: RX_VR_WARP_HEAD=0: the eyes' warp leaves the head's movement since the picture to the compositor");
            return !off;
        }();
        if (!head_motion) d[0] = d[1] = d[2] = 0;
        // (the eyes' offset and the head's movement apart: the warp fades the head's out for points nearer than about
        // 0.8 m (vr_warp_splat.comp shift_at), which follow the head (Link's body, what his hands hold) and are not fixed in
        // the world, 23 Nov)
        p.offset[0] = stereo ? half - f.shift : 0;
        p.offset[1] = 0;
        p.head[0] = stereo ? d[0] : 0, p.head[1] = stereo ? d[1] : 0;
        p.steps = (float)std::max(4, opts.vr_quality);
        // (research: RX_VR_WARP_DEBUG=1, the left eye shows the depth as the warp reads it (lighter nearer) and the right eye
        // the picture unwarped, both of one frame: what has no depth of its own (drawn after the copy, or without depth)
        // shows as what is behind it)
        static const bool depth_view = [] {
            const char* v = vr_research_getenv("RX_VR_WARP_DEBUG");
            const bool on = v && *v == '1';
            if (on) logf("vr: RX_VR_WARP_DEBUG=1 (test): the left eye shows its depth, the right the picture unwarped");
            return on;
        }();
        p.debug = depth_view ? (e == 0 ? 1.0f : 2.0f) : 0.0f;
        p.max_shift = 0.08f;
        // (the warp's own near distance, not the camera's: nearer points (Link's hands at the face) are eased onto it, so their
        // shift stays within what the eye can fuse and the gaps behind them stay small. The game's camera keeps its near plane)
        p.near_plane = std::max(kWarpNearOf(opts.vr_near_depth), near_plane > 0 ? near_plane : 0.05f);
        f.layer[0] = stereo ? half + d[0] : 0;
        f.layer[1] = p.head[1];
    }
    // descriptors (the slots' images stay, so once per slot and eye output)
    static uint64_t set_for[kSlots];
    const uint64_t generation = image_generation.load() + 1;   // (+1: never the 0 of a set not yet written)
    for (int e = 0; e < 2; e++) {
        const int k = ke[e];
        Slot& sl = s.slots[k];
        const uint64_t want = generation * 2 + (sl.has_eye2 ? 1 : 0);   // (true stereo: the right eye's own images in its set)
        if (set_for[k] == want) continue;
        for (int o = 0; o < 2; o++) {
            const bool own = o == 1 && sl.has_eye2;
            VkDescriptorImageInfo ii[4] = {{s.linear, own ? sl.color2.view : sl.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {s.point, own ? sl.depth2.view : sl.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, s.eye_out[o].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, s.landed[o].view, VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet w3[4];
            for (int i = 0; i < 4; i++) {
                w3[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w3[i].dstSet = s.warp_sets[k][o];
                w3[i].dstBinding = (uint32_t)i;
                w3[i].descriptorCount = 1;
                w3[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w3[i].pImageInfo = &ii[i];
            }
            vkUpdateDescriptorSets(dev.device, 4, w3, 0, nullptr);
        }
        set_for[k] = want;
    }
    FlatEyePush turned_push[2];
    XrFovf turned_fov[2];
    XrRect2Di turned_rect[2];
    if ((ef[0].turned || ef[1].turned) && !turned_eye_ready(s.slots[ke[0]].color)) ef[0].turned = ef[1].turned = false;
    for (int e = 0; e < 2; e++)
        if (ef[e].turned)
            turned_push[e] = turned_eye_view(eye_pose[e], eye_fov[e], !layer_fov_located(), kCameraGuardBand, ef[e].proj_x, ef[e].proj_y,
                                             ef[e].w, ef[e].h, &turned_fov[e], &turned_rect[e]);
    shared_pose.shown = !flat && !(opts.vr_stereo != 0 && s.slots[ke[0]].has_depth);
    shared_pose.turned = ef[0].turned || ef[1].turned, shared_pose.own = ef[0].own;
    shared_pose.eye2 = s.slots[ke[0]].has_eye2, shared_pose.drawn = ef[0].drawn, shared_pose.w = ef[0].w, shared_pose.h = ef[0].h;
    uint32_t index[2];
    SwapTake take;
    if (!swap_take(take, {eyes[0].chain, eyes[1].chain}, index, "eyes")) return false;
    if (!headset_record()) return swap_give(take), false;
    for (int e = 0; e < 2; e++) {
        const EyeFrame& f = ef[e];
        if (f.turned) {   // (one picture for both eyes, this eye turned from the head: drawn as the turned eye sees it)
            turned_eye_record(turned_push[e], eyes[e].images[index[e]].image, (uint32_t)turned_rect[e].extent.width,
                              (uint32_t)turned_rect[e].extent.height);
            continue;
        }
        // (3D Mono, v1.0.2 (opts.vr_stereo 0): a gameplay frame goes to both eyes as the game drew it, a bit copy of the
        // picture into the swapchain: no warp, the cheapest. A picture made smaller (Eye resolution) still goes through it)
        const Slot& esl = s.slots[ke[e]];
        const bool flat_copy = opts.vr_stereo == 0 && !esl.head_locked && f.w == esl.color.w && f.h == esl.color.h;
        if (flat_copy) {
            const VkImage dst = eyes[e].images[index[e]].image;
            barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {f.w, f.h, 1};
            vkCmdCopyImage(cmd, esl.color.image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            continue;
        }
        barrier(cmd, s.eye_out[e].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        // (From depth in two passes: each source pixel landed where the eye sees it, the nearest kept (vr_warp_splat.comp),
        // then each eye pixel from the one that landed there, or a hole filled from the background (vr_warp.comp))
        barrier(cmd, s.landed[e].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue none{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, s.landed[e].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &none, 1, &all);
        barrier(cmd, s.landed[e].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.warp_layout, 0, 1, &s.warp_sets[ke[e]][e], 0, nullptr);
        vkCmdPushConstants(cmd, s.warp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(WarpPush), &push[e]);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.warp_splat);
        vkCmdDispatch(cmd, (f.w + 7) / 8, (f.h + 7) / 8, 1);
        memory_barrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.warp);
        vkCmdDispatch(cmd, (f.w + 7) / 8, (f.h + 7) / 8, 1);
        barrier(cmd, s.eye_out[e].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkImage dst = eyes[e].images[index[e]].image;
        barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        // (research, the world screen's quad: its right and bottom edges' pixels moved with the head, D 21 Nov and the
        // Window view's menus 22 Nov. A flat picture smaller than the swapchain goes in its top left, the rest of the image
        // left undefined (the transition from UNDEFINED): RX_VR_FLAT_EDGE=magenta|black fills the whole image first, to see
        // whether the edge shows that colour (the compositor sampling past the rectangle) or stays as it was)
        static const int edge_fill = [] {
            const char* v = vr_research_getenv("RX_VR_FLAT_EDGE");
            const int fill = !v ? 0 : !std::strcmp(v, "magenta") ? 1 : !std::strcmp(v, "black") ? 2 : 0;
            if (fill) logf("vr: RX_VR_FLAT_EDGE=%s (test): a flat frame's swapchain image filled before its picture", v);
            return fill;
        }();
        if (edge_fill && flat) {
            const VkClearColorValue colour = {{edge_fill == 1 ? 1.0f : 0.0f, 0.0f, edge_fill == 1 ? 1.0f : 0.0f, 1.0f}};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &all);
            memory_barrier(cmd);
        }
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {f.w, f.h, 1};
        vkCmdCopyImage(cmd, s.eye_out[e].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    // (one timeline: the later of the two slots' values covers both)
    if (!headset_submit(take, s.timeline, std::max(s.slots[ke[0]].value, s.slots[ke[1]].value), "eyes")) return false;
    // each eye where the warp put it: the picture's turn, moved by the eye's offset and the head's movement
    // The game camera is widened to this same symmetric OpenXR envelope in hos_main.c. Advertising it here makes the
    // projection layer cover the headset instead of leaving the game's narrower camera as a floating rectangle.
    // (Each picture is shown at the FOV it was really drawn with: its own projection. Showing it at the FOV asked of the game
    // instead stretched and squashed the world whenever the game's camera hadn't (yet) taken that FOV.)
    // (A flat frame (title, menus, cutscenes) keeps square pixels: its height follows its width by the picture's own shape, so
    // a 5:4 picture is not stretched to a 16:9 camera's FOV.)
    bool cut_both = true;   // (each eye's view its own located FOV: the layer line below)
    for (int e = 0; e < 2; e++) {
        const EyeFrame& f = ef[e];
        const float fov_tan_x = flat ? std::tan(flat_width_deg() * 0.5f / 57.2958f)
                                : f.projection_ok ? 1.0f / f.proj_x : std::max(0.1f, s.fov_tan_x);
        const float fov_tan_y = flat ? fov_tan_x * (float)f.h / (float)f.w
                                     : f.projection_ok ? 1.0f / f.proj_y : std::max(0.1f, s.fov_tan_y);
        if (e == 0) {
            static auto last = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (!flat && now - last > std::chrono::seconds(5)) {
                last = now;
                logf("vr: fov asked %.1f x %.1f deg, drawn %.1f x %.1f deg", 2 * std::atan(s.fov_tan_x) * 57.2958f,
                     2 * std::atan(s.fov_tan_y) * 57.2958f, 2 * std::atan(fov_tan_x) * 57.2958f,
                     2 * std::atan(fov_tan_y) * 57.2958f);
            }
        }
        // (flat: both eyes see the picture from where it was drawn)
        const float lp[3] = {f.layer[0], f.layer[1], 0};
        float world[3];
        qrot(f.drawn.orientation, lp, world);
        XrPosef pose = f.drawn;
        pose.position.x += world[0], pose.position.y += world[1], pose.position.z += world[2];
        // (one picture for both eyes: each view at its own eye, from the head the picture was drawn for, so the compositor
        // still makes up the head's movement since; the picture is at infinity, the same from both)
        if (f.own) pose = shared_eye_pose(f.drawn, eye_pose[e], f.turned);
        XrCompositionLayerProjectionView& v = views[e];
        v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        v.pose = pose;
        v.fov = {-std::atan(fov_tan_x), std::atan(fov_tan_x), std::atan(fov_tan_y), -std::atan(fov_tan_y)};
        v.subImage.swapchain = eyes[e].chain;
        v.subImage.imageRect.extent = {(int32_t)f.w, (int32_t)f.h};
        // (v1.0.2, gameplay: the eye's own located FOV, its frustum cut from the picture. The picture and the game's camera
        // stay the symmetric FOV enclosing both eyes; a flat frame keeps the world screen's)
        if (f.turned) {
            v.fov = turned_fov[e], v.subImage.imageRect = turned_rect[e];
        } else if (!flat && f.projection_ok && layer_fov_located()) {
            XrFovf own;
            bool ok;
            {
                std::lock_guard lk(s.pose_mu);
                ok = s.eye_fov_ok, own = s.eye_fov[e];
            }
            XrFovf cut;
            XrRect2Di rect;
            if (ok && located_eye_cut(own, kCameraGuardBand, f.proj_x, f.proj_y, f.w, f.h, &cut, &rect))
                v.fov = cut, v.subImage.imageRect = rect;
            else cut_both = false;
        } else {
            cut_both = false;
        }
    }
    if (!flat) {   // (said once, and again when it changes (bounded): the layer as submitted, for a player's log)
        static int lines = 0;
        static std::string said;
        char line[800], at[160];
        const auto deg = [](float a) { return a * 57.2958f; };
        const Slot& sl = s.slots[ke[0]];
        if (ef[0].own)
            snprintf(at, sizeof at, "each eye's own from the drawn head, one picture for both (L %.0f %.0f %.0f mm, R %.0f %.0f %.0f mm%s)",
                     eye_pose[0].position.x * 1000, eye_pose[0].position.y * 1000, eye_pose[0].position.z * 1000, eye_pose[1].position.x * 1000,
                     eye_pose[1].position.y * 1000, eye_pose[1].position.z * 1000, shared_pose.turned ? "; turned eyes, resampled" : "");
        else if (shared_pose.shown) snprintf(at, sizeof at, "the drawn head for both eyes");
        else snprintf(at, sizeof at, "the drawn head -/+ %.1f mm (3D strength %d%%)", s.ipd * 500, opts.vr_depth_scale);
        snprintf(line, sizeof line,
                 "vr: layer: projection, 2 swapchains of %ux%u (%s), one per eye, array index 0; pose: %s; fov %s; L %.1f %.1f %.1f %.1f deg rect %d,%d %dx%d; R %.1f %.1f %.1f %.1f deg rect %d,%d %dx%d; the picture "
                 "%.1f x %.1f deg",
                 ef[0].w, ef[0].h, swap_format == VK_FORMAT_R8G8B8A8_SRGB ? "RGBA8 sRGB" : "RGBA8 UNORM", at,
                 shared_pose.turned ? "located (each turned eye's own)" : cut_both ? "located (each eye's own, kept within the picture)"
                 : layer_fov_located() ? "symmetric (no located FOV to cut)" : "symmetric (RX_VR_LAYER_FOV)", deg(views[0].fov.angleLeft), deg(views[0].fov.angleRight),
                 deg(views[0].fov.angleUp), deg(views[0].fov.angleDown), views[0].subImage.imageRect.offset.x,
                 views[0].subImage.imageRect.offset.y, views[0].subImage.imageRect.extent.width,
                 views[0].subImage.imageRect.extent.height, deg(views[1].fov.angleLeft), deg(views[1].fov.angleRight),
                 deg(views[1].fov.angleUp), deg(views[1].fov.angleDown), views[1].subImage.imageRect.offset.x,
                 views[1].subImage.imageRect.offset.y, views[1].subImage.imageRect.extent.width,
                 views[1].subImage.imageRect.extent.height, 2 * deg(std::atan(1.0f / ef[0].proj_x)),
                 2 * deg(std::atan(1.0f / ef[0].proj_y)));
        if (lines < 20 && said != line) {   // (the 3D's own state, this frame's: the 10 s 3D line has the counts)
            lines++, said = line;
            uint32_t sw, sh;
            {
                std::lock_guard lk(s.mu);
                sw = s.scene_w, sh = s.scene_h;
            }
            logf("%s; the scene drawn at %ux%u (%u%% of the picture's width); depth copy %s, each eye's own picture %s", line, sw, sh,
                 ef[0].w ? sw * 100 / ef[0].w : 0, sl.has_depth ? "yes" : "no",
                 sl.has_eye2 ? "yes" : sl.eyes_mode ? "no (the left eye's for both)" : opts.vr_stereo == 4 ? "no (not a two-eye frame)" : opts.vr_stereo == 0 ? "no (Mono)" : "no (From depth)");
        }
    }
    if (flat && ef[0].w && ef[0].h) {   // (the world screen: the left eye's picture, the same as the right's, on a quad)
        flat_screen_set(ef[0].w, ef[0].h);
    }
    return true;
}

/// The world screen's quad for this frame's eyes (make_eyes said it is a flat frame), in the room's space
bool flat_screen_quad(XrCompositionLayerQuad& quad) {
    if (!flat_screen.shown) return false;
    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = s.local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = eyes[0].chain;
    // (inset kFlatInsetPx on every side: pixels along the right and bottom edges moved with the head, headset 22 Nov, after
    // 7f2faa19. The swapchain is the picture's size and all written, so they are the picture's own edge (its last column
    // and row, which the movie's viewport, rounded for the headset's shape, may leave unwritten) or the compositor's
    // filtering past the rectangle's edge; neither reaches into an inset. The screen's size is unchanged)
    // (RX_VR_FLAT_INSET=<px>, a test: another inset, e.g. 16, for the edge's pixels that move with the head)
    static const int32_t kFlatInsetPx = [] {
        const char* v = vr_research_getenv("RX_VR_FLAT_INSET");
        const int32_t px = v ? std::clamp(atoi(v), 0, 64) : 2;
        if (v) logf("vr: RX_VR_FLAT_INSET=%d (test): the world screen's quad shows its picture inset that many pixels", px);
        return px;
    }();
    const int32_t w = (int32_t)flat_screen.w, h = (int32_t)flat_screen.h;
    const int32_t in = w > 8 * kFlatInsetPx && h > 8 * kFlatInsetPx ? kFlatInsetPx : 0;
    quad.subImage.imageRect.offset = {in, in};
    quad.subImage.imageRect.extent = {w - 2 * in, h - 2 * in};
    quad.pose = flat_screen.pose;
    quad.size = {flat_screen.width, flat_screen.height};
    return true;
}

bool make_menu_quad(int k, XrCompositionLayerQuad& quad) {
    MenuSlot& sl = s.menus[k];
    uint32_t w = sl.color.w, h = sl.color.h;
    const uint32_t cap = opts.vr_eye_height > 0 ? (uint32_t)opts.vr_eye_height : 0;
    if (cap && h > cap) w = ((uint32_t)((uint64_t)w * cap / h)) & ~1u, h = cap;
    if (!headset_gpu_free() || !w || !h || !make_menu_eye(w, h)) return false;
    uint32_t index = 0;
    SwapTake take;
    if (!swap_take(take, {menu_eye.chain}, &index, "menu quad")) return false;
    if (!headset_record()) return swap_give(take), false;
    const VkImage dst = menu_eye.images[index].image;
    barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)sl.color.w, (int32_t)sl.color.h, 1};
    b.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    vkCmdBlitImage(cmd, sl.color.image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_LINEAR);
    barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (!headset_submit(take, s.timeline, sl.value, "menu quad")) return false;

    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = s.view;                         // head locked, independent of the game's camera
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = menu_eye.chain;
    quad.subImage.imageRect.extent = {(int32_t)w, (int32_t)h};
    quad.pose.orientation.w = 1.0f;
    quad.pose.position.z = -kMenuDistanceMetres;
    quad.size.width = kMenuWidthMetres;
    quad.size.height = quad.size.width * (float)h / (float)w;
    return true;
}
