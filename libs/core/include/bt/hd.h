// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Mnemonics and key derivation compatible with the LayerTwo wallets:
//  * BIP-39 English mnemonics -> 64-byte seed (PBKDF2-HMAC-SHA512, 2048 rounds,
//    salt "mnemonic"), as tiny-bip39 2.0 does;
//  * "bip32ish" over ristretto255 (bip32ish 0.1.1 + thunder-rust lib/wallet/bip32.rs):
//    master = HMAC-SHA512("Bitcoin seed", seed); hardened child =
//    HMAC-SHA512(chaincode, 0x00 || scalar_le || be32(i | 2^31)); non-hardened child =
//    HMAC-SHA512(chaincode, compressed_point || be32(i)); tweak = left half read as a
//    big-endian integer (with its top 3 bits cleared since v0.18.1, the "bias fix")
//    reduced mod L; child scalar = parent + tweak; chaincode = right half.
//  * since thunder-rust v0.19.0 (6b5e114, the networks it has: mainnet...), the same tree with BLAKE3 for HMAC-SHA512:
//    master = BLAKE3 derive_key("Thunder seed") over the seed, child = BLAKE3 keyed by the chaincode over the same
//    bytes; 96 bytes of XOF output, the first 64 read as a big-endian integer reduced mod L, the last 32 the chaincode.
#pragma once

#include <bt/bytes.h>
#include <bt/frost.h>

#include <optional>
#include <string>
#include <vector>

namespace bt::hd {

ByteArray<64> hmac_sha512(ByteSpan key, ByteSpan msg);
ByteArray<64> pbkdf2_hmac_sha512(ByteSpan password, ByteSpan salt, uint32_t rounds);

/** 12-word (128-bit) mnemonic from the OS RNG. */
std::string generate_mnemonic(size_t words = 12);
/** The BIP-39 English phrase of 16 to 32 bytes of entropy (a multiple of 4): 12 to 24 words. */
std::string mnemonic_from_entropy(ByteSpan entropy);
/** Validate words + checksum (Mnemonic::from_phrase); returns the normalised phrase. */
std::optional<std::string> parse_mnemonic(const std::string& phrase);
/** Why tiny-bip39 2.0's Mnemonic::from_phrase refuses `phrase`, in its words, in its order (an unknown word
 *  first, by index, then the word count, then the checksum); "" if it is valid. */
std::string mnemonic_error(const std::string& phrase);
/** Seed::new(mnemonic, passphrase) */
ByteArray<64> mnemonic_to_seed(const std::string& normalised_phrase, const std::string& passphrase = "");

enum class Variant {
    V0_18_1, // current: clear the top 3 bits before reduction (thunder-rust >= 0.18.1)
    V0_18_0, // pre-fix wallets (thunder-rust 0.18.0): plain big-endian reduction
    V0_19    // thunder-rust >= 0.19.0: BLAKE3 (master context "Thunder seed"), 512-bit reduction
};

struct Xpriv {
    frost::Scalar secret;
    ByteArray<32> chaincode;
    static Xpriv master(const ByteArray<64>& seed, Variant v);
    Xpriv derive_hardened(uint32_t index, Variant v) const;
    Xpriv derive_non_hardened(uint32_t index, Variant v) const;
    /** ChildIndex::from(u32): >= 2^31 hardened (index - 2^31), else non-hardened. */
    Xpriv derive(uint32_t child, Variant v) const;
};

/** Thunder-family signing key: m/43'/1899'/0'/<slot>'/0'/index */
frost::SigningKey sidechain_signing_key(const ByteArray<64>& seed, uint8_t slot, uint32_t index, Variant v = Variant::V0_18_1);

} // namespace bt::hd
