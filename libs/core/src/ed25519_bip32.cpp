// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/crypto.h>
#include <bt/ed25519_bip32.h>
#include <bt/frost.h>
#include <bt/hd.h>

#include <cstring>
#include <sodium.h>

namespace bt::ed25519_bip32 {

static void le32(uint8_t out[4], uint32_t i)
{
    for (int k = 0; k < 4; k++) out[k] = uint8_t(i >> (8 * k));
}

// v2::add_28_mul8_v2
static void add_28_mul8(uint8_t out[32], const uint8_t* x, const uint8_t* y)
{
    uint16_t carry = 0;
    for (int i = 0; i < 28; i++) {
        uint16_t r = uint16_t(x[i] + (uint16_t(y[i]) << 3) + carry);
        out[i] = uint8_t(r & 0xff);
        carry = r >> 8;
    }
    for (int i = 28; i < 32; i++) {
        uint16_t r = uint16_t(x[i] + carry);
        out[i] = uint8_t(r & 0xff);
        carry = r >> 8;
    }
}

// v2::add_256bits_v2
static void add_256(uint8_t out[32], const uint8_t* x, const uint8_t* y)
{
    uint16_t carry = 0;
    for (int i = 0; i < 32; i++) {
        uint16_t r = uint16_t(x[i] + y[i] + carry);
        out[i] = uint8_t(r);
        carry = r >> 8;
    }
}

// k*B for a 256-bit k with the top bit clear (all ed25519-bip32 keys are in
// [2^254, 2^255)); reducing mod L first gives the same point as cryptoxide's
// scalarmult_base on the unreduced bytes.
static ByteArray<32> base_mul(ByteSpan k)
{
    frost::init();
    uint8_t wide[64] = {0}, red[32];
    std::memcpy(wide, k.data(), 32);
    crypto_core_ed25519_scalar_reduce(red, wide);
    ByteArray<32> p;
    if (crypto_scalarmult_ed25519_base_noclamp(p.data(), red) != 0) p = ByteArray<32>{}; // identity (k = 0 mod L): unreachable
    return p;
}

std::optional<XPrv> XPrv::from_nonextended_noforce(const ByteArray<32>& secret, const ByteArray<32>& cc)
{
    auto h = sha512(secret.span());
    XPrv x;
    std::memcpy(x.b.data(), h.data(), 64);
    std::memcpy(x.b.data() + 64, cc.data(), 32);
    x.b[0] &= 0b1111'1000;
    x.b[31] &= 0b0011'1111;
    x.b[31] |= 0b0100'0000;
    if (x.b[31] & 0b0010'0000) return std::nullopt;
    return x;
}

ByteArray<32> XPrv::public_key() const { return base_mul(kl()); }

ByteArray<64> XPrv::xpub() const
{
    ByteArray<64> r;
    auto pk = public_key();
    std::memcpy(r.data(), pk.data(), 32);
    std::memcpy(r.data() + 32, b.data() + 64, 32);
    return r;
}

XPrv XPrv::derive(uint32_t index) const
{
    uint8_t seri[4];
    le32(seri, index);
    Bytes zmsg, imsg;
    if (index >= 0x80000000u) {
        zmsg.push_back(0x00);
        imsg.push_back(0x01);
        append(zmsg, extended_secret());
        append(imsg, extended_secret());
    } else {
        auto pk = public_key();
        zmsg.push_back(0x02);
        imsg.push_back(0x03);
        append(zmsg, pk.span());
        append(imsg, pk.span());
    }
    append(zmsg, ByteSpan(seri, 4));
    append(imsg, ByteSpan(seri, 4));
    auto z = hd::hmac_sha512(chain_code(), zmsg);
    auto i = hd::hmac_sha512(chain_code(), imsg);
    XPrv c;
    add_28_mul8(c.b.data(), b.data(), z.data());
    add_256(c.b.data() + 32, b.data() + 32, z.data() + 32);
    std::memcpy(c.b.data() + 64, i.data() + 32, 32);
    return c;
}

std::optional<ByteArray<64>> derive_xpub(const ByteArray<64>& xpub, uint32_t index)
{
    if (index >= 0x80000000u) return std::nullopt;
    frost::init();
    uint8_t seri[4];
    le32(seri, index);
    Bytes zmsg{0x02}, imsg{0x03};
    append(zmsg, ByteSpan(xpub.data(), 32));
    append(imsg, ByteSpan(xpub.data(), 32));
    append(zmsg, ByteSpan(seri, 4));
    append(imsg, ByteSpan(seri, 4));
    ByteSpan cc(xpub.data() + 32, 32);
    auto z = hd::hmac_sha512(cc, zmsg);
    auto i = hd::hmac_sha512(cc, imsg);
    uint8_t zero[32] = {0}, t[32];
    add_28_mul8(t, zero, z.data());
    auto tp = base_mul(ByteSpan(t, 32));
    ByteArray<64> r;
    if (crypto_core_ed25519_add(r.data(), xpub.data(), tp.data()) != 0) return std::nullopt;
    std::memcpy(r.data() + 32, i.data() + 32, 32);
    return r;
}

} // namespace bt::ed25519_bip32
