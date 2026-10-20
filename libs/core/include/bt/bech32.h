// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Bech32 / Bech32m (BIP-173 / BIP-350) for arbitrary 8-bit payloads, as the `bech32` 0.11
// crate's `encode::<Ck>(hrp, data)` / `decode(s)` do (used for BitNames' encryption and
// verifying keys: "bn-enc1...", "bn-svk1...").
#pragma once

#include <bt/bytes.h>

#include <optional>
#include <string>
#include <string_view>

namespace bt::bech32 {

enum class Variant { Bech32, Bech32m };

/** Lowercase encoding of hrp + 8-bit data; nullopt if the hrp is invalid or the result is
 *  longer than the code's 1023-character limit. */
std::optional<std::string> encode(std::string_view hrp, ByteSpan data, Variant v = Variant::Bech32m);

struct Decoded {
    std::string hrp; // lowercase
    Bytes data;
    Variant variant;
};
/** Accepts all-lowercase or all-uppercase strings with a valid Bech32 or Bech32m checksum
 *  and zero padding bits. */
std::optional<Decoded> decode(std::string_view s);

/** decode() and require the canonical (lowercase Bech32m) form with this hrp, as BitNames'
 *  `bech32m_decode` does by re-encoding and comparing. */
std::optional<Bytes> decode_bech32m_canonical(std::string_view s, std::string_view hrp);

} // namespace bt::bech32
