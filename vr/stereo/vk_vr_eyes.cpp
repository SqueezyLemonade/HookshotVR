// (OoT VR) True stereo: both eyes in one multiview draw (vk_vr.h, "true stereo"; the shader patch: vk_vr_spirv_eyes.h).
//
// The game camera stays at the head's centre (culling, detail and shadows as before). Its large render targets get a second
// array layer when they are made (vk_texture.cpp, eyes_target). A pass into targets that all have it draws both layers at
// once (viewMask 3, vk_draw.cpp begin_pass). The draws whose camera is the scene's (eyes_camera) use their vertex, geometry
// or mesh stage patched to move clip x by each view's eye (eyes_module), so layer 0 is the left eye and layer 1 the right.
// Every other draw into such targets (a full-screen sky, a fog pass) draws the same picture in both.
//
// Layer 0 goes on through the game's own post-processing and upscaler: the left eye's picture is the game's picture.
// Layer 1's scene is copied out at the scene's end (scene_done).
#include "vk_vr.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vk_internal.h"
#include "vk_menu.h"
#include "vk_texture.h"
#include "vk_vr_spirv_eyes.h"
#include "once_log.h"   // (the research lines' once-per-key logs)
#include "vk_vr_spirv_samples.h"
#include "vk_vr_spirv_camera.h"   // (STE-31: a fragment stage's camera reads corrected per view)
#include "../items/bow/arrow_effect_math.h"   // (the particle camera's layout: eyes_particle_camera)
#include "vk_recorder.h"   // (rec::boundary only: the test pictures' copies stay direct calls)

namespace mkwvk {
bool write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);   // (vk_replay.cpp)
uint64_t texture_frame_no();
namespace vr {
float eyes_ipd();          // (vk_vr.cpp: the headset's eye distance, metres)
float eyes_headset_p00();  // (vk_vr.cpp: the P00 of the headset's game camera, 1 / its tan half-FOV across; 0 before it is known)

namespace {

std::atomic<int> wanted_state{-1};
const char* test_dir() {
    const char* e = vr_research_getenv("RX_VR_EYES_TEST");
    return e && *e ? e : nullptr;
}
// Capture the actual first-person/headset route without switching to the headless
// test camera, fixed IPD or projection-filter bypass. Disabled unless requested.
const char* capture_dir() {
    if (const char* dir = test_dir()) return dir;
    const char* e = vr_research_getenv("RX_VR_EYES_CAPTURE");
    return e && *e ? e : nullptr;
}

// the eyes' shift: kx[view] = eye offset along the camera's right (metres) * the projection's P00, quantized so a headset's
// eye distance read again and again does not make the pipelines again
std::mutex kx_mu;
float kx_now[2] = {0, 0};
float p00_now = 0;   // (the scene projection's P00 the shift was made for, as quantized)
std::atomic<uint64_t> kx_key{0};
std::atomic<bool> kx_known{false};

// the shader modules' SPIR-V (vertex, geometry and mesh stages only), and their patched modules
PFN_vkCreateShaderModule original_create = nullptr;
std::mutex modules_mu;
std::unordered_map<VkShaderModule, std::vector<uint32_t>> spirv_of;
struct Patched {
    VkShaderModule module = VK_NULL_HANDLE;
    bool failed = false;
};
std::unordered_map<uint64_t, Patched> patched;   // (module ^ shift key)
std::map<std::pair<VkShaderModule, uint64_t>, Patched> sampled;
// (STE-30) Each patched entry's id by the module it was made from, so a destroyed module's entries can go (forget_module)
std::unordered_multimap<VkShaderModule, uint64_t> patched_from;
void note_patched_from(VkShaderModule from, uint64_t id) { patched_from.emplace(from, id); }   // (modules_mu held)

VkResult VKAPI_CALL capture_module(VkDevice device, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a,
                                   VkShaderModule* m) {
    const VkResult r = original_create(device, ci, a, m);
    if (r == VK_SUCCESS && ci && ci->pCode && ci->codeSize >= 20) {
        const int model = spirv_execution_model(ci->pCode, ci->codeSize / 4);
        if ((model >= 0 && model <= 4) || model == 5365) {
            std::lock_guard lk(modules_mu);
            spirv_of[*m].assign(ci->pCode, ci->pCode + ci->codeSize / 4);
        }
    }
    return r;
}

// (STE-30, the white foliage) The patched modules are kept by the handle of the module they were made from. The program
// cache destroys modules (a borrowed variant let go, the modules dropped after their pipelines are made) and the driver
// hands a later module the same handle: that module then got the old module's patched version, another program's shader,
// for the whole run. Which handles come back depends on the threads' timing, so a run was white or not at random.
// A destroyed module's SPIR-V and its patched versions are forgotten here. Its patched modules are not destroyed: a
// pipeline being made may still hold one (a few kilobytes each, kept)
// RX_VR_EYES_DEBUG 1048576: kept as before, each use of a forgotten one logged, for A/B
// (this file's own spare modules go to the driver's destroy: they are made under modules_mu, which the hook takes)
PFN_vkDestroyShaderModule original_destroy = nullptr;
std::unordered_set<uint64_t> stale_ids;                                // (debug 1048576: the entries kept that would go)
std::set<std::pair<VkShaderModule, uint64_t>> stale_sampled;
std::atomic<int> forgotten{0};

void VKAPI_CALL forget_module(VkDevice device, VkShaderModule m, const VkAllocationCallbacks* a) {
    if (m) {
        const bool keep = eyes_debug(1048576);
        std::lock_guard lk(modules_mu);
        spirv_of.erase(m);
        int n = 0;
        const auto [from, to] = patched_from.equal_range(m);
        for (auto it = from; it != to; ++it, n++)
            if (keep) stale_ids.insert(it->second);
            else patched.erase(it->second);
        if (!keep) patched_from.erase(from, to);
        for (auto it = sampled.lower_bound({m, 0}); it != sampled.end() && it->first.first == m; n++)
            if (keep) stale_sampled.insert(it->first), ++it;
            else it = sampled.erase(it);
        if (n && forgotten++ < 4)
            logf("vr: true stereo: a destroyed module's %d patched version(s) %s", n,
                 keep ? "kept (RX_VR_EYES_DEBUG 1048576)" : "forgotten");
    }
    original_destroy(device, m, a);
}
/// (modules_mu held, debug 1048576 only) a patched entry made from a module since destroyed, about to be used again
void stale_use(bool hit, const char* kind) {
    if (!hit) return;
    static std::atomic<int> told{0};
    if (told++ < 16)
        logf("vr: true stereo: program %016llx: a %s module made from a destroyed module's handle used again (RX_VR_EYES_DEBUG "
             "1048576)", (unsigned long long)eyes_building_prog, kind);
}

// the two-layer attachment views, by texture, format and level
struct EyesView {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t used = 0;
};
std::mutex views_mu;
std::unordered_map<uint64_t, EyesView> views;
// (VRAM flicker, 18 Nov) The cache is bounded. Its entries are by texture id, and ids are never used again: every render target
// the game makes at the scene's size in true stereo left its views here for good, on images long destroyed, and a view a texture
// remade at the same id replaced was left as well. A dungeon lets go of 3,000-7,000 dead render targets every 600 presents
// (last-run.log, 18 Nov 20:13: 202,000 in one session), each made anew at another start, so the views grew without end. Now a
// view unused for kViewIdle presents is destroyed (no work in flight can still use it: 120 is the engine's own wait, vk_draw.cpp's
// old TAAU histories), and a replaced one waits that long. The count is in the log every 600 presents ("two-layer views").
// RX_VR_EYES_VIEWS_KEEP=1: kept, as before (to compare)
constexpr uint64_t kViewIdle = 120;
std::vector<std::pair<VkImageView, uint64_t>> views_retired;   // (view, the present it was replaced at)
uint64_t st_views_made = 0, st_views_destroyed = 0;
void retire_replaced(EyesView& v, uint64_t now) {   // (under views_mu)
    static const bool keep_all = vr_research_getenv("RX_VR_EYES_VIEWS_KEEP") && atoi(vr_research_getenv("RX_VR_EYES_VIEWS_KEEP")) != 0;
    if (v.view && !keep_all) views_retired.push_back({v.view, now});
    v = EyesView{};
}
/// (eyes_frame_end, every kViewIdle presents) the views unused since, and the replaced ones as old
void prune_views(uint64_t now) {
    static const bool keep_all = vr_research_getenv("RX_VR_EYES_VIEWS_KEEP") && atoi(vr_research_getenv("RX_VR_EYES_VIEWS_KEEP")) != 0;
    if (keep_all) return;
    std::lock_guard lk(views_mu);
    for (auto it = views.begin(); it != views.end();) {
        if (now - it->second.used >= kViewIdle) {
            if (it->second.view) vkDestroyImageView(dev.device, it->second.view, nullptr), st_views_destroyed++;
            it = views.erase(it);
        } else {
            ++it;
        }
    }
    size_t keep = 0;
    for (auto& r : views_retired) {
        if (now - r.second >= kViewIdle) vkDestroyImageView(dev.device, r.first, nullptr), st_views_destroyed++;
        else views_retired[keep++] = r;
    }
    views_retired.resize(keep);
}

// the images made with both eyes' layers, as their views are made (the present knows its source by image alone)
std::mutex two_mu;
std::unordered_map<VkImage, std::pair<uint32_t, VkFormat>> two_layer;   // image -> (texture id, format)
void note_two_layer(const Texture* t) {
    std::lock_guard lk(two_mu);
    if (two_layer.size() > 4096) two_layer.clear();
    two_layer[t->image] = {t->id, t->format};
}

// the scene camera's uniform buffers seen this frame
std::mutex cams_mu;
std::unordered_map<uint64_t, std::pair<uint64_t, bool>> cams;   // addr -> (frame, is the scene's camera)

// statistics a frame
std::atomic<int64_t> st_passes{0}, st_draws{0}, st_moved{0}, st_frames{0};

}  // namespace
thread_local uint64_t eyes_building_prog = 0;   // (research, STE-30: vk_vr.h)

bool eyes_wanted() {
    int w = wanted_state.load(std::memory_order_relaxed);
    if (w < 0) {
        // (with the headset in any 3D mode: the targets' second layer is made from the start, so 3D can be switched to and from
        // True stereo in the menu at any time. RX_VR_EYES=0 leaves it out: no layer, no True stereo, a little less video memory)
        const char* off = getenv("RX_VR_EYES");
        w = test_dir() ? 1 : (wanted() && !(off && *off == '0')) ? 1 : 0;
        wanted_state.store(w);
        if (w) logf("vr: true stereo ready: chosen in the menu at any time (3D: True stereo)%s", test_dir() ? " (RX_VR_EYES_TEST, no headset)" : "");
    }
    return w != 0;
}

bool eyes_frame() {   // (with the headset: while 3D is True stereo; another choice in the menu stops drawing both at once)
    return eyes_wanted() && kx_known.load(std::memory_order_acquire) && (test_dir() || opts.vr_stereo == 4);
}

/// (vk_vr.cpp note_scene_draw) the scene's projection: the eyes' shift from its P00 and the eye distance
void eyes_note_projection(const float proj[16]) {
    if (!eyes_wanted()) return;
    const float p00 = std::fabs(proj[0]);
    if (!(p00 > 0.05f && p00 < 20.0f)) return;
    // (with the headset: only its own camera's projection. The title's and a loading screen's cameras have others, and each
    // new shift made every two-eye pipeline again: hundreds of draws left out while they compiled)
    // (a headless benchmark, RX_VR_EYES_TEST with RX_VR_BENCH_FOV=<w>x<h> (vr/camera/view_host.c, vr/tools/bench.py): the same
    // rule, with the P00 of that FOV. Without it, every title and loading camera's own shift rebuilt the two-eye pipelines in
    // test mode too, and the compiles that followed (4 workers busy all run) were measured as the game's cost)
    static const float bench_p00 = [] {
        float w = 0, h = 0;
        const char* e = vr_research_getenv("RX_VR_BENCH_FOV");
        return e && sscanf(e, "%fx%f", &w, &h) == 2 && w > 10 && w < 170 ? 1.0f / std::tan(w * 0.5f / 57.2958f) : 0.0f;
    }();
    if (!test_dir() || bench_p00 > 0) {
        const float want = test_dir() ? bench_p00 : eyes_headset_p00();
        if (want > 0 && std::fabs(p00 - want) > want * 0.03f) return;
    }
    // (research: RX_VR_EYES_TEST_IPD_MM=<mm> in a headless run, 64 if not given; 0 draws both eyes from the middle camera, the
    // reference a one-eye buffer is told from: its eye right and the other off by the whole disparity, a middle-camera one both
    // off by half)
    static const float test_ipd = vr_research_getenv("RX_VR_EYES_TEST_IPD_MM") ? (float)atof(vr_research_getenv("RX_VR_EYES_TEST_IPD_MM")) / 1000.0f : 0.064f;
    const float ipd = test_dir() ? test_ipd : eyes_ipd();
    const float scale = test_dir() ? 1.0f : std::clamp(opts.vr_depth_scale, 0, 300) / 100.0f;
    const float half = std::round(ipd * scale * 0.5f / 0.0005f) * 0.0005f;   // (to half a millimetre)
    const float p = std::round(p00 * 1000.0f) / 1000.0f;
    const float k[2] = {-half * p, half * p};   // (view 0 the left eye: x -= kx[view])
    std::lock_guard lk(kx_mu);
    if (kx_known.load() && k[0] == kx_now[0] && k[1] == kx_now[1]) return;
    kx_now[0] = k[0], kx_now[1] = k[1], p00_now = p;
    uint32_t a, b;
    std::memcpy(&a, &k[0], 4), std::memcpy(&b, &k[1], 4);
    kx_key.store(((uint64_t)a << 32 | b) * 0x9E3779B97F4A7C15ull | 1);
    kx_known.store(true, std::memory_order_release);
    logf("vr: true stereo: eyes %.1f mm apart, P00 %.3f: clip x %+.5f / %+.5f", half * 2000.0f, p, k[0], k[1]);
}

uint64_t eyes_key() { return kx_key.load(std::memory_order_relaxed); }

/// (research, RX_VR_PIPE_LOG) a hash of the SPIR-V a module was made from (its patched words for a patched one), 0 when its
/// words are not kept (a compute stage, or one made before the capture): names a module across runs, where its handle does not
uint64_t module_words_hash(VkShaderModule module) {
    std::lock_guard lk(modules_mu);
    const auto it = spirv_of.find(module);
    if (it == spirv_of.end()) return 0;
    uint64_t h = 0xCBF29CE484222325ull;
    for (const uint32_t w : it->second) h = (h ^ w) * 1099511628211ull;
    return h ? h : 1;
}

float eyes_half() {
    std::lock_guard lk(kx_mu);
    return p00_now > 0 ? std::fabs(kx_now[1]) / p00_now : 0.0f;   // (the eye offset the shift was made for, metres)
}

/// (vk_vr.cpp device_created) the modules' SPIR-V is kept from now on, for the patch
void eyes_device_created() {
    // (also for the HUD size patch, hud_module: with the headset in any mode, and RX_VR_HUD_TEST's headless checks)
    if ((!eyes_wanted() && !wanted() && !vr_research_getenv("RX_VR_HUD_TEST")) || original_create) return;
    original_create = vkCreateShaderModule;
    vkCreateShaderModule = capture_module;
    original_destroy = vkDestroyShaderModule;   // (STE-30: a destroyed module's patched versions forgotten, forget_module)
    vkDestroyShaderModule = forget_module;
    logf("vr: true stereo: vertex, geometry and mesh stages kept for their two-eye versions");
}

bool eyes_target(uint32_t type, uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples) {
    // (the scene's targets and the full-size ones after it: 2D, one layer and level, not multisampled, at least 1.5 MP and not
    // square, which leaves out the shadow maps and the cube map's faces)
    // (the test without the headset presents 1280 x 720: its final picture is taken in too)
    // (down to a quarter of that: the scene's quarter-size targets and buffers. The light the game gathers at a quarter of the size,
    // made from the left eye's depth alone, left a black copy of Link's head and the sword beside them in the right eye)
    // (research, STE-32, RX_VR_EYES_MORE=mips|small|1: screen buffers with a mip chain (a depth pyramid for screen-space
    // shadows: mips), ones down to an eighth of the scene's size each way (small), or both (1) get the right eye's layer too.
    // Without it they stay one layer, made from the left eye's depth and read by both: near objects' contact shadows at the
    // same place in both eyes)
    static const int more = [] {
        const char* e = vr_research_getenv("RX_VR_EYES_MORE");
        if (!e || !*e || !strcmp(e, "0")) return 0;
        return !strcmp(e, "mips") ? 1 : !strcmp(e, "small") ? 2 : 3;   // (bit 1 mip chains, bit 2 small buffers)
    }();
    const uint64_t least = (test_dir() ? 900000 : 1500000) / ((more & 2) ? 64 : 4);
    // (STE-32/35, by default: the steps of a screen downsample chain below the quarter, the scene's size over 3 to 16 each way.
    // The glow's 180x144, 240x192 and 480x384 stayed one layer, built from the left eye's picture and added to both: a light's
    // glow in the right eye where nothing is there, Navi's white ghost on her right in the right lens only, likely the red
    // flashes with white streaks too. RX_VR_EYES_MORE=0: one layer, as before)
    static const bool chain = !(vr_research_getenv("RX_VR_EYES_MORE") && !strcmp(vr_research_getenv("RX_VR_EYES_MORE"), "0"));
    return eyes_wanted() && type == VK_IMAGE_TYPE_2D && layers == 1 && (levels == 1 || (more & 1)) &&
           samples == VK_SAMPLE_COUNT_1_BIT && width != height &&
           ((uint64_t)width * height >= least || (chain && levels == 1 && eyes_scene_chain(width, height)));
}

VkImageView eyes_view(Texture* t, VkFormat format, uint32_t level) {
    if (!t || !t->vr_eyes || !t->image) return VK_NULL_HANDLE;
    const uint64_t key = ((uint64_t)t->id << 24) ^ ((uint64_t)format << 4) ^ level;
    const uint64_t now = texture_frame_no();
    std::lock_guard lk(views_mu);
    EyesView& v = views[key];
    if (v.view && v.image == t->image) return v.used = now, v.view;
    retire_replaced(v, now);   // (an older view of a texture remade at the same id: it may still be in recorded work)
    VkImageViewUsageCreateInfo use{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    const bool depth = (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    use.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.pNext = &use;
    ci.image = t->image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    ci.format = format;
    ci.subresourceRange = {depth ? (VkImageAspectFlags)(t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                                 : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                           level, 1, 0, 2};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &ci, nullptr, &view) != VK_SUCCESS) {
        static int told;
        if (told++ < 8) logf("vr: true stereo: no two-layer view of texture %u (format %d)", t->id, (int)format);
        return VK_NULL_HANDLE;
    }
    v.image = t->image, v.view = view, v.used = now;
    st_views_made++;
    note_two_layer(t);
    return view;
}

thread_local int eyes_layer_bias = 0;
thread_local bool eyes_twin = false;

bool eyes_debug(int bit) {   // (RX_VR_EYES_DEBUG=<mask>: parts of true stereo off, to find what a picture fault comes from)
    static const int mask = vr_research_getenv("RX_VR_EYES_DEBUG") ? atoi(vr_research_getenv("RX_VR_EYES_DEBUG")) : 0;
    return (mask & bit) != 0;
}
thread_local bool eyes_sample_array = false;
thread_local int eyes_storage_made = 0;

VkImageView eyes_layer_view(Texture* t, VkFormat format, uint32_t level, uint32_t layer) {
    if (!t || !t->vr_eyes || !t->image || layer > 1) return VK_NULL_HANDLE;
    const uint64_t key = (((uint64_t)t->id << 24) ^ ((uint64_t)format << 4) ^ level) * 31 + 0x1000000000000ull * (2 + layer);
    const uint64_t now = texture_frame_no();
    std::lock_guard lk(views_mu);
    EyesView& v = views[key];
    if (v.view && v.image == t->image) return v.used = now, v.view;
    retire_replaced(v, now);
    VkImageViewUsageCreateInfo use{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    const bool depth = (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    use.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.pNext = &use;
    ci.image = t->image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange = {depth ? (VkImageAspectFlags)(t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                                 : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                           level, 1, layer, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &ci, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    v.image = t->image, v.view = view, v.used = now;
    st_views_made++;
    note_two_layer(t);
    return view;
}

void eyes_note_image(const Texture* t) {
    if (t && t->image && t->vr_eyes) note_two_layer(t);
}

bool eyes_two_layer(VkImage image) {
    std::lock_guard lk(two_mu);
    return two_layer.count(image) != 0;
}

std::atomic<int64_t> st_cam_calls{0}, st_cam_reads{0};

bool eyes_camera(uint64_t camera, uint64_t size, uint32_t width, uint32_t height) {
    if (!camera || size < 0x230) return false;
    st_cam_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = texture_frame_no();
    // (frame rate: the same buffer as the draw before needs no lock or search; most draws repeat their neighbour's)
    // (by the buffer and the targets' size: the answer depends on both. Kept by the buffer alone, a quarter-size pass reading the
    // scene's camera first said no for the frame's full-size draws, and most of the scene was drawn without its shift)
    const uint64_t key = camera ^ ((uint64_t)width << 40) ^ ((uint64_t)height << 52);
    static thread_local uint64_t last_key = 0, last_frame = ~0ull;
    static thread_local bool last_is = false;
    if (key == last_key && now == last_frame) return last_is;
    {
        std::lock_guard lk(cams_mu);
        auto it = cams.find(key);
        if (it != cams.end() && it->second.first == now) {
            last_key = key, last_frame = now, last_is = it->second.second;
            return last_is;
        }
        if (cams.size() > 4096) cams.clear();
    }
    st_cam_reads.fetch_add(1, std::memory_order_relaxed);
    // (only what the test reads: the projection's P00 at 0x70, its w row at 0xa0 and the target's size at 0x120. Not the
    // whole 0x230 bytes, read again for each new buffer)
    float c[(0x128 - 0x70) / 4];
    const auto at = [&](uint32_t off) { return c[(off - 0x70) / 4]; };
    bool is = memory_read_as_submitted(camera + 0x70, c, sizeof c) && at(0x120) == (float)width && at(0x124) == (float)height &&
              at(0xa0) == 0.0f && at(0xa4) == 0.0f && at(0xa8) == -1.0f && at(0xac) == 0.0f;
    if (is) {   // (the scene's projection: another perspective camera into these targets would need a shift of its own)
        std::lock_guard lk(kx_mu);
        is = kx_known.load() && std::fabs(std::fabs(at(0x70)) - p00_now) < 0.0015f;
        // (research, RX_VR_EYES_CAM_LOG=1: a perspective camera into the scene's targets that is not the scene's, by its P00: drawn
        // the same in both eyes, at infinity. The particle camera in first person is suspected: particles seen behind things)
        static const bool log_on = vr_research_getenv("RX_VR_EYES_CAM_LOG") && atoi(vr_research_getenv("RX_VR_EYES_CAM_LOG")) != 0;
        static std::unordered_set<int> told;
        const int p00 = (int)std::lround(std::fabs(at(0x70)) * 100);
        if (log_on && !is && kx_known.load() && told.size() < 16 && told.insert(p00).second)
            logf("vr: true stereo: a perspective camera that is not the scene's, P00 %.3f (the scene's %.3f), into %ux%u: drawn "
                 "the same in both eyes", std::fabs(at(0x70)), p00_now, width, height);
    }
    std::lock_guard lk(cams_mu);
    cams[key] = {now, is};
    last_key = key, last_frame = now, last_is = is;
    return is;
}

/// (STE-26) Whether a buffer is a particle camera with the scene's projection: the camera the game's effects and particles read
/// (vertex UBO 10, "C13": its view at 0x40 and projection at 0x80, column-major: vr/items/bow/arrow_effect_math.h). Those
/// draws never read the scene's camera at UBO 3, so they were drawn the same in both eyes: at infinity, behind anything near
/// (first person, headset 20 Nov: "particles behind the object", right again after a cutscene's camera cut). With the
/// scene's P00 the same clip-space shift moves them for each eye. Once per buffer and frame, as eyes_camera
bool eyes_particle_camera(uint64_t camera, uint64_t size) {
    if (!camera || size < 0xc0 || !kx_known.load(std::memory_order_acquire)) return false;
    const uint64_t now = texture_frame_no();
    const uint64_t key = camera ^ 0x5041525449434c45ull;   // (apart from eyes_camera's keys in the same cache)
    static thread_local uint64_t last_key = 0, last_frame = ~0ull;
    static thread_local bool last_is = false;
    if (key == last_key && now == last_frame) return last_is;
    {
        std::lock_guard lk(cams_mu);
        auto it = cams.find(key);
        if (it != cams.end() && it->second.first == now) {
            last_key = key, last_frame = now, last_is = it->second.second;
            return last_is;
        }
        if (cams.size() > 4096) cams.clear();
    }
    float c[0xc0 / 4];
    arrow_fx::Camera info;
    bool is = memory_read_as_submitted(camera, c, sizeof c) && arrow_fx::inspect_camera(c, sizeof c, info);
    if (is) {
        std::lock_guard lk(kx_mu);
        is = std::fabs(std::fabs(c[arrow_fx::kProjection]) - p00_now) < 0.0015f;
    }
    std::lock_guard lk(cams_mu);
    cams[key] = {now, is};
    last_key = key, last_frame = now, last_is = is;
    return is;
}

/// Whether a buffer is the scene's camera, by its own size field (0x120): for a dispatch, which has no targets to compare
bool eyes_scene_camera(uint64_t camera, uint64_t size) {
    if (!camera || size < 0x230) return false;
    float wh[2];
    return memory_read_as_submitted(camera + 0x120, wh, sizeof wh) && wh[0] >= 1.0f && wh[1] >= 1.0f &&
           eyes_camera(camera, size, (uint32_t)wh[0], (uint32_t)wh[1]);
}

/// (research, RX_VR_EYES_CAM_LOG=1, STE-29) the scene camera's view and inverse view as last seen (note_scene_draw), and a search
/// for them in a dispatch's buffers: where a pass that does not read the scene camera keeps its own copy of the camera
std::mutex seen_cam_mu;
float seen_view[12], seen_inv[12], seen_vp[16], seen_ivp[16];
bool seen_cam = false;
/// a 4x4's inverse (rows), Gauss-Jordan; false when singular
bool invert4(const float* m, float* out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) std::swap(a[c][k], a[pivot][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) out[r * 4 + k] = (float)a[r][k + 4];
    return true;
}
void eyes_note_camera(const float* c) {
    std::lock_guard lk(seen_cam_mu);
    std::memcpy(seen_view, c, sizeof seen_view);
    std::memcpy(seen_inv, c + 0xb0 / 4, sizeof seen_inv);
    std::memcpy(seen_vp, c + 0x30 / 4, sizeof seen_vp);
    if (!invert4(seen_vp, seen_ivp)) std::memset(seen_ivp, 0, sizeof seen_ivp);
    seen_cam = true;
}
void eyes_find_camera(uint64_t prog, const char* kind, int index, uint64_t addr, uint64_t size) {
    static const bool on = vr_research_getenv("RX_VR_EYES_CAM_LOG") && atoi(vr_research_getenv("RX_VR_EYES_CAM_LOG")) != 0;
    if (!on || !addr || size < 48) return;
    float mats[4][16];   // (view and inverse view: their 3 rows; view-projection and its inverse: 4)
    {
        std::lock_guard lk(seen_cam_mu);
        if (!seen_cam) return;
        std::memcpy(mats[0], seen_view, sizeof seen_view), std::memcpy(mats[1], seen_inv, sizeof seen_inv);
        std::memcpy(mats[2], seen_vp, sizeof seen_vp), std::memcpy(mats[3], seen_ivp, sizeof seen_ivp);
    }
    static const char* const names[4] = {"view", "inverse view", "view-projection", "inverse view-projection"};
    const bool have_ivp = mats[3][0] != 0.0f || mats[3][5] != 0.0f;   // (a singular one left zero: it would match any zeros)
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    {
        std::lock_guard lk(mu);
        const uint64_t key = prog * 131 + (uint64_t)index * 7 + (kind[0] == 's');
        if (seen.size() >= 256 || !seen.insert(key).second) return;
    }
    const uint64_t n = std::min<uint64_t>(size, 0x10000) / 4;
    std::vector<float> b(n);
    if (!memory_read_as_submitted(addr, b.data(), n * 4)) return;
    // a 3x4 of rows as stored (r), or the 4x4 column-major of it (c: element (row, col) at col * 4 + row)
    auto match = [&](const float* m, int rows, uint64_t at, bool colmajor) {
        if (at + 16 > n) return false;
        for (int r = 0; r < rows; r++)
            for (int k = 0; k < 4; k++) {
                const float v = colmajor ? b[at + k * 4 + r] : b[at + r * 4 + k];
                if (!(std::fabs(v - m[r * 4 + k]) <= 1e-3f * std::max(1.0f, std::fabs(m[r * 4 + k])))) return false;
            }
        return true;
    };
    char line[700];
    int len = 0;
    for (uint64_t at = 0; at + 12 <= n && len < (int)sizeof line - 80; at += 4)
        for (int which = 0; which < 4; which++)
            for (int col = 0; col < 2; col++)
                if ((which < 3 || have_ivp) && match(mats[which], which < 2 ? 3 : 4, at, col))
                    len += snprintf(line + len, sizeof line - len, " %s %s at %#llx;", names[which],
                                    col ? "column-major" : "rows", (unsigned long long)(at * 4));
    if (len)
        logf("vr: true stereo: dispatch %016llx %s %d (%llu bytes) holds the scene camera's%s", (unsigned long long)prog, kind,
             index, (unsigned long long)size, line);
}

/// (STE-29) Where a pass keeps copies of the scene camera, by program and binding: the game binds several to its lighting,
/// shadow and fog passes, in other layouts than the scene camera's own buffer (a 512-byte one with the view-projection at 0xc0
/// and 0x100, a 14848-byte one with the view at 0 and the inverse view at 0x80, ...: RX_VR_EYES_CAM_LOG, 20 Nov). Moving the
/// scene camera's buffer alone (STE-28) left these at the middle camera, so the shadow and fog passes still worked out each
/// eye's pixels from the middle of the head. Learned once by matching the scene camera as last seen (a few tries while it
/// may not match yet), then each eye's copy has every one moved by that eye's offset along the camera's right
struct CamCopies {
    // (offset in floats, kind: 0 view rows 3x4, 1 inverse view rows, 2 view-projection rows 4x4, 3 a 4x4 the game applies to a
    // view-space point, rows as stored: STE-33's shadow matrices)
    std::vector<std::pair<uint32_t, uint8_t>> at;
    int tries = 0;
    bool cascades = false;   // (STE-33: the scene camera buffer's cascade matrices added)
    int8_t scene = -1;       // (STE-33: this binding is the scene camera buffer, by its own contents: -1 not seen yet, 0 no, 1 yes)
    bool derived = false;    // (STE-34: an actor shadow box's matrices added)
    uint8_t told = 0;        // (STE-33: its tries logged)
};
std::mutex copies_mu;
std::unordered_map<uint64_t, CamCopies> copies;
/// (STE-31) the same, by program: binding -> its copies, for the fragment patch (eyes_camera_module)
std::unordered_map<uint64_t, std::map<int, std::vector<std::pair<uint32_t, uint8_t>>>> copies_of_program;
bool eyes_camera_copies_maybe(uint64_t prog, int binding) {
    std::lock_guard lk(copies_mu);
    auto it = copies.find(prog * 131 + (uint64_t)binding);
    return it == copies.end() || !it->second.at.empty() || it->second.tries < 3;
}
/// (copies_mu held) one try at learning where `data` holds the camera, while this program and binding is not learned yet;
/// false while the scene camera has not been seen
/// (STE-33) The scene camera buffer also holds the game's three view->shadow matrices, one per shadow cascade (the cascade
/// shadow map is a 3-layer array): 4x4s with rows as stored at floats 3088, 3104 and 3120 (bytes 0x3040-0x30ff), the first
/// orthographic (its last row 0 0 0 1), the other two perspective (a w row of their own, in the same direction). The game made
/// them from the middle camera. The shadow pass (d8180bd8c6ecc77b) and the forward-lit programs multiply each eye's own
/// view-space position (plus a normal offset) by all 48 numbers: each eye's lookup landed half an eye distance to its side, the
/// shadows of pebbles, bushes, Link and enemies at the same screen place in both eyes (no disparity; two shadows by a small
/// rock), while the terrain's, from a world->shadow matrix, came right with STE-29. M_eye = M_mid T(ex): each row's w gains ex
/// times its x, every row (kind 3). The headset dump (20 Nov, 70b99fec) showed the layout; the first guess, four 3x4s at
/// 3088/3100/3112/3124, straddled these and never passed its check. Added to the scene camera buffer (told by its own fields)
/// once the block passes a check of that shape (cascades_check): the stereo shadows investigation.
/// RX_VR_EYES_DEBUG 65536: left out, for A/B
constexpr uint32_t kCascadeAt[3] = {3088, 3104, 3120};   // (floats: each cascade's view->shadow 4x4)
constexpr uint64_t kCascadeEnd = 3136;   // (floats: the block's end)
std::atomic<int> cascades_state{0};      // (0 not checked yet, 1 the block is the three cascades, -1 it is not)
int eyes_cascades_state() { return cascades_state.load(std::memory_order_relaxed); }
/// whether a buffer, as read, is the scene camera's own (by its fields, as eyes_scene_camera checks it: the projection's w row
/// at 0xa0 and its P00 at 0x70 the scene's), whatever copies were learned from it. The headset's d8180bd8c6ecc77b learned
/// 3 copies (from its vertex stage, which reads the buffer's start only) where the others learned 5: tying the cascades to
/// the learned offsets left them out there (fe51a7db, 20 Nov)
static bool scene_camera_buffer(const float* data, uint64_t n) {
    if (n < 0x230 / 4) return false;
    if (!(data[0xa0 / 4] == 0.0f && data[0xa4 / 4] == 0.0f && data[0xa8 / 4] == -1.0f && data[0xac / 4] == 0.0f)) return false;
    std::lock_guard lk(kx_mu);
    return p00_now > 0 && std::fabs(std::fabs(data[0x70 / 4]) - p00_now) < 0.0015f;
}
/// the block as read from a scene camera buffer: the three cascades' shape? The 4 floats before it zero, the orthographic one's
/// last row 0 0 0 1 and its other rows not zero, the two perspective ones' w rows (floats 3116 and 3132) not zero and in the
/// same direction. Returns the reason it decided nothing yet, for the log
static const char* cascades_check(const float* data, uint64_t n) {
    if (cascades_state.load() != 0) return "already decided";
    if (n < kCascadeEnd) return "this stage reads less of the buffer than the cascades";
    double big = 0;
    for (uint64_t i = kCascadeAt[0]; i < kCascadeEnd; i++) big = std::max(big, (double)std::fabs(data[i]));
    if (big == 0) return "no sun in the block (all zeros): decided later";
    const double eps = big * 1e-6;   // (relative to the block's own numbers, whatever units the game uses)
    auto norm3 = [&](uint64_t o) { return std::sqrt((double)data[o] * data[o] + (double)data[o + 1] * data[o + 1] + (double)data[o + 2] * data[o + 2]); };
    bool shape = true;
    for (int i = 3084; i < 3088; i++) shape &= std::fabs(data[i]) <= eps;                     // (nothing just before)
    shape &= std::fabs(data[3100]) <= eps && std::fabs(data[3101]) <= eps && std::fabs(data[3102]) <= eps &&
             std::fabs(data[3103] - 1.0f) <= 1e-4;                                            // (the orthographic one's 0 0 0 1)
    for (int r = 0; r < 3; r++) shape &= norm3(kCascadeAt[0] + r * 4) > eps;                  // (and its other rows)
    const double a = norm3(3116), b = norm3(3132);
    double parallel = 0;
    if (a > eps && b > eps)
        parallel = std::fabs((double)data[3116] * data[3132] + (double)data[3117] * data[3133] + (double)data[3118] * data[3134]) / (a * b);
    const bool ok = shape && parallel >= 0.999;
    static std::atomic<int> fails{0};
    if (!ok && ++fails < 16) return "not the three cascades' shape this time (one odd frame does not decide it)";
    int expected = 0;
    if (cascades_state.compare_exchange_strong(expected, ok ? 1 : -1))
        logf("vr: true stereo: the scene camera's shadow block (0x3040-0x30ff) %s (shape %d, w rows parallel to %.5f)%s",
             ok ? "is three view->shadow cascades (one orthographic, two perspective): moved for each eye"
                : "is NOT the expected three cascades: left alone",
             (int)shape, parallel, eyes_debug(65536) ? "; RX_VR_EYES_DEBUG 65536: left out anyway" : "");
    return ok ? "checked: the cascades" : "checked: not the cascades";
}
/// (copies_mu held) the three cascades added to a scene camera buffer's copies, once the block passed its check
static void add_cascades(uint64_t prog, int binding, CamCopies& c) {
    if (c.cascades || c.scene != 1 || cascades_state.load() != 1 || eyes_debug(65536)) return;
    c.cascades = true;
    for (const uint32_t o : kCascadeAt) c.at.push_back({o, 3});
    copies_of_program[prog][binding] = c.at;
    static std::atomic<int> told{0};
    if (told++ < 24)
        logf("vr: true stereo: program %016llx binding %d: the three shadow cascades moved for each eye too (%zu copies)",
             (unsigned long long)prog, binding, c.at.size());
}
/// (STE-33, research) where the bytes the check sees came from: the binding's address, its bound size and whether the GPU
/// writes that range (Hookshot binds such a range in place: guest memory and the captures a copy is made from do not have
/// the GPU's bytes yet, so a copy, ours included, holds what was there before). Set by uniform_range's VR hook and by
/// eyes_camera_copies_learn just before the bytes are looked at; dumped a few times while the check is undecided
struct CamSource {
    uint64_t addr = 0, size = 0;
    int gpu_written = -1;   // (-1 not asked, 0 no, 1 earlier in this submission, 2 an earlier submission not done yet)
};
thread_local CamSource cam_source;
std::atomic<int> cam_dumps{0};
bool eyes_camera_dump_wanted() { return cam_dumps.load(std::memory_order_relaxed) < 3 && cascades_state.load() == 0; }
void eyes_camera_source(uint64_t addr, uint64_t size, int gpu_written) { cam_source = {addr, size, gpu_written}; }
static void cascades_dump(uint64_t prog, int binding, const float* data, uint64_t n) {
    if (n < kCascadeEnd + 5 || cam_dumps.fetch_add(1) >= 3) return;
    const CamSource src = cam_source;
    logf("vr: true stereo: cascade dump: program %016llx binding %d, buffer %#llx (bound %llu bytes), %llu floats read, the GPU "
         "writes this range: %s", (unsigned long long)prog, binding, (unsigned long long)src.addr, (unsigned long long)src.size,
         (unsigned long long)n, src.gpu_written < 0 ? "not asked" : src.gpu_written == 0 ? "no" :
         src.gpu_written == 1 ? "yes, earlier in this submission" : "yes, an earlier submission not done yet");
    auto row = [&](uint64_t from, uint64_t to) {
        char line[700];
        int len = snprintf(line, sizeof line, "vr: true stereo: cascade dump floats %llu-%llu:", (unsigned long long)from,
                           (unsigned long long)(to - 1));
        for (uint64_t i = from; i < to && len < (int)sizeof line - 16; i++) len += snprintf(line + len, sizeof line - len, " %.6g", data[i]);
        logf("%s", line);
    };
    row(0, 12);
    row(3084, 3104), row(3104, 3124), row(3124, 3141);
}
/// (STE-34) The actors' shadows: one box per actor (Link, Navi, NPCs, enemies, chickens, pots) projecting that actor's own
/// shadow map onto the scene's depth, in three programs (4, 9 and 13 compares). Their fragment stage builds a VIEW-space
/// position from the depth and the view ray, then multiplies it by two 3x4s in its per-box buffer (binding 5): a view->box
/// test at float 36 (outside: discarded) and a view->world or view->light at float 0. Both are made from the middle camera,
/// so each eye's lookup landed half an eye distance to its side: every actor's shadow at the same screen place in both eyes.
/// Each is a 3x4 applied to a view-space point, so each row's w gains ex times its x, kind 1, whichever float 0 is (the
/// inverse view's copy, learned, or a product). Read from the shader cache by the investigation session
/// (actor-shadows-box-programs.md, 20 Nov); keyed by program, as no buffer's numbers tell these matrices apart.
/// RX_VR_EYES_DEBUG 131072: left out, for A/B
constexpr uint64_t kActorShadowBoxes[3] = {0x99e9ef57bd48daa9ull, 0x3427a1dbefd5e02cull, 0x1e26755d8cb22dc8ull};
constexpr int kActorShadowBinding = 5;
constexpr uint32_t kActorShadowAt[2] = {0, 36};   // (floats: view->world or light, view->box)
bool eyes_actor_shadow_box(uint64_t prog) {
    if (eyes_debug(131072)) return false;
    for (const uint64_t k : kActorShadowBoxes)
        if (k == prog) return true;
    return false;
}
/// (copies_mu held) the actor shadow box's two view-space 3x4s added to its per-box buffer's copies, once
static void actor_shadow_try(uint64_t prog, int binding, CamCopies& c) {
    if (c.derived || binding != kActorShadowBinding || !eyes_actor_shadow_box(prog)) return;
    c.derived = true;
    for (const uint32_t o : kActorShadowAt) {
        bool have = false;
        for (auto& [at, kind] : c.at) have |= at == o;
        if (!have) c.at.push_back({o, 1});
    }
    copies_of_program[prog][binding] = c.at;
    static std::atomic<int> told{0};
    if (told++ < 6)
        logf("vr: true stereo: program %016llx binding %d: an actor's shadow box, its view-space matrices moved for each eye "
             "(%zu copies)", (unsigned long long)prog, binding, c.at.size());
}
/// (copies_mu held) STE-33 for a buffer as read: is it the scene camera's, then the block's check, then the cascades. The
/// first tries are logged with what was seen and why nothing was added, so a run shows where it stopped
static void cascades_try(uint64_t prog, int binding, CamCopies& c, const float* data, uint64_t n) {
    if (c.cascades || eyes_debug(65536)) return;
    if (c.scene != 1) {
        const bool scene = scene_camera_buffer(data, n);
        if (!scene && c.scene == 0) return;
        c.scene = scene ? 1 : (n >= 0x230 / 4 ? 0 : -1);   // (a stage that reads too little to tell: asked again later)
        if (!scene) return;
    }
    if (cascades_state.load() == 0) cascades_dump(prog, binding, data, n);   // (research: what the check is given)
    const char* why = cascades_check(data, n);
    add_cascades(prog, binding, c);
    static int told = 0;   // (copies_mu held)
    if (c.told < 2 && told < 64) {
        c.told++, told++;
        char line[400];
        int len = 0;
        for (const auto& [o, kind] : c.at)
            if (len < (int)sizeof line - 16) len += snprintf(line + len, sizeof line - len, " %u:%u", o, kind);
        logf("vr: true stereo: cascades for program %016llx binding %d (%llu floats read; copies at float:kind%s): %s%s",
             (unsigned long long)prog, binding, (unsigned long long)n, len ? line : " none", why,
             c.cascades ? ", added" : "");
    }
}
static bool learn_copies(uint64_t prog, int binding, CamCopies& c, const float* data, uint64_t n) {
    if (!c.at.empty() || c.tries >= 3) {   // (learned already: STE-33's cascades on a later read, until they are in)
        cascades_try(prog, binding, c, data, n);
        actor_shadow_try(prog, binding, c);
        return true;
    }
    float view[12], inv[12], vp[16];
    {
        std::lock_guard lk2(seen_cam_mu);
        if (!seen_cam) return false;
        std::memcpy(view, seen_view, sizeof view), std::memcpy(inv, seen_inv, sizeof inv), std::memcpy(vp, seen_vp, sizeof vp);
    }
    c.tries++;
    auto same = [&](const float* m, int rows, uint64_t o) {
        if (o + (uint64_t)rows * 4 > n) return false;
        for (int i = 0; i < rows * 4; i++)
            if (!(std::fabs(data[o + i] - m[i]) <= 1e-3f * std::max(1.0f, std::fabs(m[i])))) return false;
        return true;
    };
    for (uint64_t o = 0; o + 12 <= n; o += 4) {
        if (same(view, 3, o)) c.at.push_back({(uint32_t)o, 0}), o += 8;
        else if (same(inv, 3, o)) c.at.push_back({(uint32_t)o, 1}), o += 8;
        else if (same(vp, 4, o)) c.at.push_back({(uint32_t)o, 2}), o += 12;
    }
    if (!c.at.empty()) {
        copies_of_program[prog][binding] = c.at;
        cascades_try(prog, binding, c, data, n);   // (STE-33: the copies just learned)
        actor_shadow_try(prog, binding, c);       // (STE-34)
        static std::atomic<int> told{0};
        if (told++ < 48)
            logf("vr: true stereo: program %016llx binding %d holds %zu copies of the camera, moved for each eye",
                 (unsigned long long)prog, binding, c.at.size());
    }
    return true;
}
/// (STE-31) whether this program's buffer at this binding holds copies of the camera, learned from the buffer as bound now
/// (a few tries): 1 it does, 0 it does not, -1 not known yet. Nothing is moved
int eyes_camera_copies_learn(uint64_t prog, int binding, uint64_t addr, uint64_t size) {
    const uint64_t key = prog * 131 + (uint64_t)binding;
    {
        std::lock_guard lk(copies_mu);
        auto it = copies.find(key);
        if (it != copies.end() && (!it->second.at.empty() || it->second.tries >= 3)) {
            CamCopies& c = it->second;
            // (STE-33: a scene camera buffer whose cascade block was not checked yet is read once more, for the check)
            const bool check = !c.cascades && !eyes_debug(65536) && c.scene != 0 && size >= 0x230 &&
                               (c.scene == -1 || cascades_state.load() == 0);
            if (!check) {
                add_cascades(prog, binding, c);
                actor_shadow_try(prog, binding, c);   // (STE-34)
                return c.at.empty() ? 0 : 1;
            }
        }
    }
    {
        std::lock_guard lk(seen_cam_mu);   // (nothing to match before the scene camera is seen: no read)
        if (!seen_cam) return -1;
    }
    const uint64_t n = std::min<uint64_t>(size, 0x4000) / 4;   // (the largest copy seen sits in a 14848-byte buffer)
    std::vector<float> b(n);
    if (!n || !memory_read_as_submitted(addr, b.data(), n * 4)) return -1;
    if (eyes_camera_dump_wanted())   // (STE-33 research: the dump's source)
        eyes_camera_source(addr, size, memory_gpu_writes_any() && addr < (1ull << 40) ? memory_gpu_written(addr, n * 4) : 0);
    std::lock_guard lk(copies_mu);
    CamCopies& c = copies[key];
    if (!learn_copies(prog, binding, c, b.data(), n)) return -1;
    return !c.at.empty() ? 1 : c.tries >= 3 ? 0 : -1;
}
bool eyes_move_camera_copies(uint64_t prog, int binding, float* data, uint64_t bytes, int eye) {
    const uint64_t key = prog * 131 + (uint64_t)binding, n = bytes / 4;
    std::vector<std::pair<uint32_t, uint8_t>> at;
    {
        std::lock_guard lk(copies_mu);
        CamCopies& c = copies[key];
        if (!learn_copies(prog, binding, c, data, n)) return false;   // (learn: where this buffer holds the camera's matrices)
        at = c.at;
    }
    if (at.empty()) return false;
    const float ex = eyes_half() * (eye == 1 ? -1.0f : 1.0f);
    float p00;
    {
        std::lock_guard lk(kx_mu);
        p00 = p00_now;
    }
    for (const auto& [o, kind] : at) {
        if (o + (kind >= 2 ? 16u : 12u) > n) continue;   // (learned from more of the buffer than this stage reads: STE-31)
        float* m = data + o;
        if (kind == 0) m[3] -= ex;                                                   // (view: the eye's place along its right)
        else if (kind == 1) for (int r = 0; r < 3; r++) m[r * 4 + 3] += ex * m[r * 4];   // (inverse view: the eye's place)
        else if (kind == 2) m[3] -= p00 * ex;                                        // (view-projection: P's first column is (P00, 0, 0, 0))
        else for (int r = 0; r < 4; r++) m[r * 4 + 3] += ex * m[r * 4];              // (a view-space 4x4: M_eye = M_mid T(ex), every row)
    }
    return true;
}

/// (research, RX_VR_EYES_CAM_LOG=1) Which programs read the scene's camera, and how the camera buffer is laid out: what a
/// per-eye camera would have to move (STE-16, STE-24). Once per program and stage (96 at most); the layout once
void eyes_camera_research(uint64_t camera, uint64_t size, uint64_t prog, int kind, int stage, uint32_t width, uint32_t height,
                          int split) {
    static const bool on = vr_research_getenv("RX_VR_EYES_CAM_LOG") && atoi(vr_research_getenv("RX_VR_EYES_CAM_LOG")) != 0;
    if (!on || !camera || size < 0x230 || !kx_known.load(std::memory_order_acquire)) return;
    static OnceLog<96> cam_log;   // (the "camera read by" lines: 96 at most, each once)
    const uint64_t key = (prog * 31 + (uint64_t)stage) * 3 + (uint64_t)split;   // (a split dispatch: both halves)
    if (cam_log.closed(key)) return;   // (logged, or the log full: no read, no lock. The layout lines came with the first)
    float c[0x230 / 4];
    if (!memory_read_as_submitted(camera, c, sizeof c)) return;
    // (the scene's camera by its own fields: the projection's w row and P00, whatever the target's size)
    if (!(c[0xa0 / 4] == 0.0f && c[0xa4 / 4] == 0.0f && c[0xa8 / 4] == -1.0f && c[0xac / 4] == 0.0f)) return;
    {
        std::lock_guard lk(kx_mu);
        if (std::fabs(std::fabs(c[0x70 / 4]) - p00_now) >= 0.0015f) return;
    }
    static std::mutex mu;   // (the layout lines, once)
    static bool layout_told = false;
    std::lock_guard lk(mu);
    if (!layout_told) {
        layout_told = true;
        // each 4x4 (rows of 4 floats) in the buffer tried as the inverse of the view-projection (0x30), the projection (0x70)
        // and the view (0x00, 3x4 with (0, 0, 0, 1) below), multiplied on either side
        auto mul = [](const float* a, const float* b, float* o) {
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) {
                    float s = 0;
                    for (int j = 0; j < 4; j++) s += a[r * 4 + j] * b[j * 4 + k];
                    o[r * 4 + k] = s;
                }
        };
        auto identity = [](const float* m) {
            float e = 0;
            for (int i = 0; i < 16; i++) e = std::max(e, std::fabs(m[i] - ((i % 5) == 0 ? 1.0f : 0.0f)));
            return e < 2e-3f;
        };
        float view[16];
        std::memcpy(view, c, 48);
        view[12] = view[13] = view[14] = 0.0f, view[15] = 1.0f;
        const struct { const char* name; const float* m; } refs[] = {{"view-projection 0x30", c + 0x30 / 4},
                                                                     {"projection 0x70", c + 0x70 / 4}, {"view 0x00", view}};
        char line[600];
        int n = snprintf(line, sizeof line, "vr: true stereo: camera layout:");
        for (uint32_t off = 0; off + 64 <= 0x230; off += 16) {
            const float* m = c + off / 4;
            for (const auto& ref : refs) {
                float a[16], b[16];
                mul(m, ref.m, a), mul(ref.m, m, b);
                if ((identity(a) || identity(b)) && n < (int)sizeof line - 64)
                    n += snprintf(line + n, sizeof line - n, " inverse of the %s at %#x;", ref.name, off);
            }
        }
        logf("%s (size field %.0fx%.0f)", line, c[0x120 / 4], c[0x124 / 4]);
        for (uint32_t row = 0; row < 0x230 / 4; row += 16) {   // (the numbers themselves, to read the layout offline)
            char nums[400];
            int m = snprintf(nums, sizeof nums, "vr: true stereo: camera %#05x:", row * 4);
            for (uint32_t i = row; i < row + 16 && i < 0x230 / 4 && m < (int)sizeof nums - 16; i++) m += snprintf(nums + m, sizeof nums - m, " %.5g", c[i]);
            logf("%s", nums);
        }
    }
    if (!cam_log.first(key)) return;
    logf("vr: true stereo: camera read by %s %016llx (stage %d)%s, into %ux%u, the camera says %.0fx%.0f%s",
         kind == 2 ? "dispatch" : "draw", (unsigned long long)prog, stage, split ? ", per eye (split)" : "", width, height,
         c[0x120 / 4], c[0x124 / 4], eyes_camera(camera, size, width, height) ? ": moved per eye" : "");
}

VkShaderModule eyes_module(VkShaderModule module) {
    if (!module || !kx_known.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
    float k[2];
    uint64_t key;
    {
        std::lock_guard lk(kx_mu);
        k[0] = kx_now[0], k[1] = kx_now[1], key = kx_key.load();
    }
    const uint64_t id = (uint64_t)(uintptr_t)module ^ key;
    std::vector<uint32_t> words;
    {
        std::lock_guard lk(modules_mu);
        if (auto it = patched.find(id); it != patched.end()) {
            stale_use(!stale_ids.empty() && stale_ids.count(id), "patched");
            return it->second.failed ? VK_NULL_HANDLE : it->second.module;
        }
        auto s = spirv_of.find(module);
        if (s == spirv_of.end()) return VK_NULL_HANDLE;   // (a fragment stage, or made before the capture)
        words = s->second;
    }
    const int model = spirv_execution_model(words.data(), words.size());
    const SpirvEyesStage stage = model == 0 ? SpirvEyesStage::Vertex : model == 3 ? SpirvEyesStage::Geometry : SpirvEyesStage::Mesh;
    std::vector<uint32_t> out;
    const char* why = "";
    Patched made;
    if (spirv_eyes_patch(words.data(), words.size(), stage, k, out, &why)) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = out.size() * 4;
        ci.pCode = out.data();
        if (original_create(dev.device, &ci, nullptr, &made.module) != VK_SUCCESS) made.module = VK_NULL_HANDLE, why = "not made";
    }
    made.failed = !made.module;
    if (made.failed) {
        static std::atomic<int> told{0};
        if (told++ < 12) logf("vr: true stereo: a stage (model %d) drawn the same in both eyes: %s", model, why);
    }
    std::lock_guard lk(modules_mu);
    const auto [it, fresh] = patched.emplace(id, made);
    if (fresh) note_patched_from(module, id);
    if (fresh && made.module) spirv_of[made.module] = out;
    if (!fresh && made.module && it->second.module != made.module) original_destroy(dev.device, made.module, nullptr);
    return it->second.failed ? VK_NULL_HANDLE : it->second.module;
}

VkShaderModule hud_module(VkShaderModule module, float scale) {
    if (!module) return VK_NULL_HANDLE;
    uint32_t bits;
    std::memcpy(&bits, &scale, 4);
    const uint64_t id = ((uint64_t)(uintptr_t)module * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)bits << 1 | 1) ^ 0x4855440000000000ull;
    std::vector<uint32_t> words;
    {
        std::lock_guard lk(modules_mu);
        if (auto it = patched.find(id); it != patched.end()) {
            stale_use(!stale_ids.empty() && stale_ids.count(id), "patched");
            return it->second.failed ? VK_NULL_HANDLE : it->second.module;
        }
        auto s = spirv_of.find(module);
        if (s == spirv_of.end()) return VK_NULL_HANDLE;
        words = s->second;
    }
    std::vector<uint32_t> out;
    const char* why = "";
    Patched made;
    if (spirv_hud_patch(words.data(), words.size(), scale, out, &why)) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = out.size() * 4;
        ci.pCode = out.data();
        if (original_create(dev.device, &ci, nullptr, &made.module) != VK_SUCCESS) made.module = VK_NULL_HANDLE, why = "not made";
    }
    made.failed = !made.module;
    {
        static std::atomic<int> told{0};
        if (told++ < 12) logf("vr: HUD size: a 2D layer vertex stage %s%s", made.failed ? "left full size: " : "made smaller", why);
    }
    std::lock_guard lk(modules_mu);
    const auto [it, fresh] = patched.emplace(id, made);
    if (fresh) note_patched_from(module, id);
    if (fresh && made.module) spirv_of[made.module] = out;
    if (!fresh && made.module && it->second.module != made.module) original_destroy(dev.device, made.module, nullptr);
    return it->second.failed ? VK_NULL_HANDLE : it->second.module;
}

VkShaderModule eyes_sample_module(VkShaderModule module, uint64_t bindings, bool shifted) {
    if (shifted) {
        module = eyes_module(module);
        if (!module) return VK_NULL_HANDLE;
    }
    if (!bindings) return module;
    const auto key = std::make_pair(module, bindings);
    std::vector<uint32_t> words;
    {
        std::lock_guard lk(modules_mu);
        if (auto it = sampled.find(key); it != sampled.end()) {
            stale_use(!stale_sampled.empty() && stale_sampled.count(key), "sampling");
            return it->second.module;
        }
        const auto it = spirv_of.find(module);
        if (it == spirv_of.end()) return VK_NULL_HANDLE;
        words = it->second;
    }
    std::vector<uint32_t> out;
    const char* why = "";
    Patched made;
    if (spirv_eye_samples(words, bindings, out, &why)) {
        if (const char* dir = vr_research_getenv("RX_VR_EYES_SAMPLE_DUMP")) {
            char path[1024];
            snprintf(path, sizeof path, "%s/sample_%llx_%llx.spv", dir,
                     (unsigned long long)(uintptr_t)module, (unsigned long long)bindings);
            if (FILE* f = fopen(path, "wb")) { fwrite(out.data(), 4, out.size(), f); fclose(f); }
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = out.size() * 4; ci.pCode = out.data();
        if (original_create(dev.device, &ci, nullptr, &made.module) != VK_SUCCESS) why = "module creation";
    }
    made.failed = !made.module;
    if (made.failed) {
        static std::atomic<int> told{0};
        if (told++ < 24) logf("vr: shared stereo sampling fallback: model %d bindings %llx: %s",
            spirv_execution_model(words.data(), words.size()), (unsigned long long)bindings, why);
    }
    std::lock_guard lk(modules_mu);
    const auto [it, fresh] = sampled.emplace(key, made);
    // (STE-34: its words kept, for the camera patch made on top of it, eyes_camera_module. Without them every draw drawn once
    // for both eyes that both samples per-eye textures and reads the camera went unpatched, without a word: the actors' shadow
    // boxes among them)
    if (fresh && made.module) spirv_of[made.module] = out;
    if (!fresh && made.module) original_destroy(dev.device, made.module, nullptr);
    return it->second.module;
}

/// (research, STE-36: the fog dump) a program's learned camera copies as text, "b3 0:0 12:2; b5 0:1" ("none"), and the scene
/// projection's P00 the eye shift is made for
std::string eyes_copies_text(uint64_t prog) {
    std::lock_guard lk(copies_mu);
    auto it = copies_of_program.find(prog);
    if (it == copies_of_program.end() || it->second.empty()) return "none";
    std::string out;
    char b[32];
    for (const auto& [binding, at] : it->second) {
        snprintf(b, sizeof b, "%sb%d", out.empty() ? "" : "; ", binding);
        out += b;
        for (const auto& [o, kind] : at) snprintf(b, sizeof b, " %u:%u", o, kind), out += b;
    }
    return out;
}
float eyes_scene_p00() {
    std::lock_guard lk(kx_mu);
    return p00_now;
}
/// (STE-31) A program's learned camera copies as a signature for its pipeline key: 0 none learned
uint64_t eyes_camera_signature(uint64_t prog) {
    std::lock_guard lk(copies_mu);
    auto it = copies_of_program.find(prog);
    if (it == copies_of_program.end() || it->second.empty()) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& [binding, at] : it->second)
        for (const auto& [o, kind] : at) h = (h ^ ((uint64_t)binding << 40 | (uint64_t)o << 8 | kind)) * 0x100000001b3ull;
    return h | 1;
}
/// (STE-31) The fragment stage of a draw drawn once for both eyes, its reads of the camera copies corrected for each view
/// (vk_vr_spirv_camera.h): the same corrections eyes_move_camera_copies makes to a buffer, made in the shader. Null when the
/// stage reads none at a constant place (the draw then works out its lighting from the middle of the head, as before)
VkShaderModule eyes_camera_module(VkShaderModule module, uint64_t prog, uint64_t signature) {
    if (!module || !signature || !kx_known.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
    const uint64_t id = ((uint64_t)(uintptr_t)module * 0x9e3779b97f4a7c15ull) ^ signature ^ (kx_key.load() * 0xff51afd7ed558ccdull) ^
                        0x43414d0000000000ull;   // ("CAM")
    std::vector<uint32_t> words;
    {
        std::lock_guard lk(modules_mu);
        if (auto it = patched.find(id); it != patched.end()) {
            stale_use(!stale_ids.empty() && stale_ids.count(id), "patched");
            return it->second.failed ? VK_NULL_HANDLE : it->second.module;
        }
        auto s = spirv_of.find(module);
        if (s == spirv_of.end()) return VK_NULL_HANDLE;
        words = s->second;
    }
    // each view's corrections, as eyes_move_camera_copies moves each eye's copy (view 0 the left eye, view 1 the right)
    float p00;
    {
        std::lock_guard lk(kx_mu);
        p00 = p00_now;
    }
    const float ex[2] = {-eyes_half(), eyes_half()};
    std::vector<CameraFix> fixes;
    {
        std::lock_guard lk(copies_mu);
        if (auto it = copies_of_program.find(prog); it != copies_of_program.end())
            for (const auto& [binding, at] : it->second)
                for (const auto& [o, kind] : at) {
                    const uint32_t b = (uint32_t)binding;
                    if (kind == 0) fixes.push_back({b, o + 3, -1, {-ex[0], -ex[1]}});              // (view: x translation)
                    else if (kind == 2) fixes.push_back({b, o + 3, -1, {-p00 * ex[0], -p00 * ex[1]}});   // (view-projection)
                    else   // (inverse view: 3 rows; a view-space 4x4 (STE-33): 4 rows; each row's w by its x)
                        for (uint32_t r = 0; r < (kind == 3 ? 4u : 3u); r++)
                            fixes.push_back({b, o + r * 4 + 3, (int32_t)(o + r * 4), {ex[0], ex[1]}});
                }
    }
    std::vector<uint32_t> out;
    const char* why = "";
    int edits = 0, dynamic = 0;
    Patched made;
    if (spirv_camera_patch(words, fixes, out, &edits, &dynamic, &why)) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = out.size() * 4;
        ci.pCode = out.data();
        if (original_create(dev.device, &ci, nullptr, &made.module) != VK_SUCCESS) made.module = VK_NULL_HANDLE, why = "not made";
    }
    made.failed = !made.module;
    {
        static std::atomic<int> told{0};
        if (told++ < 64)
            logf("vr: true stereo: program %016llx's fragment stage: %d camera reads corrected per view%s%s (%d at a computed place)",
                 (unsigned long long)prog, made.failed ? 0 : edits, made.failed ? ", not patched: " : "", made.failed ? why : "",
                 dynamic);
    }
    std::lock_guard lk(modules_mu);
    const auto [it, fresh] = patched.emplace(id, made);
    if (fresh) note_patched_from(module, id);
    if (fresh && made.module) spirv_of[made.module] = out;
    if (!fresh && made.module && it->second.module != made.module) original_destroy(dev.device, made.module, nullptr);
    return it->second.failed ? VK_NULL_HANDLE : it->second.module;
}

OnceLog<80> sampling_log;   // (eyes_note_sampling's programs: 80 at most, each once)
bool eyes_note_sampling_open(uint64_t program) { return !sampling_log.closed(program); }
void eyes_note_sampling(uint64_t program, int sampled, bool moved, int count, bool mesh) {
    if (!sampled) return;
    if (sampling_log.first(program))
        logf("vr: true stereo: program %016llx samples %d two-layer texture(s): %s, %d vertices%s", (unsigned long long)program,
             sampled, moved ? "moved for each eye" : "not moved", count, mesh ? " (mesh)" : "");
}

std::atomic<int64_t> st_route[4];   // (routed draws: all, mesh, reading per-eye textures, full-screen)
void eyes_count_route(bool mesh, bool inputs, bool screen) {
    st_route[0]++, st_route[1] += mesh, st_route[2] += inputs, st_route[3] += screen;
}

void eyes_count(int passes, int draws, int moved) {
    st_passes += passes, st_draws += draws, st_moved += moved;
}

/// (vk_vr.cpp, each present) the frame's counts in the log now and then
void eyes_frame_end() {
    if (!eyes_wanted()) return;
    static uint64_t ended = ~0ull;   // (once a frame: from the scene's end, or the present without the headset)
    const uint64_t frame = texture_frame_no();
    if (ended == frame) return;
    ended = frame;
    static uint64_t pruned = 0;
    if (frame - pruned >= kViewIdle) pruned = frame, prune_views(frame);
    const int64_t f = ++st_frames;
    if (f % 600) return;
    size_t kept = 0, made = 0;
    {
        std::lock_guard lk(modules_mu);
        kept = spirv_of.size();
        for (auto& [id, p] : patched) made += !p.failed;
    }
    logf("vr: true stereo: a frame %.1f passes and %.1f draws in both eyes, %.1f of them moved for each eye; %zu stages kept, "
         "%zu made for both eyes", st_passes.exchange(0) / 600.0, st_draws.exchange(0) / 600.0, st_moved.exchange(0) / 600.0,
         kept, made);
    {
        std::lock_guard lk(views_mu);
        logf("vr: true stereo: two-layer views: %zu held, %zu waiting to be destroyed; %llu made, %llu destroyed in all", views.size(),
             views_retired.size(), (unsigned long long)st_views_made, (unsigned long long)st_views_destroyed);
    }
    logf("vr: true stereo: a frame %.1f camera checks, %.1f camera buffers read", st_cam_calls.exchange(0) / 600.0,
         st_cam_reads.exchange(0) / 600.0);
    logf("vr: true stereo: a frame %.1f draws drawn one eye after the other (%.1f mesh, %.1f reading per-eye textures, %.1f full-screen)",
         st_route[0].exchange(0) / 600.0, st_route[1].exchange(0) / 600.0, st_route[2].exchange(0) / 600.0,
         st_route[3].exchange(0) / 600.0);
}

// ---- RX_VR_EYES_TEST: both eyes' scene as pictures, without the headset ----
namespace {
struct Dump {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint32_t w = 0, h = 0;
    VkFormat format = VK_FORMAT_UNDEFINED, depth_format = VK_FORMAT_UNDEFINED;
    uint64_t frame = 0;   // (recorded at; read some frames later)
    uint64_t copied = 0;  // (the frame of the last copy: the frame's last one before its networks is the scene)
    bool pending = false;
    int number = 0;
};
Dump dump;

bool dump_buffer(VkDeviceSize size) {
    if (dump.buffer) return true;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &dump.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev.device, dump.buffer, &mr);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(dev.device, &ai, nullptr, &dump.memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev.device, dump.buffer, dump.memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev.device, dump.memory, 0, VK_WHOLE_SIZE, 0, &dump.mapped) != VK_SUCCESS) {
        logf("vr: true stereo test: no readback buffer");
        return false;
    }
    return true;
}

float unpack11(uint32_t v, int mant_bits) {   // (an unsigned 11- or 10-bit float: 5-bit exponent)
    const uint32_t e = v >> mant_bits, m = v & ((1u << mant_bits) - 1);
    if (!e) return m / (float)(1u << mant_bits) * std::ldexp(1.0f, -14);
    if (e == 31) return 65000.0f;
    return (1.0f + m / (float)(1u << mant_bits)) * std::ldexp(1.0f, (int)e - 15);
}

void dump_write() {
    const uint32_t w = dump.w, h = dump.h;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    const uint32_t* px = (const uint32_t*)dump.mapped;
    for (int eye = 0; eye < 2; eye++) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            const uint32_t v = px[(size_t)eye * w * h + i];
            float c[3];
            if (dump.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
                c[0] = unpack11(v & 0x7ff, 6), c[1] = unpack11(v >> 11 & 0x7ff, 6), c[2] = unpack11(v >> 22, 5);
            else
                c[0] = (v & 255) / 255.0f, c[1] = (v >> 8 & 255) / 255.0f, c[2] = (v >> 16 & 255) / 255.0f;
            for (int k = 0; k < 3; k++) {
                const float t = c[k] / (1.0f + c[k]);   // (a plain tone map: enough to see the two eyes' pictures)
                rgba[i * 4 + k] = (uint8_t)std::lround(std::pow(std::clamp(t, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f);
            }
            rgba[i * 4 + 3] = 255;
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/eyes_%02d_%s.png", capture_dir(), dump.number, eye ? "right" : "left");
        const bool ok = write_png(path, rgba.data(), w, h);
        logf("vr: true stereo test: %s %s", path, ok ? "written" : "NOT written");
        // Reinterpretation pictures are for format experiments, not routine HMD
        // diagnosis: omit their extra full-resolution disk writes on the headset.
        if (test_dir()) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                const uint32_t v = px[(size_t)eye * w * h + i];
                rgba[i * 4] = v & 255, rgba[i * 4 + 1] = v >> 8 & 255, rgba[i * 4 + 2] = v >> 16 & 255, rgba[i * 4 + 3] = 255;
            }
            snprintf(path, sizeof path, "%s/eyes_%02d_%s_as_rgba8.png", capture_dir(), dump.number, eye ? "right" : "left");
            write_png(path, rgba.data(), w, h);
        }
        if (dump.depth_format != VK_FORMAT_UNDEFINED) {   // (the depth: nearer is brighter, the range stretched)
            const uint8_t* base = (const uint8_t*)dump.mapped + (size_t)(2 + eye) * w * h * 4;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                float d;
                if (dump.depth_format == VK_FORMAT_D32_SFLOAT || dump.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT)
                    std::memcpy(&d, base + i * 4, 4);
                else {
                    uint32_t v;
                    std::memcpy(&v, base + i * 4, 4);
                    d = (v & 0xffffff) / 16777215.0f;
                }
                const uint8_t g = (uint8_t)std::lround(std::clamp(std::pow(std::clamp(d, 0.0f, 1.0f), 32.0f), 0.0f, 1.0f) * 255.0f);
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g, rgba[i * 4 + 3] = 255;
            }
            snprintf(path, sizeof path, "%s/eyes_%02d_%s_depth.png", capture_dir(), dump.number, eye ? "right" : "left");
            write_png(path, rgba.data(), w, h);
        }
    }
}
}  // namespace

/// (vk_vr.cpp scene_done) the scene's end: with RX_VR_EYES_TEST, both layers of the scene's colour copied for a picture
/// now and then (and written some frames later, from this same call)
bool no_pictures() { static const bool v = vr_research_on("RX_VR_EYES_NOPICTURES"); return v; }   // (for timing)
int capture_pair_limit(bool final) { return test_dir() ? (final ? 6 : 8) : 3; }

void eyes_scene_done(VkCommandBuffer cmd, const Texture* color, const Texture* depth) {
    if (!capture_dir() || no_pictures()) return;
    const uint64_t now = texture_frame_no();
    if (dump.pending && now >= dump.frame + 12) {   // (long finished on the GPU)
        dump_write();
        dump.pending = false;
        dump.number++;
    }
    static uint64_t last = 0;
    // (RX_VR_EYES_PICTURE_EVERY=<presents>: the pictures further apart, to reach a spot later in a scripted run; default 240)
    static const uint64_t every = [] {
        const char* e = vr_research_getenv("RX_VR_EYES_PICTURE_EVERY");
        return e && atoi(e) > 0 ? (uint64_t)atoi(e) : 240ull;
    }();
    if (dump.pending || dump.number >= capture_pair_limit(false) || !color || !color->vr_eyes || !color->image || (now < last + every && dump.copied != now))
        return;
    if (color->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && color->format != VK_FORMAT_R8G8B8A8_UNORM) return;
    const VkDeviceSize layer = (VkDeviceSize)color->width * color->height * 4;
    if (!dump_buffer(layer * 4) || (dump.w && (dump.w != color->width || dump.h != color->height))) return;
    const bool with_depth = depth && depth->vr_eyes && depth->image && depth->width == color->width && depth->height == color->height &&
                            (depth->format == VK_FORMAT_D32_SFLOAT || depth->format == VK_FORMAT_D24_UNORM_S8_UINT ||
                             depth->format == VK_FORMAT_X8_D24_UNORM_PACK32 || depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT);
    dump.depth_format = with_depth ? depth->format : VK_FORMAT_UNDEFINED;
    last = now;
    dump.w = color->width, dump.h = color->height, dump.format = color->format;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (direct calls into the replay's command buffer: vk_vr.cpp frame_capture.inc)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    VkBufferImageCopy c[2]{};
    for (int eye = 0; eye < 2; eye++) {
        c[eye].bufferOffset = layer * eye;
        c[eye].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)eye, 1};
        c[eye].imageExtent = {color->width, color->height, 1};
    }
    vkCmdCopyImageToBuffer(cmd, color->image, VK_IMAGE_LAYOUT_GENERAL, dump.buffer, 2, c);
    if (with_depth) {   // (and both layers of the depth: drawn by the geometry itself, the eyes' parallax shows there)
        for (int eye = 0; eye < 2; eye++) {
            c[eye].bufferOffset = layer * (2 + eye);
            c[eye].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, (uint32_t)eye, 1};
        }
        vkCmdCopyImageToBuffer(cmd, depth->image, VK_IMAGE_LAYOUT_GENERAL, dump.buffer, 2, c);
    }
    vkCmdPipelineBarrier2(cmd, &di);
    dump.copied = now;   // (taken again at the frame's next copy; kept at its networks: eyes_frame_complete)
}

void eyes_frame_complete() {
    if (!capture_dir() || dump.pending || dump.copied != texture_frame_no()) return;
    dump.frame = dump.copied, dump.pending = true;
    logf("vr: true stereo test: both eyes of the scene (%ux%u) copied at frame %llu", dump.w, dump.h, (unsigned long long)dump.frame);
}

/// (vk_vr.cpp present, RX_VR_EYES_TEST) both layers of the final picture, when it was made with both: the right eye's whole
/// picture as the game's own post-processing made it, now and then
void eyes_present_test(VkCommandBuffer cmd, VkImage src, uint32_t w, uint32_t h) {
    eyes_frame_end();   // (the frame's counts, in a run without the headset too)
    if (!capture_dir() || no_pictures()) return;
    {
        static int told;
        if (!src && told++ < 3) logf("vr: true stereo test: the presented picture (%ux%u) has one layer", w, h);
    }
    if (!src) return;
    static Dump fin;
    const uint64_t now = texture_frame_no();
    if (fin.pending && now >= fin.frame + 12) {
        std::vector<uint8_t> rgba((size_t)fin.w * fin.h * 4);
        for (int eye = 0; eye < 2; eye++) {
            const uint8_t* px = (const uint8_t*)fin.mapped + (size_t)eye * fin.w * fin.h * 4;
            for (size_t i = 0; i < (size_t)fin.w * fin.h; i++) {
                const bool bgr = fin.format == VK_FORMAT_B8G8R8A8_UNORM || fin.format == VK_FORMAT_B8G8R8A8_SRGB;
                rgba[i * 4] = px[i * 4 + (bgr ? 2 : 0)], rgba[i * 4 + 1] = px[i * 4 + 1], rgba[i * 4 + 2] = px[i * 4 + (bgr ? 0 : 2)];
                rgba[i * 4 + 3] = 255;
            }
            char path[1024];
            snprintf(path, sizeof path, "%s/final_%02d_%s.png", capture_dir(), fin.number, eye ? "right" : "left");
            logf("vr: true stereo test: %s %s", path, write_png(path, rgba.data(), fin.w, fin.h) ? "written" : "NOT written");
        }
        fin.pending = false, fin.number++;
    }
    static uint64_t last = 0;
    // (gameplay only: the scene was drawn in both eyes and copied in the last frames)
    if (fin.pending || fin.number >= capture_pair_limit(true) || now < last + 240 || !dump.copied || dump.copied + 2 < now) return;
    VkFormat format;
    {
        std::lock_guard lk(two_mu);
        auto it = two_layer.find(src);
        static int told;
        if (told < 6 && now > 3000) {
            told++;
            char line[600];
            int n = snprintf(line, sizeof line, "vr: true stereo test: the present's source %ux%u is %s; two-layer images:", w, h,
                             it == two_layer.end() ? "ONE layer" : "two layers");
            for (auto& [img, info] : two_layer)
                if (n < 560) n += snprintf(line + n, sizeof line - n, " #%u vk %d", info.first, (int)info.second);
            logf("%s", line);
        }
        if (it == two_layer.end()) return;
        format = it->second.second;
    }
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_B8G8R8A8_UNORM &&
        format != VK_FORMAT_B8G8R8A8_SRGB && format != VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        return;
    const VkDeviceSize layer = (VkDeviceSize)w * h * 4;
    if (!fin.buffer) {
        std::swap(dump, fin);   // (dump_buffer makes the test's buffer: one of its own for the final picture)
        const bool ok = dump_buffer(layer * 2);
        std::swap(dump, fin);
        if (!ok) return;
    }
    if (fin.w && (fin.w != w || fin.h != h)) return;
    last = now;
    fin.w = w, fin.h = h, fin.format = format;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (as eyes_scene_done)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    VkBufferImageCopy c[2]{};
    for (int eye = 0; eye < 2; eye++) {
        c[eye].bufferOffset = layer * eye;
        c[eye].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)eye, 1};
        c[eye].imageExtent = {w, h, 1};
    }
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_GENERAL, fin.buffer, 2, c);
    vkCmdPipelineBarrier2(cmd, &di);
    fin.frame = now, fin.pending = true;
}

}  // namespace vr
}  // namespace mkwvk
