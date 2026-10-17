#pragma once
#include <string>
#include <vector>

namespace lualoader::pinyin {

    /// 候选词条
    struct Candidate {
        std::string text;   // UTF-8（单字或词）
        unsigned int weight = 0;
    };

    /// 加载内置拼音词库（幂等）。返回是否就绪。
    bool init();

    /// 是否已就绪
    bool ready();

    /// 查询候选：input 为拼音串（小写字母，无空格）。
    /// 先精确匹配；无果则按前缀匹配。最多返回 max_n 条，按权重降序、去重。
    std::vector<Candidate> query(const std::string &input, size_t max_n);

} // namespace lualoader::pinyin
