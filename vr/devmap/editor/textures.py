"""The dev map editor's textures: a material's colour texture from YOUR copy of the game (Tex/<name>.bntx.zs), as a small
PNG for the editor's models (model_glb.py). Kept in memory only.

A BNTX file holds one texture (BRTI): its format at +0x1c (the high byte the kind: 0x0b RGBA8, 0x1a-0x20 BC1-BC7,
0x2d-0x3a ASTC block sizes), size at +0x24, block height (log2, in GOBs) at +0x34, and its mip levels' offsets at +0x70.
The data is in the Switch's block-linear layout (64-byte by 8-row GOBs, stacked block-height GOBs tall), unswizzled here
for the first mip level, decoded with texture2ddecoder (in the builder's Python) and shrunk.
"""
import io
import struct

import numpy as np

# (format kind -> (block width, block height, bytes per block, decoder name))
FORMATS = {0x0b: (1, 1, 4, "rgba8"), 0x1a: (4, 4, 8, "decode_bc1"), 0x1b: (4, 4, 16, "decode_bc3"), 0x1c: (4, 4, 16, "decode_bc3"),
           0x1d: (4, 4, 8, "decode_bc4"), 0x1e: (4, 4, 16, "decode_bc5"), 0x1f: (4, 4, 16, "decode_bc6"),
           0x20: (4, 4, 16, "decode_bc7")}
ASTC = {0x2d: (4, 4), 0x2e: (5, 4), 0x2f: (5, 5), 0x30: (6, 5), 0x31: (6, 6), 0x32: (8, 5), 0x33: (8, 6), 0x34: (8, 8),
        0x35: (10, 5), 0x36: (10, 6), 0x37: (10, 8), 0x38: (10, 10), 0x39: (12, 10), 0x3a: (12, 12)}
for k, (bx, by) in ASTC.items():
    FORMATS[k] = (bx, by, 16, "astc")


def deswizzle(data, start, row_bytes, rows, block_height_log2):
    """the block-linear data of a surface row_bytes wide and rows tall (in blocks) as plain rows"""
    gob_rows = 8 << block_height_log2   # (rows in one block of GOBs)
    gobs_wide = (row_bytes + 63) // 64
    y = np.arange(rows)[:, None]
    x = np.arange(row_bytes)[None, :]
    off = ((y // gob_rows) * gobs_wide * (512 << block_height_log2) + (x // 64) * (512 << block_height_log2) +
           ((y % gob_rows) // 8) * 512 + ((x % 64) // 32) * 256 + ((y % 8) // 4) * 128 + ((x % 32) // 16) * 64 +
           ((y // 2) % 2) * 32 + (y % 2) * 16 + (x % 16))   # (a GOB's order as vr/tools/generate/icons.py, proven in game)
    src = np.frombuffer(data, dtype=np.uint8)
    off = off + start
    off = np.minimum(off, len(src) - 1)
    return src[off].tobytes()


def decode(bntx, size=256):
    """a BNTX file's first mip level as PNG bytes, at most size pixels across; None when its format is not one of these"""
    p = bntx.find(b"BRTI")
    if p < 0: return None
    fmt = struct.unpack_from("<I", bntx, p + 0x1c)[0]
    kind = fmt >> 8
    if kind not in FORMATS: return None
    w, h = struct.unpack_from("<II", bntx, p + 0x24)
    th = struct.unpack_from("<I", bntx, p + 0x34)[0] & 7
    table = struct.unpack_from("<Q", bntx, p + 0x70)[0]
    start = struct.unpack_from("<Q", bntx, table)[0]
    bw, bh, bpe, how = FORMATS[kind]
    cols, rows = (w + bw - 1) // bw, (h + bh - 1) // bh
    # (the block height shrinks for small surfaces: as tall as the surface needs, in GOBs)
    while th > 0 and rows <= (8 << (th - 1)): th -= 1
    plain = deswizzle(bntx, start, cols * bpe, rows, th)
    import texture2ddecoder
    from PIL import Image
    pw, ph = cols * bw, rows * bh
    if how == "rgba8":
        image = Image.frombytes("RGBA", (pw, ph), plain)
    else:
        if how == "astc": raw = texture2ddecoder.decode_astc(plain, pw, ph, bw, bh)
        else: raw = getattr(texture2ddecoder, how)(plain, pw, ph)
        image = Image.frombytes("RGBA", (pw, ph), raw, "raw", "BGRA")
    image = image.crop((0, 0, w, h))
    if kind in (0x1d,):   # (one channel: grey)
        image = image.convert("L").convert("RGB")
    scale = min(1.0, size / max(w, h))
    if scale < 1: image = image.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
    out = io.BytesIO()
    image.convert("RGB").save(out, "PNG", optimize=False)
    return out.getvalue()
