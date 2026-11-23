// update.h: the check against GitHub's releases and the one-click update. The self-test runs it against a stand-in release
// server (tools/fake_release_server.py) and a stand-in builder (tools/selftest.cpp).
#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace upd {

const char *const kVersion = "v1.0.1";
const char *const kVersionName = "Alpha v1.0.1";
// The public release repository: its releases (and the source at each tag). Never a URL or a header with a token in it
static const char *const kRepo = "SqueezyLemonade/HookshotVR";
static constexpr double kGameGB = 60;       // (a rebuild: a second game folder beside the one played, while it builds)
static constexpr double kLauncherGB = 4;    // (the download and what it unpacks: the builder is most of it)

static std::string g_version = kVersion;
std::string version() { return g_version; }
void set_version(const std::string &v) { g_version = v; }

static fs::path P(const std::string &s) { return fs::u8path(s); }

bool newer(const std::string &a, const std::string &b) {
    int x[3] = {0}, y[3] = {0};
    sscanf(a.c_str(), "v%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b.c_str(), "v%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

// (a tag the launcher acts on: vX.Y.Z and nothing else, as it names folders and URLs)
static bool is_version(const std::string &v) {
    int dots = 0;
    if (v.size() < 6 || v.size() > 20 || v[0] != 'v' || v[1] < '0' || v[1] > '9') return false;
    for (size_t i = 1; i < v.size(); i++) {
        if (v[i] == '.' && v[i - 1] != '.' && i + 1 < v.size()) dots++;
        else if (v[i] < '0' || v[i] > '9') return false;
    }
    return dots == 2;
}
static bool is_file_name(const std::string &n) {
    if (n.empty() || n.size() > 200 || n[0] == '.') return false;
    for (char c : n)
        if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') return false;
    return true;
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

// ---- where releases come from ----
// (HOOKSHOTVR_DEV_RELEASES: development only, the self-test's stand-in server; with it, plain http is allowed)
static std::string dev_base() { return plat::env("HOOKSHOTVR_DEV_RELEASES"); }
static std::string api() { return (dev_base().empty() ? "https://api.github.com" : dev_base() + "/api") + "/repos/" + kRepo; }
static std::string site() { return (dev_base().empty() ? "https://github.com" : dev_base() + "/site") + "/" + kRepo; }
static std::string raw() { return (dev_base().empty() ? "https://raw.githubusercontent.com" : dev_base() + "/raw") + "/" + kRepo; }
std::string release_page(const std::string &v) { return site() + (is_version(v) ? "/releases/tag/" + v : "/releases"); }
static std::string download_url(const std::string &tag, const std::string &file) { return site() + "/releases/download/" + tag + "/" + file; }

// curl, the system's own (plat::system_tool), never asked for a login: -q leaves ~/.curlrc out, no netrc, no token. Only
// https (redirects too), but for the stand-in server
static std::vector<std::string> curl(const std::string &url, bool download) {
    std::vector<std::string> a{plat::system_tool("curl"), "-q", "--silent", "--fail", "--location", "--max-redirs", "5",
                               "--connect-timeout", "20", "-A", "HookshotVR-Launcher/" + version()};
    if (dev_base().empty()) a.insert(a.end(), {"--proto", "=https", "--proto-redir", "=https", "--tlsv1.2"});
    // (the check: one try, a short wait. A download: a stalled one stops after a minute, a dropped one is tried again)
    if (download) a.insert(a.end(), {"--retry", "2", "--retry-max-time", "60", "--speed-limit", "1024", "--speed-time", "60"});
    else a.insert(a.end(), {"--max-time", "30", "-H", "Accept: application/vnd.github+json"});
    a.push_back(url);
    return a;
}
static bool fetch_text(const std::string &url, std::string *out) {
    if (plat::system_tool("curl").empty()) return false;
    int code = -1;
    *out = sys::run(curl(url, false), "", &code);
    return code == 0;
}

// ---- JSON, enough of it for GitHub's releases list ----
namespace {
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json &operator[](const char *k) const {
        static const Json none;
        for (auto &kv : o)
            if (kv.first == k) return kv.second;
        return none;
    }
};
struct Parser {
    const std::string &t;
    size_t i = 0;
    bool bad = false;
    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++;
    }
    static void utf8(std::string &s, unsigned c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | ((c >> 6) & 0x3F)), s += (char)(0x80 | (c & 0x3F));
        else s += (char)(0xF0 | c >> 18), s += (char)(0x80 | ((c >> 12) & 0x3F)), s += (char)(0x80 | ((c >> 6) & 0x3F)), s += (char)(0x80 | (c & 0x3F));
    }
    unsigned hex4() {
        if (i + 4 > t.size()) return bad = true, 0;
        unsigned v = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
        i += 4;
        return v;
    }
    std::string str() {
        std::string s;
        i++;   // (")
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { s += c; continue; }
            if (i >= t.size()) break;
            char e = t[i++];
            switch (e) {
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u': {
                    unsigned c1 = hex4();
                    if (c1 >= 0xD800 && c1 < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        unsigned c2 = hex4();
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    utf8(s, c1);
                    break;
                }
                default: s += e;
            }
        }
        if (i >= t.size()) bad = true;
        i++;
        return s;
    }
    Json value(int depth) {
        Json v;
        ws();
        if (i >= t.size() || depth > 32) return bad = true, v;
        char c = t[i];
        if (c == '{') {
            v.t = Json::Obj, i++;
            for (ws(); i < t.size() && t[i] != '}';) {
                if (t[i] != '"') return bad = true, v;
                std::string k = str();
                ws();
                if (i >= t.size() || t[i++] != ':') return bad = true, v;
                v.o.emplace_back(k, value(depth + 1));
                ws();
                if (i < t.size() && t[i] == ',') i++, ws();
                if (bad) return v;
            }
            i++;
        } else if (c == '[') {
            v.t = Json::Arr, i++;
            for (ws(); i < t.size() && t[i] != ']';) {
                v.a.push_back(value(depth + 1));
                ws();
                if (i < t.size() && t[i] == ',') i++, ws();
                if (bad) return v;
            }
            i++;
        } else if (c == '"') {
            v.t = Json::Str, v.s = str();
        } else if (!t.compare(i, 4, "true")) {
            v.t = Json::Bool, v.b = true, i += 4;
        } else if (!t.compare(i, 5, "false")) {
            v.t = Json::Bool, i += 5;
        } else if (!t.compare(i, 4, "null")) {
            i += 4;
        } else {
            char *end = nullptr;
            v.t = Json::Num, v.n = strtod(t.c_str() + i, &end);
            if (end == t.c_str() + i) return bad = true, v;
            i = (size_t)(end - t.c_str());
        }
        return v;
    }
};
}  // namespace

// ---- SHA-256 (FIPS 180-4) ----
namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t used = 0;
    uint64_t bits = 0;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char *p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    void add(const unsigned char *p, size_t n) {
        bits += (uint64_t)n * 8;
        while (n) {
            size_t take = std::min(n, sizeof buf - used);
            memcpy(buf + used, p, take);
            used += take, p += take, n -= take;
            if (used == 64) block(buf), used = 0;
        }
    }
    std::string hex() {
        uint64_t len = bits;
        unsigned char pad = 0x80, zero = 0;
        add(&pad, 1);
        while (used != 56) add(&zero, 1);
        unsigned char l[8];
        for (int i = 0; i < 8; i++) l[i] = (unsigned char)(len >> (56 - 8 * i));
        add(l, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};
}  // namespace

std::string sha256_file(const std::string &path) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return "";
    Sha256 s;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        if (f.gcount() > 0) s.add((const unsigned char *)buf.data(), (size_t)f.gcount());
    }
    return f.bad() ? "" : s.hex();
}

// "<hash>  <name>" lines (SHA256SUMS.txt, GAME_STAMPS.txt): the hash said for name
static std::string listed(const std::string &text, const std::string &name) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string file = line.substr(line.find_first_not_of(" *", sp));
        if (file == name) return line.substr(0, sp);
    }
    return "";
}

// ---- the folders ----
static std::mutex mu;
static std::mutex root_mu;   // (its own: install_root() is asked with mu held too)
static std::string root_override;

static bool is_repo(const std::string &d) { return sys::exists(sys::join(d, "nx2/make_game.py")) && sys::is_dir(sys::join(d, "vr/launcher")); }
std::string install_root() {
    {
        std::lock_guard<std::mutex> l(root_mu);
        if (!root_override.empty()) return root_override;
    }
    // (a release's folder: the launcher with the HookshotVR source beside it; a checkout's build folder is not one)
    std::string here = sys::exe_dir();
    return sys::exists(sys::join(here, plat::launcher_file())) && is_repo(sys::join(here, "HookshotVR")) ? here : "";
}
void set_install_root(const std::string &dir) {
    std::lock_guard<std::mutex> l(root_mu);
    root_override = dir;
}
std::string staging() {
    std::string r = install_root();
    return r.empty() ? "" : sys::join(r, ".update");
}
static std::string download_dir(const std::string &tag) { return sys::join(sys::join(staging(), "download"), tag); }

std::string game_folder_name(const std::string &v, const std::string &current) {
    std::string name = v + "-game";
    for (int i = 1; name == current; i++) name = v + "." + std::to_string(i) + "-game";
    return name;
}

double needed_gb(const Release &r) { return (r.launcher ? kLauncherGB : 0) + (r.rebuild ? kGameGB : 0); }

// What the notes say is new, when the release's CHANGELOG.txt says nothing: its list items, else its first lines. Never empty
static std::vector<std::string> notes_lines(const std::string &body, const std::string &tag) {
    std::vector<std::string> items, plain;
    std::istringstream in(body);
    std::string line;
    auto clean = [](std::string s) {
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '*' || s[i] == '`' || (s[i] == '_' && (i == 0 || s[i - 1] == ' ' || i + 1 == s.size() || s[i + 1] == ' '))) continue;
            if (s[i] == '[') {   // ([text](link): the text)
                size_t close = s.find("](", i), end = close == std::string::npos ? close : s.find(')', close);
                if (end != std::string::npos) {
                    out += s.substr(i + 1, close - i - 1);
                    i = end;
                    continue;
                }
            }
            out += s[i];
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    };
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t lead = line.find_first_not_of(' ');
        if (lead == std::string::npos) continue;
        line = line.substr(lead);
        if ((line[0] == '-' || line[0] == '*') && line.size() > 2 && line[1] == ' ') items.push_back(clean(line.substr(2)));
        else if (line[0] != '#' && line[0] != '|' && line[0] != '>' && line.compare(0, 3, "```") && plain.size() < 6) plain.push_back(clean(line));
    }
    std::vector<std::string> &use = items.empty() ? plain : items;
    use.erase(std::remove(use.begin(), use.end(), std::string()), use.end());
    if (use.empty()) use.push_back("HookshotVR " + tag);
    return use;
}

// ---- the check ----
namespace {
bool found = false, check_done = false, check_started = false;
Release best;
std::atomic<bool> bg_fetch{false};   // (Automatic's download, on the check's thread)
}  // namespace

static bool fetch_release(const Release &r, std::string *err, const std::atomic<bool> *stop);

void check(const sys::Paths &p, const sys::Config &c) {
    bool none = c.updates == sys::kUpdatesOff || install_root().empty() || plat::system_tool("curl").empty();
    {
        std::lock_guard<std::mutex> l(mu);
        if (check_started) return;   // (once a start: GitHub allows 60 unsigned asks an hour)
        check_started = true;
        if (none) {
            check_done = true;
            return;
        }
    }
    bool fetch = c.updates == sys::kUpdatesAuto;
    std::string game_stamp = sys::game_stamp(p.game), mine = version();
    std::thread([fetch, game_stamp, mine] {
        Release r;
        std::string text;
        Json list;
        if (fetch_text(api() + "/releases?per_page=20", &text)) {
            Parser ps{text};
            list = ps.value(0);
            if (ps.bad) list = Json();
        }
        // (the newest vX.Y.Z that is not a draft; the alphas are pre-releases, so /releases/latest would find none)
        const Json *newest = nullptr;
        for (const Json &rel : list.a) {
            const std::string &tag = rel["tag_name"].s;
            if (rel["draft"].b || !is_version(tag) || !newer(tag, newest ? (*newest)["tag_name"].s : mine)) continue;
            newest = &rel;
        }
        if (newest) {
            const Json &rel = *newest;
            r.version = rel["tag_name"].s;
            r.page = release_page(r.version);
            bool sums = false, stamps = false;
            for (const Json &a : rel["assets"].a) {
                const std::string &name = a["name"].s;
                if (name == "SHA256SUMS.txt") sums = true;
                else if (name == "GAME_STAMPS.txt") stamps = true;
                else if (is_file_name(name) && name.size() > strlen(plat::release_suffix()) &&
                         !name.compare(name.size() - strlen(plat::release_suffix()), std::string::npos, plat::release_suffix()))
                    r.archive = name, r.archive_mb = a["size"].n / 1048576.0;
            }
            if (!sums) r.archive.clear();   // (an archive nothing checks is never used)
            if (stamps && !r.archive.empty() && fetch_text(download_url(r.version, "GAME_STAMPS.txt"), &text)) r.stamp = listed(text, r.archive);
            if (fetch_text(raw() + "/" + r.version + "/vr/launcher/CHANGELOG.txt", &text)) r.changes = changes_of(text, r.version);
            if (r.changes.empty()) r.changes = notes_lines(rel["body"].s, r.version);
            r.rebuild = r.stamp.empty() || game_stamp.empty() || r.stamp != game_stamp;
            // (Automatic: downloaded and checked now, so Update only installs)
            std::string err;
            if (!r.archive.empty() && fetch) {
                bg_fetch = true;
                r.downloaded = fetch_release(r, &err, nullptr);
                bg_fetch = false;
            }
        }
        std::lock_guard<std::mutex> l(mu);
        found = !r.archive.empty(), best = r, check_done = true;
    }).detach();
}

bool available(Release *r) {
    std::lock_guard<std::mutex> l(mu);
    if (found && r) *r = best;
    return found;
}
bool checked() {
    std::lock_guard<std::mutex> l(mu);
    return check_done;
}
void forget_check() {
    std::lock_guard<std::mutex> l(mu);
    found = check_done = check_started = false, best = Release();
}

bool pending(const sys::Paths &p, Release *r) {
    std::string src = sys::source_stamp(p.repo), game = sys::game_stamp(p.game);
    if (!p.game_built || !p.builder_ok || src.empty() || game.empty() || src == game) return false;
    if (r) {
        *r = Release();
        r->version = version(), r->page = release_page(r->version), r->launcher = false, r->rebuild = true, r->stamp = src;
    }
    return true;
}

// ---- the download ----
namespace {
Progress prog;
sys::Paths paths;
Release rel;
std::string fake, new_game, old_game, new_stamp;
std::atomic<bool> busy{false}, thread_done{false}, thread_ok{false}, cancelled{false};
std::atomic<double> dl_fraction{0};
std::string thread_err, dl_part;   // (under mu)
}  // namespace

bool running() { return busy; }

// url into file (through file.part, so a file there is whole). stop: Cancel, which ends curl. total_mb: for the progress
static bool download_to(const std::string &url, const std::string &file, double total_mb, const std::atomic<bool> *stop) {
    std::string part = file + ".part";
    sys::remove_tree(part);
    std::vector<std::string> argv = curl(url, true);
    argv.insert(argv.end() - 1, {"-o", part});
    sys::Proc pr = plat::spawn(argv, {}, "", plat::Out::Null);
    if (!pr.ok()) return false;
    {
        std::lock_guard<std::mutex> l(mu);
        dl_part = total_mb > 0 ? part : "";
    }
    int code = -1;
    while (!plat::exited(pr, &code)) {
        if (stop && *stop) {
            plat::kill_tree(pr);
            plat::wait(pr);
            code = -1;
            break;
        }
        if (total_mb > 0) {
            std::error_code e;
            auto n = fs::file_size(P(part), e);
            if (!e) dl_fraction = std::min(1.0, (double)n / (total_mb * 1048576.0));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    {
        std::lock_guard<std::mutex> l(mu);
        dl_part.clear();
    }
    std::error_code e;
    if (code == 0) fs::rename(P(part), P(file), e);
    if (code != 0 || e) sys::remove_tree(part);
    return code == 0 && !e;
}

// The release's archive into <staging>/download/<tag>, checked against its SHA256SUMS.txt (an archive already there that
// checks is not downloaded again; one that does not is deleted)
static bool fetch_release(const Release &r, std::string *err, const std::atomic<bool> *stop) {
    std::string dir = download_dir(r.version), sums = sys::join(dir, "SHA256SUMS.txt"), file = sys::join(dir, r.archive);
    if (!sys::make_dirs(dir)) return *err = "No space", false;
    if (!download_to(download_url(r.version, "SHA256SUMS.txt"), sums, 0, stop)) return *err = stop && *stop ? "Cancelled" : "Download failed", false;
    std::string want = listed(sys::read_file(sums), r.archive);
    if (want.size() != 64) return *err = "Bad download", false;
    if (sys::exists(file) && sha256_file(file) == want) return true;
    if (!download_to(download_url(r.version, r.archive), file, r.archive_mb, stop)) return *err = stop && *stop ? "Cancelled" : "Download failed", false;
    if (sha256_file(file) != want) {
        sys::remove_tree(file);
        return *err = "Bad download", false;
    }
    return true;
}

// The checked archive into <staging>/new: one HookshotVR folder holding this launcher's file, the source and its CHANGELOG.txt
// for this very release
static bool unpack(const Release &r, std::string *err) {
    std::string to = sys::join(staging(), "new"), top = sys::join(to, "HookshotVR");
    sys::remove_tree(to);
    if (!sys::make_dirs(to)) return *err = "No space", false;
    std::vector<std::string> argv = plat::unpack_cmd(sys::join(download_dir(r.version), r.archive), to);
    if (argv[0].empty()) return *err = "No tar", false;
    int code = -1;
    sys::run(argv, to, &code);
    if (code != 0) return *err = "Unpack failed", false;
    std::string log = sys::read_file(sys::join(top, "CHANGELOG.txt")), first;
    for (size_t at = 0; at < log.size() && first.empty();) {
        size_t nl = log.find('\n', at);
        std::string line = log.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 1 && line[0] == 'v' && line[1] >= '0' && line[1] <= '9') first = line;
        at = nl == std::string::npos ? log.size() : nl + 1;
    }
    if (!plat::is_exe(sys::join(top, plat::launcher_file())) || !is_repo(sys::join(top, "HookshotVR")) || first != r.version)
        return *err = "Bad download", false;
    return true;
}

// ---- the install: what the archive ships swapped in beside the launcher ----
// (the player's own: the game folders, the translation, the caches, the update's own folder; none is in an archive)
static bool players(const std::string &name) {
    return name == ".update" || name == "main-work" || name == "cache" ||
           (name.size() > 5 && name.compare(name.size() - 5, 5, "-game") == 0);
}

static bool install(std::string *err) {
    std::string root = install_root(), from = sys::join(sys::join(staging(), "new"), "HookshotVR"), old = sys::join(staging(), "old");
    sys::remove_tree(old);
    if (!sys::make_dirs(old)) return *err = "Install failed", false;
    std::vector<std::string> names;
    std::error_code e;
    for (auto &d : fs::directory_iterator(P(from), e)) names.push_back(d.path().filename().u8string());
    std::sort(names.begin(), names.end());
    // (each one: the old moved aside into .update/old, then the new into its place. Renames on one drive: a running exe
    // and the DLLs it loaded can be renamed on Windows, not deleted, so the old ones go at the next start)
    std::vector<std::pair<std::string, std::string>> moved;
    auto mv = [&](const std::string &a, const std::string &b) {
        // (a few tries: on Windows a virus scanner can hold a file it was just shown for a moment)
        std::error_code ec;
        for (int i = 0; i < 10; i++) {
            fs::rename(P(a), P(b), ec);
            if (!ec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if (!ec) moved.emplace_back(a, b);
        return !ec;
    };
    bool ok = !e && !names.empty();
    for (auto &n : names) {
        if (players(n)) continue;
        std::string dst = sys::join(root, n);
        if ((sys::exists(dst) && !mv(dst, sys::join(old, n))) || !mv(sys::join(from, n), dst)) {
            ok = false;
            break;
        }
    }
    if (ok) return true;
    for (auto m = moved.rbegin(); m != moved.rend(); ++m) {   // (back as it was)
        std::error_code ec;
        fs::rename(P(m->second), P(m->first), ec);
    }
    return *err = "Install failed", false;
}

// ---- the update ----
static void fail(const std::string &why) {
    prog.stage = Stage::Failed, prog.error = why;
    // (no mess: what it unpacked and the half-made game folder go; the build the player plays was never touched)
    if (!staging().empty()) sys::remove_tree(sys::join(staging(), "new"));
    if (!new_game.empty() && new_game != old_game) sys::remove_tree(new_game);
}

// The game from the new source into its own folder: from the player's game file when it is still there, else from the game
// files the old folder keeps (its romfs and .hookshot_exefs). The translation is the builds' one
static void begin_build(const sys::Config &cfg, sys::BuildState &build) {
    sys::BuildJob j;
    j.repo = paths.repo, j.out = new_game, j.work = paths.work;
    j.log = sys::join(paths.logs, "build-" + prog.version + ".log");
    if (!cfg.game_file.empty() && sys::exists(cfg.game_file)) j.game_file = cfg.game_file;
    else j.from_game = old_game;
    sys::remove_tree(new_game);   // (what an update stopped before left)
    build = sys::BuildState();
    build.running = true, build.started = build.step_started = sys::now_seconds();
    if (sys::build_start(paths, j, fake)) prog.stage = Stage::Build;
    else fail("Build failed");
}

bool start(const sys::Paths &p, const sys::Config &c, const Release &r, const std::string &fake_build) {
    if (busy || r.version.empty()) return false;
    if (r.launcher ? (install_root().empty() || r.archive.empty()) : (!p.builder_ok || p.hookshot.empty() || !p.game_built)) return false;
    double gb = sys::free_gb(r.launcher ? install_root() : p.hookshot);
    busy = true, cancelled = false;
    paths = p, fake = fake_build, rel = r;
    prog = Progress{Stage::Download, r.version, "", 0, false, ""};
    old_game = p.game, new_game.clear(), new_stamp.clear();
    if (gb >= 0 && gb < needed_gb(r)) {
        prog.stage = Stage::Failed, prog.error = "Not enough space";   // (tick says it once)
        return true;
    }
    if (!r.launcher) {
        // (a pending rebuild: the source is the new one already)
        new_game = sys::join(p.hookshot, game_folder_name(r.version, sys::base_name(p.game)));
        new_stamp = sys::source_stamp(p.repo);
        thread_ok = true, thread_done = true;   // (nothing to download: tick starts the build, which needs the config)
        return true;
    }
    thread_done = false, dl_fraction = 0;
    Release copy = r;
    std::thread([copy] {
        std::string err;
        while (bg_fetch && !cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(100));   // (the same files)
        bool ok = fetch_release(copy, &err, &cancelled) && !cancelled && unpack(copy, &err);
        {
            std::lock_guard<std::mutex> l(mu);
            thread_err = cancelled ? "Cancelled" : err;
        }
        thread_ok = ok && !cancelled, thread_done = true;
    }).detach();
    return true;
}

void cancel() {
    if (!busy) return;
    cancelled = true;
    if (prog.stage == Stage::Build) sys::build_cancel();
    // (the download's curl is stopped before this returns: the launcher may be closing)
    for (int i = 0; i < 30 && prog.stage == Stage::Download && !thread_done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            prog.fraction = dl_fraction;
            build.fraction = dl_fraction;
            if (!thread_done) break;
            if (cancelled) fail("Cancelled");
            else if (!thread_ok) {
                std::string err;
                {
                    std::lock_guard<std::mutex> l(mu);
                    err = thread_err.empty() ? "Download failed" : thread_err;
                }
                fail(err);
            } else if (!rel.launcher) begin_build(cfg, build);
            else prog.stage = Stage::Install;
            break;
        case Stage::Install: {
            std::string err;
            std::string stamp = sys::source_stamp(sys::join(sys::join(sys::join(staging(), "new"), "HookshotVR"), "HookshotVR"));
            if (!install(&err)) {
                fail(err);
                break;
            }
            sys::remove_tree(sys::join(staging(), "new"));
            // (the game again only when the build reads something else now. A game built before stamps were recorded is built
            // again once: nothing says what it was built from)
            std::string game = sys::game_stamp(old_game);
            prog.rebuild = paths.game_built && (stamp.empty() || game.empty() || stamp != game);
            // (the old updater's download of a source, in the data folder: the source is the one beside the launcher now)
            std::string versions = sys::join(sys::data_dir(), "versions");
            if (!cfg.hookshotvr.empty() && cfg.hookshotvr.compare(0, versions.size(), versions) == 0) cfg.hookshotvr.clear(), sys::save_config(cfg);
            prog.restart = sys::join(install_root(), plat::launcher_file());
            prog.stage = Stage::Restart;
            break;
        }
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
            cfg.game_folder = sys::base_name(new_game);
            sys::save_config(cfg);   // (the switch: one whole write of launcher.ini)
            prog.stage = Stage::Clean;
            // The old build goes: its folder in the Hookshot Folder. Never a folder the player chose themself
            std::string game = old_game, hookshot = paths.hookshot, keep = new_game;
            thread_done = false;
            std::thread([game, hookshot, keep] {
                if (sys::parent(game) == hookshot && game != keep) sys::remove_tree(game);
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
    if (prog.stage == Stage::Done || prog.stage == Stage::Failed || prog.stage == Stage::Restart) {
        busy = false;   // (said once)
        prog = Progress();
        std::lock_guard<std::mutex> l(mu);
        if (out.stage != Stage::Failed) found = false;
    }
    return out;
}

void tidy(const sys::Paths &p, const sys::Config &c) {
    if (busy) return;
    std::error_code e;
    // (the update's folder: the old files now, the new ones it never installed, and downloads of releases not newer than
    // this one. The old launcher may still be closing: tried again for a few seconds)
    std::string up = staging();
    if (!up.empty() && sys::is_dir(up)) {
        sys::remove_tree(sys::join(up, "new"));
        for (auto &d : fs::directory_iterator(P(sys::join(up, "download")), e)) {
            std::string tag = d.path().filename().u8string();
            if (!is_version(tag) || !newer(tag, version())) sys::remove_tree(d.path().u8string());
        }
        std::string old = sys::join(up, "old");
        std::thread([old, up] {
            for (int i = 0; i < 20 && sys::exists(old); i++) {
                sys::remove_tree(old);
                if (sys::exists(old)) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::error_code ec;
            fs::remove(P(sys::join(up, "download")), ec);   // (only when empty)
            fs::remove(P(up), ec);
        }).detach();
    }
    // (v1.0.0's update cloned sources into <data>/versions/<v>, on the system drive)
    std::string versions = sys::join(sys::data_dir(), "versions");
    for (auto &d : fs::directory_iterator(P(versions), e)) {
        std::string dir = d.path().u8string();
        if (sys::join(dir, "HookshotVR") != p.repo) sys::remove_tree(dir);
    }
    if (p.hookshot.empty()) return;
    // (only the launcher's own names: <v>-game in the Hookshot Folder; never main-game)
    for (auto &d : fs::directory_iterator(P(p.hookshot), e)) {
        std::string name = d.path().filename().u8string();
        if (name.size() > 6 && name[0] == 'v' && name[1] >= '0' && name[1] <= '9' && name.compare(name.size() - 5, 5, "-game") == 0 &&
            name != c.game_folder && d.is_directory(e))
            sys::remove_tree(d.path().u8string());
    }
}

}  // namespace upd
