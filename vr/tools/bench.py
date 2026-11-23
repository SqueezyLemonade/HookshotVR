"""Hookshot VR's headless performance benchmark: the same spots and the same route in each heavy area, timed.

    <builder python> -I -B vr/tools/bench.py --game <game folder> --out <run folder> [options]

What it does, in order:
  1. Copies the player's save into the run folder (the save itself is only read), and writes the game folder's
     hookshot_options.ini for the run (the player's own settings with 3D = True stereo), putting the old one back at the end.
     Use a private copy of the game folder (vr/tools/bench.py writes its ini), not the shared one the player plays.
  2. Starts the game without a window, true stereo without a headset (RX_VR_EYES_TEST, no pictures), the game's camera
     widened to the headset's FOV (RX_VR_BENCH_FOV, vr/camera/view_host.c), the VR launcher's RX_VK_* settings, and the
     control bridge (HOS_CONTROL, docs/control.md).
  3. Loads the save (timed A presses), then for each area: the debug teleport there, waits for the map to load, lets it
     settle, then the route: standing still, a full turn of the camera (right stick), a walk forward (left stick).
  4. Measures all the while: every frame's time and the replay's CPU phases (RX_VK_FRAME_TRACE's hookshot_frames.csv), the
     renderer's lines every 120 presents (draws, passes, barriers, submits, GPU time, pipeline waits, memory sync), CPU time
     per thread (/proc), process memory, and the GPU's use and video memory (nvidia-smi). With --perf, a perf profile of
     the whole process for each area's route (perf record, user space only).
  5. Writes results.json (everything, per area and part of the route) and summary.md (the tables) into the run folder.

The run never starts while a game is running (pgrep -x hookshot): the player may be in the headset. Take the shared
game lock around it (flock <update folder>/.game.lock) so no other session's build runs meanwhile and skews the numbers.

Compare two builds or settings: run each into its own --out, then
    <builder python> -I -B vr/tools/bench.py --compare <run A> <run B>
A run summarised again from its raw data (after a change to this file's summary): --again <run>.
"""
import argparse
import csv
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import hookshot_ctl  # noqa: E402  (the control bridge's client: scripts/hookshot_ctl.py)

# The four heavy areas, by the debug teleport's destination index (host/hos_main.c oot_destinations).
AREAS = {
    "field": (1, "Hyrule Field"),
    "kokiri": (0, "Kokiri Forest"),
    "kakariko": (2, "Kakariko Village"),
    "market": (3, "Hyrule Castle Town (Market)"),
}

# The player's VR launcher (VR/hookshot-vr/play-hookshot-vr-2.2-candidate.sh), less the headset's own variables.
LAUNCHER_ENV = {
    "HOS_FPS_LOG": "1", "HOS_FPS_TARGET": "120", "RX_VK_IMMEDIATE": "1", "HOOKSHOT_NO_UPDATE_CHECK": "1",
    "HOS_INPUT_BACKGROUND": "1", "RX_VR_VISIBILITY_TRACE": "1", "RX_VR_PROG_COST": "1",
    "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1", "RX_VK_PRECOMPILE": "0",
    "RX_VK_VRAM_MB": "6144", "RX_VK_MIRROR_MB": "1536", "RX_VK_WB_SHADOW_MB": "512", "RX_VK_TARGET_POOL_MB": "128",
    "RX_VK_COMPACT": "1", "RX_VK_COMPACT_PCT": "80", "RX_VK_MIRROR_TIGHT": "1", "RX_VK_UPLOAD_BUDGET_MS": "40",
}
# --profile clean: the launcher's research tracers left out (what they cost is then the difference)
CLEAN_DROP = ("HOS_FPS_LOG", "RX_VR_VISIBILITY_TRACE", "RX_VR_PROG_COST")

# The headset's FOV in the player's logs ("vr: fov asked 109.6 x 111.6 deg"), the scene drawn at the player's 2880x2304.
HEADSET_FOV = "109.6x111.6"
TITLE_SCRIPT = "5:A/120,8:A/120,11:A/120,14:A/120,17:A/120,20:A/120,24:A/120,28:A/120"

# ---- the renderer's lines, every 120 presents (gpu/vk/vk_replay.cpp) ----
NUM = r"([-\d.]+)"
LINE_RULES = {
    "presents": re.compile(r"vk: (\d+) presents, " + NUM + r" fps; a frame: " + NUM + r" draws \(" + NUM + r" skipped\), "
                           + NUM + r" dispatches, " + NUM + r" passes, " + NUM + r" barriers, " + NUM + r" submits, "
                           + NUM + r" uploads \(" + NUM + r" MB\), " + NUM + r" ub updates .*GPU-bound wait " + NUM
                           + r" ms[^;]*; (\d+) pipelines .* (\d+) textures \((\d+) MB\)"),   # ([^;]*: Hookshot 2.3's "(recording slots only)")
    "replay": re.compile(r"vk: a frame's replay time \(ms\): (.*)"),
    "gpu": re.compile(r"vk: GPU time " + NUM + r" ms a frame \(the longest submission " + NUM + r" ms\)"),
    "waits": re.compile(r"vk: a frame: " + NUM + r" draws left out \(not ready\), " + NUM + r" waits for shaders or pipelines \("
                        + NUM + r" ms\)"),
    "mirrors": re.compile(r"vk: a frame: mirrors (\d+) \((\d+) MB\): " + NUM + r" MB synced in " + NUM + r" copies, " + NUM
                          + r" MB written back"),
    "memwork": re.compile(r"vk: a submit's memory work \(ms\): maps " + NUM + r", capture taken " + NUM + r", writes collected "
                          + NUM),
    "stereo": re.compile(r"vr: true stereo: a frame " + NUM + r" passes and " + NUM + r" draws in both eyes, " + NUM
                         + r" of them moved"),
    "eye_routes": re.compile(r"vr: true stereo: a frame " + NUM + r" draws drawn one eye after the other \(" + NUM + r" mesh, "
                             + NUM + r" reading per-eye textures, " + NUM + r" full-screen\)"),
    "gpu_passes": re.compile(r"vk:   " + NUM + r" ms a frame in all"),
    "status": re.compile(r"vk: status: (\d+) frames, the slowest " + NUM + r" ms; (.*)"),
    # (gpu/vk/vk_texture.cpp, every 600 presents, counted since the start: the summary takes the difference over a part)
    "targets": re.compile(r"vk: render target images: (\d+) reused from the pool, (\d+) made new, (\d+) destroyed from it; "
                          r"it holds (\d+) \((\d+) MB\)"),
    "vram": re.compile(r"vk: VRAM: (\d+) MB used of (\d+) MB allowed \((\d+) MB of it free in the blocks\); textures (\d+) MB "
                       r"\(budget (\d+) MB\)"),
    # (gpu/vk/vk_texture.cpp, every 1800 presents: the process's malloc heap, the driver's own allocations in it)
    "malloc": re.compile(r"vk: malloc: (\d+) MB in arenas \((\d+) MB in use"),
    # (host/hos_thread.c, every 5 s: the game's threads waiting for one of the 6 virtual cores)
    "vcores": re.compile(r"vcores: (\d+) takes, (\d+) waits \(mean " + NUM + r" us, longest " + NUM + r" ms[^)]*\), (\d+) kicks, "
                         r"(\d+) yields, (?:\d+ starved grants, )?(\d+) escapes in " + NUM + r" s"),   # (starved grants: 2.3)
}
PRESENTS_FIELDS = ("presents", "fps", "draws", "skipped", "dispatches", "passes", "barriers", "submits", "uploads",
                   "upload_mb", "ub_updates", "gpu_bound_wait_ms", "pipelines", "textures", "texture_mb")


def parse_line(text):
    """(kind, values) for a renderer line the benchmark reads, or None"""
    for kind, rule in LINE_RULES.items():
        m = rule.search(text)
        if not m:
            continue
        g = m.groups()
        if kind == "presents":
            return kind, {k: float(v) for k, v in zip(PRESENTS_FIELDS, g)}
        if kind == "replay":
            return kind, {k.strip(): float(v) for k, v in (p.rsplit(" ", 1) for p in g[0].split(","))}
        if kind == "status":
            return kind, {"slowest_ms": float(g[1]), "text": g[2]}
        names = {"gpu": ("gpu_ms", "longest_submit_ms"), "waits": ("left_out", "pipeline_waits", "pipeline_wait_ms"),
                 "mirrors": ("mirrors", "mirror_mb", "synced_mb", "sync_copies", "written_back_mb"),
                 "memwork": ("maps_ms", "capture_ms", "collect_ms"), "stereo": ("passes", "draws", "moved"),
                 "eye_routes": ("per_eye_draws", "mesh", "per_eye_textures", "full_screen"),
                 "gpu_passes": ("gpu_spans_ms",),
                 "malloc": ("arenas_mb", "in_use_mb"),
                 "targets": ("reused", "made", "destroyed", "held", "held_mb"),
                 "vram": ("used_mb", "allowed_mb", "free_in_blocks_mb", "textures_mb", "texture_budget_mb"),
                 "vcores": ("takes", "waits", "wait_mean_us", "wait_longest_ms", "kicks", "yields", "escapes", "seconds")}[kind]
        return kind, {k: float(v) for k, v in zip(names, g)}
    return None


class Game:
    """The running game: its log read as it comes (each line with the time it came), its threads and memory sampled"""

    def __init__(self, proc, out):
        self.proc, self.out = proc, out
        self.lines = []      # (monotonic time, kind, values)
        self.samples = []    # (monotonic time, {thread: cpu ns}, rss kB, GPU use %, video memory MB)
        self.gpu_now = (None, None)
        self.stop = threading.Event()
        threading.Thread(target=self._read_log, daemon=True).start()

    def _read_log(self):
        with (self.out / "run.log").open("w", errors="replace") as log:
            for raw in self.proc.stdout:
                text = raw.decode("utf-8", "replace").rstrip("\n")
                log.write(text + "\n")
                parsed = parse_line(text)
                if parsed:
                    self.lines.append((time.monotonic(), parsed[0], parsed[1]))

    def threads(self):
        """{"<name> <tid>": CPU ns so far} from /proc (schedstat: run time in ns)"""
        cpu = {}
        base = Path("/proc/%d/task" % self.proc.pid)
        try:
            tasks = list(base.iterdir())
        except OSError:
            return cpu
        for t in tasks:
            try:
                name = (t / "comm").read_text().strip()
                ns = int((t / "schedstat").read_text().split()[0])
            except (OSError, ValueError, IndexError):
                continue
            cpu["%s %s" % (name, t.name)] = ns
        return cpu

    def others(self):
        """Other games running meanwhile (pgrep -x hookshot, this one left out): they share the GPU and the CPU, and the
        numbers of a part that had one are not the build's own"""
        r = subprocess.run(["pgrep", "-x", "hookshot"], capture_output=True, text=True)
        return [int(p) for p in r.stdout.split() if int(p) != self.proc.pid]

    def rss_kb(self):
        try:
            for line in Path("/proc/%d/status" % self.proc.pid).read_text().splitlines():
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        except OSError:
            pass
        return None

    def sample_forever(self, every):
        smi = None
        if shutil.which("nvidia-smi"):   # (one nvidia-smi that prints a line every `every` seconds, not one a sample)
            smi = subprocess.Popen(["nvidia-smi", "--query-gpu=utilization.gpu,memory.used", "--format=csv,noheader,nounits",
                                    "-lms", str(int(every * 1000))], stdout=subprocess.PIPE, text=True)
            threading.Thread(target=self._read_smi, args=(smi,), daemon=True).start()
        while not self.stop.is_set() and self.proc.poll() is None:
            self.samples.append((time.monotonic(), self.threads(), self.rss_kb(), self.gpu_now[0], self.gpu_now[1],
                                 self.others()))
            self.stop.wait(every)
        if smi:
            smi.terminate()

    def _read_smi(self, smi):
        for line in smi.stdout:
            try:
                util, used = (float(x) for x in line.split(","))
                self.gpu_now = (util, used)
            except ValueError:
                pass


def wait_until(bridge, test, timeout, what):
    """Polls the bridge's status until test(status) holds; the last status"""
    end = time.monotonic() + timeout
    while True:
        s = bridge.request("status")
        if test(s):
            return s
        if time.monotonic() > end:
            raise SystemExit("bench: %s did not happen in %d s (status %s)" % (what, timeout, json.dumps(s)[:300]))
        time.sleep(0.5)


def in_play(s):
    g = s.get("game") or {}
    return s.get("state") == "play" and s.get("map") and not g.get("loading") and not g.get("event")


def run_route(bridge, game, area, args, marks):
    """One area: teleport, load, settle, then the route's parts; each part's (start, end) in marks"""
    index, title = AREAS[area] if area in AREAS else (int(area), "destination %s" % area)   # (or the teleport list's index)
    print("bench: %s: teleport" % title, flush=True)
    t_ask = time.monotonic()
    for attempt in range(8):   # (refused while an event plays: a talk the walk ran into is closed with B, then A, and tried again)
        r = bridge.request("teleport", to=index, timeout=10)
        if r.get("ok") and r.get("sent"):
            break
        print("bench: %s: teleport not taken (%s): B and A pressed" % (title, r.get("message") or r.get("error")), flush=True)
        bridge.request("press", buttons="B" if attempt % 2 == 0 else "A", frames=8, timeout=10)
        time.sleep(2)
    else:
        raise SystemExit("bench: the teleport to %s was not taken: %s" % (title, r))
    bridge.request("wait", event="map", map=r["map"], timeout=60)
    time.sleep(2)
    wait_until(bridge, in_play, 90, "%s loaded" % title)
    t_loaded = time.monotonic()
    marks.append({"area": area, "part": "load", "start": t_ask, "end": t_loaded})
    print("bench: %s: loaded in %.1f s; settling %d s" % (title, t_loaded - t_ask, args.settle), flush=True)
    time.sleep(args.settle)
    marks.append({"area": area, "part": "settle", "start": t_loaded, "end": time.monotonic()})
    # (the walk before the turn: the stick's forward is the camera's, which the turn changes. A stick is held for a time, not a
    # number of the game's frames: at 20 fps 8 s of frames would last 24 s)
    route = (("still", None, args.still), ("walk", ("L", 0.0, 1.0), args.walk), ("turn", ("R", 1.0, 0.0), args.turn))
    go = Path(str(game.out / "samples.txt") + ".go")
    if args.sample_all:   # (each area's route in a file of its own: samples-<area>.txt, renamed after)
        go.touch()
    for part, stick, secs in route:
        t0 = time.monotonic()
        if stick:
            bridge.request("stick", stick=stick[0], x=stick[1], y=stick[2], frames=216000, exclusive=True, wait=False)
            time.sleep(secs)
            bridge.request("release")
        else:
            time.sleep(secs)
        t1 = time.monotonic()
        # (an event at the part's end, a talk or a cutscene the walk ran into: the part is flagged, the event closed with B)
        g = bridge.request("status").get("game") or {}
        marks.append({"area": area, "part": part, "start": t0, "end": t1, "event": bool(g.get("event"))})
        print("bench: %s: %s %.1f s%s" % (title, part, t1 - t0, " (an event was playing at its end)" if g.get("event") else ""),
              flush=True)
        if g.get("event"):
            bridge.request("press", buttons="B", frames=8, timeout=10)
    if args.sample_all:
        go.unlink(missing_ok=True)
        time.sleep(0.5)
        samples = game.out / "samples.txt"
        if samples.exists():
            samples.rename(game.out / ("samples-%s.txt" % area))
    # (the whole stay after the load, for the lines logged only every 600 presents: true stereo's, the virtual cores')
    marks.append({"area": area, "part": "all", "start": t_loaded, "end": time.monotonic()})


def percentile(values, p):
    if not values:
        return None
    v = sorted(values)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def summarise(game, marks, frames_csv, base_vram):
    """Per area and part: frame times, the per-frame replay phases, the renderer's lines, CPU per thread, memory"""
    rows = []
    if frames_csv and frames_csv.exists():
        with frames_csv.open() as f:
            rows = list(csv.DictReader(f))
        # (each frame's time on the steady clock: Hookshot VR's mono_s column on the 2.2 base; on Hookshot 2.3 its own at_ns,
        # hos_now_ns, the same CLOCK_MONOTONIC on Linux, in ns)
        for r in rows:
            if not r.get("mono_s") and r.get("at_ns"):
                r["mono_s"] = str(int(r["at_ns"]) / 1e9)
        rows = [r for r in rows if r.get("mono_s")]
    results = []
    for m in marks:
        t0, t1 = m["start"], m["end"]
        res = dict(m, seconds=round(t1 - t0, 2))
        # frames presented in the part (their present's time inside it)
        fr = [r for r in rows if t0 <= float(r["mono_s"]) < t1]
        ms = [float(r["ms"]) for r in fr]
        if ms:
            span = float(fr[-1]["mono_s"]) - float(fr[0]["mono_s"]) + ms[0] / 1000.0
            res["frames"] = len(ms)
            res["fps"] = round(len(ms) / span, 2) if span > 0 else None
            res["frame_ms"] = {"mean": round(statistics.fmean(ms), 2), "p50": percentile(ms, 50), "p95": percentile(ms, 95),
                               "p99": percentile(ms, 99), "max": max(ms)}
            res["frames_over_33ms"] = sum(1 for v in ms if v > 33.4)
            cols = [c for c in fr[0].keys() if c not in ("frame", "ms", "textures", "mono_s", "at_ns", "ab_side")]
            res["per_frame"] = {c: round(statistics.fmean(float(r[c]) for r in fr), 3) for c in cols}
        # the renderer's lines: those that came in the part, at least 3 s after its start (each covers 120 presents)
        lines = [(t, k, v) for t, k, v in game.lines if t0 + min(3.0, (t1 - t0) / 2) <= t < t1]
        by_kind = {}
        for _, k, v in lines:
            by_kind.setdefault(k, []).append(v)
        res["renderer"] = {}
        for k, vs in by_kind.items():
            if k == "status":
                res["renderer"][k] = vs[-1]
                continue
            if k == "targets":   # (counted since the start: what the part added, and the pool at its end)
                res["renderer"][k] = {key: vs[-1][key] - vs[0][key] for key in ("reused", "made", "destroyed")}
                res["renderer"][k].update(lines=len(vs), held=vs[-1]["held"], held_mb=vs[-1]["held_mb"])
                continue
            keys = set().union(*vs)
            res["renderer"][k] = {key: round(statistics.fmean(v.get(key, 0.0) for v in vs), 3) for key in sorted(keys)}
        # CPU per thread: the samples nearest the part's ends
        inside = [s for s in game.samples if t0 <= s[0] <= t1]
        others = sorted({p for s in inside if len(s) > 5 for p in s[5]})
        if others:
            res["other_games"] = others
        if len(inside) >= 2:
            a, b = inside[0], inside[-1]
            dt = (b[0] - a[0]) * 1e9
            per = {}
            for name, ns in b[1].items():
                if name in a[1]:
                    per[name] = (ns - a[1][name]) / dt * 100.0
            by_name = {}
            for name, pct in per.items():
                by_name.setdefault(name.rsplit(" ", 1)[0], []).append(pct)
            res["cpu_pct"] = {"process": round(sum(per.values()), 1),
                              "threads": {n: round(p, 1) for n, p in sorted(per.items(), key=lambda x: -x[1])[:12] if p >= 2},
                              "by_name": {n: [round(sum(v), 1), len(v)] for n, v in
                                          sorted(by_name.items(), key=lambda x: -sum(x[1]))[:12] if sum(v) >= 2}}
            rss = [s[2] for s in inside if s[2]]
            gpu = [s[3] for s in inside if s[3] is not None]
            vram = [s[4] for s in inside if s[4] is not None]
            res["memory"] = {"rss_mb": round(max(rss) / 1024) if rss else None,
                             "vram_mb": round(max(vram) - (base_vram or 0)) if vram else None,
                             "gpu_util_pct": round(statistics.fmean(gpu), 1) if gpu else None}
        results.append(res)
    return results


def table(results, parts=("still", "walk", "turn", "all")):
    """summary.md's tables: one row per area and part of the route"""
    out = ["| area | part | fps | frame ms p50 / p95 / p99 / max | >33 ms | CPU (process) | replay thread (Presentation) | GPU ms | GPU % | "
           "draws | passes | eye passes | pipeline waits | memsync ms | RSS MB | VRAM MB |",
           "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        if r["part"] not in parts:
            continue
        fm = r.get("frame_ms") or {}
        ren = r.get("renderer", {})
        pre, gpu, st, wa = ren.get("presents", {}), ren.get("gpu", {}), ren.get("stereo", {}), ren.get("waits", {})
        cpu = r.get("cpu_pct", {})
        replay = sum(p for n, (p, _) in cpu.get("by_name", {}).items() if n in ("Presentation Th", "mkw_vk replay"))
        mem = r.get("memory", {})
        pf = r.get("per_frame", {})
        out.append("| %s | %s | %s | %s / %s / %s / %s | %s | %s%% | %.0f%% | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["area"], r["part"] + (" (event)" if r.get("event") else "") + (" (ANOTHER GAME RAN)" if r.get("other_games") else ""), r.get("fps"), fm.get("p50"), fm.get("p95"), fm.get("p99"), fm.get("max"),
            r.get("frames_over_33ms"), cpu.get("process"), replay, gpu.get("gpu_ms"), mem.get("gpu_util_pct"),
            pre.get("draws"), pre.get("passes"), st.get("passes"), wa.get("pipeline_waits"), pf.get("memsync_ms"),
            mem.get("rss_mb"), mem.get("vram_mb")))
    return "\n".join(out)


def phases_table(results, parts=("still", "walk", "turn")):
    """The replay thread's time a frame by phase (the renderer's own 120-present lines)"""
    names = ("draw", "descriptors", "texture lookups", "uploads", "pipelines", "programs", "commands", "dispatch",
             "memory sync", "queue submit", "slot wait", "submit", "present")
    out = ["| area | part | " + " | ".join(names) + " |", "|---|---|" + "---|" * len(names)]
    for r in results:
        if r["part"] in parts:
            rep = r.get("renderer", {}).get("replay", {})
            out.append("| %s | %s | %s |" % (r["area"], r["part"], " | ".join(str(rep.get(n, "")) for n in names)))
    return "\n".join(out)


def area_table(results):
    """Each area's whole stay: true stereo's counts, the game threads' waits for a virtual core, memory, the memory sync"""
    out = ["| area | eye passes | eye draws (moved) | drawn per eye: all / mesh / per-eye textures / full-screen | vcore waits a s "
           "(mean us, longest ms) | memory sync a submit: maps / capture / collect ms | mirrors MB | render targets made / "
           "reused | VRAM used / textures / budget MB | pipelines | malloc in use MB | RSS MB | GPU VRAM MB |",
           "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        if r["part"] != "all":
            continue
        ren = r.get("renderer", {})
        st, er, vc, mw, mi, tg, vr = (ren.get(k, {}) for k in ("stereo", "eye_routes", "vcores", "memwork", "mirrors", "targets",
                                                               "vram"))
        mem = r.get("memory", {})
        secs = vc.get("seconds") or 0
        out.append("| %s | %s | %s (%s) | %s / %s / %s / %s | %s (%s, %s) | %s / %s / %s | %s | %s / %s | %s / %s / %s | %s | %s | %s | %s |" % (
            r["area"], st.get("passes"), st.get("draws"), st.get("moved"), er.get("per_eye_draws"), er.get("mesh"),
            er.get("per_eye_textures"), er.get("full_screen"), round(vc["waits"] / secs) if secs else None,
            vc.get("wait_mean_us"), vc.get("wait_longest_ms"), mw.get("maps_ms"), mw.get("capture_ms"), mw.get("collect_ms"),
            mi.get("mirror_mb"), tg.get("made"), tg.get("reused"), vr.get("used_mb"), vr.get("textures_mb"),
            vr.get("texture_budget_mb"), ren.get("presents", {}).get("pipelines"), ren.get("malloc", {}).get("in_use_mb"),
            mem.get("rss_mb"), mem.get("vram_mb")))
    return "\n".join(out)


def threads_table(results, parts=("still", "walk", "turn")):
    out = ["| area | part | threads by name: % of one core (count) |", "|---|---|---|"]
    for r in results:
        if r["part"] in parts:
            bn = r.get("cpu_pct", {}).get("by_name", {})
            out.append("| %s | %s | %s |" % (r["area"], r["part"], ", ".join("%s %.0f%% (%d)" % (n, p, c)
                                                                            for n, (p, c) in bn.items())))
    return "\n".join(out)


def write_ini(game_dir, source_ini, sets, out):
    """The game folder's options for the run: the source's, with `sets` changed; the old file kept in the run folder for
    restore_ini (options.before.ini), and the run's (options.run.ini) written there before the game folder's"""
    ini = game_dir / "hookshot_options.ini"
    if ini.exists():
        shutil.copy2(ini, out / "options.before.ini")
    lines = Path(source_ini).read_text().splitlines() if source_ini else ini.read_text().splitlines()
    seen = set()
    for i, line in enumerate(lines):
        key = line.split("=", 1)[0]
        if key in sets:
            lines[i] = "%s=%s" % (key, sets[key])
            seen.add(key)
    lines += ["%s=%s" % (k, v) for k, v in sets.items() if k not in seen]
    (out / "options.run.ini").write_text("\n".join(lines) + "\n")
    shutil.copy2(out / "options.run.ini", ini)


def restore_ini(game_dir, out):
    """The game folder's options as they were before write_ini: the old file put back, or, when there was none, the run's
    removed. Nothing when write_ini wrote nothing"""
    ini, before = game_dir / "hookshot_options.ini", out / "options.before.ini"
    if before.exists():
        shutil.copy2(before, ini)
    elif (out / "options.run.ini").exists():
        ini.unlink(missing_ok=True)


def compare(a, b):
    """Two runs' summaries side by side: fps and frame times by area and part"""
    ra = {(r["area"], r["part"]): r for r in json.loads((Path(a) / "results.json").read_text())["results"]}
    rb = {(r["area"], r["part"]): r for r in json.loads((Path(b) / "results.json").read_text())["results"]}
    print("| area | part | fps A | fps B | change | p95 ms A | p95 ms B | GPU ms A | GPU ms B |")
    print("|---|---|---|---|---|---|---|---|---|")
    for key in ra:
        if key in rb and key[1] in ("still", "turn", "walk"):
            x, y = ra[key], rb[key]
            fa, fb = x.get("fps") or 0, y.get("fps") or 0
            ga = x.get("renderer", {}).get("gpu", {}).get("gpu_ms")
            gb = y.get("renderer", {}).get("gpu", {}).get("gpu_ms")
            print("| %s | %s | %.1f | %.1f | %+.1f%% | %s | %s | %s | %s |" % (
                key[0], key[1], fa, fb, (fb / fa - 1) * 100 if fa else 0, (x.get("frame_ms") or {}).get("p95"),
                (y.get("frame_ms") or {}).get("p95"), ga, gb))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", help="the game folder (a private copy: its hookshot_options.ini is written for the run)")
    p.add_argument("--out", help="the run folder (made; must not exist yet)")
    p.add_argument("--save", default=os.path.expanduser("~/.local/share/hookshot/save"), help="the save root (only read)")
    p.add_argument("--ini", help="the settings to start from (default: the game folder's own)")
    p.add_argument("--areas", default="field,kokiri,kakariko,market",
                   help="areas by name, or by the debug teleport's destination index (host/hos_main.c oot_destinations)")
    p.add_argument("--passes", type=int, default=1, help="times round all the areas (the second: caches warm)")
    p.add_argument("--mode", choices=("stereo", "flat"), default="stereo", help="True stereo (default) or one picture")
    p.add_argument("--fov", default=HEADSET_FOV, help="the camera's FOV, WxH degrees (default the headset's); 'game': its own")
    p.add_argument("--profile", choices=("launcher", "clean"), default="launcher",
                   help="launcher: the VR launcher's settings as they are; clean: without its research tracers")
    p.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="an ini setting for the run")
    p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="an environment variable for the run")
    p.add_argument("--settle", type=int, default=15)
    p.add_argument("--still", type=int, default=12)
    p.add_argument("--turn", type=int, default=16)
    p.add_argument("--walk", type=int, default=12)
    p.add_argument("--perf", action="store_true", help="a perf profile of each area's route (perf record, user space)")
    p.add_argument("--sample-all", action="store_true",
                   help="every thread's stack every 2 ms during each area's route (HOS_SAMPLE_ALL; scripts/crit.py reads it)")
    p.add_argument("--compare", nargs=2, metavar=("RUN_A", "RUN_B"))
    p.add_argument("--again", metavar="RUN", help="a finished run summarised again from its raw.json")
    args = p.parse_args()
    if args.compare:
        return compare(*args.compare)
    if args.again:
        return again(args.again)
    if not args.game or not args.out:
        p.error("--game and --out are needed")
    if subprocess.run(["pgrep", "-x", "hookshot"], capture_output=True).returncode == 0:
        raise SystemExit("bench: a game is running (the player may be in the headset): not started")
    game_dir, out = Path(args.game).resolve(), Path(args.out).resolve()
    if out.exists():
        raise SystemExit("bench: %s exists already" % out)
    (out / "user").mkdir(parents=True)
    shutil.copytree(args.save, out / "user/save")

    sets = {"3d": "4" if args.mode == "stereo" else "1"}
    sets.update(kv.split("=", 1) for kv in args.set)
    write_ini(game_dir, args.ini, sets, out)
    try:   # (a start-up that fails puts the game folder's options back too; once started, the finally below does)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("RX_VR_", "RX_VK_", "HOS_", "HOOKSHOT_VR"))}
        env.update(LAUNCHER_ENV)
        if args.profile == "clean":
            for k in CLEAN_DROP:
                env.pop(k, None)
        elif any(line.strip() == "gpu_profile=1" for line in (out / "options.run.ini").read_text().splitlines()):
            env["RX_VK_PASS_TIMING"] = "1"   # (as the launcher does when the VR tab's GPU profile is on)
        # (a Unix socket's path is at most 107 bytes, so the socket goes in a short folder of this user's alone)
        sock_dir = Path(tempfile.mkdtemp(prefix="hsbench-", dir=os.environ.get("XDG_RUNTIME_DIR") or None))
        sock = sock_dir / "control.sock"
        env.update(HOOKSHOT_VR="0", HOS_HEADLESS="1", HOS_AUDIO="0", SDL_AUDIODRIVER="dummy",
                   HOS_ENV_FILE=str(out / "env.txt"), HOS_CONTROL=str(sock), HOS_INPUT_SCRIPT=TITLE_SCRIPT, HOS_INPUT_SCRIPT_BACKGROUND="1",
                   RX_VK_FRAME_TRACE="1")
        (out / "env.txt").write_text("")
        if args.mode == "stereo":
            (out / "eyes").mkdir()
            env.update(RX_VR_EYES_TEST=str(out / "eyes"), RX_VR_EYES_NOPICTURES="1")
        if args.sample_all:   # (host/posix/prof.c: sampled while <file>.go exists, which run_route makes for the route)
            env.update(HOS_PROFILE="60", HOS_SAMPLE_ALL=str(out / "samples.txt"))
        if args.fov != "game":
            env["RX_VR_BENCH_FOV"] = args.fov
        for kv in args.env:
            k, v = kv.split("=", 1)
            if v:
                env[k] = v
            else:
                env.pop(k, None)   # (NAME= : left out)
        (out / "run-env.json").write_text(json.dumps({k: v for k, v in env.items()
                                                      if k.startswith(("RX_", "HOS_", "HOOKSHOT", "SDL_"))}, indent=1))
        for stale in (game_dir / "bin/hookshot_frames.csv", game_dir / "hookshot_frames.csv",
                      game_dir / "logs/hookshot_frames.csv"):
            stale.unlink(missing_ok=True)

        base_vram = None
        if shutil.which("nvidia-smi"):
            r = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"], capture_output=True,
                               text=True)
            base_vram = float(r.stdout.split()[0]) if r.returncode == 0 and r.stdout.strip() else None
        t_start = time.monotonic()
        proc = subprocess.Popen([str(game_dir / "bin/hookshot"), "--user", str(out / "user")], cwd=game_dir, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        game = Game(proc, out)
        threading.Thread(target=game.sample_forever, args=(0.5,), daemon=True).start()
    except BaseException:
        restore_ini(game_dir, out)
        raise
    marks, perf_runs = [], []
    try:
        bridge = None
        for _ in range(240):
            try:
                bridge = hookshot_ctl.Bridge(str(sock), 5)
                break
            except hookshot_ctl.Failed:
                if proc.poll() is not None:
                    raise SystemExit("bench: the game ended early (%s): see %s" % (proc.returncode, out / "run.log"))
                time.sleep(0.5)
        if bridge is None:
            raise SystemExit("bench: no control bridge")
        s = wait_until(bridge, in_play, 240, "the save loaded")
        print("bench: in play at %.0f s, map %s" % (time.monotonic() - t_start, s.get("map")), flush=True)
        marks.append({"area": "boot", "part": "load", "start": t_start, "end": time.monotonic()})
        time.sleep(5)
        for n in range(args.passes):
            for area in args.areas.split(","):
                try:
                    run_route(bridge, game, area, args, marks)
                except (SystemExit, hookshot_ctl.Failed) as e:   # (the areas done so far are still summarised)
                    print("bench: %s: given up: %s" % (area, e), flush=True)
                    marks = [m for m in marks if m["area"] != area or m.get("pass")]
                    break
                if args.perf:   # (after the route: a camera turn as long as the route, profiled)
                    secs = args.still + args.turn + args.walk
                    perf_out = out / ("perf-%s-%d.data" % (area, n + 1))
                    t0 = time.monotonic()
                    perf = subprocess.Popen(["perf", "record", "-F", "499", "-e", "cpu-clock", "-p", str(proc.pid), "-o",
                                             str(perf_out), "--", "sleep", str(secs)], stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL)
                    bridge.request("stick", stick="R", x=1.0, y=0.0, frames=216000, exclusive=True, wait=False)
                    perf.wait()
                    bridge.request("release")
                    marks.append({"area": area, "part": "perf", "start": t0, "end": time.monotonic()})
                    perf_runs.append((area, n + 1, perf_out))
                for m in marks:
                    if m["area"] == area and "pass" not in m:
                        m["pass"] = n + 1
        bridge.request("quit")
    finally:
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
        game.stop.set()
        shutil.rmtree(sock_dir, ignore_errors=True)
        restore_ini(game_dir, out)
    frames_csv = next((f for f in (game_dir / "bin/hookshot_frames.csv", game_dir / "hookshot_frames.csv",
                       game_dir / "logs/hookshot_frames.csv") if f.exists()), None)   # (logs/: Hookshot 2.3)
    if frames_csv:
        shutil.move(str(frames_csv), out / "frames.csv")
        frames_csv = out / "frames.csv"
    # (the raw, timed data too: vr/tools/bench.py --again <run> summarises it again, e.g. after a change to the summary)
    (out / "raw.json").write_text(json.dumps({"marks": marks, "lines": game.lines, "samples": game.samples,
                                              "base_vram": base_vram}))
    for area, n, data in perf_runs:
        if data.exists():
            rep = subprocess.run(["perf", "report", "-i", str(data), "--no-children", "--sort", "comm,dso,symbol", "--stdio",
                                  "--percent-limit", "0.3"], capture_output=True, text=True)
            (out / ("perf-%s-%d.txt" % (area, n))).write_text(rep.stdout)
    results = summarise(game, marks, frames_csv, base_vram)
    if any(r.get("other_games") for r in results):
        print("bench: WARNING: another game ran during this run (see 'ANOTHER GAME RAN'): its numbers are not the build's own",
              flush=True)
    write_summary(out, vars(args), sets, results)


def write_summary(out, args, sets, results):
    """results.json and summary.md in the run folder (and the summary printed)"""
    if args["passes"] > 1:
        for r in results:
            r["area"] = "%s#%s" % (r["area"], r.get("pass", 1))
    (out / "results.json").write_text(json.dumps({"args": args, "sets": sets, "results": results}, indent=1))
    md = ["# Hookshot VR benchmark: %s" % out.name, "",
          "mode %s, FOV %s, profile %s, %d pass(es); settings: %s; extra environment: %s" % (
              args["mode"], args["fov"], args["profile"], args["passes"], ", ".join("%s=%s" % kv for kv in sets.items()),
              " ".join(args["env"]) or "none"), "",
          table(results), "", "## Each area's whole stay", "", area_table(results), "",
          "## The replay thread's time a frame (ms, the renderer's lines)", "", phases_table(results), "",
          "## CPU by thread", "", threads_table(results), ""]
    (out / "summary.md").write_text("\n".join(md))
    print("\n".join(md))


def again(run):
    """A run's raw.json summarised again (after a change to the summary's code)"""
    run = Path(run)
    raw = json.loads((run / "raw.json").read_text())
    old = json.loads((run / "results.json").read_text())
    game = Game.__new__(Game)
    game.lines = [tuple(x) for x in raw["lines"]]
    game.samples = [tuple(x) for x in raw["samples"]]
    frames = run / "frames.csv"
    write_summary(run, old["args"], old.get("sets", {}), summarise(game, raw["marks"], frames if frames.exists() else None,
                                                                    raw["base_vram"]))

if __name__ == "__main__":
    sys.exit(main())
