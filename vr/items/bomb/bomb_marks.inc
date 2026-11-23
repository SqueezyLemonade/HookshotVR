// Hookshot VR: the bombs' marks in the headset: the throw arc's dots and the target it lands on, PARKED with Bomb aim's
// Arc to target (vr/items/bomb/bomb_host.c says why, and where they are: hookshot_vr_bomb_marks). The pull's ring that
// was here too is gone: the bomb a hand would pull pulses instead (vr/items/gear/equipment_skin.inc).
//
// Included by vr/headset/vk_vr.cpp after radial_draw.inc (it uses barrier, cmd_pool, cmd, fence, s, swap_format and
// radial_facing). Not compiled on its own.
//
// Each mark is a quad layer floating in the room, as the bow's reticle is: drawn over the game's picture (the compositor
// knows no depth), so a mark behind a wall still shows. Two small pictures of our own, drawn once on the CPU side into
// static swapchains: a soft dot (the arc) and a ring with a centre dot (where the bomb lands). A game-world point is put
// in the room through the camera the game drew with (the first-person body's pose), as the bow's reticle is.
constexpr int kBombMarkPx = 64;
constexpr int kBombMarksMax = 12;           // (as many as the game side sends: bomb_host.c BOMB_MARKS_MAX)
constexpr int kBombMarkLayersMax = 16;      // (OpenXR promises at least 16 layers in all: the marks fill what is left)
enum BombMarkKind { kBombMarkDot = 1, kBombMarkTarget = 2 };
struct BombMark { int kind; float at[3]; float size; };
std::mutex bomb_marks_mu;
BombMark bomb_marks_in[kBombMarksMax];      // (bomb_marks_mu) as the game side last sent them
int bomb_marks_n = 0;
std::chrono::steady_clock::time_point bomb_marks_at{};
Eye bomb_mark_art[2];                       // the two pictures: dot, target
bool bomb_mark_ready[2]{}, bomb_mark_failed[2]{};

// (the game side, every game frame: 5 floats a mark, kind x y z size; n 0 clears them)
void bomb_marks_set(const float* m, int n) {
    std::lock_guard lk(bomb_marks_mu);
    bomb_marks_n = 0;
    for (int i = 0; m && i < n && bomb_marks_n < kBombMarksMax; i++) {
        const float* f = m + 5 * i;
        const int kind = (int)f[0];
        if (kind < kBombMarkDot || kind > kBombMarkTarget || !std::isfinite(f[1] + f[2] + f[3]) || !(f[4] > 0 && f[4] < 5)) continue;
        bomb_marks_in[bomb_marks_n++] = {kind, {f[1], f[2], f[3]}, f[4]};
    }
    bomb_marks_at = std::chrono::steady_clock::now();
}

// One picture: its alpha at a pixel (0..1) and its colour, premultiplied when written
float bomb_mark_alpha(int kind, float x, float y) {   // (x, y: -1..1 across the picture)
    const float r = std::sqrt(x * x + y * y);
    const auto band = [&](float from, float to) {      // (1 inside [from, to], soft edges a pixel and a half wide)
        const float soft = 3.0f / kBombMarkPx;
        return std::clamp(std::min(r - from, to - r) / soft + .5f, 0.0f, 1.0f);
    };
    if (kind == kBombMarkDot) return band(-1, .55f) * (1 - .4f * r);
    if (kind == kBombMarkTarget) return std::max(band(.72f, .92f), band(-1, .18f));
    return 0;
}

bool bomb_mark_make(int which) {
    const int kind = which + 1;
    if (bomb_mark_ready[which] || bomb_mark_failed[which]) return bomb_mark_ready[which];
    if (!headset_gpu_free()) return false;   // (not a failure: tried again once the headset's last submit is done)
    bomb_mark_failed[which] = true;
    Eye& m = bomb_mark_art[which];
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    ci.width = ci.height = kBombMarkPx;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &m.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(m.chain, 0, &n, nullptr);
    m.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(m.chain, n, &n, (XrSwapchainImageBaseHeader*)m.images.data());
    m.w = m.h = kBombMarkPx;
    uint32_t index = 0;
    SwapTake take;
    if (!swap_take(take, {m.chain}, &index, "bomb mark")) return false;
    if (!headset_record()) return swap_give(take), false;
    const VkImage img = m.images[index].image;
    barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue clear{{0, 0, 0, 0}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = (VkFormat)swap_format, vi.subresourceRange = range;
    VkImageView view = VK_NULL_HANDLE;
    const bool viewed = vkCreateImageView(dev.device, &vi, nullptr, &view) == VK_SUCCESS;
    if (viewed) {   // (painted as runs of equal pixels, cleared one rectangle at a time, as the bow reticle's art is)
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = view, attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {kBombMarkPx, kBombMarkPx}, rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1, rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(cmd, &rendering);
        // colours (sRGB values): the arc warm white, the target orange
        const float colour[2][3] = {{1.0f, .95f, .80f}, {1.0f, .55f, .15f}};
        const auto level = [&](int x, int y) {   // (the alpha in 16 steps, so runs of equal pixels are long)
            const float fx = (x + .5f) * 2 / kBombMarkPx - 1, fy = (y + .5f) * 2 / kBombMarkPx - 1;
            return (int)std::lround(bomb_mark_alpha(kind, fx, fy) * 15);
        };
        for (int y = 0; y < kBombMarkPx; y++)
            for (int x = 0; x < kBombMarkPx;) {
                const int a = level(x, y);
                int end = x + 1;
                while (end < kBombMarkPx && level(end, y) == a) ++end;
                if (a) {
                    VkClearAttachment ink{};
                    ink.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    const float f = a / 15.0f;
                    ink.clearValue.color = {{colour[which][0] * f, colour[which][1] * f, colour[which][2] * f, f}};
                    const VkClearRect rect{{{x, y}, {(uint32_t)(end - x), 1}}, 0, 1};
                    vkCmdClearAttachments(cmd, 1, &ink, 1, &rect);
                }
                x = end;
            }
        vkCmdEndRendering(cmd);
    }
    bool ok = false;
    if (viewed)   // (its view destroyed once the submit is done, whenever that is: before, a failed one's leaked)
        ok = headset_submit(take, VK_NULL_HANDLE, 0, "bomb mark", [view] { vkDestroyImageView(dev.device, view, nullptr); });
    else
        vkEndCommandBuffer(cmd), swap_give(take);
    bomb_mark_ready[which] = ok, bomb_mark_failed[which] = !ok;
    if (ok) logf("vr: bomb mark %d ready", kind);
    return ok;
}

// A rotation whose +z (the quad's face) points along `up`: a mark lying flat on the ground, facing the sky
XrQuaternionf bomb_mark_flat(const float up[3]) {
    const float n = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(n > 1e-4f)) return {-0.7071068f, 0, 0, 0.7071068f};
    const float u[3] = {up[0] / n, up[1] / n, up[2] / n};
    // (the shortest turn from +z to u: axis z x u, angle acos(z . u))
    const float w = 1 + u[2];
    if (w < 1e-4f) return {1, 0, 0, 0};
    const float len = std::sqrt(w * w + u[1] * u[1] + u[0] * u[0]);
    return {-u[1] / len, u[0] / len, 0, w / len};
}

/// The bombs' marks as quad layers into out (at most max), for this headset frame; returns how many
int bomb_mark_layers(XrCompositionLayerQuad* out, int max, const XrPosef& head) {
    BombMark marks[kBombMarksMax];
    int n = 0;
    {
        std::lock_guard lk(bomb_marks_mu);
        if (std::chrono::steady_clock::now() - bomb_marks_at < std::chrono::milliseconds(250))
            for (n = 0; n < bomb_marks_n; n++) marks[n] = bomb_marks_in[n];
    }
    if (!n || max <= 0 || opts.vr_mode != 1 || s.menu_visible.load()) return 0;
    // (the camera the game drew the newest first-person body with: game world -> room, as the bow's reticle)
    LinkBody body;
    if (!link_body(body)) return 0;
    Pose pose{};
    {
        std::lock_guard lk(s.pose_mu);
        const Pose& p = s.poses[body.pose_seq % kPoses];
        if (p.seq != body.pose_seq || !p.drawn) return 0;
        pose = p;
    }
    const auto room = [&](const float w[3]) {
        float camera[3], local[3];
        for (int row = 0; row < 3; row++) {
            camera[row] = pose.view[row * 4 + 3];
            for (int j = 0; j < 3; j++) camera[row] += pose.view[row * 4 + j] * w[j];
        }
        qrot(pose.pose.orientation, camera, local);
        return XrVector3f{pose.pose.position.x + local[0], pose.pose.position.y + local[1], pose.pose.position.z + local[2]};
    };
    float up_room[3];   // (the game world's up, in the room: the target lies flat on it)
    {
        float camera[3];
        for (int row = 0; row < 3; row++) camera[row] = pose.view[row * 4 + 1];
        qrot(pose.pose.orientation, camera, up_room);
    }
    int used = 0;
    // (the target first: it matters more than the arc's last dots when layers run short)
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n && used < max; i++) {
            const BombMark& m = marks[i];
            if ((m.kind == kBombMarkDot) != (pass == 1) || !bomb_mark_make(m.kind - 1)) continue;
            XrCompositionLayerQuad& q = out[used++];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = s.local;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = bomb_mark_art[m.kind - 1].chain;
            q.subImage.imageRect.extent = {kBombMarkPx, kBombMarkPx};
            q.pose.position = room(m.at);
            q.pose.orientation = m.kind == kBombMarkTarget ? bomb_mark_flat(up_room) : radial_facing(q.pose.position, head.position);
            q.size = {m.size, m.size};
        }
    static std::atomic<bool> told{false};
    if (used && !told.exchange(true)) logf("vr: bombs: %d marks shown (the throw arc and its target)", used);
    return used;
}
