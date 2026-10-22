// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/appui/phrase.h>

#include <bt/bip32.h>
#include <bt/crypto.h>
#include <bt/hd.h>

#include <sodium.h>

#include <stdexcept>

namespace bt::appui {

static ByteSpan text(std::string_view s) { return ByteSpan{reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

uint32_t dropin_phrase_index(std::string_view stem, std::string_view network)
{
    const uint8_t zero{0};
    const Hash32 h{sha256_parts({text(DROPIN_PHRASE_LABEL), ByteSpan{&zero, 1}, text(stem), ByteSpan{&zero, 1}, text(network)})};
    return (uint32_t(h[0]) << 24 | uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | uint32_t(h[3])) & 0x7fffffffu;
}

std::string bip85_phrase(ByteSpan seed, uint32_t index, unsigned words)
{
    auto m{bip32::master(seed)};
    if (!m) throw std::runtime_error("BIP-32: this seed makes no master key");
    std::string phrase{bip85_phrase_from_master(*m, index, words)};
    sodium_memzero(m->secret.data(), m->secret.size());
    return phrase;
}

std::string bip85_phrase_from_master(const bip32::Xpriv& m, uint32_t index, unsigned words)
{
    if (words != 12 && words != 18 && words != 24) throw std::invalid_argument("BIP-85: 12, 18 or 24 words");
    if (index >= bip32::HARDENED) throw std::invalid_argument("BIP-85: the index is below 2^31");
    auto k{bip32::derive_path(m, {83696968u | bip32::HARDENED, 39u | bip32::HARDENED, 0u | bip32::HARDENED, words | bip32::HARDENED, index | bip32::HARDENED})};
    if (!k) throw std::runtime_error("BIP-32: no key at this path");
    ByteArray<64> e{hd::hmac_sha512(text("bip-entropy-from-k"), k->secret.span())};
    sodium_memzero(k->secret.data(), k->secret.size());
    const size_t len{words == 12 ? 16u : words == 18 ? 24u : 32u};
    std::string phrase{hd::mnemonic_from_entropy(ByteSpan{e.data(), len})};
    sodium_memzero(e.data(), e.size());
    return phrase;
}

std::string dropin_phrase(ByteSpan seed, std::string_view stem, std::string_view network) { return bip85_phrase(seed, dropin_phrase_index(stem, network), 12); }

std::string seed_fingerprint(ByteSpan seed64)
{
    const uint8_t zero{0};
    const Hash32 h{sha256_parts({text("sidechain/dropin/fingerprint/v1"), ByteSpan{&zero, 1}, seed64})};
    return hex_encode(ByteSpan{h.data(), 8});
}

} // namespace bt::appui
