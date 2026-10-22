// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// A drop-in chain's own backup phrase made from the user's one backup phrase (the unified wallet's, doc/app.md
// "One backup phrase"): BIP-85 (deterministic entropy from a BIP-32 key), its BIP-39 application, at an index named by
// a versioned, product-neutral label. The drop-in wallets take a BIP-39 phrase of their own (LayerTwo's wallets do),
// so the child phrase is what their set-seed RPC takes, and what any BIP-85 tool gives back from the master phrase:
//   index   = BE32(SHA-256("sidechain/dropin/v1" 0x00 stem 0x00 network)[0..4]) & 0x7fffffff
//   key     = BIP-32 (secp256k1) private key at m/83696968'/39'/0'/12'/index' of the BIP-39 seed (with its extra word)
//   entropy = HMAC-SHA512(key = "bip-entropy-from-k", msg = key)[0..16]
//   phrase  = the 12 English BIP-39 words of the entropy
// stem: the chain's id of the app's catalog (thunder, bitnames, ...), never what a node says; network: the drop-in's
// own network name (betanet, regtest, signet, forknet, ...).
#pragma once

#include <bt/bip32.h>
#include <bt/bytes.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace bt::appui {

inline constexpr std::string_view DROPIN_PHRASE_LABEL{"sidechain/dropin/v1"};

uint32_t dropin_phrase_index(std::string_view stem, std::string_view network);
/** BIP-85's BIP-39 application: `words` (12, 18 or 24) English words at m/83696968'/39'/0'/words'/index'. Throws. */
std::string bip85_phrase(ByteSpan bip39_seed, uint32_t index, unsigned words = 12);
/** The same from a BIP-32 master key (BIP-85's own test vectors start from one). */
std::string bip85_phrase_from_master(const bip32::Xpriv& master, uint32_t index, unsigned words = 12);
/** The phrase of a drop-in chain from the user's BIP-39 seed. */
std::string dropin_phrase(ByteSpan bip39_seed, std::string_view stem, std::string_view network);
/**
 * A public fingerprint of a drop-in wallet's seed (its BIP-39 seed of 64 bytes): the first 8 bytes, hex, of
 * SHA-256("sidechain/dropin/fingerprint/v1" 0x00 seed). The unified wallet compares it with the one of the phrase it
 * would set, to tell a wallet of the phrase from one with a phrase of its own; it gives nothing of the seed away.
 */
std::string seed_fingerprint(ByteSpan seed64);

} // namespace bt::appui
