"""vk_vr_wrist_maps.h: the wrist HUD's minimap pictures (vr/menu/wrist_draw.inc, Wrist HUD Full), from your game's own map
menu: each area's map (UI/CustomTex/Map/Field/<area>_<age>_NN) and each dungeon floor's (Dungeon/<dungeon>_<floor>_Back
under its _Main), with where each lies in the world.

Where a picture lies: Game/MapCaptureInfo's entries, in the order of Game/MapMenuInfo's (the map menu's areas), are the
top-down cameras the pictures were taken with: a picture is Height metres wide, centred on CameraPos (x, z), x to the
right and z down the picture (found 22 Nov by fitting the map's own icon places, Banc/BancBasedResource/MapParam, onto
the pictures). An area's later pictures (_01, ...) are shown once their entry's
LightParamData flag (GameDataName, a Bool) is set; a dungeon's floors by Link's height (MapParam's Floor TransY).
Each picture is cropped to what it shows (its alpha), scaled to fit MAX_W x MAX_H, premultiplied (sRGB) and zstd-packed,
once however many entries show it. Dungeon floors: the first of each name kind (a water level's Lv1, a state's State00).
The Lost Woods and the Haunted Wasteland (IsInvisibleMapIcon: the map menu shows them in fog): none. Not a GENERATORS module: the pictures take a while to decode, so the header is made again only when its
inputs or this file change (its first line's key), as panel_frame.py's. A game without these files: a header with no
maps (Full shows the Simple panel).
    python wrist_maps.py <romfs> <output folder> [preview.png]
"""
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = "vk_vr_wrist_maps.h"
MAX_W, MAX_H = 384, 264   # (the minimap's largest room on the panel, pixels: vr/menu/wrist_draw.inc kWristMapW, kWristMapH)
MENU = "Game/MapMenuInfo/MapMenuInfo.game__ui__MapMenuInfo.bgyml"
CAPTURE = "Game/MapCaptureInfo/MapCaptureInfo.game__MapCaptureInfo.bgyml"
PARAM = "Banc/BancBasedResource/MapParam.byml.zs"
TEXTURES = "UI/CustomTex/Map/"
ASTC = {0x2d: (4, 4), 0x2e: (5, 4), 0x2f: (5, 5), 0x30: (6, 5), 0x31: (6, 6), 0x32: (8, 5), 0x33: (8, 6), 0x34: (8, 8)}
FLOOR_SLACK = 1.5   # (metres: Link stands a little above his floor's TransY)


def murmur3(text):
    """MurmurHash3 x86 32, seed 0: the game's flag names (host/oot_state_tap.c state_murmur)"""
    d, h, c1, c2 = text.encode(), 0, 0xcc9e2d51, 0x1b873593
    rotl = lambda v, r: ((v << r) | (v >> (32 - r))) & 0xffffffff
    n = len(d) // 4 * 4
    for i in range(0, n, 4):
        k = rotl(int.from_bytes(d[i:i + 4], "little") * c1 & 0xffffffff, 15) * c2 & 0xffffffff
        h = (rotl(h ^ k, 13) * 5 + 0xe6546b64) & 0xffffffff
    if len(d) > n:
        h ^= rotl(int.from_bytes(d[n:], "little") * c1 & 0xffffffff, 15) * c2 & 0xffffffff
    h ^= len(d)
    for shift, mul in ((16, 0x85ebca6b), (13, 0xc2b2ae35)):
        h = (h ^ (h >> shift)) * mul & 0xffffffff
    return h ^ (h >> 16)


def fnv1a(text):
    """the banc's name as the game side sends it (vr/menu/wrist_host.c vr_wrist_banc_hash)"""
    h = 0x811c9dc5
    for b in text.encode():
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def plain(v):
    import byml
    if isinstance(v, dict):
        return {k: plain(x) for k, x in v.items()}
    if isinstance(v, list):
        return [plain(x) for x in v]
    return v.get() if isinstance(v, byml.Value) else v


def byml_file(game, rel):
    sys.path.insert(0, os.path.join(HERE, "..", "..", "devmap"))
    try:
        import byml
        return plain(byml.read(game.read(rel)))
    finally:
        sys.path.pop(0)


def picture(d):
    """a map BNTX (one ASTC texture) as a PIL RGBA image"""
    import texture2ddecoder
    from PIL import Image
    p = d.find(b"BRTI")
    assert p >= 0
    fmt = struct.unpack_from("<I", d, p + 0x1c)[0]
    assert fmt >> 8 in ASTC, hex(fmt)
    bw, bh = ASTC[fmt >> 8]
    w, h = struct.unpack_from("<II", d, p + 0x24)
    th = struct.unpack_from("<I", d, p + 0x34)[0] & 7
    start = struct.unpack_from("<Q", d, struct.unpack_from("<Q", d, p + 0x70)[0])[0]
    nx, ny = (w + bw - 1) // bw, (h + bh - 1) // bh
    gobs = (nx * 16 + 63) // 64
    out = bytearray(nx * ny * 16)
    for by in range(ny):   # (block-linear: icons.py's unswizzle, for any block size)
        row = (by >> (3 + th)) * gobs * (512 << th) + ((by >> 3) & ((1 << th) - 1)) * 512 + ((by & 7) >> 2) * 128 + \
              ((by >> 1) & 1) * 32 + (by & 1) * 16
        for bx in range(nx):
            x = bx * 16
            off = start + row + (x >> 6) * (512 << th) + ((x & 63) >> 5) * 256 + ((x & 31) >> 4) * 64
            o = (by * nx + bx) * 16
            out[o:o + 16] = d[off:off + 16]
    bgra = texture2ddecoder.decode_astc(bytes(out), w, h, bw, bh)
    return Image.frombytes("RGBA", (w, h), bgra, "raw", "BGRA")


def inputs(game):
    rels = [MENU, CAPTURE, PARAM]
    for sub in ("Field", "Dungeon"):
        folder = game.path(TEXTURES + sub)
        if os.path.isdir(folder):
            rels += [TEXTURES + sub + "/" + f for f in sorted(os.listdir(folder))]
    return rels


def maps(game):
    """[(banc, flag hash or 0, floor y from, to, picture)] and each picture's world rect: (x0, z0, x1, z1)"""
    from PIL import Image
    menu = byml_file(game, MENU)["MapMenuParamArray"]
    capture = byml_file(game, CAPTURE)["MapCaptureParamArray"]
    param = byml_file(game, PARAM)
    out = []
    for entry, cap in zip(menu, capture):
        if "CameraPos" not in cap or "Height" not in cap or entry.get("IsInvisibleMapIcon"):
            continue
        cx, cz, width = cap["CameraPos"]["X"], cap["CameraPos"]["Z"], cap["Height"]
        bancs, names = entry.get("BancName") or [], entry.get("TextureName") or []
        lights = cap.get("LightParamData") or {}
        if names and names[0] and not names[0].endswith("_00"):   # (a dungeon: its floors, Back under Main, by Link's height)
            d, name = param.get(names[0]), names[0]
            if not d or "Floor" not in d:
                continue
            floors = d["Floor"]
            for i, floor in enumerate(floors):
                f = d["MinFloor"] + i
                tag = ("B%d" % -f) if f < 0 else "%dF" % (f + 1)
                layers = []
                for parts in (("Back", "State00_Back"), ("Main", "State00_Main", "Lv1_Main", "Lv1_State00_Main")):
                    rel = next((r for r in (TEXTURES + "Dungeon/%s_%s_%s.bntx.zs" % (name, tag, p) for p in parts)
                                if game.exists(r)), None)
                    if rel:
                        layers.append(picture(game.read(rel)))
                if not layers:
                    continue
                im = layers[0]
                for top in layers[1:]:
                    im = Image.alpha_composite(im, top)
                y0 = floor.get("TransY", -1e4) - FLOOR_SLACK if i else -1e4
                y1 = floors[i + 1].get("TransY", 1e4) - FLOOR_SLACK if i + 1 < len(floors) else 1e4
                out.append((name, 0, y0, y1, im, (cx, cz, width)))
            continue
        for age, (banc, tex) in enumerate(zip(bancs, names)):
            if not banc or not tex or not tex.endswith("_00"):
                continue
            also = [c.get("BancName") for c in entry.get("ChangeBancInfo") or [] if c.get("BancName", "").endswith(
                ("_Adult_Alternative" if age else "_Child_Alternative"))]
            for k in range(4):
                rel = TEXTURES + "Field/%s_%02d.bntx.zs" % (tex[:-3], k)
                if not game.exists(rel):
                    continue
                flag = (lights.get("%02d" % k) or {}).get("GameDataBoolParam", {}).get("GameDataName") if k else None
                if k and not flag:
                    continue   # (a later picture with no flag of its own: not known when it shows)
                im = picture(game.read(rel))
                for b in [banc] + also:
                    out.append((b, murmur3(flag) if flag else 0, -1e4, 1e4, im, (cx, cz, width)))
    return out


def literal(data):
    out, line, width = [], [], 0
    for b in data:
        piece = "\\%03o" % b if b < 32 or b > 126 or b in (34, 63, 92) else chr(b)
        line.append(piece)
        width += len(piece)
        if width >= 120:
            out.append('"%s"' % "".join(line))
            line, width = [], 0
    if line:
        out.append('"%s"' % "".join(line))
    return "\n".join(out) or '""'


def header(game, key, preview=None):
    import zstandard
    from PIL import Image
    lines = ["// key %s" % key,
             "// Generated by vr/tools/generate (wrist_maps.py) from your game's map menu pictures.",
             "// Local file: do not commit or share.", "#pragma once", "namespace mkwvk::vr::art {",
             "// (banc: FNV-1a of its name; flag: the Bool (MurmurHash3) that shows this picture, 0 none; floor: Link's height"
             " from y0 to y1; the picture: x0..x1 left to right, z0..z1 top to bottom, metres; w x h premultiplied sRGB RGBA,"
             " zstd)",
             "struct WristMap { unsigned banc, flag; float y0, y1, x0, z0, x1, z1; int w, h; const char* zst; unsigned size; };"]
    found = maps(game) if game.exists(MENU) and game.exists(CAPTURE) and game.exists(PARAM) else []
    entries, previews, packs, seen = [], [], {}, set()
    for banc, flag, y0, y1, im, (cx, cz, width) in found:
        if (banc, flag, y0) in seen:
            continue
        seen.add((banc, flag, y0))
        W, H = im.size
        height = width * H / W
        box = im.split()[3].point(lambda a: 255 if a > 24 else 0).getbbox()
        if not box:
            continue
        im = im.crop(box)
        x0, x1 = cx - width / 2 + width * box[0] / W, cx - width / 2 + width * box[2] / W
        z0, z1 = cz - height / 2 + height * box[1] / H, cz - height / 2 + height * box[3] / H
        s = min(1.0, MAX_W / im.size[0], MAX_H / im.size[1])
        im = im.resize((max(1, round(im.size[0] * s)), max(1, round(im.size[1] * s))), Image.LANCZOS)
        px = bytearray(im.tobytes())
        for i in range(0, len(px), 4):
            a = px[i + 3]
            px[i], px[i + 1], px[i + 2] = px[i] * a // 255, px[i + 1] * a // 255, px[i + 2] * a // 255
        digest = hashlib.sha256(bytes(px)).hexdigest()
        if digest not in packs:
            packed = zstandard.ZstdCompressor(level=19).compress(bytes(px))
            n = packs[digest] = (len(packs), len(packed))
            lines.append("inline constexpr char wrist_map_%d[] =\n%s;" % (n[0], literal(packed)))
            previews.append(im)
        n, size = packs[digest]
        entries.append("{%#010xu, %#010xu, %.2ff, %.2ff, %.3ff, %.3ff, %.3ff, %.3ff, %d, %d, wrist_map_%d, %du}, // %s" % (
            fnv1a(banc), flag, y0, y1, x0, z0, x1, z1, im.size[0], im.size[1], n, size, banc))
    lines.append("inline constexpr int wrist_map_count = %d;" % len(entries))
    lines.append("inline constexpr WristMap wrist_maps[%d] = {\n%s\n};" % (max(1, len(entries)), "\n".join(entries) or
                                                                         "{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \"\", 0}"))
    lines.append("}")
    if preview and previews:
        sheet = Image.new("RGBA", (MAX_W * 8, MAX_H * ((len(previews) + 7) // 8)), (30, 20, 12, 255))
        for k, im in enumerate(previews):
            sheet.alpha_composite(im, ((k % 8) * MAX_W, (k // 8) * MAX_H))
        sheet.convert("RGB").save(preview)
    return "\n".join(lines) + "\n"


def write(romfs, out_dir, preview=None):
    """Writes the header into out_dir when its inputs or this file changed; True when it did"""
    from game_files import GameFiles
    game = GameFiles(romfs)
    digest = hashlib.sha256(open(os.path.abspath(__file__), "rb").read())
    for rel in inputs(game):
        digest.update(rel.encode() + b"\0" + (open(game.path(rel), "rb").read() if game.exists(rel) else b""))
    key = digest.hexdigest()
    path = os.path.join(out_dir, OUTPUT)
    try:
        with open(path, encoding="utf-8") as f:
            if f.readline().strip() == "// key %s" % key and not preview:
                return False
    except OSError:
        pass
    text = header(game, key, preview)
    os.makedirs(out_dir, exist_ok=True)
    with open(path + ".new", "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(path + ".new", path)
    return True


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    write(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
