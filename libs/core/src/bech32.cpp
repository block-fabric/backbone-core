// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/bech32.h>

#include <cstring>
#include <vector>

namespace bt::bech32 {

static const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static constexpr uint32_t BECH32_CONST = 1, BECH32M_CONST = 0x2bc830a3;

static uint32_t polymod(const std::vector<uint8_t>& v)
{
    static const uint32_t G[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    uint32_t chk = 1;
    for (uint8_t x : v) {
        uint32_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ x;
        for (int i = 0; i < 5; i++)
            if ((top >> i) & 1) chk ^= G[i];
    }
    return chk;
}

static std::vector<uint8_t> hrp_expand(std::string_view hrp)
{
    std::vector<uint8_t> r;
    for (char c : hrp) r.push_back(uint8_t(c) >> 5);
    r.push_back(0);
    for (char c : hrp) r.push_back(uint8_t(c) & 31);
    return r;
}

static bool hrp_ok(std::string_view hrp)
{
    if (hrp.empty() || hrp.size() > 83) return false;
    for (char c : hrp)
        if (c < 33 || c > 126) return false;
    return true;
}

std::optional<std::string> encode(std::string_view hrp_in, ByteSpan data, Variant v)
{
    std::string hrp(hrp_in);
    for (auto& c : hrp)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (!hrp_ok(hrp)) return std::nullopt;
    std::vector<uint8_t> d5;
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t b : data) {
        acc = acc << 8 | b;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            d5.push_back(uint8_t((acc >> bits) & 31));
        }
    }
    if (bits) d5.push_back(uint8_t((acc << (5 - bits)) & 31));
    auto e = hrp_expand(hrp);
    e.insert(e.end(), d5.begin(), d5.end());
    e.insert(e.end(), 6, 0);
    uint32_t mod = polymod(e) ^ (v == Variant::Bech32 ? BECH32_CONST : BECH32M_CONST);
    std::string out = hrp + "1";
    for (uint8_t x : d5) out += CHARSET[x];
    for (int i = 0; i < 6; i++) out += CHARSET[(mod >> (5 * (5 - i))) & 31];
    if (out.size() > 1023) return std::nullopt;
    return out;
}

std::optional<Decoded> decode(std::string_view s)
{
    if (s.size() < 8 || s.size() > 1023) return std::nullopt;
    bool lower = false, upper = false;
    for (char c : s) {
        if (c < 33 || c > 126) return std::nullopt;
        if (c >= 'a' && c <= 'z') lower = true;
        if (c >= 'A' && c <= 'Z') upper = true;
    }
    if (lower && upper) return std::nullopt;
    std::string str(s);
    for (auto& c : str)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    auto sep = str.rfind('1');
    if (sep == std::string::npos || sep == 0 || sep + 7 > str.size()) return std::nullopt;
    std::string hrp = str.substr(0, sep);
    if (!hrp_ok(hrp)) return std::nullopt;
    std::vector<uint8_t> d5;
    for (size_t i = sep + 1; i < str.size(); i++) {
        const char* p = std::strchr(CHARSET, str[i]);
        if (!p || !*p) return std::nullopt;
        d5.push_back(uint8_t(p - CHARSET));
    }
    auto e = hrp_expand(hrp);
    e.insert(e.end(), d5.begin(), d5.end());
    uint32_t pm = polymod(e);
    Decoded r;
    if (pm == BECH32_CONST) r.variant = Variant::Bech32;
    else if (pm == BECH32M_CONST) r.variant = Variant::Bech32m;
    else return std::nullopt;
    d5.resize(d5.size() - 6);
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t x : d5) {
        acc = acc << 5 | x;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            r.data.push_back(uint8_t((acc >> bits) & 0xff));
        }
    }
    if (bits >= 5 || (acc & ((1u << bits) - 1)) != 0) return std::nullopt; // padding
    r.hrp = hrp;
    return r;
}

std::optional<Bytes> decode_bech32m_canonical(std::string_view s, std::string_view hrp)
{
    auto d = decode(s);
    if (!d || d->hrp != hrp) return std::nullopt;
    auto re = encode(hrp, d->data, Variant::Bech32m);
    if (!re || *re != s) return std::nullopt;
    return d->data;
}

} // namespace bt::bech32
