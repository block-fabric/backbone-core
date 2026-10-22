// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The side-tips index of the LayerTwo-family forks without Utreexo (lib/archive/side_tips/mod.rs
// and its users in lib/archive/mod.rs put_body/delete_body, lib/node/mainchain_task.rs
// sync_side_tips_to_tip and lib/node/net_task.rs handle_mainchain_block_event/NewTipReady).
// The files are identical in the plain-bitnames fork v0.18.2 (a38200d) and the plain-bitassets
// fork v0.17.1 (16f2f69); moved here from backbone-bitnames (bitnames/side_tips.{h,cpp}).
//
// It decides what a mainchain reorg does to the sidechain, and their rule is a quirk we
// reproduce on purpose: disconnecting a mainchain block removes the sidechain block it
// BMM-committed only from the ORDERED tips (side_tips/mod.rs:339-352), never from the tip set,
// so the state tip keeps sidechain blocks whose BMM block was reorged away; the only
// "disconnect" (net_task.rs:933-944) fires for a state tip outside the tip set, which a
// mainchain reorg never causes.
#pragma once

#include <bt/kv.h>
#include <bt/log.h>
#include <bt/sidechain/archive.h>

#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>

namespace bt::sidechain {

/** THE switch for their side-tips halt (side_tips/mod.rs:339-352): disconnecting a main block
 *  whose BMM commitment is outside the tip set, or no longer ordered, fails their mainchain
 *  sync on every retry (their node freezes at the old mainchain tip). true = reproduce it;
 *  false (default, pending the user's decision) = log a warning and treat the commitment as
 *  a no-op. A liveness choice: which sidechain blocks are connected does not depend on it
 *  while their nodes still follow the mainchain. */
inline constexpr bool REPRODUCE_SIDE_TIPS_HALT = false;

/** side_tips/mod.rs SidechainTipInfo. `work` = total work of `main_block_hash`. */
struct SideTipInfo {
    uint32_t sidechain_height = 0;
    U256 work;
    /** Main block hash with the least amount of work (the first one recorded). */
    Hash32 main_block_hash;
};

/** The fork's side_tips::Error texts (thiserror Display). */
class SideTipsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace side_tips_detail {
inline Bytes unit_key() { return Bytes{0}; }
inline Bytes enc_info(const SideTipInfo& i)
{
    Writer w;
    w.u32(i.sidechain_height);
    w.raw(i.work.to_le());
    w.raw(i.main_block_hash);
    return w.buf;
}
inline SideTipInfo dec_info(ByteSpan v)
{
    Reader r(v);
    SideTipInfo i;
    i.sidechain_height = r.u32();
    i.work = U256::from_le(r.array<32>());
    i.main_block_hash = r.array<32>();
    return i;
}
inline std::string side_hex(const Hash32& h) { return hex_encode(h); }
inline std::string main_hex(const Hash32& h) { return hex_encode_rev(h.span()); }
/** their `MainchainTip::block_hash()`: all zeros when there is no tip. */
inline Hash32 tip_or_zero(const std::optional<Hash32>& h) { return h ? *h : Hash32{}; }
} // namespace side_tips_detail

template <class C>
class SideTips {
public:
    using Archive = bt::sidechain::Archive<C>;
    using Txn = kv::Txn;

    /** Defaults to REPRODUCE_SIDE_TIPS_HALT; tests cover both. */
    bool halt_on_missing_commitment = REPRODUCE_SIDE_TIPS_HALT;

    // T_MAIN_TIP: () -> main block hash (absent = none, their `tip_info: None`).
    // T_TIPS: side hash -> info (`sidechain_tips`).
    // T_ORDERED: height BE || work BE || main hash -> side hash (`ordered_sidechain_tips`,
    //   LMDB byte order of the bincode key: the same order).
    enum Table : uint8_t { T_MAIN_TIP = 83, T_TIPS = 84, T_ORDERED = 85 };

    /** Operator rebuild: drop the index; the next mainchain sync rebuilds it. */
    void clear(Txn& t) const
    {
        t.clear(T_MAIN_TIP);
        t.clear(T_TIPS);
        t.clear(T_ORDERED);
    }

    std::optional<Hash32> mainchain_tip(const Txn& t) const
    {
        auto v = t.get(T_MAIN_TIP, side_tips_detail::unit_key());
        if (!v || v->size() != 32) return std::nullopt;
        return Hash32::from_span(*v);
    }
    bool contains(const Txn& t, const Hash32& side) const { return t.get(T_TIPS, side.span()).has_value(); }
    std::optional<SideTipInfo> info(const Txn& t, const Hash32& side) const
    {
        auto v = t.get(T_TIPS, side.span());
        if (!v) return std::nullopt;
        return side_tips_detail::dec_info(*v);
    }
    /** best_side_tip: the last ordered tip. */
    std::optional<std::pair<Hash32, SideTipInfo>> best(const Txn& t) const
    {
        auto l = t.last(T_ORDERED);
        if (!l) return std::nullopt;
        Hash32 side = Hash32::from_span(l->second);
        auto i = info(t, side);
        if (!i) throw SideTipsError("ordered side tip without tip info " + side_tips_detail::side_hex(side));
        return std::make_pair(side, *i);
    }
    bool in_ordered(const Txn& t, const Hash32& side) const
    {
        auto i = info(t, side);
        return i && t.get(T_ORDERED, ordered_key(*i)).has_value();
    }

    /** side_tips/mod.rs:194-235 connect_sidechain_tip: no-op when the block is already in the
     *  tip set (even when it is no longer ordered: it is NOT re-ordered). */
    void connect_sidechain_tip(Txn& t, const Hash32& main, const U256& work, const Hash32& side, const std::optional<Hash32>& prev_side) const
    {
        uint32_t height = 0;
        if (prev_side) {
            auto p = info(t, *prev_side);
            if (!p) throw SideTipsError("missing sidechain parent (" + side_tips_detail::side_hex(*prev_side) + ")");
            height = p->sidechain_height + 1; // saturating_add; u32::MAX is unreachable
        }
        if (contains(t, side)) return;
        SideTipInfo i{height, work, main};
        t.put(T_TIPS, side.span(), side_tips_detail::enc_info(i));
        t.put(T_ORDERED, ordered_key(i), side.span());
    }
    /** side_tips/mod.rs:240-253 disconnect_sidechain_tip (delete_body of an invalid body). */
    void disconnect_sidechain_tip(Txn& t, const Hash32& side) const
    {
        auto i = info(t, side);
        if (!i) return;
        t.del(T_ORDERED, ordered_key(*i));
        t.del(T_TIPS, side.span());
    }
    /** side_tips/mod.rs:258-293 connect_mainchain_tip; commitment = (side block, its prev_side_hash). */
    void connect_mainchain_tip(Txn& t, const Archive& a, const Hash32& main, const std::optional<std::pair<Hash32, std::optional<Hash32>>>& commitment) const
    {
        using namespace side_tips_detail;
        auto hi = a.need_main(t, main);
        auto cur = mainchain_tip(t);
        Hash32 expected = tip_or_zero(cur);
        if (hi.prev != expected)
            throw SideTipsError("failed to connect mainchain tip (" + main_hex(main) + "): invalid mainchain parent, expected (" + main_hex(expected) + ")");
        if (cur) {
            uint32_t th = a.need_main(t, *cur).height;
            if (hi.height != th + 1)
                throw SideTipsError("failed to connect mainchain tip (" + main_hex(main) + "): invalid mainchain tip height, expected (" + std::to_string(th + 1) + ")");
        }
        // their cumulative work, summed from the first mainchain block, is the archive's total work
        if (commitment) connect_sidechain_tip(t, main, hi.total_work, commitment->first, commitment->second);
        t.put(T_MAIN_TIP, unit_key(), main.span());
    }
    /** side_tips/mod.rs:295-359 disconnect_mainchain_tip; commitment = the RAW bmm_commitment.
     *  THE QUIRK: the committed sidechain block leaves only the ordered tips (and only when it
     *  was recorded at this main block); it stays in the tip set. */
    void disconnect_mainchain_tip(Txn& t, const Archive& a, const std::optional<Hash32>& prev, const std::optional<Hash32>& commitment) const
    {
        using namespace side_tips_detail;
        auto cur = mainchain_tip(t);
        if (!cur) throw SideTipsError("no mainchain tip to disconnect");
        auto tip = a.need_main(t, *cur);
        std::optional<Hash32> new_tip;
        if (prev) {
            auto p = a.need_main(t, *prev);
            if (p.hash != tip.prev) throw SideTipsError("invalid mainchain parent, expected (" + main_hex(tip.prev) + ")");
            if (p.height + 1 != tip.height) throw SideTipsError("invalid mainchain parent height, expected (" + std::to_string(tip.height) + " - 1)");
            if (!tip.prev.is_zero()) new_tip = p.hash;
        } else if (!tip.prev.is_zero()) {
            throw SideTipsError("invalid mainchain parent, expected (" + main_hex(tip.prev) + ")");
        }
        if (commitment) {
            auto i = info(t, *commitment);
            // a commitment to a block outside the tip set (unknown, body never received, failed
            // BMM) fails here: their mainchain task then fails on every retry; likewise a block
            // whose recorded main block was disconnected before (and reconnected: it is never
            // re-ordered), which is no longer ordered
            bool missing = !i || (i->main_block_hash == tip.hash && !t.del(T_ORDERED, ordered_key(*i)));
            if (missing) {
                std::string e = "missing sidechain tip (" + side_hex(*commitment) + ")";
                if (halt_on_missing_commitment) throw SideTipsError(e);
                LOG_WARN("disconnecting mainchain block %s: %s; ignored (their node halts here)", main_hex(tip.hash).c_str(), e.c_str());
            }
        }
        if (new_tip) t.put(T_MAIN_TIP, unit_key(), new_tip->span());
        else t.del(T_MAIN_TIP, unit_key());
    }

    static Bytes ordered_key(const SideTipInfo& i)
    {
        // bincode of SidechainTipInfo: [u8; 4] BE height, Work as serialize_bytes (u64 length 32,
        // then the BE bytes), BlockHash as serialize_bytes (length, internal bytes). The constant
        // length prefixes do not change the order, so they are left out.
        Writer w;
        w.u32be(i.sidechain_height);
        w.raw(i.work.to_be());
        w.raw(i.main_block_hash);
        return w.buf;
    }
};

/** mainchain_task.rs:270-412 sync_side_tips_to_tip: disconnect to the common ancestor, connect up
 *  to main_tip. Returns the number of mainchain blocks disconnected + connected (= the events
 *  their task emits). Throws (ArchiveError/SideTipsError) as their task fails; the caller drops
 *  the transaction. `progress(done, total, tip_height)` per 32 connected blocks. */
template <class C>
size_t sync_side_tips(kv::Txn& t, const Archive<C>& a, const SideTips<C>& st, const Hash32& main_tip,
                      const std::function<void(uint32_t, uint32_t, uint32_t)>& progress = {})
{
    using namespace side_tips_detail;
    auto side_main = st.mainchain_tip(t);
    Hash32 common;
    if (side_main && !main_tip.is_zero()) common = a.last_common_main_ancestor(t, main_tip, *side_main);
    size_t events = 0;
    std::optional<Hash32> cur = side_main;
    // disconnect mainchain state tip until common ancestor is reached
    while (cur && *cur != common) {
        auto hi = a.need_main(t, *cur);
        std::optional<Hash32> prev;
        if (!hi.prev.is_zero()) prev = a.need_main(t, hi.prev).hash;
        if (!a.has_main_block_info(t, *cur)) throw ArchiveError("no mainchain block info for block hash " + main_hex(*cur));
        auto commitment = a.main_block_info(t, *cur).bmm_commitment;
        st.disconnect_mainchain_tip(t, a, prev, commitment);
        events++;
        cur = prev;
    }
    // connect mainchain state tip until mainchain tip is reached
    if (tip_or_zero(cur) == main_tip) return events;
    std::vector<Hash32> path; // main_tip back to (excluding) cur
    for (Hash32 h = main_tip; !h.is_zero() && h != tip_or_zero(cur);) {
        path.push_back(h);
        h = a.need_main(t, h).prev;
    }
    uint32_t tip_height = a.need_main(t, main_tip).height;
    uint32_t total = uint32_t(path.size()), done = 0;
    if (progress) progress(0, total, tip_height);
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const Hash32& main = *it;
        if (!a.has_main_block_info(t, main)) throw ArchiveError("no mainchain block info for block hash " + main_hex(main));
        auto bi = a.main_block_info(t, main);
        std::optional<std::pair<Hash32, std::optional<Hash32>>> commitment;
        // an invalidated block has no body in their archive (invalidate_block deletes it)
        if (bi.bmm_commitment && a.has_body(t, *bi.bmm_commitment) && !a.invalid(t, *bi.bmm_commitment)) {
            const Hash32& side = *bi.bmm_commitment;
            auto sh = a.need_header(t, side);
            if (!sh.prev_side_hash || st.contains(t, *sh.prev_side_hash)) {
                auto res = a.bmm_results(t, side);
                auto r = res.find(main);
                if (r == res.end()) throw ArchiveError("no BMM result with block " + side_hex(side));
                if (r->second == BmmResult::Verified) commitment = std::make_pair(side, sh.prev_side_hash);
            }
        }
        st.connect_mainchain_tip(t, a, main, commitment);
        events++;
        if (progress && (++done % 32 == 0 || done == total)) progress(done, total, tip_height);
    }
    return events;
}

/** archive/mod.rs:672-730: put_body's side-tips update (call after Archive::put_body). */
template <class C>
void side_tips_put_body(kv::Txn& t, const Archive<C>& a, const SideTips<C>& st, const Hash32& block)
{
    using namespace side_tips_detail;
    if (a.invalid(t, block)) return;
    Hash32 main_tip = tip_or_zero(st.mainchain_tip(t));
    std::deque<Hash32> queue;
    auto update = [&](const Hash32& h, const typename C::Header& hd) {
        bool connected = false;
        // their HashMap order; at most one Verified main block (a child of prev_main) can be
        // on the active mainchain, so the order does not matter
        for (auto& [m, r] : a.bmm_results(t, h)) {
            if (r != BmmResult::Verified) continue;
            if (hd.prev_side_hash && !st.contains(t, *hd.prev_side_hash)) continue;
            if (!a.is_main_descendant(t, m, main_tip)) continue; // throws without a mainchain tip
            st.connect_sidechain_tip(t, m, a.total_work(t, m), h, hd.prev_side_hash);
            connected = true;
        }
        if (connected)
            for (auto& s : a.successors(t, h)) queue.push_back(s);
    };
    update(block, a.need_header(t, block));
    while (!queue.empty()) {
        Hash32 h = queue.front();
        queue.pop_front();
        if (!a.has_body(t, h) || a.invalid(t, h)) continue;
        update(h, a.need_header(t, h));
    }
}

/** archive/mod.rs:761-776: delete_body's side-tips update (call after Archive::delete_body). */
template <class C>
void side_tips_delete_body(kv::Txn& t, const Archive<C>& a, const SideTips<C>& st, const Hash32& block)
{
    std::deque<Hash32> queue{block};
    while (!queue.empty()) {
        Hash32 h = queue.front();
        queue.pop_front();
        if (!st.contains(t, h)) continue;
        st.disconnect_sidechain_tip(t, h);
        for (auto& s : a.successors(t, h)) queue.push_back(s);
    }
}

/** net_task.rs NewTipReady: a tip is reorged to only when it is in the tip set and its main
 *  block is an ancestor of (or is) the side-tips mainchain tip. */
template <class C>
bool side_tips_tip_ready(const kv::Txn& t, const Archive<C>& a, const SideTips<C>& st, const TipRef& tip)
{
    // policy: never an operator-invalidated block (its descendants are marked invalid with it
    // and no new descendant can be stored: the header is gone)
    if (!st.contains(t, tip.block_hash) || a.invalid(t, tip.block_hash)) return false;
    return a.is_main_descendant(t, tip.main_block_hash, side_tips_detail::tip_or_zero(st.mainchain_tip(t)));
}

/** peer/task.rs check_peer_tip_and_request_headers: a peer tip is considered only when its
 *  main block is known, the side tips have a mainchain tip, and the peer's main block is an
 *  ancestor of (or is) it. */
template <class C>
bool side_tips_peer_tip_relevant(const kv::Txn& t, const Archive<C>& a, const SideTips<C>& st, const Hash32& peer_main)
{
    if (!a.main_header(t, peer_main)) return false;
    auto side_main = st.mainchain_tip(t);
    if (!side_main) return false;
    return a.is_main_descendant(t, peer_main, *side_main);
}

} // namespace bt::sidechain
