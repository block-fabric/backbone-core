// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BIP-32 over secp256k1 (rust-bitcoin 0.32 bip32::Xpriv::new_master / derive_priv), used by the
// plain-bitassets wallet: its keys are BIP-32 secp256k1 private keys whose 32 secret bytes are
// then reinterpreted (little-endian, mod L) as ristretto255 or x25519 secrets. Only the private
// derivation is needed. Built on OpenSSL's secp256k1 group (bt_net).
#pragma once

#include <bt/bytes.h>

#include <optional>
#include <vector>

namespace bt::bip32 {

struct Xpriv {
    ByteArray<32> secret; // big-endian scalar in [1, n)
    ByteArray<32> chaincode;
};

/** Xpriv::new_master: HMAC-SHA512("Bitcoin seed", seed); nullopt if IL is 0 or >= n. */
std::optional<Xpriv> master(ByteSpan seed);
/** ckd_priv: index >= 2^31 hardened (0x00 || k || ser32(i)), else (serP(K) || ser32(i));
 *  nullopt if IL >= n or the child key is 0 (rust-bitcoin errors there). */
std::optional<Xpriv> derive(const Xpriv& parent, uint32_t index);
/** Derive along a path of child numbers (hardened ones with the 2^31 bit set). */
std::optional<Xpriv> derive_path(const Xpriv& m, const std::vector<uint32_t>& path);
/** Compressed public key (33 bytes) of a secret. */
ByteArray<33> public_key(const ByteArray<32>& secret);

constexpr uint32_t HARDENED = 0x80000000u;

} // namespace bt::bip32
