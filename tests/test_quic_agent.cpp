// Our name in the QUIC certificate (bt/agent.h, bt/quic.h): the CN we present, how a peer's
// CN becomes its name, and both over a loopback handshake (ports 49311-49330).
#include "test.h"

#include <bt/agent.h>
#include <bt/quic.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

using namespace bt;

TEST(agent_cert_cn_and_peer_name)
{
    CHECK_EQ(agent_cert_cn("Thunder 1.0.0"), std::string("Thunder 1.0.0 (Backbone drop-in)"));
    CHECK_EQ(agent_cert_cn(""), std::string("rcgen self signed cert"));
    CHECK_EQ(agent(), std::string(agent_name()) + " " + agent_version());
    CHECK_EQ(quic::Options().agent, agent());

    CHECK_EQ(peer_name_from_cn(std::string("Thunder 1.0.0 (Backbone drop-in)")), std::string("Thunder 1.0.0"));
    CHECK_EQ(peer_name_from_cn(std::string("BitNames 1.0.0 (Backbone drop-in)")), std::string("BitNames 1.0.0"));
    CHECK_EQ(peer_name_from_cn(agent_cert_cn(agent())), agent());
    CHECK_EQ(peer_name_from_cn(std::string("rcgen self signed cert")), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::nullopt), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::string("")), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::string(" (Backbone drop-in)")), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::string("  (Backbone drop-in)")), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::string("Thunder 1.0.0 (Backbone drop-in) ")), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::string("Evil\x1b[2J (Backbone drop-in)")), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::string("caf\xc3\xa9 (Backbone drop-in)")), std::string("unknown (upstream)"));
    CHECK_EQ(peer_name_from_cn(std::string(64, 'a') + " (Backbone drop-in)"), std::string(64, 'a'));
    CHECK_EQ(peer_name_from_cn(std::string(65, 'a') + " (Backbone drop-in)"), std::string("unknown (upstream)"));

    CHECK(!quic::certificate_cn(Bytes{}));
    CHECK(!quic::certificate_cn(Bytes{0x30, 0x03, 0x02, 0x01, 0x00}));
}

namespace {
struct Ep {
    std::mutex mu;
    std::condition_variable cv;
    std::map<quic::ConnId, std::string> names; // as seen inside on_connected
    std::unique_ptr<quic::Endpoint> ep;
    quic::SockAddr addr;

    bool start(const std::string& agent, uint16_t& port)
    {
        for (; port <= 49330; port++) {
            quic::Callbacks cb;
            cb.on_connected = [this](quic::ConnId c, const quic::SockAddr&, bool) {
                std::lock_guard<std::mutex> l(mu);
                names[c] = ep->peer_name(c);
                cv.notify_all();
            };
            quic::Options o;
            o.agent = agent;
            ep = std::make_unique<quic::Endpoint>(cb, o);
            std::string err;
            if (ep->start({"127.0.0.1", port}, err)) {
                addr = {"127.0.0.1", port++};
                return true;
            }
        }
        return false;
    }
    std::string wait(quic::ConnId c)
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(10), [&] { return names.count(c) > 0; });
        return names.count(c) ? names[c] : std::string("(timeout)");
    }
    std::string wait_inbound()
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(10), [&] { return !names.empty(); });
        return names.empty() ? std::string("(timeout)") : names.begin()->second;
    }
};
} // namespace

TEST(quic_peer_name_over_loopback)
{
    uint16_t port = 49311;
    Ep ours, upstream, client;
    CHECK(ours.start("Test 9.9.9", port));
    CHECK(upstream.start("", port)); // rcgen's CN, as the upstream nodes present
    CHECK(client.start("Client 1.2.3", port));
    if (!ours.ep || !upstream.ep || !client.ep) return;
    CHECK_EQ(quic::certificate_cn(ours.ep->certificate_der()).value_or("(none)"), std::string("Test 9.9.9 (Backbone drop-in)"));
    CHECK_EQ(quic::certificate_cn(upstream.ep->certificate_der()).value_or("(none)"), std::string("rcgen self signed cert"));

    auto c1 = client.ep->connect(ours.addr, "localhost");
    CHECK_EQ(client.wait(c1), std::string("Test 9.9.9"));
    CHECK_EQ(client.ep->peer_name(c1), std::string("Test 9.9.9"));
    CHECK_EQ(ours.wait_inbound(), std::string("unknown")); // a client presents no certificate
    auto c2 = client.ep->connect(upstream.addr, "localhost");
    CHECK_EQ(client.wait(c2), std::string("unknown (upstream)"));
    CHECK_EQ(client.ep->peer_name(12345), std::string());
    client.ep->stop();
    CHECK_EQ(client.ep->peer_name(c1), std::string());
}
