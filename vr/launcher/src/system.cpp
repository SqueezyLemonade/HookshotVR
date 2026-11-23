// system.h's settings, folders, builder and game: the same on every platform (what differs is platform.h's).
#include "system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace sys {

// (UTF-8 in and out of std::filesystem: a plain std::string is the ANSI code page's on Windows)
static fs::path P(const std::string &s) { return fs::u8path(s); }
static std::string S(const fs::path &p) { return p.u8string(); }

double now_seconds() { return plat::now_seconds(); }

// ---- paths ----
std::string home() { return plat::home(); }
std::string data_dir() { return plat::data_dir(); }
std::string exe_dir() { return plat::exe_dir(); }
std::string config_path() { return join(plat::config_dir(), "launcher.ini"); }
std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char s = plat::sep();
    return a.back() == '/' || a.back() == s ? a + b : a + s + b;
}
std::string parent(const std::string &p) { return S(P(p).parent_path()); }
std::string base_name(const std::string &p) { return S(P(p).filename()); }
bool exists(const std::string &p) {
    std::error_code e;
    return !p.empty() && fs::exists(P(p), e);
}
bool is_dir(const std::string &p) {
    std::error_code e;
    return !p.empty() && fs::is_directory(P(p), e);
}
bool make_dirs(const std::string &p) {
    std::error_code e;
    if (p.empty() || is_dir(p)) return true;
    fs::create_directories(P(p), e);
    return is_dir(p);
}
double free_gb(const std::string &start) {
    fs::path p = P(start);
    while (!p.empty() && !is_dir(S(p)) && p != p.parent_path()) p = p.parent_path();
    return p.empty() ? -1 : plat::free_gb(S(p));
}
void open_folder(const std::string &p) {
    make_dirs(p);
    plat::open(p);
}
std::string pretty(const std::string &p) {
    std::string h = home();
    return !h.empty() && p.compare(0, h.size(), h) == 0 && (p.size() == h.size() || p[h.size()] == '/' || p[h.size()] == plat::sep())
               ? "~" + p.substr(h.size())
               : p;
}
std::string read_file(const std::string &p) {
    std::ifstream f(P(p), std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
bool copy_file(const std::string &from, const std::string &to) {
    std::error_code e;
    return fs::copy_file(P(from), P(to), fs::copy_options::overwrite_existing, e);
}
bool remove_tree(const std::string &p) {
    if (p.empty() || P(p) == P(p).root_path() || P(p) == P(home())) return false;
    std::error_code e;
    fs::remove_all(P(p), e);
    return !e;
}

static std::string first_line(const std::string &file) {
    std::string t = read_file(file);
    t = t.substr(0, t.find_first_of("\r\n"));
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}
std::string source_stamp(const std::string &repo) { return repo.empty() ? "" : first_line(join(repo, "GAME_STAMP")); }
std::string game_stamp(const std::string &game) { return game.empty() ? "" : first_line(join(game, ".hookshotvr_stamp")); }

static bool is_builder(const std::string &b) { return plat::is_exe(plat::builder_python(b)); }
static bool is_repo(const std::string &d) { return exists(join(d, "nx2/make_game.py")) && is_dir(join(d, "vr/launcher")); }

std::string find_repo() {
    // (the release archive's source beside the launcher; in a checkout, the launcher's own folder upwards: vr/launcher/build/)
    std::string here = exe_dir();
    for (const char *beside : {"HookshotVR", "source"})
        if (is_repo(join(here, beside))) return join(here, beside);
    std::string d = here;
    for (int i = 0; i < 6 && !d.empty(); i++) {
        if (is_repo(d)) return d;
        std::string up = parent(d);
        if (up == d) break;
        d = up;
    }
    return "";
}

std::string find_hookshot() {
    // (the release archive: builder/ beside the launcher, or beside the launcher's folder)
    std::string here = exe_dir();
    for (const std::string &d : {here, parent(here)})
        if (is_builder(join(d, "builder"))) return d;
    return "";
}

static std::string builder_tag(const std::string &builder) {
    // (the builder's parts list: its first line, "tag v2.1.0")
    for (const char *list : {"hookshot-linux.txt", "hookshot-windows.txt"}) {
        std::string t = read_file(join(builder, list));
        if (t.compare(0, 4, "tag ") == 0) return t.substr(4, t.find_first_of("\r\n") - 4);
    }
    return "";
}

Paths paths(const Config &c) {
    Paths p;
    p.hookshot = c.hookshot.empty() ? find_hookshot() : c.hookshot;
    p.repo = c.hookshotvr.empty() ? find_repo() : c.hookshotvr;
    p.builder = join(p.hookshot, "builder");
    p.python = plat::builder_python(p.builder);
    p.game = join(p.hookshot, c.game_folder.empty() ? "main-game" : c.game_folder);
    p.work = join(p.hookshot, "main-work");
    p.make_game = join(p.repo, "nx2/make_game.py");
    p.game_bin = p.hookshot.empty() ? "" : plat::game_bin(p.game);
    p.logs = c.logs.empty() ? join(data_dir(), "logs") : c.logs;
    p.crashes = join(p.logs, "crashes");
    std::string xdg_cache = plat::env("XDG_CACHE_HOME");
    p.cache = !xdg_cache.empty() ? xdg_cache : p.hookshot.empty() ? "" : join(p.hookshot, "cache");
    p.builder_ok = !p.hookshot.empty() && is_builder(p.builder);
    if (p.builder_ok) p.builder_tag = builder_tag(p.builder);
    p.repo_ok = !p.repo.empty() && exists(p.make_game);
    p.game_built = !p.game_bin.empty();
    return p;
}

// ---- settings ----
static const char *kRuntimeKeys[kRuntimeKinds] = {"system", "wivrn", "steamvr", "monado", "custom"};
const char *runtime_key(int kind) { return kind >= 0 && kind < kRuntimeKinds ? kRuntimeKeys[kind] : "system"; }

void load_config(Config &c) {
    std::ifstream f(P(config_path()));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "mode") c.mode = v == "desktop" ? "desktop" : "vr";
        else if (k == "remember") c.remember = v != "0";
        else if (k == "on_game_start") c.on_start = v == "keep" ? kKeepLauncher : kCloseLauncher;
        else if (k == "updates") c.updates = v == "auto" ? kUpdatesAuto : v == "off" ? kUpdatesOff : kUpdatesNotify;
        else if (k == "vr_runtime") {
            c.runtime = kRuntimeSystem;
            for (int i = 0; i < kRuntimeKinds; i++)
                if (v == kRuntimeKeys[i]) c.runtime = i;
        }
        else if (k == "vr_runtime_json") c.runtime_json = v;
        else if (k == "start_with") c.start_with = v;
        else if (k == "hookshot_folder") c.hookshot = v;
        else if (k == "hookshotvr_folder") c.hookshotvr = v;
        else if (k == "game_folder") c.game_folder = v.empty() ? "main-game" : v;
        else if (k == "logs_folder") c.logs = v;
        else if (k == "game_file") c.game_file = v;
    }
}
void save_config(const Config &c) {
    std::string path = config_path();
    make_dirs(parent(path));
    std::string tmp = path + ".new";
    {
        std::ofstream f(P(tmp));
        f << "# HookshotVR Launcher\n"
          << "mode=" << c.mode << "\n"
          << "remember=" << (c.remember ? 1 : 0) << "\n"
          << "on_game_start=" << (c.on_start == kKeepLauncher ? "keep" : "close") << "\n"
          << "updates=" << (c.updates == kUpdatesAuto ? "auto" : c.updates == kUpdatesOff ? "off" : "notify") << "\n"
          << "vr_runtime=" << runtime_key(c.runtime) << "\n"
          << "vr_runtime_json=" << c.runtime_json << "\n"
          << "start_with=" << c.start_with << "\n"
          << "hookshot_folder=" << c.hookshot << "\n"
          << "hookshotvr_folder=" << c.hookshotvr << "\n"
          << "game_folder=" << c.game_folder << "\n"
          << "logs_folder=" << c.logs << "\n"
          << "game_file=" << c.game_file << "\n";
        if (!f) return;
    }
    std::error_code e;
    fs::rename(P(tmp), P(path), e);   // (whole or not at all: a kill mid-write leaves the old file)
}

// ---- processes ----
static void read_lines(Proc p, std::function<void(const std::string &)> each) {
    std::string pending;
    char buf[8192];
    long n;
    while ((n = plat::read_out(p, buf, sizeof buf)) > 0) {
        pending.append(buf, (size_t)n);
        size_t nl;
        while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
            if (nl) each(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) each(pending);
    plat::close_out(p);
}

std::string run(const std::vector<std::string> &argv, const std::string &cwd, int *code) {
    Proc p = plat::spawn(argv, {}, cwd, plat::Out::Pipe);
    std::string out;
    if (!p.ok()) {
        if (code) *code = -1;
        return out;
    }
    char buf[4096];
    long n;
    while ((n = plat::read_out(p, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    plat::close_out(p);
    int c = plat::wait(p);
    if (code) *code = c;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// ---- dialogs: plat::pick on a thread ----
namespace {
std::mutex dlg_mu;
std::function<void(std::string)> dlg_done;
std::string dlg_result;
std::atomic<bool> dlg_busy{false}, dlg_ready{false};

void dialog(bool folder, const std::string &title, const std::string &start, const std::string &filter_name,
            const std::vector<std::string> &patterns, std::function<void(std::string)> done) {
    if (dlg_busy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> l(dlg_mu);
        dlg_done = std::move(done);
    }
    std::thread([=] {
        std::string r = plat::pick(folder, title, start, filter_name, patterns);
        std::lock_guard<std::mutex> l(dlg_mu);
        dlg_result = r;
        dlg_ready = true;
    }).detach();
}
}  // namespace

void choose_folder(const std::string &title, const std::string &start, std::function<void(std::string)> done) {
    dialog(true, title, start, "", {}, std::move(done));
}
void choose_file(const std::string &title, const std::string &start, const std::string &filter_name,
                 const std::vector<std::string> &patterns, std::function<void(std::string)> done) {
    dialog(false, title, start, filter_name, patterns, std::move(done));
}
bool dialog_open() { return dlg_busy; }
void poll_dialogs() {
    if (!dlg_ready) return;
    std::function<void(std::string)> done;
    std::string r;
    {
        std::lock_guard<std::mutex> l(dlg_mu);
        done.swap(dlg_done);
        r = dlg_result;
        dlg_ready = false;
        dlg_busy = false;
    }
    if (done) done(r);
}

// ---- the builder ----
namespace {
std::mutex b_mu;
std::deque<std::string> b_lines;
Proc b_proc;   // (under b_mu)
std::string b_stamp;   // (under b_mu: the source's game stamp, for the folder once built)
std::atomic<bool> b_eof{false}, b_cancel{false};

std::mutex c_mu;
bool c_done = false, c_ok = false;
std::string c_text;
}  // namespace

// The builder's environment, as vr/tools/build.sh gives it: the toolchain of the unpacked Hookshot builder and the HookshotVR
// source as Hookshot's tools. Not build.sh's HOOKSHOT_KEEP_BUILD_FILES: a player's build lets the translated code go once the
// game is built (1.5 GB) and keeps its compiled objects, so an update compiles only what changed (make_game.py)
static Env tools_env(const Paths &p, const std::string &repo) {
    Env e = plat::builder_env(p.builder);
    e["HOOKSHOT_TOOLS"] = repo;
    e["HOOKSHOT_NO_UPDATE_CHECK"] = "1";
    if (!p.cache.empty()) e["XDG_CACHE_HOME"] = p.cache;
    e["HOOKSHOT_VK_PREBUILT"] = std::nullopt;
    e["PYTHONUNBUFFERED"] = "1";
    // (temporary files in the work folder, on the Hookshot Folder's drive: not the system drive's %TEMP%, nor a /tmp in RAM)
    if (!p.work.empty() && make_dirs(join(p.work, "tmp")))
        for (const char *t : {"TMPDIR", "TEMP", "TMP"}) e[t] = join(p.work, "tmp");
    return e;
}

static std::vector<std::string> make_game_cmd(const Paths &p, const std::string &repo, const std::string &fake) {
    if (!fake.empty()) return {"python3", "-I", fake};
    return {p.python, "-I", join(repo, "nx2/make_game.py")};
}

BuildJob first_build(const Paths &p, const std::string &game_file) {
    BuildJob j;
    j.repo = p.repo, j.game_file = game_file, j.out = p.game, j.work = p.work, j.log = join(p.logs, "build.log");
    return j;
}

bool build_start(const Paths &p, const BuildJob &j, const std::string &fake) {
    {
        std::lock_guard<std::mutex> l(b_mu);
        if (b_proc.ok()) return false;
    }
    std::vector<std::string> argv = make_game_cmd(p, j.repo, fake);
    if (!j.update.empty()) argv.insert(argv.end(), {"--update", j.update});
    else if (!j.from_game.empty())
        argv.insert(argv.end(), {"--romfs", join(j.from_game, "romfs"), "--exefs", join(j.from_game, ".hookshot_exefs"), "--out", j.out});
    else argv.insert(argv.end(), {"--game", j.game_file, "--out", j.out});
    if (!j.log.empty()) make_dirs(parent(j.log));
    argv.insert(argv.end(), {"--work", j.work});
    if (!j.log.empty()) argv.insert(argv.end(), {"--log", j.log});
    Proc pr = plat::spawn(argv, tools_env(p, j.repo), j.repo, plat::Out::Pipe);
    if (!pr.ok()) return false;
    {
        std::lock_guard<std::mutex> l(b_mu);
        b_lines.clear();
        b_lines.push_back("@@stage read");
        b_proc = pr;
        b_stamp = source_stamp(j.repo);
    }
    b_eof = false, b_cancel = false;
    std::thread([pr] {
        read_lines(pr, [](const std::string &line) {
            std::lock_guard<std::mutex> l(b_mu);
            b_lines.push_back(line);
        });
        b_eof = true;
    }).detach();
    return true;
}

static Step step_of(const std::string &stage) {
    if (stage == "lift") return Step::Translate;
    if (stage == "compile" || stage == "finish") return Step::Compile;
    if (stage == "shaders") return Step::Shaders;
    return Step::Check;
}

void build_poll(BuildState &s) {
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> l(b_mu);
        lines.swap(b_lines);
    }
    for (auto &line : lines) {
        if (!line.compare(0, 8, "@@stage ")) {
            size_t sp = line.find(' ', 8);
            Step st = step_of(line.substr(8, sp == std::string::npos ? std::string::npos : sp - 8));
            if (st != s.step || s.step_started == 0) s.step_started = now_seconds();
            s.step = st, s.fraction = 0, s.eta = -1;
        } else if (!line.compare(0, 11, "@@progress ")) {
            s.fraction = atof(line.c_str() + 11);
        } else if (!line.compare(0, 6, "@@eta ")) {
            s.eta = atof(line.c_str() + 6);
        } else if (!line.compare(0, 7, "@@done ")) {
            s.folder = line.substr(7);
        } else if (!line.compare(0, 8, "@@error ")) {
            s.error = line.substr(8);
        }
    }
    if (!b_eof) return;
    std::lock_guard<std::mutex> l(b_mu);
    int code = -1;
    if (b_proc.ok() && plat::exited(b_proc, &code)) {
        b_proc = Proc();
        s.running = false, s.finished = true;
        s.cancelled = b_cancel;
        s.ok = code == 0 && !s.folder.empty() && !b_cancel;
        if (s.ok) s.fraction = 1;
        // (what the game was built from, so an update knows whether it must be built again: update.h)
        std::string stamp = join(s.folder, ".hookshotvr_stamp");
        if (s.ok && !b_stamp.empty()) {
            std::ofstream f(P(stamp));
            f << b_stamp << "\n";
        } else if (s.ok) {
            std::error_code e;
            fs::remove(P(stamp), e);   // (a checkout's source: nothing known)
        }
    }
}

void build_cancel() {
    std::lock_guard<std::mutex> l(b_mu);
    if (!b_proc.ok()) return;
    b_cancel = true;
    plat::kill_tree(b_proc);
}

void check_game_file(const Paths &p, const std::string &file, const std::string &fake) {
    {
        std::lock_guard<std::mutex> l(c_mu);
        c_done = false;
    }
    std::vector<std::string> argv = make_game_cmd(p, p.repo, fake);
    // (--work: make_game.py's default is the system drive's (%LOCALAPPDATA%\\HookshotBuilder on Windows), where the check
    // unpacks the game's code to look at it; the Hookshot Folder's work folder keeps it on the drive the game is built on)
    argv.insert(argv.end(), {"--check", "--game", file, "--work", p.work});
    Env env = tools_env(p, p.repo);
    std::string cwd = p.repo;
    std::thread([argv, env, cwd] {
        Proc pr = plat::spawn(argv, env, cwd, plat::Out::Pipe);
        bool ok = false;
        std::string text;
        if (pr.ok()) {
            read_lines(pr, [&](const std::string &line) {
                if (!line.compare(0, 11, "@@check ok ")) ok = true, text = line.substr(11);
                else if (!line.compare(0, 12, "@@check bad ")) ok = false, text = line.substr(12);
                else if (!line.compare(0, 8, "@@error ") && text.empty()) text = line.substr(8);
            });
            plat::wait(pr);
        }
        std::lock_guard<std::mutex> l(c_mu);
        c_done = true, c_ok = ok, c_text = text;
    }).detach();
}

bool poll_check(bool *ok, std::string *text) {
    std::lock_guard<std::mutex> l(c_mu);
    if (!c_done) return false;
    c_done = false;
    *ok = c_ok, *text = c_text;
    return true;
}

// ---- the game ----
static bool has_line(const std::string &file, const char *start) {
    std::ifstream f(P(file));
    std::string line;
    while (std::getline(f, line))
        if (line.rfind(start, 0) == 0) return true;
    return false;
}

Env game_env(const Paths &p, const Config &c, bool vr, const Runtime &r) {
    // What the game needs from its launcher, the reasons kept short. No VRAM budget (RX_VK_VRAM_MB): every card gets
    // Hookshot's own
    Env e{
        {"HOOKSHOT_NO_UPDATE_CHECK", "1"},             // (Hookshot's own update would replace this VR build with the plain one)
        {"HOS_INPUT_BACKGROUND", "1"},                 // (controllers keep working while another window has the focus)
        {"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"},
        {"RX_VK_PRECOMPILE", "0"},                     // (the build prepared the shaders: the disk caches, not again)
        // (said either way: the game reads it before the ini's vr= (vr/headset/runtime_loader.inc), so Hookshot's Desktop
        // Mode is never turned into VR by an inherited HOOKSHOT_VR or the ini)
        {"HOOKSHOT_VR", vr ? "1" : "0"},
    };
    if (vr) {
        // (a ceiling, not the headset's rate: the game's own frame limiter (60) is lifted to 120 so it never holds the game
        // below the headset, and the runtime paces the frames (xrWaitFrame) at whatever rate the headset runs. A 144 Hz
        // headset is held to 120)
        e["HOS_FPS_TARGET"] = "120";
        e["RX_VK_IMMEDIATE"] = "1";    // (the desktop window without vsync: fifo held the game to a 60 Hz monitor)
        // (the VR Runtime setting. System: as the launcher was started, so the loader picks its own; the VR checks set the
        // launcher's for their probe)
        e["XR_RUNTIME_JSON"] = r.set_env.empty() ? inherited_runtime_json() : std::optional<std::string>(r.set_env);
    }
    // (the shader caches the build prepared its pipelines into, under its XDG_CACHE_HOME, not the user's: NVIDIA's and Mesa's)
    if (!p.cache.empty() && is_dir(join(p.cache, "nvidia"))) {
        e["__GL_SHADER_DISK_CACHE_PATH"] = join(p.cache, "nvidia");
        e["__GL_SHADER_DISK_CACHE_SKIP_CLEANUP"] = "1";
    }
    if (!p.cache.empty() && (is_dir(join(p.cache, "mesa_shader_cache")) || is_dir(join(p.cache, "mesa_shader_cache_db"))))
        e["MESA_SHADER_CACHE_DIR"] = p.cache;
    // (the VR tab's GPU profile row: the GPU's passes timed this run. The settings file is in the user folder, or still
    // beside the game before its first start)
    bool pass = false;
    std::string user = plat::game_user_dir(&pass);
    if (has_line(join(user, "hookshot_options.ini"), "gpu_profile=1") || has_line(join(p.game, "hookshot_options.ini"), "gpu_profile=1"))
        e["RX_VK_PASS_TIMING"] = "1";
    return e;
}

std::vector<std::string> game_args(const Paths &) {
    bool pass = false;
    std::string user = plat::game_user_dir(&pass);
    if (pass) return {"--user", user};   // (the saves and settings outside the game folder, so a new build keeps them)
    return {};
}

bool launch_game(const Paths &p, const Config &c, bool vr, const Runtime &r, Proc *game, std::string *err) {
    if (!p.game_built) {
        if (err) *err = "Not built";
        return false;
    }
    make_dirs(p.logs);
    std::string log = join(p.logs, "last-run.log");
    std::error_code ec;
    if (exists(log)) fs::rename(P(log), P(join(p.logs, "last-run.1.log")), ec);
    if (!plat::prepare_game(p.game_bin, err)) return false;
    std::vector<std::string> argv{p.game_bin};
    for (auto &a : game_args(p)) argv.push_back(a);
    Proc pr = plat::spawn(argv, game_env(p, c, vr, r), p.game, plat::Out::File, log);
    if (!pr.ok()) {
        if (err) *err = strerror(errno);
        return false;
    }
    if (game) *game = pr;
    return true;
}

bool game_exited(Proc &game, int *code) { return plat::exited(game, code); }

std::string app_name(const std::string &app) {
    std::string name = base_name(app);
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".desktop") == 0) {
        std::stringstream f(read_file(app));
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("Name=", 0) == 0) return line.substr(5);
    }
    size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

int collect_crashes(const Paths &p) {
    std::error_code e;
    int n = 0;
    for (auto &f : fs::directory_iterator(P(join(p.game, "bug-reports/pending")), e)) {
        std::string name = S(f.path().filename());
        if (name.rfind("report-", 0) != 0 || name.size() < 12 || name.compare(name.size() - 4, 4, ".txt")) continue;
        std::string to = join(p.crashes, name);
        if (exists(to)) continue;
        make_dirs(p.crashes);
        if (copy_file(S(f.path()), to)) n++;
    }
    return n;
}

}  // namespace sys
