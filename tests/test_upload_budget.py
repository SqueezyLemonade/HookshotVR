"""(Hookshot) The upload budget (RX_VK_UPLOAD_BUDGET_MS, vk_texture.cpp's upload_defer): a new ASTC texture a draw samples is
held back for a later present once the looking-up thread's own upload time this present reached the budget, not every
thread's summed (the parallel replay's segment workers upload side by side: their sum reached 40 ms when the busiest had spent
11-31 ms, at the Market's arrival). RX_VK_UPLOAD_BUDGET_SUM=1 is the summed rule, as before. And the deferral's measurement:
how long each held texture waited (defer_note_done, defer_frame_end), its percentile, and its log lines.

upload_defer and defer_pct are cut out of vk_texture.cpp and compiled with the stubs below (a C++20 compiler: g++ or clang++;
skipped without one).

python -I -B tests/test_upload_budget.py
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"
DOC = ROOT / "docs" / "development.md"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
struct Texture {
    bool uploaded = false, astc = true, gpu_written = false;
    uint32_t id = 1, width = 64, height = 64, layers = 1;
    int nvn_format = 0x7b;
    uint64_t defer_frame = 0, defer_seen = 0;
    int64_t defer_ns = 0;
    uint32_t defer_asks = 0;
};
uint64_t frame_no = 7;
std::atomic<int64_t> frame_upload_ns{0}, st_deferred{0}, st_deferred_frame{0}, st_deferred_tex_frame{0};
std::atomic<int64_t> present_interval_ns{0}, st_defer_timed_out{0};
thread_local uint64_t tl_upload_frame = 0;
thread_local int64_t tl_upload_ns = 0;
thread_local bool upload_defer_ok = true;
struct Held { uint32_t w, h, layers; int nvn; };
struct { int64_t deferred = 0; std::unordered_map<uint32_t, Held> held; } burst;
std::mutex burst_mu;
int64_t now_ns = 123;
int64_t steady_ns() { return now_ns; }
static bool env_on(const char* n) { const char* e = std::getenv(n); return e && *e && std::atoi(e) != 0; }
int deadline_noted = 0;
void deadline_note_held(Texture&) { deadline_noted++; }   // (texture_deadline_run's queue)
CUT_DEFER
CUT_PCT
// (this thread spent `ms` in upload() this present, every thread `sum_ms` in all: is a new ASTC texture held back?)
bool held(double ms, double sum_ms, bool astc = true, bool uploaded = false) {
    Texture t;
    t.astc = astc, t.uploaded = uploaded;
    tl_upload_frame = frame_no, tl_upload_ns = (int64_t)(ms * 1e6);
    frame_upload_ns = (int64_t)(sum_ms * 1e6);
    return upload_defer(t);
}
int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "thread";
    int bad = 0;
    auto expect = [&](bool got, bool want, const char* what) {
        if (got != want) { std::printf("FAIL (%s): %s: %d, wanted %d\n", mode, what, got, want); bad++; }
    };
    if (std::string(mode) == "thread") {
        expect(held(31, 82), false, "the busiest thread at 31 ms of a 40 ms budget, 82 ms summed");
        expect(held(45, 45), true, "this thread at 45 ms");
        tl_upload_frame = frame_no - 1, tl_upload_ns = 90000000, frame_upload_ns = 0;   // (an earlier present's time)
        Texture t;
        expect(upload_defer(t), false, "this thread's time from an earlier present");
        bool other = true;   // (another thread's own count starts at nothing)
        std::thread([&] { Texture u; frame_upload_ns = 200000000; other = upload_defer(u); }).join();
        expect(other, false, "a thread that uploaded nothing this present, the sum at 200 ms");
        expect(held(45, 45, false), false, "an uncompressed texture");
        expect(held(45, 45, true, true), false, "an ASTC texture uploaded before");
        Texture w;
        tl_upload_frame = frame_no, tl_upload_ns = 50000000;
        upload_defer(w), upload_defer(w);   // (two lookups in one present: one texture, held back since this present)
        expect(w.defer_frame == frame_no && w.defer_asks == 2 && w.defer_ns == 123, true, "the held texture's age noted once");
        expect(st_deferred_tex_frame.load() >= 1 && burst.held.count(w.id) == 1, true, "the distinct textures counted");
        std::vector<uint32_t> v{9, 1, 5, 3, 7, 2, 8, 4, 6, 10};
        expect(defer_pct(v, 1.0) == 10 && defer_pct(v, 0.95) == 10 && defer_pct(v, 0.5) == 6, true, "the percentiles");
        std::vector<float> none;
        expect(defer_pct(none, 0.95) == 0.0f, true, "no waits: 0");
    } else if (std::string(mode) == "time") {   // (the release by time: RX_VK_UPLOAD_BUDGET_FRAC 0.25, RX_VK_UPLOAD_WAIT_MS 250)
        present_interval_ns = 16000000;
        expect(held(45, 45), true, "a 16 ms present: the 40 ms budget, 45 ms spent");
        present_interval_ns = 1000000000;
        expect(held(45, 45), false, "a present of 1 s: a quarter of it, 250 ms, is the budget");
        expect(held(300, 300), true, "a present of 1 s, 300 ms spent");
        present_interval_ns = 16000000;
        Texture t;
        tl_upload_frame = frame_no, tl_upload_ns = 50000000;
        now_ns = 1000;
        expect(upload_defer(t), true, "held back first");
        now_ns = 1000 + 249000000;
        expect(upload_defer(t), true, "held for 249 ms: still held");
        now_ns = 1000 + 250000000;
        expect(upload_defer(t), false, "held for 250 ms: let in over the budget");
        expect(st_defer_timed_out.load() == 1, true, "counted as let in by the wait");
    } else if (std::string(mode) == "time-off") {   // (RX_VK_UPLOAD_BUDGET_FRAC=0 RX_VK_UPLOAD_WAIT_MS=0: by presents alone)
        present_interval_ns = 1000000000;
        expect(held(45, 45), true, "a present of 1 s, the fraction off: the 40 ms budget");
        Texture t;
        tl_upload_frame = frame_no, tl_upload_ns = 50000000;
        now_ns = 1000;
        upload_defer(t);
        now_ns = 1000 + 10000000000LL;
        expect(upload_defer(t), true, "the wait off: still held after 10 s");
    } else if (std::string(mode) == "sum") {
        expect(held(31, 82), true, "RX_VK_UPLOAD_BUDGET_SUM=1: 82 ms summed");
        expect(held(31, 39), false, "RX_VK_UPLOAD_BUDGET_SUM=1: 39 ms summed");
    } else {
        expect(held(500, 500), false, "RX_VK_UPLOAD_BUDGET_MS=0");
    }
    if (!bad) std::printf("ok (%s)\n", mode);
    return bad != 0;
}
"""


class UploadBudget(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = TEX.read_text(encoding="utf-8")

    def test_the_budget_is_the_threads_own_unless_summed_is_asked(self):
        defer = block(self.tex, "bool upload_defer(Texture& tex)")
        self.assertIn('env_on("RX_VK_UPLOAD_BUDGET_SUM")', defer)
        self.assertIn("tl_upload_frame == frame_no ? tl_upload_ns : 0", defer)
        self.assertIn("frame_upload_ns.load(", defer)   # (the summed rule kept behind the switch)
        up = block(self.tex, "bool upload(Texture& tex, const Ranges* dirty)")
        self.assertIn("if (tl_upload_frame != frame_no) tl_upload_frame = frame_no, tl_upload_ns = 0;", up)
        self.assertIn("tl_upload_ns += ns", up)
        self.assertIn("frame_kind_ns[kind].fetch_add(ns", up)

    def test_a_held_texture_is_measured_until_it_comes_in_or_goes(self):
        touch = block(self.tex, "Texture* texture_touch(Texture* tex, bool for_sampling)")
        self.assertLess(touch.index("if (upload_defer(*tex)) return nullptr;"),
                        touch.index("if (tex->defer_frame) defer_note_done(*tex, true);"))
        self.assertIn("if (tex.defer_frame) defer_note_done(tex, false);", block(self.tex, "void destroy_later(Texture& tex)"))
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        self.assertLess(frame.index("defer_frame_end("), frame.index("st_deferred_frame.exchange(0"))
        end = block(self.tex, "void defer_frame_end(int64_t asks_now)")
        self.assertIn("vk: deferral done:", end)
        self.assertIn("frame_no - burst.last_frame < 600", end)   # (a run ends once all are in, or after 600 quiet presents)
        self.assertIn("if (burst.told++ < 40)", end)              # (quiet: a run's first 40 deferring presents only)

    def test_held_textures_are_released_by_time_too(self):
        defer = block(self.tex, "bool upload_defer(Texture& tex)")
        self.assertIn('"RX_VK_UPLOAD_BUDGET_FRAC"', defer)
        self.assertIn('"RX_VK_UPLOAD_WAIT_MS"', defer)
        self.assertIn("present_interval_ns.load(", defer)
        self.assertIn("steady_ns() - tex.defer_ns >= wait_ns", defer)
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        self.assertIn("present_interval_ns.store(now - last_ns", frame)

    def test_held_textures_are_released_at_a_recordings_start_by_their_deadline(self):
        # (the hold is noted and cancelled where it begins and ends; the service runs where texture_ahead_run prepares textures)
        defer = block(self.tex, "bool upload_defer(Texture& tex)")
        self.assertIn("deadline_note_held(tex);", defer)
        self.assertIn("deadline_held.erase(&tex);", block(self.tex, "void defer_note_done(Texture& tex, bool arrived)"))
        replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
        at = replay.index("texture_ahead_run();   // (hookshot, RX_VK_TEX_AHEAD")
        next_line = replay[replay.index("\n", at) + 1:].split("\n", 1)[0]
        self.assertTrue(next_line.strip().startswith("texture_deadline_run();"), next_line)   # (the same safe point, after it)
        self.assertIn("vk: upload deadline:", block(self.tex, "void texture_frame(uint64_t completed)"))
        cxx = shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            self.skipTest("no C++ compiler")
        run = block(self.tex, "void texture_deadline_run()")
        code = r"""
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
struct Texture { bool uploaded = false; uint64_t defer_frame = 1; int64_t defer_ns = 0; uint64_t bytes = 8 << 20; };
namespace texture_ahead { constexpr uint64_t kReserveBytes = 256ull << 20; }
uint64_t total_bytes = 1ull << 30, ahead_budget_bytes = 4ull << 30, destroy_gen = 1;
int bumps = 0;
void destroy_gen_bump(const Texture*) { bumps++; }
thread_local bool upload_defer_ok = true;
int64_t now_ns = 0, upload_ns = 1000000;   // (each upload takes 1 ms of the clock)
int64_t steady_ns() { return now_ns; }
std::mutex deadline_mu;
std::unordered_map<Texture*, int64_t> deadline_held;
struct DeadlineCounts { int64_t released, bytes, ns, over_ceiling, no_room, services, holds_ended, held_ns, held_max_ns; } deadline_n{};
int touched = 0;
Texture* texture_touch(Texture* tex, bool) {   // (as the real one: the hold ends, and it is uploaded)
    if (upload_defer_ok) std::abort();       // (the service uploads over the budget: never held again)
    deadline_held.erase(tex), tex->defer_frame = 0, tex->uploaded = true, total_bytes += tex->bytes, now_ns += upload_ns, touched++;
    return tex;
}
CUT_RUN
int main() {
    int bad = 0;
    auto expect = [&](long long got, long long want, const char* what) {
        if (got != want) { std::printf("FAIL: %s: %lld, wanted %lld\n", what, got, want); bad++; }
    };
    std::vector<Texture> t(20);
    for (int i = 0; i < 20; i++) t[i].defer_ns = i * 10000000LL, deadline_held[&t[i]] = t[i].defer_ns;   // (held 10 ms apart)
    now_ns = 200000000;   // (200 ms: none is 250 ms old)
    texture_deadline_run();
    expect(touched, 0, "none due before 250 ms");
    now_ns = 300000000;   // (300 ms: the first six are due, oldest first; 4 ms of uploading a recording)
    texture_deadline_run();
    expect(touched, 4, "the time ceiling");
    expect(t[0].uploaded && t[3].uploaded && !t[4].uploaded, 1, "oldest first");
    expect(deadline_n.over_ceiling, 2, "left for the ceiling");
    expect(bumps, 1, "the placeholders go stale");
    expect(upload_defer_ok, 1, "the lookup's rule as it was");
    upload_ns = 0, now_ns = 1000000000;   // (1 s: the rest due; 64 MB a recording: 8 of 8 MB)
    texture_deadline_run();
    expect(touched, 12, "the bytes ceiling");
    ahead_budget_bytes = total_bytes - (256ull << 20) + (8ull << 20) - 1;   // (one more would pass 256 MB over the budget)
    texture_deadline_run();
    expect(touched, 12, "no room in the cache");
    expect(deadline_n.no_room, 8, "left for the room");
    ahead_budget_bytes = total_bytes;   // (at the budget: 256 MB over it allowed, as a lookup would have gone over anyway)
    deadline_held.erase(&t[19]);   // (gone, or in at a lookup, meanwhile)
    texture_deadline_run();
    expect(touched, 19, "the rest, not the one gone");
    expect(!t[19].uploaded, 1, "the one gone untouched");
    expect(deadline_n.released, 19, "counted");
    return bad ? 1 : 0;
}
""".replace("CUT_RUN", run)
        with tempfile.TemporaryDirectory(prefix="upload-deadline-") as tmp:
            src, exe = Path(tmp) / "f.cpp", Path(tmp) / ("f.exe" if os.name == "nt" else "f")
            src.write_text(code, encoding="utf-8")
            built = subprocess.run([cxx, "-std=c++20", "-O0", "-Wall", "-Wextra", "-Werror", "-pthread", str(src), "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-3000:])
            env = {k: v for k, v in os.environ.items() if not k.startswith(("RX_VK_UPLOAD_",))}
            r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_the_switches_are_documented(self):
        doc = DOC.read_text(encoding="utf-8")
        self.assertIn("RX_VK_UPLOAD_BUDGET_MS", doc)
        self.assertIn("RX_VK_UPLOAD_BUDGET_SUM=1", doc)
        self.assertIn("RX_VK_UPLOAD_BUDGET_FRAC", doc)
        self.assertIn("RX_VK_UPLOAD_WAIT_MS", doc)
        self.assertIn("RX_VK_UPLOAD_DEADLINE_MS", doc)
        self.assertIn("RX_VK_UPLOAD_DEADLINE_MB", doc)

    def test_the_rule_runs(self):
        cxx = shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            self.skipTest("no C++ compiler")
        code = FIXTURE.replace("CUT_DEFER", block(self.tex, "bool upload_defer(Texture& tex)"))
        code = code.replace("CUT_PCT", block(self.tex, "template <class T> T defer_pct(std::vector<T>& v, double q)"))
        code = code.replace("#include <vector>", "#include <vector>\n#include <string>")
        with tempfile.TemporaryDirectory(prefix="upload-budget-") as tmp:
            src, exe = Path(tmp) / "f.cpp", Path(tmp) / ("f.exe" if os.name == "nt" else "f")
            src.write_text(code, encoding="utf-8")
            built = subprocess.run([cxx, "-std=c++20", "-D_CRT_SECURE_NO_WARNINGS", "-O0", "-Wall", "-Wextra", "-Werror",
                                    "-pthread", str(src), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, "the code cut out of vk_texture.cpp does not build in the fixture: " +
                             built.stderr[-3000:])
            env = {k: v for k, v in os.environ.items() if not k.startswith(("RX_VK_UPLOAD_BUDGET", "RX_VK_UPLOAD_WAIT"))}
            env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
            # (the cases are written for a 40 ms budget: main's default is 12, so they set it)
            for mode, extra in (("thread", {"RX_VK_UPLOAD_BUDGET_MS": "40"}),
                                ("sum", {"RX_VK_UPLOAD_BUDGET_SUM": "1", "RX_VK_UPLOAD_BUDGET_MS": "40"}),
                                ("time", {"RX_VK_UPLOAD_BUDGET_MS": "40"}),
                                ("time-off", {"RX_VK_UPLOAD_BUDGET_MS": "40", "RX_VK_UPLOAD_BUDGET_FRAC": "0",
                                              "RX_VK_UPLOAD_WAIT_MS": "0"}),
                                ("off", {"RX_VK_UPLOAD_BUDGET_MS": "0"})):
                r = subprocess.run([str(exe), mode], env={**env, **extra}, capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
