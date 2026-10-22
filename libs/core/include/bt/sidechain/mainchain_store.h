// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Chain-independent pieces of the LayerTwo-family block archive: 256-bit work arithmetic,
// stored mainchain header infos, BMM results, tips, and the on-disk encoding of the
// enforcer's BlockInfo. Shared by every chain's Archive (bt/sidechain/archive.h).
#pragma once

#include <bt/bytes.h>
#include <bt/codec.h>
#include <bt/mainchain.h>

#include <compare>
#include <stdexcept>

namespace bt::sidechain {

enum class BmmResult : uint8_t { Verified = 0, Failed = 1 };

/** 256-bit little-endian unsigned integer (bitcoin::Work arithmetic). */
struct U256 {
    std::array<uint64_t, 4> w{}; // w[0] least significant
    static U256 from_le(const Hash32& b);
    Hash32 to_le() const;
    Hash32 to_be() const;
    U256 operator+(const U256& o) const;
    friend auto operator<=>(const U256& a, const U256& b)
    {
        for (int i = 3; i >= 0; i--)
            if (auto c = a.w[i] <=> b.w[i]; c != 0) return c;
        return std::strong_ordering::equal;
    }
    friend bool operator==(const U256&, const U256&) = default;
};

struct MainHeader {
    Hash32 hash, prev;
    uint32_t height = 0;
    U256 work, total_work;
    Hash32 skip; // ancestor used for O(log n) nth-ancestor jumps
    uint64_t timestamp = 0; // BlockHeaderInfo.timestamp (0 in records written before it was kept)
};

/** A sidechain tip and the mainchain block that commits to it (types/lib.rs Tip). */
struct TipRef {
    Hash32 block_hash;
    Hash32 main_block_hash;
    bool operator==(const TipRef&) const = default;
};

class ArchiveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void enc_block_info(Writer& w, const mainchain::BlockInfo& bi);
mainchain::BlockInfo dec_block_info(Reader& r);

/** Skip-list height used for mainchain ancestor jumps (Bitcoin Core GetSkipHeight). */
uint32_t skip_height(uint32_t h);

} // namespace bt::sidechain
