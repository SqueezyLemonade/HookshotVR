"""The dev map editor's server: the editor page, and what it needs from YOUR copy of the game, on this PC only.

    <builder python> server.py <romfs> [--port 8765] [--mods <folder>]
    then open http://127.0.0.1:8765

It reads the game's files on request (the actor catalog, a base map's actors, models) and never writes into the game's
folder except the dev map's own mod folder (--mods, default <game>/mods/hookshot-vr-devmap) when you press Build.
Layouts are saved in vr/devmap/layouts (your design: actor names and places, nothing of the game's). It listens on
127.0.0.1 only.
"""
import argparse
import io
import json
import math
import os
import re
import sys
import threading
import traceback
from contextlib import redirect_stdout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
DEVMAP = os.path.dirname(HERE)
sys.path.insert(0, DEVMAP)
sys.path.insert(0, os.path.join(DEVMAP, "..", "tools", "generate"))
import build_devmap  # noqa: E402
import prefab  # noqa: E402
import byml  # noqa: E402
import catalog  # noqa: E402
from game_files import GameFiles  # noqa: E402

LAYOUTS = os.path.join(DEVMAP, "layouts")
STATIC = {"/": ("index.html", "text/html"), "/editor.js": ("editor.js", "text/javascript"),
          "/editor.css": ("editor.css", "text/css")}
SAFE = re.compile(r"^[A-Za-z0-9_\-]+$")


class State:
    romfs = mods = None
    game = None
    cat = None
    models = None
    lock = threading.Lock()


def number(v):
    return v.get() if hasattr(v, "get") else v


def base_actors(name):
    """a base map's placed actors, for the editor to show around ours: name, place, rotation (degrees), scale, box"""
    g = State.game
    base = build_devmap.Base(g, name)
    out = []
    for layer in ("Static", "Dynamic"):
        rel = "Banc/%s/%s_%s.bcett.byml.zs" % (base.folder, base.file, layer)
        if not g.exists(rel): continue
        for a in byml.read(g.read(rel)).get("Actors", []):
            n = a.get("Gyaml")
            if "Translate" not in a: continue
            e = State.cat["actors"].get(n, {})
            out.append({"actor": n, "layer": layer, "name": a.get("Name", ""), "hash": "%016x" % a["Hash"].raw if "Hash" in a else "",
                        "baked": bool(a.get("Bakeable")) or bool(e.get("pre_actor")),
                        "position": [number(v) for v in a["Translate"]],
                        "rotation": [math.degrees(number(v)) for v in a.get("Rotate", [])] or [0, 0, 0],
                        "scale": [number(v) for v in a.get("Scale", [])] or [1, 1, 1],
                        "aabb": e.get("aabb"), "category": e.get("category", "Other"),
                        "left_out": any(n.startswith(x) for x in build_devmap.LEFT_OUT),
                        "model": e.get("FmdbName")})
    return {"name": name, "start": base.start, "start_rot": [math.degrees(v) for v in base.start_rot], "actors": out}


def bake(name):
    """a base map's baked lighting (Bake/Scene/<map>_*.bkres: SARC of a big-endian BYML): for each placed piece (its hash, as
    base_actors gives it), its atlas and, by the model's material index, where that material lies in it (scale x, y, offset
    x, y of the model's bake UVs). The atlas's red is ambient occlusion, its green a light's reach (DataType 3; DataType 5,
    the bounced light as spherical harmonics, is not read yet)"""
    import sarc
    g = State.game
    out = {}
    folder = g.path("Bake/Scene")
    if not os.path.isdir(folder): return out
    for f in sorted(os.listdir(folder)):
        if not f.startswith(name + "_") or not f.endswith(".bkres.zs"): continue
        try:
            files = sarc.read(g.read("Bake/Scene/" + f))
            files = files if isinstance(files, dict) else dict(files)
            data = byml.read(files["bkdat.byaml"])
        except Exception as e:
            print("dev map editor: bake %s not read: %s" % (f, e))
            continue
        for de in data.get("DataElements", []):
            if de["DataType"].get() != 3 or not de.get("TextureNames"): continue
            names = de["TextureNames"]
            for m in de.get("ModelElements", []):
                h = "%016x" % int(m["Guid"].split("_")[0])
                mats = {}
                for e in m.get("MaterialElements", []):
                    k = e["TextureIndex"].get()
                    if not 0 <= k < len(names): continue
                    sc, of = e["TexcoordScale"], e["TexcoordOffset"]
                    mats[e["OriginalMaterialIndex"].get()] = [sc["X"].get(), sc["Y"].get(), of["X"].get(), of["Y"].get(), names[k]]
                if mats: out[h] = mats
    return out


def lights(name):
    """a base map's placed lights (its LocationalLightScenePlacement, named by its settings, a parent's when it has none):
    name, kind (point, spot, amplifier: a lamp's glow), position"""
    g = State.game
    rel, seen = None, 0
    while name and seen < 6 and not rel:
        p = byml.read(g.read("Banc/GameBancParam/%s.game__banc__GameBancParam.bgyml" % name))
        path = p.get("LocationalLightScenePlacement")
        if path: rel = path.replace("Work/", "", 1).replace(".gyml", ".bgyml")
        par = p.get("$parent")
        name = par.split("/")[-1].split(".")[0] if par else None
        seen += 1
    out = []
    if not rel or not g.exists(rel): return out
    kinds = {399553389: "amplifier", 1420666488: "spot"}
    for group in (byml.read(g.read(rel)).get("GroupSetting") or {}).values():
        for n, e in (group.get("PlacementSetting") or {}).items():
            pos = e.get("Position")
            if not pos: continue
            t = e.get("LightType")
            out.append({"name": n, "kind": kinds.get(t.get() if t else 0, "point"),
                        "position": [pos[k].get() for k in "XYZ"]})
    return out


def bases():
    """the maps a dev map can be cloned from: those with their own settings file"""
    g = State.game
    names = sorted(f.split(".")[0] for f in os.listdir(g.path("Banc/GameBancParam")) if f.endswith(".bgyml"))
    return [n for n in names if n != build_devmap.NAME]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, kind="application/json"):
        if not isinstance(body, (bytes, bytearray)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = unquote(urlparse(self.path).path)
        try:
            if path in STATIC:
                f, kind = STATIC[path]
                with open(os.path.join(HERE, f), "rb") as fh:
                    return self.send(200, fh.read(), kind)
            if path == "/api/catalog":
                acts = State.cat["actors"]
                bx = build_devmap.BLOCK_SIZE
                own = [{"name": n, "category": "Blocks", "count": 0, "own": True, "size": bx,
                        "aabb": [[-bx[0] / 2, 0, -bx[2] / 2], [bx[0] / 2, bx[1], bx[2] / 2]], "pre_actor": False, "pack": True,
                        "model": None if "unseen" in how else acts.get(base, {}).get("FmdbName"), "maps": ["ours"],
                        "invisible": "unseen" in how}
                       for n, (base, how) in build_devmap.OWN.items()]
                return self.send(200, own + [{"name": n, "category": e["category"], "count": e["count"], "aabb": e.get("aabb"),
                                              "pre_actor": e.get("pre_actor", False), "pack": e.get("pack", False),
                                              "model": e.get("FmdbName"), "maps": e["maps"][:3]}
                                             for n, e in sorted(acts.items())])
            if path.startswith("/api/actor/"):
                n = path[len("/api/actor/"):]
                e = State.cat["actors"].get(build_devmap.OWN[n][0] if n in build_devmap.OWN else n)
                return self.send(200 if e else 404, e or {"error": "no such actor"})
            if path == "/api/prefabs":
                sets = [{"name": n, "from": sorted(ms)} for n, ms in sorted(State.cat.get("prefabs", {}).items())]
                wired = [{"name": w["module"], "label": "%s (%s)" % (w["label"], w["from"]), "from": [w["from"]], "group": w["group"], "wired": True}
                         for w in State.cat.get("wired", [])]
                return self.send(200, wired + sets)
            if path.startswith("/api/prefab/"):
                parts = path[len("/api/prefab/"):].split("/")
                with State.lock:
                    m = prefab.members(State.game, State.cat, parts[0], parts[1] if len(parts) > 1 and parts[1] else None, 0,
                                       parts[2] if len(parts) > 2 and parts[2] else None)
                for x in m["members"]:
                    e = State.cat["actors"].get(x["actor"], {})
                    x["aabb"], x["model"], x["category"] = e.get("aabb"), e.get("FmdbName"), e.get("category", "Other")
                return self.send(200, m)
            if path == "/api/marks":   # (places marked in the game: Menu + X, vr/devmap/devmap_host.c)
                marks = []
                try:
                    with open(os.path.join(State.mods, "marks.jsonl")) as fh:
                        for line in fh:
                            try: marks.append(json.loads(line))
                            except ValueError: pass
                except OSError:
                    pass
                return self.send(200, marks)
            if path == "/api/bases":
                return self.send(200, bases())
            if path.startswith("/api/bake/"):
                return self.send(200, bake(path[len("/api/bake/"):]))
            if path.startswith("/api/atlas/"):
                png = State.models.atlas(path[len("/api/atlas/"):]) if State.models else None
                return self.send(200, png, "image/png") if png else self.send(404, {"error": "no such atlas"})
            if path.startswith("/api/lights/"):
                return self.send(200, lights(path[len("/api/lights/"):]))
            if path.startswith("/api/base/"):
                return self.send(200, base_actors(path[len("/api/base/"):]))
            if path == "/api/layouts":
                return self.send(200, sorted(f[:-5] for f in os.listdir(LAYOUTS) if f.endswith(".json")))
            if path.startswith("/api/layout/"):
                n = path[len("/api/layout/"):]
                if not SAFE.match(n): return self.send(400, {"error": "bad name"})
                p = os.path.join(LAYOUTS, n + ".json")
                if not os.path.exists(p): return self.send(404, {"error": "no such layout"})
                with open(p, "rb") as fh:
                    return self.send(200, fh.read())
            if path.startswith("/api/model/"):
                return self.model(path[len("/api/model/"):])
            return self.send(404, {"error": "not found"})
        except Exception as e:
            traceback.print_exc()
            return self.send(500, {"error": str(e)})

    def model(self, name):
        """a model as the editor draws it (vr/devmap/editor/model_glb.py), or 404 when it cannot be read"""
        if not SAFE.match(name): return self.send(400, {"error": "bad name"})
        if State.models is None: return self.send(404, {"error": "models not available"})
        data = State.models.glb(name)
        return self.send(200, data, "model/gltf-binary") if data else self.send(404, {"error": "no model"})

    def do_POST(self):
        path = unquote(urlparse(self.path).path)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        try:
            if path.startswith("/api/layout/"):
                name = path[len("/api/layout/"):]
                if not SAFE.match(name): return self.send(400, {"error": "bad name"})
                layout = json.loads(body)
                with open(os.path.join(LAYOUTS, name + ".json"), "w") as fh:
                    json.dump(layout, fh, indent=1)
                    fh.write("\n")
                return self.send(200, {"saved": name})
            if path == "/api/marks/clear":
                try: os.remove(os.path.join(State.mods, "marks.jsonl"))
                except OSError: pass
                return self.send(200, {"cleared": True})
            if path.startswith("/api/build/"):
                name = path[len("/api/build/"):]
                if not SAFE.match(name): return self.send(400, {"error": "bad name"})
                log = io.StringIO()
                with State.lock, redirect_stdout(log):
                    build_devmap.build(State.romfs, State.mods, os.path.join(LAYOUTS, name + ".json"))
                return self.send(200, {"log": log.getvalue(), "mods": State.mods})
            return self.send(404, {"error": "not found"})
        except Exception as e:
            traceback.print_exc()
            return self.send(500, {"error": "%s: %s" % (type(e).__name__, e)})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("romfs")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--mods", default=None)
    a = ap.parse_args()
    State.romfs = os.path.abspath(a.romfs)
    State.mods = os.path.abspath(a.mods or os.path.join(os.path.dirname(State.romfs), "mods", "hookshot-vr-devmap"))
    State.game = GameFiles(State.romfs)
    print("dev map editor: reading the actor catalog (the first start takes a moment)")
    State.cat = catalog.load(State.romfs)
    try:
        import model_glb
        State.models = model_glb.Models(State.game, State.cat)
    except Exception as e:
        print("dev map editor: models not available (%s): boxes instead" % e)
    os.makedirs(LAYOUTS, exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print("dev map editor: http://127.0.0.1:%d  (mods: %s)" % (a.port, State.mods))
    srv.serve_forever()


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    main()
