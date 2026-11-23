// What the launcher asks of the system: its settings file, the folders, the OpenXR runtime and the headset, Hookshot's
// builder and the game. The same on every platform (system.cpp, runtime.cpp); what differs is behind platform.h.
#pragma once
#include <stdint.h>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "platform.h"

namespace sys {

using plat::Env;
using plat::Proc;

// ---- the launcher's settings: launcher.ini in the config folder (~/.config/HookshotVR on Linux) ----
enum OnStart { kCloseLauncher = 0, kKeepLauncher = 1 };
enum Updates { kUpdatesAuto = 0, kUpdatesNotify = 1, kUpdatesOff = 2 };
// The VR runtime the game is given (XR_RUNTIME_JSON). System: the one the OpenXR loader picks itself
enum RuntimeKind { kRuntimeSystem = 0, kRuntimeWiVRn, kRuntimeSteamVR, kRuntimeMonado, kRuntimeCustom, kRuntimeKinds };
struct Config {
    std::string mode = "vr";        // vr | desktop, the last choice
    bool remember = true;
    int on_start = kCloseLauncher;
    int updates = kUpdatesNotify;
    int runtime = kRuntimeSystem;
    std::string runtime_json;       // Custom's manifest
    std::string start_with;         // an app started before the game in VR (the WiVRn dashboard, ALVR, SteamVR); empty: none
    std::string hookshot;           // Hookshot Folder: holds builder/ (an unpacked Hookshot builder) and the game it builds
    std::string hookshotvr;         // HookshotVR Folder: the HookshotVR source the game is built from
    std::string game_folder = "main-game";   // the build in the Hookshot Folder that Play starts (an update makes a new one)
    std::string logs;               // Logs: empty = the data folder's logs/
    std::string game_file;          // the .nup / .nx chosen at the first start
};
std::string config_path();
void load_config(Config &c);
void save_config(const Config &c);
const char *runtime_key(int kind);   // (the ini's word for it)

// ---- paths (UTF-8) ----
std::string home();
std::string data_dir();
std::string exe_dir();
std::string join(const std::string &a, const std::string &b);
std::string parent(const std::string &p);
std::string base_name(const std::string &p);
bool exists(const std::string &p);
bool is_dir(const std::string &p);
bool make_dirs(const std::string &p);
double free_gb(const std::string &p);   // at p, or its nearest folder that exists; -1 unknown
void open_folder(const std::string &p);
std::string pretty(const std::string &p);   // (the home folder as ~)
std::string read_file(const std::string &p);
bool copy_file(const std::string &from, const std::string &to);
bool remove_tree(const std::string &p);
// The game stamp: a hash of everything the build reads (scripts/hookshotvr_release.py game_stamp). The source's is its
// GAME_STAMP file (a release's; a checkout has none). A game folder's is the stamp of the source it was built from, written
// there after the build (.hookshotvr_stamp). "" when none
std::string source_stamp(const std::string &repo);
std::string game_stamp(const std::string &game);
// The folders a configuration names, and what is in them
struct Paths {
    std::string hookshot, builder, python, game, game_bin, repo, make_game, logs, crashes;
    std::string work;          // the translation the builds keep between them (main-work: every build's, an update's too)
    std::string cache;         // the build's XDG_CACHE_HOME: the shader caches it prepared
    std::string builder_tag;   // the builder's own version (builder/hookshot-linux.txt's "tag" line)
    bool builder_ok = false, repo_ok = false, game_built = false;
};
Paths paths(const Config &c);
std::string find_repo();       // the HookshotVR source beside the launcher or above it; empty when none
std::string find_hookshot();   // a Hookshot Folder beside the launcher (the release archive's builder/); empty when none

// ---- choosing a file or a folder: the desktop's own dialog, on a thread ----
// done(path) runs on the next poll_dialogs() after the dialog closes; an empty path is a cancel.
void choose_folder(const std::string &title, const std::string &start, std::function<void(std::string)> done);
void choose_file(const std::string &title, const std::string &start, const std::string &filter_name,
                 const std::vector<std::string> &patterns, std::function<void(std::string)> done);
void poll_dialogs();
bool dialog_open();

// ---- the OpenXR runtime (runtime.cpp) ----
struct Runtime {
    int chosen = kRuntimeSystem;
    bool found = false;
    std::string json;          // its manifest
    std::string name;          // its own name ("runtime": {"name": ...})
    std::string set_env;       // XR_RUNTIME_JSON for the game; empty: the loader's own choice (System)
    int is = kRuntimeCustom;   // what the manifest is: WiVRn, SteamVR, Monado, or another (Custom)
};
int runtime_kind_of(const std::string &json, const std::string &name);
std::string runtime_name(const std::string &manifest_text);
Runtime find_runtime(const Config &c);
// Every runtime found at its usual install path, with what it is
std::vector<Runtime> installed_runtimes();
std::optional<std::string> inherited_runtime_json();   // (XR_RUNTIME_JSON as the launcher was started; nullopt: unset)
enum class Check { Unknown, Ok, Waiting, Bad };
struct VrState {
    Runtime runtime;
    Check session = Check::Unknown;   // the runtime's service running
    Check headset = Check::Unknown;
    bool can_start = false;           // Start With is set, and its app is not running yet
    bool none_installed = false;      // (not found: no runtime at any usual install path either)
};
// What the main screen calls the runtime: WiVRn, SteamVR, Monado, else its manifest's own name
std::string runtime_label(const Runtime &r);
void vr_watch(bool on, const Config &c);   // checks every 2 s on a thread while on (c: the runtime and Start With)
VrState vr_state();
// The Start With app, when one is set: the player's own choice, never a runtime the launcher picks. Never stopped
bool start_with(const Config &c);
std::string app_name(const std::string &app);   // (a desktop entry's Name=, else the file's name without its extension)

// ---- Hookshot's builder: nx2/make_game.py, its @@ lines (make_game.py's docstring) ----
enum class Step { Check = 0, Translate, Compile, Shaders };
struct BuildState {
    bool running = false, finished = false, ok = false, cancelled = false;
    Step step = Step::Check;
    double fraction = 0;    // the step's @@progress
    double eta = -1;        // @@eta, seconds (the compile)
    double step_started = 0, started = 0;
    std::string error, folder;
};
// What one build makes, and from what: one of game_file (a first build from the player's .nup or .nx), from_game (a folder
// built before: its romfs and .hookshot_exefs, into a new folder) or update (a folder built again in place)
struct BuildJob {
    std::string repo;   // the HookshotVR source: make_game.py and Hookshot's tools (HOOKSHOT_TOOLS)
    std::string game_file, from_game, update;
    std::string out, work, log;
};
BuildJob first_build(const Paths &p, const std::string &game_file);
bool build_start(const Paths &p, const BuildJob &j, const std::string &fake_script);
void build_poll(BuildState &s);
void build_cancel();
// (a build that finished records its source's game stamp in the folder it built)
// The game file's check (make_game.py --check, in the Hookshot Folder's work folder): done(ok, text) from poll_check
void check_game_file(const Paths &p, const std::string &file, const std::string &fake_script);
bool poll_check(bool *ok, std::string *text);

// ---- the game ----
// The game's environment for the chosen mode (system.cpp, README.md's table)
Env game_env(const Paths &p, const Config &c, bool vr, const Runtime &r);
std::vector<std::string> game_args(const Paths &p);
// Starts the game in its folder, its output to <logs>/last-run.log (the run before kept as last-run.1.log)
bool launch_game(const Paths &p, const Config &c, bool vr, const Runtime &r, Proc *game, std::string *err);
bool game_exited(Proc &game, int *code);
// Crash records the game left (<game>/bug-reports/pending/report-*.txt), copied into <logs>/crashes
int collect_crashes(const Paths &p);

// A short command's output (git, for the update check), its exit code in *code
std::string run(const std::vector<std::string> &argv, const std::string &cwd, int *code);

double now_seconds();

}  // namespace sys
