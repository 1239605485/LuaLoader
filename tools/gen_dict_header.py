#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_dict_header.py —— 把 lib/ime/pinyin_simp.dict 生成嵌入用的 C 头文件。

输出：lib/ime/pinyin_simp_dict.h
  static const unsigned char kPinyinDict[] = { ... };
  static const unsigned int  kPinyinDictSize = <n>;

用法： python3 tools/gen_dict_header.py
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "lib", "ime", "pinyin_simp.dict")
DST = os.path.join(ROOT, "lib", "ime", "pinyin_simp_dict.h")


def main():
    if not os.path.exists(SRC):
        print("缺少输入: " + SRC, file=sys.stderr)
        return 1
    data = open(SRC, "rb").read()
    with open(DST, "w", encoding="utf-8") as f:
        f.write("// 由 tools/gen_dict_header.py 自动生成，请勿手改。\n")
        f.write("// 数据来源：rime/rime-pinyin-simp (Apache-2.0)，见 THIRD_PARTY_NOTICES.md。\n")
        f.write("static const unsigned char kPinyinDict[] = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("  " + ",".join("0x%02x" % b for b in chunk) + ",\n")
        f.write("};\n")
        f.write("static const unsigned int kPinyinDictSize = %d;\n" % len(data))
    print("wrote %s (%d bytes data, %d bytes header)" %
          (os.path.relpath(DST, ROOT), len(data), os.path.getsize(DST)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
