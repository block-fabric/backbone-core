// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Shared LayerTwo-family types (bt/sidechain/types.h). Ported from backbone-bitnames
// bitnames/types.cpp (fork paths cited there), made slot-generic.
#include <bt/sidechain/types.h>

#include <bt/bech32.h>
#include <bt/crypto.h>
#include <bt/parse.h>

#include <cctype>
#include <cstring>
#include <vector>

namespace bt::sidechain {

const Address Address::ALL_ZEROS{};

// ---------------------------------------------------------------- addresses and keys
std::string Address::to_base58() const { return base58_encode(b.span()); }

std::optional<Address> Address::from_base58(std::string_view s)
{
    auto d = base58_decode(s);
    if (!d || d->size() != 20) return std::nullopt;
    Address a;
    std::memcpy(a.b.data(), d->data(), 20);
    return a;
}

std::string Address::format_for_deposit(uint8_t slot) const
{
    std::string prefix = "s" + std::to_string(slot) + "_" + to_base58() + "_";
    auto d = sha256(ByteSpan((const uint8_t*)prefix.data(), prefix.size()));
    return prefix + hex_encode(ByteSpan(d.data(), 3));
}

std::optional<Address> Address::from_deposit_address(std::string_view s, uint8_t slot)
{
    const std::string prefix = "s" + std::to_string(slot) + "_";
    if (s.substr(0, prefix.size()) != prefix) return std::nullopt; // MissingDepositPrefix
    std::string_view rest = s.substr(prefix.size());
    auto us = rest.rfind('_');
    if (us == std::string_view::npos) return std::nullopt;
    std::string_view addr = rest.substr(0, us), checksum = rest.substr(us + 1);
    if (checksum.empty()) return std::nullopt; // MissingDepositChecksum
    std::string pre = prefix + std::string(addr) + "_";
    std::string digest = hex_encode(sha256(ByteSpan((const uint8_t*)pre.data(), pre.size())).span());
    // const_hex::encode(digest).starts_with(&checksum.to_lowercase()): only ASCII can lower
    // to a hex digit, so a non-ASCII checksum never matches.
    if (checksum.size() > digest.size()) return std::nullopt;
    for (size_t i = 0; i < checksum.size(); i++) {
        unsigned char c = (unsigned char)checksum[i];
        if (c >= 0x80) return std::nullopt;
        if (char(std::tolower(c)) != digest[i]) return std::nullopt; // WrongDepositChecksum
    }
    return from_base58(addr);
}

Address address_from_vk(const frost::Point& vk)
{
    Blake3 h;
    h.update(vk.span());
    Address a;
    h.finalize_xof(a.b.data(), 20);
    return a;
}

Address deposit_address(ByteSpan raw, uint8_t slot)
{
    std::string s(raw.begin(), raw.end());
    if (Reader::utf8_valid(s)) {
        if (auto a = Address::from_deposit_address(s, slot)) return *a;
        if (auto b = Address::from_base58(s)) return *b;
    }
    return Address::ALL_ZEROS;
}

std::string vk_to_bech32(const char* hrp, const frost::Point& vk) { return *bech32::encode(hrp, vk.span()); }

std::optional<frost::Point> vk_from_bech32(std::string_view s, const char* hrp)
{
    auto d = bech32::decode_bech32m_canonical(s, hrp);
    if (!d || d->size() != 32) return std::nullopt;
    auto p = frost::Point::from_span(*d);
    if (!frost::valid_element(p)) return std::nullopt; // VerifyingKey::deserialize
    return p;
}

std::string epk_to_bech32(const char* hrp, const ByteArray<32>& k) { return *bech32::encode(hrp, k.span()); }

std::optional<ByteArray<32>> epk_from_bech32(std::string_view s, const char* hrp)
{
    auto d = bech32::decode_bech32m_canonical(s, hrp);
    if (!d || d->size() != 32) return std::nullopt;
    return ByteArray<32>::from_span(*d);
}

Hash32 keyed_hash(const Hash32& key, ByteSpan msg)
{
    Blake3 h(key);
    h.update(msg);
    return h.finalize();
}

// ---------------------------------------------------------------- socket addresses
static std::string ipv4_str(const uint8_t* ip)
{
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);
}

std::string SocketV4::to_string() const { return ipv4_str(ip.data()) + ":" + std::to_string(port); }

// Rust Ipv4Addr::from_str: four decimal octets, no leading zeros, <= 255.
static bool parse_ipv4(std::string_view s, uint8_t* out)
{
    int part = 0;
    size_t i = 0;
    while (part < 4) {
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
        if (j == i || j - i > 3) return false;
        if (j - i > 1 && s[i] == '0') return false;
        int v = 0;
        for (size_t k = i; k < j; k++) v = v * 10 + (s[k] - '0');
        if (v > 255) return false;
        out[part++] = uint8_t(v);
        if (part < 4) {
            if (j >= s.size() || s[j] != '.') return false;
            i = j + 1;
        } else {
            i = j;
        }
    }
    return i == s.size();
}

std::optional<SocketV4> SocketV4::parse(std::string_view s)
{
    auto c = s.rfind(':');
    if (c == std::string_view::npos) return std::nullopt;
    SocketV4 r;
    if (!parse_ipv4(s.substr(0, c), r.ip.data())) return std::nullopt;
    auto p = parse_u64(s.substr(c + 1));
    if (!p || *p > 65535) return std::nullopt;
    r.port = uint16_t(*p);
    return r;
}

// Rust's Display for Ipv6Addr: IPv4-mapped as ::ffff:a.b.c.d, otherwise the longest run
// (first if tied) of two or more zero segments collapses to "::", lowercase hex.
static std::string ipv6_str(const std::array<uint8_t, 16>& b)
{
    uint16_t seg[8];
    for (int i = 0; i < 8; i++) seg[i] = uint16_t(b[2 * i] << 8 | b[2 * i + 1]);
    bool mapped = true;
    for (int i = 0; i < 5; i++)
        if (seg[i]) mapped = false;
    if (mapped && seg[5] == 0xffff) return "::ffff:" + ipv4_str(b.data() + 12);
    int best = -1, best_len = 0;
    for (int i = 0; i < 8;) {
        if (seg[i] != 0) {
            i++;
            continue;
        }
        int j = i;
        while (j < 8 && seg[j] == 0) j++;
        if (j - i > best_len) {
            best = i;
            best_len = j - i;
        }
        i = j;
    }
    auto hexs = [](uint16_t v) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%x", v);
        return std::string(buf);
    };
    std::string r;
    if (best_len > 1) {
        for (int i = 0; i < best; i++) r += (i ? ":" : "") + hexs(seg[i]);
        r += "::";
        for (int i = best + best_len; i < 8; i++) r += (i > best + best_len ? ":" : "") + hexs(seg[i]);
    } else {
        for (int i = 0; i < 8; i++) r += (i ? ":" : "") + hexs(seg[i]);
    }
    return r;
}

std::string SocketV6::to_string() const { return "[" + ipv6_str(ip) + "]:" + std::to_string(port); }

static bool parse_ipv6(std::string_view s, std::array<uint8_t, 16>& out)
{
    std::vector<uint16_t> head, tail;
    bool dbl = false;
    auto dc = s.find("::");
    auto parse_groups = [](std::string_view p, std::vector<uint16_t>& g, std::array<uint8_t, 4>* v4, bool& has_v4) -> bool {
        if (p.empty()) return true;
        size_t i = 0;
        while (i <= p.size()) {
            size_t j = p.find(':', i);
            std::string_view part = p.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
            if (j == std::string_view::npos && part.find('.') != std::string_view::npos) {
                if (!parse_ipv4(part, v4->data())) return false;
                has_v4 = true;
                return true;
            }
            if (part.empty() || part.size() > 4) return false;
            uint32_t v = 0;
            for (char c : part) {
                int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d < 0) return false;
                v = v * 16 + uint32_t(d);
            }
            g.push_back(uint16_t(v));
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        return true;
    };
    std::array<uint8_t, 4> v4{};
    bool has_v4 = false, dummy = false;
    if (dc != std::string_view::npos) {
        dbl = true;
        if (!parse_groups(s.substr(0, dc), head, &v4, dummy) || dummy) return false;
        if (!parse_groups(s.substr(dc + 2), tail, &v4, has_v4)) return false;
    } else {
        if (!parse_groups(s, head, &v4, has_v4)) return false;
    }
    size_t n = head.size() + tail.size() + (has_v4 ? 2 : 0);
    if (dbl ? n > 7 : n != 8) return false;
    std::vector<uint16_t> all = head;
    if (dbl) all.insert(all.end(), 8 - n, 0);
    all.insert(all.end(), tail.begin(), tail.end());
    if (has_v4) {
        all.push_back(uint16_t(v4[0] << 8 | v4[1]));
        all.push_back(uint16_t(v4[2] << 8 | v4[3]));
    }
    for (int i = 0; i < 8; i++) {
        out[2 * i] = uint8_t(all[i] >> 8);
        out[2 * i + 1] = uint8_t(all[i]);
    }
    return true;
}

std::optional<SocketV6> SocketV6::parse(std::string_view s)
{
    if (s.empty() || s[0] != '[') return std::nullopt;
    auto e = s.find(']');
    if (e == std::string_view::npos || e + 1 >= s.size() || s[e + 1] != ':') return std::nullopt;
    SocketV6 r;
    std::string_view ip = s.substr(1, e - 1);
    if (ip.find('%') != std::string_view::npos) return std::nullopt; // scope ids are not encoded
    if (!parse_ipv6(ip, r.ip)) return std::nullopt;
    auto p = parse_u64(s.substr(e + 2));
    if (!p || *p > 65535) return std::nullopt;
    r.port = uint16_t(*p);
    return r;
}

void raw_v4(Writer& w, const SocketV4& s)
{
    w.raw(ByteSpan(s.ip.data(), 4));
    w.u16(s.port);
}
void raw_v6(Writer& w, const SocketV6& s)
{
    w.raw(ByteSpan(s.ip.data(), 16));
    w.u16(s.port);
}
SocketV4 de_v4(Reader& r)
{
    SocketV4 s;
    auto b = r.array<4>();
    std::memcpy(s.ip.data(), b.data(), 4);
    s.port = r.u16();
    return s;
}
SocketV6 de_v6(Reader& r)
{
    SocketV6 s;
    auto b = r.array<16>();
    std::memcpy(s.ip.data(), b.data(), 16);
    s.port = r.u16();
    return s;
}

// ---------------------------------------------------------------- outpoints
std::array<uint8_t, 37> OutPoint::key() const
{
    std::array<uint8_t, 37> k{};
    k[0] = kind;
    std::memcpy(k.data() + 1, txid.data(), 32);
    for (int i = 0; i < 4; i++) k[33 + i] = uint8_t(vout >> (8 * i));
    return k;
}

OutPoint OutPoint::from_key(ByteSpan k)
{
    if (k.size() != 37 || k[0] > 2) throw DecodeError("bad OutPointKey");
    OutPoint o;
    o.kind = Kind(k[0]);
    std::memcpy(o.txid.data(), k.data() + 1, 32);
    o.vout = uint32_t(k[33]) | uint32_t(k[34]) << 8 | uint32_t(k[35]) << 16 | uint32_t(k[36]) << 24;
    return o;
}

std::string OutPoint::to_string() const
{
    switch (kind) {
    case Regular: return "regular " + hex_encode(txid) + " " + std::to_string(vout);
    case Coinbase: return "coinbase " + hex_encode(txid) + " " + std::to_string(vout);
    case Deposit: return "deposit " + hex_encode_rev(txid.span()) + " " + std::to_string(vout);
    }
    return {};
}

void borsh(Writer& w, const OutPoint& o)
{
    w.u8(o.kind);
    w.raw(o.txid);
    w.u32(o.vout);
}

void bincode(Writer& w, const OutPoint& o)
{
    w.u32(o.kind);
    if (o.kind == OutPoint::Deposit) w.bincode_bytes(o.txid.span()); // bitcoin::Txid serde = bytes
    else w.raw(o.txid);
    w.u32(o.vout);
}

OutPoint de_outpoint(Reader& r)
{
    OutPoint o;
    uint32_t tag = r.u32();
    if (tag > 2) throw DecodeError("invalid OutPoint tag");
    o.kind = OutPoint::Kind(tag);
    if (o.kind == OutPoint::Deposit) {
        Bytes b = r.bincode_bytes();
        if (b.size() != 32) throw DecodeError("invalid txid length");
        o.txid = Hash32::from_span(b);
    } else {
        o.txid = r.array<32>();
    }
    o.vout = r.u32();
    return o;
}

OutPoint borsh_de_outpoint(Reader& r)
{
    OutPoint o;
    uint8_t tag = r.u8();
    if (tag > 2) throw DecodeError("invalid OutPoint tag");
    o.kind = OutPoint::Kind(tag);
    o.txid = r.array<32>();
    o.vout = r.u32();
    return o;
}

// ---------------------------------------------------------------- shared codec pieces
frost::Signature de_sig64(ByteSpan b)
{
    if (b.size() != 64) throw DecodeError("MalformedSignature");
    frost::Signature s;
    std::memcpy(s.R.data(), b.data(), 32);
    std::memcpy(s.z.data(), b.data() + 32, 32);
    if (!frost::valid_element(s.R) || !frost::canonical_scalar(s.z)) throw DecodeError("invalid signature encoding");
    return s;
}

frost::Point de_vk_raw(Reader& r)
{
    auto p = r.array<32>();
    if (!frost::valid_element(p)) throw DecodeError("invalid verifying key");
    return p;
}

void bincode_withdrawal(Writer& w, uint64_t value, uint64_t main_fee, const btc::Address& a)
{
    w.u64(value);
    w.u64(main_fee);
    w.bincode_str(a.to_string()); // Address<NetworkUnchecked> serde = Display string
}

void de_withdrawal(Reader& r, uint64_t& value, uint64_t& main_fee, btc::Address& a)
{
    value = r.u64();
    main_fee = r.u64();
    std::string s = r.bincode_str();
    auto p = btc::Address::parse(s);
    if (!p) throw DecodeError("invalid main_address");
    a = *p;
}

btc::Address address_from_spk(const Bytes& spk)
{
    btc::Address a;
    if (spk.size() == 25 && spk[0] == 0x76 && spk[1] == 0xa9 && spk[2] == 0x14 && spk[23] == 0x88 && spk[24] == 0xac) {
        a.kind = btc::Address::Kind::P2pkh;
        a.program.assign(spk.begin() + 3, spk.begin() + 23);
    } else if (spk.size() == 23 && spk[0] == 0xa9 && spk[1] == 0x14 && spk[22] == 0x87) {
        a.kind = btc::Address::Kind::P2sh;
        a.program.assign(spk.begin() + 2, spk.begin() + 22);
    } else if (spk.size() >= 4 && (spk[0] == 0 || (spk[0] >= 0x51 && spk[0] <= 0x60)) && spk[1] + 2u == spk.size()) {
        a.kind = btc::Address::Kind::Segwit;
        a.witver = spk[0] == 0 ? 0 : uint8_t(spk[0] - 0x50);
        a.program.assign(spk.begin() + 2, spk.end());
    } else {
        throw DecodeError("withdrawal script is not an address");
    }
    return a;
}

} // namespace bt::sidechain
