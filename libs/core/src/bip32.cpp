// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/bip32.h>

#include <bt/hd.h>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

#include <cstring>
#include <memory>
#include <stdexcept>

namespace bt::bip32 {

namespace {
struct BnCtx {
    BN_CTX* p = BN_CTX_new();
    ~BnCtx() { BN_CTX_free(p); }
};
struct Bn {
    BIGNUM* p = BN_new();
    ~Bn() { BN_clear_free(p); }
};
struct Group {
    EC_GROUP* g = EC_GROUP_new_by_curve_name(NID_secp256k1);
    ~Group() { EC_GROUP_free(g); }
};
const Group& group()
{
    static Group g;
    if (!g.g) throw std::runtime_error("OpenSSL lacks secp256k1");
    return g;
}
void to32(const BIGNUM* b, uint8_t* out)
{
    if (BN_bn2binpad(b, out, 32) != 32) throw std::runtime_error("bip32: bn2bin");
}
} // namespace

ByteArray<33> public_key(const ByteArray<32>& secret)
{
    const Group& G = group();
    BnCtx ctx;
    Bn k;
    BN_bin2bn(secret.data(), 32, k.p);
    BN_set_flags(k.p, BN_FLG_CONSTTIME);
    std::unique_ptr<EC_POINT, void (*)(EC_POINT*)> P(EC_POINT_new(G.g), EC_POINT_clear_free);
    if (!EC_POINT_mul(G.g, P.get(), k.p, nullptr, nullptr, ctx.p)) throw std::runtime_error("bip32: point mul");
    ByteArray<33> out;
    if (EC_POINT_point2oct(G.g, P.get(), POINT_CONVERSION_COMPRESSED, out.data(), 33, ctx.p) != 33) throw std::runtime_error("bip32: point2oct");
    return out;
}

static std::optional<ByteArray<32>> add_mod_n(const uint8_t* il, const ByteArray<32>& k)
{
    const Group& G = group();
    BnCtx ctx;
    Bn n, a, b, r;
    if (!EC_GROUP_get_order(G.g, n.p, ctx.p)) throw std::runtime_error("bip32: order");
    BN_bin2bn(il, 32, a.p);
    if (BN_cmp(a.p, n.p) >= 0) return std::nullopt;
    BN_bin2bn(k.data(), 32, b.p);
    if (!BN_mod_add(r.p, a.p, b.p, n.p, ctx.p)) throw std::runtime_error("bip32: add");
    if (BN_is_zero(r.p)) return std::nullopt;
    ByteArray<32> out;
    to32(r.p, out.data());
    return out;
}

std::optional<Xpriv> master(ByteSpan seed)
{
    static const char key[] = "Bitcoin seed";
    auto I = hd::hmac_sha512(ByteSpan((const uint8_t*)key, 12), seed);
    ByteArray<32> zero{};
    auto sk = add_mod_n(I.data(), zero);
    if (!sk) return std::nullopt;
    Xpriv x;
    x.secret = *sk;
    std::memcpy(x.chaincode.data(), I.data() + 32, 32);
    return x;
}

std::optional<Xpriv> derive(const Xpriv& p, uint32_t index)
{
    Bytes data;
    if (index & HARDENED) {
        data.push_back(0);
        data.insert(data.end(), p.secret.begin(), p.secret.end());
    } else {
        auto pk = public_key(p.secret);
        data.insert(data.end(), pk.begin(), pk.end());
    }
    for (int i = 3; i >= 0; i--) data.push_back(uint8_t(index >> (8 * i)));
    auto I = hd::hmac_sha512(p.chaincode.span(), data);
    auto sk = add_mod_n(I.data(), p.secret);
    if (!sk) return std::nullopt;
    Xpriv c;
    c.secret = *sk;
    std::memcpy(c.chaincode.data(), I.data() + 32, 32);
    return c;
}

std::optional<Xpriv> derive_path(const Xpriv& m, const std::vector<uint32_t>& path)
{
    std::optional<Xpriv> x = m;
    for (uint32_t i : path) {
        x = derive(*x, i);
        if (!x) return std::nullopt;
    }
    return x;
}

} // namespace bt::bip32
