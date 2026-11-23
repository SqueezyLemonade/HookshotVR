// update.h: the check and the one-click update. Untested against a real release (there is none yet); the self-test runs it
// through with a stand-in builder and a source already in place (tools/selftest.cpp).
#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace upd {

const char *const kVersion = "v1.0.0";
const char *const kVersionName = "Alpha v1.0.0";
// The public release repo (tags vX.Y.Z); never a URL with a token in it
const char *const kReleaseRepo = "https://git.notrexed.dev/SqueezyLemonade/HookshotVR.git";
const char *const kReleasePage = "https://git.notrexed.dev/SqueezyLemonade/HookshotVR/releases";
static constexpr double kNeededGB = 60;   // (a second game folder beside the one played, while it builds)

bool newer(const std::string &a, const std::string &b) {
    int x[3] = {0}, y[3] = {0};
    sscanf(a.c_str(), "v%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b.c_str(), "v%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

std::vector<std::string> changes_of(const std::string &text, const std::string &version) {
    std::vector<std::string> out;
    bool in = false;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 1 && line[0] == 'v' && line[1] >= '0' && line[1] <= '9') in = line == version;
        else if (in && !line.empty()) out.push_back(line);
    }
    return out;
}

std::string source_folder(const std::string &version) { return sys::join(sys::join(sys::join(sys::data_dir(), "versions"), version), "HookshotVR"); }
std::string game_folder_name(const std::string &version) { return version + "-game"; }

// git never asks for a login: a private, moved or unreachable repo just fails the check quietly
static void git_quiet() {
#ifdef _WIN32
    _putenv_s("GIT_TERMINAL_PROMPT", "0");
    _putenv_s("GCM_INTERACTIVE", "never");
#else
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
    setenv("GIT_ASKPASS", "", 1);
    setenv("SSH_ASKPASS", "", 1);
#endif
}

// The release's source into dir. For now a shallow clone of its tag (git on PATH); the release archive over HTTPS, checked
// by its hash, with the launcher and the pinned builder in it, is the next step
static bool download(const std::string &version, const std::string &dir, std::string *err) {
    if (!*kReleaseRepo) return *err = "No release", false;
    if (!plat::on_path("git")) return *err = "No git", false;
    sys::remove_tree(dir);
    sys::make_dirs(sys::parent(dir));
    git_quiet();
    int code = 0;
    sys::run({"git", "clone", "--quiet", "--depth", "1", "--no-tags", "--branch", version, kReleaseRepo, dir}, "", &code);
    if (code != 0 || !sys::exists(sys::join(dir, "nx2/make_game.py"))) return *err = "Download failed", false;
    return true;
}

// ---- the check ----
namespace {
std::mutex mu;
bool found = false;
Release best;
}  // namespace

void check(const sys::Config &c) {
    if (!*kReleaseRepo || c.updates == sys::kUpdatesOff) return;
    bool fetch = c.updates == sys::kUpdatesAuto;
    git_quiet();
    std::thread([fetch] {
        int code = 0;
        std::string tags = sys::run({"git", "ls-remote", "--tags", "--refs", kReleaseRepo}, "", &code);
        if (code != 0) return;
        Release r;
        size_t at = 0;
        while ((at = tags.find("refs/tags/v", at)) != std::string::npos) {
            size_t end = tags.find_first_of("\r\n", at);
            std::string tag = tags.substr(at + 10, end == std::string::npos ? std::string::npos : end - at - 10);
            if (newer(tag, r.version.empty() ? kVersion : r.version)) r.version = tag;
            at += 10;
        }
        if (r.version.empty()) return;
        // (Automatically Download: the release fetched now, its changelog read from it; the update then only builds)
        std::string dir = source_folder(r.version), err;
        if (fetch && (sys::exists(sys::join(dir, "nx2/make_game.py")) || download(r.version, dir, &err))) {
            r.source = dir;
            r.changes = changes_of(sys::read_file(sys::join(dir, "vr/launcher/CHANGELOG.txt")), r.version);
        }
        std::lock_guard<std::mutex> l(mu);
        found = true, best = r;
    }).detach();
}

bool available(Release *r) {
    std::lock_guard<std::mutex> l(mu);
    if (found && r) *r = best;
    return found;
}

// ---- the update ----
namespace {
Progress prog;
sys::Paths paths;
std::string fake, new_source, new_game, old_game, old_source;
std::atomic<bool> busy{false}, thread_done{false}, thread_ok{false}, cancelled{false};
std::string thread_err;   // (under mu)
}  // namespace

bool running() { return busy; }

static void fail(const std::string &why) {
    prog.stage = Stage::Failed, prog.error = why;
    // (no mess: the half-made folder goes; the build the player plays was never touched)
    if (!new_game.empty() && new_game != old_game) sys::remove_tree(new_game);
    if (!new_source.empty() && new_source != old_source && sys::parent(sys::parent(new_source)) == sys::join(sys::data_dir(), "versions"))
        sys::remove_tree(sys::parent(new_source));
}

bool start(const sys::Paths &p, const sys::Config &c, const Release &r, const std::string &fake_build) {
    if (busy || r.version.empty() || !p.builder_ok || p.hookshot.empty()) return false;
    double gb = sys::free_gb(p.hookshot);
    if (gb >= 0 && gb < kNeededGB) {
        prog = Progress{Stage::Failed, r.version, "Not enough space"};
        busy = true;   // (tick says it once)
        return true;
    }
    busy = true, cancelled = false;
    paths = p, fake = fake_build;
    prog = Progress{Stage::Download, r.version, ""};
    old_game = p.game, old_source = p.repo;
    new_game = sys::join(p.hookshot, game_folder_name(r.version));
    new_source = r.source.empty() ? source_folder(r.version) : r.source;
    (void)c;
    thread_done = false;
    if (!r.source.empty()) {
        thread_ok = true, thread_done = true;   // (downloaded already)
        return true;
    }
    std::string version = r.version, dir = new_source;
    std::thread([version, dir] {
        std::string err;
        bool ok = download(version, dir, &err);
        {
            std::lock_guard<std::mutex> l(mu);
            thread_err = err;
        }
        thread_ok = ok, thread_done = true;
    }).detach();
    return true;
}

void cancel() {
    if (!busy) return;
    cancelled = true;
    if (prog.stage == Stage::Build) sys::build_cancel();
}

// The new folder holds a whole game: the builder said it finished there, and the game and its files are in it
static bool build_passed(const sys::BuildState &b) {
    return b.ok && plat::real_path(b.folder) == plat::real_path(new_game) && !plat::game_bin(new_game).empty() &&
           sys::is_dir(sys::join(new_game, "romfs"));
}

Progress tick(sys::Config &cfg, sys::BuildState &build) {
    if (!busy) return Progress();
    switch (prog.stage) {
        case Stage::Download:
            if (!thread_done) break;
            if (cancelled) fail("Cancelled");
            else if (!thread_ok) {
                std::lock_guard<std::mutex> l(mu);
                fail(thread_err);
            } else {
                // The game from the new source into its own folder: from the player's game file when it is still there, else
                // from the game files the old folder keeps (its romfs and .hookshot_exefs). The translation is the builds' one
                sys::BuildJob j;
                j.repo = new_source, j.out = new_game, j.work = paths.work;
                j.log = sys::join(paths.logs, "build-" + prog.version + ".log");
                if (!cfg.game_file.empty() && sys::exists(cfg.game_file)) j.game_file = cfg.game_file;
                else j.from_game = old_game;
                sys::remove_tree(new_game);   // (what an update stopped before left)
                build = sys::BuildState();
                build.running = true, build.started = build.step_started = sys::now_seconds();
                if (sys::build_start(paths, j, fake)) prog.stage = Stage::Build;
                else fail("Build failed");
            }
            break;
        case Stage::Build:
            sys::build_poll(build);
            if (!build.finished) break;
            if (build.cancelled || cancelled) fail("Cancelled");
            else if (!build_passed(build)) fail(build.error.empty() ? "Build failed" : build.error);
            else prog.stage = Stage::Switch;
            break;
        case Stage::Switch: {
            // The player's own files in the old folder go along (hookshot_env.txt); crash reports are saved first
            sys::collect_crashes(paths);
            for (const char *own : {"hookshot_env.txt"})
                if (sys::exists(sys::join(old_game, own)) && !sys::exists(sys::join(new_game, own)))
                    sys::copy_file(sys::join(old_game, own), sys::join(new_game, own));
            cfg.game_folder = game_folder_name(prog.version);
            cfg.hookshotvr = new_source;
            sys::save_config(cfg);   // (the switch: one whole write of launcher.ini)
            prog.stage = Stage::Clean;
            // The old build goes: its folder in the Hookshot Folder, and its source when the launcher downloaded it. Never
            // a folder the player chose themself
            std::string game = old_game, source = old_source, versions = sys::join(sys::data_dir(), "versions");
            std::string hookshot = paths.hookshot;
            thread_done = false;
            std::thread([game, source, versions, hookshot] {
                if (sys::parent(game) == hookshot && game != new_game) sys::remove_tree(game);
                if (!source.empty() && source != new_source && sys::parent(sys::parent(source)) == versions) sys::remove_tree(sys::parent(source));
                thread_done = true;
            }).detach();
            break;
        }
        case Stage::Clean:
            if (thread_done) prog.stage = Stage::Done;
            break;
        default:
            break;
    }
    Progress out = prog;
    if (prog.stage == Stage::Done || prog.stage == Stage::Failed) {
        busy = false;   // (said once)
        prog = Progress();
        std::lock_guard<std::mutex> l(mu);
        if (out.stage == Stage::Done) found = false;
    }
    return out;
}

void tidy(const sys::Paths &p, const sys::Config &c) {
    if (busy) return;
    namespace fs = std::filesystem;
    std::error_code e;
    // (only the launcher's own names: <data>/versions/<v>, and <v>-game in the Hookshot Folder; never main-game)
    std::string versions = sys::join(sys::data_dir(), "versions");
    for (auto &d : fs::directory_iterator(fs::u8path(versions), e)) {
        std::string dir = d.path().u8string();
        if (sys::join(dir, "HookshotVR") != p.repo) sys::remove_tree(dir);
    }
    if (p.hookshot.empty()) return;
    for (auto &d : fs::directory_iterator(fs::u8path(p.hookshot), e)) {
        std::string name = d.path().filename().u8string();
        if (name.size() > 6 && name[0] == 'v' && name[1] >= '0' && name[1] <= '9' && name.compare(name.size() - 5, 5, "-game") == 0 &&
            name != c.game_folder && d.is_directory(e))
            sys::remove_tree(d.path().u8string());
    }
}

}  // namespace upd
