// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/sidechain/mainchain_store.h>

namespace bt::sidechain {

U256 U256::from_le(const Hash32& b)
{
    U256 r;
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int j = 7; j >= 0; j--) v = v << 8 | b[8 * i + j];
        r.w[i] = v;
    }
    return r;
}
Hash32 U256::to_le() const
{
    Hash32 b;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) b[8 * i + j] = uint8_t(w[i] >> (8 * j));
    return b;
}
Hash32 U256::to_be() const
{
    Hash32 le = to_le(), be;
    for (int i = 0; i < 32; i++) be[i] = le[31 - i];
    return be;
}
U256 U256::operator+(const U256& o) const
{
    U256 r;
    unsigned __int128 carry = 0;
    for (int i = 0; i < 4; i++) {
        unsigned __int128 s = (unsigned __int128)w[i] + o.w[i] + carry;
        r.w[i] = uint64_t(s);
        carry = s >> 64;
    }
    return r; // bitcoin::Work `+` on U256 (overflow unreachable in practice)
}

void enc_block_info(Writer& w, const mainchain::BlockInfo& bi)
{
    w.u8(bi.bmm_commitment ? 1 : 0);
    if (bi.bmm_commitment) w.raw(*bi.bmm_commitment);
    w.u32(uint32_t(bi.events.size()));
    for (auto& e : bi.events) {
        if (auto d = std::get_if<mainchain::Deposit>(&e)) {
            w.u8(0);
            w.u64(d->sequence_number);
            w.raw(d->txid);
            w.u32(d->vout);
            w.bincode_bytes(d->address);
            w.u64(d->value_sats);
        } else {
            auto& b = std::get<mainchain::WithdrawalBundleEvent>(e);
            w.u8(1);
            w.raw(b.m6id);
            w.u8(uint8_t(b.status));
            w.u8(b.sequence_number ? 1 : 0);
            if (b.sequence_number) w.u64(*b.sequence_number);
            w.bincode_bytes(b.transaction);
        }
    }
}

mainchain::BlockInfo dec_block_info(Reader& r)
{
    mainchain::BlockInfo bi;
    if (r.u8()) bi.bmm_commitment = r.array<32>();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n; i++) {
        if (r.u8() == 0) {
            mainchain::Deposit d;
            d.sequence_number = r.u64();
            d.txid = r.array<32>();
            d.vout = r.u32();
            d.address = r.bincode_bytes();
            d.value_sats = r.u64();
            bi.events.emplace_back(d);
        } else {
            mainchain::WithdrawalBundleEvent b;
            b.m6id = r.array<32>();
            b.status = mainchain::BundleStatus(r.u8());
            if (r.u8()) b.sequence_number = r.u64();
            b.transaction = r.bincode_bytes();
            bi.events.emplace_back(b);
        }
    }
    return bi;
}

static uint32_t invert_lowest_one(uint32_t n) { return n & (n - 1); }
uint32_t skip_height(uint32_t h)
{
    if (h < 2) return 0;
    return (h & 1) ? invert_lowest_one(invert_lowest_one(h - 1)) + 1 : invert_lowest_one(h);
}

} // namespace bt::sidechain

#include <bt/sidechain/p2p.h>

namespace bt::sidechain {

void encode_peer_state(Writer& w, const PeerState& p)
{
    if (p.tip_info) {
        w.u8(1);
        w.u32(p.tip_info->block_height);
        w.raw(p.tip_info->tip.block_hash);
        w.bincode_bytes(p.tip_info->tip.main_block_hash.span());
        w.bincode_bytes(p.tip_info->total_work_be.span());
    } else {
        w.u8(0);
    }
    w.u64(p.version.major);
    w.u64(p.version.minor);
    w.u64(p.version.patch);
}

PeerState decode_peer_state(Reader& r)
{
    PeerState p;
    uint8_t opt = r.u8();
    if (opt == 1) {
        TipInfo t;
        t.block_height = r.u32();
        t.tip.block_hash = r.array<32>();
        Bytes mb = r.bincode_bytes();
        if (mb.size() != 32) throw DecodeError("bad main block hash");
        t.tip.main_block_hash = Hash32::from_span(mb);
        Bytes wb = r.bincode_bytes();
        if (wb.size() != 32) throw DecodeError("bad work");
        t.total_work_be = Hash32::from_span(wb);
        p.tip_info = t;
    } else if (opt != 0) {
        throw DecodeError("bad Option tag");
    }
    p.version.major = r.u64();
    p.version.minor = r.u64();
    p.version.patch = r.u64();
    return p;
}

} // namespace bt::sidechain
