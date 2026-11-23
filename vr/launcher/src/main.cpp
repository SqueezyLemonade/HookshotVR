// HookshotVR Launcher: the window before Hookshot starts (vr/launcher/README.md). It builds the game with Hookshot's own
// builder underneath, keeps it up to date, checks the VR runtime and the headset, and starts the game as Hookshot (Desktop
// Mode) or HookshotVR, itself (no script, no terminal). It never starts a VR runtime by itself.
//
//   hookshotvr-launcher                         the launcher
//   hookshotvr-launcher --state <name>          one screen of the design, with sample values (nothing is started or saved):
//                                               setup, building, ready, desktop, waiting, notrunning, noruntime, update,
//                                               settings, changelog, updatelog, updating; or live: this PC's own state (with --screenshot, nothing
//                                               is saved or started)
//   hookshotvr-launcher --state <name> --screenshot <file.png> [--scale <n>]   that screen into a PNG, no window shown
//   --art <folder>       the art slots from there (default: art/ beside the launcher, then vr/launcher/art)
//   --no-art             the drawn fallbacks instead of the art slots
//   --fake-build <py>    a script in make_game.py's place, for trying the build screens (vr/launcher/tools/fake_make_game.py)
//   hookshotvr-launcher --play [vr|desktop]     Play pressed for you once the launcher is ready (in VR: once the runtime and
//                                               the headset answer); the desktop entry's Play actions
//   hookshotvr-launcher --build                 Build pressed for you once the first start's rows are all good (the game file
//                                               set in launcher.ini, its check passed)
//   hookshotvr-launcher --update                Update pressed for you once the check finds one (or a rebuild is pending)
//   hookshotvr-launcher --after-update          (the update starts the new launcher so: it says so, and builds the game
//                                               again when the update needs that)
//   hookshotvr-launcher --install               the "Hookshot VR" entry in the app menu and on the desktop, starting this
//                                               launcher with no terminal, and its icon (into the data folder)
//   hookshotvr-launcher --icon <file.png> [--scale <n>]   the launcher's icon alone (256 px at scale 1)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include "draw.h"
#include "image.h"
#include "imgui.h"
#include "system.h"
#include "update.h"
#include "window.h"

using namespace draw;

using upd::kVersion;
static constexpr double kNeededGB = 60;

enum class Screen { Setup, Building, Main };
enum class Overlay { None, Settings, Changelog, UpdateLog };
enum class FileState { None, Checking, Ok, Bad };

struct Art {
    ImTextureID tex = ImTextureID_Invalid;
    int w = 0, h = 0;
};

struct App {
    sys::Config cfg;
    sys::Paths paths;
    bool preview = false;
    std::string fake_build;
    Screen screen = Screen::Setup;
    Overlay overlay = Overlay::None;
    bool vr = true;
    // the first start
    FileState file = FileState::None;
    std::string file_text;
    double free_gb = -1;
    // the build
    sys::BuildState build;
    // the update
    bool update_ready = false;
    upd::Release update;
    upd::Stage update_stage = upd::Stage::Idle;
    bool after_update = false;   // (--after-update: the new launcher, started by the old one's update)
    // the game
    sys::Proc game;
    bool pending_play = false;   // (Play with Start With: the game starts once the runtime and the headset are there)
    bool auto_play = false;      // (--play: Play pressed once the launcher is ready)
    bool auto_build = false;     // (--build: Build pressed once the game file's check passes)
    bool auto_update = false;    // (--update: Update pressed once there is one)
    std::string notice;
    double notice_until = 0;
    // VR
    sys::VrState vr_state;
    // art and text
    Art frame, wordmark, icon_desktop, icon_vr;
    ImTextureID grain = ImTextureID_Invalid;
    std::vector<std::string> changes;   // this version's, from CHANGELOG.txt
    bool quit = false;
} app;

static void say(const std::string &s) {
    app.notice = s;
    app.notice_until = sys::now_seconds() + 8;
}

static bool no_save;   // (--state live --screenshot: this PC's state, nothing written)
static void save() {
    if (!app.preview && !no_save) sys::save_config(app.cfg);
}

static void refresh() {
    app.paths = sys::paths(app.cfg);
    if (app.screen == Screen::Building) return;
    app.screen = app.paths.game_built ? Screen::Main : Screen::Setup;
    // (the folder the build goes to: the one found beside the launcher before the first build saves it, not the home drive)
    if (!app.preview) app.free_gb = sys::free_gb(!app.paths.hookshot.empty() ? app.paths.hookshot : sys::home());
}

static std::string read_text(const std::string &p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Update: the one-click update (update.h), shown on the Building screen. Not while the game runs (a rebuild deletes the
// old game folder, and the files the update replaces are in use)
static void start_update() {
    if (app.preview || upd::running() || !app.update_ready) return;
    if (app.game.ok() || plat::process_running("hookshot")) return say("Game running");
    app.build = sys::BuildState();
    if (upd::start(app.paths, app.cfg, app.update, app.fake_build)) app.screen = Screen::Building, app.overlay = Overlay::None;
    else say("Update failed");
}

// What the update is: the launcher alone (a small download), or the game built again too (a second game folder while it
// builds), and the free space that needs
static std::string update_kind(const upd::Release &r) {
    char gb[48];
    snprintf(gb, sizeof gb, " \xC2\xB7 %.0f GB", upd::needed_gb(r));
    return (r.rebuild ? "Game rebuild" : "Launcher only") + std::string(gb);
}

static void start_build() {
    if (app.preview) return;
    app.cfg.hookshot = app.paths.hookshot;
    save();
    app.build = sys::BuildState();
    app.build.running = true, app.build.started = app.build.step_started = sys::now_seconds();
    if (sys::build_start(app.paths, sys::first_build(app.paths, app.cfg.game_file), app.fake_build)) app.screen = Screen::Building;
    else app.build.running = false, say("Build failed");
}

static bool build_ready() {
    return app.paths.builder_ok && app.paths.repo_ok && app.file == FileState::Ok && (app.free_gb < 0 || app.free_gb >= kNeededGB);
}

static void choose_game_file() {
    if (app.preview) return;
    std::string from = app.cfg.game_file.empty() ? "" : app.cfg.game_file.substr(0, app.cfg.game_file.rfind('/'));
    sys::choose_file("Game file", from, "Game file", {"*.nup", "*.nx"}, [](std::string f) {
        if (f.empty()) return;
        app.cfg.game_file = f;
        save();
        app.file = FileState::Checking;
        sys::check_game_file(app.paths, f, app.fake_build);
    });
}

static void choose_folder(std::string *slot, const char *title) {
    if (app.preview) return;
    sys::choose_folder(title, *slot, [slot](std::string f) {
        if (f.empty()) return;
        *slot = f;
        save();
        refresh();
    });
}

// The VR Runtime setting's choice: Custom asks for the runtime's manifest (OpenXR runtimes are chosen by that file)
static void choose_runtime_json() {
    if (app.preview) return;
    std::string from = app.cfg.runtime_json.empty() ? "" : sys::parent(app.cfg.runtime_json);
    sys::choose_file("OpenXR runtime", from, "OpenXR manifest", {"*.json"}, [](std::string f) {
        if (f.empty()) return;
        app.cfg.runtime_json = f;
        save();
    });
}
static void choose_start_with() {
    if (app.preview) return;
    std::string from = app.cfg.start_with.empty() ? "" : sys::parent(app.cfg.start_with);
    sys::choose_file("Start With", from, "App", {}, [](std::string f) {
        if (f.empty()) return;
        app.cfg.start_with = f;
        save();
    });
}

static bool vr_waiting() {
    const sys::VrState &s = app.vr_state;
    return app.vr && (s.session != sys::Check::Ok || s.headset == sys::Check::Waiting);
}

static void play() {
    if (app.preview || app.game.ok()) return;
    if (app.vr && vr_waiting()) {
        // (Start With: its app now, the game once the runtime answers. Again: not after all)
        if (app.pending_play) app.pending_play = false;
        else if (app.vr_state.can_start && sys::start_with(app.cfg)) app.pending_play = true;
        return;
    }
    app.pending_play = false;
    std::string err;
    if (!sys::launch_game(app.paths, app.cfg, app.vr, app.vr_state.runtime, &app.game, &err)) {
        say("Start failed");
        fprintf(stderr, "launcher: the game did not start: %s\n", err.c_str());
        app.game = sys::Proc();
        return;
    }
    if (app.cfg.on_start == sys::kCloseLauncher) app.quit = true;
}

static bool overlay_free() { return app.overlay == Overlay::None && !sys::dialog_open(); }

static void set_mode(bool vr) {
    app.vr = vr;
    app.pending_play = false;
    if (app.cfg.remember) {
        app.cfg.mode = vr ? "vr" : "desktop";
        save();
    }
}

// ---- the screens ----
static const float CX = 76, CW = 1113;   // (the content's left edge and width in the frame)

struct Row {
    std::string name, text;
    ImU32 dot;
    bool ok;
    std::string fix;
    std::function<void()> on_fix;
};

static ImU32 dot_of(sys::Check c) {
    return c == sys::Check::Ok ? rgb(kGreen) : c == sys::Check::Waiting ? rgb(kAmber) : c == sys::Check::Bad ? rgb(kRed) : rgb(kDotIdle);
}

// A check row (the canvas's grid: 20 | 180 | 1fr | auto, gap 14, 52 high, 12 in from each side)
static void check_row(const Row &r, float y, int index) {
    const float h = 52, cy = y + h * 0.5f, x = CX + 12;
    dot(x + 7, cy, 14, r.dot, true);
    text(fonts.medium, 24, x + 34, cy, kInk, fit(fonts.medium, 24, r.name, 180));
    float tx = x + 34 + 180 + 14;
    float room = CX + CW - 12 - tx - (r.fix.empty() ? 0 : 194);
    text(fonts.medium, 21, tx, cy, r.ok ? kInkSoft : kInkWarm, fit(fonts.medium, 21, r.text, room));
    if (!r.fix.empty()) {
        char id[32];
        snprintf(id, sizeof id, "fix%d", index);
        if (small_button(id, CX + CW - 12 - 180, cy - 19, 180, 38, r.fix, true, 20) && r.on_fix) r.on_fix();
    }
    row_rule(CX, CX + CW, y + h);
}

static void screen_main(float t) {
    const float card_w = (CW - 22) * 0.5f;
    if (mode_card("flat", CX, 321, card_w, 118, "Hookshot", false, !app.vr, app.icon_desktop.tex)) set_mode(false);
    if (mode_card("vr", CX + card_w + 22, 321, card_w, 118, "HookshotVR", true, app.vr, app.icon_vr.tex)) set_mode(true);
    bool update = app.update_ready;
    if (update) {
        // (the banner is the one click too: Update Log beside it shows what comes)
        bool hb = false;
        if (hit("upbanner", CX, 455, CW - 8 - 140 - 12 - 150 - 12, 50, true, &hb)) start_update();
        chamfer(CX, 455, CW, 50, 8, rgb(0xC98B5C, hb ? 0.28f : 0.18f));
        float room = CW - 8 - 140 - 12 - 150 - 12 - 36;
        std::string what = (app.update.launcher ? "Update " : "Rebuild ") + app.update.version;
        text(fonts.medium, 23, CX + 18, 480, kInkWarm, what);
        float ww = text_width(fonts.medium, 23, what) + 24;
        text(fonts.medium, 20, CX + 18 + ww, 480, kInkMuted, fit(fonts.medium, 20, update_kind(app.update), room - ww));
        if (small_button("uplog", CX + CW - 8 - 140 - 12 - 150, 462, 150, 36, "Update Log", false)) app.overlay = Overlay::UpdateLog;
        if (small_button("upgo", CX + CW - 8 - 140, 462, 140, 36, "Update", true)) start_update();
    }
    if (app.vr) {
        const sys::VrState &s = app.vr_state;
        std::vector<Row> rows;
        // (labels and values: the runtime by its name and whether it runs, then the headset. Nothing is started from here
        // unless the player set Start With)
        Row a{"VR runtime", "", dot_of(s.session), s.session == sys::Check::Ok, "", nullptr};
        if (!s.runtime.found) {
            a.text = s.none_installed ? "None installed" : app.cfg.runtime == sys::kRuntimeSystem ? "Not set" : "Not found";
            a.fix = "Change", a.on_fix = [] { app.overlay = Overlay::Settings; };
        } else {
            a.name = sys::runtime_label(s.runtime);
            a.text = s.session == sys::Check::Ok ? "Running" : s.session == sys::Check::Waiting ? "Not running" : "Checking";
            if (s.can_start && !app.pending_play) a.fix = "Start", a.on_fix = [] { if (!app.preview) sys::start_with(app.cfg); };
        }
        rows.push_back(a);
        const char *h = s.headset == sys::Check::Ok ? "Connected" : s.headset == sys::Check::Waiting ? "Waiting" : "Unknown";
        rows.push_back({"Headset", h, dot_of(s.headset), s.headset != sys::Check::Waiting, "", nullptr});
        float y = update ? 519 : 459;
        for (size_t i = 0; i < rows.size(); i++) check_row(rows[i], y + 52 * i, (int)i);
    }
    (void)t;
}

static void screen_setup() {
    text(fonts.semibold, 30, 640, 348, kInkButton, "Set up", Center);
    std::vector<Row> rows;
    if (!app.paths.builder_ok)
        rows.push_back({"Hookshot", app.cfg.hookshot.empty() ? "Not set" : "Not found", rgb(kRed), false, "Choose",
                        [] { choose_folder(&app.cfg.hookshot, "Hookshot Folder"); }});
    if (!app.paths.repo_ok)
        rows.push_back({"HookshotVR", "Not found", rgb(kRed), false, "Choose", [] { choose_folder(&app.cfg.hookshotvr, "HookshotVR Folder"); }});
    const char *none = "None";
    Row g{"Game file", "", rgb(kDotIdle), true, "Choose", choose_game_file};
    switch (app.file) {
        case FileState::None: g.text = app.cfg.game_file.empty() ? none : sys::pretty(app.cfg.game_file); break;
        case FileState::Checking: g.text = "Checking", g.dot = rgb(kAmber); break;
        case FileState::Ok: g.text = app.file_text, g.dot = rgb(kGreen); break;
        case FileState::Bad: g.text = app.file_text.empty() ? "Not supported" : app.file_text, g.dot = rgb(kRed), g.ok = false; break;
    }
    rows.push_back(g);
    char gb[64];
    if (app.free_gb >= 0) snprintf(gb, sizeof gb, "%.0f GB free", app.free_gb);
    else snprintf(gb, sizeof gb, "Unknown");
    bool space = app.free_gb < 0 || app.free_gb >= kNeededGB;
    rows.push_back({"Free space", gb, app.free_gb < 0 ? rgb(kDotIdle) : space ? rgb(kGreen) : rgb(kRed), space, "", nullptr});
    for (size_t i = 0; i < rows.size(); i++) check_row(rows[i], 381 + 52 * i, (int)i + 10);
}

static const char *step_title(sys::Step s) {
    switch (s) {
        case sys::Step::Check: return "Checking";
        case sys::Step::Translate: return "Translating";
        case sys::Step::Compile: return "Compiling";
        default: return "Shaders";
    }
}

static void screen_building() {
    const sys::BuildState &b = app.build;
    const float x0 = CX + 60, w = CW - 120;
    bool failed = b.finished && !b.ok;
    const char *title = failed ? "Build Stopped" : step_title(b.step);
    if (app.update_stage == upd::Stage::Download) title = "Downloading";
    else if (app.update_stage == upd::Stage::Install || app.update_stage == upd::Stage::Restart) title = "Installing";
    else if (app.update_stage == upd::Stage::Switch || app.update_stage == upd::Stage::Clean) title = "Switching";
    text(fonts.semibold, 32, 640, 359, kInkButton, title, Center);
    chamfer(x0, 395, w, 30, 8, rgb(0x1E110A));
    list()->AddRectFilled(P(x0 + 4, 395), P(x0 + w - 4, 398), rgb(0, 0.7f));   // (the inset shadow)
    float f = (float)(b.fraction < 0 ? 0 : b.fraction > 1 ? 1 : b.fraction);
    if (f > 0.005f) {
        float fw = (w - 6) * f;
        chamfer_glow(x0 + 3, 398, fw, 24, 6, rgb(0xF0AA6E, 0.45f), 8);
        chamfer_gradient(x0 + 3, 398, fw, 24, 6, rgb(0xF2BC8C), rgb(0xA8683E), rgb(0x6E3F22), 0.55f);
    }
    std::string line;
    if (failed) {
        line = b.error.empty() ? "Stopped" : b.error;
        while (!line.empty() && line.back() == '.') line.pop_back();
    } else {
        char buf[64];
        // (a tenth of a percent under 10: the shaders' step moves 0.1% at a time on a cold cache, and a bar at "0%" for
        // minutes looked stuck)
        char pct[16];
        if (f > 0 && f < 0.1) snprintf(pct, sizeof pct, "%.1f%%", floor(f * 1000) / 10);
        else snprintf(pct, sizeof pct, "%d%%", (int)floor(f * 100 + 0.5));
        if (b.eta > 0) snprintf(buf, sizeof buf, "%s \xC2\xB7 %d min left", pct, (int)ceil(b.eta / 60));
        else snprintf(buf, sizeof buf, "%s", pct);
        line = buf;
    }
    text(fonts.medium, 23, 640, 457, failed ? kInkWarm : kInkSoft, fit(fonts.medium, 23, line, w), Center);
    const char *names[4] = {"Check", "Translate", "Compile", "Shaders"};
    float widths[4], total = 0;
    for (int i = 0; i < 4; i++) widths[i] = 22 + text_width(fonts.medium, 22, names[i]), total += widths[i] + (i ? 34 : 0);
    float x = 640 - total * 0.5f;
    for (int i = 0; i < 4; i++) {
        bool done = i < (int)b.step || (b.ok && b.finished), now = i == (int)b.step && !b.finished;
        ImU32 d = done ? rgb(kGreen) : now ? rgb(kAmber) : rgb(kDotIdle);
        dot(x + 6, 505, 12, d, done || now);
        text(fonts.medium, 22, x + 22, 505, done || now ? kInk : kInkMuted, names[i]);
        x += widths[i] + 34;
    }
}

static void screen_log(bool update) {
    const std::vector<std::string> &lines = update ? app.update.changes : app.changes;
    std::string version = update ? app.update.version : upd::kVersionName;
    text(fonts.semibold, 30, CX + 12, 346, kInkButton, update ? "Update Log" : "Changelog");
    text(fonts.medium, 24, CX + CW - 12, 346, kCopper, version, Right);
    float y = 371;
    for (size_t i = 0; i < lines.size() && i < 6; i++, y += 46) {
        diamond(CX + 12 + 8, y + 23, 5.7f, rgb(kCopper));
        text(fonts.medium, 22, CX + 12 + 34, y + 23, kInk, fit(fonts.medium, 22, lines[i], CW - 70));
        row_rule(CX, CX + CW, y + 46);
    }
    y += 18;
    // (View Online: the release's own page on GitHub)
    std::string page = update ? app.update.page : upd::release_page(upd::kVersion);
    bool online = !page.empty();
    float total = (online ? 220 : 0) + (update ? 240 : 0) + (online && update ? 14 : 0), x = 640 - total * 0.5f;
    if (online) {
        if (small_button("online", x, y, 220, 38, "View Online", false) && !app.preview) plat::open(page);
        x += 234;
    }
    if (update && small_button("logupgo", x, y, 240, 38, "Update", true)) start_update();
}

static const char *kOnStart[] = {"Close Launcher", "Keep Open"};
static const char *kUpdates[] = {"Automatic", "Notify", "Off"};
static const char *kRuntimes[sys::kRuntimeKinds] = {"System Default", "WiVRn", "SteamVR", "Monado", "Custom"};
static const float kRowH = 40;   // (seven rows between the wordmark and the bottom row)

// A value with an arrow either side; true when it changed
static bool setting_value(const char *id, float y, int *value, const char *const *names, int n) {
    const float col = CX + CW - 12 - 440, cy = y + kRowH * 0.5f;
    const float vx = col + 45;   // (350 wide, an arrow either side)
    bool hl = false, hr = false;
    int was = *value;
    if (hit((std::string(id) + "<").c_str(), vx - 34, cy - 20, 34, 40, true, &hl)) *value = (*value + n - 1) % n;
    if (hit((std::string(id) + ">").c_str(), vx + 350, cy - 20, 34, 40, true, &hr)) *value = (*value + 1) % n;
    if (hit((std::string(id) + "=").c_str(), vx, cy - 20, 350, 40)) *value = (*value + 1) % n;
    arrow(vx - 16, cy, 15, false, hl ? rgb(0xF2BC8C) : rgb(kCopper));
    arrow(vx + 350 + 16, cy, 15, true, hr ? rgb(0xF2BC8C) : rgb(kCopper));
    text(fonts.medium, 21, vx + 175, cy, kInkButton, names[*value], Center);
    if (*value != was) save();
    return *value != was;
}

static void screen_settings() {
    const char *names[7] = {"On Game Start", "Updates", "VR Runtime", "Start With", "Hookshot Folder", "HookshotVR Folder", "Logs"};
    const float col = CX + CW - 12 - 440;
    float y = 325;
    for (int i = 0; i < 7; i++, y += kRowH) {
        const float cy = y + kRowH * 0.5f, by = cy - 16;
        text(fonts.medium, 22, CX + 12, cy, kInk, names[i]);
        switch (i) {
            case 0: setting_value("onstart", y, &app.cfg.on_start, kOnStart, 2); break;
            case 1: setting_value("updates", y, &app.cfg.updates, kUpdates, 3); break;
            case 2:
                // (Custom: the runtime's own OpenXR manifest; the main screen's VR row says what was found)
                if (setting_value("runtime", y, &app.cfg.runtime, kRuntimes, sys::kRuntimeKinds) && app.cfg.runtime == sys::kRuntimeCustom)
                    choose_runtime_json();
                if (app.cfg.runtime == sys::kRuntimeCustom && !app.cfg.runtime_json.empty())
                    text(fonts.medium, 19, col - 20, cy, kInkMuted, fit(fonts.medium, 19, sys::base_name(app.cfg.runtime_json), 300), Right);
                break;
            case 3: {
                // (off unless the player picks an app: the launcher never starts a runtime by itself)
                bool set = !app.cfg.start_with.empty();
                text(fonts.medium, 21, col + 60, cy, set ? kInkButton : kInkMuted,
                     set ? fit(fonts.medium, 21, sys::app_name(app.cfg.start_with), 400) : "Off", Right);
                if (small_button("swchoose", col + 74, by, 140, 32, "Choose", true)) choose_start_with();
                if (small_button("swoff", col + 226, by, 140, 32, "Off", set) && set) app.cfg.start_with.clear(), save();
                break;
            }
            case 4:
                if (small_button("hsshow", col + 74, by, 140, 32, "Show", true) && !app.preview && !app.paths.hookshot.empty())
                    sys::open_folder(app.paths.hookshot);
                if (small_button("hschange", col + 226, by, 140, 32, "Change", true)) choose_folder(&app.cfg.hookshot, "Hookshot Folder");
                break;
            case 5:
                if (small_button("hvopen", col + 74, by, 140, 32, "Open", true) && !app.preview && !app.paths.repo.empty())
                    sys::open_folder(app.paths.repo);
                if (small_button("hvchange", col + 226, by, 140, 32, "Change", true)) {
                    if (app.cfg.hookshotvr.empty()) app.cfg.hookshotvr = app.paths.repo;
                    choose_folder(&app.cfg.hookshotvr, "HookshotVR Folder");
                }
                break;
            case 6:
                if (small_button("logs", col + 74, by, 292, 32, "Open", true) && !app.preview) {
                    sys::make_dirs(app.paths.crashes);
                    sys::open_folder(app.paths.logs);
                }
                break;
        }
        row_rule(CX, CX + CW, y + kRowH);
    }
}

static void bottom_row() {
    bool over = app.overlay != Overlay::None;
    // Settings / Back
    bool hs = false;
    float sw = 34 + text_width(fonts.medium, 22, over ? "Back" : "Settings");
    if (hit("settings", CX - 4, 652, sw + 8, 36, true, &hs)) {
        if (app.overlay == Overlay::None) app.overlay = Overlay::Settings;
        else app.overlay = Overlay::None;
    }
    ImU32 ink = hs ? rgb(0xFFF0D8) : rgb(kInk);
    icon_gear(CX + 12, 671, 22, ink);
    text(fonts.medium, 22, CX + 33, 671, hs ? 0xFFF0D8 : kInk, over ? "Back" : "Settings", Left, 1, true);
    // Remember my choice
    if (app.screen == Screen::Main) {
        float rw = 36 + text_width(fonts.medium, 22, "Remember choice");
        if (hit("remember", 232, 656, rw, 30)) app.cfg.remember = !app.cfg.remember, save();
        chamfer(232, 658, 26, 26, 8, app.cfg.remember ? rgb(0x6E3F22) : rgb(0x2A1A10));
        if (!app.cfg.remember) chamfer_line(232, 658, 26, 26, 8, rgb(0x6E3F22), 1.5f);
        if (app.cfg.remember) icon_check(245, 671, 18, rgb(0xFFF0D8));
        text(fonts.medium, 22, 268, 671, kInk, "Remember choice", Left, 1, true);
    }
    // Version: the changelog
    std::string v = upd::kVersionName;
    if (app.update_ready && app.update.launcher) v += " \xC2\xB7 " + app.update.version + " ready";
    float vw = text_width(fonts.medium, 17, v);
    bool hv = false;
    if (hit("version", 1188 - vw, 604, vw, 22, true, &hv)) app.overlay = app.overlay == Overlay::Changelog ? Overlay::None : Overlay::Changelog;
    text(fonts.medium, 17, 1188, 615, hv ? kInkSoft : kInkMuted, v, Right);
    list()->AddRectFilled(P(1188 - vw, 625), P(1188, 626), rgb(kInkMuted, hv ? 0.8f : 0.4f));
    if (over) return;
    // The big button
    const float bx = 879, by = 649, bw = 311, bh = 52;
    if (app.screen == Screen::Main) {
        bool running = app.game.ok();
        bool waiting = app.vr && vr_waiting();
        // (waiting with Start With set: Play starts that app first, then the game; Starting until the runtime answers)
        if (running) big_button("play", bx, by, bw, bh, "Running", 2, 24);
        else if (app.pending_play) {
            if (big_button("play", bx, by, bw, bh, "Starting", 1, 26)) play();
        } else if (waiting && !app.vr_state.can_start) big_button("play", bx, by, bw, bh, "Waiting", 2, 24);
        else if (big_button("play", bx, by, bw, bh, "Play", 0, 29)) play();
    } else if (app.screen == Screen::Setup) {
        if (!build_ready()) big_button("build", bx, by, bw, bh, "Build", 2, 26);
        else if (big_button("build", bx, by, bw, bh, "Build", 0, 29)) start_build();
    } else {
        bool done = app.build.finished && !upd::running();
        if (big_button("cancel", bx, by, bw, bh, done ? "Back" : "Cancel", 1, 26)) {
            if (done || app.preview) {
                app.screen = Screen::Setup, app.build = sys::BuildState();
                refresh();
            } else if (upd::running()) {
                upd::cancel();
            } else {
                sys::build_cancel();
            }
        }
    }
}

static void keys() {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) app.overlay = Overlay::None;
    if (app.overlay != Overlay::None) return;
    if (app.screen == Screen::Main) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) set_mode(false);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) set_mode(true);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) play();
    }
}

static void frame_ui(float t) {
    ImGuiIO &io = ImGui::GetIO();
    begin(ImGui::GetBackgroundDrawList(), io.DisplaySize);
    draw::frame(app.frame.tex, app.frame.w, app.frame.h, app.grain);
    wordmark(app.wordmark.tex, app.wordmark.w, app.wordmark.h);
    switch (app.overlay) {
        case Overlay::Settings: screen_settings(); break;
        case Overlay::Changelog: screen_log(false); break;
        case Overlay::UpdateLog: screen_log(true); break;
        case Overlay::None:
            if (app.screen == Screen::Main) screen_main(t);
            else if (app.screen == Screen::Setup) screen_setup();
            else screen_building();
            break;
    }
    if (!app.notice.empty() && sys::now_seconds() < app.notice_until) text(fonts.medium, 20, CX + 12, 622, kAmber, app.notice);
    bottom_row();
    keys();
    win::set_hand_cursor(any_hover());
}

// ---- the game, the build and the VR checks, each frame ----
static void tick() {
    sys::poll_dialogs();
    bool ok;
    std::string text_;
    if (sys::poll_check(&ok, &text_)) app.file = ok ? FileState::Ok : FileState::Bad, app.file_text = text_;
    if (upd::running()) {
        // (the update drives its own build; the player's build stays as it was until the switch)
        upd::Progress u = upd::tick(app.cfg, app.build);
        app.update_stage = u.stage;
        if (u.stage == upd::Stage::Restart) {
            // (the new launcher, from the files just put in place, in this one's stead: it builds the game again if needed)
            // (with the environment this one was started with: the VR checks set XR_RUNTIME_JSON for their probe)
            sys::vr_watch(false, app.cfg);
            std::optional<std::string> xr = sys::inherited_runtime_json();
            sys::Proc next = plat::spawn({u.restart, "--after-update"}, {{"XR_RUNTIME_JSON", xr}}, sys::parent(u.restart), plat::Out::Null);
            if (next.ok()) {
                plat::forget(next);
                app.quit = true;
            } else {
                say("Restart HookshotVR");
            }
            app.update_ready = false, app.update_stage = upd::Stage::Idle;
            app.screen = Screen::Main, app.build = sys::BuildState();
            refresh();
        } else if (u.stage == upd::Stage::Done || u.stage == upd::Stage::Failed) {
            say(u.stage == upd::Stage::Done ? std::string("Updated to ") + upd::kVersionName : u.error.empty() ? "Update failed" : u.error);
            if (u.stage == upd::Stage::Done) app.update_ready = false;
            app.update_stage = upd::Stage::Idle;
            app.screen = Screen::Main;
            app.build = sys::BuildState();
            refresh();
        }
    } else if (app.screen == Screen::Building) {
        sys::build_poll(app.build);
        if (app.build.finished && app.build.ok) {
            app.screen = Screen::Main;
            refresh();
        } else if (app.build.finished && app.build.cancelled) {
            app.screen = Screen::Setup;
            app.build = sys::BuildState();
            refresh();
        }
    }
    if (!app.update_ready && !upd::running() && upd::available(&app.update)) app.update_ready = true;
    if (app.game.ok()) {
        int code = 0;
        if (sys::game_exited(app.game, &code)) {
            app.game = sys::Proc();
            if (sys::collect_crashes(app.paths) > 0) say("Crash report saved");
        }
    }
    if (!app.preview) {
        sys::vr_watch(app.screen == Screen::Main && app.vr, app.cfg);
        app.vr_state = sys::vr_state();
        if (app.pending_play && !vr_waiting()) play();   // (Start With's app is up: the game now)
        if (app.auto_build && app.screen == Screen::Setup && overlay_free() && build_ready()) app.auto_build = false, start_build();
        if (app.auto_update && app.update_ready && app.screen == Screen::Main && overlay_free()) app.auto_update = false, start_update();
        // (--play: once the screen is the main one and, in VR, the checks have answered and found the runtime and headset)
        if (app.auto_play && app.screen == Screen::Main && overlay_free() && !app.game.ok() &&
            (!app.vr || (app.vr_state.session != sys::Check::Unknown && !vr_waiting())))
            app.auto_play = false, play();
    }
}

// ---- the preview states (--state): the canvas's sample values ----
static bool set_preview(const std::string &st) {
    app.preview = true;
    app.paths.builder_ok = app.paths.repo_ok = app.paths.game_built = true;
    app.vr = true;
    sys::VrState &v = app.vr_state;
    v.runtime.found = true, v.runtime.name = "WiVRn", v.runtime.is = sys::kRuntimeWiVRn;
    v.session = v.headset = sys::Check::Ok;
    app.screen = Screen::Main;
    if (st == "setup") {
        app.screen = Screen::Setup;
        app.file = FileState::Ok, app.file_text = "Ocarina of Time 1.1.1";
        app.free_gb = 258;
    } else if (st == "building") {
        app.screen = Screen::Building;
        app.build.running = true, app.build.step = sys::Step::Compile, app.build.fraction = 0.63, app.build.eta = 342;
    } else if (st == "ready") {
    } else if (st == "desktop") {
        app.vr = false;
    } else if (st == "waiting") {
        v.session = sys::Check::Waiting, v.headset = sys::Check::Waiting, v.can_start = true;
    } else if (st == "notrunning") {
        v.session = sys::Check::Waiting, v.headset = sys::Check::Waiting;
    } else if (st == "noruntime") {
        v.runtime = sys::Runtime(), v.session = sys::Check::Bad, v.headset = sys::Check::Unknown, v.none_installed = true;
    } else if (st == "update" || st == "updatelog") {
        app.update_ready = true, app.update.version = "v1.1.0", app.update.page = upd::release_page("v1.1.0");
        app.update.changes = {"Per-eye fog in True stereo", "New VR menu", "Faster loading", "Fixed a crash after resizing the window"};
        if (st == "updatelog") app.overlay = Overlay::UpdateLog;
    } else if (st == "updating") {
        app.screen = Screen::Building, app.update_stage = upd::Stage::Download;
    } else if (st == "settings") {
        app.cfg.runtime = sys::kRuntimeWiVRn, app.cfg.start_with = "/var/lib/flatpak/exports/share/applications/io.github.wivrn.wivrn.desktop";
        app.overlay = Overlay::Settings;
    } else if (st == "changelog") {
        app.overlay = Overlay::Changelog;
    } else {
        return false;
    }
    return true;
}

// ---- art and fonts ----
static Art load_art(const std::vector<std::string> &dirs, const char *name) {
    Art a;
    for (auto &d : dirs) {
        img::Image im;
        if (!d.empty() && img::load_png(sys::join(d, name), im)) {
            a.tex = win::texture(im.rgba.data(), im.width, im.height, false);
            a.w = im.width, a.h = im.height;
            break;
        }
    }
    return a;
}

// The leather's grain: tiling value noise, light around 1 so it only shades the colour it is drawn with
static ImTextureID make_grain() {
    const int N = 256;
    std::vector<uint8_t> px((size_t)N * N * 4);
    auto hash = [](int x, int y, int s) {
        unsigned h = (unsigned)(x * 374761393 + y * 668265263 + s * 2246822519u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    auto noise = [&](float x, float y, int cells, int s) {
        float fx = x * cells / N, fy = y * cells / N;
        int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
        float tx = fx - x0, ty = fy - y0;
        tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
        auto at = [&](int i, int j) { return hash(((i % cells) + cells) % cells, ((j % cells) + cells) % cells, s); };
        float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
        return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
    };
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float v = 0.5f * noise(x, y, 4, 1) + 0.3f * noise(x, y, 16, 2) + 0.2f * noise(x, y, 64, 3);
            float pores = noise(x, y, 128, 4);
            float crease = fabsf(noise(x, y, 8, 5) - 0.5f);
            float l = 0.8f + 0.26f * v - (pores > 0.8f ? 0.08f : 0) - (crease < 0.02f ? 0.035f * (1 - crease / 0.02f) : 0);
            l = l < 0 ? 0 : l > 1 ? 1 : l;
            uint8_t c = (uint8_t)(l * 255);
            uint8_t *p = &px[((size_t)y * N + x) * 4];
            p[0] = p[1] = p[2] = c, p[3] = 255;
        }
    return win::texture(px.data(), N, N, true);
}

static ImFont *load_font(const std::vector<std::string> &dirs, const std::vector<const char *> &names) {
    for (auto &d : dirs)
        for (const char *n : names) {
            std::string p = sys::join(d, n);
            if (!d.empty() && sys::exists(p)) return ImGui::GetIO().Fonts->AddFontFromFileTTF(p.c_str(), 22);
        }
    return nullptr;
}

// --icon and --install: the icon drawn offscreen (its cut corners clear), then the desktop entries that start this launcher
static int install_entry(bool install, const std::string &icon, float scale, const char *argv0) {
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    if (!win::open("HookshotVR", 256, 256, true)) return 1;
    const int px = (int)(256 * (scale > 0 ? scale : 1));
    win::set_offscreen(px, px, 1);
    for (int i = 0; i < 2; i++) {
        win::begin_frame(0);
        ImGui::NewFrame();
        app_icon(ImGui::GetBackgroundDrawList(), (float)px);
        win::end_frame();
    }
    std::vector<uint8_t> rgba((size_t)px * px * 4);
    bool ok = win::read_offscreen(rgba.data());
    win::close();
    ImGui::DestroyContext();
    const float c = app_icon_cut((float)px);
    for (int y = 0; y < px && ok; y++)
        for (int x = 0; x < px; x++) {
            // (each corner's cut: coverage across its diagonal, for a smooth edge)
            float a = 1, cx[2] = {x + 0.5f, px - x - 0.5f}, cy[2] = {y + 0.5f, px - y - 0.5f};
            for (float u : cx)
                for (float v : cy) a = fminf(a, fmaxf(0, fminf(1, (u + v - c) / 1.4142f + 0.5f)));
            rgba[((size_t)y * px + x) * 4 + 3] = (uint8_t)(a * 255 + 0.5f);
        }
    sys::make_dirs(sys::parent(icon));
    if (!ok || !img::save_png(icon, rgba.data(), px, px, true)) {
        fprintf(stderr, "launcher: could not write %s\n", icon.c_str());
        return 1;
    }
    if (!install) return 0;
    std::vector<std::string> written;
    std::string err, exe = sys::join(sys::exe_dir(), sys::base_name(argv0));
    ok = plat::install_shortcut("Hookshot VR", exe, icon, &written, &err);
    for (auto &f : written) printf("%s\n", f.c_str());
    if (!err.empty()) fprintf(stderr, "launcher: %s\n", err.c_str());
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    std::string state, shot, art_dir, icon;
    bool no_art = false, install = false;
    std::string play_mode;
    float shot_scale = 1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--state") state = next();
        else if (a == "--screenshot") shot = next();
        else if (a == "--scale") shot_scale = (float)atof(next().c_str());
        else if (a == "--art") art_dir = next();
        else if (a == "--no-art") no_art = true;
        else if (a == "--install") install = true;
        else if (a == "--build") app.auto_build = true;
        else if (a == "--after-update") app.after_update = true;
        else if (a == "--update") app.auto_update = true;
        else if (a == "--play") {
            app.auto_play = true;
            if (i + 1 < argc && (!strcmp(argv[i + 1], "vr") || !strcmp(argv[i + 1], "desktop"))) play_mode = argv[++i];
        }
        else if (a == "--icon") icon = next();
        else if (a == "--fake-build") {
            app.fake_build = plat::real_path(next());   // (the builder runs in the repository's folder)
        }
        else if (a == "--help" || a == "-h") {
            printf("hookshotvr-launcher [--build] [--play [vr|desktop]] [--update]\n"
                   "hookshotvr-launcher [--state setup|building|ready|desktop|waiting|notrunning|noruntime|update|updating|settings|\n"
                   "                             changelog|updatelog]\n"
                   "                    [--screenshot <file.png> [--scale <n>]] [--art <folder> | --no-art] [--fake-build <script.py>]\n"
                   "hookshotvr-launcher --install                  the Hookshot VR entry in the app menu and on the desktop\n"
                   "hookshotvr-launcher --icon <file.png> [--scale <n>]\n");
            return 0;
        } else {
            fprintf(stderr, "launcher: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (shot_scale <= 0) shot_scale = 1;
    if (!shot.empty() && state.empty()) state = "ready";
    if ((!shot.empty() || !icon.empty() || install) && !img::available()) {
        fprintf(stderr, "launcher: built without libpng: no --screenshot, --icon or --install\n");
        return 2;
    }
    if (install || !icon.empty()) return install_entry(install, icon.empty() ? sys::join(sys::data_dir(), "hookshotvr.png") : icon,
                                                       install ? 1 : shot_scale, argv[0]);

    sys::load_config(app.cfg);
    app.vr = app.cfg.mode != "desktop";
    if (!play_mode.empty()) app.vr = play_mode == "vr";   // (this start's mode, not saved)
    app.paths = sys::paths(app.cfg);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    if (!win::open("HookshotVR", (int)kW, (int)kH, !shot.empty())) return 1;

    std::string here = sys::exe_dir(), repo = app.paths.repo.empty() ? sys::find_repo() : app.paths.repo;
    std::vector<std::string> font_dirs = {sys::join(here, "fonts"), repo.empty() ? "" : sys::join(repo, "vr/launcher/fonts"),
                                          repo.empty() ? "" : sys::join(repo, "third_party/fonts/libertinus")};
    fonts.medium = load_font(font_dirs, {"EBGaramond-Medium.ttf", "EBGaramond-Regular.ttf", "LibertinusSerif-Semibold.ttf"});
    fonts.semibold = load_font(font_dirs, {"EBGaramond-SemiBold.ttf", "LibertinusSerif-Semibold.ttf"});
    fonts.italic = load_font(font_dirs, {"EBGaramond-Italic.ttf", "EBGaramond-MediumItalic.ttf", "LibertinusSerif-Semibold.ttf"});
    if (!fonts.medium) fonts.medium = io.Fonts->AddFontDefault();
    if (!fonts.semibold) fonts.semibold = fonts.medium;
    if (!fonts.italic) fonts.italic = fonts.medium;

    std::vector<std::string> art_dirs = {art_dir, sys::join(here, "art"), repo.empty() ? "" : sys::join(repo, "vr/launcher/art")};
    if (no_art) art_dirs = {art_dir};   // (the drawn fallbacks, unless --art names a folder)
    app.frame = load_art(art_dirs, "frame.png");
    app.wordmark = load_art(art_dirs, "wordmark.png");
    app.icon_desktop = load_art(art_dirs, "icon_desktop.png");
    app.icon_vr = load_art(art_dirs, "icon_vr.png");
    app.grain = make_grain();

    std::string log = read_text(sys::join(here, "CHANGELOG.txt"));
    if (log.empty() && !repo.empty()) log = read_text(sys::join(repo, "vr/launcher/CHANGELOG.txt"));
    app.changes = upd::changes_of(log, kVersion);

    bool live = state == "live";
    if (live) state.clear(), no_save = true;
    if (!state.empty()) {
        if (!set_preview(state)) {
            fprintf(stderr, "launcher: no state %s\n", state.c_str());
            return 2;
        }
    } else {
        refresh();
        if (!app.cfg.game_file.empty() && app.screen == Screen::Setup && app.paths.repo_ok && app.paths.builder_ok) {
            app.file = FileState::Checking;
            sys::check_game_file(app.paths, app.cfg.game_file, app.fake_build);
        }
        if (!no_save && sys::collect_crashes(app.paths) > 0) say("Crash report saved");
        if (!no_save) {
            upd::tidy(app.paths, app.cfg);
            // (an update's rebuild still to do: the banner offers it, and right after the update it starts by itself)
            if (upd::pending(app.paths, &app.update)) {
                app.update.changes = app.changes, app.update_ready = true;
                if (app.after_update) start_update();
            }
            if (app.after_update) say(std::string("Updated to ") + upd::kVersionName);
            upd::check(app.paths, app.cfg);
        }
    }

    if (!shot.empty()) {
        int w = (int)(kW * shot_scale), h = (int)(kH * shot_scale);
        win::set_offscreen(w, h, shot_scale);
        if (live) {
            // (the VR checks' first answer, then the screen as this PC has it)
            sys::vr_watch(true, app.cfg);
            for (int i = 0; i < 30 && app.vr_state.session == sys::Check::Unknown; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                tick();
            }
            for (int i = 0; i < 20 && app.file == FileState::Checking; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                tick();
            }
        }
        // (a few frames: the font atlas fills in the sizes the first frame asked for)
        for (int i = 0; i < 4; i++) {
            win::begin_frame(0);
            ImGui::NewFrame();
            frame_ui((float)i);
            win::end_frame();
        }
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        bool ok = win::read_offscreen(rgba.data()) && img::save_png(shot, rgba.data(), w, h);
        win::close();
        if (!ok) fprintf(stderr, "launcher: could not write %s\n", shot.c_str());
        return ok ? 0 : 1;
    }

    double t0 = sys::now_seconds();
    while (!app.quit) {
        bool busy = app.screen == Screen::Building || ImGui::IsMouseDown(0);
        if (!win::begin_frame(busy ? 16 : 50)) break;
        if (!app.preview) tick();
        ImGui::NewFrame();
        frame_ui((float)(sys::now_seconds() - t0));
        win::end_frame();
    }
    // (closing the window stops the build: nothing half-made is left running. An update cleans up after itself next start)
    if (upd::running()) upd::cancel();
    else if (app.screen == Screen::Building) sys::build_cancel();
    sys::vr_watch(false, app.cfg);
    win::close();
    ImGui::DestroyContext();
    return 0;
}
