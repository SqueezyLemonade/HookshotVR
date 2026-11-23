// Hookshot VR: Settings at startup and the dynamically loaded OpenXR API.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- settings ----------------------------------------------------------------------------------------------------------------

/// vr= in hookshot_options.ini (the menu's VR row; the menu reads its file after the instance is made) or HOOKSHOT_VR
bool asked() {
    if (const char* e = getenv("HOOKSHOT_VR")) return atoi(e) != 0;
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string ini(exe, n);
    ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "hookshot_options.ini";
    int v = 0;
    if (FILE* f = fopen(ini.c_str(), "r")) {
        char line[80];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "vr=", 3)) v = atoi(line + 3);
        fclose(f);
    }
    return v != 0;
}

// ---- OpenXR, loaded at run time --------------------------------------------------------------------------------------------

#define XRX(X)                                                                                                                 \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetVulkanInstanceExtensionsKHR)                          \
    X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) X(xrGetVulkanGraphicsRequirementsKHR)                     \
    X(xrCreateSession) X(xrDestroySession) X(xrCreateReferenceSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)        \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage)                       \
    X(xrReleaseSwapchainImage) X(xrPollEvent) X(xrBeginSession) X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)     \
    X(xrLocateSpace) X(xrLocateViews) X(xrEnumerateViewConfigurationViews)                                                     \
    X(xrStringToPath) X(xrPathToString) X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings)            \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateFloat) X(xrGetActionStateBoolean)                            \
    X(xrGetActionStateVector2f) X(xrGetActionStatePose) X(xrCreateActionSpace) X(xrApplyHapticFeedback)                         \
    X(xrGetCurrentInteractionProfile)
#define XRX_PTR(f) PFN_##f f = nullptr;
XRX(XRX_PTR)
#undef XRX_PTR
PFN_xrGetInstanceProcAddr xr_proc = nullptr;
PFN_xrCreateInstance xr_create_instance = nullptr;
PFN_xrRequestExitSession xr_request_exit = nullptr;   // (optional: vr::close asks the session's end with it, where there is one)

bool load_loader() {
#ifdef _WIN32
    HMODULE m = LoadLibraryA("openxr_loader.dll");
    if (!m) return false;
    xr_proc = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(m, "xrGetInstanceProcAddr");
#else
    void* m = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!m) m = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!m) return false;
    xr_proc = (PFN_xrGetInstanceProcAddr)dlsym(m, "xrGetInstanceProcAddr");
#endif
    if (!xr_proc) return false;
    return XR_SUCCEEDED(xr_proc(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xr_create_instance)) &&
           xr_create_instance;
}

bool load_functions(XrInstance inst) {
    bool ok = true;
#define XRX_LOAD(f) ok = ok && XR_SUCCEEDED(xr_proc(inst, #f, (PFN_xrVoidFunction*)&f)) && f;
    XRX(XRX_LOAD)
#undef XRX_LOAD
    if (ok && (XR_FAILED(xr_proc(inst, "xrRequestExitSession", (PFN_xrVoidFunction*)&xr_request_exit)) || !xr_request_exit))
        xr_request_exit = nullptr;   // (a runtime without it: closed without asking, vr::close)
    return ok;
}
