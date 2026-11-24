// Hookshot VR: the VR menu's calls (vk_vr.h): its open state, the images the present's thread draws it into, the controllers
// at the panel, and what the game side is told of the controllers while it is open (panel_mask). The headset thread's part is
// vr_menu_layer.inc (PanelLayer panel).
//
// Included by vr/headset/vk_vr.cpp after its private helpers (frame_capture.inc's neighbour). Not compiled on its own.
#if __has_include("vk_vr_panel_frame.h")
#include "vk_vr_panel_frame.h"   // (the frame art, VR_PANEL_FRAME, when assets/vr/panel_frame.png was there at the build)
#endif

/// The frame art's straight RGBA pixels (vr/tools/generate/panel_frame.py; false: not in this build)
bool panel_frame_pixels(uint32_t* w, uint32_t* h, std::vector<unsigned char>& out) {
#ifdef VR_PANEL_FRAME
    const size_t size = (size_t)vr_panel_frame_w * vr_panel_frame_h * 4;
    out.resize(size);
    if (ZSTD_decompress(out.data(), size, vr_panel_frame_zst, sizeof vr_panel_frame_zst - 1) == size) {
        *w = vr_panel_frame_w, *h = vr_panel_frame_h;
        logf("vr: VR menu: the frame art (%ux%u)", *w, *h);
        return true;
    }
    logf("vr: VR menu: the frame art did not unpack: a drawn frame instead");
#endif
    out.clear();
    return false;
}

/// A game icon's straight RGBA pixels, size x size (vk_vr_icons.h, vr/tools/generate/icons.py; false: not in this build).
/// The VR menu's item sections show them
bool icon_pixels(const char* name, int* size, std::vector<unsigned char>& out) {
    for (const art::Icon& i : art::icons) {
        if (std::strcmp(i.name, name)) continue;
        const int n = art::icon_size;
        out.resize((size_t)n * n * 4);
        for (int k = 0; k < n * n; k++) {   // (stored premultiplied: back to straight, as Dear ImGui blends)
            const unsigned a = i.rgba[k * 4 + 3];
            for (int c = 0; c < 3; c++) out[k * 4 + c] = a ? (unsigned char)std::min(255u, (i.rgba[k * 4 + c] * 255u + a / 2) / a) : 0;
            out[k * 4 + 3] = (unsigned char)a;
        }
        *size = n;
        return true;
    }
    out.clear();
    return false;
}

bool panel_open() { return panel.open.load(std::memory_order_acquire); }
void panel_set_open(bool open) {
    if (panel.open.exchange(open, std::memory_order_acq_rel) == open) return;
    logf("vr: [%.1f s] VR menu %s", run_s(), open ? "opened" : "closed");
    if (open) {   // (the grips as they are now: what the hands hold stays held while the menu is open)
        std::lock_guard lk(s.pose_mu);
        if (!panel.holding)
            for (int h = 0; h < 2; h++) panel.grip_held[h] = ctl.shared[h].squeeze;
        panel.holding = true;
    }
}
bool window_options_changed() { return window_take_options_changed(); }   // (the Window view's Move mode: window_view.inc)
/// The hint's words from the buttons' names (each empty: not on this controller): Y is named alone, as only the left hand has it
std::string menu_hint_words(const std::string& menu, const std::string& upper, bool sticks, bool pads, bool value) {
    const std::string second = sticks ? (pads ? "both pads" : "both sticks") : upper;
    if (value) return sticks ? (pads ? "Both pads" : "Both sticks") : upper.empty() ? "Menu" : "Menu or " + upper;
    std::string out = menu.empty() ? "" : "left " + menu;
    if (!second.empty()) out += (out.empty() ? "" : " or ") + std::string(sticks || second == "Y" ? "" : "left ") + second;
    return out.empty() ? "F2" : "Hold " + out;
}
std::string menu_hint(bool value) {
    std::lock_guard lk(s.pose_mu);
    return menu_hint_words(ctl.menu_name, ctl.upper_name, opts.vr_menu_button == 1, ctl.pads, value);
}
bool panel_toggle() {
    if (!panel_open() && menu_is_open()) return false;   // (not over Hookshot's own menu)
    panel_set_open(!panel_open());
    return true;
}

/// (host_api.inc, the game side's hands) what the game is told of hand h while the menu holds the controllers: buttons (but
/// Menu), trigger and stick let go, the grip as when it opened. The Menu button row's button also comes as MENU2, menu or
/// not: its hold opens and closes the menu (vr/input/controllers_host.c vr_menu_y_step). pose_mu held
void panel_mask(int h, float& trigger, float& squeeze, float* stick, uint32_t* buttons) {
    const uint32_t second = opts.vr_menu_button == 1 ? HOOKSHOT_VR_BTN_STICK : h ? 0 : HOOKSHOT_VR_BTN_UPPER;
    if (buttons && (*buttons & second)) *buttons |= HOOKSHOT_VR_BTN_MENU2;
    if (!panel.holding) return;
    if (!panel_open()) {   // (closed: held until everything is let go, so the press that closed it stays the menu's)
        bool down = false;
        for (int k = 0; k < 2; k++) down |= (ctl.shared[k].buttons & ~(uint32_t)HOOKSHOT_VR_BTN_MENU) || ctl.shared[k].trigger > .3f;
        if (!down) {
            panel.holding = false;
            return;
        }
    }
    trigger = 0;
    squeeze = std::min(squeeze, panel.grip_held[h]);   // (a grip let go meanwhile is let go: it is not squeezed again)
    panel.grip_held[h] = squeeze;
    if (stick) stick[0] = stick[1] = 0;
    if (buttons) *buttons &= HOOKSHOT_VR_BTN_MENU | HOOKSHOT_VR_BTN_MENU2;
}

bool panel_hands(PanelHand out[2]) {
    std::lock_guard lk(s.pose_mu);
    for (int h = 0; h < 2; h++) {
        out[h] = panel.hands[h];
        const HandState& hs = ctl.shared[h];
        out[h].valid = ctl.attached && hs.valid;
        out[h].trigger = hs.trigger, out[h].buttons = hs.buttons, out[h].stick[0] = hs.stick[0], out[h].stick[1] = hs.stick[1];
    }
    return ctl.attached;
}

// ---- the present's thread: the images (as menu_target, menu_ready and menu_hidden, frame_capture.inc) ----
VkImage panel_target(uint32_t width, uint32_t height, VkImageView* view) {
    if (view) *view = VK_NULL_HANDLE;
    if (!active() || !view || !width || !height || dev.swap_view_format == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (as menu_target: make_image may destroy a slot's image the ring's commands name)
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    std::lock_guard lk(panel.mu);
    int k = panel.next;
    if (k == panel.reading.load(std::memory_order_acquire)) k = (k + 1) % kPanelSlots;
    panel.next = (k + 1) % kPanelSlots;
    MenuSlot& sl = panel.slots[k];
    if (!make_image(sl.color, dev.swap_view_format, usage, width, height)) return VK_NULL_HANDLE;
    sl.value = 0;
    panel.writing = k;
    *view = sl.color.view;
    return sl.color.image;
}
void panel_ready(VkCommandBuffer cmd) {
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (as menu_ready: records into the present's command buffer directly)
    std::lock_guard lk(panel.mu);
    if (panel.writing < 0) return;
    MenuSlot& sl = panel.slots[panel.writing];
    barrier(cmd, sl.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    {   // (the present's submit signals it: frame_capture.inc present_signal)
        std::lock_guard sk(s.mu);
        sl.value = ++s.timeline_value;
        s.pending_value = sl.value;
    }
    panel.writing = -1;
    panel.visible.store(true, std::memory_order_release);
}
void panel_hidden() {
    std::lock_guard lk(panel.mu);
    panel.writing = -1;
    panel.visible.store(false, std::memory_order_release);
}

