// Hookshot VR: How the VR settings' values are shown (the VR menu's) (tx_vr_*: "Stereo", "Auto", "64 mm", ...).
//
// A piece of gpu/vk/vk_menu.cpp, included there at its VR HOOK (it uses that file's types and helpers). Not compiled on its own.
// (OoT VR) the VR settings' values
const char* tx_vr_recenter(int) { return "Press A"; }
const char* tx_vr_window_room(int v) { return v ? "Room" : "Dark"; }
const char* tx_vr_calibrate(int) { return "Hold neutral, press A"; }
const char* tx_vr_degrees(int v) {
    static char b[16];
    snprintf(b, sizeof b, "%+d deg", v);
    return b;
}
const char* tx_vr_width(int v) {
    static char b[16];
    snprintf(b, sizeof b, "%d deg", v);
    return b;
}
const char* tx_vr_shown(int v) { return v ? "Shown" : "Hidden"; }
const char* tx_vr_item_get_view(int v) { return v ? "Keep your view" : "Turn with Link"; }
const char* tx_vr_item_get(int v) { return v ? "In front of you" : "Over your head"; }
const char* tx_vr_radial_place(int v) { return v ? "In front" : "At the hand"; }
const char* tx_vr_wrist_hud(int v) { return v == 2 ? "Full" : v == 1 ? "Simple" : "Off"; }
const char* tx_vr_wrist_size(int v) { return v == 2 ? "Large" : v == 1 ? "Medium" : "Small"; }
const char* tx_vr_menu_button(int v) { return v ? "Both sticks" : "Menu or Y"; }
const char* tx_vr_menu_position(int v) { return v ? "Follows your head" : "Fixed in world"; }
const char* tx_vr_turn_style(int v) { return v ? "Snap" : "Smooth"; }
const char* tx_vr_snap_angle(int v) {
    static const char* t[] = {"30\xC2\xB0", "45\xC2\xB0", "60\xC2\xB0", "90\xC2\xB0"};
    return t[(unsigned)v < 4 ? v : 1];
}
const char* tx_vr_turn_speed(int v) {
    static const char* t[] = {"Slow", "Medium", "Fast", "Very fast"};
    return t[(unsigned)v < 4 ? v : 1];
}
const char* tx_vr_vignette(int v) {
    static const char* t[] = {"Off", "Low", "Medium", "High"};
    return t[(unsigned)v < 4 ? v : 0];
}
const char* tx_vr_events(int v) { return v == 2 ? "In 3D" : v == 1 ? "Screen" : "Flat"; }
const char* tx_vr_cinematic_view(int v) {
    return v == 3 ? "Follow Link" : v == 2 ? "Link's spot" : v == 1 ? "Theatre seat" : "Director";
}
const char* tx_vr_cinematic_light(int v) { return v == 3 ? "Area's" : v == 2 ? "Held" : v == 1 ? "Every shot" : "Auto"; }
const char* tx_vr_event_detail(int v) { return v ? "Game camera" : "Your view"; }
const char* tx_vr_realign(int v) {
    return v == 1 ? "Quick turn at the end" : v == 2 ? "Snap at the end" : v == 3 ? "Follow Link" : "Off";
}
const char* tx_vr_mm(int v) {   // (a length kept in mm)
    static char b[16];
    snprintf(b, sizeof b, "%d mm", v);
    return b;
}
const char* tx_vr_cm(int v) {   // (a length kept in cm)
    static char b[16];
    snprintf(b, sizeof b, "%d cm", v);
    return b;
}
const char* tx_vr_tenths_ms(int v) {   // (a speed kept in tenths of m/s)
    static char b[16];
    snprintf(b, sizeof b, "%d.%d m/s", v / 10, v % 10);
    return b;
}
const char* tx_vr_shield_pose(int v) { return v == 0 ? "Arm" : v == 2 ? "Fist" : "Guard turn"; }
const char* tx_vr_boomerang_throw(int v) {
    static const char* t[] = {"Aim with Boomerang Hand", "Aim with Empty Hand", "Aim with Headset"};
    return t[(unsigned)v < 3 ? v : 0];   // (a value saved when there were six, 3-5: the first)
}
const char* tx_vr_bomb_throw(int v) { return v ? "Squeeze again" : "Let go of the grip"; }
const char* tx_vr_decimetres(int v) {   // (a distance kept in decimetres, shown in metres)
    static char b[16];
    snprintf(b, sizeof b, "%.1f m", v * 0.1);
    return b;
}
const char* tx_vr_cone(int v) {
    static char b[16];
    snprintf(b, sizeof b, "%d deg", v);
    return b;
}
const char* tx_vr_string_input(int v) { return v == 1 ? "Trigger" : v == 2 ? "Grip" : "Grip or trigger"; }
const char* tx_vr_mode(int v) { return v ? "First person" : "Third person"; }
const char* tx_vr_move(int v) { return v ? "Headset" : "Camera"; }
// (the 3D modes, as the ini's 3d= has them: vr_options.inc's vr_stereo, with the switch that offers From depth)
constexpr int kVr3dMono = 0, kVr3dFromDepth = 1, kVr3dStereo = 4;
const char* tx_vr_stereo(int v) { return v == kVr3dStereo ? "Stereo" : v == kVr3dMono ? "Mono" : "From depth"; }
const char* tx_vr_near_depth(int v) { return v == 2 ? "Full" : v == 1 ? "Medium" : "Low"; }
const char* tx_vr_height(int v) {
    static char b[16];
    if (v <= 0) return "Auto";
    snprintf(b, sizeof b, "%d cm", v);
    return b;
}
const char* tx_vr_body_depth(int v) {
    static char b[24];
    if (!v) return "Centred";
    snprintf(b, sizeof b, "%d cm %s", std::abs(v), v > 0 ? "ahead" : "behind");
    return b;
}
const char* tx_vr_steps(int v) {
    static char b[16];
    snprintf(b, sizeof b, "%d steps", v);
    return b;
}
const char* tx_vr_distance(int v) {
    static const char* t[] = {"Low", "Switch 2", "High", "Ultra", "Off (headset's FOV)"};
    return t[(unsigned)v < 5 ? v : 1];
}
const char* tx_vr_eye(int v) {
    static char b[24];
    if (v <= 0) return "The picture's";
    snprintf(b, sizeof b, "%d pixels tall", v);
    return b;
}
