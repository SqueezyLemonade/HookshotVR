// The launcher's one platform interface: everything that differs between Linux and Windows, and nothing else. The rest of
// the launcher (system.cpp, runtime.cpp, update.cpp, main.cpp) is the same on both and asks only this.
// platform_posix.cpp is Linux's; platform_win32.cpp is Windows' (build.sh builds the one for the system it runs on).
// Text is UTF-8 everywhere: the Windows half converts at its own edges (the repository's rule: no ANSI code page).
#pragma once
#include <stdint.h>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace plat {

double now_seconds();   // (monotonic)

// ---- folders and files ----
std::string home();
std::string config_dir();   // the launcher's settings: ~/.config/HookshotVR, %LOCALAPPDATA%\HookshotVR
std::string data_dir();     // its logs and downloads: ~/.local/share/HookshotVR, %LOCALAPPDATA%\HookshotVR
std::string exe_dir();      // the launcher's own folder
char sep();                 // '/' or '\\'
std::string env(const char *name);   // "" when unset
void set_env(const char *name, const std::string &value);   // (empty: unset)
bool is_exe(const std::string &p);
std::string real_path(const std::string &p);   // (p itself when it cannot be resolved)
double free_gb(const std::string &dir);        // -1 unknown

// ---- processes ----
// The variables a child gets on top of the launcher's own (nullopt: taken away)
using Env = std::map<std::string, std::optional<std::string>>;
enum class Out { Null, Pipe, File };
struct Proc {
    intptr_t pid = -1;   // (Windows: the process handle)
    intptr_t job = 0;    // (Windows: the job object that Cancel closes)
    intptr_t out = -1;   // (Out::Pipe: the read end, its stdout and stderr together)
    bool ok() const { return pid != -1; }
};
// Started in a group of its own (it outlives the launcher, and kill_tree stops what it started too). argv[0] without a
// folder is looked for on PATH. Out::File: stdout and stderr added to file's end
Proc spawn(const std::vector<std::string> &argv, const Env &env, const std::string &cwd, Out out, const std::string &file = "");
long read_out(Proc &p, char *buf, long n);   // <= 0 at the end
void close_out(Proc &p);
bool exited(Proc &p, int *code);   // (reaps it; true once it has ended, then p is done with)
int wait(Proc &p);
void kill_tree(Proc &p);
void forget(Proc &p);              // nobody asks how it ends (spawn_quiet's)
// A message the player sees without a terminal (Windows: a message box; elsewhere stderr only, as nothing else is sure to show)
void error_box(const char *title, const char *text);
bool process_running(const char *name);   // a process of that name, without .exe ("hookshot", "vrserver")
int process_count(const char *name);      // how many (two WiVRn servers: the launcher may ask the one without the headset)
bool on_path(const char *tool);
// The system's own copy of a tool the update runs ("curl", "tar"): Windows' System32\curl.exe and tar.exe (Windows 10 1803
// and later; tar.exe is bsdtar, which unpacks a .zip), never one found first on PATH or beside the launcher. Linux: the one
// on PATH. "" when there is none
std::string system_tool(const char *name);

// ---- the release ----
const char *launcher_file();    // the launcher's own file name ("hookshotvr-launcher", "hookshotvr-launcher.exe")
const char *release_suffix();   // the end of this platform's release archive's name ("-linux-x86_64.tar.gz", "-windows-x64.zip")
// The command that unpacks such an archive into dir (which exists)
std::vector<std::string> unpack_cmd(const std::string &archive, const std::string &dir);

// ---- the desktop ----
// The system's own file or folder dialog. Blocks: system.cpp calls it on a thread. "" when cancelled or there is none
std::string pick(bool folder, const std::string &title, const std::string &start, const std::string &filter_name,
                 const std::vector<std::string> &patterns);
void open(const std::string &path_or_url);   // the file manager or the browser
// The player's Start With app: an executable, a desktop entry (.desktop on Linux) or a shortcut (.lnk on Windows), or on
// Linux a command line typed into launcher.ini ("flatpak run io.github.wivrn.wivrn"). Not waited for, never stopped
bool start_app(const std::string &app);
// The launcher's own entry in the desktop's app menu and on the desktop (--install): Linux's .desktop files, Terminal=false,
// exe started as it is (no script, no terminal). Each file written goes in *written. A file there already that the launcher
// did not write is left alone (*err says so). Windows: shortcuts (.lnk) in the Start menu and on the desktop, the exe's own
// icon (icon is not used)
bool install_shortcut(const std::string &name, const std::string &exe, const std::string &icon, std::vector<std::string> *written,
                      std::string *err);

// ---- OpenXR ----
// The manifest the OpenXR loader picks with no XR_RUNTIME_JSON: Linux's active_runtime.json in the XDG folders and /etc,
// Windows' registry ActiveRuntime. "" when none is set
std::string active_runtime();
// Runtime manifests at their usual install paths (and, on Windows, the registry's AvailableRuntimes) that exist
std::vector<std::string> installed_runtimes();
void *open_openxr_loader();   // libopenxr_loader.so.1, openxr_loader.dll beside the launcher; null when missing
void *symbol(void *lib, const char *name);
// WiVRn's server on D-Bus (Linux): false when that cannot be asked here; else *running and *headset
bool wivrn_status(bool *running, bool *headset);
const char *wivrn_process();   // its server's process name ("" where there is none)
const char *steamvr_process(); // SteamVR's server ("vrserver")

// ---- the game ----
std::string game_bin(const std::string &game_dir);   // the executable a built folder holds ("" when none)
// The user folder (saves and hookshot_options.ini) the game uses whatever folder it runs from, so a new build keeps them.
// *pass: said to the game (--user), so its settings live there too and not beside the build
std::string game_user_dir(bool *pass);
// What the launcher ships for the game and puts beside its executable before each start (Windows: openxr_loader.dll, which
// Windows does not have and the game loads from its own folder; Linux: nothing, the system's loader is used). False, with
// *err, when one could not be put there
bool prepare_game(const std::string &game_bin, std::string *err);
// The builder's environment beside tools_env's common part: its toolchain inside the unpacked builder
Env builder_env(const std::string &builder);
std::string builder_python(const std::string &builder);

}  // namespace plat
