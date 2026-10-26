// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The height of the block of a sidechain last anchored on the mainchain (blind merged mining): what the drop-in apps
// take as the floor of the network's height (bt::appui::sync_state). Display only, never what a node follows.
#pragma once

#include <bt/bytes.h>

#include <cstdint>
#include <optional>

namespace bt::sidechain {

/** The latest mainchain blocks walked back from the mainchain tip (two days of eCash blocks). */
constexpr uint32_t ANCHOR_DEPTH = 288;

/**
 * From the mainchain tip `main` back (at most `depth` blocks the archive has), the first BMM commitment to a block of
 * this chain the archive knows and has not marked invalid: its height. None when there is none (a commitment to a
 * block not known yet is passed over: it may never be published).
 */
template <class Archive, class Txn>
std::optional<uint32_t> anchored_height(const Archive& a, const Txn& t, std::optional<Hash32> main, uint32_t depth = ANCHOR_DEPTH)
{
    for (uint32_t i = 0; main && !main->is_zero() && i < depth; ++i) {
        if (a.has_main_block_info(t, *main)) {
            const auto bi{a.main_block_info(t, *main)};
            if (bi.bmm_commitment && !a.invalid(t, *bi.bmm_commitment)) {
                if (auto h{a.height(t, *bi.bmm_commitment)}) return h;
            }
        }
        const auto m{a.main_header(t, *main)};
        if (!m) break;
        main = m->prev;
    }
    return std::nullopt;
}

} // namespace bt::sidechain
