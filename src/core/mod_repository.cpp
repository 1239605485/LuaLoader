/*******************************************************************************
 * LuaLoader - static Mod repository client
 *******************************************************************************/

#include "mod_repository.hpp"

#include "json.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace {

bool safe_component(const std::string &value) {
    if (value.empty() || value == "." || value == "..") return false;
    for (const unsigned char c: value) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool safe_relative_path(const std::string &value) {
    fs::path path(value);
    if (value.empty() || path.is_absolute()) return false;
    for (const auto &part: path) {
        if (part == ".." || part == ".") return false;
    }
    return true;
}

class sha256 {
public:
    void update(const unsigned char *data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            buffer_[length_ & 63u] = data[i];
            ++length_;
            if ((length_ & 63u) == 0) transform(buffer_.data());
        }
    }

    std::string finish() {
        const uint64_t bit_length = length_ * 8;
        const unsigned char one = 0x80;
        update(&one, 1);
        const unsigned char zero = 0;
        while ((length_ & 63u) != 56u) update(&zero, 1);
        unsigned char length_bytes[8];
        for (int i = 0; i < 8; ++i) length_bytes[7 - i] = static_cast<unsigned char>(bit_length >> (i * 8));
        update(length_bytes, 8);
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (uint32_t value: state_) out << std::setw(8) << value;
        return out.str();
    }

private:
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

    void transform(const unsigned char *data) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) | (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(data[i * 4 + 2]) << 8) | data[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t t1 = h + s1 + ch(e, f, g) + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t t2 = s0 + maj(a, b, c);
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<unsigned char, 64> buffer_{};
    uint64_t length_{0};
    std::array<uint32_t, 8> state_{{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}};
};

std::string file_sha256(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    sha256 digest;
    std::array<unsigned char, 1024 * 64> buffer{};
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) digest.update(buffer.data(), static_cast<size_t>(count));
    }
    return digest.finish();
}

} // namespace

namespace lualoader::repository {

bool load_index(const fs::path &index_path, repository_index &out, std::string &error) {
    try {
        std::ifstream input(index_path);
        if (!input) { error = "cannot open index"; return false; }
        const auto json = nlohmann::json::parse(input);
        if (!json.contains("mods") || !json["mods"].is_array()) {
            error = "unsupported repository index"; return false;
        }
        repository_index parsed;
        parsed.schema_version = 1;
        parsed.repository = json.value("repository", std::string{});
        for (const auto &item: json["mods"]) {
            package_entry entry;
            entry.pkg_id = item.value("pkgId", std::string{});
            entry.download_url = item.value("downloadUrl", std::string{});
            entry.sha256 = item.value("sha256", std::string{});
            entry.native = item.value("native", false);
            if (!safe_component(entry.pkg_id) || entry.download_url.empty() || entry.native) {
                error = "invalid or native package entry"; return false;
            }
            parsed.packages.push_back(std::move(entry));
        }
        out = std::move(parsed);
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

bool resolve_download_url(const std::string &index_url, const package_entry &entry,
                          std::string &out, std::string &error) {
    if (!safe_component(entry.pkg_id) || entry.download_url.empty()) {
        error = "invalid package download URL"; return false;
    }
    if (entry.download_url.rfind("https://", 0) == 0 || entry.download_url.rfind("http://", 0) == 0) {
        out = entry.download_url;
        return true;
    }
    if (index_url.empty()) {
        error = "relative download URL requires an index URL"; return false;
    }
    const auto slash = index_url.find_last_of('/');
    out = (slash == std::string::npos ? index_url + "/" : index_url.substr(0, slash + 1)) + entry.download_url;
    return true;
}

bool verify_downloaded_package(const fs::path &path, const std::string &expected_sha256, std::string &error) {
    std::ifstream input(path, std::ios::binary);
    unsigned char magic[4]{};
    if (!input || !input.read(reinterpret_cast<char *>(magic), sizeof(magic)) || magic[0] != 'P' || magic[1] != 'K') {
        error = "downloaded package is not a ZIP archive"; return false;
    }
    if (!expected_sha256.empty() && file_sha256(path) != expected_sha256) {
        error = "downloaded package SHA-256 mismatch"; return false;
    }
    return true;
}

} // namespace lualoader::repository
