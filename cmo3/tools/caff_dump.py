#!/usr/bin/env python3
"""CAFF (cmo3/can3) 解包与结构对照工具。

用法:
    python caff_dump.py <file.cmo3> [outdir]        # 解包全部条目
    python caff_dump.py <file.cmo3> --list          # 仅列出条目表
"""
import io
import struct
import sys
import zipfile
import zlib
from pathlib import Path


class CaffReader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0
        self.key = 0

    def raw(self, n: int) -> bytes:
        b = self.data[self.pos:self.pos + n]
        if len(b) != n:
            raise EOFError(f"need {n} bytes at {self.pos}, got {len(b)}")
        self.pos += n
        return b

    def u8(self) -> int:
        return self.raw(1)[0]

    def eu8(self) -> int:
        return self.u8() ^ (self.key & 0xFF)

    def u16(self) -> int:
        return struct.unpack(">H", self.raw(2))[0]

    def u32(self) -> int:
        return struct.unpack(">I", self.raw(4))[0]

    def eu32(self) -> int:
        return self.u32() ^ (self.key & 0xFFFFFFFF)

    def i32(self) -> int:
        return struct.unpack(">i", self.raw(4))[0]

    def evarint(self) -> int:
        # 每字节先 XOR 再解析；高位为续位
        result = 0
        shift = 0
        for _ in range(4):
            b = self.eu8()
            result |= (b & 0x7F) << shift
            if not (b & 0x80):
                return result
            shift += 7
        raise ValueError("varint too long")

    def estring(self) -> str:
        n = self.evarint()
        return bytes(self.eu8() for _ in range(n)).decode("utf-8")


def unwrap_zip(blob: bytes) -> bytes:
    """解 ZIP 包裹的单文件 "contents"。

    官方写入器为流式 ZIP（flags bit3，无 central directory），
    zipfile 拒收，手工解析 local header + raw deflate。
    """
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as zf:
            return zf.read(zf.namelist()[0])
    except zipfile.BadZipFile:
        pass
    sig, _, _, method, _, _, _, _, _, nlen, elen = struct.unpack("<IHHHHHIIIHH", blob[:30])
    if sig != 0x04034B50:
        raise ValueError("not a zip local header")
    payload = blob[30 + nlen + elen:]
    if method == 8:
        return zlib.decompressobj(-15).decompress(payload)
    if method == 0:
        return payload
    raise ValueError(f"unsupported zip method {method}")


def parse(path: Path, outdir: Path | None, list_only: bool):
    data = path.read_bytes()
    r = CaffReader(data)

    magic = r.raw(4)
    if magic != b"CAFF":
        raise ValueError(f"bad magic: {magic!r}")
    archive_ver = tuple(r.raw(3))
    fmt_id = r.raw(4).decode("ascii", "replace")
    fmt_ver = tuple(r.raw(3))
    key_signed = r.i32()
    r.key = key_signed & 0xFFFFFFFF
    r.raw(8)  # reserved
    prev_fmt = r.u8()
    prev_color = r.u8()
    r.raw(2)
    prev_w = r.u16()
    prev_h = r.u16()
    r.raw(8)   # PreviewStartPosition (int64)
    r.raw(4)   # PreviewFileSize (int32)
    r.raw(8)   # padding
    # 对齐检查: 此时应位于 0x36
    print(f"magic=CAFF ver={archive_ver} fmt={fmt_id!r} v{fmt_ver} "
          f"key={key_signed} (0x{r.key:08X}) preview fmt={prev_fmt} color={prev_color} {prev_w}x{prev_h}")
    print(f"file table at 0x{r.pos:X} (expect 0x36)")

    count = r.eu32()
    print(f"file count = {count}")
    if count > 100000:
        raise ValueError("count implausible — layout mismatch")

    entries = []
    for i in range(count):
        name = r.estring()
        tag = r.estring()
        # int64 offset：官方写入器高 32 位为占位残留垃圾，仅低 32 位被回填有效
        r.eu32()
        offset = r.eu32()
        size = r.eu32()
        is_obf = r.eu8() != 0
        comp = r.eu8()
        r.raw(8)  # padding
        entries.append((name, tag, offset, size, is_obf, comp))

    data_begin = r.pos
    print(f"data begins at 0x{data_begin:X}")
    comp_names = {16: "RAW", 33: "FAST", 37: "SMALL"}
    for i, (name, tag, offset, size, is_obf, comp) in enumerate(entries):
        if i < 5 or i >= count - 2 or list_only:
            print(f"  [{i:4d}] {name!r:<40} tag={tag!r:<12} off=0x{offset:X} "
                  f"size={size} obf={is_obf} comp={comp_names.get(comp, comp)}")
    if not list_only and count > 7:
        print(f"  ... ({count - 7} more)")

    guard = data[-2:]
    print(f"guard bytes = {list(guard)} (expect [98, 99])")

    if list_only or outdir is None:
        return

    outdir.mkdir(parents=True, exist_ok=True)
    key8 = r.key & 0xFF
    for name, tag, offset, size, is_obf, comp in entries:
        blob = data[offset:offset + size]
        if is_obf:
            blob = bytes(b ^ key8 for b in blob)
        if comp in (33, 37):
            blob = unwrap_zip(blob)
        target = outdir / name.replace("/", "_").replace("\\", "_")
        target.write_bytes(blob)
    print(f"extracted {len(entries)} entries -> {outdir}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = Path(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == "--list":
        parse(src, None, True)
    else:
        out = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".extracted")
        parse(src, out, False)
