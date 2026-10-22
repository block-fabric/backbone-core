// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/crypto.h>
#include <bt/hd.h>

#include <sodium.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace bt::hd {

static const char* const WORDS[2048] = {
#include "bip39_english.inc"
};

ByteArray<64> hmac_sha512(ByteSpan key, ByteSpan msg)
{
    uint8_t k[128] = {0};
    if (key.size() > 128) {
        auto h = sha512(key);
        std::memcpy(k, h.data(), 64);
    } else {
        std::memcpy(k, key.data(), key.size());
    }
    uint8_t ipad[128], opad[128];
    for (int i = 0; i < 128; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    auto inner = sha512_parts({ByteSpan(ipad, 128), msg});
    return sha512_parts({ByteSpan(opad, 128), inner.span()});
}

ByteArray<64> pbkdf2_hmac_sha512(ByteSpan password, ByteSpan salt, uint32_t rounds)
{
    Bytes s(salt.begin(), salt.end());
    s.push_back(0);
    s.push_back(0);
    s.push_back(0);
    s.push_back(1);
    auto u = hmac_sha512(password, s);
    ByteArray<64> t = u;
    for (uint32_t i = 1; i < rounds; i++) {
        u = hmac_sha512(password, u.span());
        for (int j = 0; j < 64; j++) t[j] ^= u[j];
    }
    return t;
}

static int word_index(const std::string& w)
{
    auto it = std::lower_bound(std::begin(WORDS), std::end(WORDS), w, [](const char* a, const std::string& b) { return std::strcmp(a, b.c_str()) < 0; });
    if (it == std::end(WORDS) || w != *it) return -1;
    return int(it - std::begin(WORDS));
}

std::string mnemonic_from_entropy(ByteSpan ent)
{
    if (ent.size() < 16 || ent.size() > 32 || ent.size() % 4) throw std::invalid_argument("BIP-39 entropy is 16 to 32 bytes, a multiple of 4");
    const size_t ent_bits = ent.size() * 8, words = (ent_bits + ent_bits / 32) / 11;
    Hash32 h = sha256(ent);
    std::vector<int> bits;
    for (uint8_t b : ent)
        for (int i = 7; i >= 0; i--) bits.push_back((b >> i) & 1);
    for (size_t i = 0; i < ent_bits / 32; i++) bits.push_back((h[i / 8] >> (7 - i % 8)) & 1);
    std::string r;
    for (size_t w = 0; w < words; w++) {
        int idx = 0;
        for (int i = 0; i < 11; i++) idx = idx << 1 | bits[w * 11 + i];
        if (w) r += " ";
        r += WORDS[idx];
    }
    return r;
}

std::string generate_mnemonic(size_t words)
{
    size_t ent_bits = words * 32 / 3; // 12 -> 128
    Bytes ent(ent_bits / 8);
    randombytes_buf(ent.data(), ent.size());
    std::string r = mnemonic_from_entropy(ent);
    sodium_memzero(ent.data(), ent.size());
    return r;
}

std::optional<std::string> parse_mnemonic(const std::string& phrase)
{
    std::istringstream in(phrase);
    std::vector<std::string> ws;
    std::string w;
    while (in >> w) ws.push_back(w);
    if (ws.size() != 12 && ws.size() != 15 && ws.size() != 18 && ws.size() != 21 && ws.size() != 24) return std::nullopt;
    std::vector<int> bits;
    for (auto& x : ws) {
        int i = word_index(x);
        if (i < 0) return std::nullopt;
        for (int b = 10; b >= 0; b--) bits.push_back((i >> b) & 1);
    }
    size_t cs = ws.size() / 3, ent_bits = bits.size() - cs;
    Bytes ent(ent_bits / 8);
    for (size_t i = 0; i < ent_bits; i++)
        if (bits[i]) ent[i / 8] |= uint8_t(1 << (7 - i % 8));
    Hash32 h = sha256(ent);
    for (size_t i = 0; i < cs; i++)
        if (bits[ent_bits + i] != ((h[i / 8] >> (7 - i % 8)) & 1)) return std::nullopt;
    std::string r;
    for (size_t i = 0; i < ws.size(); i++) r += (i ? " " : "") + ws[i];
    return r;
}

std::string mnemonic_error(const std::string& phrase)
{
    std::istringstream in(phrase);
    std::vector<std::string> ws;
    std::string w;
    while (in >> w) ws.push_back(w);
    // from_phrase joins split_whitespace() with " ", then phrase_to_entropy splits on ' ': no words is one empty word.
    if (ws.empty()) return "invalid word in phrase with index 0";
    for (size_t i = 0; i < ws.size(); i++)
        if (word_index(ws[i]) < 0) return "invalid word in phrase with index " + std::to_string(i);
    if (ws.size() != 12 && ws.size() != 15 && ws.size() != 18 && ws.size() != 21 && ws.size() != 24)
        return "invalid number of words in phrase: " + std::to_string(ws.size());
    return parse_mnemonic(phrase) ? std::string{} : std::string{"invalid checksum"};
}

ByteArray<64> mnemonic_to_seed(const std::string& phrase, const std::string& passphrase)
{
    std::string salt = "mnemonic" + passphrase;
    return pbkdf2_hmac_sha512(ByteSpan((const uint8_t*)phrase.data(), phrase.size()), ByteSpan((const uint8_t*)salt.data(), salt.size()), 2048);
}

// scalar_from_uniform_be_bytes / scalar_from_be_bytes
static frost::Scalar scalar_from_be(const uint8_t* be32, Variant v)
{
    uint8_t wide[64] = {0};
    for (int i = 0; i < 32; i++) wide[i] = be32[31 - i];
    if (v == Variant::V0_18_1) wide[31] &= 0x1f; // bytes[0] &= 0b0001_1111 before reversing
    frost::Scalar s;
    crypto_core_ristretto255_scalar_reduce(s.data(), wide);
    return s;
}

static Xpriv finish(const Xpriv& parent, const ByteArray<64>& d, Variant v)
{
    Xpriv c;
    frost::Scalar tweak = scalar_from_be(d.data(), v);
    crypto_core_ristretto255_scalar_add(c.secret.data(), parent.secret.data(), tweak.data());
    std::memcpy(c.chaincode.data(), d.data() + 32, 32);
    return c;
}

Xpriv Xpriv::master(const ByteArray<64>& seed, Variant v)
{
    frost::init();
    auto d = hmac_sha512(ByteSpan((const uint8_t*)"Bitcoin seed", 12), seed.span());
    Xpriv x;
    x.secret = scalar_from_be(d.data(), v);
    std::memcpy(x.chaincode.data(), d.data() + 32, 32);
    return x;
}

Xpriv Xpriv::derive_hardened(uint32_t index, Variant v) const
{
    Bytes m;
    m.push_back(0x00); // Hasher<true> prefix
    append(m, secret.span()); // PrimeField repr: 32-byte little-endian
    uint32_t n = index | 0x80000000u;
    for (int i = 3; i >= 0; i--) m.push_back(uint8_t(n >> (8 * i)));
    return finish(*this, hmac_sha512(chaincode.span(), m), v);
}

Xpriv Xpriv::derive_non_hardened(uint32_t index, Variant v) const
{
    Bytes m;
    frost::SigningKey sk{secret};
    append(m, sk.verifying_key().span()); // GroupEncoding: compressed ristretto point
    for (int i = 3; i >= 0; i--) m.push_back(uint8_t(index >> (8 * i)));
    return finish(*this, hmac_sha512(chaincode.span(), m), v);
}

Xpriv Xpriv::derive(uint32_t child, Variant v) const
{
    if (child & 0x80000000u) return derive_hardened(child & 0x7fffffffu, v);
    return derive_non_hardened(child, v);
}

frost::SigningKey sidechain_signing_key(const ByteArray<64>& seed, uint8_t slot, uint32_t index, Variant v)
{
    Xpriv x = Xpriv::master(seed, v);
    x = x.derive_hardened(43, v).derive_hardened(1899, v).derive_hardened(0, v).derive_hardened(slot, v).derive_hardened(0, v).derive(index, v);
    if (x.secret.is_zero()) throw std::runtime_error("derived zero secret scalar");
    return frost::SigningKey{x.secret};
}

} // namespace bt::hd
