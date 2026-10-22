// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: Bitcoin Core's CSHA256 (crypto/sha256.h), an incremental SHA-256, on OpenSSL-free code of our own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class CSHA256
{
public:
    static const size_t OUTPUT_SIZE = 32;
    CSHA256();
    CSHA256& Write(const unsigned char* data, size_t len);
    void Finalize(unsigned char hash[OUTPUT_SIZE]);
    CSHA256& Reset();

private:
    uint32_t s[8];
    unsigned char buf[64];
    uint64_t bytes{0};
};
