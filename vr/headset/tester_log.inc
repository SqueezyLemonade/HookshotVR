// Hookshot VR: what a player's log must hold to settle a picture that looks wrong on a runtime we cannot run (v1.0.2): the
// options and switches that shape the picture, each change of them, the runtime's own offer, each swapchain, each quad as it
// is first shown, and the frames as sent beside the eyes as located. In every build, all bounded; each line has the run time
// in seconds ("[12.3 s]", run_s), as the log's lines have none. A path in a value is redacted by the log (host/log_redact.h).
//
// Included by vr/headset/vk_vr.cpp after flat_eyes.inc, before frame_loop.inc. Not compiled on its own.

// ---- the options that shape the picture --------------------------------------------------------------------------------

struct TesterOption {
    const char* name;
    int (*get)();
    const char* (*text)(int v);   // (null: the number)
};
const char* tester_view(int v) { return v == 2 ? "Window" : v == 1 ? "First person" : "Third person"; }
const char* tester_3d(int v) { return v == 0 ? "Mono" : v == 4 ? "Stereo" : "From depth"; }
const char* tester_preset(int v) { return v == kAutoPreset ? "Auto (this PC)" : v == 0 ? "Custom" : nullptr; }
const char* tester_render(int v) {
    static char b[24];
    if (!upscaler_rows::percent(v)) return "Auto (dynamic)";
    snprintf(b, sizeof b, "%d%%", upscaler_rows::percent(v));
    return b;
}
const char* tester_upscaler(int v) {
    static const char* const t[] = {"Game's own", "DLSS", "DLAA", "FSR 3.1", "FSR 4", "FSR 4 native AA", "Off", "XeSS", "XeSS native AA"};
    static_assert(sizeof t / sizeof t[0] == upscaler_rows::kUpscalers);
    return v >= 0 && v < upscaler_rows::kUpscalers ? t[v] : nullptr;
}
const TesterOption tester_options[] = {
    {"View", [] { return opts.vr_mode == 1 ? 1 : opts.vr_window ? 2 : 0; }, tester_view},
    {"3D", [] { return opts.vr_stereo; }, tester_3d},
    {"3D strength %", [] { return opts.vr_depth_scale; }, nullptr},
    {"Near depth", [] { return opts.vr_near_depth; }, nullptr},
    {"Quality preset", [] { return opts.preset; }, tester_preset},
    {"Render resolution", [] { return opts.resolution; }, tester_render},
    {"Upscaler", [] { return opts.dlss; }, tester_upscaler},
    {"Output resolution (choice)", [] { return opts.output_res; }, nullptr},
    {"Eye resolution (height)", [] { return opts.vr_eye_height; }, nullptr},
    {"Screen size deg", [] { return opts.vr_cutscene_screen; }, nullptr},
    {"Eye height cm", [] { return opts.vr_height; }, nullptr},
    {"HUD size %", [] { return opts.vr_hud_size; }, nullptr},
    {"Wrist HUD", [] { return opts.vr_wrist_hud; }, nullptr},
    {"VR menu follows head", [] { return opts.vr_menu_position; }, nullptr},
    {"Cutscene view", [] { return opts.vr_events; }, nullptr},
    {"Cinematic in 3D", [] { return opts.vr_cinematic_view; }, nullptr},
    {"Window background", [] { return opts.vr_window_room; }, nullptr},
    {"Vignette (move+turn+air+carried)", [] { return opts.vr_vignette_move + opts.vr_vignette_turn + opts.vr_vignette_air + opts.vr_vignette_carried; }, nullptr},
    {"Frame generation", [] { return opts.framegen; }, nullptr},
    {"Frame rate cap", [] { return opts.fps_cap; }, nullptr},
    {"V-Sync", [] { return opts.vsync; }, nullptr},
    {"HDR", [] { return opts.hdr; }, nullptr},
    {"Handheld", [] { return opts.handheld; }, nullptr},
    {"Aspect ratio (choice)", [] { return opts.ultrawide; }, nullptr},
    {"Recenter (count)", [] { return opts.vr_recenter; }, nullptr},
};
constexpr int kTesterOptions = sizeof tester_options / sizeof tester_options[0];
struct TesterLog {
    bool started = false;
    int option[kTesterOptions]{};
    int changes = 0;                 // (change lines so far)
    double changed_at = -1;          // an option or the layers' way changed then, the frame as it settled still to be logged
    const char* why = "";
    int kind = -1, way = -1;         // the last frame's: kTesterFlat.., and how its layers were made
    bool panel_was = false;
    double frame_at = -1e9;          // the last frame logged
    int frames = 0;
    uint32_t quads_was = 0;          // the named quads of the last frame
    int quad_told[8]{};
} tester;

void tester_option_text(const TesterOption& o, int v, char* out, size_t n) {
    const char* t = o.text ? o.text(v) : nullptr;
    if (t) snprintf(out, n, "%s (%d)", t, v);
    else snprintf(out, n, "%d", v);
}

/// At the session's first frame: the options and the switches set (the first call); afterwards a line for each option changed.
/// True: one changed now
bool tester_options_frame() {
    char a[64], b[64];
    if (!tester.started) {
        tester.started = true;
        logf("vr: [%.1f s] picture options at the start:", run_s());
        std::string line;
        for (int i = 0; i < kTesterOptions; i++) {
            tester_option_text(tester_options[i], tester.option[i] = tester_options[i].get(), a, sizeof a);
            line += std::string(line.empty() ? "" : "; ") + tester_options[i].name + " " + a;
            if (line.size() > 180 || i == kTesterOptions - 1) logf("vr:   %s", line.c_str()), line.clear();
        }
        int set = 0;
        for (const switches::Row& r : switches::rows) {
            if (std::strncmp(r.name, "RX", 2) || std::strncmp(r.name + 2, "_VR_", 4)) continue;
            const char* e = getenv(r.name);
            if (e && *e) set++, logf("vr:   switch set: %s=%.160s", r.name, e);
        }
        if (!set) logf("vr:   no RX_VR_ switch set");
        logf("vr:   layers: flat frames %s, shared picture's poses %s, layer FOV %s; the game's HUD is drawn in its picture, not a layer",
             flat_mode_name(), mono_pose_eyes() ? "eyes" : "head", layer_fov_located() ? "located" : "symmetric");
        return false;
    }
    bool changed = false;
    for (int i = 0; i < kTesterOptions; i++) {
        const int v = tester_options[i].get();
        if (v == tester.option[i]) continue;
        tester_option_text(tester_options[i], tester.option[i], a, sizeof a);
        tester_option_text(tester_options[i], v, b, sizeof b);
        tester.option[i] = v, changed = true;
        if (tester.changes++ < 300) logf("vr: [%.1f s] option %s: %s -> %s", run_s(), tester_options[i].name, a, b);
    }
    return changed;
}

// ---- the runtime's offer ------------------------------------------------------------------------------------------------

/// (the instance made) the extensions enabled, and which of the ones that matter to the picture the runtime lists
void tester_extensions(const XrInstanceCreateInfo& ci) {
    std::string on;
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) on += std::string(i ? ", " : "") + ci.enabledExtensionNames[i];
    logf("vr: [%.1f s] OpenXR %u.%u asked, %u extension(s) enabled: %s", run_s(), (unsigned)XR_VERSION_MAJOR(ci.applicationInfo.apiVersion),
         (unsigned)XR_VERSION_MINOR(ci.applicationInfo.apiVersion), ci.enabledExtensionCount, on.c_str());
    PFN_xrEnumerateInstanceExtensionProperties list = nullptr;
    uint32_t n = 0;
    if (XR_FAILED(xr_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&list)) || !list ||
        XR_FAILED(list(nullptr, 0, &n, nullptr)))
        return;
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(list(nullptr, n, &n, props.data()))) return;
    std::string offered;
    for (const char* want : {"XR_EXT_user_presence", "XR_FB_display_refresh_rate", "XR_KHR_composition_layer_depth", "XR_FB_composition_layer_settings",
                             "XR_EXT_local_floor", "XR_KHR_visibility_mask", "XR_FB_space_warp", "XR_FB_foveation", "XR_META_recommended_layer_resolution"})
        for (const XrExtensionProperties& p : props)
            if (!std::strcmp(p.extensionName, want)) offered += std::string(offered.empty() ? "" : ", ") + want;
    logf("vr:   offered, not used: %s (user presence is not asked of the runtime: the session states say it)", offered.empty() ? "none of those looked for" : offered.c_str());
}

PFN_xrCreateSwapchain tester_create_swapchain = nullptr;   // (the runtime's own; xrCreateSwapchain is tester_swapchain from device_created on)
XRAPI_ATTR XrResult XRAPI_CALL tester_swapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    const XrResult r = tester_create_swapchain(session, ci, out);
    static std::atomic<int> told{0};
    if (told++ < 60)
        logf("vr: [%.1f s] swapchain %p: format %lld (%s), %ux%u, %u array, %u mip(s), %u face(s), %u sample(s), usage %#llx, flags %#llx: %d", run_s(),
             XR_SUCCEEDED(r) ? (void*)*out : nullptr, (long long)ci->format, ci->format == VK_FORMAT_R8G8B8A8_SRGB ? "RGBA8 sRGB" :
             ci->format == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8 UNORM" : "other", ci->width, ci->height, ci->arraySize, ci->mipCount, ci->faceCount,
             ci->sampleCount, (unsigned long long)ci->usageFlags, (unsigned long long)ci->createFlags, (int)r);
    return r;
}

/// (the session made) the view configuration, the reference spaces and the swapchain formats
void tester_session_report(const std::vector<int64_t>& formats) {
    PFN_xrGetViewConfigurationProperties props = nullptr;
    PFN_xrEnumerateViewConfigurations configs = nullptr;
    PFN_xrEnumerateReferenceSpaces spaces = nullptr;
    const auto get = [&](const char* name, auto* fn) {
        if (XR_FAILED(xr_proc(s.inst, name, (PFN_xrVoidFunction*)fn))) *fn = nullptr;
    };
    get("xrGetViewConfigurationProperties", &props), get("xrEnumerateViewConfigurations", &configs), get("xrEnumerateReferenceSpaces", &spaces);
    std::string list;
    uint32_t n = 0;
    XrViewConfigurationType types[8];
    if (configs && XR_SUCCEEDED(configs(s.inst, s.system, 8, &n, types)))
        for (uint32_t i = 0; i < n && i < 8; i++) list += " " + std::to_string((int)types[i]);
    XrViewConfigurationProperties vp{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
    const bool have = props && XR_SUCCEEDED(props(s.inst, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &vp));
    logf("vr: [%.1f s] view configurations%s; primary stereo used, its FOV %s", run_s(), list.empty() ? " ?" : list.c_str(),
         !have ? "mutable: not said" : vp.fovMutable ? "mutable (a layer may have another FOV)" : "NOT mutable (a layer must keep the located FOV)");
    XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(s.inst, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)))
        for (uint32_t e = 0; e < n && e < 2; e++)
            logf("vr:   view %u: recommended %ux%u (%u samples), at most %ux%u (%u samples)", e, v[e].recommendedImageRectWidth,
                 v[e].recommendedImageRectHeight, v[e].recommendedSwapchainSampleCount, v[e].maxImageRectWidth, v[e].maxImageRectHeight,
                 v[e].maxSwapchainSampleCount);
    list.clear();
    XrReferenceSpaceType rt[16];
    if (spaces && XR_SUCCEEDED(spaces(s.session, 16, &n, rt)))
        for (uint32_t i = 0; i < n && i < 16; i++) list += " " + std::to_string((int)rt[i]);
    std::string f;
    for (size_t i = 0; i < formats.size() && i < 24; i++) f += " " + std::to_string((long long)formats[i]);
    logf("vr:   reference spaces offered%s (1 VIEW, 2 LOCAL, 3 STAGE); used: LOCAL for the layers, VIEW for the head; swapchain formats%s",
         list.empty() ? " ?" : list.c_str(), f.c_str());
}

// ---- the frames ---------------------------------------------------------------------------------------------------------

enum TesterKind { kTesterNone, kTesterFlat, kTesterPlay, kTesterWindow };
struct TesterQuad {
    const char* name;
    const XrCompositionLayerQuad* quad;
};

/// How old the drawn pose is, in milliseconds of display time (the display time of this frame less the one the pose was
/// located for); negative: not a pose of ours
double tester_pose_age(const XrPosef& drawn, XrTime display) {
    std::lock_guard lk(s.pose_mu);
    const Pose* found = nullptr;
    for (const Pose& p : s.poses)
        if (p.seq && p.time && !std::memcmp(&p.pose, &drawn, sizeof drawn) && (!found || p.seq > found->seq)) found = &p;
    return found ? (double)(display - found->time) * 1e-6 : -1;
}

/// After a frame's end: a quad first shown (each time, a few times each), and the frame as sent beside the eyes as located,
/// when the kind of frame or its layers change, a second after an option changed, as the VR menu opens, and every 30 s
void tester_frame(const XrCompositionLayerBaseHeader* const* layers, uint32_t n_layers, const TesterQuad* quads, int n_quads,
                  const XrFrameState& fs, const XrPosef& head, const XrView* located, XrViewStateFlags flags, XrResult ended, bool flat_told) {
    const bool option_changed = tester_options_frame();
    const double now = run_s();
    uint32_t shown = 0;
    for (int q = 0; q < n_quads && q < 8; q++)
        for (uint32_t i = 0; i < n_layers; i++)
            if (layers[i] == (const XrCompositionLayerBaseHeader*)quads[q].quad) shown |= 1u << q;
    for (int q = 0; q < n_quads && q < 8; q++) {
        if (!(shown & ~tester.quads_was & (1u << q)) || tester.quad_told[q]++ >= 6) continue;
        const XrCompositionLayerQuad& l = *quads[q].quad;
        const bool view = l.space == s.view;
        float from[3] = {l.pose.position.x, l.pose.position.y, l.pose.position.z};
        if (!view) {
            const float d[3] = {from[0] - head.position.x, from[1] - head.position.y, from[2] - head.position.z};
            qrot(qinv(head.orientation), d, from);
        }
        logf("vr: [%.1f s] quad shown: %s; %s space; at %.3f %.3f %.3f turn %.4f %.4f %.4f %.4f; %.3f %.3f %.3f from the head; %.3f x %.3f m; "
             "eyes %s; swapchain %p rect %d,%d %dx%d; flags %#llx; frame end %d", now, quads[q].name, view ? "VIEW" : "LOCAL", l.pose.position.x,
             l.pose.position.y, l.pose.position.z, l.pose.orientation.x, l.pose.orientation.y, l.pose.orientation.z, l.pose.orientation.w, from[0],
             from[1], from[2], l.size.width, l.size.height, l.eyeVisibility == XR_EYE_VISIBILITY_BOTH ? "both" : l.eyeVisibility ==
             XR_EYE_VISIBILITY_LEFT ? "left" : "right", (void*)l.subImage.swapchain, l.subImage.imageRect.offset.x, l.subImage.imageRect.offset.y,
             l.subImage.imageRect.extent.width, l.subImage.imageRect.extent.height, (unsigned long long)l.layerFlags, (int)ended);
    }
    tester.quads_was = shown;
    const bool panel_now = panel_open(), panel_opened = panel_now && !tester.panel_was;
    tester.panel_was = panel_now;
    const XrCompositionLayerProjection* sent = nullptr;
    for (uint32_t i = 0; i < n_layers && !sent; i++)
        if (layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) sent = (const XrCompositionLayerProjection*)layers[i];
    const bool blank = n_quads > 0 && (shown >> (n_quads - 1) & 1);   // (the last named one: nothing else is sent then)
    const int kind = !n_layers || blank ? kTesterNone : sent && window_view() ? kTesterWindow : flat_screen.shown ? kTesterFlat : sent ? kTesterPlay : kTesterNone;
    const int way = kind != kTesterPlay ? 0 : 1 + (shared_pose.shown ? 1 : 0) + (shared_pose.own ? 2 : 0) + (shared_pose.turned ? 4 : 0);
    const char* why = nullptr;
    if (kind != kTesterNone && kind != tester.kind) why = flat_told ? nullptr : kind == kTesterPlay ? "gameplay entered" : kind == kTesterFlat ? "a flat frame" : "the Window view";
    else if (way != tester.way && kind == kTesterPlay) tester.changed_at = now, tester.why = "the layer's way changed";
    if (kind != kTesterNone) tester.kind = kind, tester.way = way;
    if (flat_told) tester.frame_at = now;   // (that flat frame was just told: frame_loop.inc)
    if (option_changed) tester.changed_at = now, tester.why = "an option changed";
    if (!why && tester.changed_at >= 0 && now - tester.changed_at >= 1) why = tester.why;   // (a second on: what it settled to)
    if (why) tester.changed_at = -1;
    if (panel_opened) why = "the VR menu opened";
    if (!why && kind != kTesterNone && now - tester.frame_at >= 30) why = "every 30 s";
    if (!why || kind == kTesterNone || tester.frames >= 400) return;
    // (a kind that flips back and forth: one frame a second at most, but always what was asked for by a change or the menu)
    if (now - tester.frame_at < 1 && why != tester.why && !panel_opened) return;
    tester.frame_at = now, tester.frames++;
    uint32_t sw, sh;
    {
        std::lock_guard lk(s.mu);
        sw = s.scene_w, sh = s.scene_h;
    }
    logf("vr: [%.1f s] frame sent (%s): %s; View %s, 3D %s %d%%; %u layer(s); display time %lld ns, period %.2f ms (%.1f Hz); frame end %d", now, why,
         kind == kTesterFlat ? "flat" : kind == kTesterWindow ? "Window view" : "gameplay", tester_view(tester_options[0].get()), tester_3d(opts.vr_stereo),
         opts.vr_depth_scale, n_layers, (long long)fs.predictedDisplayTime, (double)fs.predictedDisplayPeriod * 1e-6,
         fs.predictedDisplayPeriod > 0 ? 1e9 / (double)fs.predictedDisplayPeriod : 0.0, (int)ended);
    if (kind == kTesterPlay) {
        const XrPosef& d = shared_pose.drawn;
        char age[40] = "?";
        if (const double ms = tester_pose_age(d, fs.predictedDisplayTime); ms >= 0) snprintf(age, sizeof age, "%.1f ms", ms);
        logf("vr:   picture: %s%s; drawn for the head at %.4f %.4f %.4f turn %.5f %.5f %.5f %.5f, %s old; %ux%u, the scene %ux%u", shared_pose.shown ?
             "one for both eyes" : shared_pose.eye2 ? "each eye its own" : "one, moved by its depth", !shared_pose.shown ? "" : shared_pose.turned ?
             ", at each eye's own pose and turn (resampled)" : shared_pose.own ? ", at each eye's own pose" : ", at the drawn head for both", d.position.x,
             d.position.y, d.position.z, d.orientation.x, d.orientation.y, d.orientation.z, d.orientation.w, age, shared_pose.w, shared_pose.h, sw, sh);
    } else if (kind == kTesterFlat) {
        const XrPosef& p = flat_screen.pose;
        logf("vr:   flat mode %s; screen at %.4f %.4f %.4f turn %.5f %.5f %.5f %.5f, %.3f x %.3f m, picture %ux%u", flat_eyes.failed ?
             "projection (eyes could not be made)" : flat_mode_name(), p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y,
             p.orientation.z, p.orientation.w, flat_screen.width, flat_screen.height, flat_screen.w, flat_screen.h);
    }
    views_log(sent, head, located, flags);
}
