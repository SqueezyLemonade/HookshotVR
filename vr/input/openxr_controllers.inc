// Hookshot VR: Raw OpenXR controller actions, bindings, tracking and haptics.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- OpenXR controllers: tracking, buttons and haptics ----------------------------------------------------------------------
// One action set: each hand's grip and aim poses, trigger, grip (squeeze), thumbstick, the two face buttons (X/A, Y/B), the
// stick's click, the menu button (left) and its haptics. Bindings are suggested for every common controller's interaction
// profile (Quest Touch and its Pro and Plus, Valve Index, the HTC Vive wand, Cosmos and Focus 3, Windows Mixed Reality and the
// HP Reverb G2's, Pico 4, and the OpenXR simple controller); the runtime binds the one matching the connected controller,
// and the one it picked is logged (note_profiles). A profile from an extension is suggested only where the runtime lists
// that extension. Tested: Quest Touch; others untested (their layouts from the makers' own button maps, not tried in a
// headset). The poses and velocities are located in local space every headset frame;
// hookshot_vr_hands hands them to the host in the same recentred frame as the head.

struct HandState {
    bool valid = false;            // its grip pose is tracked
    XrPosef grip{{0, 0, 0, 1}, {0, 0, 0}}, aim{{0, 0, 0, 1}, {0, 0, 0}};   // local space
    float lin_vel[3]{}, ang_vel[3]{};   // the grip's, local space (m/s, rad/s)
    float trigger = 0, squeeze = 0, stick[2]{};
    uint32_t buttons = 0;          // HOOKSHOT_VR_BTN_*
};
std::atomic<int64_t> bow_active_at{0};
std::atomic<bool> bow_is_sling{false};   // (the bow out is the slingshot: host_api.inc hookshot_vr_bow_active 2)
std::atomic<int64_t> hook_active_at{0};   // (the hookshot in a VR hand: host_api.inc hookshot_vr_hook_active)
std::atomic<int> hook_hand_now{-1};       // (and which)
std::atomic<int64_t> hook_seen_at{0};     // (and when it was last out, held or being put away)
std::atomic<int> bow_hand_now{-1};   // the hand that took the bow this time it came out (-1: none yet: the left)
std::atomic<bool> bow_art_ready{false};
struct Controls {
    bool made = false, attached = false;
    XrActionSet set = XR_NULL_HANDLE;
    XrAction grip = XR_NULL_HANDLE, aim = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE,
             stick = XR_NULL_HANDLE, lower = XR_NULL_HANDLE, upper = XR_NULL_HANDLE, stick_click = XR_NULL_HANDLE,
             menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    XrPath hand[2]{};
    XrSpace grip_space[2]{}, aim_space[2]{};
    HandState now[2];              // (the headset thread; copied under pose_mu for the host)
    HandState shared[2];
    uint32_t seq = 0;
    std::string profile[2];
    std::string menu_name = "menu", upper_name = "Y";   // (pose_mu: name_menu_buttons; empty: not on this controller)
    bool pads = false;             // (pose_mu: its sticks are trackpads)
    Eye marker[4];                 // blue/red controllers, white bow reticle, green palm socket
    bool marker_ready[4]{};
} ctl;

/// The controller profiles that come from an extension: each enabled at the instance's creation where the runtime lists it
struct ControllerExt {
    const char* name;
    bool on;   // listed by the runtime and enabled
};
ControllerExt ctl_ext[] = {
    {XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME, false},   // htc/vive_cosmos_controller
    {XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME, false},   // htc/vive_focus3_controller
    {XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME, false},          // hp/mixed_reality_controller
    {XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, false},                // bytedance/pico4_controller
    {XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME, false},                  // facebook/touch_controller_pro
    {XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME, false},               // meta/touch_controller_plus
};

/// (the instance's creation) each controller extension the runtime lists, enabled; one it lacks is left out (its controller
/// falls to another profile the runtime maps it to)
void controller_instance_extensions(std::vector<const char*>& exts) {
    PFN_xrEnumerateInstanceExtensionProperties list = nullptr;
    uint32_t n = 0;
    if (!xr_proc || XR_FAILED(xr_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&list)) ||
        !list || XR_FAILED(list(nullptr, 0, &n, nullptr)))
        return;
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(list(nullptr, n, &n, props.data()))) return;
    std::string said;
    for (ControllerExt& e : ctl_ext) {
        for (const XrExtensionProperties& p : props) e.on = e.on || !std::strcmp(p.extensionName, e.name);
        if (e.on) exts.push_back(e.name), said += said.empty() ? e.name : std::string(", ") + e.name;
    }
    logf("vr: controllers: extension profiles enabled: %s", said.empty() ? "none listed" : said.c_str());
}

XrPath xpath(const char* p) {
    XrPath out = XR_NULL_PATH;
    xrStringToPath(s.inst, p, &out);
    return out;
}

XrAction make_action(const char* name, const char* label, XrActionType type) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(ci.actionName, name, sizeof ci.actionName - 1);
    std::strncpy(ci.localizedActionName, label, sizeof ci.localizedActionName - 1);
    ci.actionType = type;
    ci.countSubactionPaths = 2;
    ci.subactionPaths = ctl.hand;
    XrAction a = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(ctl.set, &ci, &a))) logf("vr: controller action %s could not be made", name);
    return a;
}

/// Before the session's first frame: the action set, its bindings, its spaces, attached to the session
void make_controls() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "hookshot_hands");
    std::strcpy(si.localizedActionSetName, "Hookshot hands");
    if (XR_FAILED(xrCreateActionSet(s.inst, &si, &ctl.set))) {
        logf("vr: controllers: no action set (the hands stay off)");
        return;
    }
    ctl.hand[0] = xpath("/user/hand/left");
    ctl.hand[1] = xpath("/user/hand/right");
    ctl.grip = make_action("grip_pose", "Hand grip", XR_ACTION_TYPE_POSE_INPUT);
    ctl.aim = make_action("aim_pose", "Hand aim", XR_ACTION_TYPE_POSE_INPUT);
    ctl.trigger = make_action("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    ctl.squeeze = make_action("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
    ctl.stick = make_action("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    ctl.lower = make_action("button_lower", "X / A", XR_ACTION_TYPE_BOOLEAN_INPUT);
    ctl.upper = make_action("button_upper", "Y / B", XR_ACTION_TYPE_BOOLEAN_INPUT);
    ctl.stick_click = make_action("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    ctl.menu = make_action("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    ctl.haptic = make_action("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    struct B { XrAction a; const char* path; };
    // (a profile from an extension the runtime does not list is not suggested: the runtime would refuse its paths)
    auto suggest = [](const char* profile, const char* ext, std::initializer_list<B> list) {
        if (ext) {
            bool on = false;
            for (const ControllerExt& e : ctl_ext) on = on || (e.on && !std::strcmp(e.name, ext));
            if (!on) {
                logf("vr: controllers: %s not offered (no %s)", profile, ext);
                return;
            }
        }
        std::vector<XrActionSuggestedBinding> v;
        for (const B& b : list)
            if (b.a) v.push_back({b.a, xpath(b.path)});
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = xpath(profile);
        sb.countSuggestedBindings = (uint32_t)v.size();
        sb.suggestedBindings = v.data();
        const XrResult r = xrSuggestInteractionProfileBindings(s.inst, &sb);
        logf("vr: controllers: %s bindings %s (%d)", profile, XR_SUCCEEDED(r) ? "suggested" : "refused", (int)r);
    };
    // Every table binds the same actions as Quest Touch's, but for the gaps a controller's buttons leave (vr/tools/test.py
    // check_controller_tables): the VR menu on the left Menu button (held), the radial wheels on the stick clicks, jump on
    // the right stick up. The VR menu also opens and closes on the left upper button held (Y; Index: B), read from upper
    // (vr/input/controllers_host.c vr_menu_y_step): SteamVR keeps Touch's left Menu for its dashboard, and binding Y
    // to menu as well would make a quick Y the game's Plus. The layouts: docs/vr-knowledge/research/controller-layouts.md.
    // Quest 1/2/3/3S, Rift S (the tested layout)
    suggest("/interaction_profiles/oculus/touch_controller", nullptr, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/value"}, {ctl.squeeze, "/user/hand/right/input/squeeze/value"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/x/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/y/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // Quest Pro: Touch's layout
    suggest("/interaction_profiles/facebook/touch_controller_pro", XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/value"}, {ctl.squeeze, "/user/hand/right/input/squeeze/value"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/x/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/y/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // Quest 3/3S Touch Plus: Touch's layout
    suggest("/interaction_profiles/meta/touch_controller_plus", XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/value"}, {ctl.squeeze, "/user/hand/right/input/squeeze/value"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/x/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/y/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // Valve Index: A/B on both hands; no Menu button (System is SteamVR's), so the left trackpad pressed firmly is Menu (its
    // tap the game's Plus, its hold the VR menu); the left B is the upper button, whose hold is the VR menu's too
    suggest("/interaction_profiles/valve/index_controller", nullptr, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/value"}, {ctl.squeeze, "/user/hand/right/input/squeeze/value"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/a/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/b/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/trackpad/force"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // HTC Vive wand: the trackpad is the stick and its click the stick click; no face buttons, so the right Menu button is A
    // (X, Y and B unbound); the grip is a click
    suggest("/interaction_profiles/htc/vive_controller", nullptr, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/click"}, {ctl.squeeze, "/user/hand/right/input/squeeze/click"},
        {ctl.stick, "/user/hand/left/input/trackpad"}, {ctl.stick, "/user/hand/right/input/trackpad"},
        {ctl.stick_click, "/user/hand/left/input/trackpad/click"}, {ctl.stick_click, "/user/hand/right/input/trackpad/click"},
        {ctl.lower, "/user/hand/right/input/menu/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // HTC Vive Cosmos: Touch's layout; the grip is a click (the shoulder buttons unbound)
    suggest("/interaction_profiles/htc/vive_cosmos_controller", XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/click"}, {ctl.squeeze, "/user/hand/right/input/squeeze/click"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/x/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/y/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // HTC Vive Focus 3: Touch's layout; the grip as a click (its value is only in the extension's second version)
    suggest("/interaction_profiles/htc/vive_focus3_controller", XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/click"}, {ctl.squeeze, "/user/hand/right/input/squeeze/click"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/x/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/y/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // Windows Mixed Reality: no face buttons, so the trackpad pressed is X/A and the right Menu button B (Y unbound); the
    // grip is a click
    suggest("/interaction_profiles/microsoft/motion_controller", nullptr, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/click"}, {ctl.squeeze, "/user/hand/right/input/squeeze/click"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/trackpad/click"}, {ctl.lower, "/user/hand/right/input/trackpad/click"},
        {ctl.upper, "/user/hand/right/input/menu/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // HP Reverb G2: Touch's layout
    suggest("/interaction_profiles/hp/mixed_reality_controller", XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/value"}, {ctl.squeeze, "/user/hand/right/input/squeeze/value"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/x/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/y/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // Pico 4: Touch's layout
    suggest("/interaction_profiles/bytedance/pico4_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.trigger, "/user/hand/left/input/trigger/value"}, {ctl.trigger, "/user/hand/right/input/trigger/value"},
        {ctl.squeeze, "/user/hand/left/input/squeeze/value"}, {ctl.squeeze, "/user/hand/right/input/squeeze/value"},
        {ctl.stick, "/user/hand/left/input/thumbstick"}, {ctl.stick, "/user/hand/right/input/thumbstick"},
        {ctl.stick_click, "/user/hand/left/input/thumbstick/click"}, {ctl.stick_click, "/user/hand/right/input/thumbstick/click"},
        {ctl.lower, "/user/hand/left/input/x/click"}, {ctl.lower, "/user/hand/right/input/a/click"},
        {ctl.upper, "/user/hand/left/input/y/click"}, {ctl.upper, "/user/hand/right/input/b/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    // The simple controller: what any runtime offers; select is X/A (no sticks: a fallback only)
    suggest("/interaction_profiles/khr/simple_controller", nullptr, {
        {ctl.grip, "/user/hand/left/input/grip/pose"}, {ctl.grip, "/user/hand/right/input/grip/pose"},
        {ctl.aim, "/user/hand/left/input/aim/pose"}, {ctl.aim, "/user/hand/right/input/aim/pose"},
        {ctl.lower, "/user/hand/left/input/select/click"}, {ctl.lower, "/user/hand/right/input/select/click"},
        {ctl.menu, "/user/hand/left/input/menu/click"},
        {ctl.haptic, "/user/hand/left/output/haptic"}, {ctl.haptic, "/user/hand/right/output/haptic"}});
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo ai{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ai.subactionPath = ctl.hand[h];
        ai.poseInActionSpace.orientation.w = 1;
        ai.action = ctl.grip;
        if (ctl.grip) xrCreateActionSpace(s.session, &ai, &ctl.grip_space[h]);
        ai.action = ctl.aim;
        if (ctl.aim) xrCreateActionSpace(s.session, &ai, &ctl.aim_space[h]);
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &ctl.set;
    ctl.attached = XR_SUCCEEDED(xrAttachSessionActionSets(s.session, &at));
    ctl.made = true;
    logf("vr: controllers: action set %s", ctl.attached ? "attached" : "NOT attached");
}

float get_float(XrAction a, int h) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a, gi.subactionPath = ctl.hand[h];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return a && XR_SUCCEEDED(xrGetActionStateFloat(s.session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
bool get_bool(XrAction a, int h) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a, gi.subactionPath = ctl.hand[h];
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return a && XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &gi, &st)) && st.isActive && st.currentState;
}

void haptic_pulse(int h, float amplitude, int64_t ns) {
    if (!ctl.haptic) return;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = ctl.haptic, hi.subactionPath = ctl.hand[h];
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude, v.duration = ns, v.frequency = XR_FREQUENCY_UNSPECIFIED;
    xrApplyHapticFeedback(s.session, &hi, (const XrHapticBaseHeader*)&v);
}

/// Each headset frame: the actions synced, both hands located at the frame's display time
void update_controls(XrTime t) {
    if (!ctl.attached) return;
    XrActiveActionSet active{ctl.set, XR_NULL_PATH};
    XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO};
    sy.countActiveActionSets = 1;
    sy.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(s.session, &sy))) return;   // (not focused: the last state stays)
    for (int h = 0; h < 2; h++) {
        HandState& hs = ctl.now[h];
        const HandState was = hs;
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        loc.next = &vel;
        hs.valid = ctl.grip_space[h] && XR_SUCCEEDED(xrLocateSpace(ctl.grip_space[h], s.local, t, &loc)) &&
                   (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                   (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (hs.valid) {
            hs.grip = loc.pose;
            const bool lv = vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT,
                       av = vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            hs.lin_vel[0] = lv ? vel.linearVelocity.x : 0, hs.lin_vel[1] = lv ? vel.linearVelocity.y : 0,
            hs.lin_vel[2] = lv ? vel.linearVelocity.z : 0;
            hs.ang_vel[0] = av ? vel.angularVelocity.x : 0, hs.ang_vel[1] = av ? vel.angularVelocity.y : 0,
            hs.ang_vel[2] = av ? vel.angularVelocity.z : 0;
        }
        XrSpaceLocation al{XR_TYPE_SPACE_LOCATION};
        if (ctl.aim_space[h] && XR_SUCCEEDED(xrLocateSpace(ctl.aim_space[h], s.local, t, &al)) &&
            (al.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            hs.aim = al.pose;
        hs.trigger = get_float(ctl.trigger, h);
        hs.squeeze = get_float(ctl.squeeze, h);
        {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = ctl.stick, gi.subactionPath = ctl.hand[h];
            XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
            const bool ok = ctl.stick && XR_SUCCEEDED(xrGetActionStateVector2f(s.session, &gi, &st)) && st.isActive;
            hs.stick[0] = ok ? st.currentState.x : 0, hs.stick[1] = ok ? st.currentState.y : 0;
        }
        hs.buttons = (get_bool(ctl.lower, h) ? HOOKSHOT_VR_BTN_LOWER : 0) | (get_bool(ctl.upper, h) ? HOOKSHOT_VR_BTN_UPPER : 0) |
                     (get_bool(ctl.stick_click, h) ? HOOKSHOT_VR_BTN_STICK : 0) | (get_bool(ctl.menu, h) ? HOOKSHOT_VR_BTN_MENU : 0) |
                     (hs.trigger > 0.85f ? HOOKSHOT_VR_BTN_TRIGGER : 0) | (hs.squeeze > 0.85f ? HOOKSHOT_VR_BTN_GRIP : 0);
        // (the test of the whole path, input to haptics: a short pulse when the trigger or grip is pressed fully, and the
        // presses in the log, 40 of them)
        const uint32_t pressed = hs.buttons & ~was.buttons;
        if (pressed & (HOOKSHOT_VR_BTN_TRIGGER | HOOKSHOT_VR_BTN_GRIP)) haptic_pulse(h, 0.4f, 25000000);
        static int told;
        if (pressed && told < 40) {
            told++;
            logf("vr: %s hand pressed %#x (trigger %.2f, grip %.2f, stick %.2f %.2f)", h ? "right" : "left", pressed,
                 hs.trigger, hs.squeeze, hs.stick[0], hs.stick[1]);
        }
    }
    {
        std::lock_guard lk(s.pose_mu);
        ctl.shared[0] = ctl.now[0], ctl.shared[1] = ctl.now[1];
        ctl.seq++;
    }
}

/// A runtime's name for a button, as the hints show it: "Y Button" is Y, "Left Menu Button" menu. Empty when it cannot be
/// shown (not ASCII: the menu's font; too long for a row)
std::string short_button_name(std::string n) {
    for (char c : n)
        if (c < 32 || c > 126) return "";
    auto cut = [&](size_t at, size_t len, const char* word) {
        if (n.size() <= len || at > n.size() - len) return;
        for (size_t i = 0; i < len; i++)
            if (std::tolower((unsigned char)n[at + i]) != word[i]) return;
        n.erase(at, len);
    };
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.size() > 7) cut(n.size() - 7, 7, " button");
    cut(0, 5, "left ");
    if (n.size() > 12) return "";
    if (n.size() > 1)
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
    return n;
}
/// Our own names for a profile's Menu and left upper button (make_controls' tables); empty: that controller has none
void own_button_names(const std::string& profile, std::string& menu, std::string& upper) {
    menu = "menu", upper = "Y";
    if (profile == "/interaction_profiles/valve/index_controller") menu = "trackpad", upper = "B";
    else if (profile == "/interaction_profiles/htc/vive_controller" || profile == "/interaction_profiles/khr/simple_controller" ||
             profile == "/interaction_profiles/microsoft/motion_controller")
        upper = "";
}
/// The left hand's source bound to an action: how many (-1: the runtime does not say), and the first one's name
int bound_button(XrAction a, std::string& name) {
    name.clear();
    if (!a || !xr_bound_sources || !xr_source_name) return -1;
    XrBoundSourcesForActionEnumerateInfo info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
    info.action = a;
    uint32_t n = 0;
    if (XR_FAILED(xr_bound_sources(s.session, &info, 0, &n, nullptr))) return -1;
    std::vector<XrPath> paths(n);
    if (n && XR_FAILED(xr_bound_sources(s.session, &info, n, &n, paths.data()))) return -1;
    paths.resize(std::min<size_t>(n, paths.size()));
    int left = 0;
    for (XrPath p : paths) {
        char buf[XR_MAX_PATH_LENGTH] = "";
        uint32_t m = 0;
        if (XR_FAILED(xrPathToString(s.inst, p, sizeof buf, &m, buf)) || std::strstr(buf, "/user/hand/right")) continue;
        left++;
        XrInputSourceLocalizedNameGetInfo gi{XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
        gi.sourcePath = p, gi.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
        if (name.empty() && XR_SUCCEEDED(xr_source_name(s.session, &gi, 0, &m, nullptr)) && m) {
            std::string got(m, 0);
            if (XR_SUCCEEDED(xr_source_name(s.session, &gi, m, &m, got.data()))) name = short_button_name(got.c_str());
        }
    }
    return left;
}
/// The names of what opens the VR menu, for its hint (menu_hint): the runtime's for what is bound now, the player's own
/// rebinding in it included, else ours for the profile. With no controller the last ones stay
void name_menu_buttons() {
    const std::string& profile = ctl.profile[0];
    if (profile.empty() || profile == "(none)") return;
    std::string menu, upper, own_menu, own_upper;
    own_button_names(profile, own_menu, own_upper);
    const int menus = bound_button(ctl.menu, menu), uppers = bound_button(ctl.upper, upper);
    const bool said = menus >= 0 && uppers >= 0 && menus + uppers > 0;   // (neither bound: a runtime that does not list them)
    if (!said || (menus && menu.empty())) menu = own_menu;
    if (!said || (uppers && upper.empty())) upper = own_upper;
    const bool pads = profile == "/interaction_profiles/htc/vive_controller";
    std::lock_guard lk(s.pose_mu);
    if (menu == ctl.menu_name && upper == ctl.upper_name && pads == ctl.pads) return;
    ctl.menu_name = menu, ctl.upper_name = upper, ctl.pads = pads;
    logf("vr: controllers: the VR menu's buttons: menu \"%s\", left upper \"%s\" (%s names)", menu.c_str(), upper.c_str(),
         said ? "the runtime's" : "our own");
}

/// The interaction profile the runtime picked for each hand (logged when it changes)
void note_profiles() {
    if (!ctl.attached) return;
    for (int h = 0; h < 2; h++) {
        XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(s.session, ctl.hand[h], &ps))) continue;
        char buf[XR_MAX_PATH_LENGTH] = "(none)";
        uint32_t n = 0;
        if (ps.interactionProfile) xrPathToString(s.inst, ps.interactionProfile, sizeof buf, &n, buf);
        if (ctl.profile[h] != buf) {
            ctl.profile[h] = buf;
            const bool tested = !strcmp(buf, "/interaction_profiles/oculus/touch_controller") || !strcmp(buf, "(none)");
            logf("vr: controllers: %s hand uses %s%s", h ? "right" : "left", buf, tested ? "" : " (untested: tested is Quest Touch)");
        }
    }
    name_menu_buttons();
}
