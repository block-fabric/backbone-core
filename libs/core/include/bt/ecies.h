// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// ECIES as `libes` 0.9.1 builds it for Ecies<X25519, Aes256Gcm, HmacSha256> (ECIES-MAC),
// the scheme BitNames uses for encrypt_msg/decrypt_msg and paymail:
//   ephemeral x25519 key pair (e, E); shared = X25519(e, recipient_pk)
//   km = HKDF-SHA256(salt = none, ikm = shared || E, info = "", 64 bytes)
//   enc_key = km[0..32], mac_key = km[32..64]; nonce = 12 random bytes
//   c = AES-256-GCM(enc_key, nonce, plaintext, aad = "") (ciphertext || 16-byte tag)
//   mac = HMAC-SHA256(mac_key, nonce || c)
//   output = E(32) || nonce(12) || mac(32) || c
// Lives in bt_net because AES-GCM comes from OpenSSL.
#pragma once

#include <bt/bytes.h>

#include <optional>

namespace bt::ecies {

/** x25519 public key of a secret (StaticSecret -> PublicKey, clamped as RFC 7748). */
ByteArray<32> x25519_public(const ByteArray<32>& secret);
Bytes encrypt(const ByteArray<32>& recipient_pk, ByteSpan plaintext);
/** Deterministic variant for tests: caller supplies the ephemeral secret and the nonce. */
Bytes encrypt_with(const ByteArray<32>& recipient_pk, ByteSpan plaintext, const ByteArray<32>& ephemeral_secret, const ByteArray<12>& nonce);
/** nullopt: BadData (too short), VerificationError (MAC) or DecryptionError (GCM). */
std::optional<Bytes> decrypt(const ByteArray<32>& recipient_secret, ByteSpan ciphertext);

} // namespace bt::ecies
