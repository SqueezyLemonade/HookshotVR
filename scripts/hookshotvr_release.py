"""The HookshotVR downloads: one folder a player unpacks and starts the launcher from, for Windows (a .zip) and Linux (a
.tar.gz). No game data and nothing made from it is in either: the launcher builds the game from the player's own game file.

    python3 -I scripts/hookshotvr_release.py <out folder> --version "Alpha v1.0.0" [--commit HEAD]
        [--linux-launcher <vr/launcher/build>] [--linux-builder <an unpacked Hookshot Linux builder (AppImage tree)>]
        [--windows-launcher <vr/launcher/build/windows>] [--windows-parts <folder>] [--keep-staging]

Each platform is made when its two folders are given. What each holds (the launcher finds them beside itself:
vr/launcher/src/system.cpp find_hookshot, find_repo):

    HookshotVR/
      hookshotvr-launcher[.exe], fonts/, art/, CHANGELOG.txt   the launcher (vr/launcher/build.sh, build-windows.sh)
      README.txt                                               how to start (written here)
      HookshotVR/        the source the game is built from: the commit's tracked files (git archive, so never a working
                         tree's untracked files) less DEV_ONLY (the agents' skills, CI), a VERSION file (nx2/version.py;
                         there is no .git) and a GAME_STAMP file (game_stamp: what the launcher's update compares)
      builder/           Hookshot's builder, as Hookshot ships it: what the source builds with
    Windows also:
      SDL2.dll, SDL3.dll                    the launcher's window (Hookshot's sdl2-compat SDL2.dll on SDL3)
      openxr_loader.dll                     Khronos' OpenXR loader (Windows has none); the launcher copies it beside the
                                            game (platform_win32.cpp prepare_game)
      licenses/                             the OpenXR loader's and SDL's licences
      builder/tools/python, cpm, llvm-mingw Hookshot's Windows builder parts (its setup's own: python, cpm) and llvm-mingw's
                                            official Windows release, laid out as Hookshot's setup installs them
      HookshotVR/third_party/...            the prebuilt files of Hookshot's third_party part the repository does not carry
                                            (SDL2.dll, SDL3.dll beside the game; a64emit, zbic: rebuilt when their sources
                                            changed), as scripts/release_parts.py package takes them

--windows-parts is a folder holding Hookshot's builder parts list (hookshot-setup.txt), its python, cpm and third_party
zips (HookshotBuilder-<part>-<sha>.zip), llvm-mingw's Windows zip and Khronos' openxr_loader_windows-<version>.zip; each
zip of the list is checked against the list's size and SHA-256, llvm-mingw against scripts/make_setup_parts.py LLVM.

Staging goes into <out folder>/staging (on disk, not a RAM folder: the Linux builder alone is ~900 MB). Writes
HookshotVR-<version>-windows-x64.zip, HookshotVR-<version>-linux-x86_64.tar.gz, SHA256SUMS.txt and GAME_STAMPS.txt into
<out folder>: all four are the GitHub release's files (the launcher's update reads SHA256SUMS.txt to check the archive it
downloads, GAME_STAMPS.txt to say before it whether the game will be built again: vr/launcher/src/update.h).
"""
import argparse
import hashlib
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = "HookshotVR"


def module(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, rel))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


release_parts = module("release_parts", "scripts/release_parts.py")
LLVM = module("make_setup_parts", "scripts/make_setup_parts.py").LLVM   # (sha256, size, top folder, url)


def slug(version):
    return re.sub(r"[^A-Za-z0-9.]+", "-", version).strip("-")   # ("Alpha v1.0.0" -> Alpha-v1.0.0)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), check=True, stdout=subprocess.PIPE).stdout


# What a player's download leaves out of the source: what only the repository's developers and its CI use, never the build
# (tests/test_hookshotvr_release.py checks the build reads none of it, and that none of it reaches an archive). Paths as git
# pathspecs, a folder with its trailing slash
DEV_ONLY = (
    ".claude/",                       # the agents' skills
    ".github/",                       # CI workflows, issue templates, dependabot
    "AGENTS.md",                      # notes for AI coding agents
    "release-please-config.json",     # the release automation's (CI)
    ".release-please-manifest.json",
)


def dev_only(rel):
    return any(rel == d or (d.endswith("/") and rel.startswith(d)) for d in DEV_ONLY)


# (and in what the archives carry of others: the builder parts' packages keep their own CI, e.g. the Vulkan headers' .github/)
DEV_ONLY_DIRS = (".github",)


def bundled(rel):
    """a builder part's file, rel to the part's folder: shipped unless it sits in a DEV_ONLY_DIRS folder"""
    return not any(seg in DEV_ONLY_DIRS for seg in rel.split("/")[:-1])


def source(dest, commit, version):
    """The commit's tracked files (git archive), less DEV_ONLY, into dest, and its VERSION"""
    data = git("archive", "--format=tar", commit, "--", ".", *(":(exclude)" + d.rstrip("/") for d in DEV_ONLY))
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        left = [m.name for m in t.getmembers() if dev_only(m.name)]
        if left:
            sys.exit("git archive kept developer-only files: %s" % left[:5])
        t.extractall(dest, filter="tar")
    describe = git("describe", "--tags", "--long", "--always", "--match", "v[0-9]*", commit).decode().strip()
    with open(os.path.join(dest, "VERSION"), "w", encoding="utf-8") as f:
        f.write("hookshotvr-%s-%s\n" % (slug(version).lower(), describe.rsplit("-", 1)[-1]))
    return describe


# What the game's build does not read: a release that changes only these is launcher-only, and the launcher's update keeps
# the game folder as it is (vr/launcher/src/update.h). Everything else in the source is the game's. Paths as in dev_only().
# VERSION is left out on purpose: the build writes it into the game (nx2/version.py), but it names each release, so a
# launcher-only release would build the game again for its name alone; the game keeps the name of the release it was built
# from. (tests/test_hookshotvr_release.py: nothing the build runs opens one of these)
NOT_GAME = (
    "vr/launcher/",        # the launcher: a program of its own (nx2/build.py leaves it out)
    "docs/", "tests/", "website/",
    "readme.md", "CHANGELOG.md", "changelog.txt", "CONTRIBUTING.md", "START HERE.md",
    "VERSION", "GAME_STAMP",
)
NOT_GAME_TOP = re.compile(r"release-notes-[^/]*\.md$")


def not_game(rel):
    return any(rel == d or (d.endswith("/") and rel.startswith(d)) for d in NOT_GAME) or bool(NOT_GAME_TOP.match(rel))


def game_stamp(src, builder_tag):
    """A hash of everything in the staged source the build reads (its path and bytes, all but NOT_GAME), and the builder's
    tag (another Hookshot builder makes another game): the same stamp, the same game. Written into the source as GAME_STAMP"""
    h = hashlib.sha256(b"hookshotvr-game-stamp 1\0builder %s\0" % builder_tag.encode("utf-8"))
    files = []
    for d, ds, fs in os.walk(src):
        ds.sort()
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), src).replace(os.sep, "/")
            if not not_game(rel):
                files.append(rel)
    for rel in sorted(files):
        p = os.path.join(src, rel)
        h.update(rel.encode("utf-8") + b"\0")
        if os.path.islink(p):
            h.update(b"link " + os.readlink(p).encode("utf-8") + b"\0")
        else:
            with open(p, "rb") as f:
                h.update(hashlib.sha256(f.read()).digest())
    stamp = h.hexdigest()
    with open(os.path.join(src, "GAME_STAMP"), "w", encoding="utf-8", newline="\n") as f:
        f.write(stamp + "\n")
    return stamp


def launcher(src, dest, exe):
    if not os.path.isfile(os.path.join(src, exe)):
        sys.exit("%s: no %s (build it first)" % (src, exe))
    os.makedirs(dest, exist_ok=True)
    shutil.copy2(os.path.join(src, exe), dest)
    for d in ("fonts", "art"):
        shutil.copytree(os.path.join(src, d), os.path.join(dest, d), dirs_exist_ok=True)
    shutil.copy2(os.path.join(src, "CHANGELOG.txt"), dest)


README = {
    "windows": """HookshotVR {version} for Windows (64-bit)

1. Unpack this whole folder somewhere with 60 GB free (not in Program Files). The game is built into it.
2. Start hookshotvr-launcher.exe.
3. Choose your own game file (the .nup or .nx Hookshot builds from) under Set up, then Build. The first build takes a long time (an hour or
   more: it translates and compiles the game, then prepares its shaders). Nothing of the game is in this download.
4. Play: Hookshot (Desktop Mode) or HookshotVR. For VR, start your OpenXR runtime first (SteamVR, the Meta Quest Link
   app, Virtual Desktop) and make it the active OpenXR runtime; the launcher shows it and the headset, and waits for both.

VR needs an OpenXR runtime with Vulkan support (SteamVR, Meta Quest Link, Virtual Desktop). Windows Mixed Reality's own
runtime has none: use SteamVR for those headsets. Settings, saves and logs: %LOCALAPPDATA%\\HookshotVR and
%APPDATA%\\Hookshot. "Create shortcuts": run hookshotvr-launcher.exe --install once.

Source: HookshotVR\\ (the code this download builds, {describe}). builder\\ is Hookshot's Windows builder (its Python and
packages, and llvm-mingw's official release). Licences: HookshotVR\\LICENSE, licenses\\, builder\\tools\\*\\LICENSE*.
""",
    "linux": """HookshotVR {version} for Linux (x86-64)

1. Unpack this whole folder somewhere with 60 GB free: tar xzf HookshotVR-{slug}-linux-x86_64.tar.gz
   The game is built into it.
2. Start HookshotVR/hookshotvr-launcher (or run it once with --install for an app menu and desktop entry).
3. Choose your own game file (the .nup or .nx Hookshot builds from) under Set up, then Build. The first build takes a long time (an hour or
   more: it translates and compiles the game, then prepares its shaders). Nothing of the game is in this download.
4. Play: Hookshot (Desktop Mode) or HookshotVR. For VR, start your OpenXR runtime first (WiVRn, Monado, SteamVR); the
   launcher shows it and the headset, and waits for both.

Needs: glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 and later), SDL2 is carried, libpng16 and OpenGL 3.3 for
the launcher's window, and for VR the system's OpenXR loader (libopenxr_loader.so.1: Debian/Ubuntu/Mint
"libopenxr-loader1", Fedora "openxr-libs", Arch "openxr"). Settings and logs: ~/.config/HookshotVR,
~/.local/share/HookshotVR; saves ~/.local/share/hookshot.

Source: HookshotVR/ (the code this download builds, {describe}). builder/ is Hookshot's Linux builder (its unpacked
AppImage: Python, clang, the sysroot, FFmpeg, SDL2). Licences: HookshotVR/LICENSE, builder/usr/share/hookshot/licenses.
""",
}


def readme(dest, plat, version, describe):
    text = README[plat].format(version=version, slug=slug(version), describe=describe)
    with open(os.path.join(dest, "README.txt"), "w", encoding="utf-8", newline="\r\n" if plat == "windows" else "\n") as f:
        f.write(text)


# ---------------- Windows ----------------

def list_parts(path):
    """{part name: (sha256, size, file name)} of a Hookshot builder parts list (hookshot-setup.txt)"""
    out = {}
    for w in release_parts.part_lines(path):
        name = w[6].rsplit("/", 1)[-1]
        m = re.match(r"HookshotBuilder-(.+)-[0-9a-f]{12}\.zip$", name)
        if m:
            out[m.group(1)] = (w[1], int(w[2]), name)
    return out


def checked(folder, name, sha, size):
    p = os.path.join(folder, name)
    if not os.path.isfile(p) or os.path.getsize(p) != size or sha256(p) != sha:
        sys.exit("%s: missing, or not the size and SHA-256 its list gives" % p)
    return p


def windows(a, stage, describe):
    top = os.path.join(stage, TOP)
    launcher(a.windows_launcher, top, "hookshotvr-launcher.exe")
    src = os.path.join(top, TOP)
    setup = os.path.join(a.windows_parts, "hookshot-setup.txt")
    parts = list_parts(setup)
    tag = open(setup, encoding="utf-8").readline().strip()
    builder_tools = os.path.join(top, "builder", "tools")
    for part in ("python", "cpm"):
        sha, size, name = parts[part]
        n = release_parts.extract(checked(a.windows_parts, name, sha, size), "HookshotBuilder/tools/%s/" % part,
                                  os.path.join(builder_tools, part), keep=bundled)
        print("  builder/tools/%s: %d files (%s)" % (part, n, name))
    # (Hookshot's setup unpacks llvm-mingw and renames its folder to llvm-mingw: builder/setup/setup.c)
    sha, size, folder, url = LLVM
    z = checked(a.windows_parts, url.rsplit("/", 1)[-1], sha, size)
    n = release_parts.extract(z, folder + "/", os.path.join(builder_tools, "llvm-mingw"), keep=bundled)
    print("  builder/tools/llvm-mingw: %d files (%s)" % (n, os.path.basename(z)))
    with open(os.path.join(top, "builder", "hookshot-windows.txt"), "w", encoding="utf-8") as f:
        f.write("%s\n" % tag)   # (the launcher reads the builder's tag from it: system.cpp builder_tag)
    # (the third_party part's prebuilt files into the source, as release_parts.py package puts them into tools/)
    sha, size, name = parts["third_party"]
    known = set(release_parts.tracked("third_party"))
    prebuilt = lambda rel: rel.startswith("third_party/") and rel not in known and rel.lower().endswith(release_parts.PREBUILT_EXT)
    n = release_parts.extract(checked(a.windows_parts, name, sha, size), "HookshotBuilder/tools/", src, keep=prebuilt)
    print("  HookshotVR/third_party: %d prebuilt files (%s)" % (n, name))
    for dll in ("SDL2.dll", "SDL3.dll"):
        p = os.path.join(src, "third_party", "sdl2", dll)
        if not os.path.isfile(p):
            sys.exit("the third_party part has no third_party/sdl2/%s" % dll)
        shutil.copy2(p, top)
    lic = os.path.join(top, "licenses")
    os.makedirs(os.path.join(lic, "SDL"), exist_ok=True)
    shutil.copy2(os.path.join(src, "third_party", "sdl2", "LICENSE.txt"), os.path.join(lic, "SDL", "LICENSE.txt"))
    loaders = sorted(f for f in os.listdir(a.windows_parts) if re.match(r"openxr_loader_windows-[0-9.]+\.zip$", f))
    if not loaders:
        sys.exit("%s: no openxr_loader_windows-<version>.zip" % a.windows_parts)
    with zipfile.ZipFile(os.path.join(a.windows_parts, loaders[-1])) as zf:
        names = zf.namelist()
        dll = [n for n in names if n.lower().endswith("x64/bin/openxr_loader.dll")]
        lics = [n for n in names if re.search(r"(^|/)(LICENSE|COPYING)[^/]*$", n, re.I)]   # (share/doc/openxr/LICENSE)
        if len(dll) != 1:
            sys.exit("%s: not one x64 openxr_loader.dll (%s)" % (loaders[-1], dll))
        with zf.open(dll[0]) as s, open(os.path.join(top, "openxr_loader.dll"), "wb") as d:
            shutil.copyfileobj(s, d)
        os.makedirs(os.path.join(lic, "OpenXR-loader"), exist_ok=True)
        for n in lics:
            with zf.open(n) as s, open(os.path.join(lic, "OpenXR-loader", os.path.basename(n)), "wb") as d:
                shutil.copyfileobj(s, d)
        if not lics:   # (Apache-2.0: shipped with its licence or not at all)
            sys.exit("%s: no LICENSE in it" % loaders[-1])
    print("  openxr_loader.dll from %s" % loaders[-1])
    a.stamp = game_stamp(src, tag)
    print("  game stamp %s (builder %s)" % (a.stamp[:16], tag))
    readme(top, "windows", a.version, describe)
    out = os.path.join(a.out, "HookshotVR-%s-windows-x64.zip" % slug(a.version))
    with zipfile.ZipFile(out + ".new", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for d, ds, fs in os.walk(top):
            ds.sort()
            for f in sorted(fs):
                p = os.path.join(d, f)
                zf.write(p, os.path.relpath(p, stage).replace(os.sep, "/"))
    os.replace(out + ".new", out)
    return out


# ---------------- Linux ----------------

def linux(a, stage, describe):
    top = os.path.join(stage, TOP)
    launcher(a.linux_launcher, top, "hookshotvr-launcher")
    builder = os.path.join(top, "builder")
    if not os.path.isfile(os.path.join(a.linux_builder, "usr", "python", "bin", "python3")):
        sys.exit("%s: not an unpacked Hookshot Linux builder (no usr/python/bin/python3)" % a.linux_builder)
    # (as Hookshot ships it: its files only, not the caches a run of it left: __pycache__)
    shutil.copytree(a.linux_builder, builder, symlinks=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", *DEV_ONLY_DIRS))
    with open(os.path.join(builder, "hookshot-linux.txt"), encoding="utf-8") as f:
        tag = f.readline().strip()
    a.stamp = game_stamp(os.path.join(top, TOP), tag)
    print("  game stamp %s (builder %s)" % (a.stamp[:16], tag))
    readme(top, "linux", a.version, describe)
    out = os.path.join(a.out, "HookshotVR-%s-linux-x86_64.tar.gz" % slug(a.version))

    def owner(ti):
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = ""
        return ti
    with tarfile.open(out + ".new", "w:gz", compresslevel=6) as t:
        t.add(top, TOP, filter=owner)
    os.replace(out + ".new", out)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("out")
    p.add_argument("--version", required=True)
    p.add_argument("--commit", default="HEAD")
    p.add_argument("--linux-launcher")
    p.add_argument("--linux-builder")
    p.add_argument("--windows-launcher")
    p.add_argument("--windows-parts")
    p.add_argument("--keep-staging", action="store_true")
    a = p.parse_args()
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    made, stamps = [], []
    for plat, need, fn in (("windows", (a.windows_launcher, a.windows_parts), windows),
                           ("linux", (a.linux_launcher, a.linux_builder), linux)):
        if not all(need):
            continue
        stage = os.path.join(a.out, "staging", plat)
        shutil.rmtree(stage, ignore_errors=True)
        os.makedirs(stage)
        print("%s:" % plat, flush=True)
        describe = source(os.path.join(stage, TOP, TOP), a.commit, a.version)
        made.append(fn(a, stage, describe))
        stamps.append(a.stamp)
        if not a.keep_staging:
            shutil.rmtree(stage)
        print("  %s: %d MB" % (made[-1], os.path.getsize(made[-1]) >> 20), flush=True)
    if not made:
        sys.exit("nothing to make: give --windows-launcher and --windows-parts, or --linux-launcher and --linux-builder")
    listing(os.path.join(a.out, "SHA256SUMS.txt"), {os.path.basename(m): sha256(m) for m in made})
    listing(os.path.join(a.out, "GAME_STAMPS.txt"), {os.path.basename(m): st for m, st in zip(made, stamps)})
    return 0


def listing(path, new):
    """'<hash>  <file>' lines: the files made now replace their own earlier lines (a platform made again), the others' kept
    (the platforms made in two runs). The launcher takes the first line naming its archive"""
    lines = []
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            lines = [l for l in f.read().splitlines() if l.strip() and l.split(None, 1)[-1].lstrip("*") not in new]
    lines += ["%s  %s" % (h, name) for name, h in new.items()]
    with open(path + ".new", "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(l + "\n" for l in lines))
    os.replace(path + ".new", path)


if __name__ == "__main__":
    sys.exit(main())
