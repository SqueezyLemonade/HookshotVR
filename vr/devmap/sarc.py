"""SARC archives (the game's .pack files), read and written, for the dev map's own actors (vr/devmap).

read(bytes) -> {name: bytes}; write({name: bytes}) -> bytes. Little endian, version 0x100, the names' hash multiplier 0x65,
entries sorted by that hash, every file aligned to 8 bytes (BYML files keep their 64-bit values aligned).
"""
import struct

MULTIPLIER = 0x65


def name_hash(name):
    h = 0
    for c in name.encode():
        h = (h * MULTIPLIER + (c if c < 128 else c - 256)) & 0xffffffff
    return h


def read(b):
    assert b[:4] == b"SARC", b[:4]
    header = struct.unpack_from("<H", b, 4)[0]
    data = struct.unpack_from("<I", b, 12)[0]
    count = struct.unpack_from("<H", b, header + 6)[0]
    names = header + 12 + 16 * count + 8
    out = {}
    for i in range(count):
        _, attr, start, end = struct.unpack_from("<IIII", b, header + 12 + 16 * i)
        o = names + (attr & 0xffff) * 4
        out[b[o:b.index(b"\0", o)].decode()] = b[data + start:data + end]
    return out


def write(files, align=8):
    entries = sorted(files.items(), key=lambda kv: name_hash(kv[0]))
    names = bytearray()
    name_at = []
    for name, _ in entries:
        name_at.append(len(names) // 4)
        names += name.encode() + b"\0"
        while len(names) % 4: names.append(0)
    sfat = bytearray(b"SFAT" + struct.pack("<HHI", 12, len(entries), MULTIPLIER))
    blob = bytearray()
    for (name, data), at in zip(entries, name_at):
        while len(blob) % align: blob.append(0)
        sfat += struct.pack("<IIII", name_hash(name), 0x01000000 | at, len(blob), len(blob) + len(data))
        blob += data
    sfnt = b"SFNT" + struct.pack("<HH", 8, 0) + bytes(names)
    head = 0x14 + len(sfat) + len(sfnt)
    pad = (-head) % max(align, 0x80)   # (the data starts aligned)
    data_at = head + pad
    out = b"SARC" + struct.pack("<HHIIHH", 0x14, 0xfeff, data_at + len(blob), data_at, 0x100, 0)
    return bytes(out) + bytes(sfat) + sfnt + bytes(pad) + bytes(blob)
