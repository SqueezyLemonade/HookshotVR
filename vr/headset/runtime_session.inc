// Hookshot VR: Runtime/device negotiation and creation of the OpenXR session.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- the back end's calls ---------------------------------------------------------------------------------------------------

bool wanted() { return s.wanted; }

int option_at_start(const char* key, int def) {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string ini(exe, n);
    ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "hookshot_options.ini";
    const size_t klen = std::strlen(key);
    int v = def;
    if (FILE* f = fopen(ini.c_str(), "r")) {
        char line[80];
        while (fgets(line, sizeof line, f))
            if (!std::strncmp(line, key, klen) && line[klen] == '=') v = atoi(line + klen + 1);
        fclose(f);
    }
    return v;
}

void instance_extensions(std::vector<const char*>& exts) {
    opts.vr = asked() ? 1 : 0;   // (the game side's VR switches follow it all run: vk_replay.cpp's options. No menu row sets it)
    if (!asked()) return;
    if (!load_loader()) {
#ifdef _WIN32   // (Windows has no system loader: it is shipped beside hookshot.exe)
        logf("vr: VR is on, but there is no OpenXR loader (openxr_loader.dll beside hookshot.exe): VR off");
#else
        logf("vr: VR is on, but there is no OpenXR loader (libopenxr_loader.so.1, the system's openxr-loader package): VR off");
#endif
        return;
    }
    std::vector<const char*> xr_exts = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    window_instance_extensions(xr_exts);   // (the Window view's room, where offered: vr/headset/window_view.inc)
    controller_instance_extensions(xr_exts);   // (the controller profiles from extensions: vr/input/openxr_controllers.inc)
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "Hookshot VR");
    ci.applicationInfo.applicationVersion = 1;
    std::strcpy(ci.applicationInfo.engineName, "Hookshot");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = (uint32_t)xr_exts.size();
    ci.enabledExtensionNames = xr_exts.data();
    XrResult r = xr_create_instance(&ci, &s.inst);
    if (XR_FAILED(r) && xr_exts.size() > 1) {   // (refused with the optional ones: again without them)
        logf("vr: the OpenXR runtime refused its own optional extensions (%d): started again without them", (int)r);
        window.fb_listed = false;
        for (ControllerExt& e : ctl_ext) e.on = false;
        ci.enabledExtensionCount = 1;
        r = xr_create_instance(&ci, &s.inst);
    }
    if (XR_FAILED(r)) {
        logf("vr: VR is on, but the OpenXR runtime did not start (%d): check your active OpenXR runtime; VR off this run", (int)r);
        return;
    }
    tester_extensions(ci);
    if (!load_functions(s.inst)) {
        logf("vr: the OpenXR runtime lacks functions VR needs: VR off");
        return;
    }
    window_load_functions(s.inst);   // (optional, each)
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(s.inst, &ip))) logf("vr: OpenXR runtime %s", ip.runtimeName);
    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(s.inst, &sg, &s.system))) {
        logf("vr: no headset (connect your headset to your OpenXR runtime before starting the game): VR off this run");
        return;
    }
    {   // (once, for a player's log: the runtime, its version and the headset)
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        const bool have_ip = XR_SUCCEEDED(xrGetInstanceProperties(s.inst, &ip));
        const bool have_sp = window_xrGetSystemProperties && XR_SUCCEEDED(window_xrGetSystemProperties(s.inst, s.system, &sp));
        logf("vr: runtime %s %u.%u.%u, system \"%s\" (vendor %#x), max swapchain %ux%u, %u layers", have_ip ? ip.runtimeName : "?",
             have_ip ? (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion) : 0u, have_ip ? (unsigned)XR_VERSION_MINOR(ip.runtimeVersion) : 0u,
             have_ip ? (unsigned)XR_VERSION_PATCH(ip.runtimeVersion) : 0u, have_sp ? sp.systemName : "?", have_sp ? sp.vendorId : 0u,
             have_sp ? sp.graphicsProperties.maxSwapchainImageWidth : 0u, have_sp ? sp.graphicsProperties.maxSwapchainImageHeight : 0u,
             have_sp ? sp.graphicsProperties.maxLayerCount : 0u);
    }
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    xrGetVulkanGraphicsRequirementsKHR(s.inst, s.system, &req);
    uint32_t n = 0;
    xrGetVulkanInstanceExtensionsKHR(s.inst, s.system, 0, &n, nullptr);
    std::string list(n, '\0');
    xrGetVulkanInstanceExtensionsKHR(s.inst, s.system, n, &n, list.data());
    for (size_t at = 0; at < list.size();) {
        size_t end = list.find_first_of(" \0", at, 2);
        if (end == std::string::npos) end = list.size();
        if (end > at) s.inst_exts.emplace_back(list.substr(at, end - at));
        at = end + 1;
    }
    for (const std::string& e : s.inst_exts) {
        bool have = false;
        for (const char* x : exts) have = have || e == x;
        if (!have) exts.push_back(e.c_str());
    }
    s.wanted = true;
    logf("vr: VR on: %zu instance extension(s) for the runtime", s.inst_exts.size());
}

bool device_matches(VkPhysicalDevice physical) {
    if (!s.wanted) return false;
    VkPhysicalDevice xr_device = VK_NULL_HANDLE;
    if (XR_FAILED(xrGetVulkanGraphicsDeviceKHR(s.inst, s.system, dev.instance, &xr_device)) || xr_device != physical) {
        logf("vr: the headset's runtime drives another GPU than the game picked: VR off this run");
        s.wanted = false;
        return false;
    }
    s.matched = true;
    return true;
}

void device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& have) {
    if (!s.matched) return;
    uint32_t n = 0;
    xrGetVulkanDeviceExtensionsKHR(s.inst, s.system, 0, &n, nullptr);
    std::string list(n, '\0');
    xrGetVulkanDeviceExtensionsKHR(s.inst, s.system, n, &n, list.data());
    for (size_t at = 0; at < list.size();) {
        size_t end = list.find_first_of(" \0", at, 2);
        if (end == std::string::npos) end = list.size();
        if (end > at) s.dev_exts.emplace_back(list.substr(at, end - at));
        at = end + 1;
    }
    for (const std::string& e : s.dev_exts) {
        bool there = false, listed = false;
        for (const auto& p : have) there = there || e == p.extensionName;
        for (const char* x : want) listed = listed || e == x;
        if (!there) logf("vr: the GPU lacks %s, which the runtime asks for", e.c_str());
        else if (!listed) want.push_back(e.c_str());
    }
}

uint32_t queue_count(uint32_t family_queues) {
    if (!s.matched) return 1;
    if (family_queues < 2) {
        logf("vr: the GPU's graphics family has one queue: VR off");
        s.matched = s.wanted = false;
        return 1;
    }
    return 2;
}

void device_created() {
    eyes_device_created();   // (true stereo: the stages' SPIR-V kept from the first module on, headset or not)
    if (!s.matched) return;
    s.family = dev.queue_family;
    vkGetDeviceQueue(dev.device, s.family, 1, &s.queue);
    if (!s.queue || !make_pipelines()) {
        logf("vr: the headset's pipelines could not be made: VR off");
        s.wanted = s.matched = false;
        return;
    }
    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = dev.instance;
    gb.physicalDevice = dev.physical;
    gb.device = dev.device;
    gb.queueFamilyIndex = s.family;
    gb.queueIndex = 1;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = s.system;
    if (XR_FAILED(xrCreateSession(s.inst, &sci, &s.session))) {
        logf("vr: the headset's session could not start: VR off");
        s.wanted = s.matched = false;
        return;
    }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xrCreateReferenceSpace(s.session, &rs, &s.local);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(s.session, &rs, &s.view);
    tester_create_swapchain = xrCreateSwapchain, xrCreateSwapchain = tester_swapchain;   // (each swapchain logged)
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(s.session, 0, &nf, nullptr);
    std::vector<int64_t> formats(nf);
    xrEnumerateSwapchainFormats(s.session, nf, &nf, formats.data());
    // The game's final present image contains display-encoded sRGB values.  Advertise that to OpenXR so the
    // compositor does not treat those values as linear light and apply another transfer curve (a washed-out image).
    for (int64_t f : formats)
        if (f == VK_FORMAT_R8G8B8A8_SRGB) swap_format = f;
    if (!swap_format)
        for (int64_t f : formats)
            if (f == VK_FORMAT_R8G8B8A8_UNORM) swap_format = f;
    if (!swap_format) {
        logf("vr: the runtime offers no RGBA8 swapchain: VR off");
        s.wanted = s.matched = false;
        return;
    }
    logf("vr: OpenXR color swapchain is %s", swap_format == VK_FORMAT_R8G8B8A8_SRGB ? "RGBA8 sRGB" : "RGBA8 UNORM fallback");
    tester_session_report(formats);
    window_runtime_report();   // (what the runtime offers the Window view's room)
    make_controls();   // (OoT VR: the hands; before the first frame, as attaching must be)
    s.running = true;
    s.thread = std::thread(headset_thread);
    s.thread.detach();
    logf("vr: the headset's session is made (queue %u.1); put the headset on", s.family);
}

bool active() { return s.running && s.session_running; }

/// The game quits: the session ended (asked of the runtime, frames sent until it has, kCloseEndNs at most), then the session
/// and the instance destroyed, on the headset thread. Waited for kCloseWaitMs at most: a runtime that hangs keeps the game
/// from closing no longer than that
void close() {
    constexpr int kCloseWaitMs = 2000;
    if (!s.inst) return;   // (no VR this run)
    int idle = 0;
    if (!close_step.compare_exchange_strong(idle, 1)) return;   // (asked before)
    if (!s.running) std::thread(headset_destroy).detach();   // (an instance, no session thread: the instance alone)
    const auto t0 = std::chrono::steady_clock::now();
    const auto waited = [&] {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    while (close_step.load(std::memory_order_acquire) != 2 && waited() < kCloseWaitMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (close_step.load(std::memory_order_acquire) == 2) logf("vr: closed in %d ms", waited());
    else logf("vr: the OpenXR runtime did not close within %d ms: the game closes anyway", kCloseWaitMs);
}
std::mutex& queue_mutex() { return s.queue_mu; }
