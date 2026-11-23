"""A windowed run with the stand-in headset (vr/tools/fake_headset): the title screen's pictures, then the save in play, a
teleport, and the desktop window resized again and again while the headset's eyes are made (the 19 Nov crash: the GPU lost at
once when the window was snapped to half the screen). Needs a desktop (X11) and wmctrl; the window shows on the screen.

    python -I -B vr/tools/fake_headset/window_test.py --game <game folder> --fake <folder with libopenxr_loader.so.1>
        --out <run folder> [--resizes 8] [--set key=value ...] [--env NAME=VALUE ...] [--area kakariko]

The run folder gets run.log (the game's log, the stand-in's lines among it), title_*.png and play_*.png. Exit 0 only with the
evidence for what was asked (verdict(): the title reached and its pictures, headset frames shown; for a whole run also every
resize sent, a new swapchain after each and headset frames after the last; no lost GPU or fatal line); 1 otherwise, each
missing piece printed. The game folder's options file is put back however the run ends.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "vr/tools"))
import hookshot_ctl  # noqa: E402
import bench  # noqa: E402  (its environment, ini writer and play checks)

SIZES = ((960, 1008), (1280, 720), (700, 900), (1600, 900))


FRAMES = re.compile(r"vr: (\d+) headset frames shown")
BROKEN = ("device lost", "DEVICE_LOST", "FATAL", "swapchain creation failed")


def window_of(pid):
    out = subprocess.run(["wmctrl", "-lp"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split(None, 4)
        if len(f) >= 3 and f[2] == str(pid):
            return f[0]
    return None


def log_size(out):
    """run.log's length in bytes now (the game logs to stderr unbuffered: what it has said so far)"""
    return os.path.getsize(out / "run.log")


def frames_after(data, at):
    """The headset frame counts of the 10 s lines logged after byte `at`"""
    return [int(m) for m in FRAMES.findall(data[at:].decode(errors="replace"))]


def verdict(ev, data, title_only):
    """What the run did not show, as reasons; none: a pass. The title: the menu reached, its two pictures taken, headset frames
    shown. A whole run also: the save in play, every resize sent (wmctrl exit 0), a new swapchain after each, and headset frames
    shown after the last. Both: the game still running when asked to quit, and no lost GPU or fatal line"""
    text = data.decode(errors="replace")
    why = []
    for key in BROKEN:
        hits = [l for l in text.splitlines() if key in l]
        if hits:
            why.append("%d '%s' line(s): %s" % (len(hits), key, hits[-1][:160]))
    if not ev["alive"]:
        why.append("the game had ended before the quit")
    if not ev["menu"]:
        why.append("the title menu was not reached in 180 s")
    if ev["title_shots"] < 2:
        why.append("%d of 2 title pictures taken" % ev["title_shots"])
    if not any(n > 0 for n in frames_after(data, 0)):
        why.append("no headset frames shown")
    if title_only:
        return why
    if not ev["in_play"]:
        why.append("the save was not in play in %s" % ev["area"])
        return why
    if ev["wmctrl_failed"]:
        why.append("wmctrl failed: %s" % "; ".join(ev["wmctrl_failed"]))
    marks = ev["marks"]
    if len(marks) < ev["resizes"]:
        why.append("%d of %d resizes made" % (len(marks), ev["resizes"]))
    for i, at in enumerate(marks):
        until = marks[i + 1] if i + 1 < len(marks) else len(data)
        if b"vk: swapchain " not in data[at:until].replace(b"vk: swapchain creation failed", b""):
            why.append("no new swapchain after resize %d" % (i + 1))
    if marks and not any(n > 0 for n in frames_after(data, marks[-1])):
        why.append("no headset frames shown after the last resize")
    return why


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", required=True)
    p.add_argument("--fake", required=True, help="the folder holding the stand-in libopenxr_loader.so.1")
    p.add_argument("--out", required=True)
    p.add_argument("--save", default=os.path.expanduser("~/.local/share/hookshot/save"))
    p.add_argument("--area", default="kakariko")
    p.add_argument("--resizes", type=int, default=8)
    p.add_argument("--title-only", action="store_true", help="the title's pictures, then quit")
    p.add_argument("--every", type=float, default=6.0, help="seconds between resizes")
    p.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    args = p.parse_args()
    if subprocess.run(["pgrep", "-x", "hookshot"], capture_output=True).returncode == 0:
        raise SystemExit("window_test: a game is running: not started")
    game_dir, out = Path(args.game).resolve(), Path(args.out).resolve()
    if out.exists():
        raise SystemExit("window_test: %s exists already" % out)
    (out / "user").mkdir(parents=True)
    shutil.copytree(args.save, out / "user/save")
    # (what the run showed, for the verdict: see verdict())
    ev = dict(alive=False, menu=False, title_shots=0, in_play=False, area=args.area, wmctrl_failed=[], marks=[],
              resizes=args.resizes)
    proc = log = sock_dir = None
    sets = dict(kv.split("=", 1) for kv in args.set)
    bench.write_ini(game_dir, None, sets, out)
    try:   # (from here on the game folder's own options are put back, however the run ends)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("RX_VR_", "RX_VK_", "HOS_", "HOOKSHOT_VR"))}
        env.update(bench.LAUNCHER_ENV)
        sock_dir = Path(tempfile.mkdtemp(prefix="hswin-", dir=os.environ.get("XDG_RUNTIME_DIR") or None))
        env.update(HOOKSHOT_VR="1", HOS_AUDIO="0", SDL_AUDIODRIVER="dummy", HOS_ENV_FILE=str(out / "env.txt"),
                   HOS_CONTROL=str(sock_dir / "control.sock"), HOS_INPUT_BACKGROUND="1",
                   LD_LIBRARY_PATH=str(Path(args.fake).resolve()))
        for kv in args.env:
            k, v = kv.split("=", 1)
            if v:
                env[k] = v
            else:
                env.pop(k, None)
        (out / "env.txt").write_text("")
        (out / "run-env.json").write_text(json.dumps({k: v for k, v in env.items()
                                                      if k.startswith(("RX_", "HOS_", "HOOKSHOT", "SDL_", "LD_"))}, indent=1))
        log = open(out / "run.log", "wb")
        proc = subprocess.Popen([str(game_dir / "bin/hookshot"), "--user", str(out / "user")], cwd=game_dir, env=env,
                                stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        bridge = None
        for _ in range(240):
            try:
                bridge = hookshot_ctl.Bridge(str(sock_dir / "control.sock"), 5)
                break
            except hookshot_ctl.Failed:
                if proc.poll() is not None:
                    raise SystemExit("window_test: the game ended early (%s)" % proc.returncode)
                time.sleep(0.5)
        if bridge is None:
            raise SystemExit("window_test: no control bridge")
        # (the title, its menu shown: the state "menu" a while, then two pictures; an intro movie is passed with A)
        end = time.monotonic() + 180
        while time.monotonic() < end:
            if bridge.request("status").get("state") == "menu":
                ev["menu"] = True
                break
            time.sleep(1)
        time.sleep(12)
        for i in range(2):
            r = bridge.request("shot", path=str(out / ("title_%d.png" % i)), timeout=30)
            ev["title_shots"] += bool(r.get("ok"))
            print("window_test: title picture %d: %s" % (i, "taken" if r.get("ok") else r), flush=True)
            time.sleep(1)
        if not args.title_only:
            for i in range(30):   # (A through the title and the file select, until the save plays)
                s = bridge.request("status")
                if bench.in_play(s):
                    break
                bridge.request("press", buttons="A", frames=8, timeout=10)
                time.sleep(3)
            bench.wait_until(bridge, bench.in_play, 120, "the save in play")
            index, title = bench.AREAS[args.area]
            for attempt in range(8):
                r = bridge.request("teleport", to=index, timeout=10)
                if r.get("ok") and r.get("sent"):
                    break
                bridge.request("press", buttons="B" if attempt % 2 == 0 else "A", frames=8, timeout=10)
                time.sleep(2)
            bridge.request("wait", event="map", map=r["map"], timeout=60)
            bench.wait_until(bridge, bench.in_play, 90, "%s loaded" % title)
            ev["in_play"] = True
            print("window_test: %s in play; settling" % title, flush=True)
            time.sleep(20)
            bridge.request("shot", path=str(out / "play_before.png"), timeout=30)
            win = window_of(proc.pid)
            if not win:
                raise SystemExit("window_test: the game's window was not found (wmctrl -lp)")
            for i in range(args.resizes):
                w, h = SIZES[i % len(SIZES)]
                ev["marks"].append(log_size(out))   # (what follows in run.log came after this resize)
                for cmd in (["-b", "remove,maximized_vert,maximized_horz"], ["-e", "0,0,0,%d,%d" % (w, h)]):
                    r = subprocess.run(["wmctrl", "-i", "-r", win] + cmd, capture_output=True, text=True)
                    if r.returncode != 0:
                        ev["wmctrl_failed"].append("resize %d %s: exit %d %s" % (i + 1, cmd[0], r.returncode,
                                                                                  r.stderr.strip()[:80]))
                print("window_test: resize %d: %dx%d" % (i + 1, w, h), flush=True)
                time.sleep(args.every)
                if proc.poll() is not None:
                    raise SystemExit("window_test: the game ended after resize %d (%s)" % (i + 1, proc.returncode))
            # (the headset's 10 s frame count, once more after the last resize: up to 15 s for it)
            end = time.monotonic() + 15
            while time.monotonic() < end and not frames_after((out / "run.log").read_bytes(), ev["marks"][-1]):
                time.sleep(1)
            bridge.request("shot", path=str(out / "play_after.png"), timeout=30)
            time.sleep(5)
        ev["alive"] = proc.poll() is None
        bridge.request("quit", timeout=10)
    finally:
        if proc is not None:
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        if log is not None:
            log.close()
        if sock_dir is not None:
            shutil.rmtree(sock_dir, ignore_errors=True)
        bench.restore_ini(game_dir, out)
        data = (out / "run.log").read_bytes() if (out / "run.log").exists() else b""
        chains = re.findall(r"vk: swapchain (\d+x\d+)", data.decode(errors="replace"))
        print("window_test: swapchains made: %s" % ", ".join(chains), flush=True)
        print("window_test: headset frames (10 s counts): %s" % frames_after(data, 0), flush=True)
    why = verdict(ev, data, args.title_only)
    for w in why:
        print("window_test: not shown: %s" % w, flush=True)
    what = "the title" if args.title_only else "every resize"
    print("window_test: %s" % ("PASS: the game ran through %s, the headset's frames shown" % what if not why else "FAIL"),
          flush=True)
    return 0 if not why else 1


if __name__ == "__main__":
    sys.exit(main())
