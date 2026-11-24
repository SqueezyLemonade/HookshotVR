// Hookshot VR: a stand-in OpenXR runtime for headless tests of the headset's path (no headset, no OpenXR runtime).
//
// Built as libopenxr_loader.so.1 (vr/tools/fake_headset/build.sh) and found first through LD_LIBRARY_PATH, it is what the
// game's dlopen of the OpenXR loader gets (vr/headset/runtime_loader.inc): xrGetInstanceProcAddr hands out the functions
// below. What runs is the game's own headset thread, eyes, timeline and queue: swapchain images are real Vulkan images made
// on the game's device, and each swapchain image's acquire and release submits a barrier to the headset's queue (index 1) from the
// calling thread, as WiVRn's in-process client does (its image barriers). Frames are paced at FAKE_XR_HZ (90), the head
// turns FAKE_XR_TURN degrees each side (25) over 8 s (tilted FAKE_XR_PITCH degrees, 0), the hands hang still with nothing pressed. Every 10 s one line on
// stderr: the frames ended and the layers they carried. A dev tool for this PC: never shipped, holds nothing of the game.
//
// Faults, for the headset path's failure handling (vr/headset/eye_rendering.inc headset_submit, each logged on stderr):
//   FAKE_XR_FAIL_ACQUIRE=<n>  every n-th xrAcquireSwapchainImage fails (XR_ERROR_RUNTIME_FAILURE)
//   FAKE_XR_FAIL_WAIT=<n>     every n-th xrWaitSwapchainImage fails
//   FAKE_XR_STALL_S=<s>       every s seconds the headset's queue is held FAKE_XR_STALL_MS (1500) at an acquire: a submit that
//                             waits for a timeline value the stand-in signals from the CPU later, so the game's next submit
//                             there waits longer than its 1 s and its wait gives up (the GPU itself never hangs)
// Layers, for the headset's world marks (the aim reticles, vr/items/aim):
//   FAKE_XR_QUADS=1           once a second, each quad layer of the frame on stderr: its place (local space, m), size and
//                             swapchain, and the head the frame was made with
// The eyes, for the projection layer's per-eye FOV (vr/headset/eye_rendering.inc located_eye_cut):
//   FAKE_XR_FOV=<l>,<r>,<u>,<d>  the left eye's FOV in degrees (left and down negative), the right eye's its mirror image: an
//                             asymmetric headset's, as a Rift's (default: a symmetric 106 x 108 degrees)
//   FAKE_XR_CANT=<l>[,<r>]    the eyes turned outward by that many degrees (canted displays, 0; one value: both)
//   FAKE_XR_SWAY=<m>          the head also moves that far each side and as far up and down (0), over 5 s
//   FAKE_XR_MENU_AT=<s>[,..]  the menu button held 0.9 s at each of those seconds (the VR menu opened and closed)
//   FAKE_XR_DUMP=<folder>     every FAKE_XR_DUMP_S seconds (20), at most FAKE_XR_DUMP_N times (6), each projection view's
//                             swapchain image as <folder>/eye<n>-<view>.ppm, and its pose, fov and imageRect on stderr, with each eye's
//                             located pose; a frame with no projection layer: its first quad (view 9) with its pose and size
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

namespace {

std::mutex mu;
uint64_t next_handle = 0x1000;
template <class H>
H make_handle() {
    std::lock_guard lk(mu);
    return (H)(uintptr_t)(next_handle += 0x10);
}
int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double env_num(const char* name, double def) {
    const char* e = getenv(name);
    return e && *e ? atof(e) : def;
}

// ---- Vulkan, the runtime's own function pointers ----
PFN_vkGetInstanceProcAddr gipa = nullptr;
PFN_vkGetDeviceProcAddr gdpa = nullptr;
PFN_vkEnumeratePhysicalDevices enum_physical = nullptr;
PFN_vkGetPhysicalDeviceProperties physical_props = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties memory_props = nullptr;
PFN_vkCreateImage create_image = nullptr;
PFN_vkDestroyImage destroy_image = nullptr;
PFN_vkGetImageMemoryRequirements image_reqs = nullptr;
PFN_vkAllocateMemory allocate = nullptr;
PFN_vkFreeMemory free_memory = nullptr;
PFN_vkBindImageMemory bind_image = nullptr;
PFN_vkGetDeviceQueue get_queue = nullptr;
PFN_vkQueueSubmit queue_submit = nullptr;
PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
PFN_vkCreateCommandPool create_pool = nullptr;
PFN_vkAllocateCommandBuffers alloc_cmd = nullptr;
PFN_vkBeginCommandBuffer begin_cmd = nullptr;
PFN_vkEndCommandBuffer end_cmd = nullptr;
PFN_vkResetCommandBuffer reset_cmd = nullptr;
PFN_vkCmdPipelineBarrier cmd_barrier = nullptr;
PFN_vkCreateFence create_fence = nullptr;
PFN_vkWaitForFences wait_fences = nullptr;
PFN_vkResetFences reset_fences = nullptr;
PFN_vkCreateSemaphore create_semaphore = nullptr;
PFN_vkSignalSemaphore signal_semaphore = nullptr;
PFN_vkCreateBuffer create_buffer = nullptr;   // (FAKE_XR_DUMP's readback)
PFN_vkDestroyBuffer destroy_buffer = nullptr;
PFN_vkGetBufferMemoryRequirements buffer_reqs = nullptr;
PFN_vkBindBufferMemory bind_buffer = nullptr;
PFN_vkMapMemory map_memory = nullptr;
PFN_vkCmdCopyImageToBuffer cmd_copy_to_buffer = nullptr;

bool load_vulkan(VkInstance instance) {
    if (!gipa) {
        void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!m) return false;
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(m, "vkGetInstanceProcAddr");
    }
    if (!gipa || !instance) return false;
#define I(f, p) p = (decltype(p))gipa(instance, #f)
    I(vkGetDeviceProcAddr, gdpa);
    I(vkEnumeratePhysicalDevices, enum_physical);
    I(vkGetPhysicalDeviceProperties, physical_props);
    I(vkGetPhysicalDeviceMemoryProperties, memory_props);
#undef I
    return gdpa && enum_physical && physical_props && memory_props;
}
bool load_device(VkDevice device) {
#define D(f, p) p = (decltype(p))gdpa(device, #f)
    D(vkCreateImage, create_image);
    D(vkDestroyImage, destroy_image);
    D(vkGetImageMemoryRequirements, image_reqs);
    D(vkAllocateMemory, allocate);
    D(vkFreeMemory, free_memory);
    D(vkBindImageMemory, bind_image);
    D(vkGetDeviceQueue, get_queue);
    D(vkQueueSubmit, queue_submit);
    D(vkQueueWaitIdle, queue_wait_idle);
    D(vkCreateCommandPool, create_pool);
    D(vkAllocateCommandBuffers, alloc_cmd);
    D(vkBeginCommandBuffer, begin_cmd);
    D(vkEndCommandBuffer, end_cmd);
    D(vkResetCommandBuffer, reset_cmd);
    D(vkCmdPipelineBarrier, cmd_barrier);
    D(vkCreateFence, create_fence);
    D(vkWaitForFences, wait_fences);
    D(vkResetFences, reset_fences);
    D(vkCreateSemaphore, create_semaphore);
    D(vkSignalSemaphore, signal_semaphore);
    D(vkCreateBuffer, create_buffer);
    D(vkDestroyBuffer, destroy_buffer);
    D(vkGetBufferMemoryRequirements, buffer_reqs);
    D(vkBindBufferMemory, bind_buffer);
    D(vkMapMemory, map_memory);
    D(vkCmdCopyImageToBuffer, cmd_copy_to_buffer);
#undef D
    return create_pool && alloc_cmd && begin_cmd && end_cmd && reset_cmd && cmd_barrier && create_fence && wait_fences &&
           reset_fences && create_semaphore && signal_semaphore && create_image && destroy_image && image_reqs && allocate && free_memory && bind_image && get_queue && queue_submit &&
           queue_wait_idle;
}

// ---- the session ----
struct Session {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore stall = VK_NULL_HANDLE;   // (FAKE_XR_STALL_S: a timeline the queue waits on, signalled from the CPU)
    uint64_t stall_value = 0;
    int64_t stall_next = 0;
    uint64_t acquires = 0, waits = 0;
    std::deque<XrSessionState> events;
    bool running = false;
    int64_t next_frame = 0, period = 11111111;
    uint64_t frames = 0, layers = 0, said_at = 0, said_frames = 0, said_layers = 0;
} sess;
XrSession the_session = XR_NULL_HANDLE;
XrInstance the_instance = XR_NULL_HANDLE;
VkPhysicalDevice chosen_physical = VK_NULL_HANDLE;

struct Chain {
    std::vector<XrSwapchainImageVulkanKHR> images;
    std::vector<VkDeviceMemory> memory;
    uint32_t next = 0;
    uint32_t w = 0, h = 0;
};
std::unordered_map<XrSwapchain, Chain> chains;
enum SpaceKind { kLocal, kView, kHandLeft, kHandRight };
std::unordered_map<XrSpace, SpaceKind> spaces;
std::unordered_map<std::string, XrPath> paths;
std::vector<std::string> path_names{""};

XrPath path_of(const char* s) {
    std::lock_guard lk(mu);
    auto it = paths.find(s);
    if (it != paths.end()) return it->second;
    path_names.emplace_back(s);
    return paths[s] = (XrPath)(path_names.size() - 1);
}

// the runtime's own queue use, as WiVRn's client's image barriers: a command buffer with the image's barrier (its layout kept),
// submitted to the headset's queue from the calling thread with a fence, waited for
void touch_queue(VkImage image) {
    if (!sess.queue || !sess.cmd) return;
    reset_cmd(sess.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin_cmd(sess.cmd, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (image) cmd_barrier(sess.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    end_cmd(sess.cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sess.cmd;
    reset_fences(sess.device, 1, &sess.fence);
    if (queue_submit(sess.queue, 1, &si, sess.fence) == VK_SUCCESS) wait_fences(sess.device, 1, &sess.fence, VK_TRUE, 1000000000ull);
}

/// FAKE_XR_STALL_S: once due, the headset's queue held for FAKE_XR_STALL_MS behind a wait the CPU ends later
void maybe_stall() {
    const double every = env_num("FAKE_XR_STALL_S", 0);
    if (every <= 0 || !sess.queue) return;
    const int64_t now = now_ns();
    if (!sess.stall_next) sess.stall_next = now + (int64_t)(every * 1e9);
    if (now < sess.stall_next) return;
    sess.stall_next = now + (int64_t)(every * 1e9);
    if (!sess.stall) {
        VkSemaphoreTypeCreateInfo tt{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tt.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &tt;
        if (create_semaphore(sess.device, &ci, nullptr, &sess.stall) != VK_SUCCESS) return;
    }
    const uint64_t value = ++sess.stall_value;
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 1;
    ts.pWaitSemaphoreValues = &value;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sess.stall;
    si.pWaitDstStageMask = &stage;
    if (queue_submit(sess.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return;
    const int ms = (int)env_num("FAKE_XR_STALL_MS", 1500);
    fprintf(stderr, "[fake xr] the headset's queue held %d ms (stall %llu)\n", ms, (unsigned long long)value);
    const VkDevice device = sess.device;
    const VkSemaphore sem = sess.stall;
    std::thread([device, sem, value, ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        VkSemaphoreSignalInfo sg{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        sg.semaphore = sem, sg.value = value;
        signal_semaphore(device, &sg);
    }).detach();
}

XrPosef head_pose(XrTime t) {
    const double turn = env_num("FAKE_XR_TURN", 25.0) * M_PI / 180.0;
    const double yaw = turn * std::sin((double)t * 1e-9 * 2.0 * M_PI / 8.0);
    // (FAKE_XR_PITCH=<deg>: the head tilted by that much too, negative down: to look at Link's legs and arms in first person)
    const double pitch = env_num("FAKE_XR_PITCH", 0.0) * M_PI / 180.0;
    const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    XrPosef p{};
    p.orientation = {(float)(cy * sp), (float)(sy * cp), (float)(-sy * sp), (float)(cy * cp)};   // (yaw, then pitch)
    const double sway = env_num("FAKE_XR_SWAY", 0.0), at = (double)t * 1e-9 * 2.0 * M_PI / 5.0;
    p.position = {(float)(sway * std::sin(at)), (float)(1.6 + sway * std::cos(at)), (float)(0.5 * sway * std::sin(2 * at))};
    return p;
}

XrPosef compose(const XrPosef& a, const XrPosef& b) {
    const XrQuaternionf &p = a.orientation, &q = b.orientation;
    XrPosef o;
    o.orientation = {p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y, p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
                     p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w, p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z};
    const XrVector3f& v = b.position;   // (v + 2 w (u x v) + 2 u x (u x v))
    const float cx = p.y * v.z - p.z * v.y, cy = p.z * v.x - p.x * v.z, cz = p.x * v.y - p.y * v.x;
    o.position = {a.position.x + v.x + 2 * (p.w * cx + p.y * cz - p.z * cy), a.position.y + v.y + 2 * (p.w * cy + p.z * cx - p.x * cz),
                  a.position.z + v.z + 2 * (p.w * cz + p.x * cy - p.y * cx)};
    return o;
}

// ---- the functions ----
XrResult XRAPI_CALL x_GetInstanceProperties(XrInstance, XrInstanceProperties* p) {
    p->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    snprintf(p->runtimeName, sizeof p->runtimeName, "Hookshot VR fake headset");
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_CreateInstance(const XrInstanceCreateInfo*, XrInstance* out) {
    *out = the_instance = make_handle<XrInstance>();
    fprintf(stderr, "[fake xr] instance made\n");
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_DestroyInstance(XrInstance) { return XR_SUCCESS; }
XrResult XRAPI_CALL x_GetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* id) {
    *id = 1;
    return XR_SUCCESS;
}
XrResult list_out(const char* text, uint32_t cap, uint32_t* n, char* buf) {
    *n = (uint32_t)strlen(text) + 1;
    if (cap && buf) {
        if (cap < *n) return XR_ERROR_SIZE_INSUFFICIENT;
        memcpy(buf, text, *n);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_GetVulkanInstanceExtensionsKHR(XrInstance, XrSystemId, uint32_t cap, uint32_t* n, char* buf) {
    return list_out("", cap, n, buf);
}
XrResult XRAPI_CALL x_GetVulkanDeviceExtensionsKHR(XrInstance, XrSystemId, uint32_t cap, uint32_t* n, char* buf) {
    return list_out("", cap, n, buf);
}
XrResult XRAPI_CALL x_GetVulkanGraphicsDeviceKHR(XrInstance, XrSystemId, VkInstance instance, VkPhysicalDevice* out) {
    // (the first discrete GPU, as the game's own choice on this PC)
    if (!load_vulkan(instance)) return XR_ERROR_RUNTIME_FAILURE;
    uint32_t n = 0;
    enum_physical(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> list(n);
    enum_physical(instance, &n, list.data());
    *out = VK_NULL_HANDLE;
    for (VkPhysicalDevice p : list) {
        VkPhysicalDeviceProperties pr;
        physical_props(p, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !*out) *out = p;
    }
    if (!*out && n) *out = list[0];
    chosen_physical = *out;
    return *out ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}
XrResult XRAPI_CALL x_GetVulkanGraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR* r) {
    r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_CreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* out) {
    const auto* gb = (const XrGraphicsBindingVulkanKHR*)ci->next;
    if (!gb || gb->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!load_vulkan(gb->instance)) return XR_ERROR_RUNTIME_FAILURE;
    sess.instance = gb->instance, sess.physical = gb->physicalDevice, sess.device = gb->device;
    if (!load_device(gb->device)) return XR_ERROR_RUNTIME_FAILURE;
    get_queue(gb->device, gb->queueFamilyIndex, gb->queueIndex, &sess.queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = gb->queueFamilyIndex;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (create_pool(gb->device, &pc, nullptr, &sess.pool) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    ca.commandPool = sess.pool;
    if (alloc_cmd(gb->device, &ca, &sess.cmd) != VK_SUCCESS || create_fence(gb->device, &fc, nullptr, &sess.fence) != VK_SUCCESS)
        return XR_ERROR_RUNTIME_FAILURE;
    sess.period = (int64_t)(1e9 / std::max(10.0, env_num("FAKE_XR_HZ", 90.0)));
    sess.events = {XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY};
    *out = the_session = make_handle<XrSession>();
    fprintf(stderr, "[fake xr] session made on queue %u.%u, %.0f Hz\n", gb->queueFamilyIndex, gb->queueIndex, 1e9 / sess.period);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_DestroySession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL x_BeginSession(XrSession, const XrSessionBeginInfo*) {
    sess.running = true;
    for (XrSessionState st : {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED}) sess.events.push_back(st);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_EndSession(XrSession) {
    sess.running = false;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_PollEvent(XrInstance, XrEventDataBuffer* ev) {
    if (sess.events.empty()) return XR_EVENT_UNAVAILABLE;
    auto* e = (XrEventDataSessionStateChanged*)ev;
    *e = {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    e->session = the_session;
    e->state = sess.events.front();
    e->time = now_ns();
    sess.events.pop_front();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* ci, XrSpace* out) {
    *out = make_handle<XrSpace>();
    std::lock_guard lk(mu);
    spaces[*out] = ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? kView : kLocal;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_EnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* n, int64_t* out) {
    const int64_t formats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    *n = 2;
    if (cap && out) {
        if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
        memcpy(out, formats, sizeof formats);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_CreateSwapchain(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkPhysicalDeviceMemoryProperties mp;
    memory_props(sess.physical, &mp);
    Chain c;
    c.w = ci->width, c.h = ci->height;
    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = (VkFormat)ci->format;
        ic.extent = {ci->width, ci->height, 1};
        ic.mipLevels = 1, ic.arrayLayers = ci->arraySize ? ci->arraySize : 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = usage;
        VkImage img = VK_NULL_HANDLE;
        if (create_image(sess.device, &ic, nullptr, &img) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
        VkMemoryRequirements mr;
        image_reqs(sess.device, img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; t++)
            if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                ai.memoryTypeIndex = t;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (ai.memoryTypeIndex == UINT32_MAX || allocate(sess.device, &ai, nullptr, &m) != VK_SUCCESS ||
            bind_image(sess.device, img, m, 0) != VK_SUCCESS)
            return XR_ERROR_RUNTIME_FAILURE;
        XrSwapchainImageVulkanKHR x{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
        x.image = img;
        c.images.push_back(x);
        c.memory.push_back(m);
    }
    *out = make_handle<XrSwapchain>();
    std::lock_guard lk(mu);
    chains[*out] = std::move(c);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_DestroySwapchain(XrSwapchain sc) {
    Chain c;
    {
        std::lock_guard lk(mu);
        auto it = chains.find(sc);
        if (it == chains.end()) return XR_ERROR_HANDLE_INVALID;
        c = std::move(it->second);
        chains.erase(it);
    }
    queue_wait_idle(sess.queue);   // (the caller's thread: the one that submits there)
    for (size_t i = 0; i < c.images.size(); i++) destroy_image(sess.device, c.images[i].image, nullptr), free_memory(sess.device, c.memory[i], nullptr);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_EnumerateSwapchainImages(XrSwapchain sc, uint32_t cap, uint32_t* n, XrSwapchainImageBaseHeader* out) {
    std::lock_guard lk(mu);
    auto it = chains.find(sc);
    if (it == chains.end()) return XR_ERROR_HANDLE_INVALID;
    *n = (uint32_t)it->second.images.size();
    if (cap && out) {
        if (cap < *n) return XR_ERROR_SIZE_INSUFFICIENT;
        for (uint32_t i = 0; i < *n; i++) ((XrSwapchainImageVulkanKHR*)out)[i].image = it->second.images[i].image;
    }
    return XR_SUCCESS;
}
std::unordered_map<XrSwapchain, VkImage> acquired;
XrResult XRAPI_CALL x_AcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    if (const int n = (int)env_num("FAKE_XR_FAIL_ACQUIRE", 0); n > 0 && ++sess.acquires % n == 0) {
        fprintf(stderr, "[fake xr] acquire %llu failed (FAKE_XR_FAIL_ACQUIRE)\n", (unsigned long long)sess.acquires);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    VkImage img;
    {
        std::lock_guard lk(mu);
        auto it = chains.find(sc);
        if (it == chains.end()) return XR_ERROR_HANDLE_INVALID;
        *index = it->second.next;
        img = acquired[sc] = it->second.images[*index].image;
        it->second.next = (it->second.next + 1) % (uint32_t)it->second.images.size();
    }
    touch_queue(img);
    maybe_stall();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_WaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) {
    if (const int n = (int)env_num("FAKE_XR_FAIL_WAIT", 0); n > 0 && ++sess.waits % n == 0) {
        fprintf(stderr, "[fake xr] wait %llu failed (FAKE_XR_FAIL_WAIT)\n", (unsigned long long)sess.waits);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_ReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo*) {
    VkImage img;
    {
        std::lock_guard lk(mu);
        img = acquired[sc];
    }
    touch_queue(img);
    return XR_SUCCESS;
}
/// FAKE_XR_DUMP: a released swapchain image (left by the game as a colour attachment) read back and written as a PPM
void dump_image(VkImage image, uint32_t w, uint32_t h, const std::string& file) {
    if (!create_buffer || !map_memory || !cmd_copy_to_buffer || !image || !w || !h) return;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = (VkDeviceSize)w * h * 4;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (create_buffer(sess.device, &bc, nullptr, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    buffer_reqs(sess.device, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    memory_props(sess.physical, &mp);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mp.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; t++)
        if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) ai.memoryTypeIndex = t;
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (ai.memoryTypeIndex == UINT32_MAX || allocate(sess.device, &ai, nullptr, &m) != VK_SUCCESS) return destroy_buffer(sess.device, buf, nullptr);
    bind_buffer(sess.device, buf, m, 0);
    reset_cmd(sess.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin_cmd(sess.cmd, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmd_barrier(sess.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    cmd_copy_to_buffer(sess.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    std::swap(b.oldLayout, b.newLayout);
    cmd_barrier(sess.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    end_cmd(sess.cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sess.cmd;
    reset_fences(sess.device, 1, &sess.fence);
    void* p = nullptr;
    if (queue_submit(sess.queue, 1, &si, sess.fence) == VK_SUCCESS &&
        wait_fences(sess.device, 1, &sess.fence, VK_TRUE, 2000000000ull) == VK_SUCCESS &&
        map_memory(sess.device, m, 0, bc.size, 0, &p) == VK_SUCCESS && p) {
        if (FILE* f = fopen(file.c_str(), "wb")) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            std::vector<unsigned char> row(w * 3);
            for (uint32_t y = 0; y < h; y++) {
                const unsigned char* src = (const unsigned char*)p + (size_t)y * w * 4;
                for (uint32_t x = 0; x < w; x++) row[x * 3] = src[x * 4], row[x * 3 + 1] = src[x * 4 + 1], row[x * 3 + 2] = src[x * 4 + 2];
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
    }
    destroy_buffer(sess.device, buf, nullptr);
    free_memory(sess.device, m, nullptr);
}
XrResult XRAPI_CALL x_WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* fs) {
    int64_t t = now_ns();
    if (!sess.next_frame || t > sess.next_frame + 4 * sess.period) sess.next_frame = t;
    if (sess.next_frame > t) std::this_thread::sleep_for(std::chrono::nanoseconds(sess.next_frame - t));
    fs->predictedDisplayTime = sess.next_frame + 2 * sess.period;
    fs->predictedDisplayPeriod = sess.period;
    fs->shouldRender = sess.running;
    sess.next_frame += sess.period;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_BeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL x_EndFrame(XrSession, const XrFrameEndInfo* ei) {
    sess.frames++;
    sess.layers += ei->layerCount;
    const uint64_t t = (uint64_t)now_ns();
    static const bool quads = env_num("FAKE_XR_QUADS", 0) != 0;
    static uint64_t quads_at;
    if (quads && t - quads_at >= 1000000000ull) {
        quads_at = t;
        for (uint32_t i = 0; i < ei->layerCount; i++) {
            const XrCompositionLayerBaseHeader* l = ei->layers[i];
            if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_QUAD) continue;
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)l;
            fprintf(stderr, "[fake xr] quad layer %u of %u: at %.3f %.3f %.3f, %.3f x %.3f m, swapchain %p\n", i, ei->layerCount,
                    q->pose.position.x, q->pose.position.y, q->pose.position.z, q->size.width, q->size.height,
                    (void*)q->subImage.swapchain);
        }
    }
    static const char* dump_dir = getenv("FAKE_XR_DUMP");
    static uint64_t dump_at = 0;
    static int dumps = 0;
    const uint64_t dump_every = (uint64_t)(env_num("FAKE_XR_DUMP_S", 20) * 1e9);
    if (dump_dir && *dump_dir && dumps < (int)env_num("FAKE_XR_DUMP_N", 6) && t - dump_at >= dump_every) {
        for (uint32_t i = 0; i < ei->layerCount; i++) {
            const XrCompositionLayerBaseHeader* l = ei->layers[i];
            if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const XrCompositionLayerProjection* pl = (const XrCompositionLayerProjection*)l;
            if (!dump_at) {   // (the first projection layer starts the clock: the first dump one period later)
                dump_at = t;
                break;
            }
            dump_at = t, dumps++;
            for (uint32_t v = 0; v < pl->viewCount && v < 2; v++) {
                const XrCompositionLayerProjectionView& pv = pl->views[v];
                VkImage img = VK_NULL_HANDLE;
                uint32_t w = 0, h = 0;
                {
                    std::lock_guard lk(mu);
                    auto it = chains.find(pv.subImage.swapchain);
                    if (it != chains.end()) img = acquired[pv.subImage.swapchain], w = it->second.w, h = it->second.h;
                }
                const std::string file = std::string(dump_dir) + "/eye" + std::to_string(dumps) + "-" + std::to_string(v) + ".ppm";
                fprintf(stderr, "[fake xr] dump %d view %u: %s (%ux%u), pose x %.4f y %.4f z %.4f turn %.5f %.5f %.5f %.5f, fov L %.2f R %.2f U %.2f D %.2f deg, "
                        "imageRect %d,%d %dx%d\n", dumps, v, file.c_str(), w, h, pv.pose.position.x, pv.pose.position.y,
                        pv.pose.position.z, pv.pose.orientation.x, pv.pose.orientation.y, pv.pose.orientation.z, pv.pose.orientation.w,
                        pv.fov.angleLeft * 57.2958f, pv.fov.angleRight * 57.2958f, pv.fov.angleUp * 57.2958f,
                        pv.fov.angleDown * 57.2958f, pv.subImage.imageRect.offset.x, pv.subImage.imageRect.offset.y,
                        pv.subImage.imageRect.extent.width, pv.subImage.imageRect.extent.height);
                dump_image(img, w, h, file);
            }
            break;
        }
        const XrCompositionLayerBaseHeader* first = ei->layerCount ? ei->layers[0] : nullptr;
        if (first && first->type == XR_TYPE_COMPOSITION_LAYER_QUAD && ((const XrCompositionLayerQuad*)first)->size.width > 1) {
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)first;   // (a flat frame as a quad, not the blank layer)
            if (!dump_at) dump_at = t;
            else {
                dump_at = t, dumps++;
                VkImage img = VK_NULL_HANDLE;
                uint32_t w = 0, h = 0;
                {
                    std::lock_guard lk(mu);
                    auto it = chains.find(q->subImage.swapchain);
                    if (it != chains.end()) img = acquired[q->subImage.swapchain], w = it->second.w, h = it->second.h;
                }
                const std::string file = std::string(dump_dir) + "/eye" + std::to_string(dumps) + "-9.ppm";
                fprintf(stderr, "[fake xr] dump %d quad: %s (%ux%u), pose x %.4f y %.4f z %.4f turn %.5f %.5f %.5f %.5f, %.4f x %.4f m, "
                        "imageRect %d,%d %dx%d\n", dumps, file.c_str(), w, h, q->pose.position.x, q->pose.position.y, q->pose.position.z,
                        q->pose.orientation.x, q->pose.orientation.y, q->pose.orientation.z, q->pose.orientation.w, q->size.width, q->size.height,
                        q->subImage.imageRect.offset.x, q->subImage.imageRect.offset.y, q->subImage.imageRect.extent.width,
                        q->subImage.imageRect.extent.height);
                dump_image(img, w, h, file);
            }
        }
    }
    if (!sess.said_at) sess.said_at = t;
    if (t - sess.said_at >= 10000000000ull) {
        fprintf(stderr, "[fake xr] %llu frames ended in the last 10 s, %llu layers\n",
                (unsigned long long)(sess.frames - sess.said_frames), (unsigned long long)(sess.layers - sess.said_layers));
        sess.said_at = t, sess.said_frames = sess.frames, sess.said_layers = sess.layers;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_LocateSpace(XrSpace space, XrSpace, XrTime time, XrSpaceLocation* loc) {
    SpaceKind k;
    {
        std::lock_guard lk(mu);
        auto it = spaces.find(space);
        k = it == spaces.end() ? kLocal : it->second;
    }
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    loc->pose = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
    if (k == kView) loc->pose = head_pose(time);
    if (k == kHandLeft || k == kHandRight) loc->pose.position = {k == kHandLeft ? -0.2f : 0.2f, 1.0f, -0.15f};
    // (FAKE_XR_HAND_R=<x>,<y>,<z>: the right hand there instead, local space, metres: held up in front of the eyes)
    static float hand_r[3];
    static const bool hand_set = [] {
        const char* e = getenv("FAKE_XR_HAND_R");
        return e && sscanf(e, "%f,%f,%f", &hand_r[0], &hand_r[1], &hand_r[2]) == 3;
    }();
    if (k == kHandRight && hand_set) loc->pose.position = {hand_r[0], hand_r[1], hand_r[2]};
    for (auto* n = (XrBaseOutStructure*)loc->next; n; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITY) ((XrSpaceVelocity*)n)->velocityFlags = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_LocateViews(XrSession, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* n, XrView* v) {
    *n = 2;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (!cap || !v) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    XrSpace in;
    bool room;
    {
        std::lock_guard lk(mu);
        in = li->space;
        auto it = spaces.find(in);
        room = it == spaces.end() || it->second != kView;
    }
    const XrPosef head = head_pose(li->displayTime);
    static double cants[2];
    static const bool canted = [] {
        const char* c = getenv("FAKE_XR_CANT");
        const int n = c ? sscanf(c, "%lf,%lf", &cants[0], &cants[1]) : 0;
        if (n == 1) cants[1] = cants[0];
        return n > 0;
    }();
    // (Hookshot VR) the eyes' FOV, so the game's camera is the headset's: the player's logs' "vr: fov asked 109.6 x 111.6 deg" is
    // the camera's, the eyes' widened by the camera's guard band (vr/headset/frame_loop.inc kCameraGuardBand, 1.03). Handed as
    // the eyes' FOV it was widened twice: the camera 113.2 degrees high, the two-eye pipelines' P00 0.685 where the headset's
    // is 0.705, so a headless warm-up built no two-eye pipeline the headset uses (pipeline-key-stability.md)
    static const float kGuard = 1.03f, half_x = std::atan(std::tan(109.6f * 0.5f / 57.2958f) / kGuard),
                       half_y = std::atan(std::tan(111.6f * 0.5f / 57.2958f) / kGuard);
    static float own[4];   // (FAKE_XR_FOV: the left eye's own, degrees)
    static const bool asymmetric = [] {
        const char* e = getenv("FAKE_XR_FOV");
        const bool ok = e && sscanf(e, "%f,%f,%f,%f", &own[0], &own[1], &own[2], &own[3]) == 4;
        if (ok) fprintf(stderr, "[fake xr] eyes' FOV: left %.1f %.1f %.1f %.1f deg, the right its mirror image\n", own[0], own[1], own[2], own[3]);
        return ok;
    }();
    for (int e = 0; e < 2; e++) {   // (in the view space: the eyes 63 mm apart)
        v[e].pose = XrPosef{{0, 0, 0, 1}, {e ? 0.0316f : -0.0316f, 0, 0}};
        v[e].fov = {-half_x, half_x, half_y, -half_y};
        if (asymmetric) {
            const float k = 1 / 57.2958f;
            v[e].fov = e ? XrFovf{-own[1] * k, -own[0] * k, own[2] * k, own[3] * k} : XrFovf{own[0] * k, own[1] * k, own[2] * k, own[3] * k};
        }
        const double cant = canted ? cants[e] * M_PI / 360.0 : 0.0;
        v[e].pose.orientation = {0, (float)(e ? -std::sin(cant) : std::sin(cant)), 0, (float)std::cos(cant)};
        if (room) v[e].pose = compose(head, v[e].pose);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_EnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap, uint32_t* n,
                                                     XrViewConfigurationView* v) {
    *n = 2;
    for (uint32_t e = 0; v && e < cap && e < 2; e++) {
        v[e].recommendedImageRectWidth = v[e].maxImageRectWidth = 2880;
        v[e].recommendedImageRectHeight = v[e].maxImageRectHeight = 2304;
        v[e].recommendedSwapchainSampleCount = v[e].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_EnumerateViewConfigurations(XrInstance, XrSystemId, uint32_t cap, uint32_t* n, XrViewConfigurationType* t) {
    *n = 1;
    if (cap && t) t[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_GetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType t, XrViewConfigurationProperties* p) {
    p->viewConfigurationType = t;
    p->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_EnumerateReferenceSpaces(XrSession, uint32_t cap, uint32_t* n, XrReferenceSpaceType* t) {
    *n = 2;
    if (cap >= 2 && t) t[0] = XR_REFERENCE_SPACE_TYPE_VIEW, t[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_StringToPath(XrInstance, const char* s, XrPath* p) {
    *p = path_of(s);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_PathToString(XrInstance, XrPath p, uint32_t cap, uint32_t* n, char* buf) {
    std::string s;
    {
        std::lock_guard lk(mu);
        if (p >= path_names.size()) return XR_ERROR_PATH_INVALID;
        s = path_names[p];
    }
    return list_out(s.c_str(), cap, n, buf);
}
XrResult XRAPI_CALL x_CreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out) {
    *out = make_handle<XrActionSet>();
    return XR_SUCCESS;
}
XrAction menu_action = XR_NULL_HANDLE;   // (FAKE_XR_MENU_AT: the left menu button)
XrResult XRAPI_CALL x_CreateAction(XrActionSet, const XrActionCreateInfo* ci, XrAction* out) {
    *out = make_handle<XrAction>();
    if (!strcmp(ci->actionName, "menu")) menu_action = *out;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_SuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding*) { return XR_SUCCESS; }
XrResult XRAPI_CALL x_AttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL x_SyncActions(XrSession, const XrActionsSyncInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL x_GetActionStateFloat(XrSession, const XrActionStateGetInfo*, XrActionStateFloat* st) {
    st->currentState = 0, st->changedSinceLastSync = XR_FALSE, st->lastChangeTime = 0, st->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_GetActionStateBoolean(XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* st) {
    // (FAKE_XR_MENU_AT=<s>[,<s>..]: the menu button held 0.9 s from each of those seconds of the session, opening and closing the VR menu)
    static const int64_t began = now_ns();
    bool held = false;
    if (const char* at = gi->action == menu_action ? getenv("FAKE_XR_MENU_AT") : nullptr)
        for (const char* p = at; *p;) {
            char* end = nullptr;
            const double from = strtod(p, &end), now = (double)(now_ns() - began) * 1e-9;
            if (end == p) break;
            held = held || (now >= from && now < from + 0.9);
            p = *end == ',' ? end + 1 : end;
        }
    st->currentState = held ? XR_TRUE : XR_FALSE, st->changedSinceLastSync = XR_FALSE, st->lastChangeTime = 0, st->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_GetActionStateVector2f(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f* st) {
    st->currentState = {0, 0}, st->changedSinceLastSync = XR_FALSE, st->lastChangeTime = 0, st->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_GetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* st) {
    st->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_CreateActionSpace(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
    *out = make_handle<XrSpace>();
    char name[XR_MAX_PATH_LENGTH] = "";
    uint32_t n = 0;
    x_PathToString(the_instance, ci->subactionPath, sizeof name, &n, name);
    std::lock_guard lk(mu);
    spaces[*out] = strstr(name, "right") ? kHandRight : kHandLeft;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL x_ApplyHapticFeedback(XrSession, const XrHapticActionInfo*, const XrHapticBaseHeader*) { return XR_SUCCESS; }
XrResult XRAPI_CALL x_GetCurrentInteractionProfile(XrSession, XrPath, XrInteractionProfileState* ps) {
    ps->interactionProfile = path_of("/interaction_profiles/oculus/touch_controller");
    return XR_SUCCESS;
}

struct Entry {
    const char* name;
    PFN_xrVoidFunction fn;
};
#define X(f) {"xr" #f, (PFN_xrVoidFunction)x_##f}
const Entry entries[] = {
    X(GetInstanceProperties), X(CreateInstance), X(DestroyInstance), X(GetSystem), X(GetVulkanInstanceExtensionsKHR),
    X(GetVulkanDeviceExtensionsKHR), X(GetVulkanGraphicsDeviceKHR), X(GetVulkanGraphicsRequirementsKHR), X(CreateSession),
    X(DestroySession), X(BeginSession), X(EndSession), X(PollEvent), X(CreateReferenceSpace), X(EnumerateSwapchainFormats),
    X(CreateSwapchain), X(DestroySwapchain), X(EnumerateSwapchainImages), X(AcquireSwapchainImage), X(WaitSwapchainImage),
    X(ReleaseSwapchainImage), X(WaitFrame), X(BeginFrame), X(EndFrame), X(LocateSpace), X(LocateViews),
    X(EnumerateViewConfigurationViews), X(EnumerateViewConfigurations), X(GetViewConfigurationProperties), X(EnumerateReferenceSpaces), X(StringToPath), X(PathToString), X(CreateActionSet), X(CreateAction),
    X(SuggestInteractionProfileBindings), X(AttachSessionActionSets), X(SyncActions), X(GetActionStateFloat),
    X(GetActionStateBoolean), X(GetActionStateVector2f), X(GetActionStatePose), X(CreateActionSpace), X(ApplyHapticFeedback),
    X(GetCurrentInteractionProfile),
};
#undef X

} // namespace

extern "C" __attribute__((visibility("default"))) XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char* name,
                                                                                           PFN_xrVoidFunction* fn) {
    for (const Entry& e : entries)
        if (!strcmp(e.name, name)) {
            *fn = e.fn;
            return XR_SUCCESS;
        }
    *fn = nullptr;
    fprintf(stderr, "[fake xr] not provided: %s\n", name);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
