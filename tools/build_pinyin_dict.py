#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_pinyin_dict.py —— 把 rime 拼音词库 (pinyin_simp.dict.yaml) 转成紧凑二进制。

输入：lib/ime/pinyin_simp.dict.yaml   （格式： 文本<TAB>拼音(音节空格分隔)[<TAB>权重]）
输出：lib/ime/pinyin_simp.dict        （二进制，供 LuaLoader 内置拼音输入法加载）

二进制格式（全部小端）：
  magic   : 4 bytes  "PYS1"
  n_keys  : u32      键（拼音串，ASCII，去空格）数量
  n_ent   : u32      候选总数
  keys    : n_keys 条，按 key 字节序升序：
              u16 keylen, key[keylen]
              u32 cand_off   （候选区字节偏移）
              u32 cand_cnt
  cands   : n_ent 条：
              u16 textlen, text[textlen]  (UTF-8)
              u16 weight                  （越大越靠前）

用法： python3 tools/build_pinyin_dict.py
"""
import os
import struct
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "lib", "ime", "pinyin_simp.dict.yaml")
DST = os.path.join(ROOT, "lib", "ime", "pinyin_simp.dict")


def parse(path):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line in ("---", "..."):
                continue
            if line.startswith(("name:", "version:", "sort:", "columns:")):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            text = parts[0].strip()
            code = parts[1].strip()
            weight = 0
            if len(parts) >= 3 and parts[2].strip():
                try:
                    weight = int(parts[2].strip())
                except ValueError:
                    weight = 0
            key = code.replace(" ", "").lower()
            if text and key:
                out.append((text, key, weight))
    return out


def main():
    if not os.path.exists(SRC):
        print("缺少输入: " + SRC, file=sys.stderr)
        return 1

    by_key = defaultdict(list)
    for text, key, weight in parse(SRC):
        by_key[key].append((text, weight))

    keys = sorted(by_key.keys())

    cands = bytearray()
    key_records = []
    for k in keys:
        items = sorted(by_key[k], key=lambda t: t[1], reverse=True)
        kb = k.encode("ascii", "ignore")
        key_records.append((kb, len(cands), len(items)))
        for text, weight in items:
            tb = text.encode("utf-8")
            cands += struct.pack("<H", len(tb)) + tb + struct.pack("<H", weight & 0xFFFF)

    with open(DST, "wb") as f:
        f.write(b"PYS1")
        f.write(struct.pack("<I", len(key_records)))
        f.write(struct.pack("<I", sum(c for _, _, c in key_records)))
        for kb, off, cnt in key_records:
            f.write(struct.pack("<H", len(kb)) + kb)
            f.write(struct.pack("<I", off))
            f.write(struct.pack("<I", cnt))
        f.write(cands)

    print("keys=%d candidates=%d -> %s (%d bytes)" %
          (len(key_records), sum(c for _, _, c in key_records),
           os.path.relpath(DST, ROOT), os.path.getsize(DST)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
