// platform.h on Linux.
#include "platform.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <thread>

extern char **environ;

namespace plat {

static std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}
static bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

double now_seconds() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

std::string home() {
    const char *h = getenv("HOME");
    return h && *h ? h : "/tmp";
}
static std::string xdg(const char *var, const char *fallback) {
    const char *v = getenv(var);
    return v && *v == '/' ? std::string(v) : join(home(), fallback);
}
std::string config_dir() { return join(xdg("XDG_CONFIG_HOME", ".config"), "HookshotVR"); }
std::string data_dir() { return join(xdg("XDG_DATA_HOME", ".local/share"), "HookshotVR"); }
// (read once: an update renames the running launcher aside, and /proc/self/exe follows it there)
std::string exe_dir() {
    static const std::string dir = [] {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n <= 0) return std::string(".");
        buf[n] = 0;
        std::string s(buf);
        return s.substr(0, s.rfind('/'));
    }();
    return dir;
}
char sep() { return '/'; }
std::string env(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}
void set_env(const char *name, const std::string &value) {
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
}
bool is_exe(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
}
std::string real_path(const std::string &p) {
    char buf[4096];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}
double free_gb(const std::string &dir) {
    struct statvfs v;
    if (dir.empty() || statvfs(dir.c_str(), &v) != 0) return -1;
    return (double)v.f_bavail * v.f_frsize / 1e9;
}

// ---- processes ----
static std::vector<char *> env_block(const Env &over, std::vector<std::string> &store) {
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || over.count(std::string(*e, eq - *e))) continue;
        store.emplace_back(*e);
    }
    for (auto &kv : over)
        if (kv.second) store.push_back(kv.first + "=" + *kv.second);
    std::vector<char *> out;
    for (auto &s : store) out.push_back((char *)s.c_str());
    out.push_back(nullptr);
    return out;
}

// fork and exec, in a session of its own (it outlives the launcher, and Cancel stops its whole group)
Proc spawn(const std::vector<std::string> &argv, const Env &env, const std::string &cwd, Out out, const std::string &file) {
    Proc p;
    std::vector<std::string> store;
    std::vector<char *> envp = env_block(env, store);
    std::vector<char *> args;
    for (auto &a : argv) args.push_back((char *)a.c_str());
    args.push_back(nullptr);
    int fds[2] = {-1, -1};
    if (out == Out::Pipe && pipe(fds) != 0) return p;
    int file_fd = out == Out::File ? ::open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int in = ::open("/dev/null", O_RDONLY);
        if (in >= 0) dup2(in, 0);
        int o = out == Out::Pipe ? fds[1] : file_fd >= 0 ? file_fd : ::open("/dev/null", O_WRONLY);
        if (o >= 0) dup2(o, 1), dup2(o, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        environ = envp.data();   // (then execvp: argv[0] by PATH when it has no slash, as a shell would)
        execvp(args[0], args.data());
        _exit(127);
    }
    if (file_fd >= 0) close(file_fd);
    if (out == Out::Pipe) {
        close(fds[1]);
        if (pid < 0) close(fds[0]);
        else p.out = fds[0];
    }
    if (pid > 0) p.pid = pid;
    return p;
}

long read_out(Proc &p, char *buf, long n) {
    if (p.out < 0) return 0;
    ssize_t r;
    do r = read((int)p.out, buf, (size_t)n);
    while (r < 0 && errno == EINTR);
    return (long)r;
}
void close_out(Proc &p) {
    if (p.out >= 0) close((int)p.out);
    p.out = -1;
}
bool exited(Proc &p, int *code) {
    if (p.pid <= 0) return true;
    int st = 0;
    pid_t r = waitpid((pid_t)p.pid, &st, WNOHANG);
    if (r == 0) return false;
    if (code) *code = r == p.pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    p.pid = -1;
    return true;
}
int wait(Proc &p) {
    if (p.pid <= 0) return -1;
    int st = 0;
    waitpid((pid_t)p.pid, &st, 0);
    p.pid = -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
void kill_tree(Proc &p) {
    if (p.pid > 0) kill(-(pid_t)p.pid, SIGTERM);   // (its whole group: make_game.py and the compilers it started)
}
void forget(Proc &p) {
    // (a thread waits for it, so none is left a zombie)
    pid_t pid = (pid_t)p.pid;
    if (pid > 0) std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    p.pid = -1;
}

bool process_running(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[300], comm[64] = {0};
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            found = !strcmp(comm, name);
        }
        fclose(f);
        if (found) break;
    }
    closedir(d);
    return found;
}

void error_box(const char *title, const char *text) { fprintf(stderr, "%s: %s\n", title, text); }
int process_count(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[300], comm[64] = {0};
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            n += !strcmp(comm, name);
        }
        fclose(f);
    }
    closedir(d);
    return n;
}

bool on_path(const char *tool) {
    const char *path = getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':'))
        if (!dir.empty() && is_exe(join(dir, tool))) return true;
    return false;
}

std::string system_tool(const char *name) {
    const char *path = getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(ss, dir, ':'))
        if (!dir.empty() && dir[0] == '/' && is_exe(join(dir, name))) return join(dir, name);
    return "";
}

const char *launcher_file() { return "hookshotvr-launcher"; }
const char *release_suffix() { return "-linux-x86_64.tar.gz"; }
std::vector<std::string> unpack_cmd(const std::string &archive, const std::string &dir) {
    return {system_tool("tar"), "-xzf", archive, "-C", dir, "--no-same-owner"};
}

// The output of a short command, its exit code in *code
static std::string capture(const std::vector<std::string> &argv, int *code) {
    Proc p = spawn(argv, {}, "", Out::Pipe);
    std::string out;
    if (!p.ok()) {
        if (code) *code = -1;
        return out;
    }
    char buf[4096];
    long n;
    while ((n = read_out(p, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    close_out(p);
    int c = wait(p);
    if (code) *code = c;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// ---- the desktop: zenity or kdialog ----
std::string pick(bool folder, const std::string &title, const std::string &start, const std::string &filter_name,
                 const std::vector<std::string> &patterns) {
    std::string from = start.empty() ? home() : start, globs;
    for (auto &g : patterns) globs += (globs.empty() ? "" : " ") + g;
    std::vector<std::string> argv;
    if (on_path("zenity")) {
        argv = {"zenity", "--file-selection", "--title=" + title, "--filename=" + from + (folder && from.back() != '/' ? "/" : "")};
        if (folder) argv.push_back("--directory");
        else if (!globs.empty()) argv.insert(argv.end(), {"--file-filter=" + filter_name + " | " + globs, "--file-filter=All files | *"});
    } else if (on_path("kdialog")) {
        if (folder) argv = {"kdialog", "--title", title, "--getexistingdirectory", from};
        else argv = {"kdialog", "--title", title, "--getopenfilename", from, globs.empty() ? "*" : globs};
    } else {
        fprintf(stderr, "launcher: no file dialog (zenity or kdialog)\n");
        return "";
    }
    int code = 0;
    std::string r = capture(argv, &code);
    return code == 0 ? r : "";
}

void open(const std::string &what) {
    Proc p = spawn({"xdg-open", what}, {}, "", Out::Null);
    forget(p);
}

// (words the way a desktop entry's Exec= or a shell splits them: quotes kept together, %-codes left out)
static std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> out;
    std::string w;
    bool quoted = false, any = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') quoted = !quoted, any = true;
        else if (c == '\\' && quoted && i + 1 < line.size()) w += line[++i];
        else if ((c == ' ' || c == '\t') && !quoted) {
            if (any && !(w.size() == 2 && w[0] == '%')) out.push_back(w);
            w.clear(), any = false;
        } else w += c, any = true;
    }
    if (any && !(w.size() == 2 && w[0] == '%')) out.push_back(w);
    return out;
}

bool start_app(const std::string &app) {
    std::vector<std::string> argv;
    if (app.size() > 8 && app.compare(app.size() - 8, 8, ".desktop") == 0) {
        std::ifstream f(app);
        std::string line;
        bool entry = false;
        while (std::getline(f, line)) {
            if (!line.empty() && line[0] == '[') entry = line == "[Desktop Entry]";
            else if (entry && line.rfind("Exec=", 0) == 0) {
                argv = words(line.substr(5));
                break;
            }
        }
    } else if (is_exe(app)) {
        argv = {app};
    } else {
        argv = words(app);   // (a command typed into launcher.ini)
    }
    if (argv.empty()) return false;
    Proc p = spawn(argv, {}, "", Out::Null);
    bool ok = p.ok();
    forget(p);
    return ok;
}

// (a desktop entry's Exec= argument: quoted, with the characters the spec reserves inside quotes escaped, then the value's own
// escaping of the backslash; % as %%)
static std::string exec_arg(const std::string &a) {
    std::string q = "\"";
    for (char c : a) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') q += '\\';
        if (c == '%') q += '%';
        q += c;
    }
    q += '"';
    std::string out;
    for (char c : q) out += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return out;
}

bool install_shortcut(const std::string &name, const std::string &exe, const std::string &icon, std::vector<std::string> *written,
                      std::string *err) {
    static const char *kMark = "X-HookshotVR-Launcher=1";
    std::string entry = "[Desktop Entry]\nType=Application\nVersion=1.0\nName=" + name +
                        "\nComment=Build and play HookshotVR, in VR or on the desktop\nExec=" + exec_arg(exe) + "\nTryExec=" + exe +
                        "\nIcon=" + icon + "\nTerminal=false\nCategories=Game;\nStartupNotify=true\nStartupWMClass=" +
                        exe.substr(exe.rfind('/') + 1) + "\nPrefersNonDefaultGPU=true\nActions=PlayVR;PlayDesktop;\n" + kMark +
                        "\n\n[Desktop Action PlayVR]\nName=Play HookshotVR\nExec=" + exec_arg(exe) + " --play vr\n" +
                        "\n[Desktop Action PlayDesktop]\nName=Play Hookshot\nExec=" + exec_arg(exe) + " --play desktop\n";
    // (the app menu's, and the desktop's when there is one: xdg-user-dir's DESKTOP, else ~/Desktop)
    std::string apps = join(xdg("XDG_DATA_HOME", ".local/share"), "applications");
    int code = 0;
    std::string desk = on_path("xdg-user-dir") ? capture({"xdg-user-dir", "DESKTOP"}, &code) : "";
    if (desk.empty() || code != 0 || desk == home()) desk = join(home(), "Desktop");
    std::vector<std::string> files{join(apps, "hookshotvr.desktop")};
    struct stat st;
    if (stat(desk.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) files.push_back(join(desk, name + ".desktop"));
    mkdir(xdg("XDG_DATA_HOME", ".local/share").c_str(), 0755);
    mkdir(apps.c_str(), 0755);
    bool ok = true;
    for (auto &f : files) {
        if (exists(f)) {
            std::ifstream in(f);
            std::stringstream was;
            was << in.rdbuf();
            if (was.str().find(kMark) == std::string::npos) {   // (the player's own file of that name: kept)
                if (err) *err += (err->empty() ? "" : "; ") + f + " is not the launcher's: left as it is";
                ok = false;
                continue;
            }
        }
        std::string tmp = f + ".new";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << entry;
            if (!out) {
                if (err) *err += (err->empty() ? "" : "; ") + f + ": " + strerror(errno);
                ok = false;
                continue;
            }
        }
        chmod(tmp.c_str(), 0755);   // (a desktop's file managers start only an executable entry)
        if (rename(tmp.c_str(), f.c_str()) != 0) {
            unlink(tmp.c_str());
            ok = false;
            continue;
        }
        // (GNOME's and Cinnamon's desktops ask before the first start of an entry not marked trusted)
        if (f != files[0] && on_path("gio")) capture({"gio", "set", f, "metadata::trusted", "true"}, &code);
        if (written) written->push_back(f);
    }
    return ok;
}

// ---- OpenXR ----
std::string active_runtime() {
    // The loader's order on Linux after XR_RUNTIME_JSON: active_runtime.json in $XDG_CONFIG_HOME, $XDG_CONFIG_DIRS (default
    // /etc/xdg), then /etc
    std::vector<std::string> tries{join(xdg("XDG_CONFIG_HOME", ".config"), "openxr/1/active_runtime.json")};
    const char *dirs = getenv("XDG_CONFIG_DIRS");
    std::stringstream ss(dirs && *dirs ? dirs : "/etc/xdg");
    std::string d;
    while (std::getline(ss, d, ':'))
        if (!d.empty()) tries.push_back(join(d, "openxr/1/active_runtime.json"));
    tries.push_back("/etc/openxr/1/active_runtime.json");
    for (auto &t : tries)
        if (exists(t)) return t;
    return "";
}

std::vector<std::string> installed_runtimes() {
    std::string h = home();
    std::vector<std::string> out, tries{
        // WiVRn: its Flatpak (system and user), then a distribution's package
        "/var/lib/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json",
        join(h, ".local/share/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json"),
        // SteamVR: Steam's own folder, its older link, Steam's Flatpak
        join(h, ".local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json"),
        join(h, ".steam/steam/steamapps/common/SteamVR/steamxr_linux64.json"),
        join(h, ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json"),
    };
    // (the packages' own manifests: openxr_monado.json, openxr_wivrn.json and the like)
    for (const std::string &dir : {std::string("/usr/share/openxr/1"), std::string("/usr/local/share/openxr/1"),
                                   join(xdg("XDG_DATA_HOME", ".local/share"), "openxr/1")}) {
        DIR *d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0 && n != "active_runtime.json") tries.push_back(join(dir, n));
        }
        closedir(d);
    }
    for (auto &t : tries) {
        if (!exists(t)) continue;
        std::string r = real_path(t);
        bool seen = false;
        for (auto &o : out) seen = seen || real_path(o) == r;
        if (!seen) out.push_back(t);
    }
    return out;
}

void *open_openxr_loader() { return dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL); }
void *symbol(void *lib, const char *name) { return lib ? dlsym(lib, name) : nullptr; }

bool wivrn_status(bool *running, bool *headset) {
    static int has_gdbus = -1;
    if (has_gdbus < 0) has_gdbus = on_path("gdbus") ? 1 : 0;
    if (!has_gdbus) return false;
    int code = 0;
    std::string out = capture({"timeout", "2", "gdbus", "call", "--session", "--dest", "io.github.wivrn.Server", "--object-path",
                               "/io/github/wivrn/Server", "--method", "org.freedesktop.DBus.Properties.Get",
                               "io.github.wivrn.Server", "HeadsetConnected"},
                              &code);
    *running = code == 0;   // (the name missing: the server is not running)
    *headset = code == 0 && out.find("true") != std::string::npos;
    return true;
}
const char *wivrn_process() { return "wivrn-server"; }
const char *steamvr_process() { return "vrserver"; }

// ---- the game ----
std::string game_bin(const std::string &dir) {
    for (const char *b : {"bin/hookshot", "hookshot"})
        if (is_exe(join(dir, b))) return join(dir, b);
    return "";
}

std::string game_user_dir(bool *pass) {
    // (host/hos_main.c saves_dir: a game started with nothing keeps its saves in $XDG_DATA_HOME/hookshot already, but its
    // hookshot_options.ini beside the game (host/options_path.c), so an update into a new folder lost the Insert menu's
    // choices. Said as --user, the settings live there too (the game folder's carried over once), as on Windows)
    *pass = true;
    return join(xdg("XDG_DATA_HOME", ".local/share"), "hookshot");
}

// (vr/tools/build.sh's: the unpacked AppImage builder's toolchain. And its CPM packages (the Vulkan, SPIR-V and VMA headers):
// a source with no cpm/ of its own, as the release's, stopped at the game file's check with "Missing Linux build
// prerequisites: VMA header vk_mem_alloc.h". A developer's RX_CPM stays)
Env builder_env(const std::string &builder) {
    std::string lib = join(builder, "usr/lib/hookshot"), cpm = join(builder, "usr/share/hookshot/cpm");
    Env e{{"EMSDK", join(lib, "emsdk")},
          {"HOOKSHOT_SYSROOT", join(lib, "sysroot")},
          {"HOOKSHOT_FFMPEG", join(lib, "ffmpeg")},
          {"HOOKSHOT_SDL2", join(lib, "libSDL2-2.0.so.0")}};
    struct stat st;
    if (env("RX_CPM").empty() && stat(cpm.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) e["RX_CPM"] = cpm;
    return e;
}
std::string builder_python(const std::string &builder) { return join(builder, "usr/python/bin/python3"); }

bool prepare_game(const std::string &, std::string *) { return true; }   // (the system's libopenxr_loader.so.1)

}  // namespace plat
