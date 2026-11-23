// The launcher's system side without a window: the build's @@ lines, Cancel, the game file's check, the VR runtime choice,
// two launches of a stand-in game, Start With, and the update against a stand-in GitHub (tools/fake_release_server.py):
// offline and rate-limited (quiet), a launcher-only release (no rebuild), a game release (rebuilt), a bad checksum (refused)
// and a rebuild that fails. Never the real builder, game or GitHub.
// HOME, XDG_CONFIG_HOME and XDG_DATA_HOME point into the scratch folder, so nothing of this PC's is read or written.
//
//   vr/launcher/build.sh test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

// ---- the update's stand-ins ----
static void source_at(const std::string &d, const std::string &stamp) {
    write(sys::join(d, "nx2/make_game.py"), "# stand-in\n");
    write(sys::join(d, "vr/launcher/README.md"), "stand-in\n");
    write(sys::join(d, "GAME_STAMP"), stamp + "\n");
}
static void game_at(const std::string &d, const std::string &stamp) {
    write(sys::join(d, "hookshot"), "#!/bin/sh\necho cwd=$(pwd)\nenv | sort\n");
    chmod(sys::join(d, "hookshot").c_str(), 0755);
    sys::make_dirs(sys::join(d, "romfs"));
    write(sys::join(d, "hookshot_env.txt"), "# the player's own\n");
    write(sys::join(d, ".hookshotvr_stamp"), stamp + "\n");
}
static std::string shell(const std::string &cmd) {
    std::string out;
    if (FILE *f = popen(cmd.c_str(), "r")) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        pclose(f);
    }
    return out;
}
static std::string q(const std::string &s) { return "'" + s + "'"; }   // (the scratch paths have no quote in them)
// One release on the stand-in GitHub: its archive (a launcher saying the tag, CHANGELOG.txt, the source with its stamp, a
// builder), SHA256SUMS.txt (made by sha256sum; bad: one digit changed), GAME_STAMPS.txt, CHANGELOG.txt at the tag, and the
// releases list, newest first
static void make_release(const std::string &dir, const std::string &srv, const std::string &tag, const std::string &stamp, bool good,
                         std::vector<std::string> *tags, const std::string &extra = "") {
    std::string st = sys::join(dir, "stage-" + tag), top = sys::join(st, "HookshotVR");
    std::string log = tag + "\nWhat is new in " + tag + "\n\nv1.0.1\nThis one\n";
    write(sys::join(top, plat::launcher_file()), "#!/bin/sh\necho " + tag + "\n");
    chmod(sys::join(top, plat::launcher_file()).c_str(), 0755);
    write(sys::join(top, "CHANGELOG.txt"), log);
    write(sys::join(top, "fonts/x.ttf"), "font\n");
    if (!extra.empty()) write(sys::join(top, extra + "/file"), "new\n");
    source_at(sys::join(top, "HookshotVR"), stamp);
    sys::make_dirs(sys::join(top, "builder/usr/python/bin"));
    if (symlink("/usr/bin/python3", sys::join(top, "builder/usr/python/bin/python3").c_str()) != 0) perror("symlink");
    std::string name = "HookshotVR-Alpha-" + tag + plat::release_suffix();
    std::string files = sys::join(srv, "site/SqueezyLemonade/HookshotVR/releases/download/" + tag);
    sys::make_dirs(files);
    shell("tar -czf " + q(sys::join(files, name)) + " -C " + q(st) + " HookshotVR");
    std::string sum = shell("cd " + q(files) + " && sha256sum " + q(name));
    if (!good) sum[0] = sum[0] == '0' ? '1' : '0';
    write(sys::join(files, "SHA256SUMS.txt"), sum);
    write(sys::join(files, "GAME_STAMPS.txt"), stamp + "  " + name + "\n");
    write(sys::join(srv, "raw/SqueezyLemonade/HookshotVR/" + tag + "/vr/launcher/CHANGELOG.txt"), log);
    tags->insert(tags->begin(), tag);
    std::string json = "[";
    for (auto &t : *tags) {
        std::string n = "HookshotVR-Alpha-" + t + plat::release_suffix();
        long size = (long)sys::read_file(sys::join(srv, "site/SqueezyLemonade/HookshotVR/releases/download/" + t + "/" + n)).size();
        json += std::string(json.size() > 1 ? "," : "") + "{\"tag_name\": \"" + t + "\", \"draft\": false, \"prerelease\": true, " +
                "\"body\": \"# HookshotVR " + t + "\\r\\n\\r\\n- A **change**\", \"assets\": [" +
                "{\"name\": \"" + n + "\", \"size\": " + std::to_string(size) + "}, {\"name\": \"SHA256SUMS.txt\", \"size\": 100}, " +
                "{\"name\": \"GAME_STAMPS.txt\", \"size\": 80}, {\"name\": \"HookshotVR-Alpha-" + t + "-windows-x64.zip\", \"size\": 5}]}";
    }
    json += ",{\"tag_name\": \"v99.0.0\", \"draft\": true, \"assets\": []}]";   // (a draft: never offered)
    write(sys::join(srv, "api/repos/SqueezyLemonade/HookshotVR/releases"), json);
}
static int closed_port() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t n = sizeof a;
    bind(s, (sockaddr *)&a, n);
    getsockname(s, (sockaddr *)&a, &n);
    close(s);   // (nobody listens there now: a connection is refused, as offline)
    return ntohs(a.sin_port);
}
static sys::Proc start_server(const std::string &script, const std::string &folder, std::string *base) {
    sys::Proc pr = plat::spawn({"python3", "-I", script, folder}, {}, "", plat::Out::Pipe);
    std::string line;
    char c;
    while (pr.ok() && plat::read_out(pr, &c, 1) == 1 && c != '\n') line += c;
    if (line.compare(0, 5, "port ") == 0) *base = "http://127.0.0.1:" + line.substr(5);
    return pr;
}
// A new start's check, waited for: true when it found a release newer than this launcher
static bool checked_and_found(const sys::Paths &p, const sys::Config &c, upd::Release *r) {
    upd::forget_check();
    upd::check(p, c);
    for (int i = 0; i < 600 && !upd::checked(); i++) usleep(50000);
    EXPECT(upd::checked());
    return upd::available(r);
}
static upd::Progress run_update(const sys::Paths &p, const sys::Config &c0, const upd::Release &r, const std::string &fake) {
    sys::Config &c = const_cast<sys::Config &>(c0);
    sys::BuildState b;
    upd::Progress u;
    EXPECT(upd::start(p, c, r, fake));
    for (int i = 0; i < 1200; i++) {
        u = upd::tick(c, b);
        if (u.stage == upd::Stage::Done || u.stage == upd::Stage::Failed || u.stage == upd::Stage::Restart) break;
        usleep(25000);
    }
    return u;
}

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
    {   // (the launcher's folder as it was started: the update renames the running exe aside, and /proc/self/exe follows it)
        std::string here = plat::exe_dir(), self = sys::join(here, "selftest"), aside = self + ".aside";
        EXPECT(rename(self.c_str(), aside.c_str()) == 0);
        EXPECT(plat::exe_dir() == here);
        EXPECT(rename(aside.c_str(), self.c_str()) == 0);
    }
    setenv("FAKE_GAME", "1", 1);
    {
        // A release's folder as a player unpacked it (the launcher, its CHANGELOG.txt, the source with its GAME_STAMP, the
        // builder) with a game built from that source, and a stand-in GitHub serving newer releases
        std::string R = sys::join(dir, "install"), srv = sys::join(dir, "releases");
        write(sys::join(R, plat::launcher_file()), "#!/bin/sh\necho v1.0.1\n");
        chmod(sys::join(R, plat::launcher_file()).c_str(), 0755);
        write(sys::join(R, "CHANGELOG.txt"), "v1.0.1\nThis one\n");
        source_at(sys::join(R, "HookshotVR"), "stamp-A");
        sys::make_dirs(sys::join(R, "builder/usr/python/bin"));
        if (symlink("/usr/bin/python3", sys::join(R, "builder/usr/python/bin/python3").c_str()) != 0) perror("symlink");
        game_at(sys::join(R, "main-game"), "stamp-A");
        upd::set_install_root(R);
        sys::Config uc;
        uc.hookshot = R, uc.hookshotvr = sys::join(R, "HookshotVR"), uc.logs = sys::join(dir, "logs");
        sys::Paths up = sys::paths(uc);
        EXPECT(up.game_built && up.builder_ok && up.repo_ok && sys::game_stamp(up.game) == "stamp-A");
        EXPECT(!upd::pending(up, nullptr));

        printf("  offline\n");
        setenv("HOOKSHOTVR_DEV_RELEASES", ("http://127.0.0.1:" + std::to_string(closed_port())).c_str(), 1);
        upd::Release got;
        EXPECT(!checked_and_found(up, uc, &got));

        printf("  rate-limited\n");
        sys::make_dirs(srv);
        std::string base;
        sys::Proc server = start_server(sys::join(sys::parent(fake), "fake_release_server.py"), srv, &base);
        EXPECT(server.ok() && !base.empty());
        setenv("HOOKSHOTVR_DEV_RELEASES", base.c_str(), 1);
        std::vector<std::string> tags;
        make_release(dir, srv, "v9.0.0", "stamp-A", true, &tags);   // (listed, but GitHub says no)
        write(sys::join(srv, ".ratelimited"), "");
        EXPECT(!checked_and_found(up, uc, &got));
        sys::remove_tree(sys::join(srv, ".ratelimited"));

        printf("  launcher-only update\n");
        EXPECT(checked_and_found(up, uc, &got));
        EXPECT(got.version == "v9.0.0" && got.launcher && !got.rebuild && !got.downloaded && got.stamp == "stamp-A");
        EXPECT(got.archive == std::string("HookshotVR-Alpha-v9.0.0") + plat::release_suffix());
        EXPECT(got.changes.size() == 1 && got.changes[0] == "What is new in v9.0.0");
        EXPECT(got.page == base + "/site/SqueezyLemonade/HookshotVR/releases/tag/v9.0.0");
        upd::Progress u = run_update(up, uc, got, fake);
        printf("    stage %d %s\n", (int)u.stage, u.error.c_str());
        EXPECT(u.stage == upd::Stage::Restart && !u.rebuild && u.restart == sys::join(R, plat::launcher_file()));
        EXPECT(sys::read_file(sys::join(R, plat::launcher_file())) == "#!/bin/sh\necho v9.0.0\n" && plat::is_exe(sys::join(R, plat::launcher_file())));
        EXPECT(sys::read_file(sys::join(R, ".update/old/" + std::string(plat::launcher_file()))) == "#!/bin/sh\necho v1.0.1\n");
        EXPECT(sys::read_file(sys::join(R, "CHANGELOG.txt")).compare(0, 7, "v9.0.0\n") == 0);
        EXPECT(!sys::exists(sys::join(R, ".update/new")));
        // (the game kept as it was: not built, the same folder, the player's file in it)
        EXPECT(uc.game_folder == "main-game" && sys::game_stamp(sys::join(R, "main-game")) == "stamp-A");
        EXPECT(sys::exists(sys::join(R, "main-game/hookshot_env.txt")) && !sys::exists(sys::join(uc.logs, "build-v9.0.0.log")));
        // (the new launcher starts: its version is the newest release's, so no banner; it clears what the update left)
        upd::set_version("v9.0.0");
        up = sys::paths(uc);
        upd::tidy(up, uc);
        for (int i = 0; i < 100 && sys::exists(sys::join(R, ".update")); i++) usleep(50000);
        EXPECT(!sys::exists(sys::join(R, ".update")));
        EXPECT(!checked_and_found(up, uc, &got) && !upd::pending(up, nullptr));

        printf("  game update (Automatic: downloaded with the check)\n");
        make_release(dir, srv, "v9.1.0", "stamp-B", true, &tags);
        uc.updates = sys::kUpdatesAuto;
        EXPECT(checked_and_found(up, uc, &got));
        EXPECT(got.version == "v9.1.0" && got.rebuild && got.downloaded && upd::needed_gb(got) > 60);
        u = run_update(up, uc, got, fake);
        EXPECT(u.stage == upd::Stage::Restart && u.rebuild);
        EXPECT(sys::source_stamp(sys::join(R, "HookshotVR")) == "stamp-B" && sys::game_stamp(sys::join(R, "main-game")) == "stamp-A");
        upd::set_version("v9.1.0");
        up = sys::paths(uc);
        upd::tidy(up, uc);
        upd::Release again;
        EXPECT(upd::pending(up, &again) && again.rebuild && !again.launcher && again.version == "v9.1.0");
        u = run_update(up, uc, again, fake);   // (the new launcher's --after-update)
        printf("    stage %d %s\n", (int)u.stage, u.error.c_str());
        EXPECT(u.stage == upd::Stage::Done);
        EXPECT(uc.game_folder == "v9.1.0-game" && sys::game_stamp(sys::join(R, "v9.1.0-game")) == "stamp-B");
        EXPECT(sys::exists(sys::join(R, "v9.1.0-game/hookshot_env.txt")) && !sys::exists(sys::join(R, "main-game")));
        sys::Config saved;
        sys::load_config(saved);
        EXPECT(saved.game_folder == "v9.1.0-game");
        up = sys::paths(uc);
        EXPECT(up.game_built && !upd::pending(up, nullptr));
        uc.updates = sys::kUpdatesNotify;
        for (int i = 0; i < 100 && sys::exists(sys::join(R, ".update")); i++) usleep(50000);
        EXPECT(!checked_and_found(up, uc, &got));

        printf("  bad checksum\n");
        make_release(dir, srv, "v9.2.0", "stamp-B", false, &tags);
        EXPECT(checked_and_found(up, uc, &got) && got.version == "v9.2.0" && !got.rebuild);
        u = run_update(up, uc, got, fake);
        EXPECT(u.stage == upd::Stage::Failed && u.error == "Bad download");
        EXPECT(sys::read_file(sys::join(R, plat::launcher_file())) == "#!/bin/sh\necho v9.1.0\n");
        EXPECT(!sys::exists(sys::join(R, ".update/new")) && !sys::exists(sys::join(R, ".update/download/v9.2.0/" + got.archive)));
        EXPECT(uc.game_folder == "v9.1.0-game");

        printf("  install that fails midway (rolled back)\n");
        {
            // (zzz, last in the archive: the folder there cannot be moved away, its own permission taken off)
            std::string zzz = sys::join(R, "zzz");
            write(sys::join(zzz, "file"), "old\n");
            chmod(zzz.c_str(), 0555);
            make_release(dir, srv, "v9.3.0", "stamp-B", true, &tags, "zzz");
            EXPECT(checked_and_found(up, uc, &got) && got.version == "v9.3.0");
            u = run_update(up, uc, got, fake);
            printf("    stage %d %s\n", (int)u.stage, u.error.c_str());
            EXPECT(u.stage == upd::Stage::Failed && u.error == "Install failed");
            EXPECT(sys::read_file(sys::join(R, plat::launcher_file())) == "#!/bin/sh\necho v9.1.0\n");
            EXPECT(sys::read_file(sys::join(R, "CHANGELOG.txt")).compare(0, 7, "v9.1.0\n") == 0);
            EXPECT(sys::exists(sys::join(R, "HookshotVR/nx2/make_game.py")) && sys::exists(sys::join(R, "builder/usr/python/bin/python3")));
            EXPECT(sys::read_file(sys::join(zzz, "file")) == "old\n" && !sys::exists(sys::join(R, ".update/new")));
            chmod(zzz.c_str(), 0755);
            sys::remove_tree(zzz);
        }

        printf("  rebuild that fails\n");
        write(sys::join(R, "HookshotVR/GAME_STAMP"), "stamp-C\n");
        up = sys::paths(uc);
        EXPECT(upd::pending(up, &again));
        setenv("FAKE_FAIL", "1", 1);
        u = run_update(up, uc, again, fake);
        unsetenv("FAKE_FAIL");
        EXPECT(u.stage == upd::Stage::Failed && u.error == "The compiler stopped.");
        EXPECT(uc.game_folder == "v9.1.0-game" && sys::paths(uc).game_built && !sys::exists(sys::join(R, "v9.1.0.1-game")));
        EXPECT(upd::pending(sys::paths(uc), nullptr));   // (still offered)

        printf("  tidy\n");
        sys::make_dirs(sys::join(R, "v9.9.9-game"));
        sys::make_dirs(sys::join(sys::data_dir(), "versions/v9.9.9/HookshotVR"));
        upd::tidy(sys::paths(uc), uc);
        EXPECT(!sys::exists(sys::join(R, "v9.9.9-game")) && !sys::exists(sys::join(sys::data_dir(), "versions/v9.9.9")));
        EXPECT(sys::exists(sys::join(R, "v9.1.0-game")));
        plat::kill_tree(server);
        plat::wait(server);
        unsetenv("HOOKSHOTVR_DEV_RELEASES");
    }
    EXPECT(upd::newer("v0.10.0", "v0.9.1") && !upd::newer("v0.1.0", "v0.1.0"));
    {   // (SHA-256 against FIPS 180-4's examples)
        std::string f = sys::join(dir, "abc.txt");
        write(f, "abc");
        EXPECT(upd::sha256_file(f) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        write(f, "");
        EXPECT(upd::sha256_file(f) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        write(f, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
        EXPECT(upd::sha256_file(f) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }
    {   // (the game file's check runs in the Hookshot Folder's work folder, not the builder's default on the system drive)
        std::string args = sys::join(dir, "check-args.txt");
        setenv("FAKE_ARGS", args.c_str(), 1);
        sys::check_game_file(p, "/nowhere/game.nup", fake);
        bool ok2 = false;
        std::string t2;
        for (int i = 0; i < 100 && !sys::poll_check(&ok2, &t2); i++) usleep(50000);
        unsetenv("FAKE_ARGS");
        std::string a = "\n" + sys::read_file(args);
        EXPECT(a.find("\n--work\n" + p.work + "\n") != std::string::npos);
        EXPECT(a.find("\nTMPDIR=" + sys::join(p.work, "tmp") + "\n") != std::string::npos);
    }

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
