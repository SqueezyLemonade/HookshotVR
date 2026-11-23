// platform.h on Windows, cross-built with llvm-mingw (vr/launcher/build-windows.sh). Every path in and out is UTF-8, turned
// into UTF-16 here (no ANSI code page: tests/test_utf8_paths.py's rule); the exe's manifest makes the C library's and
// argv's code page UTF-8 too, as the game's does (nx2/win_manifest.py).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>
#include <thread>

namespace plat {

static std::wstring W(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    return a.back() == '\\' || a.back() == '/' ? a + b : a + "\\" + b;
}
static bool exists(const std::string &p) { return GetFileAttributesW(W(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = U(p);
    CoTaskMemFree(p);
    return s;
}

double now_seconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

std::string home() { return known_folder(FOLDERID_Profile); }
std::string config_dir() { return join(known_folder(FOLDERID_LocalAppData), "HookshotVR"); }
std::string data_dir() { return config_dir(); }
std::string exe_dir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    buf.resize(n);
    std::string s = U(buf);
    size_t cut = s.find_last_of("\\/");
    return cut == std::string::npos ? "." : s.substr(0, cut);
}
char sep() { return '\\'; }
std::string env(const char *name) {
    std::wstring n = W(name);
    DWORD len = GetEnvironmentVariableW(n.c_str(), nullptr, 0);
    if (!len) return "";
    std::wstring v(len, L'\0');
    v.resize(GetEnvironmentVariableW(n.c_str(), &v[0], len));
    return U(v);
}
void set_env(const char *name, const std::string &value) {
    SetEnvironmentVariableW(W(name).c_str(), value.empty() ? nullptr : W(value).c_str());
}
bool is_exe(const std::string &p) {
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string real_path(const std::string &p) {
    std::wstring buf(32768, L'\0');
    DWORD n = GetFullPathNameW(W(p).c_str(), (DWORD)buf.size(), &buf[0], nullptr);
    if (!n || n >= buf.size()) return p;
    buf.resize(n);
    return U(buf);
}
double free_gb(const std::string &dir) {
    ULARGE_INTEGER avail;
    if (dir.empty() || !GetDiskFreeSpaceExW(W(dir).c_str(), &avail, nullptr, nullptr)) return -1;
    return (double)avail.QuadPart / 1e9;
}

// ---- processes ----
// (the command line CommandLineToArgvW reads back: quotes, and backslashes doubled before a quote)
static std::wstring quote(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}

// The launcher's environment with env's changes, sorted without case as Windows keeps it
static std::wstring env_block(const Env &over) {
    struct NoCase {
        bool operator()(const std::wstring &a, const std::wstring &b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, std::wstring, NoCase> vars;
    wchar_t *all = GetEnvironmentStringsW();
    for (wchar_t *e = all; e && *e; e += wcslen(e) + 1) {
        const wchar_t *eq = wcschr(e + 1, L'=');   // (+1: the "=C:" entries start with one)
        if (eq) vars[std::wstring(e, eq - e)] = eq + 1;
    }
    if (all) FreeEnvironmentStringsW(all);
    for (auto &kv : over) {
        if (kv.second) vars[W(kv.first)] = W(*kv.second);
        else vars.erase(W(kv.first));
    }
    std::wstring block;
    for (auto &kv : vars) block += kv.first + L"=" + kv.second + L'\0';
    return block + L'\0';
}

// CreateProcessW in a job object of its own: kill_tree ends it and everything it started (make_game.py's compilers)
Proc spawn(const std::vector<std::string> &argv, const Env &env, const std::string &cwd, Out out, const std::string &file) {
    Proc p;
    if (argv.empty()) return p;
    std::wstring cmd;
    for (auto &a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote(W(a));
    std::wstring block = env_block(env);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (out == Out::Pipe && !CreatePipe(&rd, &wr, &sa, 0)) return p;
    if (out == Out::Pipe) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (out == Out::File)
        wr = CreateFileW(W(file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == Out::Null) wr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in, si.hStdOutput = wr, si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = W(cwd);
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
                             (void *)block.c_str(), cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (in && in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (wr && wr != INVALID_HANDLE_VALUE) CloseHandle(wr);
    if (!ok) {
        if (rd) CloseHandle(rd);
        return p;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p.pid = (intptr_t)pi.hProcess, p.job = (intptr_t)job, p.out = rd ? (intptr_t)rd : -1;
    return p;
}

long read_out(Proc &p, char *buf, long n) {
    DWORD got = 0;
    if (p.out == -1 || !ReadFile((HANDLE)p.out, buf, (DWORD)n, &got, nullptr)) return 0;
    return (long)got;
}
void close_out(Proc &p) {
    if (p.out != -1) CloseHandle((HANDLE)p.out);
    p.out = -1;
}
static void done_with(Proc &p) {
    if (p.pid != -1) CloseHandle((HANDLE)p.pid);
    if (p.job) CloseHandle((HANDLE)p.job);   // (no KILL_ON_JOB_CLOSE: the game outlives the launcher)
    p.pid = -1, p.job = 0;
}
bool exited(Proc &p, int *code) {
    if (p.pid == -1) return true;
    if (WaitForSingleObject((HANDLE)p.pid, 0) != WAIT_OBJECT_0) return false;
    DWORD c = (DWORD)-1;
    GetExitCodeProcess((HANDLE)p.pid, &c);
    if (code) *code = (int)c;
    done_with(p);
    return true;
}
int wait(Proc &p) {
    if (p.pid == -1) return -1;
    WaitForSingleObject((HANDLE)p.pid, INFINITE);
    int c = -1;
    exited(p, &c);
    return c;
}
void kill_tree(Proc &p) {
    if (p.job) TerminateJobObject((HANDLE)p.job, 1);
    else if (p.pid != -1) TerminateProcess((HANDLE)p.pid, 1);
}
void forget(Proc &p) { done_with(p); }

bool process_running(const char *name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    std::wstring want = W(name) + L".exe";
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &e); more && !found; more = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, want.c_str()) == 0;
    CloseHandle(snap);
    return found;
}
bool on_path(const char *tool) {
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, W(tool).c_str(), L".exe", MAX_PATH, buf, nullptr) > 0;
}

// ---- the desktop ----
std::string pick(bool folder, const std::string &title, const std::string &start, const std::string &filter_name,
                 const std::vector<std::string> &patterns) {
    std::string result;
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    IFileOpenDialog *d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD opts = 0;
        d->GetOptions(&opts);
        d->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
        d->SetTitle(W(title).c_str());
        std::wstring name = W(filter_name), spec;
        for (auto &g : patterns) spec += (spec.empty() ? L"" : L";") + W(g);
        COMDLG_FILTERSPEC filters[2] = {{name.c_str(), spec.c_str()}, {L"All files", L"*.*"}};
        if (!folder && !spec.empty()) d->SetFileTypes(2, filters);
        IShellItem *from = nullptr;
        if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(W(start).c_str(), nullptr, IID_PPV_ARGS(&from)))) {
            d->SetFolder(from);
            from->Release();
        }
        IShellItem *item = nullptr;
        if (SUCCEEDED(d->Show(nullptr)) && SUCCEEDED(d->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) result = U(p);
            CoTaskMemFree(p);
            item->Release();
        }
        d->Release();
    }
    if (com) CoUninitialize();
    return result;
}

void open(const std::string &what) { ShellExecuteW(nullptr, L"open", W(what).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
bool start_app(const std::string &app) {
    // (an .exe or a shortcut: the shell starts either)
    return (intptr_t)ShellExecuteW(nullptr, L"open", W(app).c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}
// (a shortcut the launcher wrote says so in its description, so one of the player's of that name is left alone)
static const wchar_t *kShortcutMark = L"Hookshot VR launcher";

static bool own_or_none(const std::wstring &lnk) {
    if (GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    IShellLinkW *l = nullptr;
    IPersistFile *f = nullptr;
    wchar_t desc[256] = L"";
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l))) &&
        SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f))) && SUCCEEDED(f->Load(lnk.c_str(), STGM_READ)))
        l->GetDescription(desc, 256);
    if (f) f->Release();
    if (l) l->Release();
    return wcscmp(desc, kShortcutMark) == 0;
}

bool install_shortcut(const std::string &name, const std::string &exe, const std::string &, std::vector<std::string> *written,
                      std::string *err) {
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    bool ok = true;
    std::wstring wexe = W(exe), wdir = W(exe.substr(0, exe.find_last_of("\\/")));
    for (REFKNOWNFOLDERID where : {FOLDERID_Programs, FOLDERID_Desktop}) {
        std::string dir = known_folder(where);
        if (dir.empty()) continue;
        std::wstring lnk = W(join(dir, name + ".lnk"));
        if (!own_or_none(lnk)) {
            if (err) *err = U(lnk) + " is not the launcher's: left as it is";
            ok = false;
            continue;
        }
        IShellLinkW *l = nullptr;
        IPersistFile *f = nullptr;
        bool done = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l))) &&
                    SUCCEEDED(l->SetPath(wexe.c_str())) && SUCCEEDED(l->SetWorkingDirectory(wdir.c_str())) &&
                    SUCCEEDED(l->SetIconLocation(wexe.c_str(), 0)) && SUCCEEDED(l->SetDescription(kShortcutMark)) &&
                    SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f))) && SUCCEEDED(f->Save(lnk.c_str(), TRUE));
        if (f) f->Release();
        if (l) l->Release();
        if (done && written) written->push_back(U(lnk));
        if (!done) {
            if (err) *err = "could not write " + U(lnk);
            ok = false;
        }
    }
    if (com) CoUninitialize();
    return ok;
}

// ---- OpenXR ----
static std::string reg_string(HKEY root, const wchar_t *key, const wchar_t *value) {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) != ERROR_SUCCESS) return "";
    return U(buf);
}
static const wchar_t *kKhronos = L"SOFTWARE\\Khronos\\OpenXR\\1";

std::string active_runtime() {
    std::string r = reg_string(HKEY_LOCAL_MACHINE, kKhronos, L"ActiveRuntime");
    return !r.empty() && exists(r) ? r : "";
}

std::vector<std::string> installed_runtimes() {
    // The registry's AvailableRuntimes: every installed runtime's manifest is a value name there
    std::vector<std::string> out;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(kKhronos) + L"\\AvailableRuntimes").c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        wchar_t name[2048];
        for (DWORD i = 0;; i++) {
            DWORD n = 2048;
            if (RegEnumValueW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::string p = U(std::wstring(name, n));
            if (exists(p)) out.push_back(p);
        }
        RegCloseKey(k);
    }
    // (and Steam's default folder, for a SteamVR never made available)
    std::string steam = reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    for (const std::string &s : {steam, known_folder(FOLDERID_ProgramFilesX86) + "\\Steam"}) {
        std::string j = join(s, "steamapps\\common\\SteamVR\\steamxr_win64.json");
        if (s.empty() || !exists(j)) continue;
        std::string r = real_path(j);
        bool seen = false;
        for (auto &o : out) seen = seen || _wcsicmp(W(real_path(o)).c_str(), W(r).c_str()) == 0;
        if (!seen) out.push_back(j);
    }
    return out;
}

void *open_openxr_loader() {
    // (shipped beside the launcher and the game: Windows has no system loader)
    return (void *)LoadLibraryW(W(join(exe_dir(), "openxr_loader.dll")).c_str());
}
void *symbol(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : nullptr; }

bool wivrn_status(bool *, bool *) { return false; }   // (WiVRn's server is Linux's)
const char *wivrn_process() { return ""; }
const char *steamvr_process() { return "vrserver"; }

// ---- the game ----
std::string game_bin(const std::string &dir) {
    for (const char *b : {"bin\\hookshot.exe", "hookshot.exe"})
        if (is_exe(join(dir, b))) return join(dir, b);
    return "";
}

std::string game_user_dir(bool *pass) {
    // (as Hookshot's own launcher: %APPDATA%\Hookshot, host/options_path.c. Without it the saves are beside the game, and a
    // new build's folder would start without them)
    *pass = true;
    return join(known_folder(FOLDERID_RoamingAppData), "Hookshot");
}

bool prepare_game(const std::string &game_bin, std::string *err) {
    // (openxr_loader.dll: the game opens it by name (vr/headset/runtime_loader.inc), which Windows looks for beside the exe
    // first. Copied from beside the launcher when that one is missing there or differs in size, so an update's new build
    // folder gets it too. A copy that fails stops nothing but VR: the game says "no OpenXR loader")
    std::string from = join(exe_dir(), "openxr_loader.dll");
    std::string to = join(game_bin.substr(0, game_bin.find_last_of("\\/")), "openxr_loader.dll");
    WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
    if (!GetFileAttributesExW(W(from).c_str(), GetFileExInfoStandard, &a)) return true;   // (none shipped: a source checkout)
    if (GetFileAttributesExW(W(to).c_str(), GetFileExInfoStandard, &b) && a.nFileSizeLow == b.nFileSizeLow &&
        a.nFileSizeHigh == b.nFileSizeHigh)
        return true;
    if (!CopyFileW(W(from).c_str(), W(to).c_str(), FALSE) && err) *err = "could not copy openxr_loader.dll beside the game";
    return true;
}

// The Windows builder: Hookshot's own layout, as its setup installs it (the HookshotBuilder folder of a version under
// %LOCALAPPDATA%\HookshotBuilder\versions) and as the release archive carries it in builder: tools\python (its own
// Python), tools\llvm-mingw (the compiler; make_game.py looks for it beside nx2, which is the HookshotVR source here, so it
// is named) and tools\cpm (the Vulkan headers, Windows FFmpeg and dav1d)
static std::string builder_tools(const std::string &builder) { return join(builder, "tools"); }
Env builder_env(const std::string &builder) {
    Env e{{"HOOKSHOT_MINGW", join(builder_tools(builder), "llvm-mingw")}};
    std::string cpm = join(builder_tools(builder), "cpm");
    DWORD at = GetFileAttributesW(W(cpm).c_str());
    if (env("RX_CPM").empty() && at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) e["RX_CPM"] = cpm;
    return e;
}
std::string builder_python(const std::string &builder) { return join(builder_tools(builder), "python\\python.exe"); }

}  // namespace plat
