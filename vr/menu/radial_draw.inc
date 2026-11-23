// Hookshot VR: the VR radial menus in the headset (the game side, vr/menu/radial_host.c, sends what they show).
//
// Included by vr/headset/vk_vr.cpp after markers.inc (it uses that file's helpers: barrier, memory_type, cmd_pool, cmd,
// fence, s, ctl). Not compiled on its own.
// One panel, drawn on the CPU into its own swapchain image (kRadialPx square, premultiplied sRGB) whenever what it shows
// changes, and shown as a quad layer floating in the room: placed once, when the menu opens (VR tab Radial menu place: above
// the hand that clicked, or in front at chest height), turned to face the head then, and left there while you look around.
// The entries are a ring, the first at the top, clockwise (the stick's direction picks the nearest, radial_host.c): the
// items (side 0: vr_radial_items) or the equipment (side 1: vr_radial_gear). It looks as the canvas board "Radial item
// wheel" (the drawing section below): the highlighted one in glowing copper and also shown large in the middle with its
// name; the current ones (the selected item; the worn sword, shield and tunic) have a green ring. Icons: the game's own
// (vk_vr_icons.h, vr/tools/generate/icons.py).
constexpr int kRadialPx = 1024;   // (the board's 900 units: its names readable at arm's length)
struct RadialState {
    int side = -1, n = 0, highlight = -1, current = 0;   // (current: bit k for slot k)
    int items[VR_RADIAL_SLOTS]{};
    int contents[VR_RADIAL_SLOTS]{};   // (a bottle's content, -1 empty to 11: its icon)
    bool operator==(const RadialState& o) const {
        if (side != o.side || n != o.n || highlight != o.highlight || current != o.current) return false;
        for (int k = 0; k < n; k++) if (items[k] != o.items[k] || contents[k] != o.contents[k]) return false;
        return true;
    }
};
std::mutex radial_mu;
RadialState radial_in;   // (radial_mu) as the game side last sent it
std::chrono::steady_clock::time_point radial_at{};
struct RadialPanel {
    Eye eye;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* pixels = nullptr;
    bool made = false, failed = false, anchored = false, uploaded = false, asked = false;
    RadialState shown, wanted;   // (what the image shows; what was last asked of the composer)
    XrPosef pose{};
    float size = .3f;
} radial;

void radial_set(const int32_t* d, int n) {
    std::lock_guard lk(radial_mu);
    RadialState st;
    if (d && n >= 4 && d[0] >= 0) {
        st.side = d[0], st.n = std::clamp(d[1], 0, std::min(VR_RADIAL_SLOTS, n - 4)), st.highlight = d[2], st.current = d[3];
        const int last = (st.side == 1 ? VR_RADIAL_GEAR : VR_RADIAL_ITEMS) - 1;
        for (int k = 0; k < st.n; k++)   // (a bottle's entry carries its content: index + 256 x (content + 2))
            st.items[k] = std::clamp(d[4 + k] & 0xff, 0, last), st.contents[k] = std::clamp((d[4 + k] >> 8) - 2, -1, 11);
    }
    radial_in = st;
    radial_at = std::chrono::steady_clock::now();
}

bool radial_make() {
    if (radial.made || radial.failed) return radial.made;
    radial.failed = true;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = swap_format;
    ci.sampleCount = 1;
    ci.width = ci.height = kRadialPx;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s.session, &ci, &radial.eye.chain))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(radial.eye.chain, 0, &n, nullptr);
    radial.eye.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(radial.eye.chain, n, &n, (XrSwapchainImageBaseHeader*)radial.eye.images.data());
    radial.eye.w = radial.eye.h = kRadialPx;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)kRadialPx * kRadialPx * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &radial.staging) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev.device, radial.staging, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev.device, &ai, nullptr, &radial.memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev.device, radial.staging, radial.memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev.device, radial.memory, 0, bi.size, 0, (void**)&radial.pixels) != VK_SUCCESS) return false;
    radial.made = true, radial.failed = false;
    logf("vr: radial menu panel ready (%d px)", kRadialPx);
    return true;
}

// ---- drawing: the canvas board "Radial item wheel" (approved 22 Nov), in its units (900 a side), premultiplied sRGB ----
// The plate: a copper trim round the user's leather (the VR menu's frame art, assets/vr/panel_frame.png, its middle at the
// board's 380%), a copper divider between each pair of slots, a thin ring inside them; the slots: copper-rimmed leather discs
// with the game's icon and the item's name, the highlighted one in glowing copper, the current one with a green ring; the
// centre: a copper-rimmed plate with the highlighted item's icon, a diamond rule and its name. Composed on the CPU into
// radial_px (cached memory, not the mapped staging buffer: blending reads every pixel back), the plate kept for its slot
// count (radial_base), so a highlight change draws only the slots and the centre over it.
constexpr float kRadialS = kRadialPx / 900.0f;   // (pixels a board unit)
constexpr float kRadialRing = 290;               // (the slots' circle, board units)
std::vector<uint8_t> radial_px, radial_base;     // (the image being composed; the plate for radial_base_n slots)
int radial_base_n = -1;
struct RadialCol {
    float r, g, b;
};
constexpr RadialCol radial_hex(unsigned c) { return {(c >> 16 & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c & 255) / 255.0f}; }
RadialCol radial_mix(RadialCol a, RadialCol b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
struct RadialStop {
    float at;
    unsigned c;
};
/// A CSS linear gradient's colour at t (0 top, 1 bottom)
template <int N> RadialCol radial_grad(const RadialStop (&s)[N], float t) {
    if (t <= s[0].at) return radial_hex(s[0].c);
    for (int i = 1; i < N; i++)
        if (t <= s[i].at) return radial_mix(radial_hex(s[i - 1].c), radial_hex(s[i].c), (t - s[i - 1].at) / std::max(1e-4f, s[i].at - s[i - 1].at));
    return radial_hex(s[N - 1].c);
}
constexpr RadialStop kRadialTrim[] = {{0, 0xF2BC8C}, {.45f, 0xA8683E}, {.75f, 0x6E3F22}, {1, 0x3A1F12}};
constexpr RadialStop kRadialCentreTrim[] = {{0, 0xF2BC8C}, {.5f, 0xA8683E}, {.8f, 0x6E3F22}, {1, 0x3A1F12}};
constexpr RadialStop kRadialSlotRim[] = {{0, 0xB97A4E}, {.6f, 0x6E3F22}, {1, 0x3A1F12}};
constexpr RadialStop kRadialSlotFace[] = {{0, 0x33200F}, {1, 0x24150B}};
constexpr RadialStop kRadialHotRim[] = {{0, 0xF2BC8C}, {.55f, 0xA8683E}, {1, 0x6E3F22}};
constexpr RadialStop kRadialHotFace[] = {{0, 0x8A4E2C}, {1, 0x5E321B}};

void radial_over(int x, int y, RadialCol c, float a) {   // (one pixel: colour and coverage, premultiplied over)
    if (x < 0 || y < 0 || x >= kRadialPx || y >= kRadialPx || !(a > 0)) return;
    uint8_t* p = radial_px.data() + ((size_t)y * kRadialPx + x) * 4;
    a = std::min(a, 1.0f);
    const float k = 1 - a;
    p[0] = (uint8_t)std::clamp(c.r * a * 255 + p[0] * k, 0.0f, 255.0f);
    p[1] = (uint8_t)std::clamp(c.g * a * 255 + p[1] * k, 0.0f, 255.0f);
    p[2] = (uint8_t)std::clamp(c.b * a * 255 + p[2] * k, 0.0f, 255.0f);
    p[3] = (uint8_t)std::clamp(a * 255 + p[3] * k, 0.0f, 255.0f);
}
/// Fills the pixels of a board-unit box: each pixel's coverage from sd (its signed distance, board units, at the pixel's
/// centre in board units: negative inside), its colour from col, times alpha (alpha(px, py) when a function)
template <class Sd, class Col, class Alpha>
void radial_shape(float x0, float y0, float x1, float y1, Sd sd, Col col, Alpha alpha) {
    const int px0 = std::max(0, (int)(x0 * kRadialS) - 1), py0 = std::max(0, (int)(y0 * kRadialS) - 1);
    const int px1 = std::min(kRadialPx - 1, (int)(x1 * kRadialS) + 1), py1 = std::min(kRadialPx - 1, (int)(y1 * kRadialS) + 1);
    for (int y = py0; y <= py1; y++)
        for (int x = px0; x <= px1; x++) {
            const float bx = (x + .5f) / kRadialS, by = (y + .5f) / kRadialS;
            const float cover = std::clamp(.5f - sd(bx, by) * kRadialS, 0.0f, 1.0f);
            if (cover <= 0) continue;
            const float a = alpha(bx, by);
            if (a > 0) radial_over(x, y, col(bx, by), cover * a);
        }
}
auto radial_flat(RadialCol c) { return [c](float, float) { return c; }; }
auto radial_const(float a) { return [a](float, float) { return a; }; }
auto radial_disc_sd(float cx, float cy, float r) { return [=](float x, float y) { return std::hypot(x - cx, y - cy) - r; }; }
/// A soft round shadow (a CSS box-shadow of a circle: blur as its radius, offset dy)
void radial_shadow(float cx, float cy, float r, float blur, float dy, float a) {
    const float R = r + blur;
    radial_shape(cx - R, cy + dy - R, cx + R, cy + dy + R, radial_disc_sd(cx, cy + dy, R), radial_flat({0, 0, 0}),
                 [=](float x, float y) {
                     const float t = std::clamp((std::hypot(x - cx, y - cy - dy) - (r - blur)) / (2 * blur), 0.0f, 1.0f);
                     return a * (1 - t) * (1 - t);
                 });
}
/// A disc in a vertical gradient (the element's own box: top cy - r, bottom cy + r)
template <int N> void radial_grad_disc(float cx, float cy, float r, const RadialStop (&g)[N], float a = 1) {
    radial_shape(cx - r, cy - r, cx + r, cy + r, radial_disc_sd(cx, cy, r),
                 [&](float, float y) { return radial_grad(g, (y - (cy - r)) / (2 * r)); }, radial_const(a));
}
/// A ring between two radii
void radial_ring(float cx, float cy, float inner, float outer, RadialCol c, float a) {
    const float mid = (inner + outer) / 2, half = (outer - inner) / 2;
    radial_shape(cx - outer, cy - outer, cx + outer, cy + outer,
                 [=](float x, float y) { return std::abs(std::hypot(x - cx, y - cy) - mid) - half; }, radial_flat(c), radial_const(a));
}

// ---- the user's leather: the frame art (vr_menu_api.inc panel_frame_pixels), sampled bilinear ----
struct RadialArt {
    std::vector<unsigned char> px;
    uint32_t w = 0, h = 0;
    int state = -1;   // (-1 not tried, 0 not in this build: a plain leather colour, 1 loaded)
} radial_art;
/// The leather at board (x, y) of an element centred on (cx, cy) whose background is the art at `span` board units wide
RadialCol radial_leather(float x, float y, float cx, float cy, float span, RadialCol plain) {
    if (radial_art.state < 0) radial_art.state = panel_frame_pixels(&radial_art.w, &radial_art.h, radial_art.px) && radial_art.w > 3 && radial_art.h > 3;
    if (!radial_art.state) return plain;
    const float k = radial_art.w / span;   // (art pixels a board unit, both ways: background-size N% auto)
    const float u = radial_art.w / 2.0f + (x - cx) * k - .5f, v = radial_art.h / 2.0f + (y - cy) * k - .5f;
    const int x0 = std::clamp((int)std::floor(u), 0, (int)radial_art.w - 2), y0 = std::clamp((int)std::floor(v), 0, (int)radial_art.h - 2);
    const float fx = std::clamp(u - x0, 0.0f, 1.0f), fy = std::clamp(v - y0, 0.0f, 1.0f);
    const unsigned char* p = radial_art.px.data() + ((size_t)y0 * radial_art.w + x0) * 4;
    const size_t row = (size_t)radial_art.w * 4;
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = ((p[i] * (1 - fx) + p[4 + i] * fx) * (1 - fy) + (p[row + i] * (1 - fx) + p[row + 4 + i] * fx) * fy) / 255.0f;
    return {c[0], c[1], c[2]};
}
/// A leather disc as the board's .leather / .leather-dark: the art, a warm glow from above the middle, darkened edges, the
/// inset shadows (a light top edge, a dark bottom edge, a soft dark rim)
void radial_leather_disc(float cx, float cy, float r, float span, float glow_y, float glow_a, float glow_to, float dark_from,
                         float dark_a, float top, float bottom, float inner_blur) {
    const float corner = r * 1.41421f;   // (the box's farthest corner from its middle: CSS's circle size)
    radial_shape(cx - r, cy - r, cx + r, cy + r, radial_disc_sd(cx, cy, r),
                 [=](float x, float y) {
                     RadialCol c = radial_leather(x, y, cx, cy, span, radial_hex(0x2A190D));
                     const float d = std::hypot(x - cx, y - cy), gd = std::hypot(x - cx, y - (cy - r + 2 * r * glow_y));
                     c = radial_mix(c, radial_hex(0xFFC896), glow_a * std::clamp(1 - gd / glow_to, 0.0f, 1.0f));
                     c = radial_mix(c, {0, 0, 0}, dark_a * std::clamp((d / corner - dark_from) / (1 - dark_from), 0.0f, 1.0f));
                     const float in = std::clamp(1 - (r - d) / inner_blur, 0.0f, 1.0f);
                     c = radial_mix(c, {0, 0, 0}, .6f * in * in);
                     if (std::hypot(x - cx, y - cy - top) > r) c = radial_mix(c, radial_hex(0xFFD2AA), .25f);   // (inset 0 top)
                     if (std::hypot(x - cx, y - cy + bottom) > r) c = radial_mix(c, {0, 0, 0}, .5f);         // (inset 0 -bottom)
                     return c;
                 },
                 radial_const(1));
}

// ---- text: the VR menu's font (Libertinus Serif Semibold), each glyph rasterized once a size ----
struct RadialGlyph {
    int w = 0, h = 0, x0 = 0, y0 = 0;
    float advance = 0;
    std::vector<unsigned char> a;
};
stbtt_fontinfo* radial_font() {
    static stbtt_fontinfo info;
    static int ok = -1;
    if (ok < 0) ok = stbtt_InitFont(&info, hookshot_font_heading, stbtt_GetFontOffsetForIndex(hookshot_font_heading, 0)) ? 1 : 0;
    return ok ? &info : nullptr;
}
const RadialGlyph* radial_glyph(int ch, int px) {
    static std::unordered_map<int, RadialGlyph> cache;
    stbtt_fontinfo* f = radial_font();
    if (!f || px <= 0 || px > 255) return nullptr;
    auto [it, fresh] = cache.try_emplace(ch * 256 + px);
    RadialGlyph& g = it->second;
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
/// A line's width in board units at size (the CSS font-size, board units)
float radial_text_width(const std::string& t, float size) {
    float w = 0;
    for (unsigned char ch : t)
        if (const RadialGlyph* g = radial_glyph(ch, (int)std::lround(size * kRadialS))) w += g->advance;
    return w / kRadialS;
}
/// A line centred on cx, its capitals' middle on cy; a shadow (offset dy board units, its alpha) under it
void radial_text(const std::string& t, float cx, float cy, float size, RadialCol c, float a, float dy, float shadow) {
    const int px = (int)std::lround(size * kRadialS);
    const RadialGlyph* cap = radial_glyph('H', px);
    if (!cap) return;
    const float base = cy * kRadialS - (cap->y0 + cap->h / 2.0f);
    for (int pass = shadow > 0 ? 0 : 1; pass < 2; pass++) {
        float pen = (cx - radial_text_width(t, size) / 2) * kRadialS;
        const float off = pass == 0 ? dy * kRadialS : 0;
        for (unsigned char ch : t) {
            const RadialGlyph* g = radial_glyph(ch, px);
            if (!g) continue;
            const int ox = (int)std::lround(pen) + g->x0, oy = (int)std::lround(base + off) + g->y0;
            for (int j = 0; j < g->h; j++)
                for (int i = 0; i < g->w; i++)
                    if (const unsigned char v = g->a[(size_t)j * g->w + i])
                        radial_over(ox + i, oy + j, pass == 0 ? RadialCol{0, 0, 0} : c, v / 255.0f * (pass == 0 ? shadow : a));
            pen += g->advance;
        }
    }
}
/// A name broken into at most two lines of at most max_w board units (shrunk down to min_size when it does not fit)
std::vector<std::string> radial_wrap(const char* name, float max_w, float* size, float min_size) {
    std::vector<std::string> words;
    for (const char* p = name; *p;) {
        while (*p == ' ') p++;
        const char* e = p;
        while (*e && *e != ' ') e++;
        if (e > p) words.emplace_back(p, e);
        p = e;
    }
    for (;; *size -= 1) {
        std::vector<std::string> lines;
        for (const std::string& w : words) {
            if (!lines.empty() && radial_text_width(lines.back() + " " + w, *size) <= max_w) lines.back() += " " + w;
            else lines.push_back(w);
        }
        bool fits = lines.size() <= 2;
        for (const std::string& l : lines) fits = fits && radial_text_width(l, *size) <= max_w;
        if (fits || *size <= min_size) return lines;
    }
}

// ---- what is shown ----
const unsigned char* radial_icon(const char* name) {
    for (const art::Icon& i : art::icons)
        if (!std::strcmp(i.name, name)) return i.rgba;
    return nullptr;
}
/// An icon centred on board (cx, cy), size board units a side, bilinear, premultiplied over
void radial_blit(const char* name, float cx, float cy, float size, float alpha) {
    const unsigned char* src = radial_icon(name);
    if (!src) return;
    const int n = art::icon_size;
    const float pcx = cx * kRadialS, pcy = cy * kRadialS, ps = size * kRadialS;
    for (int y = (int)(pcy - ps / 2); y < (int)(pcy + ps / 2); y++)
        for (int x = (int)(pcx - ps / 2); x < (int)(pcx + ps / 2); x++) {
            if (x < 0 || y < 0 || x >= kRadialPx || y >= kRadialPx) continue;
            const float u = ((x + .5f) - (pcx - ps / 2)) / ps * n - .5f, v = ((y + .5f) - (pcy - ps / 2)) / ps * n - .5f;
            const int x0 = std::clamp((int)std::floor(u), 0, n - 1), y0 = std::clamp((int)std::floor(v), 0, n - 1);
            const int x1 = std::min(x0 + 1, n - 1), y1 = std::min(y0 + 1, n - 1);
            const float fx = std::clamp(u - x0, 0.0f, 1.0f), fy = std::clamp(v - y0, 0.0f, 1.0f);
            float c[4];
            for (int k = 0; k < 4; k++) {
                const float a = src[(y0 * n + x0) * 4 + k] * (1 - fx) + src[(y0 * n + x1) * 4 + k] * fx;
                const float b = src[(y1 * n + x0) * 4 + k] * (1 - fx) + src[(y1 * n + x1) * 4 + k] * fx;
                c[k] = (a * (1 - fy) + b * fy) / 255.0f * alpha;
            }
            uint8_t* p = radial_px.data() + ((size_t)y * kRadialPx + x) * 4;
            const float k = 1 - c[3];
            for (int j = 0; j < 4; j++) p[j] = (uint8_t)std::clamp(c[j] * 255 + p[j] * k, 0.0f, 255.0f);
        }
}
const char* radial_icon_of(const RadialState& st, int k) {
    if (st.side == 1) return vr_radial_gear[st.items[k]].name;
    const VrRadialItem& it = vr_radial_items[st.items[k]];
    return !std::strncmp(it.own, "bottle:", 7) ? vr_radial_bottle_icons[st.contents[k] + 1] : it.icon;
}
/// The name shown under an icon (by the icon's name; empty: none)
const char* radial_label(const char* icon) {
    static const char* const names[][2] = {
        {"Item_DekuStick", "Deku Stick"}, {"Item_DekuNut", "Deku Nut"}, {"Item_Bomb", "Bombs"}, {"Item_Bombchu", "Bombchu"},
        {"Item_Boomerang", "Boomerang"}, {"Weapon_Range_SlingShot", "Slingshot"}, {"Item_Hookshot", "Hookshot"},
        {"Item_Longshot", "Longshot"}, {"Item_Hammer", "Megaton Hammer"}, {"Armor_GlassTruth", "Lens of Truth"},
        {"Item_DinFire", "Din's Fire"}, {"Item_FaroreWind", "Farore's Wind"}, {"Item_NayruLove", "Nayru's Love"},
        {"Item_MagicBean", "Magic Beans"}, {"Weapon_Range_Bow", "Fairy Bow"}, {"Pouch_Range_BowAndArrow_Fire", "Fire Arrow"},
        {"Pouch_Range_BowAndArrow_Ice", "Ice Arrow"}, {"Pouch_Range_BowAndArrow_Light", "Light Arrow"},
        {"Weapon_Sword_Kokiri", "Kokiri Sword"}, {"Weapon_Sword_Master", "Master Sword"},
        {"Weapon_Sword_Biggoron", "Biggoron's Sword"}, {"Weapon_Sword_Giant", "Giant's Knife"},
        {"Weapon_Sword_BrokenGiant", "Broken Knife"}, {"Weapon_Shield_Deku", "Deku Shield"},
        {"Weapon_Shield_Hylian", "Hylian Shield"}, {"Weapon_Shield_Mirror", "Mirror Shield"},
        {"Armor_Adult_Normal", "Kokiri Tunic"}, {"Armor_Adult_Goron", "Goron Tunic"}, {"Armor_Adult_Zora", "Zora Tunic"},
        {"Item_Ocarina_Fairy", "Fairy Ocarina"}, {"Item_Ocarina_Time", "Ocarina of Time"}, {"Item_Bottle", "Empty Bottle"},
        {"Item_Bottle_RedPotion", "Red Potion"}, {"Item_Bottle_GreenPotion", "Green Potion"},
        {"Item_Bottle_BluePotion", "Blue Potion"}, {"Item_Bottle_BlueFire", "Blue Fire"}, {"Item_Bottle_Fish", "Fish"},
        {"Item_Bottle_Letter", "Ruto's Letter"}, {"Item_Bottle_BigPoe", "Big Poe"}, {"Item_Bottle_Poe", "Poe"},
        {"Item_Bottle_Bug", "Bug"}, {"Item_Bottle_Fairy", "Fairy"}, {"Item_Bottle_Milk", "Milk"},
        {"Item_Bottle_HalfMilk", "Half Milk"}};
    for (const auto& n : names)
        if (!std::strcmp(n[0], icon)) return n[1];
    return "";
}

/// The plate for n slots, into radial_px: the trim, the leather, the dividers, the inner ring, the centre plate
void radial_compose_base(int n) {
    std::fill(radial_px.begin(), radial_px.end(), 0);
    radial_shadow(450, 450, 394, 24, 10, .65f);                        // (box-shadow 0 28 70: kept inside the image)
    radial_ring(450, 450, 394, 396, {0, 0, 0}, .7f);                   // (0 0 0 2px rgba(0,0,0,.7))
    radial_grad_disc(450, 450, 394, kRadialTrim);                      // (the copper trim)
    radial_leather_disc(450, 450, 380, 760 * 3.8f, .42f, .10f, 320, .58f, .55f, 3, 4, 40);   // (.leather)
    for (int i = 0; n > 1 && i < n; i++) {   // (a divider between each pair of neighbours: 2 wide, 196-346 from the middle)
        const float t = (i + .5f) * 6.2831853f / n, dx = std::sin(t), dy = -std::cos(t);
        for (int pass = 0; pass < 2; pass++) {   // (its shadow, 1 to its side, then the copper)
            const float ox = pass ? 0 : -dy, oy = pass ? 0 : dx;
            radial_shape(450 + std::min(dx * 196, dx * 346) - 3, 450 + std::min(dy * 196, dy * 346) - 3,
                         450 + std::max(dx * 196, dx * 346) + 3, 450 + std::max(dy * 196, dy * 346) + 3,
                         [=](float x, float y) {
                             const float px = x - 450 - ox, py = y - 450 - oy, along = px * dx + py * dy;
                             return std::max(std::abs(px * -dy + py * dx) - (pass ? 1.0f : .5f), std::max(196 - along, along - 346));
                         },
                         radial_flat(pass ? radial_hex(0xC98B5C) : RadialCol{0, 0, 0}),
                         [=](float x, float y) {
                             const float f = ((x - 450) * dx + (y - 450) * dy - 196) / 150;
                             return (pass ? .55f : .55f) * std::clamp(std::min(f / .3f, (1 - f) / .3f), 0.0f, 1.0f);
                         });
        }
    }
    radial_ring(450, 450, 208, 209, radial_hex(0xC98B5C), .45f);       // (the inner ring: its copper edge outside,
    radial_ring(450, 450, 206, 208, {0, 0, 0}, .55f);                  //  its dark border,
    radial_ring(450, 450, 205, 206, radial_hex(0xC98B5C), .25f);       //  its copper edge inside)
    radial_shadow(450, 450, 127, 15, 10, .6f);                         // (the centre plate: box-shadow 0 10 30)
    radial_grad_disc(450, 450, 127, kRadialCentreTrim);
    radial_leather_disc(450, 450, 117, 234 * 6.4f, .35f, .08f, 115, .5f, .6f, 2, 3, 24);      // (.leather-dark)
    radial_base_n = n;
}

void radial_compose(const RadialState& st) {
    const size_t bytes = (size_t)kRadialPx * kRadialPx * 4;
    radial_px.resize(bytes);
    const int n = std::max(st.n, 1);
    if (radial_base_n != n || radial_base.size() != bytes) {
        radial_compose_base(n);
        radial_base = radial_px;
    } else {
        memcpy(radial_px.data(), radial_base.data(), bytes);
    }
    // (the slots: one size for all, as big as the section allows, at most 124; a name under the icon when it has room)
    const float chord = n > 1 ? 2 * kRadialRing * std::sin(3.14159265f / n) : 400;
    const float size = std::min(124.0f, chord - 26), r = size / 2;
    const bool named = size >= 84;
    for (int k = 0; k < st.n; k++) {
        const float a = 6.2831853f * k / n, x = 450 + kRadialRing * std::sin(a), y = 450 - kRadialRing * std::cos(a);
        const bool hi = k == st.highlight, cur = (st.current >> k) & 1;
        if (hi) {   // (filter: drop-shadow 0 0 12px rgba(255,160,90,.6))
            radial_shape(x - r - 24, y - r - 24, x + r + 24, y + r + 24, radial_disc_sd(x, y, r + 24), radial_flat(radial_hex(0xFFA05A)),
                         [=](float px, float py) {
                             const float t = std::clamp((std::hypot(px - x, py - y) - r) / 24, 0.0f, 1.0f);
                             return .6f * (1 - t) * (1 - t);
                         });
        } else {
            radial_shadow(x, y, r, 5, 4, .55f);   // (box-shadow 0 4 10)
        }
        if (hi) radial_grad_disc(x, y, r, kRadialHotRim);
        else radial_grad_disc(x, y, r, kRadialSlotRim);
        const float fr = r - 5;
        radial_shape(x - fr, y - fr, x + fr, y + fr, radial_disc_sd(x, y, fr),
                     [=](float px, float py) {
                         RadialCol c = radial_grad(hi ? kRadialHotFace : kRadialSlotFace, (py - (y - fr)) / (2 * fr));
                         if (std::hypot(px - x, py - y - 2) > fr) c = radial_mix(c, radial_hex(0xFFD2AA), hi ? .35f : .12f);
                         if (std::hypot(px - x, py - y + 3) > fr) c = radial_mix(c, {0, 0, 0}, hi ? .4f : .5f);
                         return c;
                     },
                     radial_const(1));
        if (cur) radial_ring(x, y, r - 4.5f, r - .5f, radial_hex(0x5AE66A), .95f);   // (the item you have now: green)
        const char* icon = radial_icon_of(st, k);
        const char* name = named ? radial_label(icon) : "";
        float fs = size >= 110 ? 15 : 13;
        const std::vector<std::string> lines = *name ? radial_wrap(name, size - 30, &fs, 11) : std::vector<std::string>{};
        const float icon_size = lines.empty() ? size * .64f : size * .5f, line_h = fs * 1.1f;
        const float h = icon_size + (lines.empty() ? 0 : 6 + line_h * lines.size());
        radial_blit(icon, x, y - h / 2 + icon_size / 2, icon_size, 1);
        for (size_t l = 0; l < lines.size(); l++)
            radial_text(lines[l], x, y - h / 2 + icon_size + 6 + line_h * (l + .5f), fs, radial_hex(hi ? 0xFFF0D8 : 0xE3CDB0), 1, 1,
                        hi ? .8f : 0);
    }
    // (the centre: the highlighted entry; else the item menu's current item, faint)
    int middle = st.highlight >= 0 && st.highlight < st.n ? st.highlight : -1;
    if (middle < 0 && st.side == 0 && st.current) middle = __builtin_ctz((unsigned)st.current);
    if (middle >= 0 && middle < st.n) {
        const float alpha = st.highlight >= 0 ? 1 : .45f;
        const char* icon = radial_icon_of(st, middle);
        float fs = 25;
        const std::vector<std::string> lines = radial_wrap(radial_label(icon), 190, &fs, 17);
        const float icon_size = 104, line_h = fs * 1.1f, rule = 7;
        const float h = icon_size + 12 + rule + (lines.empty() ? 0 : 12 + line_h * lines.size());
        float y = 450 - h / 2;
        radial_blit(icon, 450, y + icon_size / 2, icon_size, alpha);
        y += icon_size + 12;
        const float ry = y + rule / 2;   // (the rule: a diamond, a line fading at both ends, a diamond)
        radial_shape(450 - 68, ry - 1, 450 + 68, ry + 1, [=](float px, float py) { return std::max(std::abs(px - 450) - 60, std::abs(py - ry) - 1); },
                     radial_flat(radial_hex(0xC98B5C)), [=](float px, float) {
                         const float f = (px - 390) / 120;
                         return alpha * std::clamp(std::min(f / .2f, (1 - f) / .2f), 0.0f, 1.0f);
                     });
        for (int side = -1; side <= 1; side += 2) {
            const float dx = 450 + side * (60 + 8 + 3.5f);
            radial_shape(dx - 6, ry - 6, dx + 6, ry + 6, [=](float px, float py) { return (std::abs(px - dx) + std::abs(py - ry)) * .7071f - 3.5f; },
                         radial_flat(radial_hex(0xC98B5C)), radial_const(alpha));
        }
        y += rule + 12;
        for (size_t l = 0; l < lines.size(); l++)
            radial_text(lines[l], 450, y + line_h * (l + .5f), fs, radial_hex(0xF6CFAA), alpha, 2, .8f * alpha);
    }
}

// ---- the composer's thread: a first picture (the plate) takes tens of ms and a highlight change several, too long for the
// headset's frame, so the headset's thread asks for a state and uploads the picture once it is made, showing the last one
// meanwhile (latest asked wins) ----
struct RadialWork {
    std::mutex mu;
    std::condition_variable cv;
    bool started = false, asked = false, ready = false;
    RadialState want, made;          // (the next to compose; what out holds)
    std::vector<uint8_t> out;        // (a finished picture, premultiplied RGBA)
} radial_work;
void radial_ask(const RadialState& st) {
    std::lock_guard lk(radial_work.mu);
    radial_work.want = st, radial_work.asked = true;
    if (!radial_work.started) {
        radial_work.started = true;
        std::thread([] {
            for (;;) {
                RadialState st;
                {
                    std::unique_lock lk(radial_work.mu);
                    radial_work.cv.wait(lk, [] { return radial_work.asked; });
                    st = radial_work.want, radial_work.asked = false;
                }
                radial_compose(st);
                std::lock_guard lk(radial_work.mu);
                radial_work.out.swap(radial_px);
                radial_work.made = st, radial_work.ready = true;
            }
        }).detach();
    }
    radial_work.cv.notify_one();
}
/// A finished picture copied into the staging memory (false: none since the last)
bool radial_take(RadialState& made) {
    std::lock_guard lk(radial_work.mu);
    if (!radial_work.ready || radial_work.out.size() != (size_t)kRadialPx * kRadialPx * 4) return false;
    memcpy(radial.pixels, radial_work.out.data(), radial_work.out.size());
    made = radial_work.made, radial_work.ready = false;
    return true;
}

bool radial_upload() {
    uint32_t index = 0;
    SwapTake take;
    if (!headset_gpu_free() || !swap_take(take, {radial.eye.chain}, &index, "radial menu")) return false;
    if (!headset_record()) return swap_give(take), false;
    const VkImage img = radial.eye.images[index].image;
    barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)kRadialPx, (uint32_t)kRadialPx, 1};
    vkCmdCopyBufferToImage(cmd, radial.staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return headset_submit(take, VK_NULL_HANDLE, 0, "radial menu");
}

// A rotation whose +z points from `at` to `to` (the quad's face toward the head), +y as near the room's up as it can be
XrQuaternionf radial_facing(XrVector3f at, XrVector3f to) {
    float z[3] = {to.x - at.x, to.y - at.y, to.z - at.z};
    float n = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(n > 1e-4f)) return {0, 0, 0, 1};
    for (float& v : z) v /= n;
    float x[3] = {z[2], 0, -z[0]};   // (up x z)
    n = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (!(n > 1e-4f)) x[0] = 1, x[2] = 0, n = 1;
    x[0] /= n, x[2] /= n;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const float m00 = x[0], m11 = y[1], m22 = z[2], t = m00 + m11 + m22;   // (columns x, y, z)
    XrQuaternionf q;
    if (t > 0) {
        const float s4 = std::sqrt(t + 1) * 2;
        q = {(y[2] - z[1]) / s4, (z[0] - x[2]) / s4, (x[1] - y[0]) / s4, s4 / 4};
    } else if (m00 > m11 && m00 > m22) {
        const float s4 = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {s4 / 4, (y[0] + x[1]) / s4, (z[0] + x[2]) / s4, (y[2] - z[1]) / s4};
    } else if (m11 > m22) {
        const float s4 = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(y[0] + x[1]) / s4, s4 / 4, (z[1] + y[2]) / s4, (z[0] - x[2]) / s4};
    } else {
        const float s4 = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(z[0] + x[2]) / s4, (z[1] + y[2]) / s4, s4 / 4, (x[1] - y[0]) / s4};
    }
    return q;
}

/// The radial menu as a quad layer (false: not open)
bool radial_layer(XrCompositionLayerQuad& q, const XrPosef& head) {
    RadialState st;
    {
        std::lock_guard lk(radial_mu);
        st = radial_in;
        if (std::chrono::steady_clock::now() - radial_at > std::chrono::milliseconds(300)) st.side = -1;
    }
    if (st.side < 0 || st.n <= 0 || opts.vr_mode != 1) { radial.anchored = false; return false; }
    if (!radial_make()) return false;
    if (!radial.anchored) {   // (placed once, when it opens)
        XrVector3f at;
        const int h = st.side == 1 ? 1 : 0;
        if (opts.vr_radial_place == 0 && ctl.attached && ctl.now[h].valid) {
            const XrVector3f g = ctl.now[h].grip.position;
            at = {g.x, g.y + .14f, g.z};
            radial.size = .3f;
        } else {   // (in front: half a metre ahead along the head's level heading, a little below the eyes)
            float f[3];
            const float ahead[3] = {0, 0, -1};
            qrot(head.orientation, ahead, f);
            const float n = std::sqrt(f[0] * f[0] + f[2] * f[2]);
            if (n > 1e-3f) f[0] /= n, f[2] /= n;
            at = {head.position.x + f[0] * .55f, head.position.y - .18f, head.position.z + f[2] * .55f};
            radial.size = .42f;
        }
        radial.pose.position = at;
        radial.pose.orientation = radial_facing(at, head.position);
        radial.anchored = true, radial.uploaded = false, radial.asked = false;
    }
    if (!radial.asked || !(st == radial.wanted)) radial_ask(st), radial.asked = true, radial.wanted = st;
    RadialState made;   // (the staging memory written only once no upload may still read it)
    if (headset_gpu_free() && radial_take(made)) {
        if (radial_upload()) radial.shown = made, radial.uploaded = true;
        else radial.asked = false;   // (asked again next frame)
    }
    if (!radial.uploaded) return false;   // (nothing made for this opening yet: the plate's first picture)
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = radial.eye.chain;
    q.subImage.imageRect.extent = {kRadialPx, kRadialPx};
    q.pose = radial.pose;
    q.size = {radial.size, radial.size};
    return true;
}
