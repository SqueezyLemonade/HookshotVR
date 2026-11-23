"""The HookshotVR downloads' source (scripts/hookshotvr_release.py source) leaves out what only developers and CI use
(DEV_ONLY: the agents' skills in .claude/, .github/'s CI, AGENTS.md, the release automation's config), and the builder parts
keep their packages' files but not their CI (bundled: the Vulkan headers' .github/). The first archives carried .claude/ and
.github/.

Exports HEAD the way a release does (git archive, into a temporary folder) and checks: no DEV_ONLY path in it, each DEV_ONLY
entry still names something tracked (a renamed folder would slip through), the build's entry points and the licences are
there, and nothing the build runs (nx2/, host/, gpu/, native/, vr/ but its dev tools, the launcher) opens a DEV_ONLY path.

    python -I -B tests/test_hookshotvr_release.py
"""
import importlib.util
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEEDED = ("nx2/make_game.py", "nx2/build.py", "vr/vk_vr.h", "vr/launcher/src/main.cpp", "host/hos_main.c", "LICENSE", "NOTICE",
          "readme.md", "assets/vr/panel_frame.png", "VERSION")


def release():
    spec = importlib.util.spec_from_file_location("hookshotvr_release", os.path.join(ROOT, "scripts", "hookshotvr_release.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def tracked():
    r = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True, stdout=subprocess.PIPE)
    return [f for f in r.stdout.decode("utf-8").split("\0") if f]


def main():
    m = release()
    bad = []
    files = tracked()
    for d in m.DEV_ONLY:
        if not any(m.dev_only(f) and (f == d or f.startswith(d)) for f in files):
            bad.append("DEV_ONLY names %s, which nothing tracked is: renamed? (then name it again)" % d)
    # (the builder parts' packages: their own CI left out, their headers kept)
    for rel, want in (("vulkanheaders/v1.4.345/.github/workflows/ci.yml", False), ("vulkanheaders/v1.4.345/include/vulkan/vulkan.h", True),
                      ("bin/clang.exe", True), (".github", True)):
        if m.bundled(rel) != want:
            bad.append("bundled(%r) is %s" % (rel, not want))
    for need in (".claude/", ".github/", "AGENTS.md"):
        if need not in m.DEV_ONLY:
            bad.append("DEV_ONLY no longer has %s" % need)
    # (the build's code must not read them: a player's source has none)
    pattern = re.compile(r"""["'/](\.claude|\.github)/|AGENTS\.md|release-please""")
    for f in files:
        top = f.split("/")[0]
        if top not in ("nx2", "host", "gpu", "native", "vr") or f.startswith(("vr/tools/", "vr/devmap/")) or not f.endswith(
                (".py", ".c", ".cpp", ".h", ".inc", ".sh")):
            continue
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                if pattern.search(line) and "://" not in line:   # (a web link names .github/ISSUE_TEMPLATE: not a read)
                    bad.append("%s:%d reads a developer-only path: %s" % (f, n, line.strip()[:100]))
    with tempfile.TemporaryDirectory() as t:
        m.source(t, "HEAD", "Alpha v0.0.0")
        got = []
        for d, ds, fs in os.walk(t):
            got += [os.path.relpath(os.path.join(d, x), t).replace(os.sep, "/") for x in fs]
        for f in got:
            if m.dev_only(f):
                bad.append("in the export: %s" % f)
        for f in NEEDED:
            if f not in got:
                bad.append("not in the export: %s" % f)
        extra = sorted(set(got) - set(files) - {"VERSION"})
        if extra:
            bad.append("in the export but not tracked: %s" % extra[:5])
    for b in bad:
        print("FAIL: " + b)
    print("hookshotvr release source: %s (%d files tracked, %d developer-only left out)"
          % ("FAILED" if bad else "ok", len(files), sum(1 for f in files if m.dev_only(f))))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
