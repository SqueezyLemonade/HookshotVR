"""BYML (version 7, little endian) read and written with every value's type kept, for the dev map (vr/devmap).

A dict is a Python dict, an array a list, a string a str; every other value is a Value(type, raw) holding the node type and
its raw bits (a float's u32 bits, a 64-bit value's u64 bits), so a file read and written again is the same data. Values the
dev map makes: Value.float(x), Value.u64(n), Value.int(n), Value.uint(n), Value.bool(b).
Node types: 0xa0 string, 0xc0 array, 0xc1 dict, 0xc2 string table, 0xd0 bool, 0xd1 int, 0xd2 float, 0xd3 uint,
0xd4 int64, 0xd5 uint64, 0xd6 double, 0xff null.
"""
import struct

WIDE = (0xd4, 0xd5, 0xd6)   # (64-bit: stored elsewhere, the node holds their offset)


class Value:
    __slots__ = ("t", "raw")

    def __init__(self, t, raw):
        self.t, self.raw = t, raw

    def __repr__(self):
        return "Value(%#x, %r)" % (self.t, self.get())

    def get(self):
        if self.t == 0xd2: return struct.unpack("<f", struct.pack("<I", self.raw))[0]
        if self.t == 0xd6: return struct.unpack("<d", struct.pack("<Q", self.raw))[0]
        if self.t == 0xd1: return struct.unpack("<i", struct.pack("<I", self.raw))[0]
        if self.t == 0xd4: return struct.unpack("<q", struct.pack("<Q", self.raw))[0]
        if self.t == 0xd0: return bool(self.raw)
        return self.raw

    @staticmethod
    def float(x): return Value(0xd2, struct.unpack("<I", struct.pack("<f", x))[0])
    @staticmethod
    def int(n): return Value(0xd1, n & 0xffffffff)
    @staticmethod
    def uint(n): return Value(0xd3, n & 0xffffffff)
    @staticmethod
    def u64(n): return Value(0xd5, n & 0xffffffffffffffff)
    @staticmethod
    def bool(b): return Value(0xd0, 1 if b else 0)


def read(b):
    """a BYML v7 file (little-endian, YB; or big-endian, BY: the bake files) as dicts, lists, strings and Values"""
    assert b[:2] in (b"YB", b"BY"), "not a BYML file"
    e = "<" if b[:2] == b"YB" else ">"
    assert struct.unpack_from(e + "H", b, 2)[0] == 7, "not BYML version 7"
    ko, so, ro = struct.unpack_from(e + "III", b, 4)

    def head(o):   # (a node's type and count: the type byte, then a 24-bit count, in either order of bytes)
        w = struct.unpack_from(e + "I", b, o)[0]
        return (b[o], w >> 8) if e == "<" else (b[o], w & 0xffffff)

    def strtab(o):
        if not o: return []
        t, n = head(o)
        assert t == 0xc2
        offs = struct.unpack_from(e + "%dI" % n, b, o + 4)
        return [b[o + x:b.index(b"\0", o + x)].decode() for x in offs]

    keys, strs = strtab(ko), strtab(so)

    def val(t, v):
        if t == 0xa0: return strs[v]
        if t in (0xc0, 0xc1): return node(v)
        if t in WIDE: return Value(t, struct.unpack_from(e + "Q", b, v)[0])
        return Value(t, v)

    def node(o):
        t, n = head(o)
        if t == 0xc1:
            out = {}
            for i in range(n):
                k, v = struct.unpack_from(e + "II", b, o + 4 + 8 * i)
                key, typ = (k & 0xffffff, k >> 24) if e == "<" else (k >> 8, k & 0xff)
                out[keys[key]] = val(typ, v)
            return out
        if t == 0xc0:
            types = b[o + 4:o + 4 + n]
            base = o + 4 + ((n + 3) & ~3)
            return [val(types[i], struct.unpack_from(e + "I", b, base + 4 * i)[0]) for i in range(n)]
        raise ValueError("BYML node type %#x not handled" % t)

    return node(ro) if ro else None


def write(root):
    keys, strs = set(), set()

    def collect(x):
        if isinstance(x, dict):
            for k, v in x.items(): keys.add(k), collect(v)
        elif isinstance(x, list):
            for v in x: collect(v)
        elif isinstance(x, str): strs.add(x)

    collect(root)
    keys = sorted(keys, key=lambda s: s.encode())
    strs = sorted(strs, key=lambda s: s.encode())
    key_at = {k: i for i, k in enumerate(keys)}
    str_at = {s: i for i, s in enumerate(strs)}
    out = bytearray(16)

    def align(n):
        while len(out) % n: out.append(0)

    def table(items):
        if not items: return 0
        align(4)
        start = len(out)
        enc = [s.encode() + b"\0" for s in items]
        out.extend(struct.pack("<I", 0xc2 | len(items) << 8))
        at = 4 + 4 * (len(items) + 1)
        for e in enc: out.extend(struct.pack("<I", at)); at += len(e)
        out.extend(struct.pack("<I", at))
        for e in enc: out.extend(e)
        return start

    ko, so = table(keys), table(strs)

    def type_of(x):
        if isinstance(x, dict): return 0xc1
        if isinstance(x, list): return 0xc0
        if isinstance(x, str): return 0xa0
        return x.t

    def node(x):   # (a container: its offset; its children written after it, their slots patched)
        align(4)
        start = len(out)
        items = list(x.items()) if isinstance(x, dict) else list(enumerate(x))
        if isinstance(x, dict):
            items.sort(key=lambda kv: kv[0].encode())
            out.extend(struct.pack("<I", 0xc1 | len(items) << 8))
            slots = []
            for k, v in items:
                out.extend(struct.pack("<I", key_at[k] | type_of(v) << 24))
                slots.append(len(out)), out.extend(b"\0\0\0\0")
        else:
            out.extend(struct.pack("<I", 0xc0 | len(items) << 8))
            out.extend(bytes(type_of(v) for _, v in items))
            align(4)
            slots = []
            for _ in items: slots.append(len(out)), out.extend(b"\0\0\0\0")
        for slot, (_, v) in zip(slots, items):
            if isinstance(v, (dict, list)): p = node(v)
            elif isinstance(v, str): p = str_at[v]
            elif v.t in WIDE:
                align(8)
                p = len(out)
                out.extend(struct.pack("<Q", v.raw))
            else: p = v.raw
            struct.pack_into("<I", out, slot, p)
        return start

    ro = node(root)
    out[0:16] = b"YB" + struct.pack("<HIII", 7, ko, so, ro)
    return bytes(out)
