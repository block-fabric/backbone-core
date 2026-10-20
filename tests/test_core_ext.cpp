// Known-answer tests for the modules added for BitNames/BitAssets: HMAC-SHA256, HKDF,
// Bech32m, BIP32-Ed25519 (ed25519-bip32 0.4.1 vectors), the ZMQ publisher.
#include "test.h"

#include <bt/bech32.h>
#include <bt/crypto.h>
#include <bt/ed25519_bip32.h>
#include <bt/frost.h>
#include <bt/zmq.h>

#include <sodium.h>

#include <chrono>
#include <cstring>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace bt;

static Bytes hx(const char* s) { return *hex_decode(s); }

TEST(hmac_sha256_rfc4231)
{
    Bytes key(20, 0x0b);
    std::string data = "Hi There";
    auto h = hmac_sha256(key, {ByteSpan((const uint8_t*)data.data(), data.size())});
    CHECK_EQ(hex_encode(h), std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    // case 6: key longer than the block size
    Bytes k6(131, 0xaa);
    std::string d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK_EQ(hex_encode(hmac_sha256(k6, {ByteSpan((const uint8_t*)d6.data(), d6.size())})),
             std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST(hkdf_sha256_rfc5869)
{
    auto okm = hkdf_sha256(hx("000102030405060708090a0b0c"), Bytes(22, 0x0b), hx("f0f1f2f3f4f5f6f7f8f9"), 42);
    CHECK_EQ(hex_encode(okm), std::string("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
    // test 3: no salt, no info (salt None = HashLen zeros, as libes uses it)
    auto okm3 = hkdf_sha256({}, Bytes(22, 0x0b), {}, 42);
    CHECK_EQ(hex_encode(okm3), std::string("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"));
}

TEST(bech32m_vectors)
{
    // BIP-350 valid Bech32m strings (empty data / mixed hrp)
    CHECK(bech32::decode("a1lqfn3a").has_value());
    CHECK(bech32::decode("A1LQFN3A").has_value());
    CHECK(bech32::decode("abcdef1l7aum6echk45nj3s0wdvt2fg8x9yrzpqzd3ryx").has_value());
    CHECK(!bech32::decode("a1lqfn3A").has_value()); // mixed case
    // Bech32 (BIP-173) still decodes, as the crate's decode does, but is not canonical Bech32m
    auto b = bech32::decode("a12uel5l");
    REQUIRE(b.has_value());
    CHECK(b->variant == bech32::Variant::Bech32);
    CHECK(!bech32::decode_bech32m_canonical("a12uel5l", "a").has_value());
    // round trip of a 32-byte key with BitNames' HRPs
    Bytes key(32);
    for (int i = 0; i < 32; i++) key[i] = uint8_t(i * 7);
    for (const char* hrp : {"bn-enc", "bn-svk"}) {
        auto s = bech32::encode(hrp, key);
        REQUIRE(s.has_value());
        CHECK(s->rfind(std::string(hrp) + "1", 0) == 0);
        auto back = bech32::decode_bech32m_canonical(*s, hrp);
        REQUIRE(back.has_value());
        CHECK(*back == key);
        std::string upper = *s;
        for (auto& c : upper) c = char(std::toupper((unsigned char)c));
        CHECK(!bech32::decode_bech32m_canonical(upper, hrp).has_value()); // WrongVariant in theirs
    }
}

TEST(ed25519_bip32_crate_vectors)
{
    // ed25519-bip32 0.4.1 src/tests.rs: D1 --(hardened 0x80000000)--> D1_H0
    auto d1 = hx("f8a29231ee38d6c5bf715d5bac21c750577aa3798b22d79d65bf97d6fadea15adcd1ee1abdf78bd4be64731a12deb94d3671784112eb6f364b871851fd1c9a247384db9ad6003bbd08b3b1ddc0d07a597293ff85e961bf252b331262eddfad0d");
    auto d1h0 = hx("60d399da83ef80d8d4f8d223239efdc2b8fef387e1b5219137ffb4e8fbdea15adc9366b7d003af37c11396de9a83734e30e05e851efa32745c9cd7b42712c890608763770eddf77248ab652984b21b849760d1da74a6f5bd633ce41adceef07a");
    ed25519_bip32::XPrv x;
    std::memcpy(x.b.data(), d1.data(), 96);
    auto c = x.derive(0x80000000u);
    CHECK(Bytes(c.b.begin(), c.b.end()) == d1h0);
    // The public key of D1_H0 verifies the crate's signature of "Hello World" (checks kL*B).
    auto sig = hx("90194d57cde4fdadd01eb7cf161780c277e129fc7135b97779a3268837e4cd2e9444b9bb91c0e84d23bba870df3c4bda91a110ef735638fa7a34ea2046d4be04");
    auto pk = c.public_key();
    const char* msg = "Hello World";
    CHECK(crypto_sign_verify_detached(sig.data(), (const uint8_t*)msg, std::strlen(msg), pk.data()) == 0);
    // Soft derivation agrees between the private and the public path.
    for (uint32_t i : {0u, 1u, 7u, 0x7fffffffu}) {
        auto child = c.derive(i);
        auto pub = ed25519_bip32::derive_xpub(c.xpub(), i);
        REQUIRE(pub.has_value());
        CHECK(*pub == child.xpub());
    }
    CHECK(!ed25519_bip32::derive_xpub(c.xpub(), 0x80000000u).has_value());
}

#ifndef _WIN32
// A raw ZMTP 3.0 SUB client: greeting, READY(SUB), legacy subscription message.
static int zmq_sub_connect(uint16_t port, const std::string& topic)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) return -1;
    Bytes g(64, 0);
    g[0] = 0xff;
    g[9] = 0x7f;
    g[10] = 3;
    g[11] = 0;
    std::memcpy(g.data() + 12, "NULL", 4);
    Bytes ready{0x04, 0};
    Bytes body{5, 'R', 'E', 'A', 'D', 'Y', 11};
    for (char ch : std::string("Socket-Type")) body.push_back(uint8_t(ch));
    body.insert(body.end(), {0, 0, 0, 3, 'S', 'U', 'B'});
    ready[1] = uint8_t(body.size());
    append(ready, body);
    Bytes sub{0x00, uint8_t(topic.size() + 1), 1};
    for (char ch : topic) sub.push_back(uint8_t(ch));
    Bytes all = g;
    append(all, ready);
    append(all, sub);
    send(s, all.data(), all.size(), 0);
    return s;
}

static Bytes read_some(int s, int ms)
{
    Bytes r;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        pollfd p{s, POLLIN, 0};
        if (poll(&p, 1, 50) > 0) {
            uint8_t buf[4096];
            auto n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            r.insert(r.end(), buf, buf + n);
        }
    }
    return r;
}

TEST(zmq_publisher_topic_prefix)
{
    zmq::Publisher pub;
    std::string err;
    REQUIRE(pub.bind("127.0.0.1", 0, err));
    int a = zmq_sub_connect(pub.port(), "hashblock"), b = zmq_sub_connect(pub.port(), "other");
    REQUIRE(a >= 0 && b >= 0);
    for (int i = 0; i < 50 && pub.subscribers() < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(pub.subscribers(), size_t(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // subscriptions processed
    Bytes hash(32, 0xab), height{7, 0, 0, 0};
    pub.publish({Bytes{'h', 'a', 's', 'h', 'b', 'l', 'o', 'c', 'k'}, hash, height});
    Bytes ra = read_some(a, 400), rb = read_some(b, 200);
    // after our 64-byte greeting and READY: three frames, MORE on the first two
    REQUIRE(ra.size() > 64);
    Bytes want{0x01, 9, 'h', 'a', 's', 'h', 'b', 'l', 'o', 'c', 'k', 0x01, 32};
    append(want, hash);
    want.insert(want.end(), {0x00, 4, 7, 0, 0, 0});
    CHECK(std::search(ra.begin(), ra.end(), want.begin(), want.end()) != ra.end());
    CHECK(std::search(rb.begin(), rb.end(), want.begin(), want.end()) == rb.end());
    close(a);
    close(b);
    pub.stop();
}
#endif
