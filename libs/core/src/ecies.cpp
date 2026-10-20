// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/crypto.h>
#include <bt/ecies.h>
#include <bt/frost.h>

#include <openssl/evp.h>
#include <sodium.h>

#include <cstring>

namespace bt::ecies {

ByteArray<32> x25519_public(const ByteArray<32>& secret)
{
    frost::init();
    ByteArray<32> pk;
    crypto_scalarmult_base(pk.data(), secret.data());
    return pk;
}

static bool aes_gcm(bool enc, const uint8_t* key, const uint8_t* nonce, ByteSpan in, Bytes& out)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int len = 0;
    do {
        if ((enc ? EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)
                 : EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) break;
        if ((enc ? EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) : EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce)) != 1) break;
        if (enc) {
            out.resize(in.size() + 16);
            if (EVP_EncryptUpdate(ctx, out.data(), &len, in.data(), int(in.size())) != 1) break;
            int fl = 0;
            if (EVP_EncryptFinal_ex(ctx, out.data() + len, &fl) != 1) break;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data() + in.size()) != 1) break;
            ok = true;
        } else {
            if (in.size() < 16) break;
            size_t n = in.size() - 16;
            out.resize(n);
            if (EVP_DecryptUpdate(ctx, out.data(), &len, in.data(), int(n)) != 1) break;
            Bytes tag(in.begin() + n, in.end());
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) break;
            int fl = 0;
            if (EVP_DecryptFinal_ex(ctx, out.data() + len, &fl) != 1) break;
            ok = true;
        }
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static Bytes key_material(const uint8_t shared[32], const ByteArray<32>& eph_pk)
{
    Bytes ikm(shared, shared + 32);
    append(ikm, eph_pk.span());
    return hkdf_sha256({}, ikm, {}, 64);
}

Bytes encrypt_with(const ByteArray<32>& recipient_pk, ByteSpan plaintext, const ByteArray<32>& eph_sk, const ByteArray<12>& nonce)
{
    frost::init();
    ByteArray<32> eph_pk = x25519_public(eph_sk);
    uint8_t shared[32];
    // x25519-dalek diffie_hellman does not reject low-order points; libsodium does (-1)
    // and leaves the output unspecified, so use the all-zero shared secret dalek returns.
    if (crypto_scalarmult(shared, eph_sk.data(), recipient_pk.data()) != 0) std::memset(shared, 0, 32);
    Bytes km = key_material(shared, eph_pk);
    Bytes c;
    if (!aes_gcm(true, km.data(), nonce.data(), plaintext, c)) return {};
    Hash32 mac = hmac_sha256(ByteSpan(km.data() + 32, 32), {nonce.span(), c});
    Bytes out(eph_pk.begin(), eph_pk.end());
    append(out, nonce.span());
    append(out, mac.span());
    append(out, c);
    return out;
}

Bytes encrypt(const ByteArray<32>& recipient_pk, ByteSpan plaintext)
{
    frost::init();
    ByteArray<32> sk;
    ByteArray<12> nonce;
    randombytes_buf(sk.data(), 32);
    randombytes_buf(nonce.data(), 12);
    return encrypt_with(recipient_pk, plaintext, sk, nonce);
}

std::optional<Bytes> decrypt(const ByteArray<32>& sk, ByteSpan ct)
{
    frost::init();
    if (ct.size() < 32 + 12 + 32) return std::nullopt;
    ByteArray<32> eph_pk = ByteArray<32>::from_span(ct.subspan(0, 32));
    ByteSpan nonce = ct.subspan(32, 12), mac = ct.subspan(44, 32), c = ct.subspan(76);
    uint8_t shared[32];
    if (crypto_scalarmult(shared, sk.data(), eph_pk.data()) != 0) std::memset(shared, 0, 32);
    Bytes km = key_material(shared, eph_pk);
    Hash32 want = hmac_sha256(ByteSpan(km.data() + 32, 32), {nonce, c});
    if (sodium_memcmp(want.data(), mac.data(), 32) != 0) return std::nullopt;
    Bytes pt;
    if (!aes_gcm(false, km.data(), nonce.data(), c, pt)) return std::nullopt;
    return pt;
}

} // namespace bt::ecies
