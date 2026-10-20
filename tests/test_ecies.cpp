// libes-shaped ECIES (x25519 + HKDF-SHA256 + AES-256-GCM + HMAC-SHA256): layout,
// round trip and tamper detection. Byte compatibility with libes itself is a differential
// test (doc/differential-tests.md).
#include "test.h"

#include <bt/crypto.h>
#include <bt/ecies.h>

#include <cstring>

using namespace bt;

TEST(ecies_layout_and_roundtrip)
{
    ByteArray<32> sk, eph;
    for (int i = 0; i < 32; i++) {
        sk[i] = uint8_t(i + 1);
        eph[i] = uint8_t(200 - i);
    }
    ByteArray<12> nonce;
    for (int i = 0; i < 12; i++) nonce[i] = uint8_t(i);
    auto pk = ecies::x25519_public(sk);
    std::string msg = "hello bitnames";
    Bytes ct = ecies::encrypt_with(pk, ByteSpan((const uint8_t*)msg.data(), msg.size()), eph, nonce);
    REQUIRE(ct.size() == 32 + 12 + 32 + msg.size() + 16);
    CHECK(ByteArray<32>::from_span(ByteSpan(ct.data(), 32)) == ecies::x25519_public(eph));
    CHECK(std::memcmp(ct.data() + 32, nonce.data(), 12) == 0);
    auto pt = ecies::decrypt(sk, ct);
    REQUIRE(pt.has_value());
    CHECK(std::string(pt->begin(), pt->end()) == msg);
    // random path
    auto ct2 = ecies::encrypt(pk, ByteSpan((const uint8_t*)msg.data(), msg.size()));
    CHECK(ecies::decrypt(sk, ct2).has_value());
    // a flipped ciphertext byte fails the MAC; a short input is BadData
    ct[80] ^= 1;
    CHECK(!ecies::decrypt(sk, ct).has_value());
    CHECK(!ecies::decrypt(sk, ByteSpan(ct.data(), 75)).has_value());
}
