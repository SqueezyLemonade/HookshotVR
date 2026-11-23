// Hookshot VR: Shared frame/pose state, GPU images, depth views and warp pipelines.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- state -------------------------------------------------------------------------------------------------------------------

constexpr int kSlots = 4;   // (the headset's copies of the game's pictures: one being written while the eyes read another)
constexpr int kMenuSlots = 3;
constexpr int kPoses = 64;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
};

struct Slot {
    Image color, depth;        // the picture (RGBA8) and its scene depth (R32F, the scene's size)
    uint64_t value = 0;        // the timeline's value once the copies are done (0: empty)
    float vp[16]{}, proj[16]{};   // the scene's camera when it was drawn
    bool has_depth = false;
    bool head_locked = false;  // a flat overlay: keep it centred instead of matching the game's camera pose
    bool menu169 = false;      // a full-screen 2D menu (menu_tick): the picture's middle 16:9, shown flat on the world screen
    // (found at the present) the head pose the game drew it with
    bool drawn_ok = false;
    XrPosef drawn{};
    float view[12]{};          // the game view it was drawn with (rows: right, up, back and their translations; world to camera)
    // (true stereo) the right eye's own picture and depth: the second layer of the game's (vk_vr_eyes.cpp); eye_shift: each
    // picture's camera along the drawn head's right (metres)
    Image color2, depth2;
    bool has_eye2 = false;
    bool eyes_mode = false;    // (drawn in true stereo: the picture is the left eye's, from eye_shift[0])
    float eye_shift[2]{};
};

struct MenuSlot {
    Image color;                   // transparent Hookshot overlay, not a warped game frame
    uint64_t value = 0;            // timeline value after its render is complete
};

struct Pose {
    uint32_t seq = 0;
    XrPosef pose{};            // the head in the runtime's local space
    float view[12]{};          // the view the game built with it (hookshot_vr_drawn), all 0 until then
    bool drawn = false;
};

/// (BODY-11) Link's root and facing as the game side told them with one camera pose (hookshot_vr_body), and the equipment
/// it told with it (hookshot_vr_equipment)
struct BodySample {
    uint32_t seq = 0;          // the pose the game built that frame's camera with (0: no first-person body that frame)
    uint64_t gen = 0;          // which body this is, counted from the start (one per game camera update)
    bool active = false, child = true;
    float root[3]{}, right[3]{1, 0, 0}, back[3]{0, 0, 1};
    uint64_t equipment = 0;    // the equipment told with it (its EquipmentSample's gen; 0: none told with this pose)
    float crawl = 0, lowered = 0;   // a crawl told with it (hookshot_vr_crawl): how far into it, how far the view came down (m)
};
struct EquipmentSample {
    uint64_t gen = 0;          // which this is, counted from the start (equipment[gen % kBodies])
    uint32_t seq = 0, known = 0, drawn = 0;
    uint64_t owner[2]{};
};
constexpr int kBodies = 16;   // (a quarter of a second at 60 game frames a second: far more than the replay's lag)

struct State {
    bool wanted = false, matched = false, running = false;
    XrInstance inst = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    std::vector<std::string> inst_exts, dev_exts;   // (the runtime's lists, kept: the names are handed out as pointers)
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex queue_mu;                // (the headset thread's frame, and the renderer's device-wide waits: queue_mutex)
    uint32_t family = 0;
    std::thread thread;
    std::atomic<bool> session_running{false};

    // the copies (the replay's and the present's threads)
    std::mutex mu;
    Slot slots[kSlots];
    int next_slot = 0;
    std::atomic<int> reading[2]{-1, -1};   // the slots the headset thread is making each eye from
    std::atomic<int> newest{-1};        // the newest slot with its copies recorded
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t timeline_value = 0;
    uint64_t pending_value = 0;         // the present's signal still to be added to its submit
    MenuSlot menus[kMenuSlots];
    int next_menu = 0, writing_menu = -1;
    std::atomic<int> reading_menu{-1};
    std::atomic<bool> menu_visible{false};
    // this frame's scene
    const Texture* scene_color = nullptr;
    Texture* scene_depth = nullptr;
    uint64_t scene_frame = ~0ull, scene_gen = 0;
    uint32_t scene_w = 0, scene_h = 0;
    float scene_vp[16]{}, scene_proj[16]{};
    Image depth_snap;                   // the depth once the world is in it (copied into the slot at the present)
    bool depth_snap_valid = false;
    Image depth_snap2;                  // (true stereo) the right eye's: the depth's second layer
    bool depth_snap2_valid = false;
    std::unordered_map<VkImage, VkImageView> depth_views1;   // (and its views)
    float snap_vp[16]{}, snap_proj[16]{};
    std::unordered_map<VkImage, VkImageView> depth_views;
    uint64_t depth_views_gen = 0;
    // (the depth views a texture generation's change let go, destroyed retire_frames presents later: retire_view)
    std::vector<std::pair<VkImageView, uint64_t>> retired_views;

    // the pipelines
    VkSampler linear = VK_NULL_HANDLE, point = VK_NULL_HANDLE;
    VkDescriptorSetLayout warp_dsl = VK_NULL_HANDLE, depth_dsl = VK_NULL_HANDLE;
    VkPipelineLayout warp_layout = VK_NULL_HANDLE, depth_layout = VK_NULL_HANDLE;
    VkPipeline warp = VK_NULL_HANDLE, depth_copy = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet warp_sets[kSlots][2]{};
    Image eye_out[2];

    // the head (the headset thread writes, the game thread reads)
    std::mutex pose_mu;
    Pose poses[kPoses];
    uint32_t pose_seq = 0;
    XrPosef ref{};                      // the recentred pose
    bool have_ref = false;
    int recenter_seen = -1;
    float ipd = 0.063f;
    float fov_tan_x = 1.0f, fov_tan_y = 1.0f; // OpenXR's two eye FOVs, enclosed by one symmetric game camera
    std::atomic<bool> camera_owned{false};   // false for titles, menus and scripted cameras
    std::atomic<bool> menu169{false};        // a full-screen 2D menu is up (menu_tick, each present)
    // Link's root and age from the guest thread, for the same camera pose. Kept with the pose ring so a replay cannot combine
    // controllers from a gameplay frame with a title screen, cutscene or third-person frame.
    uint32_t body_seq = 0;
    bool body_active = false, body_child = true;
    uint32_t equipment_seq = 0, equipment_known = 0, equipment_drawn = 0;
    uint32_t crawl_seq = 0;             // the pose the last crawl was told with (hookshot_vr_crawl), and that crawl
    float crawl_amount = 0, crawl_lowered = 0;
    uint64_t equipment_owner[2]{};
    float body_root[3]{};
    float body_right[3]{1, 0, 0}, body_back[3]{0, 0, 1};
    // (BODY-11) The last game frames' bodies and equipment, by pose number, so the replay poses each frame with the body the
    // game built THAT frame's camera with (vr/body/tracking.inc link_body_for_frame). The replay records a frame while the
    // game is already building the next one or two (vk_replay.cpp: "the replay recorded submits up to two presents older"):
    // the latest body above is then one or two frames newer than the camera the frame is drawn with, and while Link moves
    // his first-person body was drawn that much ahead of the view, a different amount each frame.
    BodySample bodies[kBodies];
    uint64_t body_gen = 0;              // bodies told so far (the game's camera updates); bodies[g % kBodies] has gen g
    EquipmentSample equipment[kBodies];
    uint64_t equipment_gen = 0;         // equipment told so far (searched by pose number)
    // the frame (texture_frame_no) whose scene camera was last matched to a drawn pose: note_scene_draw
    uint64_t matched_frame = ~0ull, matched_gen = 0;
    uint32_t matched_seq = 0;
} s;

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (dev.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// (made again each time an image of ours is: the warp's descriptor sets compare it, not image view handles, which the driver
// hands out again after a destroy: a changed Eye resolution kept eye 1's set on a destroyed view, and the GPU was lost)
std::atomic<uint64_t> image_generation{0};
bool make_image(Image& im, VkFormat format, VkImageUsageFlags usage, uint32_t w, uint32_t h) {
    if (im.image && im.format == format && im.w == w && im.h == h) return true;
    image_generation.fetch_add(1);
    if (im.view) vkDestroyImageView(dev.device, im.view, nullptr);
    if (im.image) vkDestroyImage(dev.device, im.image, nullptr);
    if (im.memory) vkFreeMemory(dev.device, im.memory, nullptr);
    im = Image{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev.device, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev.device, im.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev.device, &ai, nullptr, &im.memory) != VK_SUCCESS ||
        vkBindImageMemory(dev.device, im.image, im.memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vi, nullptr, &im.view) != VK_SUCCESS) return false;
    im.format = format, im.w = w, im.h = h;
    return true;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void memory_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

VkShaderModule shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev.device, &ci, nullptr, &m);
    return m;
}

struct WarpPush {
    float out_size[2], uv_scale[2];
    float proj[4];      // |P00|, |P11|, -a, b: the view distance of a depth d is b / (d - a) (fsr3_math.h's camera)
    float offset[4];    // the eye's offset from where the game drew: x right, y up (metres)
    float steps, max_shift, near_plane, debug;
};

bool make_pipelines() {
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = 0;
    if (vkCreateSampler(dev.device, &sc, nullptr, &s.linear) != VK_SUCCESS) return false;
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev.device, &sc, nullptr, &s.point) != VK_SUCCESS) return false;

    // the warp: the picture, its depth, an eye's storage image
    VkDescriptorSetLayoutBinding wb[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 3;
    dl.pBindings = wb;
    if (vkCreateDescriptorSetLayout(dev.device, &dl, nullptr, &s.warp_dsl) != VK_SUCCESS) return false;
    // the depth copy: the game's depth, an R32F storage image
    VkDescriptorSetLayoutBinding db[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    dl.bindingCount = 2;
    dl.pBindings = db;
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;   // (the replay's command buffers: pushed, no set in use)
    if (!dev.push_descriptor || vkCreateDescriptorSetLayout(dev.device, &dl, nullptr, &s.depth_dsl) != VK_SUCCESS) return false;

    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(WarpPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s.warp_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pl, nullptr, &s.warp_layout) != VK_SUCCESS) return false;
    VkPushConstantRange dpr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    pl.pSetLayouts = &s.depth_dsl;
    pl.pPushConstantRanges = &dpr;
    if (vkCreatePipelineLayout(dev.device, &pl, nullptr, &s.depth_layout) != VK_SUCCESS) return false;

    const auto compute = [](VkPipelineLayout layout, const uint32_t* code, size_t bytes, VkPipeline* out) {
        VkShaderModule m = shader(code, bytes);
        if (!m) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = layout;
        const bool ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, out) == VK_SUCCESS;
        vkDestroyShaderModule(dev.device, m, nullptr);
        return ok;
    };
    if (!compute(s.warp_layout, VR_WARP_COMP_SPV, sizeof VR_WARP_COMP_SPV, &s.warp) ||
        !compute(s.depth_layout, VR_DEPTH_COMP_SPV, sizeof VR_DEPTH_COMP_SPV, &s.depth_copy))
        return false;

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 2 * kSlots + 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSlots + 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2 * kSlots + 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev.device, &pi, nullptr, &s.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.pool;
    VkDescriptorSetLayout layouts[2 * kSlots];
    for (auto& l : layouts) l = s.warp_dsl;
    ai.descriptorSetCount = 2 * kSlots;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev.device, &ai, &s.warp_sets[0][0]) != VK_SUCCESS) return false;

    VkSemaphoreTypeCreateInfo tt{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tt.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &tt;
    return vkCreateSemaphore(dev.device, &si, nullptr, &s.timeline) == VK_SUCCESS;
}

// (VRAM flicker, 18 Nov) A depth view let go when the texture generation changes is destroyed 120 presents later, not at once: the
// frames recorded before the change still read it in their push descriptors (scene_done), and the replay records up to two
// presents ahead of the GPU. Destroyed at once, the driver could hand its slot to the next view made (an evicted texture made
// again, a compaction's moved one), and a frame still on the GPU copied the wrong image as the scene's depth: the From depth
// eyes warped by garbage for a frame. The generation changes at every eviction pass, so the more VRAM pressure (a dungeon:
// budget evictions, compaction, thousands of dead targets let go every minute), the more often. 120 is the engine's own wait for
// work in flight (vk_draw.cpp's old TAAU histories). RX_VR_DEPTH_VIEWS_NOW=1: destroyed at once, as before (to compare)
constexpr uint64_t kRetireFrames = 120;
void retire_view(VkImageView v) {
    static const bool at_once = vr_research_getenv("RX_VR_DEPTH_VIEWS_NOW") && atoi(vr_research_getenv("RX_VR_DEPTH_VIEWS_NOW")) != 0;   // (as before)
    if (v && at_once) vkDestroyImageView(dev.device, v, nullptr);
    else if (v) s.retired_views.push_back({v, texture_frame_no()});
}
void destroy_retired_views() {
    const uint64_t now = texture_frame_no();
    size_t keep = 0;
    for (auto& r : s.retired_views) {
        if (now - r.second >= kRetireFrames) vkDestroyImageView(dev.device, r.first, nullptr);
        else s.retired_views[keep++] = r;
    }
    s.retired_views.resize(keep);
}

VkImageView depth_view(Texture* t) {
    const uint64_t gen = texture_destroy_generation();
    if (gen != s.depth_views_gen) {
        for (auto& [image, view] : s.depth_views) (void)image, retire_view(view);
        s.depth_views.clear();
        s.depth_views_gen = gen;
        destroy_retired_views();
    }
    if (auto it = s.depth_views.find(t->image); it != s.depth_views.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t->format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    s.depth_views[t->image] = v;
    return v;
}

/// (true stereo) a sampled view of a two-layer depth's second layer, the right eye's
VkImageView depth_view1(Texture* t) {
    if (!t->vr_eyes) return VK_NULL_HANDLE;
    depth_view(t);   // (the generation's sweep, shared)
    if (s.depth_views1.size() && s.depth_views_gen != texture_destroy_generation()) s.depth_views1.clear();
    static uint64_t gen1 = 0;
    if (gen1 != s.depth_views_gen) {
        for (auto& [image, view] : s.depth_views1) (void)image, retire_view(view);
        s.depth_views1.clear();
        gen1 = s.depth_views_gen;
    }
    if (auto it = s.depth_views1.find(t->image); it != s.depth_views1.end()) return it->second;
    VkImageViewUsageCreateInfo use{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    use.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = &use;
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t->format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    s.depth_views1[t->image] = v;
    return v;
}
