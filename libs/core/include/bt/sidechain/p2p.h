// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// P2P messages of the LayerTwo-family chains (lib/net/peer/message.rs and peer/mod.rs,
// identical in thunder-rust v0.18.2 and the plain-bitnames fork apart from the block body
// type and the magic). Every message is magic(4) || bincode(msg) on its own QUIC stream.
//   RequestMessage = Heartbeat(PeerState) | GetBlock{block_hash} | GetHeaders{start, end}
//                  | PushTransaction{transaction}            (u32 tags 0..3)
//   ResponseMessage = Block{header, body} | Headers(Vec<Header>) | NoBlock{block_hash}
//                   | NoHeader{block_hash} | TransactionAccepted(txid)
//                   | TransactionRejected(txid)              (u32 tags 0..5)
// Traits C: Header, Body, AuthorizedTransaction and their bincode encode_/decode_ functions
// (encode_header, decode_header, encode_body, decode_body, encode_authorized_tx,
// decode_authorized_tx), plus `static constexpr size_t MIN_HEADER_BYTES`.
#pragma once

#include <bt/codec.h>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace bt::sidechain {

/** Baudot prefix with the network id in the low bits of byte 4 (message.rs magic_bytes). */
enum class Network : uint8_t { Betanet, Forknet, Regtest, Signet };
inline std::array<uint8_t, 4> magic_bytes(std::array<uint8_t, 4> prefix, Network n)
{
    switch (n) {
    case Network::Regtest: break;
    case Network::Signet: prefix[3] |= 1; break;
    case Network::Forknet: prefix[3] |= 2; break;
    case Network::Betanet: prefix[3] |= 4; break;
    }
    return prefix;
}

struct Version {
    uint64_t major = 0, minor = 18, patch = 2;
};

struct Tip {
    Hash32 block_hash;
    Hash32 main_block_hash; // internal order
    bool operator==(const Tip&) const = default;
};

struct TipInfo {
    uint32_t block_height = 0;
    Tip tip;
    Hash32 total_work_be; // bitcoin::Work, serde = 32 big-endian bytes
};

struct PeerState {
    std::optional<TipInfo> tip_info;
    Version version;
};

void encode_peer_state(Writer& w, const PeerState& p);
PeerState decode_peer_state(Reader& r);

/** Response read limits (message.rs). */
constexpr size_t READ_REQUEST_LIMIT = 100 * 1024;
constexpr size_t BLOCK_RESPONSE_LIMIT = 10 * 1024 * 1024;
constexpr size_t TX_ACK_LIMIT = 256;
/** 2 KiB per expected header; ours is capped at 64 MiB as a receive-side DoS bound. */
inline size_t headers_response_limit(uint32_t height)
{
    uint64_t l = (uint64_t(height) + 1) * 2048;
    return size_t(std::min<uint64_t>(l, 64ull * 1024 * 1024));
}

template <class C>
struct P2P {
    using Header = typename C::Header;
    using Body = typename C::Body;
    using AuthorizedTransaction = typename C::AuthorizedTransaction;

    struct Request {
        enum Kind : uint32_t { Heartbeat = 0, GetBlock = 1, GetHeaders = 2, PushTransaction = 3 } kind = Heartbeat;
        PeerState heartbeat;
        Hash32 block_hash;         // GetBlock
        std::vector<Hash32> start; // GetHeaders (HashSet: any order on the wire)
        Hash32 end;                // GetHeaders
        AuthorizedTransaction tx;  // PushTransaction
    };

    struct Response {
        enum Kind : uint32_t { Block = 0, Headers = 1, NoBlock = 2, NoHeader = 3, TransactionAccepted = 4, TransactionRejected = 5 } kind = NoBlock;
        Header header; // Block
        Body body;     // Block
        std::vector<Header> headers;
        Hash32 hash; // NoBlock / NoHeader block hash; tx accepted/rejected txid
    };

    static Bytes encode(const Request& q)
    {
        Writer w;
        w.u32(q.kind);
        switch (q.kind) {
        case Request::Heartbeat: encode_peer_state(w, q.heartbeat); break;
        case Request::GetBlock: w.raw(q.block_hash); break;
        case Request::GetHeaders:
            w.bincode_len(q.start.size());
            for (auto& h : q.start) w.raw(h);
            w.raw(q.end);
            break;
        case Request::PushTransaction: C::encode_authorized_tx(w, q.tx); break;
        }
        return w.buf;
    }

    /** bincode::deserialize (legacy config) ignores trailing bytes. */
    static Request decode_request(ByteSpan b)
    {
        Reader r(b);
        Request q;
        uint32_t tag = r.u32();
        switch (tag) {
        case Request::Heartbeat:
            q.kind = Request::Heartbeat;
            q.heartbeat = decode_peer_state(r);
            break;
        case Request::GetBlock:
            q.kind = Request::GetBlock;
            q.block_hash = r.array<32>();
            break;
        case Request::GetHeaders: {
            q.kind = Request::GetHeaders;
            size_t n = r.bincode_len(32);
            for (size_t i = 0; i < n; i++) q.start.push_back(r.array<32>());
            q.end = r.array<32>();
            break;
        }
        case Request::PushTransaction:
            q.kind = Request::PushTransaction;
            q.tx = C::decode_authorized_tx(r);
            break;
        default: throw DecodeError("unknown request tag");
        }
        return q;
    }

    static Bytes encode(const Response& s)
    {
        Writer w;
        w.u32(s.kind);
        switch (s.kind) {
        case Response::Block:
            C::encode_header(w, s.header);
            C::encode_body(w, s.body);
            break;
        case Response::Headers:
            w.bincode_len(s.headers.size());
            for (auto& h : s.headers) C::encode_header(w, h);
            break;
        case Response::NoBlock:
        case Response::NoHeader:
        case Response::TransactionAccepted:
        case Response::TransactionRejected: w.raw(s.hash); break;
        }
        return w.buf;
    }

    static Response decode_response(ByteSpan b)
    {
        Reader r(b);
        Response s;
        uint32_t tag = r.u32();
        switch (tag) {
        case Response::Block:
            s.kind = Response::Block;
            s.header = C::decode_header(r);
            s.body = C::decode_body(r);
            break;
        case Response::Headers: {
            s.kind = Response::Headers;
            size_t n = r.bincode_len(C::MIN_HEADER_BYTES);
            for (size_t i = 0; i < n; i++) s.headers.push_back(C::decode_header(r));
            break;
        }
        case Response::NoBlock:
        case Response::NoHeader:
        case Response::TransactionAccepted:
        case Response::TransactionRejected:
            s.kind = typename Response::Kind(tag);
            s.hash = r.array<32>();
            break;
        default: throw DecodeError("unknown response tag");
        }
        return s;
    }
};

} // namespace bt::sidechain
