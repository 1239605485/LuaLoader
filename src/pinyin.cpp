/*******************************************************************************
 * LuaLoader - pinyin：内置拼音输入法引擎（词库来自 rime/rime-pinyin-simp, Apache-2.0）
 * Copyright (C) 2026 lzup333
 *
 * 词库二进制由 tools/build_pinyin_dict.py + tools/gen_dict_header.py 生成，
 * 以 kPinyinDict[] 形式内嵌。格式见 tools/build_pinyin_dict.py 头部注释。
 *******************************************************************************/

#include "pinyin.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.hpp"

// 内嵌词库（生成物，7MB 头文件，仅此 TU 包含）
#include "../lib/ime/pinyin_simp_dict.h"

namespace {

    struct KeyRec {
        std::string key;
        unsigned int off = 0; // 候选区字节偏移
        unsigned int cnt = 0;
    };

    std::vector<KeyRec> g_keys;        // 按 key 升序
    const unsigned char *g_cands = nullptr;
    size_t g_cands_size = 0;
    bool g_tried = false;
    bool g_ok = false;

    inline unsigned int rd_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
    inline unsigned int rd_u32(const unsigned char *p) {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    }

    void load() {
        if (g_tried) return;
        g_tried = true;

        const unsigned char *d = kPinyinDict;
        const size_t n = kPinyinDictSize;
        if (n < 12 || std::memcmp(d, "PYS1", 4) != 0) {
            LOG_ERROR("[pinyin] 词库魔数错误");
            return;
        }
        const unsigned int n_keys = rd_u32(d + 4);
        const unsigned int n_ent = rd_u32(d + 8);
        size_t p = 12;
        g_keys.reserve(n_keys);

        // keys 区
        std::vector<std::pair<unsigned int, unsigned int>> offcnt; // 临时存 off/cnt
        offcnt.reserve(n_keys);
        for (unsigned int i = 0; i < n_keys; ++i) {
            if (p + 2 > n) return;
            const unsigned int klen = rd_u16(d + p);
            p += 2;
            if (p + klen + 8 > n) return;
            KeyRec r;
            r.key.assign(reinterpret_cast<const char *>(d + p), klen);
            p += klen;
            r.off = rd_u32(d + p);
            r.cnt = rd_u32(d + p + 4);
            p += 8;
            g_keys.push_back(std::move(r));
        }
        g_cands = d + p;
        g_cands_size = n - p;
        g_ok = true;
        LOG_INFO("[pinyin] 词库已加载: {} 键, {} 候选 ({:.1} MB)", n_keys, n_ent,
                 static_cast<double>(n) / (1024.0 * 1024.0));
    }

    // 读取某键的候选，追加到 out（去重由调用方用 set 负责）
    void read_cands(const KeyRec &k, size_t max_n,
                    std::vector<lualoader::pinyin::Candidate> &out,
                    std::set<std::string> &seen) {
        size_t off = k.off;
        for (unsigned int i = 0; i < k.cnt && out.size() < max_n; ++i) {
            if (off + 2 > g_cands_size) return;
            const unsigned int tlen = rd_u16(g_cands + off);
            off += 2;
            if (off + tlen + 2 > g_cands_size) return;
            std::string text(reinterpret_cast<const char *>(g_cands + off), tlen);
            off += tlen;
            const unsigned int w = rd_u16(g_cands + off);
            off += 2;
            if (seen.insert(text).second) {
                lualoader::pinyin::Candidate c;
                c.text = std::move(text);
                c.weight = w;
                out.push_back(std::move(c));
            }
        }
    }

    const KeyRec *find_exact(const std::string &input) {
        const auto it = std::lower_bound(
                g_keys.begin(), g_keys.end(), input,
                [](const KeyRec &a, const std::string &v) { return a.key < v; });
        if (it != g_keys.end() && it->key == input) return &*it;
        return nullptr;
    }

    // 前缀区间 [begin, end)
    void prefix_range(const std::string &prefix, size_t &begin, size_t &end) {
        begin = static_cast<size_t>(std::lower_bound(
                        g_keys.begin(), g_keys.end(), prefix,
                        [](const KeyRec &a, const std::string &v) { return a.key < v; }) -
                g_keys.begin());
        end = begin;
        while (end < g_keys.size() &&
               g_keys[end].key.compare(0, prefix.size(), prefix) == 0)
            ++end;
    }

} // namespace

namespace lualoader::pinyin {

    bool init() {
        load();
        return g_ok;
    }

    bool ready() {
        load();
        return g_ok;
    }

    std::vector<Candidate> query(const std::string &input, const size_t max_n) {
        std::vector<Candidate> out;
        if (max_n == 0) return out;
        load();
        if (!g_ok || input.empty()) return out;

        std::set<std::string> seen;
        if (const KeyRec *k = find_exact(input)) {
            read_cands(*k, max_n, out, seen);
            return out;
        }
        // 前缀匹配
        size_t b = 0, e = 0;
        prefix_range(input, b, e);
        for (size_t i = b; i < e && out.size() < max_n; ++i)
            read_cands(g_keys[i], max_n, out, seen);
        return out;
    }

} // namespace lualoader::pinyin
