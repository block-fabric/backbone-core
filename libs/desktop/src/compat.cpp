// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The functions of the compat headers (libs/desktop/compat): what the template's webui host files take from
// Bitcoin Core's util, support and crypto, so that they compile unchanged outside Core.
#include <crypto/sha256.h>
#include <support/cleanse.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/threadnames.h>

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pthread.h>
#endif

void memory_cleanse(void* ptr, size_t len)
{
    if (ptr && len) sodium_memzero(ptr, len);
}

bool RenameOver(fs::path src, fs::path dest)
{
#ifdef _WIN32
    return MoveFileExW(src.wstring().c_str(), dest.wstring().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(src, dest, ec);
    return !ec;
#endif
}

bool TryCreateDirectories(const fs::path& p)
{
    try {
        return std::filesystem::create_directories(p);
    } catch (const std::filesystem::filesystem_error&) {
        if (!std::filesystem::exists(p) || !std::filesystem::is_directory(p)) throw;
    }
    return false;
}

#ifdef _WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    WCHAR path[MAX_PATH] = L"";
    if (SHGetSpecialFolderPathW(nullptr, path, nFolder, fCreate)) return fs::path(path);
    return fs::path("");
}
#endif

std::string HexStr(std::span<const unsigned char> s)
{
    static constexpr char digits[]{"0123456789abcdef"};
    std::string out(s.size() * 2, '\0');
    for (size_t i{0}; i < s.size(); ++i) {
        out[2 * i] = digits[s[i] >> 4];
        out[2 * i + 1] = digits[s[i] & 15];
    }
    return out;
}

std::string EncodeBase64(std::span<const unsigned char> input)
{
    static constexpr char table[]{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    size_t i{0};
    for (; i + 2 < input.size(); i += 3) {
        const uint32_t v{uint32_t(input[i]) << 16 | uint32_t(input[i + 1]) << 8 | input[i + 2]};
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += table[v >> 6 & 63];
        out += table[v & 63];
    }
    if (i + 1 == input.size()) {
        const uint32_t v{uint32_t(input[i]) << 16};
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += "==";
    } else if (i + 2 == input.size()) {
        const uint32_t v{uint32_t(input[i]) << 16 | uint32_t(input[i + 1]) << 8};
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += table[v >> 6 & 63];
        out += '=';
    }
    return out;
}

std::optional<std::vector<unsigned char>> DecodeBase64(std::string_view str)
{
    if (str.size() % 4 != 0) return std::nullopt;
    auto value{[](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }};
    std::vector<unsigned char> out;
    out.reserve(str.size() / 4 * 3);
    for (size_t i{0}; i < str.size(); i += 4) {
        int v[4];
        int pad{0};
        for (int k{0}; k < 4; ++k) {
            const char c{str[i + k]};
            if (c == '=') {
                // Padding only at the end, at most two.
                if (i + 4 != str.size() || k < 2) return std::nullopt;
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return std::nullopt;
                v[k] = value(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        const uint32_t n{uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3])};
        out.push_back(static_cast<unsigned char>(n >> 16));
        if (pad < 2) out.push_back(static_cast<unsigned char>(n >> 8 & 255));
        if (pad < 1) out.push_back(static_cast<unsigned char>(n & 255));
    }
    return out;
}

std::string ToLower(std::string_view str)
{
    std::string out{str};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string ToUpper(std::string_view str)
{
    std::string out{str};
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

bool IsHex(std::string_view str)
{
    if (str.empty() || str.size() % 2) return false;
    return std::all_of(str.begin(), str.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

namespace util {
void ThreadRename(const std::string& name)
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#else
    (void)name;
#endif
}
} // namespace util

// ---- SHA-256 (FIPS 180-4), incremental.
namespace {
constexpr uint32_t K[64]{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Transform(uint32_t s[8], const unsigned char* chunk)
{
    uint32_t w[64];
    for (int i{0}; i < 16; ++i) w[i] = uint32_t(chunk[4 * i]) << 24 | uint32_t(chunk[4 * i + 1]) << 16 | uint32_t(chunk[4 * i + 2]) << 8 | chunk[4 * i + 3];
    for (int i{16}; i < 64; ++i) {
        const uint32_t s0{Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)};
        const uint32_t s1{Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10)};
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a{s[0]}, b{s[1]}, c{s[2]}, d{s[3]}, e{s[4]}, f{s[5]}, g{s[6]}, h{s[7]};
    for (int i{0}; i < 64; ++i) {
        const uint32_t S1{Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)};
        const uint32_t ch{(e & f) ^ (~e & g)};
        const uint32_t t1{h + S1 + ch + K[i] + w[i]};
        const uint32_t S0{Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)};
        const uint32_t maj{(a & b) ^ (a & c) ^ (b & c)};
        const uint32_t t2{S0 + maj};
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
    s[4] += e;
    s[5] += f;
    s[6] += g;
    s[7] += h;
}
} // namespace

CSHA256::CSHA256() { Reset(); }

CSHA256& CSHA256::Reset()
{
    static constexpr uint32_t init[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(s, init, sizeof(s));
    bytes = 0;
    return *this;
}

CSHA256& CSHA256::Write(const unsigned char* data, size_t len)
{
    size_t fill{static_cast<size_t>(bytes % 64)};
    bytes += len;
    if (fill) {
        const size_t take{std::min(len, 64 - fill)};
        std::memcpy(buf + fill, data, take);
        data += take;
        len -= take;
        if (fill + take < 64) return *this;
        Transform(s, buf);
    }
    while (len >= 64) {
        Transform(s, data);
        data += 64;
        len -= 64;
    }
    if (len) std::memcpy(buf, data, len);
    return *this;
}

void CSHA256::Finalize(unsigned char hash[OUTPUT_SIZE])
{
    const uint64_t bits{bytes * 8};
    static constexpr unsigned char pad[64]{0x80};
    const size_t fill{static_cast<size_t>(bytes % 64)};
    Write(pad, 1 + ((119 - fill) % 64));
    unsigned char length[8];
    for (int i{0}; i < 8; ++i) length[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    Write(length, 8);
    for (int i{0}; i < 8; ++i) {
        hash[4 * i] = static_cast<unsigned char>(s[i] >> 24);
        hash[4 * i + 1] = static_cast<unsigned char>(s[i] >> 16);
        hash[4 * i + 2] = static_cast<unsigned char>(s[i] >> 8);
        hash[4 * i + 3] = static_cast<unsigned char>(s[i]);
    }
}
