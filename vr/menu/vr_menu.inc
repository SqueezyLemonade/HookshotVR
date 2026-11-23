// Hookshot VR: THE VR MENU, the mod's own settings, a panel floating in the world in the headset. Separate from Hookshot's own
// (Insert) menu, which has none of the mod's rows (no VR tab): VR itself is decided at the start (HOOKSHOT_VR, the launcher's),
// Replay threads is upstream's own again (RX_VK_PARALLEL, off: the late pages' fix needs one thread), and Controller mapping
// here opens Hookshot's Controls page.
//
// Included at the end of gpu/vk/vk_menu.cpp (its VR HOOK, after that file's namespace): it uses that file's rows (rows[],
// set_row, menu_mu: every value here is a row of hookshot_options.ini, saved as the Insert menu saves it) and Dear ImGui
// (vk_imgui.h, vk_ui.h). Not compiled on its own. vr/menu/vr_menu_layer.inc shows the image in the headset; the game side
// opens and closes it (the left Menu button held half a second: vr/input/controllers_host.c vr_menu_hold_step).
//
// The layout is the user's design (19 Nov: the design canvas's tabs array and its CSS): tabs
// along the top, the tab's sections down the left, the section's rows on the right; a row is a value with arrows either side
// (step: On/Off rows too, as the user asked after the first headset test), a button (action) or two large cards (cards). Rows the canvas marks Planned are left
// out until their feature is built, and so are sections left with none. A row that only works with another row's value in
// the same section is hidden otherwise (Depth quality only with 3D From depth, ...); one that needs something from elsewhere
// is greyed with the reason (first-person rows in third person: "First person only"). Under the rows, the selected row's
// help (the Insert menu's own text for it).
//
// How it looks: the user's frame art (assets/vr/panel_frame.png, a 9-slice 215 150 226 150, compiled in from the build's
// vr_generated/vk_vr_panel_frame.h when the file is there: vr/tools/generate/panel_frame.py; else a drawn copper frame), the
// canvas's copper-bevel chamfered buttons, the selected one with a warm glow. Each section has an icon left of its name: an
// item's section the game's own icon (vk_vr_icons.h, through vr::icon_pixels), the others drawn in copper (vrm_icon), as are
// the 3D cards' pictures. The font: Libertinus Serif Semibold, the
// overlays' own bundled serif (third_party/fonts, SIL OFL), standing in for the canvas's EB Garamond until that is added
// (vrm_font: one place to change).
//
// Why this way: the panel is drawn as 2D with Dear ImGui into an image of its own (the Insert menu's text drawing, its font
// atlas and its Vulkan backend), on the present's thread, and the headset shows that image as one quad layer in the world
// (vr_menu_layer.inc). Text, gradients and the frame art come for free and sharp, it costs one small render pass when
// something on it changes (not while it sits still), and the same path can carry the planned world-space HUD. The radial's
// way (pixels drawn on the CPU) has no text and would need a font rasterizer of its own.
//
// Controls: either controller's pointer (its aim ray) and trigger; or either stick and A (select) / B (back) (X and Y on the
// left controller too). Left and right change a row's value; up and down move; B goes back from the rows to the sections, to
// the tabs, and closes the menu from there. While it is open the game gets the controllers let go: Link stands still.
// Headless: RX_VR_MENU_TEST=<seconds>[,cycle] opens it that many seconds after the first present (cycle: a tab and a section
// further every 3 s) and draws it into the shots (RX_VK_SHOT_*), for a picture of it without the headset.
// (No #include here: vk_work.h stays vk_menu.cpp's last, tests/test_recorder_sync.py. The frame art is unpacked on the VR side,
// vr_menu_layer.inc panel_frame_pixels.)
namespace ImGui {
void RegisterUserTexture(ImTextureData* tex);   // (imgui_internal.h's: the frame art, as vk_imgui.cpp's logo)
}
extern const unsigned char hookshot_font_heading[];   // (nx2/ui_fonts.py: Libertinus Serif Semibold)
extern const unsigned hookshot_font_heading_size;

namespace mkwvk {
namespace {

// ---- the layout (the canvas's tabs array, less what is Planned) -----------------------------------------------------------
enum class VrmKind : unsigned char { step, action, cards, slider, info };   // (slider: dragged with a laser, or stepped; info:
                                                                           // a label and its words, nothing to change)
enum : unsigned {
    kVrmFirstPerson = 1,   // (greyed in third person: "First person only")
    kVrmFromDepth = 2,     // (hidden unless 3D is From depth)
    kVrmReticle = 4,       // (hidden unless Aim reticle is on)
    kVrmPull = 8,          // (hidden unless Distance grab is on)
    kVrmSnap = 16,         // (hidden unless Turn style is Snap)
    kVrmSmooth = 32,       // (hidden unless Turn style is Smooth)
    kVrm3dCutscenes = 64,  // (hidden unless Cutscene view is In 3D)
    kVrmWindow = 128,      // (greyed unless View is Window: "Window view only")
    kVrmFree = 256,        // (hidden unless Keep 16:9 is off)
    kVrmThirdPerson = 512, // (greyed in first person: "Third person only"; Window view is third person)
};
struct VrmCard {
    const char* label;
    const char* desc;
    int value;
    const char* icon;            // (its picture, as a section's icon: vrm_icon; nullptr: none, three cards have none)
};
struct VrmRow {
    const char* name;            // (as this menu calls it; its saved key is its Insert menu row's, by the value)
    int* value;                  // (the opts field: its Row in rows[] gives the range, the step, the words and the help)
    VrmKind kind;
    unsigned when;               // (kVrm*)
    const char* labels;          // (this menu's own words for the values, "|" between, from the row's lo; nullptr: the Row's)
    const char* button;          // (an action's button)
    VrmCard cards[3];            // (two, or three: View's Window)
};
struct VrmSection {
    const char* name;
    const VrmRow* rows;
    int n;
    const char* icon;            // (left of its name: a game icon's name (an item's section), or "@..." one drawn in copper: vrm_icon)
};
struct VrmTab {
    const char* name;
    const VrmSection* sections;
    int n;
};
#define VRM_STEP(name, value, when) {name, &opts.value, VrmKind::step, when, nullptr, nullptr, {}}
#define VRM_SLIDE(name, value, when) {name, &opts.value, VrmKind::slider, when, nullptr, nullptr, {}}
#define VRM_WORDS(name, value, when, words) {name, &opts.value, VrmKind::step, when, words, nullptr, {}}
#define VRM_ONOFF(name, value, when) {name, &opts.value, VrmKind::step, when, nullptr, nullptr, {}}   // (◀ On ▶: a step of two)
#define VRM_ACTION(name, value, when, button) {name, &opts.value, VrmKind::action, when, nullptr, button, {}}
#define VRM_PAGE(name, when, button) {name, nullptr, VrmKind::action, when, nullptr, button, {}}   // (no value: opens a page, vrm_open)
#define VRM_INFO(name, words) {name, nullptr, VrmKind::info, 0, nullptr, words, {}}   // (no value: its words where a value shows)
#define VRM_N(a) (int)(sizeof a / sizeof a[0])
#define VRM_SECTION(name, rows, icon) {name, rows, VRM_N(rows), icon}
constexpr unsigned kFP = kVrmFirstPerson;
const VrmRow vrm_play[] = {
    VRM_ACTION("Recenter view", vr_recenter, 0, "Recenter"),
    {"View", &opts.vr_mode, VrmKind::cards, 0, nullptr, nullptr,
     {{"First person", "Your own view, as Link.", 1}, {"Third person", "The game\xE2\x80\x99s camera, behind Link.", 0},
      {"Window Beta", "Through a frame in your room", 2}}},   // (2: third person, opts.vr_window: vrm_card_now)
    {"3D", &opts.vr_stereo, VrmKind::cards, 0, nullptr, nullptr,
     {{"From depth", "Faster.", 1, "@depth"}, {"True stereo", "Sharper depth, slower.", 4, "@stereo"}}},
};
const VrmRow vrm_window[] = {   // (the Window view's frame: vr/headset/window_view.inc)
    VRM_WORDS("Background", vr_window_room, kVrmWindow, "Dark|Room"),
    VRM_ACTION("Move", vr_window_move, kVrmWindow, "Move"),
    VRM_ACTION("Place window", vr_window_place, kVrmWindow, "Place"),
    VRM_SLIDE("Size", vr_window_width, kVrmWindow),
    VRM_ONOFF("Keep 16:9", vr_window_169, kVrmWindow),
    VRM_SLIDE("Height", vr_window_height, kVrmWindow | kVrmFree),
    VRM_SLIDE("Roundness", vr_window_round, kVrmWindow),
    VRM_SLIDE("Feather", vr_window_feather, kVrmWindow),
    VRM_SLIDE("Tilt", vr_window_tilt, kVrmWindow),
    VRM_SLIDE("Distance", vr_window_distance, kVrmWindow),
    VRM_ONOFF("Eye camera", vr_window_eye_camera, kVrmWindow),   // (experimental: window_view.inc window_look)
    VRM_ONOFF("Back panel", vr_window_back, kVrmWindow),
};
const VrmRow vrm_headset[] = {
    VRM_STEP("Eye height", vr_height, kFP),
    VRM_STEP("HUD size", vr_hud_size, 0),
    VRM_WORDS("Wrist HUD", vr_wrist_hud, 0, "Off|Simple|Full"),
    VRM_WORDS("Wrist size", vr_wrist_size, 0, "Small|Medium|Large"),
    VRM_STEP("Menu position", vr_menu_position, 0),
};
const VrmRow vrm_turning[] = {
    VRM_STEP("Turn style", vr_turn_style, kFP),
    VRM_STEP("Snap angle", vr_snap_angle, kFP | kVrmSnap),
    VRM_STEP("Turn speed", vr_turn_speed, kFP | kVrmSmooth),
};
const VrmRow vrm_moving[] = {
    VRM_STEP("Move direction", vr_move_head, kFP),
};
const VrmRow vrm_aids[] = {   // (a dark ring round both eyes: vr/comfort/vignette_layer.inc)
    VRM_STEP("Move vignette", vr_vignette_move, kFP),
    VRM_STEP("Turn vignette", vr_vignette_turn, kFP),
    VRM_STEP("Air vignette", vr_vignette_air, kFP),
    VRM_STEP("Carry vignette", vr_vignette_carried, kFP),
};
const VrmRow vrm_body[] = {
    VRM_STEP("Arm length", vr_arm_length, kFP),
    VRM_STEP("Body alignment", vr_body_depth, kFP),
    VRM_STEP("Crawl eye height", vr_crawl_eye, kFP),
    VRM_ACTION("Calibrate wrists", vr_wrist_calibrate, kFP, "Calibrate"),
    VRM_STEP("Item wheel opens", vr_radial_place, kFP),
};
const VrmRow vrm_controls[] = {   // (Hookshot VR's own controls, first, each action and how it is done; then the game's buttons)
    VRM_INFO("Menu", "Hold left menu"),
    VRM_INFO("Items wheel", "Left stick click"),
    VRM_INFO("Gear wheel", "Right stick click"),
    VRM_INFO("Walk", "Left stick"),
    VRM_INFO("Turn", "Right stick"),
    VRM_INFO("Jump", "Right stick up"),
    VRM_INFO("Wrist HUD", "Wrist up"),
    VRM_INFO("Draw sword", "Grip over shoulder"),
    VRM_INFO("Slash", "Swing"),
    VRM_INFO("Spin", "Trigger"),
    VRM_INFO("Lock on", "Shield trigger"),
    VRM_INFO("Hold item", "Grip"),
    VRM_INFO("Use item", "Trigger"),
    VRM_INFO("Throw", "Release while moving"),
    VRM_INFO("Bow", "Pull string"),
    VRM_PAGE("Controller mapping", kVrmThirdPerson, "Open"),   // (Hookshot's Controls page: the planned button remapping joins it here)
};
const VrmRow vrm_sword[] = {
    VRM_STEP("Sword angle", vr_sword_angle, kFP),
    VRM_STEP("Swing arc", vr_swing_arc, kFP),
    VRM_STEP("Swing speed", vr_swing_speed, kFP),
    VRM_ONOFF("Hit buzz", vr_hit_buzz, kFP),
};
const VrmRow vrm_shield[] = {
    VRM_STEP("Shield when guarding", vr_shield_pose, kFP),   // (today's guard pose, until the canvas's Guard with is built)
};
const VrmRow vrm_bow[] = {
    VRM_STEP("Bow size", vr_bow_scale, kFP),
    VRM_STEP("String grab with", vr_bow_string_trigger, kFP),
    VRM_ONOFF("Aim reticle", vr_bow_preview, 0),   // (both views: third person's where the shot lands, vr/items/aim)
    VRM_STEP("Aim reticle size", vr_bow_reticle_size, kVrmReticle),
    VRM_STEP("Elemental arrow effects", vr_arrow_fx_opacity, kFP),
};
const VrmRow vrm_bombs[] = {
    VRM_WORDS("Grip", vr_bomb_throw_mode, kFP, "Hold|Press to toggle"),
    VRM_STEP("Throw strength", vr_bomb_throw, kFP),
    VRM_STEP("Bomb size", vr_bomb_scale, kFP),
    VRM_ONOFF("Distance grab", vr_bomb_pull, kFP),
    VRM_STEP("Distance grab reach", vr_bomb_pull_reach, kFP | kVrmPull),
    VRM_STEP("Distance grab aim", vr_bomb_pull_cone, kFP | kVrmPull),
};
const VrmRow vrm_boomerang[] = {
    VRM_WORDS("Aim with", vr_boomerang_throw, kFP, "Boomerang hand|Empty hand|Headset"),
};
const VrmRow vrm_ocarina[] = {
    VRM_STEP("Song effects", vr_song_effects, kFP),
};
const VrmRow vrm_cutscenes[] = {
    VRM_STEP("Cutscene view", vr_events, 0),   // (talks always in VR but by Flat; story cutscenes: vr/camera/events_host.c)
    VRM_STEP("3D cutscenes", vr_cinematic_view, kVrm3dCutscenes),
    VRM_STEP("Cutscene light", vr_cinematic_light, 0),
    VRM_STEP("Screen size", vr_cutscene_screen, 0),
    VRM_WORDS("Re-align after events", vr_realign, kFP, "Off|Quick turn|Snap at the end|Follow Link"),
    VRM_STEP("Event culling", vr_event_detail, kFP),
};
const VrmRow vrm_item_get[] = {
    VRM_STEP("Item Get view", vr_item_get_view, kFP),
    VRM_STEP("Item shown", vr_item_get, kFP),
};
const VrmRow vrm_picture[] = {
    VRM_STEP("Eye resolution", vr_eye_height, 0),
    VRM_STEP("Depth quality", vr_quality, kVrmFromDepth),
    VRM_STEP("3D strength", vr_depth_scale, 0),   // (both 3D modes: vk_vr_eyes.cpp and eye_rendering.inc read it)
};
const VrmRow vrm_distance[] = {
    VRM_STEP("Detail distance", vr_detail, 0),
    VRM_STEP("Foliage distance", vr_foliage, 0),
};
const VrmRow vrm_tests[] = {   // (the Debug tab: shown only with HOOKSHOT_VR_DEBUG=1, vrm_tab_count)
    VRM_ONOFF("GPU profile", vr_gpu_profile, 0),
    VRM_ONOFF("Show hands", vr_show_hands, 0),
};
// (each section's icon: an item's section the game's own icon, child Link's where he has his own; the rest drawn in copper)
const VrmSection vrm_general[] = {VRM_SECTION("Play Mode", vrm_play, "@headset"), VRM_SECTION("Window", vrm_window, "@window"),
                                  VRM_SECTION("Headset", vrm_headset, "@eye")};
const VrmSection vrm_comfort[] = {VRM_SECTION("Turning", vrm_turning, "@turn"), VRM_SECTION("Moving", vrm_moving, "@steps"),
                                  VRM_SECTION("Comfort Aids", vrm_aids, "@vignette")};
const VrmSection vrm_body_tab[] = {VRM_SECTION("Body & Hands", vrm_body, "@hand"), VRM_SECTION("Controls", vrm_controls, "@controller")};
const VrmSection vrm_items[] = {VRM_SECTION("Sword", vrm_sword, "Weapon_Sword_Kokiri"), VRM_SECTION("Shield", vrm_shield, "Weapon_Shield_Deku"),
                                VRM_SECTION("Bow", vrm_bow, "Weapon_Range_Bow"), VRM_SECTION("Bombs", vrm_bombs, "Item_Bomb"),
                                VRM_SECTION("Boomerang", vrm_boomerang, "Item_Boomerang"),
                                VRM_SECTION("Ocarina", vrm_ocarina, "Item_Ocarina_Fairy")};
const VrmSection vrm_events[] = {VRM_SECTION("Cutscenes", vrm_cutscenes, "@clapper"), VRM_SECTION("Item Get", vrm_item_get, "@chest")};
const VrmSection vrm_graphics[] = {VRM_SECTION("Headset Picture", vrm_picture, "@picture"),
                                   VRM_SECTION("Distance", vrm_distance, "@trees")};
const VrmSection vrm_debug[] = {VRM_SECTION("Tests", vrm_tests, "@wrench")};
const VrmTab vrm_tabs[] = {{"General", vrm_general, VRM_N(vrm_general)}, {"Comfort", vrm_comfort, VRM_N(vrm_comfort)},
                           {"Body", vrm_body_tab, VRM_N(vrm_body_tab)},  {"Items", vrm_items, VRM_N(vrm_items)},
                           {"Events", vrm_events, VRM_N(vrm_events)},    {"Graphics", vrm_graphics, VRM_N(vrm_graphics)},
                           {"Debug", vrm_debug, VRM_N(vrm_debug)}};
constexpr int kVrmTabs = VRM_N(vrm_tabs);
/// The tabs shown: the last, Debug, only with HOOKSHOT_VR_DEBUG=1 (a player's menu has none of its rows)
int vrm_tab_count() {
    static const int n = env_on("HOOKSHOT_VR_DEBUG") ? kVrmTabs : kVrmTabs - 1;
    return n;
}
#undef VRM_STEP
#undef VRM_SLIDE
#undef VRM_WORDS
#undef VRM_ONOFF
#undef VRM_ACTION
#undef VRM_PAGE
#undef VRM_INFO
#undef VRM_SECTION

/// The Insert menu's row of this value (its range, words, help, saved key), or null
Row* vrm_row_of(const int* value) {
    if (!value) return nullptr;   // (a page's row: no value)
    for (Row& r : rows)
        if (r.value == value) return &r;
    return nullptr;
}
/// Hidden: its switch, in the same section, is off
bool vrm_hidden(const VrmRow& r) {
    return ((r.when & kVrmFromDepth) && opts.vr_stereo == 4) || ((r.when & kVrmReticle) && !opts.vr_bow_preview) ||
           ((r.when & kVrmPull) && !opts.vr_bomb_pull) || ((r.when & kVrmSnap) && opts.vr_turn_style != 1) ||
           ((r.when & kVrmSmooth) && opts.vr_turn_style == 1) || ((r.when & kVrm3dCutscenes) && opts.vr_events != 2) ||
           ((r.when & kVrmFree) && opts.vr_window_169);
}
/// Greyed, and why (null: it works now)
const char* vrm_greyed(const VrmRow& r) {
    if ((r.when & kVrmFirstPerson) && opts.vr_mode != 1) return "First person only";
    if ((r.when & kVrmThirdPerson) && opts.vr_mode == 1) return "Third person only";
    if ((r.when & kVrmWindow) && !(opts.vr_window && opts.vr_mode == 0)) return "Window view only";
    return nullptr;
}
/// A cards row's cards: two, or three (View's Window)
int vrm_cards(const VrmRow& r) { return r.cards[2].label ? 3 : 2; }
/// The value a cards row has now: View's is 2 (Window) in third person with opts.vr_window on
int vrm_card_now(const VrmRow& r) { return r.value == &opts.vr_mode && opts.vr_mode == 0 && opts.vr_window ? 2 : *r.value; }
/// The card of the value it has now (the first when none)
int vrm_card_of(const VrmRow& r) {
    for (int c = 0; c < vrm_cards(r); c++)
        if (r.cards[c].value == vrm_card_now(r)) return c;
    return 0;
}
/// A section's rows now shown: their indexes into its rows, in out; how many
constexpr int kVrmShownMax = 64;
int vrm_shown(const VrmSection& s, int* out) {
    int n = 0;
    for (int i = 0; i < s.n && n < kVrmShownMax; i++)
        if (!vrm_hidden(s.rows[i])) out[n++] = i;
    return n;
}
/// A section's scroll: rows of these heights in a room this tall. *first (the first row shown) is moved so the selected row
/// (row) is in sight when follow, and never past where the last rows fill the room; *visible: how many whole rows fit from it
void vrm_scroll(const float* heights, int n, float room, int row, bool follow, int* first, int* visible) {
    int last_first = n;   // (the furthest it scrolls)
    for (float used = 0; last_first > 0 && used + heights[last_first - 1] <= room + .5f;) used += heights[--last_first];
    if (follow && row >= 0 && row < n) {
        if (row < *first) *first = row;
        float used = 0;
        for (int i = *first; i <= row; i++) used += heights[i];
        while (used > room + .5f && *first < row) used -= heights[(*first)++];
    }
    *first = std::clamp(*first, 0, std::max(0, std::min(last_first, n - 1)));
    float used = 0;
    *visible = 0;
    for (int i = *first; i < n && used + heights[i] <= room + .5f; i++) used += heights[i], ++*visible;
    if (!*visible && *first < n) *visible = 1;   // (a row taller than the room: shown, cut by the frame)
}
/// The words of a row's value
std::string vrm_value_text(const VrmRow& r) {
    const Row* row = vrm_row_of(r.value);
    if (!row) return "";
    const int v = *r.value;
    if (r.labels) {
        int k = (v - row->lo) / std::max(1, row->step);
        const char* p = r.labels;
        while (k > 0 && (p = strchr(p, '|'))) p++, k--;
        if (p && k == 0) return std::string(p, strcspn(p, "|"));
    }
    return row->text ? row->text(v) : std::to_string(v);
}
constexpr const char* kVrmVrControlsHelp =
    "How each action is done with the VR controllers, in first person: the hands do what the game's buttons would.";
constexpr const char* kVrmControlsHelp =
    "Which controller button or button combination presses each of the Switch's controls (Hookshot's Controls page, with the "
    "Quest buttons as a gamepad's). Opens in Hookshot's own menu; B there goes back.";
/// A page's row (no value): Controller mapping opens Hookshot's Controls page, as its Controls tab's Controls row does, and
/// the VR menu closes
void vrm_open(const VrmRow& r) {
    if (strcmp(r.name, "Controller mapping") || !can_bind()) return;
    vr::panel_set_open(false);
    quick_debug = false, tune_mode = false, listening = 0;
    opts.pad_layout = detect_pad_layout();   // (as menu_tick does as it opens)
    open_now = true, page = 1, csel = 0, ccol = menu_hit::kCtlPrimary, menu_refresh = true;
    logf("vr: VR menu: Controller mapping: Hookshot's Controls page opened");
}
/// Steps a row's value by dir: a list of a few words goes round, a scale stops at its ends. Saved, as the Insert menu saves
void vrm_change(const VrmRow& r, int dir) {
    if (!r.value) return vrm_greyed(r) ? void() : vrm_open(r);
    Row* row = vrm_row_of(r.value);
    if (!row || vrm_greyed(r)) return;
    std::lock_guard lk(menu_mu);
    const int step = std::max(1, row->step), count = (row->hi - row->lo) / step + 1;
    int v = *r.value;
    if (r.kind == VrmKind::action) v = v + 1 > row->hi ? row->lo : v + 1;   // (a change: Recenter, Calibrate wrists)
    else if (count <= 4) v = v + dir * step > row->hi ? row->lo : v + dir * step < row->lo ? row->hi : v + dir * step;
    else v = std::clamp(v + dir * step, row->lo, row->hi);
    if (v == *r.value && r.kind != VrmKind::action) return;
    set_row(*row, v, dir);
    logf("vr: VR menu: %s = %s", r.name, vrm_value_text(r).c_str());
}
void vrm_set(const VrmRow& r, int v) {   // (a card: its value)
    Row* row = vrm_row_of(r.value);
    if (!row || vrm_greyed(r) || vrm_card_now(r) == v) return;
    std::lock_guard lk(menu_mu);
    if (r.value == &opts.vr_mode) {   // (View's Window: third person with the window on; the other two with it off)
        if (Row* w = vrm_row_of(&opts.vr_window); w && opts.vr_window != (v == 2)) set_row(*w, v == 2, v == 2 ? 1 : -1);
        if (v == 2) v = 0;
    }
    if (*r.value != v) set_row(*row, v, v > *r.value ? 1 : -1);
    logf("vr: VR menu: %s = %s", r.name, r.kind == VrmKind::cards ? r.cards[vrm_card_of(r)].label : vrm_value_text(r).c_str());
}

// ---- the panel's size and places (the canvas's, in its own pixels) ----
constexpr float kVrmW = 1600, kVrmH = 1040;
constexpr float kVrmNavX = 100, kVrmNavW = 356, kVrmNavY = 176, kVrmNavH = 78, kVrmNavGap = 12;
constexpr float kVrmMainX = 517, kVrmMainR = 1500, kVrmRowsY = 260, kVrmRowH = 72, kVrmCardsH = 228, kVrmBodyBottom = 878;
constexpr float kVrmRowsBottom = 792;   // (the rows' room, Play Mode's three rows just fit: a longer section scrolls; its help
                                         // under it from 796)

enum VrmHitKind { kHitTab, kHitSection, kHitRow, kHitLess, kHitMore, kHitCard, kHitScroll, kHitSlider };   // (kHitScroll: sub -1 up a page, 1 down)
struct VrmHit {
    float x0, y0, x1, y1;
    int kind, index, sub;
};

// ---- its state (the present's thread) ----
struct VrmState {
    bool was_open = false;
    int tab = 0, sec[kVrmTabs]{}, row = 0, card = 0;
    int focus = 2;                     // (0 the tabs, 1 the sections, 2 the rows)
    bool select_was = false, back_was = false, trigger_was[2]{};
    int stick_dir = 0;                 // (the stick's direction held: 1 up, 2 down, 3 left, 4 right; 0 none)
    int64_t stick_next = 0;
    int pointer = 1;                   // (the hand that pointed last)
    float last_u[2]{-1, -1}, last_v[2]{-1, -1};
    int drag[2]{-1, -1};               // (a slider's row each hand's trigger holds, -1 none; its track's ends)
    float drag_x0[2]{}, drag_x1[2]{};
    std::vector<VrmHit> hits;          // (where each thing was drawn last: what a pointer is over)
    uint64_t drawn = 0;                // (what was drawn last, as a hash: drawn again only when it changes)
    int64_t drawn_at = 0;
    vr::PanelHand hands[2];
    int first = 0, first_of = -1;      // (a long section's first row shown, and which section that is for: tab x 64 + section)
    int visible = 0;                   // (how many of its rows were shown, last drawn)
} vrm;

const VrmSection& vrm_section() {
    const VrmTab& t = vrm_tabs[vrm.tab];
    return t.sections[std::clamp(vrm.sec[vrm.tab], 0, t.n - 1)];
}
void vrm_buzz(int hand) { hookshot_vr_rumble(hand, .12f, .012f); }

/// The selection's moves: a tab, a section, a row (in the rows now shown), a card
void vrm_move(int dir) {   // (1 up, 2 down, 3 left, 4 right)
    const VrmTab& t = vrm_tabs[vrm.tab];
    int shown[kVrmShownMax];
    const int n = vrm_shown(vrm_section(), shown);
    vrm.row = std::clamp(vrm.row, 0, std::max(0, n - 1));
    if (vrm.focus == 0) {
        if (dir == 3 || dir == 4) vrm.tab = (vrm.tab + (dir == 4 ? 1 : vrm_tab_count() - 1)) % vrm_tab_count(), vrm.row = 0, vrm.card = 0;
        if (dir == 2) vrm.focus = 1;
    } else if (vrm.focus == 1) {
        int& s = vrm.sec[vrm.tab];
        if (dir == 1 && s == 0) vrm.focus = 0;
        else if (dir == 1 || dir == 2) s = std::clamp(s + (dir == 2 ? 1 : -1), 0, t.n - 1), vrm.row = 0, vrm.card = 0;
        if (dir == 4 && n) vrm.focus = 2;
    } else if (n) {
        const VrmRow& r = vrm_section().rows[shown[vrm.row]];
        if (dir == 1 && vrm.row == 0) vrm.focus = 0;
        else if (dir == 1 || dir == 2) {
            vrm.row = std::clamp(vrm.row + (dir == 2 ? 1 : -1), 0, n - 1);
            const VrmRow& now = vrm_section().rows[shown[vrm.row]];
            vrm.card = now.kind == VrmKind::cards ? vrm_card_of(now) : 0;
        } else if (r.kind == VrmKind::cards) {
            vrm.card = std::clamp(vrm.card + (dir == 4 ? 1 : -1), 0, vrm_cards(r) - 1);
            vrm_set(r, r.cards[vrm.card].value);
        } else if (r.kind == VrmKind::step || r.kind == VrmKind::slider) {
            vrm_change(r, dir == 4 ? 1 : -1);
        }
    }
}
/// A: the focused thing's own action (a pointer's trigger presses what it is over: vrm_pointer)
void vrm_select() {
    int shown[kVrmShownMax];
    const int n = vrm_shown(vrm_section(), shown);
    if (vrm.focus == 0) vrm.focus = 1;
    else if (vrm.focus == 1) vrm.focus = n ? 2 : 1, vrm.row = 0;
    else if (n) {
        const VrmRow& r = vrm_section().rows[shown[std::clamp(vrm.row, 0, n - 1)]];
        if (r.kind == VrmKind::cards) vrm.card = (vrm.card + 1) % vrm_cards(r), vrm_set(r, r.cards[vrm.card].value);
        else vrm_change(r, 1);
    }
}
/// B: back a level; from the tabs, the menu closes
void vrm_back() {
    if (vrm.focus > 0) vrm.focus--;
    else vr::panel_set_open(false);
}
/// A slider held by hand's pointer, at the canvas's x px: its value from where along its track (saved once let go)
void vrm_slide(int hand, float px) {
    int shown[kVrmShownMax];
    const int n = vrm_shown(vrm_section(), shown);
    if (vrm.drag[hand] < 0 || vrm.drag[hand] >= n) return void(vrm.drag[hand] = -1);
    const VrmRow& r = vrm_section().rows[shown[vrm.drag[hand]]];
    Row* row = vrm_row_of(r.value);
    if (!row || vrm_greyed(r) || !(vrm.drag_x1[hand] > vrm.drag_x0[hand])) return;
    const float f = std::clamp((px - vrm.drag_x0[hand]) / (vrm.drag_x1[hand] - vrm.drag_x0[hand]), 0.0f, 1.0f);
    const int step = std::max(1, row->step);
    const int v = row->lo + (int)std::lround(f * (row->hi - row->lo) / step) * step;
    if (v == *r.value) return;
    std::lock_guard lk(menu_mu);
    set_row(*row, v, v > *r.value ? 1 : -1, false);
}
/// The slider let go: saved, said
void vrm_slide_end(int hand) {
    int shown[kVrmShownMax];
    const int n = vrm_shown(vrm_section(), shown);
    const int at = vrm.drag[hand];
    vrm.drag[hand] = -1;
    if (at < 0 || at >= n) return;
    const VrmRow& r = vrm_section().rows[shown[at]];
    {
        std::lock_guard lk(menu_mu);
        save();
    }
    logf("vr: VR menu: %s = %s", r.name, vrm_value_text(r).c_str());
}

/// The pointer over a thing: the selection moves there (hover); press: its action (the trigger)
void vrm_pointer(const VrmHit& h, bool press) {
    int shown[kVrmShownMax];
    vrm_shown(vrm_section(), shown);
    if (h.kind == kHitTab) {
        vrm.focus = 0;
        if (press && vrm.tab != h.index) vrm.tab = h.index, vrm.row = 0, vrm.card = 0;
    } else if (h.kind == kHitSection) {
        vrm.focus = 1;
        if (press && vrm.sec[vrm.tab] != h.index) vrm.sec[vrm.tab] = h.index, vrm.row = 0, vrm.card = 0;
    } else if (h.kind == kHitScroll) {   // (the scroll bar: a page up or down; the selection stays among the rows shown)
        if (!press) return;
        vrm.first = std::max(0, vrm.first + h.sub * std::max(1, vrm.visible - 1));
        vrm.focus = 2, vrm.row = std::clamp(vrm.row, vrm.first, vrm.first + std::max(0, vrm.visible - 1));
    } else {
        vrm.focus = 2, vrm.row = h.index;
        const VrmRow& r = vrm_section().rows[shown[h.index]];
        if (h.kind == kHitCard) {
            vrm.card = h.sub;
            if (press) vrm_set(r, r.cards[h.sub].value);
        } else if (press && h.kind == kHitSlider) {   // (a slider taken: it follows this pointer until its trigger is let go)
            const int hand = vrm.pointer;
            vrm.drag[hand] = h.index, vrm.drag_x0[hand] = h.x0, vrm.drag_x1[hand] = h.x1;
            vrm_slide(hand, vrm.last_u[hand]);
        } else if (press && h.kind == kHitLess) vrm_change(r, -1);
        else if (press && h.kind == kHitMore) vrm_change(r, 1);
        else if (press && r.kind == VrmKind::action) vrm_change(r, 1);
    }
}
const VrmHit* vrm_hit_at(float x, float y) {
    for (const VrmHit& h : vrm.hits)   // (the last drawn first: arrows over their row)
        if (x >= h.x0 && x < h.x1 && y >= h.y0 && y < h.y1) {
            for (const VrmHit& g : vrm.hits)
                if (&g != &h && (g.kind == kHitLess || g.kind == kHitMore || g.kind == kHitCard || g.kind == kHitScroll ||
                                 g.kind == kHitSlider) && x >= g.x0 && x < g.x1 &&
                    y >= g.y0 && y < g.y1)
                    return &g;
            return &h;
        }
    return nullptr;
}

/// One reading of the controllers while it is open (each present)
void vrm_input() {
    vr::PanelHand hands[2];
    if (!vr::panel_hands(hands)) {
        vrm.hands[0] = vrm.hands[1] = vr::PanelHand{};
        return;
    }
    for (int h = 0; h < 2; h++) vrm.hands[h] = hands[h];
    const int64_t now = prof_now();
    // (A and X select, B and Y go back: either controller's face buttons)
    const bool select = ((hands[0].buttons | hands[1].buttons) & HOOKSHOT_VR_BTN_LOWER) != 0;
    const bool back = ((hands[0].buttons | hands[1].buttons) & HOOKSHOT_VR_BTN_UPPER) != 0;
    if (select && !vrm.select_was) vrm_select();
    if (back && !vrm.back_was) vrm_back();
    vrm.select_was = select, vrm.back_was = back;
    // (the stick pushed furthest: a move, then again after 0.4 s held, 8 a second)
    float x = 0, y = 0;
    for (int h = 0; h < 2; h++)
        if (hands[h].stick[0] * hands[h].stick[0] + hands[h].stick[1] * hands[h].stick[1] > x * x + y * y)
            x = hands[h].stick[0], y = hands[h].stick[1];
    int dir = 0;
    if (std::max(std::fabs(x), std::fabs(y)) > .6f) dir = std::fabs(y) >= std::fabs(x) ? (y > 0 ? 1 : 2) : (x < 0 ? 3 : 4);
    else if (std::max(std::fabs(x), std::fabs(y)) > .35f) dir = vrm.stick_dir;   // (held between: no new move, no release)
    if (vrm.stick_dir == 5) dir = dir ? 5 : 0;   // (held as the menu opened: nothing until it is let go)
    else if (dir && dir != vrm.stick_dir) vrm_move(dir), vrm.stick_next = now + 400000000LL;
    else if (dir && now >= vrm.stick_next) vrm_move(dir), vrm.stick_next = now + 125000000LL;
    vrm.stick_dir = dir;
    // (the pointers: moving one over a thing selects it; its trigger presses it)
    for (int h = 0; h < 2; h++) {
        const vr::PanelHand& p = hands[h];
        const bool pressed = p.trigger > .75f, released = p.trigger < .4f;
        const bool press = p.pointing && pressed && !vrm.trigger_was[h];
        if (pressed) vrm.trigger_was[h] = true;
        else if (released) vrm.trigger_was[h] = false;
        if (vrm.drag[h] >= 0 && !vrm.trigger_was[h]) vrm_slide_end(h);
        if (!p.pointing) continue;
        const float px = p.u * kVrmW, py = p.v * kVrmH;
        if (vrm.drag[h] >= 0) {   // (a slider held: it follows the pointer, nothing else is pointed at meanwhile)
            if (std::fabs(px - vrm.last_u[h]) > 1) vrm.last_u[h] = px, vrm_slide(h, px);
            continue;
        }
        const bool moved = std::fabs(px - vrm.last_u[h]) > 3 || std::fabs(py - vrm.last_v[h]) > 3;
        if (!moved && !press) continue;
        if (moved) vrm.last_u[h] = px, vrm.last_v[h] = py;
        vrm.pointer = h;
        if (const VrmHit* hit = vrm_hit_at(px, py)) {
            const int before = vrm.focus * 4096 + vrm.tab * 256 + vrm.sec[vrm.tab] * 32 + vrm.row * 2 + vrm.card;
            vrm_pointer(*hit, press);
            const int after = vrm.focus * 4096 + vrm.tab * 256 + vrm.sec[vrm.tab] * 32 + vrm.row * 2 + vrm.card;
            if (press || before != after) vrm_buzz(h);
        }
    }
}

// ---- drawing (in the canvas's pixels: P maps them onto the target) -----------------------------------------------------
constexpr ImU32 hexc(unsigned rgb, float a = 1) {
    return IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, (int)(a * 255 + .5f));
}
ImU32 vrm_mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int s) { return (ImU32)std::lround(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t) << s; };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}
ImU32 vrm_fade(ImU32 c, float k) { return ui::alpha(c, k); }
struct VrmStop {
    float at;
    ImU32 c;
};
struct VrmGrad {
    VrmStop s[3];
    int n;
    ImU32 at(float t) const {
        if (t <= s[0].at) return s[0].c;
        for (int i = 1; i < n; i++)
            if (t <= s[i].at) return vrm_mix(s[i - 1].c, s[i].c, (t - s[i - 1].at) / std::max(1e-4f, s[i].at - s[i - 1].at));
        return s[n - 1].c;
    }
};
// (the canvas's gradients)
const VrmGrad kRimActive{{{0, hexc(0xF2BC8C)}, {.55f, hexc(0xA8683E)}, {1, hexc(0x6E3F22)}}, 3};
const VrmGrad kRimIdle{{{0, hexc(0xB97A4E)}, {.6f, hexc(0x6E3F22)}, {1, hexc(0x3A1F12)}}, 3};
const VrmGrad kFaceActive{{{0, hexc(0x8A4E2C)}, {1, hexc(0x5E321B)}}, 2};
const VrmGrad kFaceIdle{{{0, hexc(0x33200F)}, {1, hexc(0x24150B)}}, 2};
const VrmGrad kFaceAction{{{0, hexc(0x4A2C17)}, {1, hexc(0x2E1A0D)}}, 2};
const VrmGrad kIconRimActive{{{0, hexc(0xF2BC8C)}, {1, hexc(0x8A512C)}}, 2};
const VrmGrad kIconRimIdle{{{0, hexc(0xB97A4E)}, {1, hexc(0x5A321B)}}, 2};
const ImU32 kGlow = hexc(0xFFA05A);

struct VrmDraw {
    ImDrawList* d;
    ImFont* font;
    float ox, oy, k, fade;   // (fade: the greyed row's opacity, 1 otherwise)
    ImVec2 P(float x, float y) const { return ImVec2(ox + x * k, oy + y * k); }
    ImU32 C(ImU32 c) const { return fade < 1 ? vrm_fade(c, fade) : c; }
    float width(float px, const char* t) const { return font->CalcTextSizeA(px * k, FLT_MAX, 0, t).x / k; }
    /// Text with its top-left at (x, y); a shadow under it when shadow > 0 (its alpha)
    void text(float px, float x, float y, ImU32 col, const char* t, float shadow = 0) const {
        if (shadow > 0) d->AddText(font, px * k, P(x, y + 2), C(IM_COL32(0, 0, 0, (int)(shadow * 255))), t);
        d->AddText(font, px * k, P(x, y), C(col), t);
    }
    /// Text whose capitals are centred on y
    void text_mid(float px, float x, float y, ImU32 col, const char* t, float shadow = 0) const {
        text(px, x, (ui::mid_top(font, px * k, oy + y * k) - oy) / k, col, t, shadow);
    }
    /// A chamfered rectangle (corners cut by cut) in a vertical gradient: strips between the chamfers' and the stops' heights
    void chamfer(float x0, float y0, float x1, float y1, float cut, const VrmGrad& g, bool edge = true) const {
        float ys[8];
        int m = 0;
        ys[m++] = y0, ys[m++] = y0 + cut, ys[m++] = y1 - cut, ys[m++] = y1;
        for (int i = 0; i < g.n; i++) ys[m++] = y0 + g.s[i].at * (y1 - y0);
        std::sort(ys, ys + m);
        auto inset = [&](float y) { return std::max({0.0f, cut - (y - y0), cut - (y1 - y)}); };
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        for (int i = 0; i + 1 < m; i++) {
            const float a = std::clamp(ys[i], y0, y1), b = std::clamp(ys[i + 1], y0, y1);
            if (b - a < 1e-3f) continue;
            const ImU32 ca = C(g.at((a - y0) / (y1 - y0))), cb = C(g.at((b - y0) / (y1 - y0)));
            const float ia = inset(a), ib = inset(b);
            d->PrimReserve(6, 4);
            const ImDrawIdx i0 = (ImDrawIdx)d->_VtxCurrentIdx;
            d->PrimWriteIdx(i0), d->PrimWriteIdx((ImDrawIdx)(i0 + 1)), d->PrimWriteIdx((ImDrawIdx)(i0 + 2));
            d->PrimWriteIdx(i0), d->PrimWriteIdx((ImDrawIdx)(i0 + 2)), d->PrimWriteIdx((ImDrawIdx)(i0 + 3));
            d->PrimWriteVtx(P(x0 + ia, a), uv, ca), d->PrimWriteVtx(P(x1 - ia, a), uv, ca);
            d->PrimWriteVtx(P(x1 - ib, b), uv, cb), d->PrimWriteVtx(P(x0 + ib, b), uv, cb);
        }
        if (edge) outline(x0, y0, x1, y1, cut, IM_COL32(0, 0, 0, 110), 1);   // (smooths the cut edges)
    }
    void outline(float x0, float y0, float x1, float y1, float cut, ImU32 col, float thick) const {
        const ImVec2 p[8] = {P(x0 + cut, y0), P(x1 - cut, y0), P(x1, y0 + cut), P(x1, y1 - cut),
                             P(x1 - cut, y1), P(x0 + cut, y1), P(x0, y1 - cut), P(x0, y0 + cut)};
        d->AddPolyline(p, 8, C(col), ImDrawFlags_Closed, thick * k);
    }
    /// The canvas's drop-shadow glow: rings growing out from the shape, fading
    void glow(float x0, float y0, float x1, float y1, float cut, float radius, float alpha) const {
        constexpr int kRings = 7;
        for (int i = kRings; i >= 1; i--) {
            const float e = radius * i / kRings, f = 1 - (float)i / (kRings + 1);
            outline(x0 - e, y0 - e, x1 + e, y1 + e, cut + e * .42f, vrm_fade(kGlow, alpha * f * f), radius / kRings + 1.5f);
        }
    }
    /// A bevelled button: the rim, its face 2 px in, the face's light top edge and dark bottom edge
    void button(float x0, float y0, float x1, float y1, float cut, const VrmGrad& rim, const VrmGrad& face, float hi, float lo) const {
        chamfer(x0, y0, x1, y1, cut, rim);
        chamfer(x0 + 2, y0 + 2, x1 - 2, y1 - 2, cut, face, false);
        d->AddRectFilled(P(x0 + 2 + cut, y0 + 2), P(x1 - 2 - cut, y0 + 4), C(IM_COL32(255, 210, 170, (int)(hi * 255))));
        d->AddRectFilled(P(x0 + 2 + cut, y1 - 5), P(x1 - 2 - cut, y1 - 2), C(IM_COL32(0, 0, 0, (int)(lo * 255))));
    }
};

ImFont* vrm_font() {   // (the panel's one font: before a frame only, the atlas takes it then)
    static ImFont* font = nullptr;
    static bool tried = false;
    if (!tried && ImGui::GetCurrentContext()) {
        tried = true;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;   // (the compiled-in bytes)
        snprintf(cfg.Name, sizeof cfg.Name, "Libertinus Serif Semibold (VR menu)");
        font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)hookshot_font_heading, (int)hookshot_font_heading_size, 18.0f, &cfg);
    }
    return font ? font : ui::fonts().heading;
}
/// The frame art as a texture (null: not in this build)
ImTextureData* vrm_frame() {
    static ImTextureData* tex = nullptr;
    static bool tried = false;
    if (!tried && ImGui::GetCurrentContext()) {
        tried = true;
        uint32_t w = 0, h = 0;
        std::vector<unsigned char> px;
        if (vr::panel_frame_pixels(&w, &h, px)) {
            tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, (int)w, (int)h);
            memcpy(tex->GetPixels(), px.data(), px.size());
            tex->UseColors = true;
            ImGui::RegisterUserTexture(tex);
        }
    }
    return tex;
}

/// The frame: the art as a 9-slice (source edges 215 150 226 150, drawn 206 144 217 144, as the canvas's border-image),
/// or a drawn copper frame
void vrm_draw_frame(VrmDraw& g) {
    if (ImTextureData* tex = vrm_frame()) {
        const float sw = (float)tex->Width, sh = (float)tex->Height;
        const float sx[4] = {0, 150, sw - 150, sw}, sy[4] = {0, 215, sh - 226, sh};
        const float dx[4] = {0, 144, kVrmW - 144, kVrmW}, dy[4] = {0, 206, kVrmH - 217, kVrmH};
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                g.d->AddImage(tex->GetTexRef(), g.P(dx[i], dy[j]), g.P(dx[i + 1], dy[j + 1]), ImVec2(sx[i] / sw, sy[j] / sh),
                              ImVec2(sx[i + 1] / sw, sy[j + 1] / sh));
        return;
    }
    const VrmGrad leather{{{0, hexc(0x2E1A0E)}, {1, hexc(0x1A0E07)}}, 2};
    g.chamfer(16, 16, kVrmW - 16, kVrmH - 16, 44, kRimIdle);
    g.chamfer(30, 30, kVrmW - 30, kVrmH - 30, 36, leather, false);
    g.outline(60, 110, kVrmW - 60, kVrmH - 110, 24, hexc(0xA8683E, .8f), 3);
}

// ---- the icons: a section's (left of its name) and a card's picture ----
/// A game icon as a texture, made once a name (null: not in this build)
ImTextureData* vrm_game_icon(const char* name) {
    static std::unordered_map<std::string, ImTextureData*> made;
    auto [it, fresh] = made.try_emplace(name, nullptr);
    if (fresh && ImGui::GetCurrentContext()) {
        int n = 0;
        std::vector<unsigned char> px;
        if (vr::icon_pixels(name, &n, px)) {
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, n, n);
            memcpy(tex->GetPixels(), px.data(), px.size());
            tex->UseColors = true;
            ImGui::RegisterUserTexture(tex);
            it->second = tex;
        }
    }
    return it->second;
}
/// A pen for the drawn icons: (u, v) 0-1 across the icon's square, widths in fiftieths of it; drawn twice, its shadow (dark,
/// a little lower and wider) then the copper
struct VrmPen {
    const VrmDraw& g;
    float x0, y0, s;
    ImU32 col, detail;   // (the shape; marks drawn on it, its dark recesses)
    float dy, grow;      // (the shadow's offset and extra width, fiftieths)
    ImVec2 P(float u, float v) const { return g.P(x0 + u * s, y0 + v * s + dy * s / 50); }
    float W(float t) const { return (t + grow) * s / 50 * g.k; }
    float R(float r) const { return (r * s + grow * s / 100) * g.k; }
    void line(float u0, float v0, float u1, float v1, float t) const {
        g.d->AddLine(P(u0, v0), P(u1, v1), col, W(t));
        g.d->AddCircleFilled(P(u0, v0), W(t) / 2, col), g.d->AddCircleFilled(P(u1, v1), W(t) / 2, col);   // (round ends)
    }
    void disc(float u, float v, float r, bool mark = false) const { g.d->AddCircleFilled(P(u, v), R(r), mark ? detail : col, 32); }
    void ring(float u, float v, float r, float t) const { g.d->AddCircle(P(u, v), r * s * g.k, col, 40, W(t)); }
    void rect(float u0, float v0, float u1, float v1, float round, bool mark = false, ImDrawFlags f = 0) const {
        const float e = mark ? 0 : grow * s / 100 * g.k;
        const ImVec2 a = P(u0, v0), b = P(u1, v1);
        g.d->AddRectFilled(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), mark ? detail : col, round * s * g.k, f);
    }
    void frame(float u0, float v0, float u1, float v1, float round, float t) const {
        g.d->AddRect(P(u0, v0), P(u1, v1), col, round * s * g.k, 0, W(t));
    }
    void poly(std::initializer_list<ImVec2> uv, bool mark = false) const {
        ImVec2 p[12];
        int n = 0;
        for (const ImVec2& q : uv) p[n++] = P(q.x, q.y);
        g.d->AddConvexPolyFilled(p, n, mark ? detail : col);
    }
    void arc(float u, float v, float r, float a0, float a1, float t) const {
        g.d->PathArcTo(P(u, v), r * s * g.k, a0, a1, 40);
        g.d->PathStroke(col, 0, W(t));
    }
    void ellipse(float u, float v, float rx, float ry, float rot) const {
        g.d->AddEllipseFilled(P(u, v), ImVec2(R(rx), R(ry)), col, rot, 32);
    }
};
/// One drawn icon's shapes (key: "@..."; false: no such icon)
bool vrm_icon_shapes(const VrmPen& p, const char* key) {
    const std::string k = key + 1;
    if (k == "headset") {   // (a headset from the front: its straps, the body, two lenses, the nose's gap)
        p.line(.04f, .44f, .14f, .44f, 4), p.line(.86f, .44f, .96f, .44f, 4);
        p.rect(.12f, .3f, .88f, .7f, .14f);
        p.disc(.33f, .5f, .11f, true), p.disc(.67f, .5f, .11f, true);
        p.poly({{.42f, .71f}, {.5f, .58f}, {.58f, .71f}}, true);
    } else if (k == "window") {   // (a window: its frame, its bars, the sill)
        p.frame(.16f, .14f, .84f, .8f, .03f, 4);
        p.line(.5f, .16f, .5f, .78f, 3), p.line(.18f, .47f, .82f, .47f, 3);
        p.line(.08f, .88f, .92f, .88f, 4);
    } else if (k == "eye") {   // (an eye: the lids' two arcs, the iris, its pupil)
        p.arc(.5f, .709f, .469f, 3.604f, 5.821f, 4), p.arc(.5f, .291f, .469f, .462f, 2.680f, 4);
        p.disc(.5f, .5f, .15f), p.disc(.5f, .5f, .06f, true);
    } else if (k == "turn") {   // (a turn: a circle's arrow, nearly all the way round)
        const float a0 = -1.0f, a1 = 4.2f, cx = .5f, cy = .53f, r = .3f;
        p.arc(cx, cy, r, a0, a1, 4.5f);
        const float ex = cx + r * std::cos(a1), ey = cy + r * std::sin(a1), tx = -std::sin(a1), ty = std::cos(a1);
        p.poly({{ex + tx * .16f, ey + ty * .16f}, {ex + std::cos(a1) * .12f, ey + std::sin(a1) * .12f},
                {ex - std::cos(a1) * .12f, ey - std::sin(a1) * .12f}});
    } else if (k == "steps") {   // (two footprints, one ahead of the other)
        p.ellipse(.36f, .6f, .1f, .14f, -.15f), p.disc(.38f, .82f, .07f);
        p.ellipse(.64f, .3f, .1f, .14f, .15f), p.disc(.62f, .52f, .07f);
    } else if (k == "vignette") {   // (a view with a dark ring round it)
        p.disc(.5f, .5f, .4f), p.disc(.5f, .5f, .32f, true), p.disc(.5f, .5f, .17f);
    } else if (k == "hand") {   // (an open hand)
        p.rect(.3f, .44f, .72f, .86f, .13f);
        p.rect(.31f, .17f, .405f, .55f, .047f), p.rect(.42f, .1f, .515f, .52f, .047f);
        p.rect(.53f, .14f, .625f, .52f, .047f), p.rect(.64f, .25f, .725f, .56f, .042f);
        p.line(.34f, .68f, .15f, .48f, 6.5f);
    } else if (k == "controller") {   // (a gamepad: its body, two grips, a cross and buttons)
        p.disc(.27f, .64f, .14f), p.disc(.73f, .64f, .14f);
        p.rect(.12f, .32f, .88f, .64f, .15f);
        p.rect(.24f, .455f, .38f, .505f, .01f, true), p.rect(.285f, .41f, .335f, .55f, .01f, true);
        p.disc(.66f, .44f, .035f, true), p.disc(.74f, .51f, .035f, true), p.disc(.58f, .51f, .035f, true), p.disc(.66f, .58f, .035f, true);
    } else if (k == "clapper") {   // (a film clapperboard: its board, the striped stick raised)
        p.rect(.14f, .44f, .86f, .86f, .03f);
        p.rect(.14f, .44f, .86f, .52f, 0, true);
        p.poly({{.13f, .38f}, {.8f, .18f}, {.83f, .28f}, {.16f, .48f}});
        for (int i = 0; i < 3; i++) {
            const float a = .26f + i * .18f;
            p.poly({{a, .345f - (a - .13f) * .3f}, {a + .08f, .32f - (a - .13f) * .3f}, {a + .02f, .44f - (a - .13f) * .3f}, {a - .06f, .465f - (a - .13f) * .3f}}, true);
        }
    } else if (k == "chest") {   // (a treasure chest: the lid, the box, the seam and its lock)
        p.rect(.14f, .28f, .86f, .48f, .14f, false, ImDrawFlags_RoundCornersTop);
        p.rect(.14f, .46f, .86f, .84f, .04f);
        p.rect(.14f, .455f, .86f, .49f, 0, true);
        p.rect(.44f, .4f, .56f, .58f, .02f, true);
        p.disc(.5f, .47f, .025f);
    } else if (k == "picture") {   // (a framed picture: mountains and a sun)
        p.frame(.12f, .18f, .88f, .82f, .03f, 4);
        p.poly({{.2f, .74f}, {.42f, .38f}, {.64f, .74f}}), p.poly({{.5f, .74f}, {.66f, .5f}, {.8f, .74f}});
        p.disc(.7f, .34f, .07f);
    } else if (k == "trees") {   // (distance: a tree near, a tree far, the ground)
        p.poly({{.14f, .78f}, {.35f, .18f}, {.56f, .78f}}), p.rect(.32f, .76f, .38f, .87f, 0);
        p.poly({{.6f, .64f}, {.71f, .34f}, {.82f, .64f}}), p.rect(.695f, .62f, .725f, .68f, 0);
        p.line(.08f, .88f, .92f, .88f, 2.5f);
    } else if (k == "wrench") {   // (a wrench: its handle, its open jaw)
        p.line(.28f, .74f, .58f, .44f, 7.5f);
        p.arc(.67f, .34f, .15f, -.785f + .75f, -.785f + 6.283f - .75f, 6.5f);
    } else if (k == "depth") {   // (From depth: layers stacked back from the eye)
        for (int i = 2; i >= 0; i--) {
            const float o = i * .15f;
            p.poly({{.5f, .14f + o}, {.88f, .3f + o}, {.5f, .46f + o}, {.12f, .3f + o}}, i == 1);
        }
    } else if (k == "stereo") {   // (True stereo: two eyes' views overlapping)
        p.ring(.36f, .5f, .25f, 4.5f), p.ring(.64f, .5f, .25f, 4.5f);
        p.disc(.36f, .5f, .07f), p.disc(.64f, .5f, .07f);
    } else {
        return false;
    }
    return true;
}
/// Whether vrm_icon can draw it (a drawn one always: vrm_icon_shapes; a game icon when this build made it)
bool vrm_has_icon(const char* icon) { return icon && (icon[0] == '@' || vrm_game_icon(icon)); }
/// An icon in the square (x0, y0, size s): a game icon's name, or "@..." drawn in copper (lit when on). False: not drawn (a
/// game icon this build has not, or no such drawn one)
bool vrm_icon(VrmDraw& g, const char* icon, float x0, float y0, float s, bool on) {
    if (!icon) return false;
    if (icon[0] != '@') {
        ImTextureData* tex = vrm_game_icon(icon);
        if (!tex) return false;
        g.d->AddImage(tex->GetTexRef(), g.P(x0, y0), g.P(x0 + s, y0 + s), ImVec2(0, 0), ImVec2(1, 1), g.C(IM_COL32_WHITE));
        return true;
    }
    const VrmPen shadow{g, x0, y0, s, g.C(hexc(0x0A0502, .8f)), g.C(hexc(0x0A0502, .8f)), 1.6f, 1.4f};
    const VrmPen copper{g, x0, y0, s, g.C(on ? hexc(0xF6CFAA) : hexc(0xD49A6A)), g.C(on ? hexc(0x5E321B) : hexc(0x2C190F)), 0, 0};
    return vrm_icon_shapes(shadow, icon) && vrm_icon_shapes(copper, icon);
}

/// The panel at (ox, oy), k target pixels a canvas pixel
void vrm_draw(ImDrawList* d, float ox, float oy, float k) {
    VrmDraw g{d, vrm_font(), ox, oy, k, 1};
    vrm.hits.clear();
    vrm_draw_frame(g);

    // (the header: the title, a copper bar, the tabs)
    g.text_mid(44, 150, 71, hexc(0xF0DEC2), "VR Settings", .8f);
    float x = 150 + g.width(44, "VR Settings") + 28;
    d->AddRectFilledMultiColor(g.P(x, 50), g.P(x + 3, 92), hexc(0xC98B5C), hexc(0x6E3F22), hexc(0x6E3F22), hexc(0xC98B5C));
    x += 3 + 28;
    for (int t = 0; t < vrm_tab_count(); t++) {
        const bool on = t == vrm.tab, focused = on && vrm.focus == 0;
        const float w = g.width(25, vrm_tabs[t].name) + 44, y0 = 45, y1 = 97;
        if (on) g.glow(x, y0, x + w, y1, 12, focused ? 16 : 8, focused ? .75f : .45f);
        g.button(x, y0, x + w, y1, 12, on ? kRimActive : kRimIdle, on ? kFaceActive : kFaceIdle, on ? .35f : .12f, on ? .4f : .5f);
        g.text_mid(25, x + 22, 71, on ? hexc(0xFFF0D8) : hexc(0xE3CDB0), vrm_tabs[t].name);
        vrm.hits.push_back({x, y0, x + w, y1, kHitTab, t, 0});
        x += w + 10;
    }

    // (the sections, down the left)
    const VrmTab& tab = vrm_tabs[vrm.tab];
    const int sec = std::clamp(vrm.sec[vrm.tab], 0, tab.n - 1);
    for (int i = 0; i < tab.n; i++) {
        const bool on = i == sec, focused = on && vrm.focus == 1;
        const float y0 = kVrmNavY + i * (kVrmNavH + kVrmNavGap), y1 = y0 + kVrmNavH, x0 = kVrmNavX, x1 = kVrmNavX + kVrmNavW;
        if (on) g.glow(x0, y0, x1, y1, 12, focused ? 18 : 10, focused ? .8f : .5f);
        g.button(x0, y0, x1, y1, 12, on ? kRimActive : kRimIdle, on ? kFaceActive : kFaceIdle, on ? .35f : .12f, on ? .4f : .5f);
        const float ix = x0 + 2 + 6, iy = y0 + 8;   // (the icon in a dark inset: an item's the game's, the rest drawn)
        if (vrm_has_icon(tab.sections[i].icon)) {   // (none in this build: no empty square either)
            g.chamfer(ix, iy, ix + 62, iy + 62, 8, on ? kIconRimActive : kIconRimIdle);
            g.chamfer(ix + 2, iy + 2, ix + 60, iy + 60, 7, VrmGrad{{{0, on ? hexc(0x2C190F) : hexc(0x1E110A)}}, 1}, false);
            vrm_icon(g, tab.sections[i].icon, ix + 6, iy + 6, 50, on);
        }
        g.text_mid(28, ix + 62 + 18, (y0 + y1) / 2, on ? hexc(0xFFF0D8) : hexc(0xE3CDB0), tab.sections[i].name);
        vrm.hits.push_back({x0, y0, x1, y1, kHitSection, i, 0});
    }
    d->AddRectFilledMultiColor(g.P(490, 176), g.P(493, 860), hexc(0xC98B5C), hexc(0x3A1F12), hexc(0x3A1F12), hexc(0xC98B5C));

    // (the section's title and its rows)
    const VrmSection& s = tab.sections[sec];
    g.text(56, kVrmMainX, 168, hexc(0xF0DEC2), s.name, .7f);
    d->AddRectFilledMultiColor(g.P(kVrmMainX, 239), g.P(kVrmMainX + 340, 242), hexc(0xD49A6A), hexc(0xD49A6A), hexc(0x7A4526),
                               hexc(0x7A4526));
    int shown[kVrmShownMax];
    const int n = vrm_shown(s, shown);
    vrm.row = std::clamp(vrm.row, 0, std::max(0, n - 1));
    // (a section longer than the room scrolls: the selection kept in sight, a bar at the side saying where it is)
    float heights[kVrmShownMax];
    for (int i = 0; i < n; i++) heights[i] = (s.rows[shown[i]].kind == VrmKind::cards ? kVrmCardsH : kVrmRowH) + 1;
    if (vrm.first_of != vrm.tab * 64 + sec) vrm.first = 0, vrm.first_of = vrm.tab * 64 + sec;
    vrm_scroll(heights, n, kVrmRowsBottom - kVrmRowsY, vrm.row, vrm.focus == 2, &vrm.first, &vrm.visible);
    float y = kVrmRowsY;
    const VrmRow* selected = nullptr;
    for (int i = vrm.first; i < vrm.first + vrm.visible; i++) {
        const VrmRow& r = s.rows[shown[i]];
        const bool sel = vrm.focus == 2 && i == vrm.row;
        const char* grey = vrm_greyed(r);
        const float h = r.kind == VrmKind::cards ? kVrmCardsH : kVrmRowH, x0 = kVrmMainX, x1 = kVrmMainR;
        if (sel) selected = &r;
        if (sel && r.kind != VrmKind::cards)   // (the selected row: a warm band behind it; cards: only the focused card glows)
            g.chamfer(x0 + 2, y + 2, x1 - 2, y + h - 2, 8, VrmGrad{{{0, hexc(0xC98B5C, .2f)}, {1, hexc(0xC98B5C, .08f)}}, 2}, false);
        g.fade = grey ? .45f : 1;
        vrm.hits.push_back({x0, y, x1, y + h, kHitRow, i, 0});
        if (r.kind == VrmKind::cards) {   // (its name over two large cards, or three narrower ones without a picture)
            g.text_mid(30, x0 + 12, y + 10 + 18, hexc(0xEEDCC0), r.name);
            const int nc = vrm_cards(r);
            const float cw = (x1 - x0 - 24 - 22 * (nc - 1)) / nc, cy0 = y + 10 + 36 + 12, cy1 = cy0 + 150;
            for (int c = 0; c < nc; c++) {
                const VrmCard& card = r.cards[c];
                const bool on = vrm_card_now(r) == card.value, hot = sel && vrm.card == c;
                const float cx0 = x0 + 12 + c * (cw + 22), cx1 = cx0 + cw;
                if (on || hot) g.glow(cx0, cy0, cx1, cy1, 12, hot ? 16 : 10, hot ? .75f : .5f);
                g.button(cx0, cy0, cx1, cy1, 12, on ? kRimActive : kRimIdle, on ? kFaceActive : kFaceIdle, on ? .35f : .12f,
                         on ? .4f : .5f);
                const float ax = cx0 + 2 + 10, ay = cy0 + 13;   // (the card's picture's place)
                const float pic = nc > 2 ? 0 : 130, tx = ax + pic + 18, room = cx1 - 12 - tx;
                if (pic > 0) {   // (its picture: a drawn icon in a dark inset)
                    g.chamfer(ax, ay, ax + pic, ay + 124, 8, VrmGrad{{{0, on ? hexc(0x2C190F) : hexc(0x1E110A)}}, 1});
                    vrm_icon(g, card.icon, ax + (pic - 96) / 2, ay + 14, 96, on);
                }
                float lp = 30, dp = 21;   // (smaller to fit a narrow card)
                while (lp > 20 && g.width(lp, card.label) > room) lp -= 1;
                while (dp > 15 && g.width(dp, card.desc) > room) dp -= 1;
                g.text(lp, tx, cy0 + 42, on ? hexc(0xFFF0D8) : hexc(0xE3CDB0), card.label);
                g.text(dp, tx, cy0 + 84, on ? hexc(0xF2D9BA) : hexc(0xC9B08F), card.desc);
                vrm.hits.push_back({cx0, cy0, cx1, cy1, kHitCard, i, c});
            }
        } else {
            const float cy = y + h / 2;
            g.text_mid(30, x0 + 12, grey ? cy - 10 : cy, hexc(0xEEDCC0), r.name);
            if (grey) {
                g.fade = 1;
                g.text_mid(19, x0 + 12, cy + 19, hexc(0xC9B08F), grey);
                g.fade = .45f;
            }
            const float bx1 = x1 - 12, by0 = cy - 24, by1 = cy + 24;
            if (r.kind == VrmKind::info) {   // (its words where the other rows show their values: nothing to press)
                float px = 28;
                while (px > 16 && g.width(px, r.button) > 250) px -= 1;
                g.text_mid(px, bx1 - 46 - 10 - 125 - g.width(px, r.button) / 2, cy, hexc(0xF0DEC2), r.button);
            } else if (r.kind == VrmKind::action) {   // (its button centred where the other rows show their values)
                const float w = std::max(200.0f, g.width(25, r.button) + 48), bc = bx1 - 46 - 10 - 125;
                if (sel) g.glow(bc - w / 2, by0, bc + w / 2, by1, 8, 12, .6f);
                g.button(bc - w / 2, by0, bc + w / 2, by1, 8, kRimIdle, kFaceAction, .18f, .5f);
                g.text_mid(25, bc - g.width(25, r.button) / 2, cy, hexc(0xF0DEC2), r.button);
            } else if (r.kind == VrmKind::slider) {   // (slider: its value, and a track with a knob to drag)
                const Row* row = vrm_row_of(r.value);
                const std::string v = vrm_value_text(r);
                const float tx0 = bx1 - 46 - 10 - 250 - 10 - 46 + 14, tx1 = bx1 - 14;
                const float f = row && row->hi > row->lo ? std::clamp((float)(*r.value - row->lo) / (row->hi - row->lo), 0.0f, 1.0f) : 0;
                const float fx = tx0 + (tx1 - tx0) * f;
                float px = 26;
                while (px > 16 && g.width(px, v.c_str()) > 170) px -= 1;
                g.text_mid(px, tx0 - 24 - g.width(px, v.c_str()), cy, hexc(0xF0DEC2), v.c_str());
                d->AddRectFilled(g.P(tx0, cy - 5), g.P(tx1, cy + 5), g.C(hexc(0x1E110A)), 5 * k);
                d->AddRectFilled(g.P(tx0, cy - 5), g.P(fx, cy + 5), g.C(sel ? hexc(0xF2BC8C) : hexc(0xD49A6A)), 5 * k);
                d->AddCircleFilled(g.P(fx, cy), 14 * k, g.C(sel ? hexc(0xFFE2C0) : hexc(0xE8B488)));
                d->AddCircle(g.P(fx, cy), 14 * k, g.C(hexc(0x5A321B)), 0, 1.5f * k);
                vrm.hits.push_back({tx0 - 14, by0, tx1 + 14, by1, kHitSlider, i, 0});
            } else {   // (step: an arrow either side of its value)
                const std::string v = vrm_value_text(r);
                float px = 28;
                while (px > 16 && g.width(px, v.c_str()) > 250) px -= 1;
                g.text_mid(px, bx1 - 46 - 10 - 125 - g.width(px, v.c_str()) / 2, cy, hexc(0xF0DEC2), v.c_str());
                for (int side = 0; side < 2; side++) {
                    const float ax0 = side ? bx1 - 46 : bx1 - 46 - 10 - 250 - 10 - 46, acx = ax0 + 23;
                    const ImU32 fill = sel ? hexc(0xF2BC8C) : hexc(0xD49A6A);
                    const ImVec2 a = side ? g.P(acx - 8, cy - 11) : g.P(acx + 8, cy - 11), b = g.P(side ? acx + 8 : acx - 8, cy),
                                 c = side ? g.P(acx - 8, cy + 11) : g.P(acx + 8, cy + 11);
                    d->AddTriangleFilled(a, b, c, g.C(fill));
                    d->AddTriangle(a, b, c, g.C(hexc(0x5A321B)), 1.5f * k);
                    vrm.hits.push_back({ax0, by0, ax0 + 46, by1, side ? kHitMore : kHitLess, i, 0});
                }
            }
        }
        g.fade = 1;
        d->AddRectFilled(g.P(x0, y + h - 1), g.P(x1, y + h), IM_COL32(0, 0, 0, 153));   // (the line under it, and its light)
        d->AddRectFilled(g.P(x0, y + h), g.P(x1, y + h + 1), hexc(0xC98B5C, .28f));
        y += h + 1;
    }
    if (vrm.first > 0 || vrm.first + vrm.visible < n) {   // (the scroll bar, its thumb the rows shown; arrows where there are more)
        const float bx0 = kVrmMainR - 7, bx1 = kVrmMainR - 1, ty0 = kVrmRowsY + 18, ty1 = kVrmRowsBottom - 18;
        d->AddRectFilled(g.P(bx0, ty0), g.P(bx1, ty1), hexc(0x1E110A, .9f), 3 * k);
        const float t0 = ty0 + (ty1 - ty0) * vrm.first / n, t1 = ty0 + (ty1 - ty0) * (vrm.first + vrm.visible) / n;
        d->AddRectFilledMultiColor(g.P(bx0, t0), g.P(bx1, t1), hexc(0xF2BC8C), hexc(0xF2BC8C), hexc(0xA8683E), hexc(0xA8683E));
        const float cx = (bx0 + bx1) / 2;
        if (vrm.first > 0)
            d->AddTriangleFilled(g.P(cx, kVrmRowsY + 2), g.P(cx + 7, kVrmRowsY + 13), g.P(cx - 7, kVrmRowsY + 13), hexc(0xD49A6A));
        if (vrm.first + vrm.visible < n)
            d->AddTriangleFilled(g.P(cx - 7, kVrmRowsBottom - 13), g.P(cx + 7, kVrmRowsBottom - 13), g.P(cx, kVrmRowsBottom - 2),
                                 hexc(0xD49A6A));
        vrm.hits.push_back({bx0 - 4, kVrmRowsY, bx1 + 6, t0, kHitScroll, 0, -1});
        vrm.hits.push_back({bx0 - 4, t1, bx1 + 6, kVrmRowsBottom, kHitScroll, 0, 1});
    }
    if (selected) {   // (the selected row's help: the Insert menu's text for it)
        const Row* row = vrm_row_of(selected->value);
        const char* help = selected->kind == VrmKind::info ? kVrmVrControlsHelp : !selected->value ? kVrmControlsHelp :
                           row && row->help ? row->help : nullptr;
        if (help) {
            const float px = 19, line = px * 1.3f, top = std::max(y + 4, 796.0f);
            const int lines = std::max(0, (int)((kVrmBodyBottom - top) / line));
            const char* t = help;
            const char* end = t + strlen(t);
            for (int l = 0; l < lines && t < end; l++) {
                const char* cut = g.font->CalcWordWrapPosition(px * k, t, end, (kVrmMainR - kVrmMainX - 24) * k);
                if (cut == t) cut = t + 1;
                std::string piece(t, cut);
                if (l == lines - 1 && cut < end) piece += "\xE2\x80\xA6";   // (more than fits: an ellipsis)
                g.text(px, kVrmMainX + 12, top + l * line, hexc(0xBFA07E), piece.c_str());
                t = cut;
                while (t < end && *t == ' ') t++;
            }
        }
    }

    // (the footer: the name, the buttons' hints)
    g.text_mid(22, 160, 976, hexc(0xBFA07E), "Hookshot VR", .8f);
    float hx = 1440;
    const char* hints[2][2] = {{"B", "Back"}, {"A", "Select"}};
    for (const auto& hint : hints) {
        hx -= g.width(26, hint[1]);
        g.text_mid(26, hx, 976, hexc(0xEEDCC0), hint[1], .8f);
        hx -= 10 + 17;
        d->AddCircleFilled(g.P(hx, 977), 17 * k, IM_COL32(0, 0, 0, 150), 32);
        d->AddCircleFilled(g.P(hx, 976), 17 * k, hexc(0xA8683E), 32);
        d->AddCircleFilled(g.P(hx, 974), 15 * k, hexc(0xE0A577), 32);
        g.text_mid(20, hx - g.width(20, hint[0]) / 2, 976, hexc(0x2A160B), hint[0]);
        hx -= 17 + 32;
    }
    // (the pointers' dots are layers of their own over the panel: vr_menu_layer.inc panel_dot, so a pointer's move draws nothing)
}

/// What the panel shows, as a hash: drawn again only when it changes (and once a second, for values changed elsewhere)
uint64_t vrm_signature() {
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](int64_t v) { hsh = (hsh ^ (uint64_t)v) * 1099511628211ull; };
    mix(vrm.tab), mix(vrm.sec[vrm.tab]), mix(vrm.row), mix(vrm.card), mix(vrm.focus), mix(vrm.first);
    mix(opts.vr_mode), mix(opts.vr_stereo), mix(opts.vr_bow_preview), mix(opts.vr_bomb_pull), mix(opts.vr_window);
    const VrmSection& s = vrm_section();
    for (int i = 0; i < s.n; i++) mix(s.rows[i].value ? *s.rows[i].value : 0);
    return hsh;
}

/// (RX_VR_MENU_TEST=<seconds>[,cycle], headless) opens the menu that long after the first present; cycle: a tab and a
/// section further every 3 s
void vrm_test_step() {
    static const char* env = vr_research_getenv("RX_VR_MENU_TEST");
    if (!env || !*env) return;
    static int64_t first = 0, next = 0;
    static bool opened = false;
    const int64_t now = prof_now();
    if (!first) first = now;
    if (!opened && now - first >= (int64_t)(atof(env) * 1e9)) {
        opened = true, next = now + 3000000000LL;
        vr::panel_set_open(true);
    }
    if (opened && strstr(env, "cycle") && now >= next && vr::panel_open()) {
        next = now + 3000000000LL;
        const VrmTab& t = vrm_tabs[vrm.tab];
        if (vrm.sec[vrm.tab] + 1 < t.n) vrm.sec[vrm.tab]++;
        else vrm.sec[vrm.tab] = 0, vrm.tab = (vrm.tab + 1) % vrm_tab_count();
        vrm.focus = 2, vrm.row = 0;
        logf("vr: VR menu test: %s, %s", vrm_tabs[vrm.tab].name, vrm_section().name);
    }
}

/// Each present: opened or closed since the last, the controllers, and whether to draw it again. true: draw it now
bool vrm_tick(bool force) {
    vrm_test_step();
    if (vr::window_options_changed()) {   // (the window moved, resized or rounded by hand: vr/headset/window_view.inc)
        std::lock_guard lk(menu_mu);
        save();
    }
    const bool open = vr::panel_open();
    if (open != vrm.was_open) {
        vrm.was_open = open;
        vrm.drawn = 0;
        vrm.select_was = vrm.back_was = true;   // (a button held as it opens is not a press)
        vrm.trigger_was[0] = vrm.trigger_was[1] = true;
        vrm.stick_dir = 5;                       // (nor a stick held)
        if (!open) {
            vr::panel_hidden();
            return false;
        }
    }
    if (!open) return false;
    vrm_input();
    const uint64_t sig = vrm_signature();
    const int64_t now = prof_now();
    if (!force && sig == vrm.drawn && now - vrm.drawn_at < 1000000000LL) return false;
    vrm.drawn = sig, vrm.drawn_at = now;
    return true;
}

void vrm_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1, dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

/// (gpu/vk/vk_replay.cpp's present, VR HOOK) the VR menu: drawn into its own image for the headset when something on it
/// changed (vr_menu_layer.inc shows it)
void vr::panel_record(VkCommandBuffer cmd) {
    if (!vrm_tick(false)) return;
    VkImageView view = VK_NULL_HANDLE;
    const VkImage image = vr::panel_target((uint32_t)kVrmW, (uint32_t)kVrmH, &view);
    if (!image) return;
    vrm_font(), vrm_frame();   // (before the frame: the atlas takes a font then)
    vrm_barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue transparent{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1, &range);
    vrm_barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!ui::begin((uint32_t)kVrmW, (uint32_t)kVrmH)) {
        vrm.drawn = 0;
        return;
    }
    vrm_draw(ImGui::GetForegroundDrawList(), 0, 0, 1);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (as menu_record: what this thread recorded reaches cmd first)
    ui::record_to(cmd, image, view, (uint32_t)kVrmW, (uint32_t)kVrmH);
    vr::panel_ready(cmd);
}

/// (headless shots, gpu/vk/vk_replay.cpp's VR HOOK) the VR menu over a shot (TRANSFER_DST before and after), when open
void vr::panel_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    vrm_tick(true);
    if (!vr::panel_open() || !image || !view) return;
    vrm_font(), vrm_frame();
    if (!ui::begin(w, h)) return;
    const float k = std::min(w * .96f / kVrmW, h * .96f / kVrmH);
    vrm_draw(ImGui::GetForegroundDrawList(), (w - kVrmW * k) / 2, (h - kVrmH * k) / 2, k);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);
    ui::record_to(cmd, image, view, w, h);
}

} // namespace mkwvk
