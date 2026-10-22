// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Types the LayerTwo-family chains share byte for byte (thunder-rust v0.18.2, the
// plain-bitnames fork v0.18.2, the plain-bitassets fork v0.17.1): the 20-byte BLAKE3
// address with its `s<slot>_<b58>_<hex>` deposit form, OutPoint / InPoint, socket
// addresses as borsh and serde-bincode write them, the Update<T> enum, Bech32m key strings,
// the withdrawal content's encodings, and the consensus error type. Moved here from
// backbone-bitnames (bitnames/types.{h,cpp}) so BitAssets and later chains share one copy.
#pragma once

#include <bt/bitcoin.h>
#include <bt/bytes.h>
#include <bt/codec.h>
#include <bt/frost.h>
#include <bt/mainchain.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bt::sidechain {

/** types/address.rs: 20 raw bytes, text = plain base58, no version, no checksum. */
struct Address {
    Hash20 b;
    static const Address ALL_ZEROS;
    std::string to_base58() const;
    static std::optional<Address> from_base58(std::string_view s);
    /** `s<slot>_<b58>_<hex(sha256(prefix))[..6]>` (address.rs format_for_deposit). */
    std::string format_for_deposit(uint8_t slot) const;
    /** address.rs from_deposit_address: "s<slot>_" prefix, rsplit at the last '_', any
     *  non-empty case-insensitive hex prefix of sha256("s<slot>_<addr>_") as checksum. */
    static std::optional<Address> from_deposit_address(std::string_view s, uint8_t slot);
    friend auto operator<=>(const Address&, const Address&) = default;
    friend bool operator==(const Address&, const Address&) = default;
};

/** authorization.rs get_address: first 20 bytes of BLAKE3-XOF(compressed VK). */
Address address_from_vk(const frost::Point& vk);

/** proto.rs deposit address: the `s<slot>_` form, else bare base58 of 20 bytes, else (also
 *  for non-UTF-8 bytes) Address::ALL_ZEROS. */
Address deposit_address(ByteSpan raw, uint8_t slot);

/** Bech32m key strings (keys.rs): VerifyingKey under `hrp`; nullopt also for invalid points. */
std::string vk_to_bech32(const char* hrp, const frost::Point& vk);
std::optional<frost::Point> vk_from_bech32(std::string_view s, const char* hrp);
/** EncryptionPubKey (x25519) under `hrp`. */
std::string epk_to_bech32(const char* hrp, const ByteArray<32>& k);
std::optional<ByteArray<32>> epk_from_bech32(std::string_view s, const char* hrp);

/** std::net::SocketAddrV4 / V6 as borsh and serde-bincode write them: address octets then
 *  the port as u16 LE (V6 flowinfo and scope id are not encoded). */
struct SocketV4 {
    std::array<uint8_t, 4> ip{};
    uint16_t port = 0;
    bool operator==(const SocketV4&) const = default;
    std::string to_string() const;
    static std::optional<SocketV4> parse(std::string_view s);
};
struct SocketV6 {
    std::array<uint8_t, 16> ip{};
    uint16_t port = 0;
    bool operator==(const SocketV6&) const = default;
    std::string to_string() const;
    static std::optional<SocketV6> parse(std::string_view s);
};
void raw_v4(Writer& w, const SocketV4& s);
void raw_v6(Writer& w, const SocketV6& s);
SocketV4 de_v4(Reader& r);
SocketV6 de_v6(Reader& r);

/** Update<T> = Delete | Retain | Set(T) (bitname_data.rs / bitasset_data.rs). */
template <class T>
struct Update {
    enum Kind : uint8_t { Delete = 0, Retain = 1, Set = 2 } kind = Retain;
    T value{};
};

/** transaction/mod.rs OutPoint. Ord = variant index, then (txid bytes, vout). */
struct OutPoint {
    enum Kind : uint8_t { Regular = 0, Coinbase = 1, Deposit = 2 };
    Kind kind = Regular;
    Hash32 txid; // Txid / CoinbaseTxid / bitcoin Txid (internal byte order)
    uint32_t vout = 0;
    friend auto operator<=>(const OutPoint&, const OutPoint&) = default;
    friend bool operator==(const OutPoint&, const OutPoint&) = default;
    /** 37-byte borsh form (OutPointKey), also the key of utxos/stxos. */
    std::array<uint8_t, 37> key() const;
    static OutPoint from_key(ByteSpan k);
    std::string to_string() const;
};
void borsh(Writer& w, const OutPoint& o);
void bincode(Writer& w, const OutPoint& o);
OutPoint de_outpoint(Reader& r);        // bincode
OutPoint borsh_de_outpoint(Reader& r);  // borsh

struct InPoint {
    enum Kind : uint8_t { Regular = 0, Withdrawal = 1 };
    Kind kind = Regular;
    Hash32 id; // txid, or m6id (internal byte order)
    uint32_t vin = 0;
    bool operator==(const InPoint&) const = default;
};

/** A consensus failure. `panic` marks paths where their node panics instead of erroring;
 *  the network cannot connect such a block either, so we reject it too. */
struct StateError : std::runtime_error {
    bool panic = false;
    explicit StateError(const std::string& m, bool p = false) : std::runtime_error(m), panic(p) {}
};

// ---------------------------------------------------------------- shared codec pieces
/** frost Signature from 64 bytes: canonical R point and canonical z scalar, else DecodeError. */
frost::Signature de_sig64(ByteSpan b);
/** frost VerifyingKey from 32 raw bytes (valid, non-identity point), else DecodeError. */
frost::Point de_vk_raw(Reader& r);
/** Withdrawal content (value, main_fee, main_address) in serde-bincode: u64, u64, Display string. */
void bincode_withdrawal(Writer& w, uint64_t value, uint64_t main_fee, const btc::Address& a);
void de_withdrawal(Reader& r, uint64_t& value, uint64_t& main_fee, btc::Address& a);
/** The borsh form carries only a withdrawal's scriptPubKey; recover the mainnet-form address
 *  of the same script (rust-bitcoin's from_script). */
btc::Address address_from_spk(const Bytes& spk);

template <class T, class F>
void borsh_opt(Writer& w, const std::optional<T>& v, F f)
{
    if (v) {
        w.u8(1);
        f(*v);
    } else {
        w.u8(0);
    }
}
template <class T, class F>
std::optional<T> de_opt(Reader& r, F f)
{
    uint8_t tag = r.u8();
    if (tag == 0) return std::nullopt;
    if (tag != 1) throw DecodeError("invalid Option tag");
    return f();
}

/** blake3::keyed_hash(key, msg). */
Hash32 keyed_hash(const Hash32& key, ByteSpan msg);

} // namespace bt::sidechain
