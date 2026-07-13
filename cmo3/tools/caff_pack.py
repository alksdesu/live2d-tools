#!/usr/bin/env python3
"""CAFF 打包器（C++ CCaffArchive::Save 的参照实现，用于往返校验）。

用法:
    python caff_pack.py roundtrip <file.cmo3>   # 解包→重打包→再解包，比较内容一致性
"""
import io
import struct
import sys
import zipfile
import zlib
from pathlib import Path

from caff_dump import CaffReader, unwrap_zip


def read_all(data: bytes):
    r = CaffReader(data)
    r.raw(4); r.raw(3); r.raw(4); r.raw(3)
    key = r.i32()
    r.key = key & 0xFFFFFFFF
    r.raw(8); r.raw(2); r.raw(2); r.raw(4); r.raw(8); r.raw(4); r.raw(8)
    count = r.eu32()
    entries = []
    for _ in range(count):
        name = r.estring()
        tag = r.estring()
        r.eu32()
        offset = r.eu32()
        size = r.eu32()
        is_obf = r.eu8() != 0
        comp = r.eu8()
        r.raw(8)
        entries.append([name, tag, offset, size, is_obf, comp])
    key8 = r.key & 0xFF
    result = []
    for name, tag, offset, size, is_obf, comp in entries:
        blob = data[offset:offset + size]
        if is_obf:
            blob = bytes(b ^ key8 for b in blob)
        if comp in (33, 37):
            blob = unwrap_zip(blob)
        result.append((name, tag, blob, is_obf, comp))
    return key, result


class CaffWriter:
    def __init__(self, key: int = 0):
        self.key = key & 0xFFFFFFFF
        self.buf = bytearray()

    def raw(self, b: bytes):
        self.buf += b

    def zeros(self, n: int):
        self.buf += bytes(n)

    def u8(self, v: int):
        self.buf.append(v & 0xFF)

    def eu8(self, v: int):
        self.u8(v ^ (self.key & 0xFF))

    def u16(self, v: int):
        self.buf += struct.pack(">H", v & 0xFFFF)

    def u32(self, v: int):
        self.buf += struct.pack(">I", v & 0xFFFFFFFF)

    def eu32(self, v: int):
        self.u32((v & 0xFFFFFFFF) ^ self.key)

    def patch_eu32(self, pos: int, v: int):
        self.buf[pos:pos + 4] = struct.pack(">I", (v & 0xFFFFFFFF) ^ self.key)

    def evarint(self, v: int):
        while True:
            b = v & 0x7F
            v >>= 7
            if v:
                b |= 0x80
            self.eu8(b)
            if not v:
                break

    def estring(self, s: str):
        raw = s.encode("utf-8")
        self.evarint(len(raw))
        for c in raw:
            self.eu8(c)

    def ebytes(self, b: bytes):
        k = self.key & 0xFF
        self.buf += bytes(x ^ k for x in b)


def zip_wrap(content: bytes) -> bytes:
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("contents", content)
    return bio.getvalue()


def pack(entries, key: int = 0) -> bytes:
    w = CaffWriter(key)
    w.raw(b"CAFF")
    w.zeros(3)
    w.raw(b"----")
    w.zeros(3)
    w.u32(key)
    w.zeros(8)
    w.u8(127)
    w.u8(127)
    w.zeros(2)
    w.u16(0)
    w.u16(0)
    w.zeros(8)
    w.zeros(4)
    w.zeros(8)

    w.eu32(len(entries))

    stored = []
    patch_pos = []
    for name, tag, content, is_obf, comp in entries:
        blob = zip_wrap(content) if comp in (33, 37) else content
        stored.append(blob)
        w.estring(name)
        w.estring(tag)
        w.eu32(0)
        patch_pos.append(len(w.buf))
        w.eu32(0)
        w.eu32(len(blob))
        w.eu8(1 if is_obf else 0)
        w.eu8(comp)
        w.zeros(8)

    for i, blob in enumerate(stored):
        w.patch_eu32(patch_pos[i], len(w.buf))
        if entries[i][3]:
            w.ebytes(blob)
        else:
            w.raw(blob)

    w.u8(98)
    w.u8(99)
    return bytes(w.buf)


def roundtrip(path: Path):
    original = path.read_bytes()
    key, entries = read_all(original)
    print(f"unpacked {len(entries)} entries (key={key})")

    repacked = pack(entries, key=0)
    print(f"repacked size = {len(repacked)} (original {len(original)})")

    key2, entries2 = read_all(repacked)
    assert key2 == 0, f"key mismatch: {key2}"
    assert len(entries) == len(entries2), "entry count mismatch"
    for (n1, t1, c1, o1, p1), (n2, t2, c2, o2, p2) in zip(entries, entries2):
        assert n1 == n2, f"name: {n1} != {n2}"
        assert t1 == t2, f"tag: {t1} != {t2}"
        assert o1 == o2 and p1 == p2, f"flags mismatch on {n1}"
        assert c1 == c2, f"content mismatch on {n1}: {len(c1)} vs {len(c2)}"
    print("ROUNDTRIP OK — all entries byte-identical after repack")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "roundtrip":
        print(__doc__)
        sys.exit(1)
    roundtrip(Path(sys.argv[2]))
