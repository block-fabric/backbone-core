// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Minimal ZeroMQ PUB socket (ZMTP 3.0/3.1, NULL security) over TCP, enough to replace the
// `zeromq` crate publisher the BitNames/BitAssets nodes bind on --zmq-addr: SUB peers
// (libzmq, pyzmq, zeromq-rs) connect, subscribe by topic prefix (legacy 0x01/0x00 message
// or the 3.1 SUBSCRIBE/CANCEL commands), and receive multipart messages whose first frame
// matches. Slow subscribers lose messages beyond a per-peer queue limit (8 MiB, ZMQ_SNDHWM
// semantics) instead of stalling the node. Per-peer caps: inbound frames 64 KiB, 1024
// subscriptions / 1 MiB of topic bytes (duplicates ignored); a peer over a cap is dropped.
// Callers bind loopback by default (the chains' --zmq-addr default is 127.0.0.1).
#pragma once

#include <bt/bytes.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bt::zmq {

class Publisher {
public:
    Publisher();
    ~Publisher();
    /** Bind "ip", port and start the I/O thread. */
    bool bind(const std::string& ip, uint16_t port, std::string& err);
    void stop();
    /** Queue a multipart message for every subscriber whose subscription prefixes frame 0. */
    void publish(const std::vector<Bytes>& frames);
    size_t subscribers() const;
    uint16_t port() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace bt::zmq
