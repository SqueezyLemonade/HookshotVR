"""(Hookshot VR) A pipeline note whose record does not make its key is not asked of the caches (vk_draw.cpp pipeline_from_caches;
no game/GPU).

A note holds no VR target fields, so a two-eye note rebuilt from its record is the one-view pipeline: asked of the caches by
the load at a program's create call or the start pass, one found was kept under the two-eye key, and the two-eye draw took a
single-view pipeline. pipeline_from_caches is cut out of vk_draw.cpp and built against stand-ins: a note whose recomputed key
is its own is asked (build_graphics called, as a probe); one whose key differs is not, and is counted.

python -I -B tests/test_stereo_notes.py
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <atomic>
#include <cstdint>
#include <cstdio>
typedef void* VkPipeline;
#define VK_NULL_HANDLE nullptr
enum VkPrimitiveTopology { kTri = 3 };
struct Program {};
struct NvnState {};
struct Targets { int eyes = 0; };
struct PipeNote { uint64_t key = 0; uint32_t topo = kTri; NvnState s; };
bool ask_start_first = false;
int asked, logged;
uint64_t one_view_key = 0x1111;   // (what a note's record makes: its one-view pipeline's key)
Targets note_targets(const PipeNote&) { return Targets{}; }
uint64_t pipeline_key(const Program&, const NvnState&, const Targets& at, VkPrimitiveTopology) { return at.eyes ? 0x2222 : one_view_key; }
VkPipeline build_graphics(const Program&, const NvnState&, const Targets&, VkPrimitiveTopology, int part, bool* cold) {
    ++asked;
    if (part != -1 || !cold || !ask_start_first) std::printf("FAIL the probe is not a whole pipeline asked of the start's cache\n");
    static int pl;
    return &pl;
}
void logf(const char*, ...) { ++logged; }
@FN@
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) ++failures, std::printf("FAIL %s\n", what); };
    Program p;
    bool cold = true;
    PipeNote mono;
    mono.key = one_view_key;
    check(pipeline_from_caches(p, mono, &cold) != nullptr && asked == 1, "a one-view note is asked of the caches");
    check(!cold, "cold starts false");
    check(!ask_start_first, "the start's cache asked first only during the probe");
    PipeNote two_eye;
    two_eye.key = 0x2222;   // (written by the two-eye draw: its record makes 0x1111)
    cold = true;
    check(pipeline_from_caches(p, two_eye, &cold) == nullptr, "a two-eye note gets no pipeline");
    check(asked == 1, "a two-eye note is not asked of the caches (it would be its one-view pipeline)");
    check(!cold, "and is not reported as compiled-required");
    check(notes_not_reproduced.load() == 1 && notes_reproduced.load() == 1 && logged == 1, "counted and logged");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
"""


def between(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i) + len(end)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    src = (args.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    try:
        fn = between(src, "VkPipeline pipeline_from_caches(const Program& p, const PipeNote& n, bool* cold) {", "\n}\n")
    except ValueError:
        raise SystemExit("pipeline_from_caches changed; update this test's extraction anchors") from None
    # (the counters are declared beside the function in vk_draw.cpp; a source from before the fix has none: they stand in, so
    # its cases say what fails)
    counters = "std::atomic<int64_t> notes_not_reproduced{0}, notes_reproduced{0};\n"
    with tempfile.TemporaryDirectory(prefix="stereo-notes-") as tmp:
        cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
        cpp.write_text(FIXTURE.replace("@FN@", counters + fn), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter", str(cpp), "-o", str(exe)],
                       check=True, timeout=60)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        print(r.stdout, end="")
        if r.returncode:
            raise SystemExit(1)
    print("PASS: a pipeline note whose record does not make its key (a two-eye or HUD note) is not asked of the caches")


if __name__ == "__main__":
    main()
