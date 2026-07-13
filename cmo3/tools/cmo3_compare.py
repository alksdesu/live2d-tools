#!/usr/bin/env python3
"""生成 cmo3 与官方样本的结构对照。

用法:
    python cmo3_compare.py <generated.cmo3> <official.cmo3>

对照维度: 实体类型计数、xs.ref 引用完整性（零孤儿）、必填实体存在性。
"""
import io
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from caff_dump import CaffReader, unwrap_zip


def extract_main_xml(path: Path) -> str:
    data = path.read_bytes()
    r = CaffReader(data)
    r.raw(4); r.raw(3); r.raw(4); r.raw(3)
    key = r.i32()
    r.key = key & 0xFFFFFFFF
    r.raw(8); r.raw(2); r.raw(2); r.raw(4); r.raw(8); r.raw(4); r.raw(8)
    count = r.eu32()
    key8 = r.key & 0xFF
    for _ in range(count):
        name = r.estring()
        tag = r.estring()
        r.eu32()
        offset = r.eu32()
        size = r.eu32()
        is_obf = r.eu8() != 0
        comp = r.eu8()
        r.raw(8)
        if tag == "main_xml":
            blob = data[offset:offset + size]
            if is_obf:
                blob = bytes(b ^ key8 for b in blob)
            if comp in (33, 37):
                blob = unwrap_zip(blob)
            return blob.decode("utf-8")
    raise ValueError("no main_xml entry")


def entity_counts(xml: str) -> Counter:
    counts = Counter()
    for m in re.finditer(r'<(C[A-Z]\w+|KeyformGridSource|KeyformBindingSource|ModelImageFilterSet|'
                         r'FilterInstance|LayeredImageWrapper|GTexture2D)[ >]', xml):
        counts[m.group(1)] += 1
    return counts


def ref_integrity(xml: str):
    ids = set(re.findall(r'xs\.id="(#\d+)"', xml))
    refs = Counter(re.findall(r'xs\.ref="(#\d+)"', xml))
    orphans = {r: c for r, c in refs.items() if r not in ids}
    return len(ids), sum(refs.values()), orphans


REQUIRED = [
    "CModelSource", "CImageCanvas", "CParameterSourceSet", "CDrawableSourceSet",
    "CPartSourceSet", "CTextureManager", "CModelInfo", "CAffecterSourceSet",
    "CLayeredImage", "CLayerGroup", "CLayer", "CModelImage", "CModelImageGroup",
    "CImageResource", "ModelImageFilterSet", "LayeredImageWrapper",
]


def main():
    gen_path, ref_path = Path(sys.argv[1]), Path(sys.argv[2])
    gen_xml = extract_main_xml(gen_path)
    ref_xml = extract_main_xml(ref_path)

    gen_counts = entity_counts(gen_xml)
    ref_counts = entity_counts(ref_xml)

    print(f"{'entity':<36}{'generated':>10}{'official':>10}")
    for name in sorted(set(gen_counts) | set(ref_counts)):
        g, r = gen_counts.get(name, 0), ref_counts.get(name, 0)
        flag = "" if (g > 0) == (r > 0) else "  <-- presence differs"
        print(f"{name:<36}{g:>10}{r:>10}{flag}")

    print("\n--- required entities in generated ---")
    missing = [name for name in REQUIRED if gen_counts.get(name, 0) == 0]
    print("MISSING:", missing if missing else "none")

    print("\n--- xs.ref integrity (generated) ---")
    n_ids, n_refs, orphans = ref_integrity(gen_xml)
    print(f"ids={n_ids} refs={n_refs} orphan_refs={len(orphans)}")
    if orphans:
        for r, c in list(orphans.items())[:10]:
            print(f"  orphan {r} x{c}")

    print("\n--- well-known root deformer ---")
    print("ROOT uuid present:", "71fae776-e218-4aee-873e-78e8ac0cb48a" in gen_xml)


if __name__ == "__main__":
    main()
