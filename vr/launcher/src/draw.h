// The canvas's look in ImGui draw calls (the Launcher board, Launcher.dc.html): everything is laid out in the board's
// 1280 x 820 frame and scaled to the window.
#pragma once
#include <string>
#include "imgui.h"

namespace draw {

// ---- colours, as the canvas names them ----
constexpr ImU32 rgb(unsigned hex, float a = 1.0f) {
    return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, (int)(a * 255 + 0.5f));
}
enum : unsigned {
    kBackground = 0x141010, kInk = 0xEEDCC0, kInkSoft = 0xE2D4B5, kInkWarm = 0xF2D9BA, kInkButton = 0xF0DEC2,
    kInkTitle = 0xFFF0D8, kInkMuted = 0xA8957A, kInkCard = 0xE3CDB0, kCopper = 0xD49A6A, kGreen = 0x7DBA6A,
    kAmber = 0xE3B04B, kRed = 0xD9634C, kDotIdle = 0x5A4636,
};

struct Fonts {
    ImFont *medium = nullptr, *semibold = nullptr, *italic = nullptr;
};
extern Fonts fonts;

// ---- the frame's space: 1280 x 820, centred in the window ----
constexpr float kW = 1280, kH = 820;
void begin(ImDrawList *dl, ImVec2 display);
ImVec2 P(float x, float y);          // a frame point in window units
float S(float v);                    // a frame length in window units
ImVec2 frame_mouse();                // the mouse in frame units
ImDrawList *list();

// ---- text ----
enum Align { Left, Center, Right };
// cy: the middle of the line
void text(ImFont *f, float size, float x, float cy, unsigned col, const std::string &s, Align a = Left, float alpha = 1,
          bool shadow = false);
float text_width(ImFont *f, float size, const std::string &s);
std::string fit(ImFont *f, float size, const std::string &s, float width);   // (cut with an ellipsis to fit)
void spaced_text(ImFont *f, float size, float cx, float cy, ImU32 col, const std::string &s, float spacing);

// ---- shapes (frame units) ----
// A rectangle with its corners cut by c (the canvas's .cf 12 px and .cf-sm 8 px clip paths)
void chamfer(float x, float y, float w, float h, float c, ImU32 col);
// The same, filled top to bottom through up to three stops (positions 0..1)
void chamfer_gradient(float x, float y, float w, float h, float c, ImU32 c0, ImU32 c1, ImU32 c2 = 0, float mid = 0.5f);
void chamfer_glow(float x, float y, float w, float h, float c, ImU32 col, float radius);
void chamfer_line(float x, float y, float w, float h, float c, ImU32 col, float thickness);
void dot(float cx, float cy, float d, ImU32 col, bool glow);
void row_rule(float x0, float x1, float y);   // the line under a row: dark, and a copper light below it

// ---- the canvas's controls (each returns true when clicked) ----
// id: anything unique this frame. hover gets set when the mouse is on it
bool hit(const char *id, float x, float y, float w, float h, bool enabled = true, bool *hover = nullptr);
// The small copper-rimmed button (Update Log, Choose, Show, Change...). strong: the lighter centre (an action)
bool small_button(const char *id, float x, float y, float w, float h, const std::string &label, bool strong, float size = 19);
// The big one (Play, Build, Cancel). kind 0 lit, 1 dark (Cancel), 2 waiting (not clickable)
bool big_button(const char *id, float x, float y, float w, float h, const std::string &label, int kind, float size);
// A mode card: Desktop Mode or VR Mode, with its drawn icon
bool mode_card(const char *id, float x, float y, float w, float h, const std::string &label, bool vr, bool selected,
               ImTextureID art);

// ---- icons ----
void icon_monitor(float cx, float cy, float size, ImU32 col);
void icon_headset(float cx, float cy, float size, ImU32 col);
void icon_gear(float cx, float cy, float size, ImU32 col);
void icon_check(float cx, float cy, float size, ImU32 col);
void arrow(float cx, float cy, float size, bool right, ImU32 col);
void diamond(float cx, float cy, float size, ImU32 col);

// ---- the frame and the wordmark ----
// The leather frame: the art slot's 9-slice when frame_tex is set, else drawn here. grain: the leather texture
void frame(ImTextureID frame_tex, int tex_w, int tex_h, ImTextureID grain);
void wordmark(ImTextureID tex, int tex_w, int tex_h);

// The launcher's own icon (the desktop entry's): the VR card's headset on a copper-rimmed tile, px square at the window's
// top left. Its corners are cut by app_icon_cut(px)
void app_icon(ImDrawList *dl, float px);
inline float app_icon_cut(float px) { return px * 0.14f; }

bool any_hover();   // the mouse is on something clickable this frame (the hand cursor)

}  // namespace draw
