// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BIP32-Ed25519 (Khovratovich-Law), derivation scheme V2, byte-exact with the
// `ed25519-bip32` 0.4.1 crate the BitNames/BitAssets wallets use:
//   XPrv = kL(32) || kR(32) || chaincode(32)
//   hardened (i >= 2^31): Z = HMAC-SHA512(cc, 0x00 || kL || kR || le32(i)),
//                         I = HMAC-SHA512(cc, 0x01 || kL || kR || le32(i))
//   soft:                 Z = HMAC-SHA512(cc, 0x02 || A || le32(i)), I = (0x03 ...),
//                         A = kL*B on Ed25519 (compressed)
//   child kL = kL + 8*trunc28(ZL) (256-bit, carry dropped), kR = kR + ZR (mod 2^256),
//   child cc = I[32..64].
#pragma once

#include <bt/bytes.h>

#include <optional>

namespace bt::ed25519_bip32 {

struct XPrv {
    ByteArray<96> b;
    ByteSpan kl() const { return ByteSpan(b.data(), 32); }
    ByteSpan extended_secret() const { return ByteSpan(b.data(), 64); }
    ByteSpan chain_code() const { return ByteSpan(b.data() + 64, 32); }

    /** XPrv::from_nonextended_noforce: SHA-512(secret), clamp (normalize_bytes_ed25519),
     *  error if the 3rd highest bit of kL is set. */
    static std::optional<XPrv> from_nonextended_noforce(const ByteArray<32>& secret, const ByteArray<32>& chain_code);
    /** XPrv::derive(DerivationScheme::V2, index). */
    XPrv derive(uint32_t index) const;
    /** Ed25519 public key kL*B (mk_public_key). */
    ByteArray<32> public_key() const;
    /** XPub bytes: public key || chain code. */
    ByteArray<64> xpub() const;
};

/** XPub::derive (soft only): nullopt for hardened indexes or invalid points. */
std::optional<ByteArray<64>> derive_xpub(const ByteArray<64>& xpub, uint32_t index);

} // namespace bt::ed25519_bip32
