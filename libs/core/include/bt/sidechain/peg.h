// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The UTXO/STXO store and the two-way peg of the LayerTwo-family chains without Utreexo:
// a literal port of lib/state/{mod.rs (peg parts), two_way_peg_data.rs, rollback.rs} of the
// plain-bitnames fork v0.18.2 and the plain-bitassets fork v0.17.1, whose two_way_peg_data.rs
// files are identical apart from formatting. Moved here from backbone-bitnames
// (bitnames/state.cpp) so every such chain shares one copy.
//
// Their branch order is kept so that every accept/reject decision and every state write is
// identical; their panics (assert!, expect) are StateError{panic = true}.
//
// Tables and key encodings mirror their LMDB databases where the order is observable:
// deposit_blocks / withdrawal_bundle_event_blocks keyed by little-endian u32 under memcmp order
// (so last() misbehaves after 256 entries exactly as theirs), utxos/stxos by the 37-byte
// OutPointKey, withdrawal_bundles by the bincode M6id.
//
// A chain plugs in through a traits class C:
//   using FilledOutput = ...;        // with `Address address`
//   static void enc_filled(Writer&, const FilledOutput&);  static FilledOutput dec_filled(Reader&);
//   static uint64_t bitcoin_value(const FilledOutput&);    // GetBitcoinValue
//   static bool is_withdrawal(const FilledOutput&);
//   static uint64_t withdrawal_value(const FilledOutput&); static uint64_t withdrawal_main_fee(...);
//   static const btc::Address& withdrawal_address(const FilledOutput&);
//   static FilledOutput deposit_output(const mainchain::Deposit&);   // proto.rs
// Table ids 1..10 belong to this class; a chain's own tables start at 11 (archive 40..52,
// node 80..82, side tips 83..85).
#pragma once

#include <bt/crypto.h>
#include <bt/kv.h>
#include <bt/mainchain.h>
#include <bt/sidechain/rollback.h>
#include <bt/sidechain/types.h>

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace bt::sidechain {

constexpr uint32_t WITHDRAWAL_BUNDLE_FAILURE_GAP = 4; // lib/state/mod.rs

enum class BundleStatus : uint8_t { Confirmed = 0, Dropped = 1, Failed = 2, Pending = 3, Submitted = 4, SubmittedUnexpected = 5 };
inline const char* to_string(BundleStatus s)
{
    switch (s) {
    case BundleStatus::Confirmed: return "Confirmed";
    case BundleStatus::Dropped: return "Dropped";
    case BundleStatus::Failed: return "Failed";
    case BundleStatus::Pending: return "Pending";
    case BundleStatus::Submitted: return "Submitted";
    case BundleStatus::SubmittedUnexpected: return "SubmittedUnexpected";
    }
    return "?";
}

template <class FO>
struct SpentOutputT {
    FO output;
    InPoint inpoint;
    bool operator==(const SpentOutputT&) const = default;
};

/** types WithdrawalBundle: the spent UTXOs (BTreeMap order) and the full tx outputs
 *  [fee OP_RETURN, inputs-commitment OP_RETURN, payouts...]. */
template <class FO>
struct WithdrawalBundleT {
    std::map<OutPoint, FO> spend_utxos;
    std::vector<btc::TxOut> outputs;
    Bytes tx_bytes() const { return btc::serialize_inputless_tx(2, outputs, 0); }
    Hash32 m6id() const { return sha256d(tx_bytes()); }
};

template <class FO>
struct WithdrawalBundleInfoT {
    enum Kind : uint8_t { Known = 0, Unknown = 1, UnknownConfirmed = 2 } kind = Unknown;
    WithdrawalBundleT<FO> bundle;      // Known
    std::map<OutPoint, FO> spend_utxos; // UnknownConfirmed
};

/** types BlockIndexEvents: coin movements a body does not carry. */
template <class FO>
struct BlockIndexEventsT {
    std::vector<std::pair<OutPoint, FO>> deposits;
    std::vector<std::pair<OutPoint, Hash32>> bundle_spends; // (outpoint, m6id)
    bool empty() const { return deposits.empty() && bundle_spends.empty(); }
};

/** WithdrawalBundle::predict_weight (types mod.rs): fits under MAX_STANDARD_TX_WEIGHT. */
inline bool predict_weight_ok(uint32_t n_outputs, uint32_t sum_txout_sizes)
{
    auto varint_size = [](uint64_t n) { return uint64_t(btc::compact_size_len(n)); };
    auto txout_size = [&](uint64_t spk) { return 8 + varint_size(spk) + spk; };
    const uint64_t TXIN = 36 + 1 + 0 + 4; // treasury input, empty scriptSig
    uint64_t no = uint64_t(n_outputs) + 2;
    if (no > UINT32_MAX) return false;
    uint64_t sum = txout_size(34) + txout_size(10) + sum_txout_sizes;
    if (sum > UINT32_MAX) return false;
    uint64_t base = 4 + (varint_size(1) + TXIN) + (varint_size(no) + sum) + 4;
    if (base > UINT32_MAX) return false;
    return base * 4 <= 400000; // MAX_STANDARD_WEIGHT
}

template <class C>
class PegState {
public:
    using FilledOutput = typename C::FilledOutput;
    using SpentOutput = SpentOutputT<FilledOutput>;
    using WithdrawalBundle = WithdrawalBundleT<FilledOutput>;
    using WithdrawalBundleInfo = WithdrawalBundleInfoT<FilledOutput>;
    using BlockIndexEvents = BlockIndexEventsT<FilledOutput>;
    using Txn = kv::Txn;

    enum PegTable : uint8_t {
        T_TIP = 1, T_HEIGHT, T_UTXOS, T_STXOS, T_PENDING_BUNDLE, T_LATEST_FAILED, T_BUNDLES, T_DEPOSIT_BLOCKS,
        T_BUNDLE_EVENT_BLOCKS, T_BLOCK_INDEX_EVENTS
    };

    // ------------------------------------------------------------------ encodings
    static Bytes u32le(uint32_t v)
    {
        Writer w;
        w.u32(v);
        return w.buf;
    }
    static Bytes u32be(uint32_t v)
    {
        Writer w;
        w.u32be(v);
        return w.buf;
    }
    static const Bytes& unit()
    {
        static const Bytes u{};
        return u;
    }
    static void enc_spent(Writer& w, const SpentOutput& s)
    {
        C::enc_filled(w, s.output);
        w.u32(s.inpoint.kind);
        w.raw(s.inpoint.id);
        if (s.inpoint.kind == InPoint::Regular) w.u32(s.inpoint.vin);
    }
    static SpentOutput dec_spent(Reader& r)
    {
        SpentOutput s;
        s.output = C::dec_filled(r);
        s.inpoint.kind = InPoint::Kind(r.u32());
        s.inpoint.id = r.array<32>();
        if (s.inpoint.kind == InPoint::Regular) s.inpoint.vin = r.u32();
        return s;
    }
    static void enc_utxo_map(Writer& w, const std::map<OutPoint, FilledOutput>& m)
    {
        w.u64(m.size());
        for (auto& [op, o] : m) {
            bincode(w, op);
            C::enc_filled(w, o);
        }
    }
    static std::map<OutPoint, FilledOutput> dec_utxo_map(Reader& r)
    {
        std::map<OutPoint, FilledOutput> m;
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            OutPoint op = de_outpoint(r);
            m[op] = C::dec_filled(r);
        }
        return m;
    }
    static void enc_bundle_info(Writer& w, const WithdrawalBundleInfo& i)
    {
        w.u8(i.kind);
        if (i.kind == WithdrawalBundleInfo::Known) {
            enc_utxo_map(w, i.bundle.spend_utxos);
            w.u64(i.bundle.outputs.size());
            for (auto& o : i.bundle.outputs) {
                w.u64(o.value);
                w.bincode_bytes(o.script_pubkey);
            }
        } else if (i.kind == WithdrawalBundleInfo::UnknownConfirmed) {
            enc_utxo_map(w, i.spend_utxos);
        }
    }
    static WithdrawalBundleInfo dec_bundle_info(Reader& r)
    {
        WithdrawalBundleInfo i;
        i.kind = typename WithdrawalBundleInfo::Kind(r.u8());
        if (i.kind == WithdrawalBundleInfo::Known) {
            i.bundle.spend_utxos = dec_utxo_map(r);
            uint64_t n = r.u64();
            for (uint64_t k = 0; k < n; k++) {
                btc::TxOut o;
                o.value = r.u64();
                o.script_pubkey = r.bincode_bytes();
                i.bundle.outputs.push_back(std::move(o));
            }
        } else if (i.kind == WithdrawalBundleInfo::UnknownConfirmed) {
            i.spend_utxos = dec_utxo_map(r);
        }
        return i;
    }
    template <class T, class F>
    static void enc_rollback(Writer& w, const RollBack<T>& rb, F encv)
    {
        w.u64(rb.v.size());
        for (auto& hs : rb.v) {
            encv(w, hs.value);
            w.u32(hs.height);
        }
    }
    template <class T, class F>
    static RollBack<T> dec_rollback(Reader& r, F decv)
    {
        RollBack<T> rb;
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            T v = decv(r);
            uint32_t h = r.u32();
            rb.v.push_back({v, h});
        }
        if (rb.v.empty()) throw DecodeError("empty RollBack");
        return rb;
    }
    static Bytes m6id_key(const Hash32& m)
    {
        Writer w;
        w.bincode_bytes(m.span()); // SerdeBincode<M6id>: u64 length + 32 bytes
        return w.buf;
    }
    static Bytes enc_index_events(const BlockIndexEvents& ev)
    {
        Writer w;
        w.u64(ev.deposits.size());
        for (auto& [op, o] : ev.deposits) {
            bincode(w, op);
            C::enc_filled(w, o);
        }
        w.u64(ev.bundle_spends.size());
        for (auto& [op, m] : ev.bundle_spends) {
            bincode(w, op);
            w.bincode_bytes(m.span());
        }
        return w.buf;
    }

    // ------------------------------------------------------------------ getters
    std::optional<Hash32> tip(const Txn& t) const
    {
        auto v = t.get(T_TIP, unit());
        if (!v) return std::nullopt;
        return Hash32::from_span(*v);
    }
    std::optional<uint32_t> height(const Txn& t) const
    {
        auto v = t.get(T_HEIGHT, unit());
        if (!v) return std::nullopt;
        Reader r(*v);
        return r.u32();
    }
    void set_tip(Txn& t, const Hash32& h, uint32_t height) const
    {
        t.put(T_TIP, unit(), h.span());
        t.put(T_HEIGHT, unit(), u32le(height));
    }
    void clear_tip(Txn& t) const
    {
        t.del(T_TIP, unit());
        t.del(T_HEIGHT, unit());
    }
    std::optional<FilledOutput> utxo(const Txn& t, const OutPoint& op) const
    {
        auto k = op.key();
        auto v = t.get(T_UTXOS, ByteSpan(k.data(), k.size()));
        if (!v) return std::nullopt;
        Reader r(*v);
        return C::dec_filled(r);
    }
    std::optional<SpentOutput> stxo(const Txn& t, const OutPoint& op) const
    {
        auto k = op.key();
        auto v = t.get(T_STXOS, ByteSpan(k.data(), k.size()));
        if (!v) return std::nullopt;
        Reader r(*v);
        return dec_spent(r);
    }
    void put_utxo(Txn& t, const OutPoint& op, const FilledOutput& o) const
    {
        auto k = op.key();
        Writer w;
        C::enc_filled(w, o);
        t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
    }
    bool del_utxo(Txn& t, const OutPoint& op) const
    {
        auto k = op.key();
        return t.del(T_UTXOS, ByteSpan(k.data(), k.size()));
    }
    void put_stxo(Txn& t, const OutPoint& op, const SpentOutput& s) const
    {
        auto k = op.key();
        Writer w;
        enc_spent(w, s);
        t.put(T_STXOS, ByteSpan(k.data(), k.size()), w.buf);
    }
    bool del_stxo(Txn& t, const OutPoint& op) const
    {
        auto k = op.key();
        return t.del(T_STXOS, ByteSpan(k.data(), k.size()));
    }
    std::vector<std::pair<OutPoint, FilledOutput>> utxos(const Txn& t) const
    {
        std::vector<std::pair<OutPoint, FilledOutput>> r;
        t.scan(T_UTXOS, [&](ByteSpan k, ByteSpan v) {
            Reader rd(v);
            r.emplace_back(OutPoint::from_key(k), C::dec_filled(rd));
            return true;
        });
        return r;
    }
    std::vector<std::pair<OutPoint, SpentOutput>> stxos(const Txn& t) const
    {
        std::vector<std::pair<OutPoint, SpentOutput>> r;
        t.scan(T_STXOS, [&](ByteSpan k, ByteSpan v) {
            Reader rd(v);
            r.emplace_back(OutPoint::from_key(k), dec_spent(rd));
            return true;
        });
        return r;
    }
    std::optional<Hash32> pending_bundle_m6id(const Txn& t) const
    {
        auto v = t.get(T_PENDING_BUNDLE, unit());
        if (!v) return std::nullopt;
        return Hash32::from_span(*v);
    }
    static BundleStatus dec_status(Reader& r) { return BundleStatus(r.u8()); }
    static void enc_status(Writer& w, BundleStatus s) { w.u8(uint8_t(s)); }
    std::optional<std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>> bundle(const Txn& t, const Hash32& m6id) const
    {
        auto v = t.get(T_BUNDLES, m6id_key(m6id));
        if (!v) return std::nullopt;
        Reader r(*v);
        auto info = dec_bundle_info(r);
        auto st = dec_rollback<BundleStatus>(r, dec_status);
        return std::make_pair(std::move(info), std::move(st));
    }
    std::vector<std::pair<Hash32, std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>>> bundles(const Txn& t) const
    {
        std::vector<std::pair<Hash32, std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>>> res;
        t.scan(T_BUNDLES, [&](ByteSpan k, ByteSpan v) {
            Reader r(v);
            auto info = dec_bundle_info(r);
            auto st = dec_rollback<BundleStatus>(r, dec_status);
            res.emplace_back(Hash32::from_span(k.subspan(8)), std::make_pair(std::move(info), std::move(st)));
            return true;
        });
        return res;
    }
    void put_bundle(Txn& t, const Hash32& m6id, const WithdrawalBundleInfo& info, const RollBack<BundleStatus>& st) const
    {
        Writer w;
        enc_bundle_info(w, info);
        enc_rollback(w, st, enc_status);
        t.put(T_BUNDLES, m6id_key(m6id), w.buf);
    }
    std::optional<RollBack<Hash32>> latest_failed_rollback(const Txn& t) const
    {
        auto v = t.get(T_LATEST_FAILED, unit());
        if (!v) return std::nullopt;
        Reader r(*v);
        return dec_rollback<Hash32>(r, [](Reader& rd) { return rd.array<32>(); });
    }
    void put_latest_failed(Txn& t, const RollBack<Hash32>& rb) const
    {
        Writer w;
        enc_rollback(w, rb, [](Writer& ww, const Hash32& h) { ww.raw(h); });
        t.put(T_LATEST_FAILED, unit(), w.buf);
    }
    /** mod.rs get_latest_failed_withdrawal_bundle */
    std::optional<std::pair<uint32_t, Hash32>> latest_failed_bundle(const Txn& t) const
    {
        auto rb = latest_failed_rollback(t);
        if (!rb) return std::nullopt;
        Hash32 m6id = rb->latest().value;
        auto b = bundle(t, m6id);
        if (!b) throw StateError("Inconsistent DBs: latest failed m6id missing", true);
        for (auto it = b->second.v.rbegin(); it != b->second.v.rend(); ++it)
            if (it->value == BundleStatus::Failed) return std::make_pair(it->height, m6id);
        throw StateError("missing failure status", true);
    }
    /** mod.rs try_get_pending_withdrawal_bundle: (bundle, height of its latest status). */
    std::optional<std::pair<WithdrawalBundle, uint32_t>> pending_bundle(const Txn& t) const
    {
        auto m = pending_bundle_m6id(t);
        if (!m) return std::nullopt;
        auto b = bundle(t, *m);
        if (!b) throw StateError("pending bundle missing from withdrawal_bundles");
        if (b->first.kind != WithdrawalBundleInfo::Known) throw StateError("PendingWithdrawalBundleUnknown");
        return std::make_pair(b->first.bundle, b->second.latest().height);
    }
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> deposit_blocks(const Txn& t) const { return seq_table(t, T_DEPOSIT_BLOCKS); }
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> bundle_event_blocks(const Txn& t) const { return seq_table(t, T_BUNDLE_EVENT_BLOCKS); }

    BlockIndexEvents block_index_events(const Txn& t, uint32_t h) const
    {
        BlockIndexEvents ev;
        auto v = t.get(T_BLOCK_INDEX_EVENTS, u32le(h));
        if (!v) return ev;
        Reader r(*v);
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            OutPoint op = de_outpoint(r);
            ev.deposits.emplace_back(op, C::dec_filled(r));
        }
        uint64_t m = r.u64();
        for (uint64_t i = 0; i < m; i++) {
            OutPoint op = de_outpoint(r);
            Bytes id = r.bincode_bytes();
            ev.bundle_spends.emplace_back(op, Hash32::from_span(id));
        }
        return ev;
    }

    /** mod.rs sidechain_wealth */
    uint64_t sidechain_wealth(const Txn& t) const
    {
        uint64_t dep_utxo = 0, dep_stxo = 0, wd_stxo = 0;
        for (auto& [op, o] : utxos(t))
            if (op.kind == OutPoint::Deposit) {
                uint64_t v = C::bitcoin_value(o);
                if (dep_utxo + v < dep_utxo) throw StateError("AmountOverflow");
                dep_utxo += v;
            }
        for (auto& [op, s] : stxos(t)) {
            uint64_t v = C::bitcoin_value(s.output);
            if (op.kind == OutPoint::Deposit) {
                if (dep_stxo + v < dep_stxo) throw StateError("AmountOverflow");
                dep_stxo += v;
            }
            if (s.inpoint.kind == InPoint::Withdrawal) {
                if (wd_stxo + v < wd_stxo) throw StateError("AmountOverflow");
                wd_stxo += v;
            }
        }
        uint64_t total = dep_utxo + dep_stxo;
        if (total < dep_utxo || total < wd_stxo) throw StateError("AmountOverflow");
        return total - wd_stxo;
    }

    // ------------------------------------------------------------------ fill (mod.rs)
    /** fill_transaction: the spent outputs, in input order (NoUtxo if one is missing). */
    template <class Tx>
    std::vector<FilledOutput> fill_spent(const Txn& t, const Tx& tx) const
    {
        std::vector<FilledOutput> r;
        r.reserve(tx.inputs.size());
        for (auto& op : tx.inputs) {
            auto u = utxo(t, op);
            if (!u) throw StateError("NoUtxo " + op.to_string());
            r.push_back(std::move(*u));
        }
        return r;
    }
    /** fill_transaction_from_stxos: inputs already spent by this tx (asserts the inpoint). */
    template <class Tx>
    std::vector<FilledOutput> fill_spent_from_stxos(const Txn& t, const Tx& tx) const
    {
        std::vector<FilledOutput> r(tx.inputs.size());
        Hash32 txid = tx.txid();
        for (size_t vin = tx.inputs.size(); vin-- > 0;) {
            auto s = stxo(t, tx.inputs[vin]);
            if (!s) throw StateError("NoStxo " + tx.inputs[vin].to_string());
            if (!(s->inpoint == InPoint{InPoint::Regular, txid, uint32_t(vin)})) throw StateError("fill_transaction_from_stxos: inpoint mismatch", true);
            r[vin] = s->output;
        }
        return r;
    }

    // ------------------------------------------------------------------ withdrawal bundles
    /** WithdrawalBundle::new (types mod.rs). nullopt = BundleTooHeavy. */
    static std::optional<WithdrawalBundle> make_withdrawal_bundle(uint32_t block_height, uint64_t fee, std::map<OutPoint, FilledOutput> spend_utxos,
                                                                  std::vector<btc::TxOut> payouts)
    {
        // inputs commitment: hash(Vec<OutPoint> = sorted keys ++ [Regular{0^32, block_height}])
        Writer w;
        w.borsh_len(spend_utxos.size() + 1);
        for (auto& [op, _] : spend_utxos) borsh(w, op);
        borsh(w, OutPoint{OutPoint::Regular, Hash32{}, block_height});
        Hash32 commitment = blake3(w.buf);
        Writer fw;
        fw.u64be(fee);
        WithdrawalBundle b;
        b.spend_utxos = std::move(spend_utxos);
        b.outputs.push_back({0, btc::op_return_push(fw.buf)});
        b.outputs.push_back({0, btc::op_return_push(commitment.span())});
        for (auto& p : payouts) b.outputs.push_back(std::move(p));
        if (btc::inputless_tx_weight(b.outputs) > 400000) return std::nullopt; // BundleTooHeavy
        return b;
    }

    /** two_way_peg_data.rs collect_withdrawal_bundle */
    std::optional<WithdrawalBundle> collect_withdrawal_bundle(const Txn& t, uint32_t block_height) const
    {
        struct Agg {
            btc::Address addr;
            uint64_t value = 0, main_fee = 0;
            std::map<OutPoint, FilledOutput> utxos;
        };
        std::vector<Agg> aggs; // HashMap keyed by Address (Eq includes network/hrp)
        for (auto& [op, o] : utxos(t)) {
            if (!C::is_withdrawal(o)) continue;
            const btc::Address& ma = C::withdrawal_address(o);
            auto it = std::find_if(aggs.begin(), aggs.end(), [&](const Agg& a) { return a.addr == ma; });
            if (it == aggs.end()) {
                aggs.push_back(Agg{ma, 0, 0, {}});
                it = std::prev(aggs.end());
            }
            uint64_t v = C::withdrawal_value(o), f = C::withdrawal_main_fee(o);
            if (it->value + v < it->value) throw StateError("AmountOverflow");
            it->value += v;
            if (it->main_fee + f < it->main_fee) throw StateError("AmountOverflow");
            it->main_fee += f;
            it->utxos[op] = o;
        }
        if (aggs.empty()) return std::nullopt;
        std::sort(aggs.begin(), aggs.end(), [](const Agg& a, const Agg& b) { // Reverse((main_fee, value, main_address))
            if (a.main_fee != b.main_fee) return a.main_fee > b.main_fee;
            if (a.value != b.value) return a.value > b.value;
            return b.addr < a.addr;
        });
        uint64_t fee = 0;
        std::map<OutPoint, FilledOutput> spend;
        std::vector<btc::TxOut> outs;
        uint32_t txouts_size = 0;
        for (auto& a : aggs) {
            Bytes spk = a.addr.script_pubkey();
            uint64_t n_outputs = outs.size() + 1;
            if (n_outputs > UINT32_MAX) break;
            if (spk.size() > UINT32_MAX) continue;
            uint64_t ts = 8ull + btc::compact_size_len(spk.size()) + spk.size();
            if (ts > UINT32_MAX) continue;
            uint64_t sum = uint64_t(txouts_size) + ts;
            if (sum > UINT32_MAX) break;
            txouts_size = uint32_t(sum);
            if (!predict_weight_ok(uint32_t(n_outputs), txouts_size)) break; // `break`, not skip
            for (auto& [op, o] : a.utxos) spend[op] = o;
            outs.push_back({a.value, spk});
            if (fee + a.main_fee < fee) throw StateError("Amount addition overflow", true); // `+=` panics
            fee += a.main_fee;
        }
        auto b = make_withdrawal_bundle(block_height, fee, std::move(spend), std::move(outs));
        if (!b) throw StateError("BundleTooHeavy");
        return b;
    }

    // ------------------------------------------------------------------ two-way peg (two_way_peg_data.rs)
    void connect_2wpd(Txn& t, const mainchain::TwoWayPegData& d) const
    {
        auto ht = height(t);
        if (!ht) throw StateError("NoTip");
        uint32_t block_height = *ht;
        BlockIndexEvents ev;
        std::optional<Hash32> latest_deposit_block, latest_bundle_event_block;
        for (auto& [event_block, bi] : d.block_info) {
            for (auto& e : bi.events) {
                if (auto dep = std::get_if<mainchain::Deposit>(&e)) {
                    OutPoint op{OutPoint::Deposit, dep->txid, dep->vout};
                    FilledOutput o = C::deposit_output(*dep);
                    put_utxo(t, op, o);
                    ev.deposits.emplace_back(op, o);
                    latest_deposit_block = event_block;
                } else {
                    auto& we = std::get<mainchain::WithdrawalBundleEvent>(e);
                    switch (we.status) {
                    case mainchain::BundleStatus::Submitted: connect_bundle_submitted(t, block_height, ev, we.m6id); break;
                    case mainchain::BundleStatus::Confirmed: connect_bundle_confirmed(t, block_height, we.m6id); break;
                    case mainchain::BundleStatus::Failed: connect_bundle_failed(t, block_height, we.m6id); break;
                    }
                    latest_bundle_event_block = event_block;
                }
            }
        }
        if (!ev.empty()) t.put(T_BLOCK_INDEX_EVENTS, u32le(block_height), enc_index_events(ev));
        if (latest_deposit_block) {
            auto l = seq_last(t, T_DEPOSIT_BLOCKS);
            uint32_t seq = l ? l->first + 1 : 0;
            t.put(T_DEPOSIT_BLOCKS, u32le(seq), enc_block_ref(*latest_deposit_block, block_height));
        }
        if (latest_bundle_event_block) {
            auto l = seq_last(t, T_BUNDLE_EVENT_BLOCKS);
            uint32_t seq = l ? l->first + 1 : 0;
            t.put(T_BUNDLE_EVENT_BLOCKS, u32le(seq), enc_block_ref(*latest_bundle_event_block, block_height));
        }
        auto lf = latest_failed_bundle(t);
        uint32_t last_failure_height = lf ? lf->first : 0;
        if (uint32_t(block_height - last_failure_height) >= WITHDRAWAL_BUNDLE_FAILURE_GAP && !pending_bundle_m6id(t)) {
            if (auto bundle_ = collect_withdrawal_bundle(t, block_height)) {
                Hash32 m6id = bundle_->m6id();
                t.put(T_PENDING_BUNDLE, unit(), m6id.span());
                RollBack<BundleStatus> st;
                if (auto existing = bundle(t, m6id)) {
                    st = existing->second;
                    if (!st.push(BundleStatus::Pending, block_height)) throw StateError("push pending", true);
                } else {
                    st = RollBack<BundleStatus>::make(BundleStatus::Pending, block_height);
                }
                WithdrawalBundleInfo info;
                info.kind = WithdrawalBundleInfo::Known;
                info.bundle = std::move(*bundle_);
                put_bundle(t, m6id, info, st);
            }
        }
    }

    void disconnect_2wpd(Txn& t, const mainchain::TwoWayPegData& d) const
    {
        auto ht = height(t);
        if (!ht) throw StateError("Height should not be None", true);
        uint32_t block_height = *ht;
        std::optional<Hash32> latest_deposit_block, latest_bundle_event_block;
        t.del(T_BLOCK_INDEX_EVENTS, u32le(block_height));
        for (auto bit = d.block_info.rbegin(); bit != d.block_info.rend(); ++bit) {
            auto& [event_block, bi] = *bit;
            for (auto eit = bi.events.rbegin(); eit != bi.events.rend(); ++eit) {
                if (auto dep = std::get_if<mainchain::Deposit>(&*eit)) {
                    OutPoint op{OutPoint::Deposit, dep->txid, dep->vout};
                    if (!del_utxo(t, op)) throw StateError("NoUtxo " + op.to_string());
                    if (!latest_deposit_block) latest_deposit_block = event_block;
                } else {
                    auto& we = std::get<mainchain::WithdrawalBundleEvent>(*eit);
                    switch (we.status) {
                    case mainchain::BundleStatus::Submitted: disconnect_bundle_submitted(t, block_height, we.m6id); break;
                    case mainchain::BundleStatus::Confirmed: disconnect_bundle_confirmed(t, block_height, we.m6id); break;
                    case mainchain::BundleStatus::Failed: disconnect_bundle_failed(t, block_height, we.m6id); break;
                    }
                    if (!latest_bundle_event_block) latest_bundle_event_block = event_block;
                }
            }
        }
        if (latest_bundle_event_block) {
            auto l = seq_last(t, T_BUNDLE_EVENT_BLOCKS);
            if (!l) throw StateError("NoWithdrawalBundleEventBlock");
            if (l->second.first != *latest_bundle_event_block || l->second.second != block_height) throw StateError("assert bundle event block", true);
            if (!t.del(T_BUNDLE_EVENT_BLOCKS, u32le(l->first))) throw StateError("NoWithdrawalBundleEventBlock");
        }
        auto lf = latest_failed_bundle(t);
        uint32_t last_failure_height = lf ? lf->first : 0;
        if (uint32_t(block_height - last_failure_height) >= WITHDRAWAL_BUNDLE_FAILURE_GAP) {
            if (auto p = pending_bundle_m6id(t)) {
                auto b = bundle(t, *p);
                if (!b) throw StateError("PendingWithdrawalBundleUnknown");
                if (b->second.latest().height == block_height) {
                    t.del(T_PENDING_BUNDLE, unit());
                    auto [rest, _] = b->second.pop();
                    if (rest) put_bundle(t, *p, b->first, *rest);
                    else t.del(T_BUNDLES, m6id_key(*p));
                }
            }
        }
        if (latest_deposit_block) {
            auto l = seq_last(t, T_DEPOSIT_BLOCKS);
            if (!l) throw StateError("NoDepositBlock");
            if (l->second.first != *latest_deposit_block || l->second.second != block_height) throw StateError("assert deposit block", true);
            if (!t.del(T_DEPOSIT_BLOCKS, u32le(l->first))) throw StateError("NoDepositBlock");
        }
    }

private:
    static std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> seq_table(const Txn& t, uint8_t table)
    {
        std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> r;
        t.scan(table, [&](ByteSpan k, ByteSpan v) {
            Reader kr(k), vr(v);
            uint32_t seq = kr.u32();
            Bytes hb = vr.bincode_bytes();
            uint32_t h = vr.u32();
            r.emplace_back(seq, std::make_pair(Hash32::from_span(hb), h));
            return true;
        });
        return r;
    }
    static Bytes enc_block_ref(const Hash32& h, uint32_t height)
    {
        Writer w;
        w.bincode_bytes(h.span());
        w.u32(height);
        return w.buf;
    }
    static std::optional<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> seq_last(const Txn& t, uint8_t table)
    {
        auto l = t.last(table); // memcmp-largest key, exactly as LMDB's last()
        if (!l) return std::nullopt;
        Reader kr(l->first), vr(l->second);
        uint32_t seq = kr.u32();
        Bytes hb = vr.bincode_bytes();
        uint32_t h = vr.u32();
        return std::make_pair(seq, std::make_pair(Hash32::from_span(hb), h));
    }

    void connect_bundle_submitted(Txn& t, uint32_t block_height, BlockIndexEvents& ev, const Hash32& m6id) const
    {
        auto pending = pending_bundle_m6id(t);
        if (pending && *pending == m6id) {
            auto b = bundle(t, m6id);
            if (!b || b->first.kind != WithdrawalBundleInfo::Known) throw StateError("PendingWithdrawalBundleUnknown");
            for (auto& [op, o] : b->first.bundle.spend_utxos) {
                if (!del_utxo(t, op)) throw StateError("NoUtxo " + op.to_string());
                put_stxo(t, op, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
                ev.bundle_spends.emplace_back(op, m6id);
            }
            if (b->second.latest().value != BundleStatus::Pending) throw StateError("assert Pending", true);
            if (!b->second.push(BundleStatus::Submitted, block_height)) throw StateError("push submitted", true);
            put_bundle(t, m6id, b->first, b->second);
            t.del(T_PENDING_BUNDLE, unit());
            return;
        }
        if (auto b = bundle(t, m6id)) {
            auto st = b->second.latest().value;
            auto kind = b->first.kind;
            if (st == BundleStatus::Confirmed) throw StateError("ConfirmedResubmitted");
            if (st == BundleStatus::Submitted || st == BundleStatus::SubmittedUnexpected) throw StateError("Resubmitted");
            if (st == BundleStatus::Dropped) {
                if (kind != WithdrawalBundleInfo::Known) throw StateError("UnknownDropped");
            } else if (st == BundleStatus::Pending) {
                if (kind == WithdrawalBundleInfo::Known) throw StateError("DroppedPending");
                throw StateError("UnknownPending");
            } else if (st == BundleStatus::Failed) {
                if (kind == WithdrawalBundleInfo::UnknownConfirmed) throw StateError("UnknownConfirmedFailed");
            }
            if (!b->second.push(BundleStatus::SubmittedUnexpected, block_height)) throw StateError("push submitted unexpected", true);
            put_bundle(t, m6id, b->first, b->second);
            return;
        }
        WithdrawalBundleInfo info;
        info.kind = WithdrawalBundleInfo::Unknown;
        put_bundle(t, m6id, info, RollBack<BundleStatus>::make(BundleStatus::Submitted, block_height));
    }

    void connect_bundle_confirmed(Txn& t, uint32_t block_height, const Hash32& m6id) const
    {
        auto b = bundle(t, m6id);
        if (!b) throw StateError("UnknownWithdrawalBundle");
        auto& [info, st] = *b;
        if (st.latest().value == BundleStatus::Confirmed) return;
        if (st.latest().value != BundleStatus::Submitted && st.latest().value != BundleStatus::SubmittedUnexpected) throw StateError("assert Submitted", true);
        if (info.kind == WithdrawalBundleInfo::UnknownConfirmed) throw StateError("UnknownWithdrawalBundleReconfirmed");
        if (info.kind == WithdrawalBundleInfo::Unknown) {
            if (block_height != 0) throw StateError("UnknownWithdrawalBundleConfirmed"); // halts the chain
            std::map<OutPoint, FilledOutput> all;
            for (auto& [op, o] : utxos(t)) all[op] = o;
            for (auto& [op, o] : all) put_stxo(t, op, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
            t.clear(T_UTXOS);
            info.kind = WithdrawalBundleInfo::UnknownConfirmed;
            info.spend_utxos = std::move(all);
        } else if (info.kind == WithdrawalBundleInfo::Known && st.latest().value == BundleStatus::SubmittedUnexpected) {
            for (auto& [op, o] : info.bundle.spend_utxos) {
                if (!del_utxo(t, op)) throw StateError("UnexpectedWithdrawalBundleInsolvency");
                put_stxo(t, op, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
            }
        }
        if (!st.push(BundleStatus::Confirmed, block_height)) throw StateError("push confirmed", true);
        put_bundle(t, m6id, info, st);
    }

    void connect_bundle_failed(Txn& t, uint32_t block_height, const Hash32& m6id) const
    {
        auto b = bundle(t, m6id);
        if (!b) throw StateError("UnknownWithdrawalBundle");
        auto& [info, st] = *b;
        if (st.latest().value == BundleStatus::Failed) return;
        if (st.latest().value != BundleStatus::Submitted && st.latest().value != BundleStatus::SubmittedUnexpected) throw StateError("assert Submitted", true);
        if (info.kind == WithdrawalBundleInfo::Known && st.latest().value != BundleStatus::SubmittedUnexpected) {
            for (auto& [op, o] : info.bundle.spend_utxos) {
                del_stxo(t, op);
                put_utxo(t, op, o);
            }
            auto lf = latest_failed_rollback(t);
            RollBack<Hash32> nl;
            if (lf) {
                nl = *lf;
                if (!nl.push(m6id, block_height)) throw StateError("push latest failed", true);
            } else {
                nl = RollBack<Hash32>::make(m6id, block_height);
            }
            put_latest_failed(t, nl);
        }
        if (!st.push(BundleStatus::Failed, block_height)) throw StateError("push failed", true);
        put_bundle(t, m6id, info, st);
    }

    void disconnect_bundle_submitted(Txn& t, uint32_t block_height, const Hash32& m6id) const
    {
        auto b = bundle(t, m6id);
        if (!b) {
            auto p = pending_bundle_m6id(t);
            if (p && *p == m6id) return;
            throw StateError("UnknownWithdrawalBundle");
        }
        auto [rest, latest] = b->second.pop();
        if (latest.value != BundleStatus::Submitted && latest.value != BundleStatus::SubmittedUnexpected) throw StateError("assert", true);
        if (latest.height != block_height) throw StateError("assert height", true);
        if (b->first.kind == WithdrawalBundleInfo::Known && rest && rest->latest().value == BundleStatus::Pending) {
            auto& su = b->first.bundle.spend_utxos;
            for (auto it = su.rbegin(); it != su.rend(); ++it) {
                if (!del_stxo(t, it->first)) throw StateError("NoStxo");
                put_utxo(t, it->first, it->second);
            }
            t.put(T_PENDING_BUNDLE, unit(), m6id.span());
        }
        if (rest) put_bundle(t, m6id, b->first, *rest);
        else t.del(T_BUNDLES, m6id_key(m6id));
    }

    void disconnect_bundle_confirmed(Txn& t, uint32_t block_height, const Hash32& m6id) const
    {
        auto b = bundle(t, m6id);
        if (!b) throw StateError("UnknownWithdrawalBundle");
        auto info = b->first;
        auto [prev, latest] = b->second.pop();
        if (latest.value == BundleStatus::Submitted || latest.value == BundleStatus::SubmittedUnexpected) return;
        if (latest.value != BundleStatus::Confirmed) throw StateError("assert Confirmed", true);
        if (latest.height != block_height) throw StateError("assert height", true);
        if (!prev) throw StateError("Pop confirmed bundle status should be valid", true);
        auto pst = prev->latest().value;
        if (pst != BundleStatus::Submitted && pst != BundleStatus::SubmittedUnexpected) throw StateError("assert", true);
        if (info.kind == WithdrawalBundleInfo::Known) {
            if (pst == BundleStatus::SubmittedUnexpected)
                for (auto& [op, o] : info.bundle.spend_utxos) {
                    put_utxo(t, op, o);
                    if (!del_stxo(t, op)) throw StateError("NoStxo");
                }
        } else if (info.kind == WithdrawalBundleInfo::UnknownConfirmed) {
            for (auto& [op, o] : info.spend_utxos) {
                put_utxo(t, op, o);
                if (!del_stxo(t, op)) throw StateError("NoStxo");
            }
            info.kind = WithdrawalBundleInfo::Unknown;
            info.spend_utxos.clear();
        }
        put_bundle(t, m6id, info, *prev);
    }

    void disconnect_bundle_failed(Txn& t, uint32_t block_height, const Hash32& m6id) const
    {
        auto b = bundle(t, m6id);
        if (!b) throw StateError("UnknownWithdrawalBundle");
        auto [prev, latest] = b->second.pop();
        if (latest.value == BundleStatus::Submitted) return;
        if (latest.value != BundleStatus::Failed) throw StateError("assert Failed", true);
        if (latest.height != block_height) throw StateError("assert height", true);
        if (!prev) throw StateError("Pop failed bundle status should be valid", true);
        auto pst = prev->latest().value;
        if (pst != BundleStatus::Submitted && pst != BundleStatus::SubmittedUnexpected) throw StateError("assert", true);
        if (b->first.kind == WithdrawalBundleInfo::Known && pst != BundleStatus::SubmittedUnexpected) {
            auto& su = b->first.bundle.spend_utxos;
            for (auto it = su.rbegin(); it != su.rend(); ++it) {
                put_stxo(t, it->first, SpentOutput{it->second, InPoint{InPoint::Withdrawal, m6id, 0}});
                if (!del_utxo(t, it->first)) throw StateError("NoUtxo");
            }
            auto lf = latest_failed_rollback(t);
            if (!lf) throw StateError("latest failed withdrawal bundle should exist", true);
            auto [prest, platest] = lf->pop();
            if (platest.value != m6id || platest.height != block_height) throw StateError("assert latest failed", true);
            if (prest) put_latest_failed(t, *prest);
            else t.del(T_LATEST_FAILED, unit());
        }
        put_bundle(t, m6id, b->first, *prev);
    }
};

} // namespace bt::sidechain
