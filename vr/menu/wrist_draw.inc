// Hookshot VR: the wrist check HUD in the headset (the game side, vr/menu/wrist_host.c, sends what it shows).
//
// Included by vr/headset/vk_vr.cpp after vr/comfort/vignette_layer.inc (it uses the headset's helpers: barrier, memory_type,
// cmd, swap_take, headset_submit, s, ctl, flat_screen, panel, opts). Not compiled on its own.
// Turn the back of either wrist to the face (where a watch face sits), within about 40 cm, and hold it there half a second:
// a small panel shows on that wrist, and fades out when the arm drops. Never in a fight: not with the sword, the shield or
// the bow out, a VR hand holding the sword or a Deku Stick, locked on, hurt in the last 3 s, in an event (the game side's
// block bits), nor while either hand moves fast, the VR or radial menu is open, or the eyes show a flat frame (here).
// What it shows is the canvas board "Wrist check HUD" (approved 22 Nov): the user's leather (the VR menu's frame art,
// assets/vr/panel_frame.png, its middle) in a copper rim; the hearts by quarters, ten to a row; a magic bar once magic is
// owned; the rupees "N / wallet"; in a dungeon its small keys with their count, and the boss key, the map and the compass it
// has, dimmed until found. Icons: the game's own (the rupee, keys, map, compass: vk_vr_icons.h, from the player's files,
// vr/tools/generate/icons.py), else drawn shapes as the board's; hearts and the bar are drawn. Digits: the VR menu's font
// (Libertinus Serif Semibold, rasterized with Dear ImGui's stb_truetype).
// Full adds the minimap under them: the game's own map picture of the area or of the dungeon floor Link is on (the map
// menu's, vk_vr_wrist_maps.h: vr/tools/generate/wrist_maps.py, from the player's files) with Link's arrow where he is and
// facing his way; its frame sized to each map with margins, never cropped or stretched. No map to show (a dungeon before its
// map is found, a house, a grotto, the Lost Woods' fog): the Simple panel.
// Drawn on the CPU like the radial menu (vr/menu/radial_draw.inc) into its own swapchain image, only when what it shows
// changes (the arrow: when Link moves a pixel or turns, at most 25 times a second, over the panel kept as it was), and its
// fade as a few recomposed steps (the core quad layer has no alpha of its own). Shown as a quad layer in the room, at the
// wrist's predicted pose each frame, turned to face the eyes. VR menu, General, Headset, Wrist HUD: Off, Simple, Full; Wrist
// size: Small, Medium, Large (the default). Third person too: at the controller (no hand drawn there), the same rules.
// RX_VR_WRIST_TEST=1 (a test): the panel shown all the time in front of the head, with sample values when the game sends
// none: the fake headset's layer count shows it, headless.

// ---- what it shows, and the gesture: no headset or GPU needed (vr/tools/test.py check_wrist_draw compiles from here) ------
constexpr int kWristW = 480, kWristH = 560;   // (the panel's image: the board's 400 units wide; its height by what it shows)
constexpr float kWristS = kWristW / 400.0f;   // (pixels a board unit)
constexpr float kWristWidths[3] = {.085f, .11f, .14f};   // (the panel's width on the wrist, metres: VR menu Wrist size)
constexpr float kWristMapW = 320, kWristMapH = 220;   // (the minimap's largest picture, board units: wrist_maps.py MAX_W, MAX_H)
enum { kWristBossKey = 1, kWristMap = 2, kWristCompass = 4 };   // (owned; the dungeon's has: the same << 4, and 128 small keys)
struct WristState {
    int shown = 0;   // (0: no save loaded, nothing to show)
    int life = 0, max_life = 0, mp = 0, mp_max = 0, rupees = 0, wallet = 200, keys = -1, dungeon = 0;
    int block = 0;   // (the game side's reasons not to show it now: not drawn)
    int mode = 1;    // (Wrist HUD: 1 Simple, 2 Full)
    uint32_t banc = 0, flags = 0;   // (the map's name hash; the asked map flags set: wrist_flags)
    int map = -1;    // (Full: the minimap's picture, art::wrist_maps; -1 none: wrist_map_pick)
    bool same_look(const WristState& o) const {
        return shown == o.shown && life == o.life && max_life == o.max_life && mp == o.mp && mp_max == o.mp_max &&
               rupees == o.rupees && wallet == o.wallet && keys == o.keys && dungeon == o.dungeon && mode == o.mode &&
               map == o.map;
    }
};
struct WristLink {   // (Link in the game world: metres; facing: radians, atan2 of his forward's x and z)
    bool ok = false;
    float x = 0, y = 0, z = 0, facing = 0;
};
std::mutex wrist_mu;
WristState wrist_in;   // (wrist_mu) as the game side last sent it
WristLink wrist_link_in;
std::chrono::steady_clock::time_point wrist_at{};

void wrist_set(const int32_t* d, int n) {
    std::lock_guard lk(wrist_mu);
    WristState st;
    WristLink l;
    if (d && n >= 10 && d[0] == 1) {
        st.shown = 1;
        st.max_life = std::clamp(d[2], 4, 400), st.life = std::clamp(d[1], 0, st.max_life);
        st.mp_max = std::clamp(d[4], 0, 1000), st.mp = std::clamp(d[3], 0, st.mp_max);
        st.rupees = std::clamp(d[5], 0, 9999), st.wallet = std::clamp(d[6], 0, 9999);
        st.keys = std::clamp(d[7], -1, 99), st.dungeon = d[7] < 0 ? 0 : d[8] & 0xff;
    }
    st.block = d && n >= 10 ? d[9] : 0;
    if (d && n >= 17 && st.shown) {
        st.banc = (uint32_t)d[10], st.flags = (uint32_t)d[16];
        l.ok = d[15] != 0, l.x = d[11] * .001f, l.y = d[12] * .001f, l.z = d[13] * .001f, l.facing = d[14] * (3.14159265f / 18000);
    }
    wrist_in = st, wrist_link_in = l;
    wrist_at = std::chrono::steady_clock::now();
}

// ---- the minimap's picture: art::wrist_maps (vk_vr_wrist_maps.h) ----
/// The Bool flags the map pictures are shown by, each once, in the table's order (the game side reads them: bit k of
/// WristState::flags is the k-th): how many, at most max
int wrist_flags(uint32_t* out, int max) {
    int n = 0;
    for (int i = 0; i < art::wrist_map_count; i++) {
        const uint32_t f = art::wrist_maps[i].flag;
        bool seen = !f;
        for (int k = 0; k < n && !seen; k++) seen = out[k] == f;
        if (!seen && n < max) out[n++] = f;
    }
    return n;
}
/// Full's picture for now (-1: none, the Simple panel): the area's, or the dungeon floor's by Link's height once the
/// dungeon's map is found; of an area's pictures the last whose flag is set
int wrist_map_pick(const WristState& st, const WristLink& l) {
    if (st.mode != 2 || !st.shown || !st.banc || !l.ok) return -1;
    if (st.keys >= 0 && !(st.dungeon & kWristMap)) return -1;   // (a dungeon before its map is found)
    uint32_t flags[32];
    const int n = wrist_flags(flags, 32);
    int pick = -1;
    for (int i = 0; i < art::wrist_map_count; i++) {
        const art::WristMap& m = art::wrist_maps[i];
        if (m.banc != st.banc || l.y < m.y0 || l.y >= m.y1 || m.w <= 0 || m.h <= 0) continue;
        if (m.flag) {
            int k = 0;
            while (k < n && flags[k] != m.flag) k++;
            if (k >= n || !((st.flags >> k) & 1)) continue;
        }
        pick = i;
    }
    return pick;
}

// The gesture, one headset frame, for one wrist: the eyes, the head's forward, the panel's place on the wrist and the way the
// back of the wrist faces (the left grip's -x, the right's +x: OpenXR's grip +x is the way the left palm faces), the hands'
// speeds (m/s) and that hand's spin (rad/s), all in the room's space
struct WristLook {
    bool tracked = false;
    float eye[3]{}, forward[3]{}, at[3]{}, face[3]{};
    float speed[2]{}, spin = 0;
};
struct WristGesture {
    float held = 0, lost = 0;   // (s the pose has held; s it has been lost while shown)
    bool on = false;
    float alpha = 0;
};
constexpr float kWristNear = .40f, kWristNearStay = .45f;   // (m from the eyes, to show; to stay)
constexpr float kWristFacing = 35, kWristFacingStay = 45;   // (degrees between the back of the wrist and the eyes)
constexpr float kWristInView = 30, kWristInViewStay = 40;   // (degrees between the head's forward and the wrist)
constexpr float kWristFast = .6f, kWristFastStay = .9f;     // (m/s: either hand faster is no wrist check)
constexpr float kWristSpin = 3, kWristSpinStay = 4.5f;      // (rad/s: the wrist's hand turning)
constexpr float kWristHold = .5f, kWristLose = .2f;         // (s held to show; s lost to hide)
constexpr float kWristFadeIn = .12f, kWristFadeOut = .25f;  // (s)

/// Whether the wrist is turned to the face now (thresholds: to show, or the looser ones to stay)
bool wrist_pose_holds(const WristLook& w, bool stay) {
    if (!w.tracked) return false;
    float d[3], dn = 0, fn = 0, pn = 0, look = 0, face = 0;
    for (int k = 0; k < 3; k++) d[k] = w.eye[k] - w.at[k], dn += d[k] * d[k], fn += w.forward[k] * w.forward[k], pn += w.face[k] * w.face[k];
    dn = std::sqrt(dn), fn = std::sqrt(fn), pn = std::sqrt(pn);
    if (!(dn > 1e-3f) || !(fn > 1e-3f) || !(pn > 1e-3f)) return false;
    for (int k = 0; k < 3; k++) face += w.face[k] * d[k], look -= w.forward[k] * d[k];
    face /= dn * pn, look /= dn * fn;
    const float deg = 3.14159265f / 180;
    return dn < (stay ? kWristNearStay : kWristNear) && face > std::cos((stay ? kWristFacingStay : kWristFacing) * deg) &&
           look > std::cos((stay ? kWristInViewStay : kWristInView) * deg) &&
           std::max(w.speed[0], w.speed[1]) < (stay ? kWristFastStay : kWristFast) && w.spin < (stay ? kWristSpinStay : kWristSpin);
}
/// One headset frame of the gesture (dt: s since the last): the panel's alpha now (0: hidden). Blocked hides it at once
float wrist_gesture(WristGesture& g, const WristLook& w, bool blocked, float dt) {
    dt = std::clamp(dt, 0.0f, .1f);
    if (blocked) g.on = false, g.held = g.lost = 0;
    else if (wrist_pose_holds(w, g.on)) {
        g.lost = 0;
        if (!g.on && (g.held += dt) >= kWristHold) g.on = true;
    } else {
        g.held = 0;
        if (g.on && (g.lost += dt) >= kWristLose) g.on = false, g.lost = 0;
    }
    g.alpha = g.on ? std::min(1.0f, g.alpha + dt / kWristFadeIn) : std::max(0.0f, g.alpha - dt / kWristFadeOut);
    return g.alpha;
}

// ---- drawing (premultiplied RGBA, sRGB values, in the board's units: x across 0..400, y down) ----
uint8_t* wrist_px = nullptr;            // (the panel's pixels being drawn: kWristW x kWristH)
const uint8_t* wrist_leather = nullptr; // (the leather behind it, opaque RGBA of the same size; null: plain)
struct WristCol { float r, g, b; };
constexpr WristCol wrist_hex(uint32_t c) { return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f}; }
WristCol wrist_mix(WristCol a, WristCol b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
void wrist_over(int x, int y, WristCol c, float a) {
    if (x < 0 || y < 0 || x >= kWristW || y >= kWristH || !(a > 0)) return;
    uint8_t* p = wrist_px + ((size_t)y * kWristW + x) * 4;
    a = std::min(a, 1.0f);
    const float k = 1 - a;
    p[0] = (uint8_t)std::clamp(c.r * a * 255 + p[0] * k, 0.0f, 255.0f);
    p[1] = (uint8_t)std::clamp(c.g * a * 255 + p[1] * k, 0.0f, 255.0f);
    p[2] = (uint8_t)std::clamp(c.b * a * 255 + p[2] * k, 0.0f, 255.0f);
    p[3] = (uint8_t)std::clamp(a * 255 + p[3] * k, 0.0f, 255.0f);
}
/// A shape by its signed distance (board units, < 0 inside) over the box x0..x1, y0..y1, in colour(y) at alpha a
template <class Sd, class Colour>
void wrist_shape(float x0, float y0, float x1, float y1, Sd sd, Colour colour, float a) {
    for (int y = std::max(0, (int)(y0 * kWristS) - 1); y <= std::min(kWristH - 1, (int)(y1 * kWristS) + 1); y++)
        for (int x = std::max(0, (int)(x0 * kWristS) - 1); x <= std::min(kWristW - 1, (int)(x1 * kWristS) + 1); x++) {
            const float px = (x + .5f) / kWristS, py = (y + .5f) / kWristS;
            const float cover = std::clamp(.5f - sd(px, py) * kWristS, 0.0f, 1.0f);
            if (cover > 0) wrist_over(x, y, colour(py), a * cover);
        }
}
template <class Sd>
void wrist_shape(float x0, float y0, float x1, float y1, Sd sd, WristCol c, float a) {
    wrist_shape(x0, y0, x1, y1, sd, [c](float) { return c; }, a);
}
float wrist_rrect_sd(float px, float py, float x0, float y0, float x1, float y1, float r) {
    const float qx = std::abs(px - (x0 + x1) / 2) - ((x1 - x0) / 2 - r), qy = std::abs(py - (y0 + y1) / 2) - ((y1 - y0) / 2 - r);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}
float wrist_segment_d(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, l = vx * vx + vy * vy;
    const float t = l > 0 ? std::clamp(((px - ax) * vx + (py - ay) * vy) / l, 0.0f, 1.0f) : 0;
    return std::hypot(px - ax - vx * t, py - ay - vy * t);
}
/// A closed polygon's signed distance (xy: n points)
float wrist_poly_sd(const float* xy, int n, float px, float py) {
    float d = 1e9f;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float ax = xy[2 * j], ay = xy[2 * j + 1], bx = xy[2 * i], by = xy[2 * i + 1];
        d = std::min(d, wrist_segment_d(px, py, ax, ay, bx, by));
        if ((ay > py) != (by > py) && px < ax + (bx - ax) * (py - ay) / (by - ay)) inside = !inside;
    }
    return inside ? -d : d;
}
/// The board's heart (its 30 x 27 path, cubic curves flattened), at x, y, its points into out (kWristHeartN pairs)
constexpr int kWristHeartN = 6 * 12;
void wrist_heart_points(float* out) {
    static const float c[6][8] = {{15, 25, 6, 18, 1, 13, 1, 8},     {1, 8, 1, 4, 4, 1, 8, 1},        {8, 1, 11, 1, 13.5f, 3, 15, 5.5f},
                                  {15, 5.5f, 16.5f, 3, 19, 1, 22, 1}, {22, 1, 26, 1, 29, 4, 29, 8}, {29, 8, 29, 13, 24, 18, 15, 25}};
    int k = 0;
    for (const auto& b : c)
        for (int i = 0; i < 12; i++) {
            const float t = i / 12.0f, u = 1 - t;
            out[k++] = u * u * u * b[0] + 3 * u * u * t * b[2] + 3 * u * t * t * b[4] + t * t * t * b[6];
            out[k++] = u * u * u * b[1] + 3 * u * u * t * b[3] + 3 * u * t * t * b[5] + t * t * t * b[7];
        }
}
/// A heart at x, y filled by quarters (0..4): the board's, its red part cut at 0, 9, 15, 21 or all 30 of its width
void wrist_heart(float x, float y, int quarters) {
    static float pts[kWristHeartN * 2];
    static bool made = false;
    if (!made) wrist_heart_points(pts), made = true;
    auto sd = [x, y](float px, float py) { return wrist_poly_sd(pts, kWristHeartN, px - x, py - y); };
    const float w = quarters <= 0 ? 0 : quarters >= 4 ? 30 : 3 + 6 * quarters;
    wrist_shape(x - 1, y - 1, x + 31, y + 28, [&](float px, float py) { return std::abs(sd(px, py)) - .75f; }, wrist_hex(0x8A5532), 1);
    wrist_shape(x, y, x + 30, y + 27, [&](float px, float py) { return sd(px, py) + .75f; }, wrist_hex(0x2E1A0E), 1);
    if (w <= 0) return;
    auto cut = [&](float px, float py) { return std::max(sd(px, py), px - (x + w)); };
    wrist_shape(x - 1, y - 1, x + w + 1, y + 28, [&](float px, float py) {
        return std::max(std::abs(sd(px, py)) - .75f, px - (x + w)); }, wrist_hex(0xF2BC8C), 1);
    wrist_shape(x, y, x + w, y + 27, [&](float px, float py) { return cut(px, py) + .75f; }, wrist_hex(0xD8453A), 1);
}

// ---- the game's icons (vk_vr_icons.h), bilinear: false when this build has none of that name ----
const unsigned char* wrist_icon(const char* name) {
    for (const art::Icon& i : art::icons)
        if (i.rgba && !std::strcmp(i.name, name)) return i.rgba;
    return nullptr;
}
bool wrist_blit(const char* name, float cx, float cy, float size, float alpha) {
    const unsigned char* src = wrist_icon(name);
    if (!src) return false;
    const int n = art::icon_size;
    const float px0 = (cx - size / 2) * kWristS, py0 = (cy - size / 2) * kWristS, ps = size * kWristS;
    for (int y = std::max(0, (int)py0); y < std::min(kWristH, (int)(py0 + ps) + 1); y++)
        for (int x = std::max(0, (int)px0); x < std::min(kWristW, (int)(px0 + ps) + 1); x++) {
            const float u = ((x + .5f) - px0) / ps * n - .5f, v = ((y + .5f) - py0) / ps * n - .5f;
            if (u < -1 || v < -1 || u > n || v > n) continue;
            const int x0 = std::clamp((int)std::floor(u), 0, n - 1), y0 = std::clamp((int)std::floor(v), 0, n - 1);
            const int x1 = std::min(x0 + 1, n - 1), y1 = std::min(y0 + 1, n - 1);
            const float fx = std::clamp(u - x0, 0.0f, 1.0f), fy = std::clamp(v - y0, 0.0f, 1.0f);
            float c[4];
            for (int k = 0; k < 4; k++) {
                const float a = src[(y0 * n + x0) * 4 + k] * (1 - fx) + src[(y0 * n + x1) * 4 + k] * fx;
                const float b = src[(y1 * n + x0) * 4 + k] * (1 - fx) + src[(y1 * n + x1) * 4 + k] * fx;
                c[k] = (a * (1 - fy) + b * fy) / 255.0f * alpha;
            }
            uint8_t* p = wrist_px + ((size_t)y * kWristW + x) * 4;
            const float k = 1 - c[3];
            for (int j = 0; j < 4; j++) p[j] = (uint8_t)std::clamp(c[j] * 255 + p[j] * k, 0.0f, 255.0f);
        }
    return true;
}

// ---- digits: the VR menu's font, each glyph rasterized once a size ----
const unsigned char* wrist_font_data();   // (the TTF: the build's bundled Libertinus Serif Semibold; null: no text)
struct WristGlyph {
    int w = 0, h = 0, x0 = 0, y0 = 0;
    float advance = 0;
    std::vector<unsigned char> a;
};
stbtt_fontinfo* wrist_font() {
    static stbtt_fontinfo info;
    static int ok = -1;
    if (ok < 0) {
        const unsigned char* data = wrist_font_data();
        ok = data && stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) ? 1 : 0;
    }
    return ok ? &info : nullptr;
}
const WristGlyph* wrist_glyph(int ch, int px) {
    static std::unordered_map<int, WristGlyph> cache;
    stbtt_fontinfo* f = wrist_font();
    if (!f || px <= 0 || px > 200) return nullptr;
    auto [it, fresh] = cache.try_emplace(ch * 256 + px);
    WristGlyph& g = it->second;
    if (fresh) {
        const float scale = stbtt_ScaleForMappingEmToPixels(f, (float)px);
        int adv = 0, lsb = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointHMetrics(f, ch, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(f, ch, scale, scale, &g.x0, &g.y0, &x1, &y1);
        g.w = std::max(0, x1 - g.x0), g.h = std::max(0, y1 - g.y0), g.advance = adv * scale;
        g.a.assign((size_t)g.w * g.h, 0);
        if (g.w && g.h) stbtt_MakeCodepointBitmap(f, g.a.data(), g.w, g.h, g.w, scale, scale, ch);
    }
    return &g;
}
/// The text's width in board units at this size (board units: the CSS font-size)
float wrist_text_width(const char* t, float size) {
    float w = 0;
    for (; *t; t++)
        if (const WristGlyph* g = wrist_glyph((unsigned char)*t, (int)std::lround(size * kWristS))) w += g->advance;
    return w / kWristS;
}
/// Text from x, its digits centred on cy (a shadow below it: the board's text-shadow 0 1 2, its alpha; 0 none)
void wrist_text(const char* t, float x, float cy, float size, WristCol c, float a, float shadow) {
    const int px = (int)std::lround(size * kWristS);
    const WristGlyph* zero = wrist_glyph('0', px);
    if (!zero) return;
    const float base = cy * kWristS - (zero->y0 + zero->h / 2.0f);   // (the baseline, pixels: the digits' middle on cy)
    for (int pass = shadow > 0 ? 0 : 1; pass < 2; pass++) {
        float pen = x * kWristS;
        const float dy = pass == 0 ? kWristS : 0;
        for (const char* p = t; *p; p++) {
            const WristGlyph* g = wrist_glyph((unsigned char)*p, px);
            if (!g) continue;
            const int ox = (int)std::lround(pen) + g->x0, oy = (int)std::lround(base + dy) + g->y0;
            for (int j = 0; j < g->h; j++)
                for (int i = 0; i < g->w; i++)
                    if (const unsigned char v = g->a[(size_t)j * g->w + i])
                        wrist_over(ox + i, oy + j, pass == 0 ? WristCol{0, 0, 0} : c, v / 255.0f * (pass == 0 ? shadow : a));
            pen += g->advance;
        }
    }
}

// ---- the drawn icons (the board's, where the game's are not in this build) ----
void wrist_drawn_rupee(float x, float y) {   // (20 x 30)
    static const float outer[] = {10, 1, 19, 9, 19, 21, 10, 29, 1, 21, 1, 9}, inner[] = {10, 6, 15, 10, 15, 20, 10, 24, 5, 20, 5, 10};
    auto so = [x, y](float px, float py) { return wrist_poly_sd(outer, 6, px - x, py - y); };
    wrist_shape(x - 1, y, x + 21, y + 30, [&](float px, float py) { return std::abs(so(px, py)) - .75f; }, wrist_hex(0xD9F2C4), 1);
    wrist_shape(x, y, x + 20, y + 30, [&](float px, float py) { return so(px, py) + .75f; }, wrist_hex(0x4FAE4A), 1);
    wrist_shape(x, y, x + 20, y + 30, [x, y](float px, float py) { return wrist_poly_sd(inner, 6, px - x, py - y); },
                wrist_hex(0x7DD46F), .7f);
}
/// A key (the small one 14 x 28: ring radius 4.5, stroke 2.5; the boss key 20 x 30: 6 and 3), in colour c
void wrist_drawn_key(float x, float y, bool boss, WristCol c, float a) {
    const float cx = x + (boss ? 10 : 7), cy = y + (boss ? 7 : 6), r = boss ? 6 : 4.5f, w = (boss ? 3 : 2.5f) / 2;
    const float top = boss ? 13 : 10.5f, bottom = boss ? 28 : 26, b1 = boss ? 21 : 20, b2 = boss ? 26 : 24, l1 = boss ? 6 : 4, l2 = boss ? 5 : 3;
    wrist_shape(x - 2, y - 2, x + (boss ? 22 : 16), y + 31, [=](float px, float py) {
        float d = std::abs(std::hypot(px - cx, py - cy) - r) - w;
        d = std::min(d, wrist_segment_d(px, py, cx, y + top, cx, y + bottom) - w);
        d = std::min(d, wrist_segment_d(px, py, cx, y + b1, cx + l1, y + b1) - w);
        return std::min(d, wrist_segment_d(px, py, cx, y + b2, cx + l2, y + b2) - w);
    }, c, a);
}
void wrist_drawn_map(float x, float y, float a) {   // (26 x 24)
    static const float edge[] = {2, 4, 9, 1, 17, 4, 24, 1, 24, 20, 17, 23, 9, 20, 2, 23};
    wrist_shape(x - 1, y - 1, x + 27, y + 25, [x, y](float px, float py) {
        float d = std::abs(wrist_poly_sd(edge, 8, px - x, py - y)) - 1;
        d = std::min(d, wrist_segment_d(px - x, py - y, 9, 1, 9, 20) - 1);
        return std::min(d, wrist_segment_d(px - x, py - y, 17, 4, 17, 23) - 1);
    }, wrist_hex(0xE2D4B5), a);
}
void wrist_drawn_compass(float x, float y, float a) {   // (24 x 24)
    static const float needle[] = {12, 4, 15, 12, 12, 20, 9, 12};
    wrist_shape(x - 1, y - 1, x + 25, y + 25, [x, y](float px, float py) { return std::abs(std::hypot(px - x - 12, py - y - 12) - 10) - 1; },
                wrist_hex(0xE2D4B5), a);
    wrist_shape(x, y, x + 24, y + 24, [x, y](float px, float py) { return wrist_poly_sd(needle, 4, px - x, py - y); }, wrist_hex(0xD8453A), a);
}

// ---- the panel ----
constexpr int kWristSingleMagic = 480;   // (MPMax with single magic: the game counts tenths, headless 22 Nov "mp 390/480")
constexpr float kWristRim = 7, kWristPadX = 26, kWristPadY = 22, kWristGap = 16, kWristRow = 30;
constexpr float kWristMapPad = 10, kWristMapEdge = 3;   // (the minimap's margin round its picture; its copper edge)
/// The minimap's picture's size on the panel (board units): the map fitted into kWristMapW x kWristMapH, never stretched
bool wrist_map_size(const WristState& st, float* w, float* h) {
    if (st.map < 0 || st.map >= art::wrist_map_count) return false;
    const art::WristMap& m = art::wrist_maps[st.map];
    const float k = std::min(kWristMapW / m.w, kWristMapH / m.h);
    *w = m.w * k, *h = m.h * k;
    return true;
}
/// The minimap's frame's height under the rows (board units); 0: no map to show, the Simple layout
float wrist_minimap(const WristState& st) {
    float w, h;
    return wrist_map_size(st, &w, &h) ? h + 2 * (kWristMapPad + kWristMapEdge) : 0;
}
int wrist_heart_rows(const WristState& st) { return (std::clamp((st.max_life + 3) / 4, 1, 20) + 9) / 10; }
/// The panel's height (board units) for what it shows: the hearts' rows, the magic bar's, the rupees' and keys' row
float wrist_height(const WristState& st) {
    const int rows = wrist_heart_rows(st);
    const float minimap = wrist_minimap(st);
    return 2 * (kWristRim + kWristPadY) + rows * 27 + (rows - 1) * 4 + (st.mp_max > 0 ? 16 + kWristGap : 0) + kWristGap + kWristRow +
           (minimap > 0 ? kWristGap + minimap : 0);
}

/// The rupee icon by the wallet, as the game's HUD colours it: green (the child's), blue (the adult's), red (the giant's)
const char* wrist_rupee_icon(int wallet) { return wallet >= 1000 ? "Obj_Rupee_Red" : wallet >= 500 ? "Obj_Rupee_Blue" : "Obj_Rupee_Green"; }

// (Full: the minimap's picture, as wrist_compose draws it: its pixels, premultiplied RGBA of art::wrist_maps[st.map]'s size
// (the headset's part unpacks them); where it went, board units, for Link's arrow)
const uint8_t* wrist_map_rgba = nullptr;
struct WristRect { float x = 0, y = 0, w = 0, h = 0; } wrist_map_at;
/// A picture (premultiplied RGBA, w x h) over the box at x, y, bw x bh board units: bilinear
void wrist_picture(const uint8_t* src, int w, int h, float x, float y, float bw, float bh) {
    const int px0 = std::max(0, (int)(x * kWristS)), py0 = std::max(0, (int)(y * kWristS));
    const int px1 = std::min(kWristW, (int)std::ceil((x + bw) * kWristS)), py1 = std::min(kWristH, (int)std::ceil((y + bh) * kWristS));
    for (int py = py0; py < py1; py++)
        for (int px = px0; px < px1; px++) {
            const float u = ((px + .5f) / kWristS - x) / bw * w - .5f, v = ((py + .5f) / kWristS - y) / bh * h - .5f;
            const int x0 = std::clamp((int)std::floor(u), 0, w - 1), y0 = std::clamp((int)std::floor(v), 0, h - 1);
            const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            const float fx = std::clamp(u - x0, 0.0f, 1.0f), fy = std::clamp(v - y0, 0.0f, 1.0f);
            uint8_t* p = wrist_px + ((size_t)py * kWristW + px) * 4;
            float c[4];
            for (int k = 0; k < 4; k++) {
                const float a = src[((size_t)y0 * w + x0) * 4 + k] * (1 - fx) + src[((size_t)y0 * w + x1) * 4 + k] * fx;
                const float b = src[((size_t)y1 * w + x0) * 4 + k] * (1 - fx) + src[((size_t)y1 * w + x1) * 4 + k] * fx;
                c[k] = (a * (1 - fy) + b * fy) / 255.0f;
            }
            const float k = 1 - c[3];
            for (int j = 0; j < 4; j++) p[j] = (uint8_t)std::clamp(c[j] * 255 + p[j] * k, 0.0f, 255.0f);
        }
}
/// Link's arrow on the minimap (the board's: a gold arrowhead, a dark edge), at his place on art::wrist_maps[map] and turned
/// his way (kept inside the picture when he is past its edge), over wrist_px at alpha a; false: no map or no Link
bool wrist_arrow(int map, const WristLink& l, float a) {
    if (map < 0 || map >= art::wrist_map_count || !l.ok || wrist_map_at.w <= 0) return false;
    const art::WristMap& m = art::wrist_maps[map];
    const float u = std::clamp((l.x - m.x0) / (m.x1 - m.x0), 0.0f, 1.0f), v = std::clamp((l.z - m.z0) / (m.z1 - m.z0), 0.0f, 1.0f);
    const float cx = wrist_map_at.x + u * wrist_map_at.w, cy = wrist_map_at.y + v * wrist_map_at.h;
    // (his forward (sin, cos of facing) in x and z is the picture's x and y: the arrow's tip, up at rest, turned onto it)
    const float sn = std::sin(l.facing), cs = std::cos(l.facing);
    static const float shape[] = {0, -10, 8, 9, 0, 4, -8, 9};
    float pts[8];
    for (int i = 0; i < 4; i++) {
        const float x = shape[2 * i], y = shape[2 * i + 1];   // (local: tip at -y; -y goes to (sn, cs))
        pts[2 * i] = cx - x * cs - y * sn, pts[2 * i + 1] = cy + x * sn - y * cs;
    }
    auto sd = [&](float px, float py) { return wrist_poly_sd(pts, 4, px, py); };
    wrist_shape(cx - 11, cy - 11, cx + 11, cy + 11, [&](float px, float py) { return std::abs(sd(px, py)) - .9f; }, wrist_hex(0x3A1F12), a);
    wrist_shape(cx - 11, cy - 11, cx + 11, cy + 11, [&](float px, float py) { return sd(px, py) + .9f; }, wrist_hex(0xF2D24A), a);
    return true;
}
void wrist_dungeon_row(const WristState& st, float right, float cy);
void wrist_compose(const WristState& st) {
    std::memset(wrist_px, 0, (size_t)kWristW * kWristH * 4);
    if (!st.shown) return;
    const float H = wrist_height(st), W = 400;
    // (the copper rim: the board's gradient F2BC8C, A8683E at 45%, 6E3F22 at 80%, 3A1F12; corner 28)
    auto rim = [H](float py) {
        const float t = std::clamp(py / H, 0.0f, 1.0f);
        return t < .45f ? wrist_mix(wrist_hex(0xF2BC8C), wrist_hex(0xA8683E), t / .45f)
             : t < .8f  ? wrist_mix(wrist_hex(0xA8683E), wrist_hex(0x6E3F22), (t - .45f) / .35f)
                        : wrist_mix(wrist_hex(0x6E3F22), wrist_hex(0x3A1F12), (t - .8f) / .2f);
    };
    wrist_shape(0, 0, W, H, [H](float px, float py) { return wrist_rrect_sd(px, py, 0, 0, 400, H, 28); }, rim, 1);
    // (the leather: the frame art's middle, its glow at the top, its darkened edges and inner shadow, as the board's)
    const float ix0 = kWristRim, iy0 = kWristRim, ix1 = W - kWristRim, iy1 = H - kWristRim;
    auto inner = [=](float px, float py) { return wrist_rrect_sd(px, py, ix0, iy0, ix1, iy1, 22); };
    for (int y = (int)(iy0 * kWristS); y <= std::min(kWristH - 1, (int)(iy1 * kWristS)); y++)
        for (int x = (int)(ix0 * kWristS); x <= (int)(ix1 * kWristS) && x < kWristW; x++) {
            const float px = (x + .5f) / kWristS, py = (y + .5f) / kWristS, sd = inner(px, py);
            const float cover = std::clamp(.5f - sd * kWristS, 0.0f, 1.0f);
            if (cover <= 0) continue;
            WristCol c = wrist_hex(0x24150B);
            if (wrist_leather) {
                const uint8_t* l = wrist_leather + ((size_t)y * kWristW + x) * 4;
                c = {l[0] / 255.0f, l[1] / 255.0f, l[2] / 255.0f};
            }
            const float ex = (px - W / 2) / ((ix1 - ix0) / 2), ey = (py - (iy0 + iy1) / 2) / ((iy1 - iy0) / 2);
            const float glow = std::hypot(ex, (py - (iy0 + (iy1 - iy0) * .3f)) / ((iy1 - iy0) / 2));
            c = wrist_mix(c, wrist_hex(0xFFC896), .08f * std::clamp(1 - glow / 1.2f, 0.0f, 1.0f));
            c = wrist_mix(c, {0, 0, 0}, .55f * std::clamp((std::hypot(ex, ey) / 1.414f - .55f) / .45f, 0.0f, 1.0f));
            c = wrist_mix(c, {0, 0, 0}, .6f * std::pow(std::clamp(1 + sd / 12, 0.0f, 1.0f), 2.0f));   // (inset 0 0 24 .6)
            if (-sd < 2 && py < iy0 + 22) c = wrist_mix(c, wrist_hex(0xFFD2AA), .25f);                  // (inset 0 2 .25)
            if (-sd < 3 && py > iy1 - 22) c = wrist_mix(c, {0, 0, 0}, .5f);                              // (inset 0 -3 .5)
            wrist_over(x, y, c, cover);
        }
    float x = ix0 + kWristPadX, y = iy0 + kWristPadY;
    const float right = ix1 - kWristPadX;
    // (the hearts: ten a row, 30 x 27 with 4 between)
    const int hearts = std::clamp((st.max_life + 3) / 4, 1, 20);
    for (int i = 0; i < hearts; i++)
        wrist_heart(x + (i % 10) * 34, y + (i / 10) * 31, std::clamp(st.life - i * 4, 0, 4));
    y += wrist_heart_rows(st) * 31 - 4 + kWristGap;
    if (st.mp_max > 0) {   // (the magic bar: 200 wide, 340 with double magic; its copper edge 3, its green by MP)
        const float bw = st.mp_max > kWristSingleMagic * 3 / 2 ? 340 : 200, by = y;
        wrist_shape(x, by, x + bw, by + 16, [=](float px, float py) { return wrist_rrect_sd(px, py, x, by, x + bw, by + 16, 8); },
                    [by](float py) { return wrist_mix(wrist_hex(0xF2BC8C), wrist_hex(0x6E3F22), std::clamp((py - by) / 16, 0.0f, 1.0f)); }, 1);
        auto well = [=](float px, float py) { return wrist_rrect_sd(px, py, x + 3, by + 3, x + bw - 3, by + 13, 6); };
        wrist_shape(x + 3, by + 3, x + bw - 3, by + 13, well, wrist_hex(0x1A0F08), 1);
        const float fill = (bw - 6) * st.mp / std::max(1, st.mp_max);
        if (fill > 0)
            wrist_shape(x + 3, by + 3, x + 3 + fill, by + 13, [=](float px, float py) { return std::max(well(px, py), px - (x + 3 + fill)); },
                        [by](float py) { return wrist_mix(wrist_hex(0x8FD46A), wrist_hex(0x3E8A2C), std::clamp((py - by - 3) / 10, 0.0f, 1.0f)); }, 1);
        y += 16 + kWristGap;
    }
    // (the rupees, "N / wallet")
    const float cy = y + kWristRow / 2;
    if (!wrist_blit(wrist_rupee_icon(st.wallet), x + 10, cy, 34, 1)) wrist_drawn_rupee(x, cy - 15);
    char text[16];
    snprintf(text, sizeof text, "%d", st.rupees);
    float tx = x + 30;
    wrist_text(text, tx, cy, 26, wrist_hex(0xFFF0D8), 1, .8f);
    tx += wrist_text_width(text, 26) + 8;
    snprintf(text, sizeof text, "/ %d", st.wallet);
    wrist_text(text, tx, cy + 1, 18, wrist_hex(0xCDBE9C), 1, 0);
    if (st.keys >= 0) wrist_dungeon_row(st, right, cy);
    float mw, mh;
    if (!wrist_map_size(st, &mw, &mh) || !wrist_map_rgba) return;
    // (Full: the minimap, its frame sized to the map: the board's copper edge, a dark well, the picture)
    const float fw = mw + 2 * (kWristMapPad + kWristMapEdge), fh = mh + 2 * (kWristMapPad + kWristMapEdge);
    const float fx = (400 - fw) / 2, fy = cy + kWristRow / 2 + kWristGap;
    wrist_shape(fx, fy, fx + fw, fy + fh, [=](float px, float py) { return wrist_rrect_sd(px, py, fx, fy, fx + fw, fy + fh, 12); },
                [=](float py) { return wrist_mix(wrist_hex(0xB97A4E), wrist_hex(0x3A1F12), std::clamp((py - fy) / fh, 0.0f, 1.0f)); }, 1);
    const float wx0 = fx + kWristMapEdge, wy0 = fy + kWristMapEdge, wx1 = fx + fw - kWristMapEdge, wy1 = fy + fh - kWristMapEdge;
    wrist_shape(wx0, wy0, wx1, wy1, [=](float px, float py) {
        return wrist_rrect_sd(px, py, wx0, wy0, wx1, wy1, 10); }, wrist_hex(0x160E08), 1);
    wrist_map_at = {fx + kWristMapEdge + kWristMapPad, fy + kWristMapEdge + kWristMapPad, mw, mh};
    wrist_picture(wrist_map_rgba, art::wrist_maps[st.map].w, art::wrist_maps[st.map].h, wrist_map_at.x, wrist_map_at.y, mw, mh);
}
/// (the dungeon row's right side: its small keys and count, then the boss key, the map and the compass it has, dimmed until
/// found)
void wrist_dungeon_row(const WristState& st, float right, float cy) {
    const int has = st.dungeon >> 4, own = st.dungeon & 15;
    const bool keys = (has & 8) || st.keys > 0, boss = (has & kWristBossKey) || (own & kWristBossKey);
    const bool map = (has & kWristMap) || (own & kWristMap), compass = (has & kWristCompass) || (own & kWristCompass);
    char count[8];
    snprintf(count, sizeof count, "%d", st.keys);
    const float key_w = 14 + 4 + wrist_text_width(count, 22);
    float w = (keys ? key_w : 0) + (boss ? 20 : 0) + (map ? 26 : 0) + (compass ? 24 : 0);
    w += 12 * std::max(0, (int)keys + (int)boss + (int)map + (int)compass - 1);
    float gx = right - w;
    auto dim = [own](int bit) { return (own & bit) ? 1.0f : .3f; };
    if (keys) {
        if (!wrist_blit("Item_Dungeon_SmallKey", gx + 7, cy, 32, 1)) wrist_drawn_key(gx, cy - 14, false, wrist_hex(0xE3CB86), 1);
        wrist_text(count, gx + 18, cy, 22, wrist_hex(0xFFF0D8), 1, .8f);
        gx += key_w + 12;
    }
    if (boss) {
        if (!wrist_blit("Item_Dungeon_BossKey", gx + 10, cy, 34, dim(kWristBossKey)))
            wrist_drawn_key(gx, cy - 15, true, wrist_hex(0xE9A34A), dim(kWristBossKey));
        gx += 20 + 12;
    }
    if (map) {
        if (!wrist_blit("Item_Dungeon_Map", gx + 13, cy, 32, dim(kWristMap))) wrist_drawn_map(gx, cy - 12, dim(kWristMap));
        gx += 26 + 12;
    }
    if (compass && !wrist_blit("Item_Dungeon_Compass", gx + 12, cy, 32, dim(kWristCompass)))
        wrist_drawn_compass(gx, cy - 12, dim(kWristCompass));
}
// ---- the headset's part ----

const unsigned char* wrist_font_data() { return hookshot_font_heading; }

struct WristPanel {
    Eye eye;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* pixels = nullptr;            // (the staging buffer's, mapped)
    std::vector<uint8_t> base, leather;   // (the panel composed at full alpha; the leather under it)
    bool made = false, failed = false, composed = false;
    WristState shown;                     // (what base shows)
    int level = -1;                       // (the alpha step uploaded: 1..kWristSteps; -1 none)
    float height = 0;                     // (base's, board units)
    WristGesture gesture[2];              // (each wrist's: 0 left, 1 right)
    int hand = 0;                         // (the wrist it shows on)
    std::vector<uint8_t> map;             // (the minimap's picture unpacked: art::wrist_maps[map_at])
    int map_at = -1;
    int arrow[4] = {-1, 0, 0, 0};         // (the arrow uploaded: its map, pixel x and y, 5-degree step)
    std::chrono::steady_clock::time_point last{}, uploaded{};
} wrist;
constexpr int kWristSteps = 4;   // (the fade's steps: each one a recompose and an upload)

std::mutex wrist_draw_mu;   // (wrist_compose's globals: the headset's thread, and RX_VR_WRIST_SHOT's on the game's)
/// The leather under the panel, once
void wrist_leather_make() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    {   // (the leather: the frame art's middle third across, as the board shows it at 300%; none in this build: plain)
        uint32_t w = 0, h = 0;
        std::vector<unsigned char> art;
        if (panel_frame_pixels(&w, &h, art) && w > 3 && h > 3) {
            wrist.leather.assign((size_t)kWristW * kWristH * 4, 255);
            const float k = (w / 3.0f) / kWristW, sx0 = w / 2.0f - kWristW * k / 2, sy0 = h / 2.0f - kWristH * k / 2;
            for (int y = 0; y < kWristH; y++)
                for (int x = 0; x < kWristW; x++) {
                    const int u = std::clamp((int)(sx0 + (x + .5f) * k), 0, (int)w - 1), v = std::clamp((int)(sy0 + (y + .5f) * k), 0, (int)h - 1);
                    for (int c = 0; c < 3; c++) wrist.leather[((size_t)y * kWristW + x) * 4 + c] = art[((size_t)v * w + u) * 4 + c];
                }
        }
    }
}
/// Full's picture unpacked into wrist.map (false: not in this build, or damaged)
bool wrist_map_load(int i) {
    if (i == wrist.map_at) return !wrist.map.empty();
    wrist.map_at = i, wrist.map.clear();
    if (i < 0 || i >= art::wrist_map_count) return false;
    const art::WristMap& m = art::wrist_maps[i];
    wrist.map.resize((size_t)m.w * m.h * 4);
    if (ZSTD_decompress(wrist.map.data(), wrist.map.size(), m.zst, m.size) != wrist.map.size()) {
        logf("vr: wrist HUD: minimap picture %d would not unpack", i);
        wrist.map.clear();
        return false;
    }
    return true;
}
/// The panel for what is shown now, into wrist_px: the leather, the minimap's picture (and its arrow when l is given)
void wrist_draw(uint8_t* into, const WristState& st, const WristLink* l, float a) {
    wrist_px = into, wrist_leather = wrist.leather.empty() ? nullptr : wrist.leather.data();
    wrist_map_rgba = st.map >= 0 && wrist_map_load(st.map) ? wrist.map.data() : nullptr;
    wrist_map_at = {};
    wrist_compose(st);
    if (l) wrist_arrow(wrist_map_rgba ? st.map : -1, *l, a);
}

bool wrist_make() {
    if (wrist.made || wrist.failed) return wrist.made;
    wrist.failed = true;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    ci.width = kWristW, ci.height = kWristH;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &wrist.eye.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(wrist.eye.chain, 0, &n, nullptr);
    wrist.eye.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(wrist.eye.chain, n, &n, (XrSwapchainImageBaseHeader*)wrist.eye.images.data());
    wrist.eye.w = kWristW, wrist.eye.h = kWristH;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)kWristW * kWristH * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &wrist.staging) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev.device, wrist.staging, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev.device, &ai, nullptr, &wrist.memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev.device, wrist.staging, wrist.memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev.device, wrist.memory, 0, bi.size, 0, (void**)&wrist.pixels) != VK_SUCCESS) return false;
    wrist.base.assign((size_t)kWristW * kWristH * 4, 0);
    wrist_leather_make();
    wrist.made = true, wrist.failed = false;
    logf("vr: wrist HUD panel ready (%dx%d px, %s)", kWristW, kWristH, wrist.leather.empty() ? "a plain panel" : "the frame art's leather");
    return true;
}

bool wrist_upload() {
    uint32_t index = 0;
    SwapTake take;
    if (!swap_take(take, {wrist.eye.chain}, &index, "wrist HUD")) return false;
    if (!headset_record()) return swap_give(take), false;
    const VkImage img = wrist.eye.images[index].image;
    barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)kWristW, (uint32_t)kWristH, 1};
    vkCmdCopyBufferToImage(cmd, wrist.staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return headset_submit(take, VK_NULL_HANDLE, 0, "wrist HUD");
}

/// A wrist's look this frame (h: 0 left, 1 right), in the room's space: the grip with its saved wrist calibration (how the
/// real hand sits on the controller: vr/body/tracking.inc), in both views (third person without it: the left wrist, whose
/// calibration is the larger, missed the facing test, headset 22 Nov), the panel 8 cm back from the grip toward the elbow and
/// 3.5 cm out from the back of the wrist (where a watch face sits: the left grip's -x, the right's +x)
WristLook wrist_look(const XrPosef& head, int h) {
    WristLook w;
    const float ahead[3] = {0, 0, -1}, out[3] = {h ? 1.0f : -1.0f, 0, 0}, elbow[3] = {0, 0, 1};
    qrot(head.orientation, ahead, w.forward);
    w.eye[0] = head.position.x, w.eye[1] = head.position.y, w.eye[2] = head.position.z;
    if (!ctl.attached || !ctl.now[h].valid) return w;
    const HandState& hs = ctl.now[h];
    const float d2r = 3.14159265f / 360;   // (half angles)
    const int deg[3] = {h ? opts.vr_wrist_r_pitch : opts.vr_wrist_l_pitch, h ? opts.vr_wrist_r_yaw : opts.vr_wrist_l_yaw,
                        h ? opts.vr_wrist_r_roll : opts.vr_wrist_l_roll};
    const XrQuaternionf pitch{std::sin(deg[0] * d2r), 0, 0, std::cos(deg[0] * d2r)};
    const XrQuaternionf yaw{0, std::sin(deg[1] * d2r), 0, std::cos(deg[1] * d2r)};
    const XrQuaternionf roll{0, 0, std::sin(deg[2] * d2r), std::cos(deg[2] * d2r)};
    const XrQuaternionf q = qmul(hs.grip.orientation, qmul(roll, qmul(yaw, pitch)));
    float z[3];
    qrot(q, out, w.face);
    qrot(q, elbow, z);
    w.at[0] = hs.grip.position.x + z[0] * .08f + w.face[0] * .035f;
    w.at[1] = hs.grip.position.y + z[1] * .08f + w.face[1] * .035f;
    w.at[2] = hs.grip.position.z + z[2] * .08f + w.face[2] * .035f;
    for (int k = 0; k < 2; k++)
        w.speed[k] = ctl.now[k].valid ? std::sqrt(ctl.now[k].lin_vel[0] * ctl.now[k].lin_vel[0] + ctl.now[k].lin_vel[1] * ctl.now[k].lin_vel[1] +
                                                  ctl.now[k].lin_vel[2] * ctl.now[k].lin_vel[2]) : 0;
    w.spin = std::sqrt(hs.ang_vel[0] * hs.ang_vel[0] + hs.ang_vel[1] * hs.ang_vel[1] + hs.ang_vel[2] * hs.ang_vel[2]);
    w.tracked = true;
    return w;
}

/// A rotation whose +z points from at to the eye and whose +y is as near the head's up as it can be (upright as you see it)
XrQuaternionf wrist_facing(const float at[3], const XrPosef& head) {
    float z[3] = {head.position.x - at[0], head.position.y - at[1], head.position.z - at[2]}, up[3];
    const float upv[3] = {0, 1, 0};
    qrot(head.orientation, upv, up);
    float n = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(n > 1e-4f)) return head.orientation;
    for (float& v : z) v /= n;
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    n = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!(n > 1e-4f)) return head.orientation;
    for (float& v : x) v /= n;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const float t = x[0] + y[1] + z[2];
    if (t > 0) {
        const float s4 = std::sqrt(t + 1) * 2;
        return {(y[2] - z[1]) / s4, (z[0] - x[2]) / s4, (x[1] - y[0]) / s4, s4 / 4};
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const float s4 = std::sqrt(1 + x[0] - y[1] - z[2]) * 2;
        return {s4 / 4, (y[0] + x[1]) / s4, (z[0] + x[2]) / s4, (y[2] - z[1]) / s4};
    } else if (y[1] > z[2]) {
        const float s4 = std::sqrt(1 + y[1] - x[0] - z[2]) * 2;
        return {(y[0] + x[1]) / s4, s4 / 4, (z[1] + y[2]) / s4, (z[0] - x[2]) / s4};
    }
    const float s4 = std::sqrt(1 + z[2] - x[0] - y[1]) * 2;
    return {(z[0] + x[2]) / s4, (z[1] + y[2]) / s4, s4 / 4, (x[1] - y[0]) / s4};
}

/// (RX_VR_WRIST_SHOT=<folder>, a test: also without a headset) every 2 s on the game side's call, the panel as it would show
/// now, Full's minimap and arrow among it, written as <folder>/wrist.pam (RGBA, premultiplied): its picture, headless
void wrist_shot() {
    static const char* dir = vr_research_getenv("RX_VR_WRIST_SHOT");
    static std::chrono::steady_clock::time_point at{};
    static int told = 0;
    if (!dir || !*dir) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - at < std::chrono::seconds(2)) return;
    at = now;
    WristState st;
    WristLink l;
    {
        std::lock_guard lk(wrist_mu);
        st = wrist_in, l = wrist_link_in;
    }
    if (!st.shown) return;
    st.mode = opts.vr_wrist_hud == 2 ? 2 : 1;
    st.map = wrist_map_pick(st, l);
    std::vector<uint8_t> px((size_t)kWristW * kWristH * 4);
    {
        std::lock_guard lk(wrist_draw_mu);
        wrist_leather_make();
        wrist_draw(px.data(), st, &l, 1);
    }
    const int h = std::clamp((int)std::ceil(wrist_height(st) * kWristS), 1, kWristH);
    const std::string path = std::string(dir) + "/wrist.pam";
    if (FILE* f = fopen((path + ".new").c_str(), "wb")) {
        fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", kWristW, h);
        fwrite(px.data(), 1, (size_t)kWristW * h * 4, f);
        fclose(f);
        std::rename((path + ".new").c_str(), path.c_str());
    }
    if (told < 40) {
        told++;
        const art::WristMap* m = st.map >= 0 ? &art::wrist_maps[st.map] : nullptr;
        logf("vr: wrist shot: mode %d, banc %#x, Link %s %.1f %.1f %.1f facing %.0f deg, minimap %d%s (%d maps in this build)",
             st.mode, st.banc, l.ok ? "at" : "not known,", l.x, l.y, l.z, l.facing * 57.2958f, st.map,
             m ? (m->flag ? " (a later picture)" : "") : " (none: the Simple panel)", art::wrist_map_count);
    }
}

/// The wrist check HUD as a quad layer (false: not shown this frame)
bool wrist_layer(XrCompositionLayerQuad& q, const XrPosef& head) {
    static const bool test = [] {
        const char* e = vr_research_getenv("RX_VR_WRIST_TEST");
        const bool on = e && atoi(e) == 1;
        if (on) logf("vr: RX_VR_WRIST_TEST=1 (test): the wrist HUD shown all the time, in front of the head");
        return on;
    }();
    const auto now = std::chrono::steady_clock::now();
    const float dt = wrist.last.time_since_epoch().count() ? std::chrono::duration<float>(now - wrist.last).count() : 0;
    wrist.last = now;
    WristState st;
    WristLink link;
    {
        std::lock_guard lk(wrist_mu);
        st = wrist_in, link = wrist_link_in;
        if (now - wrist_at > std::chrono::milliseconds(300)) st.shown = 0, st.block = 0;   // (the game side gone quiet)
    }
    if (test && !st.shown) {   // (sample values: the board's)
        st.shown = 1, st.life = 14, st.max_life = 24, st.mp = 30, st.mp_max = 48, st.rupees = 87, st.wallet = 200;
        st.keys = 1, st.dungeon = kWristMap | (8 | kWristBossKey | kWristMap | kWristCompass) << 4;
    }
    st.mode = opts.vr_wrist_hud == 2 ? 2 : 1;
    st.map = wrist_map_pick(st, link);
    const bool blocked = !st.shown || st.block || opts.vr_wrist_hud == 0 || s.menu_visible.load() || panel.open.load() ||
                         flat_screen.shown;
    // (either wrist: the one showing keeps it until it fades; the other waits meanwhile)
    WristLook looks[2] = {wrist_look(head, 0), wrist_look(head, 1)};
    const int other = 1 - wrist.hand;
    float alpha = wrist_gesture(wrist.gesture[wrist.hand], looks[wrist.hand], blocked, dt);
    const float other_alpha = wrist_gesture(wrist.gesture[other], looks[other], blocked || alpha > 0, dt);
    if (!(alpha > 0) && other_alpha > 0) wrist.hand = other, alpha = other_alpha;
    WristLook look = looks[wrist.hand];
    {   // (the log, bounded: each show and hide, and a held pose kept away and why: the game side's bits (wrist_host.c
        // kVrWrist*: 1 sword, 2 shield, 4 bow, 8 held, 16 locked on, 32 event, 64 radial, 128 hurt) or the renderer's)
        static bool was = false;
        static int told = 0, why_told = -1;
        const bool on = alpha > 0;
        if (on != was && told < 300) {
            told++;
            logf("vr: wrist HUD %s on the %s wrist (%s person, %s)", on ? "shown" : "hidden", wrist.hand ? "right" : "left",
                 opts.vr_mode == 1 ? "first" : "third", st.map >= 0 ? "with its minimap" : "no minimap");
        }
        was = on;
        const int why = !blocked ? 0 : (st.block & 0xff) | (!st.shown ? 1 << 8 : 0) | (opts.vr_wrist_hud == 0 ? 1 << 9 : 0) |
                        (s.menu_visible.load() || panel.open.load() ? 1 << 10 : 0) | (flat_screen.shown ? 1 << 11 : 0);
        if (blocked && why != why_told && told < 300 && (wrist_pose_holds(looks[0], false) || wrist_pose_holds(looks[1], false))) {
            told++, why_told = why;
            logf("vr: wrist HUD: the wrist is turned to the face but kept away (%#x: game %#x%s%s%s%s)", why, st.block & 0xff,
                 !st.shown ? ", no save" : "", opts.vr_wrist_hud == 0 ? ", Wrist HUD off" : "",
                 s.menu_visible.load() || panel.open.load() ? ", a menu open" : "", flat_screen.shown ? ", a flat frame" : "");
        }
        if (!blocked) why_told = -1;
    }
    if (test) {   // (in front: 35 cm ahead, a little below the eyes)
        alpha = opts.vr_wrist_hud ? 1 : 0;
        for (int k = 0; k < 3; k++) look.at[k] = look.eye[k] + look.forward[k] * .35f;
        look.at[1] -= .08f;
    }
    if (!(alpha > 0) || !st.shown) { wrist.level = -1; return false; }
    if (!wrist_make()) return false;
    const int level = std::clamp((int)std::ceil(alpha * kWristSteps - .01f), 1, kWristSteps);
    // (Link's arrow: its place in pixels and its heading in 5-degree steps; a change redraws it, 25 times a second at most)
    int arrow[4] = {-1, 0, 0, 0};
    if (st.map >= 0 && link.ok) {
        const art::WristMap& m = art::wrist_maps[st.map];
        arrow[0] = st.map;
        arrow[1] = (int)std::lround((link.x - m.x0) / (m.x1 - m.x0) * m.w), arrow[2] = (int)std::lround((link.z - m.z0) / (m.z1 - m.z0) * m.h);
        arrow[3] = (int)std::lround(link.facing * 57.2958f / 5);
    }
    const bool moved = !std::equal(arrow, arrow + 4, wrist.arrow) && now - wrist.uploaded >= std::chrono::milliseconds(40);
    if (!wrist.composed || !st.same_look(wrist.shown) || level != wrist.level || moved) {
        if (headset_gpu_free()) {   // (before the staging memory is written: a pending upload may read it)
            std::lock_guard lk(wrist_draw_mu);
            if (!wrist.composed || !st.same_look(wrist.shown)) {
                wrist_draw(wrist.base.data(), st, nullptr, 1);
                wrist.shown = st, wrist.composed = true, wrist.height = wrist_height(st);
            }
            const float a = (float)level / kWristSteps;
            for (size_t i = 0; i < wrist.base.size(); i++) wrist.pixels[i] = (uint8_t)(wrist.base[i] * a + .5f);
            wrist_px = wrist.pixels;   // (the arrow over the panel as composed: wrist_map_at is still the minimap's)
            wrist_arrow(wrist_map_rgba ? st.map : -1, link, a);
            if (!wrist_upload()) { wrist.level = -1; return false; }
            wrist.level = level, wrist.uploaded = now;
            std::copy(arrow, arrow + 4, wrist.arrow);
        } else if (wrist.level < 0 || !st.same_look(wrist.shown)) return false;   // (else the last step uploaded, a frame more)
    }
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = wrist.eye.chain;
    const int h = std::clamp((int)std::ceil(wrist.height * kWristS), 1, kWristH);
    q.subImage.imageRect.extent = {kWristW, h};
    q.pose.position = {look.at[0], look.at[1], look.at[2]};
    q.pose.orientation = wrist_facing(look.at, head);
    const float width = kWristWidths[std::clamp(opts.vr_wrist_size, 0, 2)];
    q.size = {width, width * h / kWristW};
    return true;
}
