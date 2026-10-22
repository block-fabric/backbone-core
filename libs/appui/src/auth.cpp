// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/appui/auth.h>

#include <bt/bytes.h>
#include <bt/crypto.h>

#include <sodium.h>

#include <cctype>
#include <cstring>

namespace bt::appui {

static bool is_hex(std::string_view s)
{
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

std::optional<RpcAuth> parse_rpcauth(std::string_view text)
{
    const size_t colon{text.find(':')};
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    const std::string_view rest{text.substr(colon + 1)};
    const size_t dollar{rest.find('$')};
    if (dollar == std::string_view::npos || dollar == 0) return std::nullopt;
    RpcAuth a{std::string{text.substr(0, colon)}, std::string{rest.substr(0, dollar)}, std::string{rest.substr(dollar + 1)}};
    if (a.hash.size() != 64 || !is_hex(a.hash) || a.user.find_first_of(":\r\n") != std::string::npos) return std::nullopt;
    for (char& c : a.hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a;
}

static std::string hmac_hex(std::string_view salt, std::string_view password)
{
    const Hash32 h{hmac_sha256(ByteSpan{reinterpret_cast<const uint8_t*>(salt.data()), salt.size()},
                               {ByteSpan{reinterpret_cast<const uint8_t*>(password.data()), password.size()}})};
    return hex_encode(h);
}

RpcAuth make_rpcauth(const std::string& user, std::string_view password)
{
    RpcAuth a{user, random_hex(16), ""};
    a.hash = hmac_hex(a.salt, password);
    return a;
}

std::string format_rpcauth(const RpcAuth& a) { return a.user + ":" + a.salt + "$" + a.hash; }

bool timing_safe_equal(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    unsigned char d{0};
    for (size_t i{0}; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

std::string server_proof(std::string_view cookie_secret, std::string_view challenge)
{
    std::string msg{"bt-app-server-proof/v1"};
    msg.push_back('\0');
    msg.append(challenge);
    return hmac_hex(cookie_secret, msg);
}

bool check_password(const RpcAuth& a, std::string_view password) { return timing_safe_equal(hmac_hex(a.salt, password), a.hash); }

std::string random_hex(size_t bytes)
{
    std::vector<uint8_t> b(bytes);
    randombytes_buf(b.data(), b.size());
    std::string out{hex_encode(ByteSpan{b.data(), b.size()})};
    sodium_memzero(b.data(), b.size());
    return out;
}

std::string base64_encode(std::string_view s)
{
    std::string out(sodium_base64_ENCODED_LEN(s.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(s.data()), s.size(), sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::optional<std::string> base64_decode(std::string_view s)
{
    std::string out(s.size(), '\0');
    size_t len{0};
    if (sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), s.data(), s.size(), nullptr, &len, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) return std::nullopt;
    out.resize(len);
    return out;
}

std::optional<std::pair<std::string, std::string>> parse_basic(std::string_view header)
{
    if (header.size() < 6 || header.substr(0, 6) != "Basic ") return std::nullopt;
    std::string_view b64{header.substr(6)};
    while (!b64.empty() && (b64.back() == ' ' || b64.back() == '\t')) b64.remove_suffix(1);
    auto decoded{base64_decode(b64)};
    if (!decoded) return std::nullopt;
    const size_t colon{decoded->find(':')};
    if (colon == std::string::npos) return std::nullopt;
    std::pair<std::string, std::string> out{decoded->substr(0, colon), decoded->substr(colon + 1)};
    sodium_memzero(decoded->data(), decoded->size());
    return out;
}

bool Whitelist::add(std::string_view entry)
{
    const size_t colon{entry.find(':')};
    if (colon == std::string_view::npos || colon == 0) return false;
    const std::string user{entry.substr(0, colon)};
    std::set<std::string> methods;
    std::string_view rest{entry.substr(colon + 1)};
    while (!rest.empty()) {
        const size_t comma{rest.find(',')};
        std::string_view m{rest.substr(0, comma)};
        while (!m.empty() && m.front() == ' ') m.remove_prefix(1);
        while (!m.empty() && m.back() == ' ') m.remove_suffix(1);
        if (!m.empty()) methods.emplace(m);
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    auto it{lists_.find(user)};
    if (it == lists_.end()) {
        lists_.emplace(user, std::move(methods));
    } else {
        // As Core: a second whitelist of the same user narrows it to what both allow.
        std::set<std::string> both;
        for (const auto& m : it->second) {
            if (methods.contains(m)) both.insert(m);
        }
        it->second = std::move(both);
    }
    return true;
}

bool Whitelist::allowed(const std::string& user, const std::string& method) const
{
    const auto it{lists_.find(user)};
    if (it != lists_.end()) return it->second.contains(method);
    return !default_.value_or(!lists_.empty());
}

bool Whitelist::may_call_all(const std::string& user) const { return !lists_.contains(user) && !default_.value_or(!lists_.empty()); }

} // namespace bt::appui
