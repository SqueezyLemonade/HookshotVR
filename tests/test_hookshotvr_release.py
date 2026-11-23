"""The HookshotVR downloads' source (scripts/hookshotvr_release.py source) leaves out what only developers and CI use
(DEV_ONLY: the agents' skills in .claude/, .github/'s CI, AGENTS.md, the release automation's config), and the builder parts
keep their packages' files but not their CI (bundled: the Vulkan headers' .github/). The first archives carried .claude/ and
.github/.

Exports HEAD the way a release does (git archive, into a temporary folder) and checks: no DEV_ONLY path in it, each DEV_ONLY
entry still names something tracked (a renamed folder would slip through), the build's entry points and the licences are
there, and nothing the build runs (nx2/, host/, gpu/, native/, vr/ but its dev tools, the launcher) opens a DEV_ONLY path.

The game stamp (game_stamp: what the launcher's update compares to build the game again or not): the same for a change in
the launcher, the docs, the tests or VERSION, another for a change in the build's code or another builder; nothing the build
runs opens a NOT_GAME path; and SHA256SUMS.txt / GAME_STAMPS.txt keep one line a file when a platform is made again.

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
    for d in m.NOT_GAME:
        if d not in ("VERSION", "GAME_STAMP") and not any(m.not_game(f) and (f == d or f.startswith(d)) for f in files):
            bad.append("NOT_GAME names %s, which nothing tracked is: renamed? (then name it again)" % d)
    reads = re.compile(r"""join\((ROOT|root|REPO|TOOLS)\s*,\s*["'](docs|tests|website)["']|["']vr/launcher/|["']vr["']\s*,\s*["']launcher["']""")
    for f in files:
        top = f.split("/")[0]
        if top not in ("nx2", "host", "gpu", "native", "vr") or f.startswith(("vr/tools/", "vr/devmap/", "vr/launcher/")) or not f.endswith(
                (".py", ".c", ".cpp", ".h", ".inc", ".sh")):
            continue
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                code = line.split("#", 1)[0] if f.endswith((".py", ".sh")) else line.split("//", 1)[0]
                if reads.search(code):
                    bad.append("%s:%d reads a path the game stamp leaves out (NOT_GAME): %s" % (f, n, line.strip()[:100]))
    with tempfile.TemporaryDirectory() as t:
        m.source(t, "HEAD", "Alpha v0.0.0")
        stamp = m.game_stamp(t, "tag v2.1.0")
        with open(os.path.join(t, "GAME_STAMP"), encoding="utf-8") as fh:
            if fh.read() != stamp + "\n":
                bad.append("GAME_STAMP does not hold the stamp")
        if m.game_stamp(t, "tag v2.1.0") != stamp:
            bad.append("the game stamp changed with nothing changed (GAME_STAMP itself is in it?)")

        def edit(rel, same):
            p = os.path.join(t, rel)
            with open(p, "ab") as fh:
                fh.write(b"\n# changed\n")
            got = m.game_stamp(t, "tag v2.1.0")
            if (got == stamp) != same:
                bad.append("a change in %s %s the game stamp" % (rel, "changed" if same else "did not change"))
            return got
        for rel in ("vr/launcher/src/main.cpp", "vr/launcher/CHANGELOG.txt", "docs/vr-troubleshooting.md", "VERSION", "readme.md"):
            edit(rel, True)
        if m.game_stamp(t, "tag v2.2.0") == stamp:
            bad.append("another builder did not change the game stamp")
        stamp = edit("nx2/build.py", False)
        edit("vr/body/" + sorted(os.listdir(os.path.join(t, "vr", "body")))[0], False)
        os.remove(os.path.join(t, "GAME_STAMP"))
        got = []
        for d, ds, fs in os.walk(t):
            got += [os.path.relpath(os.path.join(d, x), t).replace(os.sep, "/") for x in fs]
        for f in got:
            if m.dev_only(f):
                bad.append("in the export: %s" % f)
        for f in NEEDED:
            if f not in got:
                bad.append("not in the export: %s" % f)
        extra = sorted(set(got) - set(files) - {"VERSION", "GAME_STAMP"})
        if extra:
            bad.append("in the export but not tracked: %s" % extra[:5])
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "SHA256SUMS.txt")
        m.listing(p, {"a.zip": "1" * 64, "b.tar.gz": "2" * 64})
        m.listing(p, {"a.zip": "3" * 64})
        with open(p, encoding="utf-8") as fh:
            if fh.read() != "%s  b.tar.gz\n%s  a.zip\n" % ("2" * 64, "3" * 64):
                bad.append("listing() did not replace a.zip's line and keep b.tar.gz's")
    for b in bad:
        print("FAIL: " + b)
    print("hookshotvr release source: %s (%d files tracked, %d developer-only left out)"
          % ("FAILED" if bad else "ok", len(files), sum(1 for f in files if m.dev_only(f))))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
