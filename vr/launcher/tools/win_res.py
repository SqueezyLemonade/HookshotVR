"""The Windows launcher's resources as a .res its link takes (vr/launcher/build-windows.sh): the game's manifest (UTF-8 as the
process's code page, so argv, fopen and the A functions take the launcher's UTF-8 paths: nx2/win_manifest.py) under the
launcher's own name, and its icon (Explorer, the taskbar and the shortcuts --install writes) from a PNG: 256 px as PNG, and
48, 32 and 16 px when Pillow is there to make them.

    python3 -I vr/launcher/tools/win_res.py <out.res> [<icon.png>]
"""
import io
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "nx2"))
import win_manifest   # noqa: E402

RT_ICON, RT_GROUP_ICON = 3, 14


def icon_images(png):
    """(size, PNG bytes) of each size of the icon"""
    with open(png, "rb") as f:
        data = f.read()
    out = [(256, data)]
    try:
        from PIL import Image
    except ImportError:
        return out
    im = Image.open(io.BytesIO(data)).convert("RGBA")
    for s in (48, 32, 16):
        b = io.BytesIO()
        im.resize((s, s), Image.LANCZOS).save(b, "PNG")
        out.append((s, b.getvalue()))
    return out


def main():
    out = sys.argv[1]
    manifest = win_manifest.MANIFEST.replace(b'name="Hookshot.Game"', b'name="HookshotVR.Launcher"')
    res = win_manifest.res_bytes(manifest)
    if len(sys.argv) > 2 and os.path.isfile(sys.argv[2]):
        images = icon_images(sys.argv[2])
        group = struct.pack("<HHH", 0, 1, len(images))
        for i, (size, data) in enumerate(images, 1):
            res += win_manifest.res_entry(RT_ICON, i, data)
            group += struct.pack("<BBBBHHIH", size % 256, size % 256, 0, 0, 1, 32, len(data), i)
        res += win_manifest.res_entry(RT_GROUP_ICON, 1, group)
    with open(out, "wb") as f:
        f.write(res)
    return 0


if __name__ == "__main__":
    sys.exit(main())
