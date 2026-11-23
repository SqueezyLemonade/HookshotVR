// The launcher's system side without a window: the build's @@ lines, Cancel, the game file's check, the VR runtime choice,
// two launches of a stand-in game, Start With, and an update (a good one and a failed one). Never the real builder or game.
// HOME, XDG_CONFIG_HOME and XDG_DATA_HOME point into the scratch folder, so nothing of this PC's is read or written.
//
//   vr/launcher/build.sh test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include "../src/system.h"
#include "../src/update.h"

static int fails;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static sys::BuildState run_build(const sys::Paths &p, const std::string &fake, bool cancel_midway) {
    sys::BuildState s;
    s.running = true;
    EXPECT(sys::build_start(p, sys::first_build(p, "/nowhere/game.nup"), fake));
    int last = -1;
    for (int i = 0; i < 400 && !s.finished; i++) {
        usleep(50000);
        sys::build_poll(s);
        if ((int)s.step != last) printf("  step %d\n", last = (int)s.step);
        if (cancel_midway && s.step == sys::Step::Translate) sys::build_cancel();
    }
    return s;
}

static void write(const std::string &path, const std::string &text) {
    sys::make_dirs(sys::parent(path));
    std::ofstream f(path);
    f << text;
}
static std::string manifest(const char *name, const char *lib) {
    return std::string("{\"file_format_version\": \"1.0.0\", \"runtime\": {\"name\": \"") + name + "\", \"library_path\": \"" + lib + "\"}}\n";
}

// The stand-in game's output (its environment, one variable a line)
static std::string launch(const sys::Paths &p, const sys::Config &c, bool vr) {
    sys::Proc game;
    std::string err;
    EXPECT(sys::launch_game(p, c, vr, sys::find_runtime(c), &game, &err));
    int code = -1;
    for (int i = 0; i < 100 && !sys::game_exited(game, &code); i++) usleep(20000);
    return "\n" + sys::read_file(sys::join(c.logs, "last-run.log"));
}
static bool has(const std::string &out, const std::string &line) { return out.find("\n" + line + "\n") != std::string::npos; }

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "selftest <scratch folder> <fake_make_game.py>\n");
        return 2;
    }
    std::string dir = argv[1], fake = argv[2];
    setenv("FAKE_SPEED", "8", 1);
    setenv("HOME", sys::join(dir, "home").c_str(), 1);
    setenv("XDG_CONFIG_HOME", sys::join(dir, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", sys::join(dir, "data").c_str(), 1);
    unsetenv("XDG_CACHE_HOME");
    // A Hookshot Folder of stand-ins: builder/'s python, and main-game/hookshot printing its environment
    std::string hs = sys::join(dir, "hookshot");
    sys::make_dirs(sys::join(hs, "builder/usr/python/bin"));
    if (symlink("/usr/bin/python3", sys::join(hs, "builder/usr/python/bin/python3").c_str()) != 0) perror("symlink");
    write(sys::join(hs, "main-game/hookshot"), "#!/bin/sh\necho cwd=$(pwd)\nenv | sort\n");
    chmod(sys::join(hs, "main-game/hookshot").c_str(), 0755);
    write(sys::join(hs, "main-game/hookshot_env.txt"), "# the player's own\n");
    sys::make_dirs(sys::join(hs, "cache/nvidia"));
    sys::Config c;
    c.hookshot = hs;
    c.logs = sys::join(dir, "logs");
    sys::Paths p = sys::paths(c);
    printf("repo %s builder %d repo %d built %d\n", p.repo.c_str(), p.builder_ok, p.repo_ok, p.game_built);
    EXPECT(p.builder_ok && p.repo_ok && p.game_built);

    printf("build\n");
    sys::BuildState s = run_build(p, fake, false);
    EXPECT(s.finished && s.ok && s.step == sys::Step::Shaders && s.folder == p.game);
    printf("build that fails\n");
    setenv("FAKE_FAIL", "1", 1);
    s = run_build(p, fake, false);
    unsetenv("FAKE_FAIL");
    EXPECT(s.finished && !s.ok && s.error == "The compiler stopped." && s.step == sys::Step::Compile);
    printf("cancel\n");
    s = run_build(p, fake, true);
    EXPECT(s.finished && !s.ok && s.cancelled);

    printf("check\n");
    sys::check_game_file(p, "/nowhere/game.nup", fake);
    bool ok = false;
    std::string text;
    for (int i = 0; i < 100 && !sys::poll_check(&ok, &text); i++) usleep(50000);
    EXPECT(ok && text == "Ocarina of Time 1.1.1");

    printf("runtimes\n");
    std::string active = sys::join(dir, "config/openxr/1/active_runtime.json");
    std::string steam = sys::join(dir, "home/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json");
    std::string custom = sys::join(dir, "elsewhere/my_runtime.json");
    write(active, manifest("WiVRn", "libopenxr_wivrn.so"));
    write(steam, manifest("SteamVR/OpenXR", "bin/linux64/vrclient.so"));
    write(custom, manifest("Some Runtime", "libsome.so"));
    for (auto &r : sys::installed_runtimes()) printf("  installed %d '%s' %s\n", r.is, r.name.c_str(), r.json.c_str());
    sys::Runtime r = sys::find_runtime(c);
    EXPECT(r.found && r.json == active && r.name == "WiVRn" && r.is == sys::kRuntimeWiVRn && r.set_env.empty());
    c.runtime = sys::kRuntimeSteamVR;
    r = sys::find_runtime(c);
    EXPECT(r.found && r.json == steam && r.is == sys::kRuntimeSteamVR && r.set_env == steam);
    c.runtime = sys::kRuntimeWiVRn;
    r = sys::find_runtime(c);
    EXPECT(r.found && r.json == active && r.set_env == active);   // (the active one, being WiVRn)
    c.runtime = sys::kRuntimeCustom, c.runtime_json = custom;
    r = sys::find_runtime(c);
    EXPECT(r.found && r.json == custom && r.name == "Some Runtime" && r.is == sys::kRuntimeCustom && r.set_env == custom);
    c.runtime_json = sys::join(dir, "missing.json");
    EXPECT(!sys::find_runtime(c).found);
    EXPECT(sys::runtime_kind_of("/opt/x/openxr_monado.json", "Monado") == sys::kRuntimeMonado);

    printf("launch in VR on SteamVR, with HOOKSHOT_VR=0 and RX_VK_VRAM_MB unset\n");
    c.runtime = sys::kRuntimeSteamVR;
    setenv("HOOKSHOT_VR", "0", 1);
    unsetenv("RX_VK_VRAM_MB");
    std::string out = launch(p, c, true);
    for (const char *want : {"HOOKSHOT_VR=1", "HOS_FPS_TARGET=120", "RX_VK_IMMEDIATE=1", "HOOKSHOT_NO_UPDATE_CHECK=1", "HOS_INPUT_BACKGROUND=1",
                             "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1", "RX_VK_PRECOMPILE=0", "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1"}) {
        if (!has(out, want)) printf("  missing %s\n", want);
        EXPECT(has(out, want));
    }
    EXPECT(has(out, "XR_RUNTIME_JSON=" + steam));
    EXPECT(has(out, "__GL_SHADER_DISK_CACHE_PATH=" + sys::join(hs, "cache/nvidia")));
    EXPECT(out.find("RX_VK_VRAM_MB=") == std::string::npos);   // (the RTX 4070's alone: not for every card)
    EXPECT(out.find("cwd=" + p.game) != std::string::npos);
    {   // (the saves' folder said, so the settings live there and not beside the build)
        std::vector<std::string> args = sys::game_args(p), want{"--user", sys::join(dir, "data/hookshot")};
        EXPECT(args == want);
    }
    printf("launch in VR on System, XR_RUNTIME_JSON set in the launcher by a probe\n");
    c.runtime = sys::kRuntimeSystem;
    setenv("XR_RUNTIME_JSON", custom.c_str(), 1);
    out = launch(p, c, true);
    EXPECT(out.find("XR_RUNTIME_JSON=") == std::string::npos);   // (as the launcher was started: unset)
    unsetenv("XR_RUNTIME_JSON");
    printf("desktop launch, with HOOKSHOT_VR=1 inherited\n");
    setenv("HOOKSHOT_VR", "1", 1);
    out = launch(p, c, false);
    EXPECT(has(out, "HOOKSHOT_VR=0") && !has(out, "HOOKSHOT_VR=1") && has(out, "HOOKSHOT_NO_UPDATE_CHECK=1"));
    EXPECT(out.find("HOS_FPS_TARGET=") == std::string::npos);
    EXPECT(sys::exists(sys::join(c.logs, "last-run.1.log")));

    printf("start with\n");
    std::string mark = sys::join(dir, "started");
    write(sys::join(dir, "app.desktop"), "[Desktop Entry]\nName=Some Dashboard\nExec=touch \"" + mark + "\" %U\n[Desktop Action x]\nExec=false\n");
    c.start_with = sys::join(dir, "app.desktop");
    EXPECT(sys::app_name(c.start_with) == "Some Dashboard");
    EXPECT(sys::start_with(c));
    for (int i = 0; i < 100 && !sys::exists(mark); i++) usleep(20000);
    EXPECT(sys::exists(mark));
    c.start_with.clear();
    EXPECT(!sys::start_with(c));

    printf("desktop entry\n");
    {
        // (the menu's and the desktop's, both in the scratch HOME; a file of that name the launcher did not write is kept)
        std::string desk = sys::join(dir, "home/Desktop"), mine = sys::join(desk, "Hookshot VR.desktop");
        std::string menu = sys::join(dir, "data/applications/hookshotvr.desktop");
        sys::make_dirs(desk);
        write(mine, "[Desktop Entry]\nName=mine\n");
        std::string exe = sys::join(dir, "My Games/100% \"VR\"/hookshotvr-launcher"), err;
        std::vector<std::string> wrote;
        EXPECT(!plat::install_shortcut("Hookshot VR", exe, "/x/hookshotvr.png", &wrote, &err) && !err.empty());
        EXPECT(wrote.size() == 1 && sys::read_file(mine) == "[Desktop Entry]\nName=mine\n");
        std::string m = "\n" + sys::read_file(menu);
        EXPECT(has(m, "Terminal=false") && has(m, "Name=Hookshot VR") && has(m, "Icon=/x/hookshotvr.png"));
        EXPECT(has(m, "Exec=\"" + dir + "/My Games/100%% \\\\\"VR\\\\\"/hookshotvr-launcher\""));
        sys::remove_tree(mine);
        wrote.clear(), err.clear();
        EXPECT(plat::install_shortcut("Hookshot VR", exe, "/x/hookshotvr.png", &wrote, &err) && wrote.size() == 2 && err.empty());
        EXPECT(plat::is_exe(mine) && sys::read_file(mine) == sys::read_file(menu));
        EXPECT(plat::install_shortcut("Hookshot VR", exe, "/x/hookshotvr.png", nullptr, nullptr));   // (its own again: replaced)
    }

    printf("update\n");
    setenv("FAKE_GAME", "1", 1);
    upd::Release rel;
    rel.version = "v0.2.0", rel.source = p.repo;   // (downloaded already: the self-test's own source)
    EXPECT(upd::start(p, c, rel, fake));
    sys::BuildState b;
    upd::Progress u;
    for (int i = 0; i < 600 && (u = upd::tick(c, b)).stage != upd::Stage::Done && u.stage != upd::Stage::Failed; i++) usleep(50000);
    printf("  stage %d %s\n", (int)u.stage, u.error.c_str());
    EXPECT(u.stage == upd::Stage::Done);
    EXPECT(c.game_folder == "v0.2.0-game" && c.hookshotvr == p.repo);
    EXPECT(!sys::exists(sys::join(hs, "main-game")));   // (the old build cleaned up)
    EXPECT(sys::exists(sys::join(hs, "v0.2.0-game/hookshot_env.txt")));
    EXPECT(sys::exists(p.repo));   // (a source the player chose is never deleted)
    sys::Config saved;
    sys::load_config(saved);
    EXPECT(saved.game_folder == "v0.2.0-game");
    sys::Paths p2 = sys::paths(saved);
    EXPECT(p2.game_built && p2.game == sys::join(hs, "v0.2.0-game"));
    printf("update that fails\n");
    setenv("FAKE_FAIL", "1", 1);
    rel.version = "v0.3.0";
    EXPECT(upd::start(p2, c, rel, fake));
    for (int i = 0; i < 600 && (u = upd::tick(c, b)).stage != upd::Stage::Done && u.stage != upd::Stage::Failed; i++) usleep(50000);
    unsetenv("FAKE_FAIL");
    EXPECT(u.stage == upd::Stage::Failed && u.error == "The compiler stopped.");
    EXPECT(c.game_folder == "v0.2.0-game" && !sys::exists(sys::join(hs, "v0.3.0-game")) && sys::paths(c).game_built);
    printf("tidy\n");
    sys::make_dirs(sys::join(hs, "v9.9.9-game"));
    sys::make_dirs(sys::join(sys::data_dir(), "versions/v9.9.9/HookshotVR"));
    upd::tidy(sys::paths(c), c);
    EXPECT(!sys::exists(sys::join(hs, "v9.9.9-game")) && !sys::exists(sys::join(sys::data_dir(), "versions/v9.9.9")));
    EXPECT(sys::exists(sys::join(hs, "v0.2.0-game")));
    EXPECT(upd::newer("v0.10.0", "v0.9.1") && !upd::newer("v0.1.0", "v0.1.0"));

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
