"""Reads files out of YOUR copy of the game (its romfs folder), for the generators in this folder.

Most of the game's files are zstd-compressed (.zs) with one of the game's own dictionaries, which sit in
Pack/ZsDic.pack.zs (a SARC archive, itself compressed without a dictionary). Nothing read here is copied into the
repository: the generators keep only the numbers they need (bone slots, bind matrices, mask values) in local headers.
"""
import os
import struct

ZSTD = b"\x28\xb5\x2f\xfd"


class GameFiles:
    def __init__(self, romfs):
        import zstandard   # (in the Hookshot builder's Python)
        self.romfs = romfs
        self.zstd = zstandard
        self._dictionaries = None

    def path(self, rel):
        return os.path.join(self.romfs, *rel.split("/"))

    def exists(self, rel):
        return os.path.isfile(self.path(rel))

    def dictionaries(self):
        """The game's zstd dictionaries: every file of Pack/ZsDic.pack.zs"""
        if self._dictionaries is None:
            with open(self.path("Pack/ZsDic.pack.zs"), "rb") as f:
                raw = self.zstd.ZstdDecompressor().decompress(f.read(), max_output_size=1 << 28)
            if raw[:4] != b"SARC":
                raise ValueError("Pack/ZsDic.pack.zs is not a SARC archive")
            start = struct.unpack_from("<I", raw, 0x0c)[0]
            count = struct.unpack_from("<H", raw, 0x1a)[0]
            self._dictionaries = []
            for k in range(count):
                _, _, first, end = struct.unpack_from("<IIII", raw, 0x20 + k * 16)
                self._dictionaries.append(raw[start + first:start + end])
        return self._dictionaries

    def read(self, rel):
        """A file of the romfs, decompressed when it is a .zs file"""
        with open(self.path(rel), "rb") as f:
            data = f.read()
        if data[:4] != ZSTD:
            return data
        z = self.zstd
        try:
            return z.ZstdDecompressor().decompress(data, max_output_size=1 << 31)
        except z.ZstdError:
            pass
        for d in self.dictionaries():
            try:
                full = z.ZstdCompressionDict(d, dict_type=z.DICT_TYPE_FULLDICT)
                return z.ZstdDecompressor(dict_data=full).decompress(data, max_output_size=1 << 31)
            except z.ZstdError:
                continue
        raise ValueError("%s: none of the game's dictionaries decompresses it" % rel)
