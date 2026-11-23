// Hookshot VR: the Window view (the VR menu's View -> Window): the game seen through a frame fixed in the room, each eye's
// picture drawn from where that eye is through the frame's four corners (an off-axis view), so leaning, crouching, rolling
// the head and stepping closer change the view as through a real window. Around the frame: the room (Background -> Room,
// where the runtime offers it) or a dark void. Window is third person (opts.vr_mode 0) with this presentation on top
// (opts.vr_window): every vr_mode == 0 / 1 check stays as it was.
//
// The game through the frame (step 3 of its build order): the game's own
// third-person orbit camera is the frame's viewpoint. hookshot_vr_head hands the game, in place of the head's turn and
// movement, no turn (the camera keeps the orbit's axes, which are the frame's: the stick orbit, Link's movement and the
// game's camera cuts move the view, never the frame in the room) and the head's movement from where it was when the frame
// was placed, in the frame's axes, metre for metre (window_head). True stereo draws each eye from there along the frame's x;
// the headset crops each eye's picture to the frame's four corners as seen from where THAT picture was drawn (the slot's
// drawn pose and eye shift, not the head now: window_crop), so the frame holds in the room while the parallax lags only by
// the game's latency. The game's FOV stays the headset's (its pipelines' P00 unchanged): a corner past the picture's edge
// (an eye very near the frame) is a coverage miss, black there, counted in the log. Debug -> Window test shows the test
// scene instead (a shadow box with blocks and a ball: vr/headset/shaders/vr_window.comp), for geometry checks. A flat
// frame (the title, menus, cutscenes) is drawn in the frame too, as large as fits (window_fit).
//
// One projection layer, no quad: each eye's view drawn at that eye's own pose and FOV this frame (xrLocateViews: asymmetric,
// canted displays included), and in it the frame where the room has it; each pixel's ray that crosses the frame picks the
// game picture's texel through the crop. Beside the frame: alpha 0 (the room) or black (Dark: then no alpha flag at all).
// The views are posed where they were drawn for, so the compositor holds the frame in the room as it does the eyes. (First
// built as one quad per eye at the frame: headset 22 Nov on WiVRn v26.9, a Quest 3 and an RTX 4070, the quads' right and
// bottom edge pixels moved with the head, as the world screen's quad's had for package D; neither filling the swapchain
// past the picture, RX_VR_FLAT_EDGE, nor a 16 px inset changed it: likely WiVRn's compositor with quad layers on NVIDIA,
// github.com/WiVRn/WiVRn/issues/873. RX_VR_WINDOW_LAYER=quad sends those quads again, for an A/B.) A head roll or a canted
// display changes nothing inside: the game's views keep the frame's axes. The layers sent are logged as they change.
//
// The room, found per runtime (never by headset name): the core blend mode ALPHA_BLEND when the runtime lists it for the
// primary stereo view; else XR_FB_passthrough (enabled at the instance's creation when the runtime lists it, its functions
// optional) as a layer under all others; else dark. A refused frame end gives that way up for the run, logged, and the next
// is tried. What each runtime offered and said is logged once ("vr: window: ...").
//
// Included by vr/headset/vk_vr.cpp after vignette_layer.inc, before frame_loop.inc (it uses s, opts, Eye, cmd, swap_format,
// qrot, make_image, barrier, shader and the headset's submit helpers). Not compiled on its own.

constexpr float kWindowWidth = 1.0f, kWindowHeight = 0.5625f;   // (m: the frame, 16:9)
constexpr float kWindowAhead = 0.9f;    // (m: placed this far ahead of the head, level)
constexpr float kWindowDrop = 0.12f;    // (m: its middle this far under the eyes)
constexpr float kWindowNear = 0.02f;    // (m: an eye closer to the frame's plane than this, or behind it, sees no window)
constexpr float kWindowFadeNear = 0.12f;   // (m: before that, the window fades out over this much more)
constexpr int kWindowRefusals = 5;      // (a way of showing the room refused this many times: given up for the run)
constexpr auto kWindowRetry = std::chrono::seconds(2);   // (and after each refusal, dark this long before it is tried again)
constexpr float kWindowBoxDepth = 1.2f; // (m: the test scene's shadow box, behind the frame)
constexpr float kWindowRim = 0.012f;    // (m: the frame's rim)
constexpr uint32_t kWindowPxW = 1920, kWindowPxH = 1080;  // (RX_VR_WINDOW_LAYER=quad: one eye's picture of the frame, above
                                                          // a Quest 3's density there)

constexpr float kWindowHandle = 0.03f;  // (m: a Move mode handle's radius as drawn; a laser within 1.5 times of it takes it)

/// The frame: its middle and turn in the room (s.local), its size (m) and shape: round 0 a rectangle, up to 1 an ellipse
/// (window_inside); feather its edge's softness (m)
struct WindowFrame {
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    float w = kWindowWidth, h = kWindowHeight;
    float round = 0, feather = 0;
};
/// One eye's off-axis view through the frame: the eye in the frame's own axes (x right, y up, z out of the frame toward the
/// viewer, metres from its middle) and the tangents of its left, right, bottom and top edges at 1 m along -z from the eye
struct WindowFrustum {
    float e[3]{};
    float l = 0, r = 0, b = 0, t = 0;
    bool front = false;   // the eye at least kWindowNear in front of the frame's plane
};

/// A point in the room in the frame's coordinates
void window_local(const WindowFrame& f, const XrVector3f& p, float out[3]) {
    const float dp[3] = {p.x - f.pose.position.x, p.y - f.pose.position.y, p.z - f.pose.position.z};
    qrot(qinv(f.pose.orientation), dp, out);
}

/// The off-axis view from an eye (in the room) through the frame's four corners. Behind the plane or nearer than kWindowNear
/// the view is the one from kWindowNear (front false: not to be shown)
bool window_frustum(const WindowFrame& f, const XrVector3f& eye, WindowFrustum* out) {
    window_local(f, eye, out->e);
    out->front = out->e[2] >= kWindowNear;
    const float d = std::max(out->e[2], kWindowNear);
    out->l = (-0.5f * f.w - out->e[0]) / d;
    out->r = (0.5f * f.w - out->e[0]) / d;
    out->b = (-0.5f * f.h - out->e[1]) / d;
    out->t = (0.5f * f.h - out->e[1]) / d;
    return out->front;
}

/// The frame for a viewpoint (at, heading yaw): dist ahead of it along the heading and drop under it, facing it, turned
/// about its own x by tilt (radians: its top away from the viewpoint), w x h
WindowFrame window_frame_at(const XrVector3f& at, float yaw, float dist, float drop, float tilt, float w, float h) {
    WindowFrame f;
    f.pose.orientation = qmul(XrQuaternionf{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)},
                              XrQuaternionf{-std::sin(tilt / 2), 0, 0, std::cos(tilt / 2)});
    f.pose.position = {at.x - std::sin(yaw) * dist, at.y - drop, at.z - std::cos(yaw) * dist};
    f.w = w, f.h = h;
    return f;
}

/// A head's heading (its yaw about the vertical)
float window_yaw(const XrQuaternionf& q) { return std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.x * q.x + q.y * q.y)); }

/// The frame placed for a head: level, facing it, kWindowAhead ahead along its heading and kWindowDrop under its eyes
WindowFrame window_place(const XrPosef& head) {
    return window_frame_at(head.position, window_yaw(head.orientation), kWindowAhead, kWindowDrop, 0, kWindowWidth, kWindowHeight);
}

/// How far a point of the frame's plane (x, y from its middle, m) is inside its shape (negative: outside): its rectangle,
/// or with round > 0 the superellipse |x/a|^n + |y/b|^n = 1 with n = 2 / round (an ellipse at round 1; rounder corners as it
/// grows), the distance there approximated
float window_inside(const WindowFrame& f, float x, float y) {
    const float a = 0.5f * f.w, b = 0.5f * f.h;
    if (!(f.round > 0.005f)) return std::min(a - std::fabs(x), b - std::fabs(y));
    const float n = 2.0f / std::min(f.round, 1.0f);
    const float s = std::pow(std::pow(std::fabs(x) / a, n) + std::pow(std::fabs(y) / b, n), 1.0f / n);
    return (1 - s) * std::min(a, b);
}

/// Where on its diagonal (as a part of the corner's distance) a rounding dot sits: k0 for a rectangle (2 handles in from the
/// corner), k1 for an ellipse (where the ellipse crosses the diagonal, 0.707, or further in for a small frame)
void window_round_range(const WindowFrame& f, float* k0, float* k1) {
    *k0 = std::max(0.3f, 1 - 2 * kWindowHandle / std::max(0.05f, std::min(0.5f * f.w, 0.5f * f.h)));
    *k1 = std::min(0.7071f, *k0 - 0.15f);
}

/// Move mode's handle i in the frame's coordinates: 0-3 the corners (resize; x, y signs - +, + +, + -, - -), 4-7 the rounding
/// dots inside them on the diagonals, further in as the corners round (window_round_range)
void window_handle(const WindowFrame& f, int i, float out[2]) {
    const float sx = (i & 3) == 1 || (i & 3) == 2 ? 1.0f : -1.0f, sy = (i & 3) < 2 ? 1.0f : -1.0f;
    float k = 1;
    if (i >= 4) {
        float k0, k1;
        window_round_range(f, &k0, &k1);
        k = k0 + (k1 - k0) * std::clamp(f.round, 0.0f, 1.0f);
    }
    out[0] = sx * 0.5f * f.w * k, out[1] = sy * 0.5f * f.h * k;
}

/// The roundness a rounding dot dragged to k of its diagonal gives (window_handle's inverse)
float window_round_at(const WindowFrame& f, float k) {
    float k0, k1;
    window_round_range(f, &k0, &k1);
    return std::clamp((k - k0) / (k1 - k0), 0.0f, 1.0f);
}

/// Where one eye's game picture shows the frame: the picture drawn from eye e (the frame's coordinates) along the frame's -z
/// with the half-FOV tangents tx and ty; out: u0, v0 (the frame's top left) and u1, v1 (its bottom right) as parts of the
/// picture, 0..1 across and down. A value outside 0..1 is a corner past the picture's edge: miss
struct WindowCrop {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    bool miss = false;
};
WindowCrop window_crop(const WindowFrame& f, const float e[3], float tx, float ty) {
    const float d = std::max(e[2], kWindowNear);
    WindowCrop c;
    c.u0 = 0.5f * (1 + (-0.5f * f.w - e[0]) / d / tx);
    c.u1 = 0.5f * (1 + (0.5f * f.w - e[0]) / d / tx);
    c.v0 = 0.5f * (1 - (0.5f * f.h - e[1]) / d / ty);
    c.v1 = 0.5f * (1 - (-0.5f * f.h - e[1]) / d / ty);
    c.miss = c.u0 < 0 || c.v0 < 0 || c.u1 > 1 || c.v1 > 1;
    return c;
}

/// A flat picture (w x h) as large as fits in the frame, centred: the part of the picture across the frame (u0, v0 its top
/// left, u1, v1 its bottom right; outside 0..1 the frame's bare border, black)
WindowCrop window_fit(const WindowFrame& f, uint32_t w, uint32_t h) {
    WindowCrop c;
    const float frame = f.w / f.h, picture = (float)w / (float)std::max(1u, h);
    if (picture >= frame) c.v0 = 0.5f - 0.5f * picture / frame, c.v1 = 0.5f + 0.5f * picture / frame;   // (bars above and below)
    else c.u0 = 0.5f - 0.5f * frame / picture, c.u1 = 0.5f + 0.5f * frame / picture;                     // (bars at the sides)
    return c;
}

/// Eye camera (the Window section's, experimental): the game camera's turn for a viewpoint e (the frame's coordinates),
/// looking at the frame's middle, level (its right along the frame's plane): x y z w, in the frame's (and so the orbit
/// camera's) axes. Too near the middle: no turn
void window_look(const float e[3], float q[4]) {
    q[0] = q[1] = q[2] = 0, q[3] = 1;
    const float n = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (!(n > 0.05f)) return;
    const float f[3] = {-e[0] / n, -e[1] / n, -e[2] / n};   // (forward: to the middle)
    float r[3] = {-f[2], 0, f[0]};                         // (right: forward x up, level)
    const float rn = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    if (!(rn > 1e-4f)) return;
    r[0] /= rn, r[2] /= rn;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float b[3] = {-f[0], -f[1], -f[2]};
    // (the matrix with columns r, u, b as a quaternion)
    const float m00 = r[0], m11 = u[1], m22 = b[2], tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q[3] = 0.25f * s, q[0] = (u[2] - b[1]) / s, q[1] = (b[0] - r[2]) / s, q[2] = (r[1] - u[0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q[3] = (u[2] - b[1]) / s, q[0] = 0.25f * s, q[1] = (u[0] + r[1]) / s, q[2] = (b[0] + r[2]) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q[3] = (b[0] - r[2]) / s, q[0] = (u[0] + r[1]) / s, q[1] = 0.25f * s, q[2] = (b[1] + u[2]) / s;
    } else {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q[3] = (r[1] - u[0]) / s, q[0] = (b[0] + r[2]) / s, q[1] = (b[1] + u[2]) / s, q[2] = 0.25f * s;
    }
}

/// Where the game picture shows each point of the frame: a map from (fx, fy), 0..1 across the frame from its top left, to
/// the picture's (u, v) for a picture drawn from cam (the frame's coordinates) with axes r, u, b (its right, up, back) and the
/// half-FOV tangents tx, ty: u = (h0 fx + h1 fy + h2) / (h6 fx + h7 fy + 1), v = (h3 fx + h4 fy + h5) / (the same). A
/// rectangle's crop (no turn) and a turned camera's view of the frame alike. False: the frame's top left not in front of it
struct WindowMap {
    float h[8] = {1, 0, 0, 0, 1, 0, 0, 0};
};
bool window_map(const WindowFrame& f, const float cam[3], const float r[3], const float u[3], const float b[3], float tx,
                float ty, WindowMap* out) {
    // (each of the camera's x, y and depth as A fx + B fy + C, the frame's point being (w fx - w/2, h/2 - h fy, 0))
    const auto lin = [&](const float a[3], float sign, float abc[3]) {
        abc[0] = sign * a[0] * f.w;
        abc[1] = -sign * a[1] * f.h;
        abc[2] = sign * (-a[0] * 0.5f * f.w + a[1] * 0.5f * f.h - (a[0] * cam[0] + a[1] * cam[1] + a[2] * cam[2]));
    };
    float cx[3], cy[3], cz[3];
    lin(r, 1, cx), lin(u, 1, cy), lin(b, -1, cz);
    float D[3], Nu[3], Nv[3];
    for (int i = 0; i < 3; i++) {
        D[i] = tx * cz[i];
        Nu[i] = 0.5f * D[i] + 0.5f * cx[i];
        Nv[i] = 0.5f * D[i] - 0.5f * (tx / ty) * cy[i];
    }
    if (!(D[2] > 1e-6f)) return false;
    const float k = 1.0f / D[2];
    *out = WindowMap{{Nu[0] * k, Nu[1] * k, Nu[2] * k, Nv[0] * k, Nv[1] * k, Nv[2] * k, D[0] * k, D[1] * k}};
    return true;
}

/// A WindowMap at a point across the frame: the picture's (u, v); false: behind the camera
bool window_map_at(const WindowMap& m, float fx, float fy, float uv[2]) {
    const float den = m.h[6] * fx + m.h[7] * fy + 1;
    if (!(den > 1e-6f)) return false;
    uv[0] = (m.h[0] * fx + m.h[1] * fy + m.h[2]) / den, uv[1] = (m.h[3] * fx + m.h[4] * fy + m.h[5]) / den;
    return true;
}

// ---- the runtime's offer (found at the start, logged) --------------------------------------------------------------------

PFN_xrEnumerateEnvironmentBlendModes window_xrEnumerateEnvironmentBlendModes = nullptr;
PFN_xrGetSystemProperties window_xrGetSystemProperties = nullptr;
PFN_xrCreatePassthroughFB window_xrCreatePassthroughFB = nullptr;
PFN_xrPassthroughStartFB window_xrPassthroughStartFB = nullptr;
PFN_xrPassthroughPauseFB window_xrPassthroughPauseFB = nullptr;
PFN_xrCreatePassthroughLayerFB window_xrCreatePassthroughLayerFB = nullptr;
PFN_xrPassthroughLayerResumeFB window_xrPassthroughLayerResumeFB = nullptr;

enum WindowRoom { kRoomDark, kRoomAlpha, kRoomFb };
const char* window_room_said(int r) { return r == kRoomAlpha ? "the room (alpha blend)" : r == kRoomFb ? "the room (XR_FB_passthrough)" : "dark"; }

struct WindowState {
    // (the runtime's offer)
    bool fb_listed = false;       // XR_FB_passthrough in the runtime's instance extensions (then enabled)
    bool fb_system = false;       // the system says it supports passthrough
    bool alpha_listed = false;    // ALPHA_BLEND among the primary stereo view's blend modes
    bool additive_only = false;   // ADDITIVE the only one (black cannot cover the room: dark is shown there)
    bool alpha_failed = false, fb_failed = false;   // (refused again and again, or a failed creation: given up for the run)
    int alpha_refused = 0, fb_refused = 0;          // (refused frame ends with that way: given up after kWindowRefusals)
    std::chrono::steady_clock::time_point refused_at{};   // (the last refusal: that way rested kWindowRetry, then tried again)
    // (the headset thread)
    bool placed = false;
    XrVector3f anchor{};          // the viewpoint the frame is placed from (where the head was, or where Move left it)
    float anchor_yaw = 0;         // its heading
    WindowFrame frame;            // (this frame's: from the anchor and the Window rows, window_ensure_placed)
    struct Edit {                 // Move mode (the VR menu's Move): the window dragged with a controller's laser
        bool on = false;
        int seen = -1;            // opts.vr_window_move when last seen
        int hand = -1, what = 0;  // the hand dragging, and what: 1 the window, 2 a corner (its size), 3 a rounding dot
        int handle = -1;
        float reach = 0;          // (dragging the window: how far along the laser it is held)
        float grab[2]{};          // (where on it the laser took it, the frame's coordinates)
        int hover[2]{-1, -1};     // each hand's: -1 nothing, 0-7 a handle, 8 the window
        float hit[2][2]{}, along[2]{};   // where each hand's laser meets the frame's plane, and how far along it
        bool aimed[2]{};
        bool trigger_was[2]{true, true}, back_was = true;
        bool changed = false;     // (options changed by a drag: saved once it ends)
        std::chrono::steady_clock::time_point last{};
    } edit;
    std::atomic<bool> editing{false};   // (the game side's hands masked: window_mask)
    std::atomic<bool> options_changed{false};
    int place_seen = -1;          // opts.vr_window_place when last placed (the menu's Place window)
    bool was_on = false;
    XrVector3f eye[2]{};          // the eyes' last places in the room with position tracked (held while it is lost)
    bool eye_known = false, lost = false, behind = false;
    XrQuaternionf eye_q[2]{{0, 0, 0, 1}, {0, 0, 0, 1}};   // the eyes' turns in the room (held with their places)
    XrFovf fov[2]{{-.8f, .8f, .8f, -.8f}, {-.8f, .8f, .8f, -.8f}};   // the eyes' own four angles, the last located
    Eye art;                      // (one swapchain: the left eye's picture, then the right's beside it)
    uint32_t px_w = 0, px_h = 0;  // (one eye's picture in it)
    float fade = 1;               // (the window's own fade: out as an eye nears the frame's plane, none behind it)
    std::string sent_said;        // (the log: the layers last sent, as said)
    Image out;
    struct Pipe {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImageView bound = VK_NULL_HANDLE;   // (the storage image its binding 0 points at)
        bool failed = false;
    } test, game;                 // (the test scene's, and the game's crop's)
    WindowFrame shared_frame;     // (s.pose_mu) the frame and its viewpoint, for the game thread's camera (window_head)
    XrVector3f shared_anchor{};
    bool shared_placed = false;
    int game_said = -1;           // (the log: the game's source last said, 0 one picture, 1 each eye its own)
    int misses = 0, crops = 0;    // (coverage misses and crops since last logged)
    int64_t misses_at = 0;
    XrPassthroughFB pt = XR_NULL_HANDLE;
    XrPassthroughLayerFB pt_layer = XR_NULL_HANDLE;
    bool pt_running = false;
    int room = kRoomDark;         // this frame's room (decided in window_layers, for the underlay and the frame's end)
    int room_said = -1;           // the room last logged
    int end_said = 0;             // frame ends logged since the room last changed
    int64_t frames = 0;
} window;

/// (the instance's creation) XR_FB_passthrough enabled where the runtime lists it
void window_instance_extensions(std::vector<const char*>& exts) {
    PFN_xrEnumerateInstanceExtensionProperties list = nullptr;
    if (!xr_proc || XR_FAILED(xr_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&list)) ||
        !list) {
        logf("vr: window: the runtime's extensions could not be listed: no XR_FB_passthrough");
        return;
    }
    uint32_t n = 0;
    if (XR_FAILED(list(nullptr, 0, &n, nullptr))) return;
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(list(nullptr, n, &n, props.data()))) return;
    for (const XrExtensionProperties& p : props)
        if (!std::strcmp(p.extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME)) window.fb_listed = true;
    if (window.fb_listed) exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    logf("vr: window: the runtime lists %u instance extension(s), XR_FB_passthrough %s", n,
         window.fb_listed ? "among them (enabled)" : "not among them");
}

/// (the instance made) the optional functions: each may be missing
void window_load_functions(XrInstance inst) {
    const auto get = [&](const char* name, auto* fn) {
        if (XR_FAILED(xr_proc(inst, name, (PFN_xrVoidFunction*)fn))) *fn = nullptr;
    };
    get("xrEnumerateEnvironmentBlendModes", &window_xrEnumerateEnvironmentBlendModes);
    get("xrGetSystemProperties", &window_xrGetSystemProperties);
    if (!window.fb_listed) return;
    get("xrCreatePassthroughFB", &window_xrCreatePassthroughFB);
    get("xrPassthroughStartFB", &window_xrPassthroughStartFB);
    get("xrPassthroughPauseFB", &window_xrPassthroughPauseFB);
    get("xrCreatePassthroughLayerFB", &window_xrCreatePassthroughLayerFB);
    get("xrPassthroughLayerResumeFB", &window_xrPassthroughLayerResumeFB);
}

/// (the session made) what the runtime offers the Window view, logged once: the runtime, the blend modes, passthrough, the
/// colour format
void window_runtime_report() {
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(s.inst, &ip)))
        logf("vr: window: runtime %s %u.%u.%u", ip.runtimeName, (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion),
             (unsigned)XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)XR_VERSION_PATCH(ip.runtimeVersion));
    std::string modes;
    uint32_t n = 0;
    XrEnvironmentBlendMode list[8];
    if (window_xrEnumerateEnvironmentBlendModes &&
        XR_SUCCEEDED(window_xrEnumerateEnvironmentBlendModes(s.inst, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &n,
                                                              list))) {
        bool opaque = false, additive = false;
        for (uint32_t i = 0; i < n && i < 8; i++) {
            const XrEnvironmentBlendMode m = list[i];
            opaque = opaque || m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            additive = additive || m == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
            window.alpha_listed = window.alpha_listed || m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
            modes += modes.empty() ? "" : ", ";
            modes += m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE ? "opaque" : m == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE ? "additive"
                     : m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? "alpha blend" : std::to_string((int)m);
        }
        window.additive_only = additive && !opaque && !window.alpha_listed;
    } else {
        modes = window_xrEnumerateEnvironmentBlendModes ? "not said" : "not asked (no function)";
    }
    if (window.fb_listed && window_xrGetSystemProperties) {
        XrSystemPassthroughProperties2FB p2{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
        XrSystemPassthroughPropertiesFB p1{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
        p1.next = &p2;
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        sp.next = &p1;
        if (XR_SUCCEEDED(window_xrGetSystemProperties(s.inst, s.system, &sp)))
            window.fb_system = p1.supportsPassthrough || (p2.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB);
        logf("vr: window: system %s, XR_FB_passthrough %s (capabilities %#llx)", sp.systemName,
             window.fb_system ? "supported" : "not supported", (unsigned long long)p2.capabilities);
    }
    const bool fb_fns = window_xrCreatePassthroughFB && window_xrPassthroughStartFB && window_xrCreatePassthroughLayerFB &&
                        window_xrPassthroughLayerResumeFB;
    window.fb_system = window.fb_system && fb_fns;
    logf("vr: window: primary stereo blend modes: %s; XR_FB_passthrough %s; colour swapchain %s; the room shown by %s", modes.c_str(),
         !window.fb_listed ? "not listed" : !fb_fns ? "listed, its functions missing" : window.fb_system ? "usable" : "not supported",
         swap_format == VK_FORMAT_R8G8B8A8_SRGB ? "RGBA8 sRGB" : "RGBA8 UNORM",
         window.alpha_listed ? "alpha blend" : window.fb_system ? "XR_FB_passthrough" : window.additive_only ? "nothing (additive)"
                                                                                                            : "nothing (opaque only)");
}

// ---- each frame (the headset thread) -----------------------------------------------------------------------------------

/// Window view now (third person, opts.vr_window; the menu's View -> Window)
bool window_view() { return opts.vr && opts.vr_window && opts.vr_mode == 0; }

/// (the game thread, s.pose_mu held: hookshot_vr_head) In Window view, the game's camera through the frame in place of the
/// head's turn and movement: no turn (the orbit camera's own axes are the frame's) and the head's place from the frame's
/// viewpoint (where the head was as the frame was placed: kWindowAhead in front of it, kWindowDrop over its middle), in the
/// frame's axes (x right, y up, z back), metres. False: not the Window view, the test scene, or no frame yet
bool window_head(const XrPosef& head, float q[4], float p[3]) {
    if (!window_view() || opts.vr_window_test || !window.shared_placed) return false;
    float e[3], at[3];
    window_local(window.shared_frame, head.position, e);
    window_local(window.shared_frame, window.shared_anchor, at);
    if (opts.vr_window_eye_camera) window_look(e, q);   // (Eye camera: turned from the head to the frame's middle)
    else q[0] = q[1] = q[2] = 0, q[3] = 1;
    p[0] = e[0] - at[0], p[1] = e[1] - at[1], p[2] = e[2] - at[2];
    return true;
}

/// The room Background -> Room asks for, as this runtime can show it (RX_VR_WINDOW_ROOM=alpha|fb|dark, a test: that way first)
int window_room_want() {
    if (!opts.vr_window_room) return kRoomDark;
    static const int forced = [] {
        const char* e = getenv("RX_VR_WINDOW_ROOM");
        const int f = !e ? -1 : !std::strcmp(e, "alpha") ? kRoomAlpha : !std::strcmp(e, "fb") ? kRoomFb : !std::strcmp(e, "dark") ? kRoomDark : -1;
        if (f >= 0) logf("vr: RX_VR_WINDOW_ROOM=%s (test): the Window view's room tried that way first", e);
        return f;
    }();
    const bool resting = window.refused_at.time_since_epoch().count() &&
                         std::chrono::steady_clock::now() - window.refused_at < kWindowRetry;
    const bool alpha = !window.alpha_failed && !resting && (window.alpha_listed || forced == kRoomAlpha);
    const bool fb = !window.fb_failed && !resting && window.fb_system;
    if (forced == kRoomDark) return kRoomDark;
    if (forced == kRoomFb && fb) return kRoomFb;
    if (alpha) return kRoomAlpha;
    if (fb) return kRoomFb;
    return kRoomDark;
}

/// XR_FB_passthrough running, made the first time (false: it failed, given up for the run)
bool window_passthrough_on() {
    if (window.pt_running) return true;
    if (!window.pt) {
        XrPassthroughCreateInfoFB pc{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pc.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        XrResult r = window_xrCreatePassthroughFB(s.session, &pc, &window.pt);
        if (XR_SUCCEEDED(r)) {
            XrPassthroughLayerCreateInfoFB lc{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            lc.passthrough = window.pt;
            lc.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            lc.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            r = window_xrCreatePassthroughLayerFB(s.session, &lc, &window.pt_layer);
        }
        logf("vr: window: XR_FB_passthrough %s (%d)", XR_SUCCEEDED(r) ? "made" : "could not be made: the room given up", (int)r);
        if (XR_FAILED(r)) return window.fb_failed = true, false;
        return window.pt_running = true;
    }
    XrResult r = window_xrPassthroughStartFB(window.pt);
    if (XR_SUCCEEDED(r)) r = window_xrPassthroughLayerResumeFB(window.pt_layer);
    if (XR_FAILED(r)) {
        logf("vr: window: XR_FB_passthrough could not start again (%d): the room given up", (int)r);
        return window.fb_failed = true, false;
    }
    return window.pt_running = true;
}
void window_passthrough_off() {
    if (!window.pt_running) return;
    window.pt_running = false;
    if (window_xrPassthroughPauseFB) window_xrPassthroughPauseFB(window.pt);
}

/// A compute pipeline of the Window view's, made the first time (false: it could not be, logged once). bindings: binding 0
/// the eyes' storage image, then that many pictures sampled
bool window_pipe(WindowState::Pipe& p, int pictures, uint32_t push_bytes, const uint32_t* spv, size_t spv_bytes, const char* what) {
    if (p.pipe) return true;
    if (p.failed) return false;
    p.failed = true;
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1 + (uint32_t)pictures;
    dl.pBindings = b;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = pictures ? 2 : 1;
    pi.pPoolSizes = ps;
    bool ok = vkCreateDescriptorSetLayout(dev.device, &dl, nullptr, &p.dsl) == VK_SUCCESS &&
              vkCreatePipelineLayout(dev.device, &pl, nullptr, &p.layout) == VK_SUCCESS &&
              vkCreateDescriptorPool(dev.device, &pi, nullptr, &p.pool) == VK_SUCCESS;
    if (ok) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = p.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p.dsl;
        ok = vkAllocateDescriptorSets(dev.device, &ai, &p.set) == VK_SUCCESS;
    }
    VkShaderModule m = ok ? shader(spv, spv_bytes) : VK_NULL_HANDLE;
    if (m) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = p.layout;
        ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipe) == VK_SUCCESS;
        vkDestroyShaderModule(dev.device, m, nullptr);
    }
    if (!ok || !p.pipe) {
        logf("vr: window: %s's pipeline could not be made: the Window view shows nothing", what);
        return false;
    }
    p.failed = false;
    return true;
}

/// Whether the Window view goes as one quad per eye at the frame (RX_VR_WINDOW_LAYER=quad, a test) instead of the
/// projection layer
bool window_as_quads() {
    static const bool quad = [] {
        const char* e = vr_research_getenv("RX_VR_WINDOW_LAYER");
        const bool q = e && !std::strcmp(e, "quad");
        if (q) logf("vr: RX_VR_WINDOW_LAYER=quad (test): the Window view as one quad per eye at the frame");
        return q;
    }();
    return quad;
}

/// One eye's picture size: the projection layer's the runtime's recommended, at most opts.vr_eye_height tall (2160 unset);
/// the quads' kWindowPxW x kWindowPxH
void window_picture_size(uint32_t* w, uint32_t* h) {
    if (window_as_quads()) return void(*w = kWindowPxW), void(*h = kWindowPxH);
    static uint32_t rw = 0, rh = 0;
    if (!rw) {
        uint32_t n = 0;
        XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(s.inst, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)) &&
            n >= 1 && v[0].recommendedImageRectWidth && v[0].recommendedImageRectHeight)
            rw = v[0].recommendedImageRectWidth, rh = v[0].recommendedImageRectHeight;
        else
            rw = 1800, rh = 1800;
    }
    const uint32_t cap = std::max(720u, opts.vr_eye_height > 0 ? (uint32_t)opts.vr_eye_height : 2160u);
    *w = rw, *h = rh;
    if (*h > cap) *w = (uint32_t)((uint64_t)rw * cap / rh) & ~1u, *h = cap;
}

/// The window's swapchain (both eyes' pictures side by side) and the storage image they are drawn in, made the first time
/// and again when the picture's size changes; binding 0 of p's set pointed at it
bool window_images(WindowState::Pipe& p) {
    Eye& a = window.art;
    uint32_t pw = 0, ph = 0;
    window_picture_size(&pw, &ph);
    if (a.chain && (window.px_w != pw || window.px_h != ph)) xrDestroySwapchain(a.chain), a = Eye{};
    if (!a.chain) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = swap_format;
        ci.sampleCount = 1;
        ci.width = 2 * pw, ci.height = ph;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &a.chain))) {
            a.chain = XR_NULL_HANDLE;
            headset_fail("window: its swapchain could not be made");
            return false;
        }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(a.chain, 0, &n, nullptr);
        a.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(a.chain, n, &n, (XrSwapchainImageBaseHeader*)a.images.data());
        a.w = 2 * pw, a.h = ph;
        window.px_w = pw, window.px_h = ph;
        logf("vr: window: two eyes' pictures of %ux%u, %u images", pw, ph, n);
    }
    if (!make_image(window.out, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 2 * pw, ph))
        return false;
    if (p.bound != window.out.view) {   // (no submit of ours reads the set now: headset_gpu_free)
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, window.out.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = p.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev.device, 1, &w, 0, nullptr);
        p.bound = window.out.view;
    }
    return true;
}

/// The recorded dispatches' storage image copied into the swapchain's acquired image, and submitted (after wait_value of the
/// timeline, when given)
bool window_finish(SwapTake& take, uint32_t index, uint64_t wait_value) {
    barrier(cmd, window.out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImage dst = window.art.images[index].image;
    barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {2 * window.px_w, window.px_h, 1};
    vkCmdCopyImage(cmd, window.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return headset_submit(take, wait_value ? s.timeline : VK_NULL_HANDLE, wait_value, "window");
}

/// One eye's view for the window's passes: the eye in the frame's coordinates, its view's four tangents and its axes there
/// (the projection layer: the eye's own FOV and turn; the quads: the off-axis view through the corners, the frame's axes)
struct WindowEyeView {
    float e[3];
    float l, r, b, t;
    float axis[3][3];
};

/// Move mode's handles as the window's passes draw them: 0 none (not in Move mode), else 1 + the handle under a laser or
/// dragged + 1 (0-7; none: 1)
float window_edit_mark() {
    const WindowState::Edit& ed = window.edit;
    if (!ed.on) return 0;
    int hot = ed.hand >= 0 ? ed.handle : ed.hover[0] >= 0 && ed.hover[0] < 8 ? ed.hover[0] : ed.hover[1];
    if (hot > 7) hot = -1;
    return 1.0f + (float)(hot + 1);
}

/// The test scene's pictures drawn into one swapchain image (left, then right beside it)
bool window_draw(const WindowEyeView ev[2]) {
    if (!headset_gpu_free() || !window_pipe(window.test, 0, 128, VR_WINDOW_COMP_SPV, sizeof VR_WINDOW_COMP_SPV, "the test scene"))
        return false;
    if (!window_images(window.test)) return false;
    uint32_t index = 0;
    SwapTake take;
    if (!swap_take(take, {window.art.chain}, &index, "window")) return false;
    if (!headset_record()) return swap_give(take), false;
    barrier(cmd, window.out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, window.test.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, window.test.layout, 0, 1, &window.test.set, 0, nullptr);
    for (int e = 0; e < 2; e++) {
        const WindowEyeView& v = ev[e];
        const float push[32] = {(float)window.px_w, (float)window.px_h, 0.5f * window.frame.w, 0.5f * window.frame.h,
                                v.e[0], v.e[1], v.e[2], (float)(e * window.px_w), v.l, v.r, v.b, v.t,
                                kWindowBoxDepth, kWindowRim, 1.0f, 0.0f,
                                v.axis[0][0], v.axis[0][1], v.axis[0][2], 0, v.axis[1][0], v.axis[1][1], v.axis[1][2], 0,
                                v.axis[2][0], v.axis[2][1], v.axis[2][2], 0,
                                window.frame.round, window.frame.feather, window_edit_mark(), window.fade};
        vkCmdPushConstants(cmd, window.test.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(cmd, (window.px_w + 7) / 8, (window.px_h + 7) / 8, 1);
    }
    return window_finish(take, index, 0);
}

/// Slot sl's pictures (left, right) drawn into the window's swapchain image: each eye's view, its pixels on the frame
/// showing that eye's crop of its picture
bool window_crops_draw(Slot& sl, const Image& left, const Image& right, const WindowMap map[2], const WindowEyeView ev[2]) {
    if (!headset_gpu_free() ||
        !window_pipe(window.game, 2, 128, VR_WINDOW_GAME_COMP_SPV, sizeof VR_WINDOW_GAME_COMP_SPV, "the game's crop"))
        return false;
    if (!window_images(window.game)) return false;
    // (the pictures: re-pointed every frame, the slot's images may be new; no submit of ours reads the set now)
    VkDescriptorImageInfo ii[2] = {{s.linear, left.view, VK_IMAGE_LAYOUT_GENERAL}, {s.linear, right.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = window.game.set;
        w[i].dstBinding = (uint32_t)(1 + i);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev.device, 2, w, 0, nullptr);
    uint32_t index = 0;
    SwapTake take;
    if (!swap_take(take, {window.art.chain}, &index, "window")) return false;
    if (!headset_record()) return swap_give(take), false;
    barrier(cmd, window.out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, window.game.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, window.game.layout, 0, 1, &window.game.set, 0, nullptr);
    for (int e = 0; e < 2; e++) {
        const WindowEyeView& v = ev[e];
        const float* h = map[e].h;
        const float push[32] = {window.fade, opts.vr_window_back ? 1.0f : 0.0f, h[7], (float)e, h[0], h[1], h[2], h[3],
                                v.e[0], v.e[1], v.e[2], window_edit_mark(), v.l, v.r, v.b, v.t,
                                v.axis[0][0], v.axis[0][1], v.axis[0][2], h[4], v.axis[1][0], v.axis[1][1], v.axis[1][2], h[5],
                                v.axis[2][0], v.axis[2][1], v.axis[2][2], h[6],
                                0.5f * window.frame.w, 0.5f * window.frame.h, window.frame.round, window.frame.feather};
        vkCmdPushConstants(cmd, window.game.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(cmd, (window.px_w + 7) / 8, (window.px_h + 7) / 8, 1);
    }
    return window_finish(take, index, sl.value);
}

/// A flat frame (slot k: the title, a menu, a cutscene, the quick select) in the window: its picture as large as fits
bool window_flat_draw(int k, const WindowEyeView ev[2]) {
    Slot& sl = s.slots[k];
    if (!sl.color.view || !sl.color.w || !sl.color.h) return false;
    const WindowCrop fit = window_fit(window.frame, sl.color.w, sl.color.h);
    WindowMap m;   // (straight across: u from u0 to u1, v from v0 to v1)
    m.h[0] = fit.u1 - fit.u0, m.h[2] = fit.u0, m.h[4] = fit.v1 - fit.v0, m.h[5] = fit.v0;
    const WindowMap map[2] = {m, m};
    return window_crops_draw(sl, sl.color, sl.color, map, ev);
}

/// The game's pictures through the frame (slot k): a gameplay frame's, each eye's picture cropped to the frame's corners as
/// seen from where that picture was drawn (the slot's drawn head in the frame's coordinates, and its eye's shift along the
/// frame's x); a flat frame's, its one picture as large as fits in the frame, for both eyes (window_fit). Into one
/// swapchain image, each eye's view ev. False: nothing to show (logged once)
bool window_game_draw(int k, const WindowEyeView ev[2]) {
    Slot& sl = s.slots[k];
    if (sl.head_locked) return window_flat_draw(k, ev);
    const bool projection_ok = std::isfinite(sl.proj[0]) && std::isfinite(sl.proj[5]) && std::fabs(sl.proj[0]) > 1e-5f &&
                               std::fabs(sl.proj[5]) > 1e-5f && sl.proj[14] == -1.0f;
    if (!sl.drawn_ok || !projection_ok) {
        static int told;
        if (told++ < 3) logf("vr: window: a gameplay frame without its drawn pose or camera: not shown");
        return false;
    }
    const int said = sl.has_eye2 ? 1 : 0;
    if (said != window.game_said) {
        window.game_said = said;
        logf("vr: window: the game through the frame: %s", said ? "each eye its own picture (True stereo)"
                                                               : "one picture for both eyes (3D is From depth: True stereo for depth)");
    }
    float mid[3];
    window_local(window.frame, sl.drawn.position, mid);
    const float tx = 1.0f / std::fabs(sl.proj[0]), ty = 1.0f / std::fabs(sl.proj[5]);
    // (the camera each picture was drawn with, in the frame's coordinates: the drawn head's place, turned to the frame's
    // middle with Eye camera (as window_head told the game), each eye's shift along the camera's right)
    float q[4] = {0, 0, 0, 1};
    if (opts.vr_window_eye_camera) window_look(mid, q);
    const XrQuaternionf cq{q[0], q[1], q[2], q[3]};
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float r[3], u[3], b[3];
    qrot(cq, ex, r), qrot(cq, ey, u), qrot(cq, ez, b);
    WindowMap map[2];
    WindowCrop crop[2];   // (for the log: where the frame's corners fall in the left eye's picture)
    for (int e = 0; e < 2; e++) {
        const float shift = sl.eyes_mode ? sl.eye_shift[sl.has_eye2 ? e : 0] : 0.0f;
        const float cam[3] = {mid[0] + shift * r[0], mid[1] + shift * r[1], mid[2] + shift * r[2]};
        bool miss = !window_map(window.frame, cam, r, u, b, tx, ty, &map[e]);
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (int c = 0; c < 4 && !miss; c++) {
            float uv[2];
            if (!window_map_at(map[e], (float)(c & 1), (float)(c >> 1), uv)) miss = true;
            for (int i = 0; i < 2; i++) lo[i] = std::min(lo[i], uv[i]), hi[i] = std::max(hi[i], uv[i]);
        }
        miss = miss || lo[0] < 0 || lo[1] < 0 || hi[0] > 1 || hi[1] > 1;
        crop[e].u0 = lo[0], crop[e].v0 = lo[1], crop[e].u1 = hi[0], crop[e].v1 = hi[1], crop[e].miss = miss;
        window.misses += miss ? 1 : 0;
    }
    window.crops++;
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - window.misses_at >= 10) {   // (now and then: the crops, and the frames whose corners the picture missed)
        if (window.misses_at)
            logf("vr: window: %d frames through the frame in the last 10 s, %d eye pictures missing a corner (an eye too near "
                 "the frame for the game's FOV: black there); the left eye's crop %.2f %.2f - %.2f %.2f of its picture, from "
                 "%.2f %.2f %.2f m", window.crops, window.misses, crop[0].u0, crop[0].v0, crop[0].u1, crop[0].v1, mid[0], mid[1],
                 mid[2]);
        window.misses_at = now, window.misses = window.crops = 0;
    }
    return window_crops_draw(sl, sl.color, sl.has_eye2 ? sl.color2 : sl.color, map, ev);
}

/// The frame placed, if it is not yet: on entering the Window view (a gameplay frame or a flat one first) and at Place window
/// The frame from its viewpoint and the Window rows (Size, Keep 16:9, Height, Roundness, Feather, Tilt, Distance)
WindowFrame window_frame_now() {
    const float w = std::clamp(opts.vr_window_width, 20, 300) * 0.01f;
    const float h = opts.vr_window_169 ? w * 9 / 16 : std::clamp(opts.vr_window_height, 10, 200) * 0.01f;
    WindowFrame f = window_frame_at(window.anchor, window.anchor_yaw, std::clamp(opts.vr_window_distance, 40, 300) * 0.01f,
                                    kWindowDrop, std::clamp(opts.vr_window_tilt, -45, 45) / 57.2958f, w, h);
    f.round = std::clamp(opts.vr_window_round, 0, 100) * 0.01f;
    f.feather = std::clamp(opts.vr_window_feather, 0, 100) * 0.001f;
    return f;
}

/// The frame placed, if it is not yet (on entering the Window view, a gameplay frame or a flat one first, and at Place
/// window: its viewpoint where the head is), and this frame's from its viewpoint and the Window rows
void window_ensure_placed(const XrPosef& head) {
    const bool place = !window.was_on || !window.placed || opts.vr_window_place != window.place_seen;
    if (place) window.anchor = head.position, window.anchor_yaw = window_yaw(head.orientation);
    window.frame = window_frame_now();
    if (place) {
        window.placed = true, window.place_seen = opts.vr_window_place;
        logf("vr: window: placed %.2f m ahead, %.2f x %.2f m, its middle at %.2f %.2f %.2f m in the room (heading %.0f deg)",
             opts.vr_window_distance * 0.01f, window.frame.w, window.frame.h, window.frame.pose.position.x,
             window.frame.pose.position.y, window.frame.pose.position.z, window.anchor_yaw * 57.2958f);
    }
    {
        std::lock_guard lk(s.pose_mu);
        window.shared_frame = window.frame, window.shared_anchor = window.anchor, window.shared_placed = true;
    }
    window.was_on = true;
}

// ---- Move mode ---------------------------------------------------------------------------------------------------------

/// Whether Move mode changed the window's options since last asked (vk_vr.h window_options_changed, vr_menu_api.inc)
bool window_take_options_changed() { return window.options_changed.exchange(false, std::memory_order_acq_rel); }

/// (host_api.inc, the game side's hands, pose_mu held) while Move mode has the controllers, the game is told nothing of
/// them (buttons but Menu, triggers, grips and sticks let go), and until they are all let go after it ends
void window_mask(int h, float& trigger, float& squeeze, float* stick, uint32_t* buttons) {
    static bool holding = false;
    if (window.editing.load(std::memory_order_acquire)) holding = true;
    else if (holding) {
        bool down = false;
        for (int k = 0; k < 2; k++)
            down |= (ctl.shared[k].buttons & ~(uint32_t)HOOKSHOT_VR_BTN_MENU) || ctl.shared[k].trigger > .3f || ctl.shared[k].squeeze > .3f;
        if (!down) holding = false;
    }
    if (!holding) return;
    (void)h;
    trigger = squeeze = 0;
    if (stick) stick[0] = stick[1] = 0;
    if (buttons) *buttons &= HOOKSHOT_VR_BTN_MENU;
}

/// The viewpoint that puts the frame's middle at c, facing the head (level), at the Distance row's distance
void window_anchor_for(const XrVector3f& c, const XrPosef& head) {
    const float dx = c.x - head.position.x, dz = c.z - head.position.z;
    const float yaw = dx * dx + dz * dz > 1e-4f ? std::atan2(-dx, -dz) : window.anchor_yaw;
    const float dist = std::clamp(opts.vr_window_distance, 40, 300) * 0.01f;
    window.anchor_yaw = yaw;
    window.anchor = {c.x + std::sin(yaw) * dist, c.y + kWindowDrop, c.z + std::cos(yaw) * dist};
}

/// Move mode, once a frame (before the window's layers): entered from the VR menu's Move, left with B or Y, the VR menu
/// opened, or the Window view left. Each hand's laser on the frame: the trigger held inside it drags the window (the stick
/// pushing it away or pulling it closer), on a corner dot resizes it about its middle (at 16:9 with Keep 16:9), on a rounding
/// dot rounds its corners. A drag's new options are set at once and saved when it ends
void window_edit_frame(const XrPosef& head) {
    WindowState::Edit& ed = window.edit;
    if (ed.seen < 0) ed.seen = opts.vr_window_move;
    const auto now = std::chrono::steady_clock::now();
    const float dt = ed.last.time_since_epoch().count() ? std::clamp(std::chrono::duration<float>(now - ed.last).count(), 0.0f, 0.1f) : 0;
    ed.last = now;
    if (opts.vr_window_move != ed.seen) {
        ed.seen = opts.vr_window_move;
        if (window_view() && !ed.on) {
            ed.on = true, ed.hand = -1, ed.trigger_was[0] = ed.trigger_was[1] = ed.back_was = true;
            panel_set_open(false);
            logf("vr: window: Move mode: point at the window, hold the trigger and drag; B or Y ends it");
        }
    }
    const bool back = ctl.attached && ((ctl.now[0].buttons | ctl.now[1].buttons) & HOOKSHOT_VR_BTN_UPPER);
    if (ed.on && (!window_view() || panel_open() || (back && !ed.back_was))) {
        ed.on = false, ed.hand = -1;
        logf("vr: window: Move mode ended: %d x %d cm, roundness %d%%", opts.vr_window_width,
             opts.vr_window_169 ? opts.vr_window_width * 9 / 16 : opts.vr_window_height, opts.vr_window_round);
    }
    ed.back_was = back;
    window.editing.store(ed.on, std::memory_order_release);
    if (!ed.on || !ctl.attached || !window.placed) {
        if (ed.changed) ed.changed = false, window.options_changed.store(true, std::memory_order_release);
        return;
    }
    const WindowFrame& f = window.frame;
    for (int h = 0; h < 2; h++) {   // (each laser on the frame's plane, and what it is over)
        const HandState& hs = ctl.now[h];
        ed.aimed[h] = false, ed.hover[h] = -1;
        if (!hs.valid) continue;
        const float fwd[3] = {0, 0, -1};
        float d[3], o[3], dl[3];
        qrot(hs.aim.orientation, fwd, d);
        window_local(f, hs.aim.position, o);
        qrot(qinv(f.pose.orientation), d, dl);
        if (!(o[2] > 0) || !(dl[2] < -1e-3f)) continue;   // (behind the frame, or pointing away from it)
        const float along = o[2] / -dl[2];
        ed.aimed[h] = true, ed.along[h] = along;
        ed.hit[h][0] = o[0] + dl[0] * along, ed.hit[h][1] = o[1] + dl[1] * along;
        float best = 1.5f * kWindowHandle;
        for (int i = 0; i < 8; i++) {
            float at[2];
            window_handle(f, i, at);
            const float dd = std::hypot(ed.hit[h][0] - at[0], ed.hit[h][1] - at[1]);
            if (dd < best) best = dd, ed.hover[h] = i;
        }
        if (ed.hover[h] < 0 && std::fabs(ed.hit[h][0]) < 0.5f * f.w && std::fabs(ed.hit[h][1]) < 0.5f * f.h) ed.hover[h] = 8;
    }
    for (int h = 0; h < 2; h++) {   // (the triggers: a drag taken, held, let go)
        const float trig = ctl.now[h].trigger;
        const bool press = trig > .75f && !ed.trigger_was[h], release = trig < .4f;
        if (trig > .75f) ed.trigger_was[h] = true;
        else if (release) ed.trigger_was[h] = false;
        if (ed.hand == h && release) {
            ed.hand = -1;
            if (ed.changed) ed.changed = false, window.options_changed.store(true, std::memory_order_release);
        }
        if (press && ed.hand < 0 && ed.hover[h] >= 0) {
            ed.hand = h, ed.handle = ed.hover[h];
            ed.what = ed.handle == 8 ? 1 : ed.handle < 4 ? 2 : 3;
            ed.reach = ed.along[h], ed.grab[0] = ed.hit[h][0], ed.grab[1] = ed.hit[h][1];
            hookshot_vr_rumble(h, .15f, .015f);
        }
    }
    if (ed.hand < 0) return;
    const int h = ed.hand;
    const HandState& hs = ctl.now[h];
    if (!hs.valid) return;
    if (ed.what == 1) {   // (the window: held where the laser took it, the stick pushing it along the laser)
        ed.reach = std::clamp(ed.reach + hs.stick[1] * 1.5f * dt, 0.3f, 6.0f);
        const float fwd[3] = {0, 0, -1};
        float d[3];
        qrot(hs.aim.orientation, fwd, d);
        const XrVector3f held{hs.aim.position.x + d[0] * ed.reach, hs.aim.position.y + d[1] * ed.reach,
                              hs.aim.position.z + d[2] * ed.reach};
        const float off[3] = {ed.grab[0], ed.grab[1], 0};
        float wo[3];
        qrot(f.pose.orientation, off, wo);
        window_anchor_for(XrVector3f{held.x - wo[0], held.y - wo[1], held.z - wo[2]}, head);
    } else if (ed.aimed[h]) {
        const float x = std::fabs(ed.hit[h][0]), y = std::fabs(ed.hit[h][1]);
        if (ed.what == 2) {   // (a corner: the size, about the middle)
            int w = (int)std::lround(2 * x * 100 / 5) * 5, hh = (int)std::lround(2 * y * 100 / 5) * 5;
            if (opts.vr_window_169) w = std::max(w, (int)std::lround(2 * y * 16 / 9 * 100 / 5) * 5);
            w = std::clamp(w, 20, 300), hh = std::clamp(hh, 10, 200);
            if (w != opts.vr_window_width || (!opts.vr_window_169 && hh != opts.vr_window_height))
                opts.vr_window_width = w, opts.vr_window_height = opts.vr_window_169 ? opts.vr_window_height : hh, ed.changed = true;
        } else {              // (a rounding dot: along its diagonal, the corners' roundness)
            const float k = 0.5f * (x / (0.5f * f.w) + y / (0.5f * f.h));
            const int r = (int)std::lround(window_round_at(f, k) * 100 / 5) * 5;
            if (r != opts.vr_window_round) opts.vr_window_round = r, ed.changed = true;
        }
    }
    window.frame = window_frame_now();   // (this frame's at once)
}

/// Move mode's lasers (one a hand, to where it meets the frame's plane, else 1.5 m), as the VR menu's: how many
int window_edit_rays(XrCompositionLayerQuad rays[2], const XrPosef& head) {
    if (!window.edit.on) return 0;
    int n = 0;
    for (int h = 0; h < 2; h++) {
        if (!ctl.now[h].valid) continue;
        PanelHand p{};
        p.pointing = true;
        p.distance = window.edit.aimed[h] ? std::min(window.edit.along[h], 8.0f) : 1.5f;
        if (panel_ray(h, p, head, rays[n])) n++;
    }
    return n;
}

/// The Window view's layers for a frame: one projection layer (two views), or with RX_VR_WINDOW_LAYER=quad one quad per eye
struct WindowLayers {
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2];
    XrCompositionLayerQuad quads[2];
    const XrCompositionLayerBaseHeader* list[2]{};
    int n = 0;
};
WindowLayers window_last;   // (the last frame's layers sent: window_last_layers)

/// The last frame's layers again, when this frame's could not be drawn (its swapchain image as last released, its views as
/// they were): so a headset frame always carries the window once it has been shown. How many (0: never shown yet)
int window_last_layers(WindowLayers& out) {
    const WindowLayers& last = window_last;
    if (!last.n || !window.art.chain) return out.n = 0;
    out = last;   // (its pointers re-pointed at out's own: the saved ones point into the frame that sent it, 22 Nov: one
                  // of them, read as the quads, sent a layer of type 0, XR_ERROR_LAYER_INVALID)
    if (last.n == 1) {
        out.proj.views = out.views;
        out.list[0] = (const XrCompositionLayerBaseHeader*)&out.proj;
    } else {
        for (int e = 0; e < 2; e++) out.list[e] = (const XrCompositionLayerBaseHeader*)&out.quads[e];
    }
    static int told;
    if (told++ < 5) logf("vr: window: this frame's window not drawn: the last one sent again");
    return out.n;
}

/// The Window view's layers for this frame, in the room's space (out.n: 0 none). k: the frame's slot (a gameplay frame, or a
/// flat one); head: the head in the room, head_tracked its position tracked; eye_view: the eyes from the head (xrLocateViews in
/// the head's space, null when not located this frame)
int window_layers(int k, const XrPosef& head, bool head_tracked, const XrView* eye_view, WindowLayers& out) {
    out.n = 0;
    window.frames++;
    window_ensure_placed(head);
    // the eyes in the room: the head's pose and each eye's place from it (the runtime's own, IPD and canted displays included)
    if (head_tracked) {
        for (int e = 0; e < 2; e++) {
            float from_head[3] = {e ? 0.5f * s.ipd : -0.5f * s.ipd, 0, 0};
            if (eye_view) from_head[0] = eye_view[e].pose.position.x, from_head[1] = eye_view[e].pose.position.y,
                          from_head[2] = eye_view[e].pose.position.z;
            float d[3];
            qrot(head.orientation, from_head, d);
            window.eye[e] = {head.position.x + d[0], head.position.y + d[1], head.position.z + d[2]};
            window.eye_q[e] = eye_view ? qmul(head.orientation, eye_view[e].pose.orientation) : head.orientation;
            if (eye_view) window.fov[e] = eye_view[e].fov;
        }
        window.eye_known = true;
        if (window.lost) window.lost = false, logf("vr: window: the head's position tracked again");
    } else if (!window.lost) {   // (the view held where the eyes were last tracked)
        window.lost = true;
        logf("vr: window: the head's position not tracked: the view through the window held");
    }
    if (!window.eye_known) return window_last_layers(out);
    WindowFrustum fr[2];
    bool front = true;
    for (int e = 0; e < 2; e++) front = window_frustum(window.frame, window.eye[e], &fr[e]) && front;
    if (front != !window.behind) {
        window.behind = !front;
        logf(front ? "vr: window: in front of the window again"
                   : "vr: window: an eye at or behind the window's plane: the window faded out, the room or dark shown, frames still sent");
    }
    if (window.frames % 900 == 1)   // (now and then: the eyes through the frame, and their own FOVs)
        logf("vr: window: eyes at %.2f %.2f %.2f and %.2f %.2f %.2f m from the frame (x right, y up, z out); left eye's FOV "
             "%.0f %.0f %.0f %.0f deg, right's %.0f %.0f %.0f %.0f (left, right, up, down)",
             fr[0].e[0], fr[0].e[1], fr[0].e[2], fr[1].e[0], fr[1].e[1], fr[1].e[2], window.fov[0].angleLeft * 57.2958f,
             window.fov[0].angleRight * 57.2958f, window.fov[0].angleUp * 57.2958f, window.fov[0].angleDown * 57.2958f,
             window.fov[1].angleLeft * 57.2958f, window.fov[1].angleRight * 57.2958f, window.fov[1].angleUp * 57.2958f,
             window.fov[1].angleDown * 57.2958f);
    // (an eye nearing the frame's plane: the window fades out over its last kWindowFadeNear, and is gone at or behind it;
    // the layer is still sent every frame, beside the frame and behind the plane transparent: a headset frame with no layer
    // at all, as this sent before, made WiVRn's frame loop spin and the headset go black, 22 Nov)
    const float nearest = std::min(fr[0].e[2], fr[1].e[2]);
    window.fade = std::clamp((nearest - kWindowNear) / kWindowFadeNear, 0.0f, 1.0f);
    // each eye's view: the projection layer's its own FOV and turn; the quads' the off-axis view through the corners
    const bool as_quads = window_as_quads();
    if (!front && as_quads) return window_last_layers(out);   // (the quads: not drawn from behind; the last frame's again)
    WindowEyeView ev[2];
    for (int e = 0; e < 2; e++) {
        WindowEyeView& v = ev[e];
        for (int i = 0; i < 3; i++) v.e[i] = fr[e].e[i];
        const XrQuaternionf to_frame = qmul(qinv(window.frame.pose.orientation), window.eye_q[e]);
        for (int i = 0; i < 3; i++) {
            const float unit[3] = {i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
            if (as_quads) for (int j = 0; j < 3; j++) v.axis[i][j] = unit[j];
            else qrot(to_frame, unit, v.axis[i]);
        }
        if (as_quads) v.e[2] = std::max(v.e[2], kWindowNear), v.l = fr[e].l, v.r = fr[e].r, v.b = fr[e].b, v.t = fr[e].t;
        else v.l = std::tan(window.fov[e].angleLeft), v.r = std::tan(window.fov[e].angleRight),
             v.b = std::tan(window.fov[e].angleDown), v.t = std::tan(window.fov[e].angleUp);
    }
    const bool test = opts.vr_window_test != 0 && !s.slots[k].head_locked;   // (a flat frame: always its own picture)
    if (!(test ? window_draw(ev) : window_game_draw(k, ev))) return window_last_layers(out);   // (not drawn now: the last again)
    // (the alpha flag only where something must show through: the projection's room beside the frame, the test scene's hole)
    const bool room = window.room != kRoomDark;
    const XrCompositionLayerFlags flags =
        (as_quads ? test : (room || test)) ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
    if (!as_quads) {   // (each view where its eye was drawn from, at its own FOV)
        for (int e = 0; e < 2; e++) {
            XrCompositionLayerProjectionView& v = out.views[e];
            v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            v.pose = {window.eye_q[e], window.eye[e]};
            v.fov = window.fov[e];
            v.subImage.swapchain = window.art.chain;
            v.subImage.imageRect.offset = {(int32_t)(e * window.px_w), 0};
            v.subImage.imageRect.extent = {(int32_t)window.px_w, (int32_t)window.px_h};
        }
        out.proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        out.proj.layerFlags = flags;
        out.proj.space = s.local;
        out.proj.viewCount = 2;
        out.proj.views = out.views;
        out.list[0] = (const XrCompositionLayerBaseHeader*)&out.proj;
        out.n = 1;
    } else {
        for (int e = 0; e < 2; e++) {
            XrCompositionLayerQuad& q = out.quads[e];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = flags;
            q.space = s.local;
            q.eyeVisibility = e ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
            q.subImage.swapchain = window.art.chain;
            q.subImage.imageRect.offset = {(int32_t)(e * window.px_w), 0};
            q.subImage.imageRect.extent = {(int32_t)window.px_w, (int32_t)window.px_h};
            q.pose = window.frame.pose;
            q.size = {window.frame.w, window.frame.h};
            out.list[e] = (const XrCompositionLayerBaseHeader*)&q;
        }
        out.n = 2;
    }
    {   // (the layers sent, as they change: for a runtime's bug report)
        char said[240];
        snprintf(said, sizeof said, "%s, each eye %ux%u of one %ux%u swapchain, layer flags %#llx, blend %s, %s, %s",
                 as_quads ? "two quads (one per eye) at the frame" : "one projection layer (each eye's own pose and FOV)",
                 window.px_w, window.px_h, 2 * window.px_w, window.px_h, (unsigned long long)flags,
                 window.room == kRoomAlpha ? "alpha" : "opaque", window.room == kRoomFb ? "under it XR_FB_passthrough" : "no underlay",
                 test ? "the test scene" : s.slots[k].head_locked ? "a flat frame" : "the game");
        if (window.sent_said != said) window.sent_said = said, logf("vr: window: sent %s", said);
    }
    window_last = out;   // (kept: sent again on a frame whose own could not be drawn)
    return out.n;
}

/// The frame's room: decided once a frame (before window_underlay and the frame's end). Off the Window view: dark, and
/// passthrough paused
void window_room_frame() {
    const bool on = window_view();
    if (!on) window.was_on = false;
    int room = on ? window_room_want() : kRoomDark;
    if (room == kRoomFb && !window_passthrough_on()) room = window_room_want();   // (its creation failed: the next way)
    if (room != kRoomFb) window_passthrough_off();
    window.room = room;
    if (on && room != window.room_said) {
        logf("vr: window: around the window: %s%s", window_room_said(room),
             opts.vr_window_room && room == kRoomDark ? " (this runtime offers no room)" : "");
        window.room_said = room, window.end_said = 0;
    }
    if (!on) window.room_said = -1;
}

/// The passthrough layer to go under every other this frame (XR_FB_passthrough's room), false: none
bool window_underlay(XrCompositionLayerPassthroughFB& out) {
    if (window.room != kRoomFb || !window.pt_running) return false;
    out = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    out.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out.layerHandle = window.pt_layer;
    return true;
}

/// The frame's blend mode: alpha blend while the room is shown that way, else opaque
XrEnvironmentBlendMode window_blend_mode() {
    return window.room == kRoomAlpha ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
}

/// What the frame's end said (r), with the room it had: the first few logged; refused with the room, that way given up
void window_frame_ended(XrResult r) {
    if (window.room == kRoomDark && XR_SUCCEEDED(r)) return;
    if (window.end_said < 3 || XR_FAILED(r)) {
        window.end_said++;
        logf("vr: window: a frame ended with %s around the window: %d%s", window_room_said(window.room), (int)r,
             XR_FAILED(r) && window.room != kRoomDark ? " (refused: dark for 2 s, then that way again)" : "");
    }
    if (XR_FAILED(r) && window.room != kRoomDark) {   // (one refusal is not the room's fault for good: rested, then retried)
        window.refused_at = std::chrono::steady_clock::now();
        int& n = window.room == kRoomAlpha ? window.alpha_refused : window.fb_refused;
        if (++n >= kWindowRefusals) {
            logf("vr: window: %s refused %d times: given up for the run", window_room_said(window.room), n);
            if (window.room == kRoomAlpha) window.alpha_failed = true;
            else window.fb_failed = true;
        }
        if (window.room == kRoomFb) window_passthrough_off();
    }
}
