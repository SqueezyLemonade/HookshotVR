#include "draw.h"
#include <math.h>
#include <string.h>
#include "imgui_internal.h"

namespace draw {

Fonts fonts;

namespace {
ImDrawList *dl;
ImVec2 origin;
float scale = 1;
ImGuiID pressed;
bool hovered_any;

ImVec2 lerp2(ImVec2 a, ImVec2 b, float t) { return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }

ImU32 mix(ImU32 a, ImU32 b, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    int r = (int)((a & 255) + (((int)(b & 255) - (int)(a & 255)) * t));
    int g = (int)(((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t));
    int bl = (int)(((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t));
    int al = (int)(((a >> 24) & 255) + (((int)((b >> 24) & 255) - (int)((a >> 24) & 255)) * t));
    return IM_COL32(r, g, bl, al);
}

// The octagon of a cut-corner rectangle, in window units
int octagon(float x, float y, float w, float h, float c, ImVec2 *pts) {
    c = fminf(c, fminf(w, h) * 0.5f);
    const ImVec2 p[8] = {{x + c, y}, {x + w - c, y}, {x + w, y + c}, {x + w, y + h - c},
                         {x + w - c, y + h}, {x + c, y + h}, {x, y + h - c}, {x, y + c}};
    for (int i = 0; i < 8; i++) pts[i] = P(p[i].x, p[i].y);
    return 8;
}

// Colours the vertices [from, to) top to bottom through the stops, keeping each vertex's alpha (the antialiased edge)
void shade(int from, int to, float y0, float y1, ImU32 c0, ImU32 c1, ImU32 c2, float mid) {
    ImDrawVert *v = dl->VtxBuffer.Data;
    for (int i = from; i < to; i++) {
        float t = y1 > y0 ? (v[i].pos.y - y0) / (y1 - y0) : 0;
        ImU32 c = !c2 ? mix(c0, c1, t) : t < mid ? mix(c0, c1, t / mid) : mix(c1, c2, (t - mid) / (1 - mid));
        unsigned a = ((c >> 24) & 255) * ((v[i].col >> 24) & 255) / 255;
        v[i].col = (c & 0x00FFFFFF) | (a << 24);
    }
}

// A convex polygon filled with a tiling texture (the leather), coloured top to bottom
void poly_textured(const ImVec2 *pts, int n, ImTextureID tex, float tile, ImU32 top, ImU32 bottom, float y0, float y1) {
    dl->PushTexture(ImTextureRef(tex));
    dl->PrimReserve((n - 2) * 3, n);
    unsigned base = dl->_VtxCurrentIdx;
    for (int i = 0; i < n; i++) {
        float t = (pts[i].y - y0) / (y1 - y0);
        ImVec2 f((pts[i].x - origin.x) / scale, (pts[i].y - origin.y) / scale);
        dl->PrimWriteVtx(pts[i], ImVec2(f.x / tile, f.y / tile), mix(top, bottom, t));
    }
    for (int i = 2; i < n; i++) dl->PrimWriteIdx((ImDrawIdx)base), dl->PrimWriteIdx((ImDrawIdx)(base + i - 1)), dl->PrimWriteIdx((ImDrawIdx)(base + i));
    dl->PopTexture();
}

// The rim's inner lights (the canvas's inset box-shadows): a light line under the top edge, a dark one over the bottom
void inset_lights(float x, float y, float w, float h, float c, ImU32 top, float top_px, ImU32 bottom, float bottom_px) {
    dl->AddRectFilled(P(x + c * 0.6f, y), P(x + w - c * 0.6f, y + top_px), top);
    dl->AddRectFilled(P(x + c * 0.6f, y + h - bottom_px), P(x + w - c * 0.6f, y + h), bottom);
}
}  // namespace

void begin(ImDrawList *list_, ImVec2 display) {
    dl = list_;
    scale = fminf(display.x / kW, display.y / kH);
    origin = ImVec2(floorf((display.x - kW * scale) * 0.5f), floorf((display.y - kH * scale) * 0.5f));
    hovered_any = false;
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) pressed = 0;
}
ImVec2 P(float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); }
float S(float v) { return v * scale; }
ImVec2 frame_mouse() {
    ImVec2 m = ImGui::GetIO().MousePos;
    return ImVec2((m.x - origin.x) / scale, (m.y - origin.y) / scale);
}
ImDrawList *list() { return dl; }
bool any_hover() { return hovered_any; }

// ---- text ----
float text_width(ImFont *f, float size, const std::string &s) {
    return f->CalcTextSizeA(S(size), FLT_MAX, 0, s.c_str()).x / scale;
}

std::string fit(ImFont *f, float size, const std::string &s, float width) {
    if (text_width(f, size, s) <= width) return s;
    std::string t = s;
    while (!t.empty() && text_width(f, size, t + "\xE2\x80\xA6") > width) t.pop_back();
    while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();   // (not inside a letter's UTF-8)
    if (!t.empty() && (t.back() & 0x80)) t.pop_back();
    return t + "\xE2\x80\xA6";
}

void text(ImFont *f, float size, float x, float cy, unsigned col, const std::string &s, Align a, float alpha, bool shadow) {
    float w = text_width(f, size, s);
    float left = a == Left ? x : a == Center ? x - w * 0.5f : x - w;
    ImFontBaked *b = f->GetFontBaked(S(size));
    // (the middle of the lower case and capitals on cy)
    float top = cy - b->Ascent / scale * 0.62f;
    if (shadow) dl->AddText(f, S(size), P(left, top + 1), rgb(0, 0.8f * alpha), s.c_str());
    dl->AddText(f, S(size), P(left, top), rgb(col, alpha), s.c_str());
}

void spaced_text(ImFont *f, float size, float cx, float cy, ImU32 col, const std::string &s, float spacing) {
    float total = 0;
    for (size_t i = 0; i < s.size(); i++) total += text_width(f, size, s.substr(i, 1)) + (i + 1 < s.size() ? spacing : 0);
    ImFontBaked *b = f->GetFontBaked(S(size));
    float top = cy - b->Ascent / scale * 0.6f;
    float x = cx - total * 0.5f;
    for (size_t i = 0; i < s.size(); i++) {
        std::string ch = s.substr(i, 1);
        dl->AddText(f, S(size), P(x, top + 2), IM_COL32(0, 0, 0, 150), ch.c_str());
        dl->AddText(f, S(size), P(x, top), col, ch.c_str());
        x += text_width(f, size, ch) + spacing;
    }
}

// ---- shapes ----
void chamfer(float x, float y, float w, float h, float c, ImU32 col) {
    ImVec2 pts[8];
    dl->AddConvexPolyFilled(pts, octagon(x, y, w, h, c, pts), col);
}

void chamfer_gradient(float x, float y, float w, float h, float c, ImU32 c0, ImU32 c1, ImU32 c2, float mid) {
    ImVec2 pts[8];
    int n = octagon(x, y, w, h, c, pts);
    int from = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(pts, n, IM_COL32_WHITE);
    shade(from, dl->VtxBuffer.Size, P(0, y).y, P(0, y + h).y, c0, c1, c2, mid);
}

void chamfer_line(float x, float y, float w, float h, float c, ImU32 col, float thickness) {
    ImVec2 pts[8];
    dl->AddPolyline(pts, octagon(x, y, w, h, c, pts), col, S(thickness), ImDrawFlags_Closed);
}

void chamfer_glow(float x, float y, float w, float h, float c, ImU32 col, float radius) {
    // (the canvas's drop-shadow(0 0 r colour): rings fading outwards)
    float a = ((col >> 24) & 255) / 255.0f;
    for (float i = 1; i <= radius; i += 1.5f) {
        float t = 1 - i / radius;
        ImU32 ring = (col & 0x00FFFFFF) | ((ImU32)(a * t * t * 0.55f * 255) << 24);
        chamfer_line(x - i, y - i, w + 2 * i, h + 2 * i, c + i * 0.4f, ring, 1.6f);
    }
}

void dot(float cx, float cy, float d, ImU32 col, bool glow) {
    if (glow)
        for (int i = 4; i >= 1; i--) dl->AddCircleFilled(P(cx, cy), S(d * 0.5f + i * 2.0f), (col & 0x00FFFFFF) | ((ImU32)(30 / i) << 24));
    dl->AddCircleFilled(P(cx, cy), S(d * 0.5f), col);
}

void row_rule(float x0, float x1, float y) {
    dl->AddRectFilled(P(x0, y - 1), P(x1, y), rgb(0, 0.6f));
    dl->AddRectFilled(P(x0, y), P(x1, y + 1), rgb(0xC98B5C, 0.28f));
}

// ---- controls ----
bool hit(const char *id, float x, float y, float w, float h, bool enabled, bool *hover) {
    ImVec2 m = frame_mouse();
    bool in = enabled && m.x >= x && m.x < x + w && m.y >= y && m.y < y + h && ImGui::IsMousePosValid();
    if (hover) *hover = in;
    if (in) hovered_any = true;
    ImGuiID gid = ImHashStr(id);
    if (in && ImGui::IsMouseClicked(0)) pressed = gid;
    return in && ImGui::IsMouseReleased(0) && pressed == gid;
}

bool small_button(const char *id, float x, float y, float w, float h, const std::string &label, bool strong, float size) {
    bool hover = false;
    bool click = hit(id, x, y, w, h, true, &hover);
    chamfer_gradient(x, y, w, h, 8, rgb(0xB97A4E), rgb(0x6E3F22), rgb(0x3A1F12), 0.6f);
    if (hover) chamfer_glow(x, y, w, h, 8, rgb(0xF0AA6E, 0.35f), 8);
    ImU32 a = strong ? rgb(0x4A2C17) : rgb(0x33200F), b = strong ? rgb(0x2E1A0D) : rgb(0x24150B);
    if (hover) a = mix(a, rgb(0x8A4E2C), 0.45f), b = mix(b, rgb(0x5E321B), 0.45f);
    chamfer_gradient(x + 2, y + 2, w - 4, h - 4, 7, a, b);
    text(fonts.semibold, size, x + w * 0.5f, y + h * 0.5f, kInkButton, label, Center);
    return click;
}

bool big_button(const char *id, float x, float y, float w, float h, const std::string &label, int kind, float size) {
    bool hover = false;
    bool click = hit(id, x, y, w, h, kind != 2, &hover);
    if (kind == 0) {
        chamfer_glow(x, y, w, h, 12, rgb(0xF0AA6E, hover ? 0.6f : 0.4f), 12);
        chamfer_gradient(x, y, w, h, 12, rgb(0xF2BC8C), rgb(0xA8683E), rgb(0x6E3F22), 0.55f);
        chamfer_gradient(x + 2, y + 2, w - 4, h - 4, 11, hover ? rgb(0xB56C3E) : rgb(0xA35F36), hover ? rgb(0x844A25) : rgb(0x74401F));
        inset_lights(x + 2, y + 2, w - 4, h - 4, 11, rgb(0xFFD7AF, 0.4f), 2, rgb(0, 0.35f), 3);
        text(fonts.semibold, size, x + w * 0.5f, y + h * 0.5f, kInkTitle, label, Center);
    } else if (kind == 1) {
        if (hover) chamfer_glow(x, y, w, h, 12, rgb(0xF0AA6E, 0.3f), 10);
        chamfer_gradient(x, y, w, h, 12, rgb(0xB97A4E), rgb(0x6E3F22), rgb(0x3A1F12), 0.6f);
        chamfer_gradient(x + 2, y + 2, w - 4, h - 4, 11, hover ? rgb(0x4A2C17) : rgb(0x33200F), hover ? rgb(0x2E1A0D) : rgb(0x24150B));
        text(fonts.medium, size, x + w * 0.5f, y + h * 0.5f, kInkCard, label, Center);
    } else {
        chamfer_gradient(x, y, w, h, 12, rgb(0x7A5A40), rgb(0x4A3020));
        chamfer(x + 2, y + 2, w - 4, h - 4, 11, rgb(0x2A1A10));
        text(fonts.italic, size, x + w * 0.5f, y + h * 0.5f, 0xC9B08F, label, Center);
    }
    return click;
}

bool mode_card(const char *id, float x, float y, float w, float h, const std::string &label, bool vr, bool sel, ImTextureID art) {
    bool hover = false;
    bool click = hit(id, x, y, w, h, true, &hover);
    if (sel) {
        chamfer_glow(x, y, w, h, 12, rgb(0xF0AA6E, 0.35f), 14);
        chamfer_gradient(x, y, w, h, 12, rgb(0xF2BC8C), rgb(0xA8683E), rgb(0x6E3F22), 0.55f);
        chamfer_gradient(x + 2, y + 2, w - 4, h - 4, 11, rgb(0x8A4E2C), rgb(0x5E321B));
        inset_lights(x + 2, y + 2, w - 4, h - 4, 11, rgb(0xFFD2AA, 0.35f), 2, rgb(0, 0.4f), 3);
    } else {
        if (hover) chamfer_glow(x, y, w, h, 12, rgb(0xF0AA6E, 0.2f), 10);
        chamfer_gradient(x, y, w, h, 12, rgb(0xB97A4E), rgb(0x6E3F22), rgb(0x3A1F12), 0.6f);
        chamfer_gradient(x + 2, y + 2, w - 4, h - 4, 11, hover ? rgb(0x3C2513) : rgb(0x33200F), hover ? rgb(0x2B190D) : rgb(0x24150B));
        inset_lights(x + 2, y + 2, w - 4, h - 4, 11, rgb(0xFFD2AA, 0.12f), 2, rgb(0, 0.5f), 3);
    }
    ImFont *f = sel ? fonts.semibold : fonts.medium;
    float tw = text_width(f, 34, label);
    float group = 80 + 24 + tw;
    float gx = x + (w - group) * 0.5f, cy = y + h * 0.5f;
    chamfer(gx, cy - 40, 80, 80, 8, sel ? rgb(0x2C190F) : rgb(0x1E110A));
    ImU32 ic = sel ? rgb(0xF2D9BA) : rgb(0xC9B08F);
    if (art) dl->AddImage(ImTextureRef(art), P(gx + 4, cy - 36), P(gx + 76, cy + 36));
    else if (vr) icon_headset(gx + 40, cy, 54, ic);
    else icon_monitor(gx + 40, cy, 50, ic);
    text(f, 34, gx + 104, cy, sel ? kInkTitle : kInkCard, label, Left);
    return click;
}

// ---- icons: the canvas's SVGs, drawn on a 48-unit box ----
static ImVec2 icon_pt(float cx, float cy, float size, float x, float y) {
    return P(cx + (x - 24) * size / 48, cy + (y - 24) * size / 48);
}

void icon_monitor(float cx, float cy, float size, ImU32 col) {
    float k = size / 48, t = S(3 * k);
    dl->AddRect(icon_pt(cx, cy, size, 5, 8), icon_pt(cx, cy, size, 43, 33), col, S(2.5f * k), t);
    dl->AddLine(icon_pt(cx, cy, size, 24, 33), icon_pt(cx, cy, size, 24, 40), col, t);
    dl->AddLine(icon_pt(cx, cy, size, 16, 41), icon_pt(cx, cy, size, 32, 41), col, t);
    dl->AddCircleFilled(icon_pt(cx, cy, size, 16, 41), t * 0.5f, col);
    dl->AddCircleFilled(icon_pt(cx, cy, size, 32, 41), t * 0.5f, col);
}

void app_icon(ImDrawList *list_, float px) {
    dl = list_, scale = px / 100, origin = ImVec2(0, 0);   // (a 100 x 100 space of its own)
    const float c = app_icon_cut(100);
    chamfer_gradient(0, 0, 100, 100, c, rgb(0xF2BC8C), rgb(0xA8683E), rgb(0x6E3F22), 0.55f);
    chamfer_gradient(5, 5, 90, 90, c - 2, rgb(0x3A2213), rgb(0x1E110A));
    inset_lights(5, 5, 90, 90, c - 2, rgb(0xFFD2AA, 0.25f), 1.5f, rgb(0, 0.5f), 2.5f);
    icon_headset(50, 51, 74, rgb(0xF2D9BA));
}

void icon_headset(float cx, float cy, float size, ImU32 col) {
    float k = size / 48, t = S(3 * k);
    auto q = [&](float x, float y) { return icon_pt(cx, cy, size, x, y); };
    dl->PathLineTo(q(7, 18.5f));
    dl->PathBezierCubicCurveTo(q(7, 15.7f), q(9.2f, 13.5f), q(12, 13.5f));
    dl->PathLineTo(q(36, 13.5f));
    dl->PathBezierCubicCurveTo(q(38.8f, 13.5f), q(41, 15.7f), q(41, 18.5f));
    dl->PathLineTo(q(41, 27.5f));
    dl->PathBezierCubicCurveTo(q(41, 30.3f), q(38.8f, 32.5f), q(36, 32.5f));
    dl->PathLineTo(q(28.5f, 32.5f));
    dl->PathLineTo(q(25.9f, 28.9f));
    dl->PathBezierCubicCurveTo(q(25.0f, 27.7f), q(23.0f, 27.7f), q(22.1f, 28.9f));
    dl->PathLineTo(q(19.5f, 32.5f));
    dl->PathLineTo(q(12, 32.5f));
    dl->PathBezierCubicCurveTo(q(9.2f, 32.5f), q(7, 30.3f), q(7, 27.5f));
    dl->PathStroke(col, t, ImDrawFlags_Closed);
    dl->AddLine(q(7, 21), q(3.5f, 21), col, t);
    dl->AddLine(q(41, 21), q(44.5f, 21), col, t);
    dl->AddCircleFilled(q(3.5f, 21), t * 0.5f, col);
    dl->AddCircleFilled(q(44.5f, 21), t * 0.5f, col);
}

void icon_gear(float cx, float cy, float size, ImU32 col) {
    const int teeth = 8;
    float r0 = size * 0.36f, r1 = size * 0.48f;
    for (int i = 0; i < teeth * 4; i++) {
        float a = (i / (float)(teeth * 4)) * 6.2831853f;
        float r = (i % 4 == 1 || i % 4 == 2) ? r1 : r0;
        dl->PathLineTo(P(cx + cosf(a) * r, cy + sinf(a) * r));
    }
    dl->PathFillConcave(col);
    dl->AddCircleFilled(P(cx, cy), S(size * 0.16f), rgb(0x2A1A10));
}

void icon_check(float cx, float cy, float size, ImU32 col) {
    ImVec2 pts[3] = {P(cx - size * 0.32f, cy + size * 0.02f), P(cx - size * 0.08f, cy + size * 0.26f), P(cx + size * 0.34f, cy - size * 0.26f)};
    dl->AddPolyline(pts, 3, col, S(size * 0.14f));
}

void arrow(float cx, float cy, float size, bool right, ImU32 col) {
    float d = right ? 1 : -1, h = size * 0.5f;
    dl->AddTriangleFilled(P(cx + d * h * 0.85f, cy), P(cx - d * h * 0.7f, cy - h * 0.85f), P(cx - d * h * 0.7f, cy + h * 0.85f), col);
}

void diamond(float cx, float cy, float size, ImU32 col) {
    float h = size * 0.7071f;
    dl->AddQuadFilled(P(cx, cy - h), P(cx + h, cy), P(cx, cy + h), P(cx - h, cy), col);
}

// ---- the frame ----
static void nine_slice(ImTextureID tex, int tw, int th) {
    // (the canvas's border-image: the source cut 215 / 150 / 226 / 150, drawn 165 / 115 / 174 / 115 on the 1280 x 820 frame)
    const float sx[4] = {0, 150.0f / tw, 1 - 150.0f / tw, 1}, sy[4] = {0, 215.0f / th, 1 - 226.0f / th, 1};
    const float dx[4] = {0, 115, kW - 115, kW}, dy[4] = {0, 165, kH - 174, kH};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            dl->AddImage(ImTextureRef(tex), P(dx[i], dy[j]), P(dx[i + 1], dy[j + 1]), ImVec2(sx[i], sy[j]), ImVec2(sx[i + 1], sy[j + 1]));
}

static void curl(float cx, float cy, float sx, float sy, ImU32 col) {
    // (a corner's flourish: a spiral and a tail along the band, mirrored by sx and sy)
    for (int i = 0; i <= 40; i++) {
        float t = i / 40.0f, a = 3.6f + t * 6.0f, r = 3 + t * 11;
        dl->PathLineTo(P(cx + sx * cosf(a) * r, cy + sy * sinf(a) * r));
    }
    dl->PathBezierCubicCurveTo(P(cx + sx * 26, cy + sy * 10), P(cx + sx * 40, cy + sy * 2), P(cx + sx * 62, cy + sy * 4));
    dl->PathStroke(col, S(2.4f));
}

void frame(ImTextureID frame_tex, int tw, int th, ImTextureID grain) {
    if (frame_tex) {
        nine_slice(frame_tex, tw, th);
        return;
    }
    // Drawn: an outer leather plate with a copper rim, and a darker inner panel with a copper border
    ImVec2 pts[8];
    int n = octagon(10, 14, kW - 20, kH - 28, 30, pts);
    poly_textured(pts, n, grain, 256, rgb(0x5A3A22), rgb(0x3C2414), P(0, 14).y, P(0, kH - 14).y);
    chamfer_line(10, 14, kW - 20, kH - 28, 30, rgb(0x1A0E07), 4);
    chamfer_line(12, 16, kW - 24, kH - 32, 29, rgb(0xA8703F), 2.5f);
    chamfer_line(15, 19, kW - 30, kH - 38, 28, rgb(0xE0A878, 0.35f), 1);
    chamfer_line(22, 26, kW - 44, kH - 52, 24, rgb(0x6E4325, 0.8f), 1.2f);

    const float ix = 31, iy = 92, iw = kW - 62, ih = 733 - 92;
    chamfer_glow(ix, iy, iw, ih, 22, rgb(0, 0.7f), 10);
    n = octagon(ix, iy, iw, ih, 22, pts);
    poly_textured(pts, n, grain, 300, rgb(0x33200F), rgb(0x2A1A0C), P(0, iy).y, P(0, iy + ih).y);
    // (the vignette: darker towards the border)
    dl->PushClipRect(P(ix, iy), P(ix + iw, iy + ih), true);
    const float v = 90;
    dl->AddRectFilledMultiColor(P(ix, iy), P(ix + iw, iy + v), rgb(0, 0.5f), rgb(0, 0.5f), rgb(0, 0), rgb(0, 0));
    dl->AddRectFilledMultiColor(P(ix, iy + ih - v), P(ix + iw, iy + ih), rgb(0, 0), rgb(0, 0), rgb(0, 0.5f), rgb(0, 0.5f));
    dl->AddRectFilledMultiColor(P(ix, iy), P(ix + v * 1.3f, iy + ih), rgb(0, 0.45f), rgb(0, 0), rgb(0, 0), rgb(0, 0.45f));
    dl->AddRectFilledMultiColor(P(ix + iw - v * 1.3f, iy), P(ix + iw, iy + ih), rgb(0, 0), rgb(0, 0.45f), rgb(0, 0.45f), rgb(0, 0));
    dl->PopClipRect();
    chamfer_line(ix - 3, iy - 3, iw + 6, ih + 6, 24, rgb(0x1A0E07, 0.9f), 2);
    chamfer_line(ix, iy, iw, ih, 22, rgb(0xB0784A), 2.5f);
    chamfer_line(ix + 3, iy + 3, iw - 6, ih - 6, 20, rgb(0, 0.55f), 1.5f);

    const ImU32 cu = rgb(0xA8703F);
    curl(66, 58, 1, 1, cu);
    curl(kW - 66, 58, -1, 1, cu);
    curl(66, kH - 52, 1, -1, cu);
    curl(kW - 66, kH - 52, -1, -1, cu);
    // (the bottom band's middle: a diamond between two sweeps)
    const float by = 770;
    for (float s : {-1.0f, 1.0f}) {
        dl->PathLineTo(P(640 + s * 16, by));
        dl->PathBezierCubicCurveTo(P(640 + s * 60, by - 12), P(640 + s * 120, by - 10), P(640 + s * 170, by + 4));
        dl->PathStroke(cu, S(2.2f));
    }
    diamond(640, by, 11, rgb(0x1A0E07));
    diamond(640, by, 8.5f, rgb(0xC88A58));
}

void wordmark(ImTextureID tex, int tw, int th) {
    const float x = 358, y = 134, w = 565, h = 153;
    if (tex) {
        float k = fminf(w / tw, h / th), dw = tw * k, dh = th * k;
        dl->AddImage(ImTextureRef(tex), P(x + (w - dw) * 0.5f, y + (h - dh) * 0.5f), P(x + (w + dw) * 0.5f, y + (h + dh) * 0.5f));
        return;
    }
    // Drawn: the name in the serif, spaced, with the line under it
    const std::string name = "HOOKSHOTVR";
    float size = 78;
    float natural = 0;
    for (char c : name) natural += text_width(fonts.semibold, size, std::string(1, c));
    float spacing = 4;
    float fit = (w - 20 - spacing * (name.size() - 1)) / natural;
    if (fit < 1) size *= fit;
    spaced_text(fonts.semibold, size, x + w * 0.5f, y + 64, rgb(0xF6EEE2), name, spacing);
    spaced_text(fonts.semibold, 17, x + w * 0.5f, y + 124, rgb(0xEDE2D0), "AN UNOFFICIAL VR COMPATIBLE FORK OF HOOKSHOT", 2.2f);
}

}  // namespace draw
