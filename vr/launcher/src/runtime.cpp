// The VR Runtime setting and the VR checks (system.h): which OpenXR runtime the game is given, whether its service runs,
// and whether a headset is there. The launcher never starts a runtime by itself: only the player's Start With app, and only
// when they ask (Start, or Play with Start With set).
#include <ctype.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "openxr/openxr.h"
#include "system.h"

namespace sys {

// (the launcher's own XR_RUNTIME_JSON as it was started: the VR checks set it for their probe, and System gives the game
// this one back, so the loader picks as it would have)
static const std::string kInheritedJson = plat::env("XR_RUNTIME_JSON");
std::optional<std::string> inherited_runtime_json() {
    if (kInheritedJson.empty()) return std::nullopt;
    return kInheritedJson;
}

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ("runtime": { ... "name": "<this>" ... }): enough JSON for a runtime manifest
std::string runtime_name(const std::string &json) {
    size_t at = json.find("\"runtime\"");
    if (at == std::string::npos) return "";
    at = json.find("\"name\"", at);
    if (at == std::string::npos) return "";
    at = json.find(':', at);
    size_t q0 = at == std::string::npos ? at : json.find('"', at);
    size_t q1 = q0 == std::string::npos ? q0 : json.find('"', q0 + 1);
    return q1 == std::string::npos ? "" : json.substr(q0 + 1, q1 - q0 - 1);
}

int runtime_kind_of(const std::string &json, const std::string &name) {
    // (by its own name, then by where it is: a link like active_runtime.json is looked at where it points)
    std::string n = lower(name), at = lower(json) + " " + lower(plat::real_path(json));
    if (n.find("wivrn") != std::string::npos || at.find("wivrn") != std::string::npos) return kRuntimeWiVRn;
    if (n.find("steamvr") != std::string::npos || at.find("steamxr") != std::string::npos || at.find("steamvr") != std::string::npos)
        return kRuntimeSteamVR;
    if (n.find("monado") != std::string::npos || at.find("monado") != std::string::npos) return kRuntimeMonado;
    return kRuntimeCustom;
}

static Runtime describe(const std::string &json) {
    Runtime r;
    if (json.empty() || !exists(json)) return r;
    r.found = true, r.json = json;
    r.name = runtime_name(read_file(json));
    if (r.name.empty()) r.name = "OpenXR";
    r.is = runtime_kind_of(json, r.name);
    return r;
}

std::vector<Runtime> installed_runtimes() {
    std::vector<Runtime> out;
    for (auto &j : plat::installed_runtimes()) out.push_back(describe(j));
    return out;
}

Runtime find_runtime(const Config &c) {
    Runtime r;
    if (c.runtime == kRuntimeSystem) {
        // The loader's own choice: XR_RUNTIME_JSON as the launcher was started, then the system's active runtime
        r = describe(!kInheritedJson.empty() ? kInheritedJson : plat::active_runtime());
    } else if (c.runtime == kRuntimeCustom) {
        r = describe(c.runtime_json);
        r.set_env = r.json;
    } else {
        // That runtime: the active one when it is that kind (the one the player set up), else its usual install path
        Runtime active = describe(plat::active_runtime());
        if (active.found && active.is == c.runtime) r = active;
        else
            for (auto &i : installed_runtimes())
                if (i.is == c.runtime) {
                    r = i;
                    break;
                }
        r.set_env = r.json;
    }
    r.chosen = c.runtime;
    return r;
}

// ---- the checks ----
// A runtime with none of its own way to ask: an OpenXR instance, and whether it has a headset (xrGetSystem)
static void openxr_check(VrState &s) {
    static void *lib = (void *)-1;
    if (lib == (void *)-1) lib = plat::open_openxr_loader();
    if (!lib) {
        s.session = Check::Ok;   // (no loader to ask with: the game says it in its log)
        s.headset = Check::Unknown;
        return;
    }
    auto gipa = (PFN_xrGetInstanceProcAddr)plat::symbol(lib, "xrGetInstanceProcAddr");
    PFN_xrCreateInstance create = nullptr;
    if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create)) || !create) return;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "HookshotVR Launcher");
    strcpy(ci.applicationInfo.engineName, "HookshotVR");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance inst = XR_NULL_HANDLE;
    plat::set_env("XR_RUNTIME_JSON", s.runtime.json);   // (the probe asks the runtime the game will get)
    if (XR_FAILED(create(&ci, &inst))) {
        s.session = Check::Waiting;
        s.headset = Check::Waiting;
        return;
    }
    s.session = Check::Ok;
    PFN_xrGetSystem get_system = nullptr;
    PFN_xrDestroyInstance destroy = nullptr;
    gipa(inst, "xrGetSystem", (PFN_xrVoidFunction *)&get_system);
    gipa(inst, "xrDestroyInstance", (PFN_xrVoidFunction *)&destroy);
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId id = XR_NULL_SYSTEM_ID;
    s.headset = get_system && XR_SUCCEEDED(get_system(inst, &gi, &id)) ? Check::Ok : Check::Waiting;
    if (destroy) destroy(inst);
}

std::string runtime_label(const Runtime &r) {
    static const char *kNames[kRuntimeKinds] = {"", "WiVRn", "SteamVR", "Monado", ""};
    return r.is > kRuntimeSystem && r.is < kRuntimeCustom ? kNames[r.is] : r.name;
}

static void check(VrState &s) {
    const Runtime &r = s.runtime;
    if (!r.found) {
        s.session = Check::Bad;
        s.none_installed = plat::installed_runtimes().empty();
    } else if (r.is == kRuntimeWiVRn) {
        // (WiVRn says on D-Bus whether its server runs and a headset is connected; without D-Bus, its process)
        bool running = false, headset = false;
        s.servers = *plat::wivrn_process() ? plat::process_count(plat::wivrn_process()) : 0;
        if (plat::wivrn_status(&running, &headset)) {
            s.session = running ? Check::Ok : Check::Waiting;
            s.headset = headset ? Check::Ok : Check::Waiting;
        } else {
            s.session = *plat::wivrn_process() && plat::process_running(plat::wivrn_process()) ? Check::Ok : Check::Waiting;
        }
    } else if (plat::process_running("hookshot")) {
        s.session = Check::Ok;   // (no second OpenXR client while the game runs)
    } else if (r.is == kRuntimeSteamVR) {
        // (never an instance of SteamVR's: that starts SteamVR. Its server's process says it runs)
        s.session = plat::process_running(plat::steamvr_process()) ? Check::Ok : Check::Waiting;
    } else {
        openxr_check(s);
    }
}

namespace {
std::mutex vr_mu;
VrState vr_now;
Config vr_cfg;   // (the runtime and Start With the checks use)
double started_with = -1e9;   // (when Start With's app was last started from here)
std::atomic<bool> vr_on{false}, vr_thread_up{false};
}  // namespace

VrState vr_state() {
    std::lock_guard<std::mutex> l(vr_mu);
    return vr_now;
}

void vr_watch(bool on, const Config &c) {
    {
        std::lock_guard<std::mutex> l(vr_mu);
        bool changed = c.runtime != vr_cfg.runtime || c.runtime_json != vr_cfg.runtime_json;
        vr_cfg = c;
        if (changed) vr_now = VrState();   // (a new choice is not shown with the old one's answer)
    }
    vr_on = on;
    if (!on || vr_thread_up.exchange(true)) return;
    std::thread([] {
        for (;;) {
            if (vr_on) {
                Config c;
                {
                    std::lock_guard<std::mutex> l(vr_mu);
                    c = vr_cfg;
                }
                VrState s;
                s.runtime = find_runtime(c);
                check(s);
                std::lock_guard<std::mutex> l(vr_mu);
                // (Start offered again once what it started has had 20 s to come up)
                s.can_start = !vr_cfg.start_with.empty() && s.session != Check::Ok && now_seconds() - started_with > 20;
                if (vr_cfg.runtime == c.runtime && vr_cfg.runtime_json == c.runtime_json) vr_now = s;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }).detach();
}

bool start_with(const Config &c) {
    if (c.start_with.empty()) return false;
    bool ok = plat::start_app(c.start_with);
    std::lock_guard<std::mutex> l(vr_mu);
    if (ok) started_with = now_seconds(), vr_now.can_start = false;
    return ok;
}

}  // namespace sys
