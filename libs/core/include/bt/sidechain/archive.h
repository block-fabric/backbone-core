// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Block archive shared by the LayerTwo-family chains: side headers/bodies, mainchain header
// infos and block infos, BMM verification results and fork choice. Port of their common
// lib/archive/mod.rs (identical in thunder-rust v0.18.2 and plain-bitnames fork v0.18.2 apart
// from Thunder's accumulator table): put_header BMM rules, put_main_block_info, better_tip.
//
// A chain plugs in through a traits class C:
//   using Header = ...;   // with std::optional<Hash32> prev_side_hash; Hash32 prev_main_hash
//   using Body = ...;
//   static Hash32 hash(const Header&);
//   static void encode_header(Writer&, const Header&);  static Header decode_header(Reader&);
//   static void encode_body(Writer&, const Body&);      static Body decode_body(Reader&);
//   static std::vector<Hash32> txids(const Body&);
// Table ids are fixed (40..52) so every chain's datadir uses the same layout.
#pragma once

#include <bt/kv.h>
#include <bt/sidechain/mainchain_store.h>

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace bt::sidechain {

template <class C>
class Archive {
public:
    using Header = typename C::Header;
    using Body = typename C::Body;
    using Txn = bt::kv::Txn;

    enum Table : uint8_t {
        A_HEADERS = 40, A_BODIES, A_HEIGHT, A_EXP_ANC, A_SUCC, A_BMM, A_AUX, A_TX_INCL, A_MAIN_HDR, A_MAIN_INFO, A_MAIN_SUCC,
        A_INVALID, A_HEIGHT_INDEX
    };

    // ------------------------------------------------------------------ side chain
    std::optional<Header> header(const Txn& t, const Hash32& h) const
    {
        auto v = t.get(A_HEADERS, h.span());
        if (!v) return std::nullopt;
        Reader r(*v);
        return C::decode_header(r);
    }
    std::optional<Body> body(const Txn& t, const Hash32& h) const
    {
        auto v = t.get(A_BODIES, h.span());
        if (!v) return std::nullopt;
        Reader r(*v);
        return C::decode_body(r);
    }
    bool has_body(const Txn& t, const Hash32& h) const { return t.get(A_BODIES, h.span()).has_value(); }
    std::optional<uint32_t> height(const Txn& t, const Hash32& h) const
    {
        auto v = t.get(A_HEIGHT, h.span());
        if (!v) return std::nullopt;
        Reader r(*v);
        return r.u32();
    }
    bool invalid(const Txn& t, const Hash32& h) const { return t.get(A_INVALID, h.span()).has_value(); }
    std::vector<Hash32> successors(const Txn& t, const std::optional<Hash32>& h) const { return dec_hash_list(t.get(A_SUCC, opt_key(h))); }

    std::map<Hash32, BmmResult> bmm_results(const Txn& t, const Hash32& side) const
    {
        std::map<Hash32, BmmResult> r;
        auto v = t.get(A_BMM, side.span());
        if (!v) return r;
        for (size_t i = 0; i + 33 <= v->size(); i += 33) r[Hash32::from_span(ByteSpan(v->data() + i, 32))] = BmmResult((*v)[i + 32]);
        return r;
    }

    Hash32 nth_ancestor(const Txn& t, Hash32 h, uint32_t n) const
    {
        Hash32 orig = h;
        while (n > 0) {
            uint32_t ht = need_height(t, h);
            if (n > ht) throw ArchiveError("NoAncestor " + hex_encode(orig));
            if (n == 1) return *need_header(t, h).prev_side_hash;
            uint32_t idx = 31 - uint32_t(__builtin_clz(n)) - 1; // ilog2(n) - 1
            auto exp = dec_hash_list(t.get(A_EXP_ANC, h.span()));
            if (idx >= exp.size()) throw ArchiveError("exponential ancestors missing");
            h = exp[idx];
            n -= 2u << idx;
        }
        return h;
    }

    bool is_descendant(const Txn& t, const Hash32& anc, const Hash32& desc) const
    {
        if (anc == desc) return true;
        uint32_t ha = need_height(t, anc), hd = need_height(t, desc);
        if (ha > hd) return false;
        return anc == nth_ancestor(t, desc, hd - ha);
    }

    std::optional<Hash32> last_common_ancestor(const Txn& t, Hash32 a, Hash32 b) const
    {
        uint32_t ha = need_height(t, a), hb = need_height(t, b);
        if (ha < hb) b = nth_ancestor(t, b, hb - ha);
        else if (ha > hb) {
            a = nth_ancestor(t, a, ha - hb);
            ha = hb;
        }
        if (a == b) return a;
        if (nth_ancestor(t, a, ha) != nth_ancestor(t, b, ha)) return std::nullopt;
        uint32_t lo = 1, hi = ha;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (nth_ancestor(t, a, mid) == nth_ancestor(t, b, mid)) hi = mid;
            else lo = mid + 1;
        }
        return nth_ancestor(t, a, hi);
    }

    std::vector<Hash32> block_locator(const Txn& t, const Hash32& h) const
    {
        auto hd = need_header(t, h);
        auto res = dec_hash_list(t.get(A_EXP_ANC, h.span()));
        if (hd.prev_side_hash) res.insert(res.begin(), *hd.prev_side_hash);
        return res;
    }

    /** Ancestors from h (inclusive) back to genesis. */
    std::vector<Hash32> ancestors(const Txn& t, const Hash32& h) const
    {
        std::vector<Hash32> r;
        std::optional<Hash32> cur = h;
        while (cur) {
            r.push_back(*cur);
            cur = need_header(t, *cur).prev_side_hash;
        }
        return r;
    }

    std::vector<Hash32> missing_bodies(const Txn& t, const Hash32& h, const std::optional<Hash32>& stop) const
    {
        std::vector<Hash32> r;
        for (auto& a : ancestors(t, h)) {
            if (stop && a == *stop) break;
            if (!t.get(A_BODIES, a.span())) r.push_back(a);
        }
        std::reverse(r.begin(), r.end());
        return r;
    }

    /** Blocks that include txid and the index in each (BTreeMap<BlockHash, u32>). */
    std::map<Hash32, uint32_t> tx_inclusions(const Txn& t, const Hash32& txid) const
    {
        std::map<Hash32, uint32_t> r;
        auto v = t.get(A_TX_INCL, txid.span());
        if (!v) return r;
        for (size_t i = 0; i + 36 <= v->size(); i += 36) {
            Reader rd(ByteSpan(v->data() + i + 32, 4));
            r[Hash32::from_span(ByteSpan(v->data() + i, 32))] = rd.u32();
        }
        return r;
    }

    /** archive/mod.rs put_header: requires the parent header; records BMM results against
     *  the already-known children of prev_main. */
    void put_header(Txn& t, const Header& h) const
    {
        uint32_t ht = 0;
        if (h.prev_side_hash) {
            auto p = height(t, *h.prev_side_hash);
            if (!p) throw ArchiveError("InvalidPrevSideHash");
            ht = *p + 1;
        }
        Hash32 bh = C::hash(h);
        {
            Writer w;
            w.u32(ht);
            t.put(A_HEIGHT, bh.span(), w.buf);
            Writer hw;
            C::encode_header(hw, h);
            t.put(A_HEADERS, bh.span(), hw.buf);
        }
        auto succ = successors(t, h.prev_side_hash);
        if (std::find(succ.begin(), succ.end(), bh) == succ.end()) succ.push_back(bh);
        t.put(A_SUCC, opt_key(h.prev_side_hash), enc_hash_list(succ));
        if (!t.get(A_SUCC, opt_key(bh))) t.put(A_SUCC, opt_key(bh), {});
        std::vector<Hash32> exp;
        if (ht >= 2) {
            exp.push_back(nth_ancestor(t, *h.prev_side_hash, 1));
            uint64_t depth = 4;
            while (uint64_t(ht) >= depth) {
                exp.push_back(nth_ancestor(t, exp.back(), uint32_t(depth / 2)));
                depth *= 2;
            }
        }
        t.put(A_EXP_ANC, bh.span(), enc_hash_list(exp));
        auto results = bmm_results(t, bh);
        std::optional<std::map<Hash32, BmmResult>> parent_results;
        if (h.prev_side_hash) parent_results = bmm_results(t, *h.prev_side_hash);
        for (auto& mb : main_successors(t, h.prev_main_hash)) {
            auto info = main_block_info(t, mb);
            if (!info.bmm_commitment || *info.bmm_commitment != bh) {
                results[mb] = BmmResult::Failed;
                continue;
            }
            auto mh = need_main(t, mb);
            if (h.prev_main_hash != mh.prev) {
                results[mb] = BmmResult::Failed;
                continue;
            }
            if (!parent_results) {
                results[mb] = BmmResult::Verified;
                continue;
            }
            bool ok = false;
            for (auto& [pm, pr] : *parent_results)
                if (pr == BmmResult::Verified && is_main_descendant(t, pm, mb)) {
                    ok = true;
                    break;
                }
            results[mb] = ok ? BmmResult::Verified : BmmResult::Failed;
        }
        put_bmm_results(t, bh, results);
    }

    void put_body(Txn& t, const Hash32& h, const Body& b) const
    {
        need_header(t, h);
        Writer w;
        C::encode_body(w, b);
        t.put(A_BODIES, h.span(), w.buf);
        auto ids = C::txids(b);
        for (uint32_t i = 0; i < ids.size(); i++) {
            auto inc = tx_inclusions(t, ids[i]);
            inc[h] = i;
            put_inclusions(t, ids[i], inc);
        }
    }

    void delete_body(Txn& t, const Hash32& h) const
    {
        auto b = body(t, h);
        t.del(A_BODIES, h.span());
        if (!b) return;
        for (auto& txid : C::txids(*b)) {
            auto inc = tx_inclusions(t, txid);
            inc.erase(h);
            put_inclusions(t, txid, inc);
        }
    }

    /** Per-block auxiliary data a chain keeps next to its archive (Thunder: accumulator). */
    void put_aux(Txn& t, const Hash32& h, ByteSpan v) const { t.put(A_AUX, h.span(), v); }
    std::optional<Bytes> aux(const Txn& t, const Hash32& h) const { return t.get(A_AUX, h.span()); }

    /** archive/mod.rs get_best_main_verification: the Verified result with the most work. */
    std::optional<Hash32> best_main_verification(const Txn& t, const Hash32& side) const
    {
        std::optional<Hash32> best;
        U256 best_work;
        for (auto& [m, r] : bmm_results(t, side)) {
            if (r != BmmResult::Verified) continue;
            U256 w = total_work(t, m);
            if (!best || w >= best_work) { // max_by_key keeps the last maximum
                best = m;
                best_work = w;
            }
        }
        return best;
    }

    void mark_invalid(Txn& t, const Hash32& h) const
    {
        std::vector<Hash32> stack{h};
        while (!stack.empty()) {
            Hash32 b = stack.back();
            stack.pop_back();
            auto hd = header(t, b);
            if (hd) {
                auto ps = successors(t, hd->prev_side_hash);
                ps.erase(std::remove(ps.begin(), ps.end(), b), ps.end());
                t.put(A_SUCC, opt_key(hd->prev_side_hash), enc_hash_list(ps));
            }
            t.del(A_HEADERS, b.span());
            t.put(A_INVALID, b.span(), {});
            for (auto& s : successors(t, b)) stack.push_back(s);
            t.del(A_SUCC, opt_key(b));
        }
    }

    // ------------------------------------------------------------------ mainchain
    std::optional<MainHeader> main_header(const Txn& t, const Hash32& h) const
    {
        auto v = t.get(A_MAIN_HDR, h.span());
        if (!v) return std::nullopt;
        Reader r(*v);
        MainHeader m;
        m.hash = h;
        m.prev = r.array<32>();
        m.height = r.u32();
        m.work = U256::from_le(r.array<32>());
        m.total_work = U256::from_le(r.array<32>());
        m.skip = r.array<32>();
        if (!r.empty()) m.timestamp = r.u64();
        return m;
    }
    mainchain::BlockInfo main_block_info(const Txn& t, const Hash32& h) const
    {
        auto v = t.get(A_MAIN_INFO, h.span());
        if (!v) return {};
        Reader r(*v);
        return dec_block_info(r);
    }
    bool has_main_block_info(const Txn& t, const Hash32& h) const { return t.get(A_MAIN_INFO, h.span()).has_value(); }
    U256 total_work(const Txn& t, const Hash32& main) const { return need_main(t, main).total_work; }
    std::vector<Hash32> main_successors(const Txn& t, const Hash32& main) const { return dec_hash_list(t.get(A_MAIN_SUCC, main.span())); }

    Hash32 nth_main_ancestor(const Txn& t, Hash32 h, uint32_t n) const
    {
        if (n == 0) return h;
        MainHeader cur = need_main(t, h);
        if (n > cur.height) throw ArchiveError("NoMainAncestor");
        uint32_t target = cur.height - n;
        while (cur.height > target) {
            uint32_t hs = skip_height(cur.height), hsp = skip_height(cur.height - 1);
            if (cur.height >= 2 && (hs == target || (hs > target && !(hsp + 2 < hs && hsp >= target)))) cur = need_main(t, cur.skip);
            else {
                if (cur.prev.is_zero()) throw ArchiveError("NoMainAncestor");
                cur = need_main(t, cur.prev);
            }
        }
        return cur.hash;
    }

    /** archive/mod.rs put_main_header_info */
    void put_main_header_info(Txn& t, const mainchain::BlockHeaderInfo& hi) const
    {
        MainHeader m;
        m.hash = hi.block_hash;
        m.prev = hi.prev_block_hash;
        m.work = U256::from_le(hi.work);
        m.timestamp = hi.timestamp;
        if (hi.prev_block_hash.is_zero()) {
            m.height = 0;
            m.total_work = m.work;
        } else {
            auto p = main_header(t, hi.prev_block_hash);
            if (!p) throw ArchiveError("NoMainHeaderInfo (parent)");
            m.height = p->height + 1;
            m.total_work = p->total_work + m.work;
            uint32_t sh = skip_height(m.height);
            m.skip = sh == m.height - 1 ? p->hash : nth_main_ancestor(t, p->hash, p->height - sh);
        }
        Writer w;
        w.raw(m.prev);
        w.u32(m.height);
        w.raw(m.work.to_le());
        w.raw(m.total_work.to_le());
        w.raw(m.skip);
        w.u64(m.timestamp);
        t.put(A_MAIN_HDR, m.hash.span(), w.buf);
        auto s = main_successors(t, hi.prev_block_hash);
        if (std::find(s.begin(), s.end(), m.hash) == s.end()) s.push_back(m.hash);
        t.put(A_MAIN_SUCC, hi.prev_block_hash.span(), enc_hash_list(s));
    }

    /** archive/mod.rs put_main_block_info */
    void put_main_block_info(Txn& t, const Hash32& main, const mainchain::BlockInfo& bi) const
    {
        auto mh = need_main(t, main);
        Writer w;
        enc_block_info(w, bi);
        t.put(A_MAIN_INFO, main.span(), w.buf);
        if (!bi.bmm_commitment) return;
        auto hd = header(t, *bi.bmm_commitment);
        if (!hd) return;
        BmmResult res;
        if (hd->prev_main_hash != mh.prev) res = BmmResult::Failed;
        else if (hd->prev_side_hash) {
            bool ok = false;
            for (auto& [pm, pr] : bmm_results(t, *hd->prev_side_hash))
                if (pr == BmmResult::Verified && is_main_descendant(t, pm, main)) {
                    ok = true;
                    break;
                }
            res = ok ? BmmResult::Verified : BmmResult::Failed;
        } else {
            res = BmmResult::Verified;
        }
        auto r = bmm_results(t, *bi.bmm_commitment);
        r[main] = res;
        put_bmm_results(t, *bi.bmm_commitment, r);
    }

    bool is_main_descendant(const Txn& t, const Hash32& anc, const Hash32& desc) const
    {
        if (anc == desc) return true;
        uint32_t ha = need_main(t, anc).height, hd = need_main(t, desc).height;
        if (ha > hd) return false;
        return anc == nth_main_ancestor(t, desc, hd - ha);
    }

    Hash32 last_common_main_ancestor(const Txn& t, Hash32 a, Hash32 b) const
    {
        uint32_t ha = need_main(t, a).height, hb = need_main(t, b).height;
        if (ha < hb) b = nth_main_ancestor(t, b, hb - ha);
        else if (ha > hb) {
            a = nth_main_ancestor(t, a, ha - hb);
            ha = hb;
        }
        if (a == b) return a;
        uint32_t lo = 1, hi = ha;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (nth_main_ancestor(t, a, mid) == nth_main_ancestor(t, b, mid)) hi = mid;
            else lo = mid + 1;
        }
        return nth_main_ancestor(t, a, hi);
    }

    bool shared_mainchain_lineage(const Txn& t, Hash32 a, Hash32 b) const
    {
        uint32_t ha = need_main(t, a).height, hb = need_main(t, b).height;
        if (ha < hb) b = nth_main_ancestor(t, b, hb - ha);
        else if (ha > hb) a = nth_main_ancestor(t, a, ha - hb);
        return a == b;
    }

    /** better_tip(tip0, tip1) (archive/mod.rs:1435-1650): which tip wins, or nullopt. */
    std::optional<TipRef> better_tip(const Txn& t, const TipRef& tip0, const TipRef& tip1) const
    {
        if (tip0 == tip1) return std::nullopt;
        uint32_t h0 = need_height(t, tip0.block_hash), h1 = need_height(t, tip1.block_hash);
        if (h0 == 0 && h1 == 0) return std::nullopt; // the first genesis seen sticks
        if (h0 == 0) return tip1;
        if (h1 == 0) return tip0;
        U256 w0 = total_work(t, tip0.main_block_hash), w1 = total_work(t, tip1.main_block_hash);
        auto cw = w0 <=> w1;
        auto ch = h0 <=> h1;
        if ((cw <= 0) && ch < 0) return tip1;
        if ((cw >= 0) && ch > 0) return tip0;
        if (cw < 0 && ch == 0) // within one mainchain lineage prefer LOWER work
            return shared_mainchain_lineage(t, tip0.main_block_hash, tip1.main_block_hash) ? tip0 : tip1;
        if (cw > 0 && ch == 0) return !shared_mainchain_lineage(t, tip0.main_block_hash, tip1.main_block_hash) ? tip0 : tip1;
        if (cw < 0 && ch > 0) {
            Hash32 ma = last_common_main_ancestor(t, tip0.main_block_hash, tip1.main_block_hash);
            auto a0 = ancestor_before(t, tip0.block_hash, ma, false);
            std::optional<uint32_t> ah0 = a0 ? std::optional<uint32_t>(a0->first) : std::nullopt;
            if (ah0 >= std::optional<uint32_t>(h1)) return tip0;
            return tip1;
        }
        if (cw > 0 && ch < 0) {
            Hash32 ma = last_common_main_ancestor(t, tip0.main_block_hash, tip1.main_block_hash);
            auto a1 = ancestor_before(t, tip1.block_hash, ma, false);
            std::optional<uint32_t> ah1 = a1 ? std::optional<uint32_t>(a1->first) : std::nullopt;
            if (ah1 < std::optional<uint32_t>(h0)) return tip0;
            return tip1;
        }
        if (tip0.block_hash == tip1.block_hash) return tip0;
        Hash32 ma = last_common_main_ancestor(t, tip0.main_block_hash, tip1.main_block_hash);
        auto a0 = ancestor_before(t, tip0.block_hash, ma, true);
        auto a1 = ancestor_before(t, tip1.block_hash, ma, true);
        std::optional<U256> aw0 = a0 ? std::optional<U256>(a0->second) : std::nullopt;
        std::optional<U256> aw1 = a1 ? std::optional<U256>(a1->second) : std::nullopt;
        std::optional<uint32_t> ah0 = a0 ? std::optional<uint32_t>(a0->first) : std::nullopt;
        std::optional<uint32_t> ah1 = a1 ? std::optional<uint32_t>(a1->first) : std::nullopt;
        auto cwa = aw0 <=> aw1;
        auto cha = ah0 <=> ah1;
        if ((cwa <= 0 && cha == 0) || cha > 0) return tip0;
        return tip1;
    }

    MainHeader need_main(const Txn& t, const Hash32& h) const
    {
        auto m = main_header(t, h);
        if (!m) throw ArchiveError("NoMainHeaderInfo " + hex_encode_rev(h.span()));
        return *m;
    }
    uint32_t need_height(const Txn& t, const Hash32& h) const
    {
        auto x = height(t, h);
        if (!x) throw ArchiveError("NoHeight " + hex_encode(h));
        return *x;
    }
    Header need_header(const Txn& t, const Hash32& h) const
    {
        auto x = header(t, h);
        if (!x) throw ArchiveError("NoHeader " + hex_encode(h));
        return *x;
    }

private:
    static Bytes opt_key(const std::optional<Hash32>& h)
    {
        Bytes k;
        if (h) {
            k.push_back(1);
            append(k, h->span());
        } else {
            k.push_back(0);
        }
        return k;
    }
    static std::vector<Hash32> dec_hash_list(const std::optional<Bytes>& v)
    {
        std::vector<Hash32> r;
        if (!v) return r;
        for (size_t i = 0; i + 32 <= v->size(); i += 32) r.push_back(Hash32::from_span(ByteSpan(v->data() + i, 32)));
        return r;
    }
    static Bytes enc_hash_list(const std::vector<Hash32>& l)
    {
        Bytes b;
        for (auto& h : l) append(b, h.span());
        return b;
    }
    void put_bmm_results(Txn& t, const Hash32& side, const std::map<Hash32, BmmResult>& r) const
    {
        Bytes b;
        for (auto& [m, res] : r) {
            append(b, m.span());
            b.push_back(uint8_t(res));
        }
        t.put(A_BMM, side.span(), b);
    }
    void put_inclusions(Txn& t, const Hash32& txid, const std::map<Hash32, uint32_t>& inc) const
    {
        Bytes v;
        for (auto& [bh, idx] : inc) {
            append(v, bh.span());
            Writer iw;
            iw.u32(idx);
            append(v, iw.buf);
        }
        if (v.empty()) t.del(A_TX_INCL, txid.span());
        else t.put(A_TX_INCL, txid.span(), v);
    }
    /** First ancestor of tip whose prev_main is a strict ancestor of main_ancestor; returns
     *  its height (and the work of the main block following its prev_main on
     *  main_ancestor's chain, when with_work). */
    std::optional<std::pair<uint32_t, U256>> ancestor_before(const Txn& t, const Hash32& tip, const Hash32& main_ancestor, bool with_work) const
    {
        uint32_t mah = need_main(t, main_ancestor).height;
        std::optional<Hash32> cur = tip;
        while (cur) {
            Header hd = need_header(t, *cur);
            if (is_main_descendant(t, hd.prev_main_hash, main_ancestor) && hd.prev_main_hash != main_ancestor) {
                uint32_t h = need_height(t, *cur);
                U256 w;
                if (with_work) {
                    uint32_t mheight = need_main(t, hd.prev_main_hash).height + 1;
                    Hash32 mb = nth_main_ancestor(t, main_ancestor, mah - mheight);
                    w = total_work(t, mb);
                }
                return std::make_pair(h, w);
            }
            cur = hd.prev_side_hash;
        }
        return std::nullopt;
    }
};

} // namespace bt::sidechain
