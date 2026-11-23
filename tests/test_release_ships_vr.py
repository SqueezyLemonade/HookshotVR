"""The builders ship what a player's build needs of Hookshot VR (scripts/release_parts.py vr_files): every vr/ file host/
includes (host/hos_main.c includes vr/**/*_host.c and vr/devmap/devmap_host.c), every .cpp nx2/build.py compiles into the
Vulkan back end (vr_sources: vr/ but vr/tools and vr/launcher), vr/tools/generate (the headers made from the player's game
files) and the menu's frame (assets/vr/panel_frame.png), and none of vr/generated. The Windows package took no vr/ at all,
so its first build would have stopped at the first VR include; the AppImage copied the working tree's vr/, untracked files
and all. Both now take the same tracked list.

    python -I -B tests/test_release_ships_vr.py
"""
import importlib.util
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def parts():
    spec = importlib.util.spec_from_file_location("release_parts", os.path.join(ROOT, "scripts", "release_parts.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def tracked(*paths):
    r = subprocess.run(["git", "ls-files", "-z", "--"] + list(paths), cwd=ROOT, check=True, stdout=subprocess.PIPE)
    return [f for f in r.stdout.decode("utf-8").split("\0") if f]


def main():
    bad = []
    shipped = set(parts().vr_files())
    if not shipped:
        bad.append("vr_files() is empty (no git checkout?)")
    want = set()
    for f in tracked("host"):
        if f.endswith((".c", ".h")):
            for inc in re.findall(r'#include\s+"\.\./(vr/[^"]+)"', read(f)):
                want.add(inc)
    if len(want) < 10:
        bad.append("only %d vr/ files included by host/: the pattern no longer matches" % len(want))
    for f in tracked("vr"):
        top = f.split("/")[1] if f.count("/") > 1 else ""
        if f.endswith(".cpp") and top not in ("tools", "launcher"):
            want.add(f)   # (nx2/build.py vr_sources)
        if f.startswith("vr/tools/generate/") and f.endswith(".py"):
            want.add(f)
    want.add("assets/vr/panel_frame.png")
    for f in sorted(want - shipped):
        bad.append("not shipped: %s" % f)
    for f in sorted(shipped):
        if f.startswith(("vr/generated/", "vr/launcher/")) or (f.startswith("vr/tools/") and not f.startswith("vr/tools/generate/")):
            bad.append("shipped, but not the build's: %s" % f)
    if not re.search(r'^\s+for f in tracked\([^)]*\) \+ vr_files\(\):', read("scripts/release_parts.py"), re.M):
        bad.append("scripts/release_parts.py package() does not copy vr_files()")
    app = read("scripts/linux/appimage.sh")
    if "release_parts.py vr-files" not in app or re.search(r"^find vr\b", app, re.M):
        bad.append("scripts/linux/appimage.sh does not copy the vr-files list (or still copies the working tree's vr/)")
    for b in bad:
        print("FAIL: " + b)
    print("release ships vr: %s (%d files)" % ("FAILED" if bad else "ok", len(shipped)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
