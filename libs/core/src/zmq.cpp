// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/zmq.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void sclose(sock_t s) { closesocket(s); }
static int spoll(WSAPOLLFD* f, unsigned n, int t) { return WSAPoll(f, n, t); }
using pollfd_t = WSAPOLLFD;
static const sock_t BAD = INVALID_SOCKET;
static void set_nonblock(sock_t s)
{
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
}
static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using sock_t = int;
static void sclose(sock_t s) { ::close(s); }
static int spoll(pollfd* f, unsigned n, int t) { return ::poll(f, n, t); }
using pollfd_t = pollfd;
static const sock_t BAD = -1;
static void set_nonblock(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
static bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

namespace bt::zmq {

namespace {
constexpr size_t MAX_QUEUE_BYTES = 8 * 1024 * 1024; // per subscriber
constexpr size_t MAX_INBOUND_FRAME = 64 * 1024;     // subscriptions and commands only

Bytes greeting()
{
    Bytes g(64, 0);
    g[0] = 0xff;
    g[9] = 0x7f;
    g[10] = 3; // version 3.1
    g[11] = 1;
    std::memcpy(g.data() + 12, "NULL", 4);
    g[32] = 0; // as-server: unused for NULL
    return g;
}

void put_frame(Bytes& out, uint8_t flags, ByteSpan body)
{
    if (body.size() > 255) {
        out.push_back(uint8_t(flags | 0x02));
        for (int i = 7; i >= 0; i--) out.push_back(uint8_t(uint64_t(body.size()) >> (8 * i)));
    } else {
        out.push_back(flags);
        out.push_back(uint8_t(body.size()));
    }
    append(out, body);
}

Bytes ready_command()
{
    Bytes body;
    body.push_back(5);
    append(body, ByteSpan((const uint8_t*)"READY", 5));
    const char* name = "Socket-Type";
    body.push_back(uint8_t(std::strlen(name)));
    append(body, ByteSpan((const uint8_t*)name, std::strlen(name)));
    const char* val = "PUB";
    for (int i = 3; i >= 0; i--) body.push_back(uint8_t(uint32_t(std::strlen(val)) >> (8 * i)));
    append(body, ByteSpan((const uint8_t*)val, std::strlen(val)));
    Bytes out;
    put_frame(out, 0x04, body);
    return out;
}

struct Peer {
    sock_t s = BAD;
    Bytes in;
    std::deque<Bytes> out;
    size_t out_bytes = 0, out_off = 0;
    bool greeted = false, ready = false;
    std::vector<Bytes> subs;
    bool matches(const Bytes& topic) const
    {
        for (auto& p : subs)
            if (p.size() <= topic.size() && std::equal(p.begin(), p.end(), topic.begin())) return true;
        return false;
    }
};
} // namespace

struct Publisher::Impl {
    sock_t ls = BAD;
    uint16_t port = 0;
    std::atomic<bool> running{false};
    std::thread thr;
    mutable std::mutex mu;
    std::map<sock_t, Peer> peers;
    std::deque<std::vector<Bytes>> pending;

    void enqueue(Peer& p, Bytes b)
    {
        if (p.out_bytes + b.size() > MAX_QUEUE_BYTES) return; // drop (HWM)
        p.out_bytes += b.size();
        p.out.push_back(std::move(b));
    }

    // Parse complete frames in p.in; returns false to drop the peer.
    bool on_input(Peer& p)
    {
        if (!p.greeted) {
            if (p.in.size() < 64) {
                if (!p.in.empty() && p.in[0] != 0xff) return false;
                return true;
            }
            if (p.in[0] != 0xff || p.in[9] != 0x7f || p.in[10] < 3) return false;
            if (std::memcmp(p.in.data() + 12, "NULL", 4) != 0) return false;
            p.in.erase(p.in.begin(), p.in.begin() + 64);
            p.greeted = true;
        }
        while (true) {
            if (p.in.size() < 2) return true;
            uint8_t flags = p.in[0];
            size_t hdr = 2, len = p.in[1];
            if (flags & 0x02) {
                if (p.in.size() < 9) return true;
                hdr = 9;
                uint64_t l = 0;
                for (int i = 1; i <= 8; i++) l = l << 8 | p.in[i];
                if (l > MAX_INBOUND_FRAME) return false;
                len = size_t(l);
            }
            if (p.in.size() < hdr + len) return true;
            Bytes body(p.in.begin() + hdr, p.in.begin() + hdr + len);
            p.in.erase(p.in.begin(), p.in.begin() + hdr + len);
            if (flags & 0x04) { // command
                if (body.empty()) continue;
                size_t nl = body[0];
                if (body.size() < 1 + nl) return false;
                std::string name((const char*)body.data() + 1, nl);
                Bytes data(body.begin() + 1 + nl, body.end());
                if (name == "READY") p.ready = true;
                else if (name == "SUBSCRIBE") p.subs.push_back(data);
                else if (name == "CANCEL") {
                    auto it = std::find(p.subs.begin(), p.subs.end(), data);
                    if (it != p.subs.end()) p.subs.erase(it);
                } else if (name == "PING") {
                    // PING: ttl(2) context; answer PONG with the context
                    Bytes pong{4};
                    append(pong, ByteSpan((const uint8_t*)"PONG", 4));
                    if (data.size() >= 2) pong.insert(pong.end(), data.begin() + 2, data.end());
                    Bytes f;
                    put_frame(f, 0x04, pong);
                    enqueue(p, std::move(f));
                } else if (name == "ERROR") {
                    return false;
                }
            } else if (!body.empty()) { // legacy subscription message
                Bytes topic(body.begin() + 1, body.end());
                if (body[0] == 1) p.subs.push_back(topic);
                else if (body[0] == 0) {
                    auto it = std::find(p.subs.begin(), p.subs.end(), topic);
                    if (it != p.subs.end()) p.subs.erase(it);
                }
            }
        }
    }

    void loop()
    {
        while (running) {
            std::vector<pollfd_t> fds;
            {
                std::lock_guard<std::mutex> l(mu);
                // fan out pending publications
                while (!pending.empty()) {
                    auto msg = std::move(pending.front());
                    pending.pop_front();
                    if (msg.empty()) continue;
                    Bytes wire;
                    for (size_t i = 0; i < msg.size(); i++) put_frame(wire, i + 1 < msg.size() ? 0x01 : 0x00, msg[i]);
                    for (auto& [s, p] : peers)
                        if (p.ready && p.matches(msg[0])) enqueue(p, wire);
                }
                pollfd_t lf{};
                lf.fd = ls;
                lf.events = POLLIN;
                fds.push_back(lf);
                for (auto& [s, p] : peers) {
                    pollfd_t f{};
                    f.fd = s;
                    f.events = POLLIN | (p.out.empty() ? 0 : POLLOUT);
                    fds.push_back(f);
                }
            }
            int n = spoll(fds.data(), unsigned(fds.size()), 100);
            if (n <= 0) continue;
            std::lock_guard<std::mutex> l(mu);
            if (fds[0].revents & POLLIN) {
                sock_t c = accept(ls, nullptr, nullptr);
                if (c != BAD) {
                    set_nonblock(c);
                    int one = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
                    Peer p;
                    p.s = c;
                    enqueue(p, greeting());
                    enqueue(p, ready_command());
                    peers[c] = std::move(p);
                }
            }
            for (size_t i = 1; i < fds.size(); i++) {
                auto it = peers.find(fds[i].fd);
                if (it == peers.end()) continue;
                Peer& p = it->second;
                bool drop = false;
                if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) drop = true;
                if (!drop && (fds[i].revents & POLLIN)) {
                    uint8_t buf[4096];
                    auto r = recv(p.s, (char*)buf, sizeof(buf), 0);
                    if (r <= 0) {
                        if (!(r < 0 && would_block())) drop = true;
                    } else {
                        p.in.insert(p.in.end(), buf, buf + r);
                        if (p.in.size() > 2 * MAX_INBOUND_FRAME || !on_input(p)) drop = true;
                    }
                }
                while (!drop && !p.out.empty()) {
                    Bytes& b = p.out.front();
                    auto w = send(p.s, (const char*)b.data() + p.out_off, b.size() - p.out_off, 0);
                    if (w < 0) {
                        if (!would_block()) drop = true;
                        break;
                    }
                    p.out_off += size_t(w);
                    if (p.out_off < b.size()) break;
                    p.out_bytes -= b.size();
                    p.out_off = 0;
                    p.out.pop_front();
                }
                if (drop) {
                    sclose(p.s);
                    peers.erase(it);
                }
            }
        }
    }
};

Publisher::Publisher() : m(new Impl) {}
Publisher::~Publisher() { stop(); }

bool Publisher::bind(const std::string& ip, uint16_t port, std::string& err)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        err = "bad ZMQ address " + ip;
        return false;
    }
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BAD) {
        err = "socket failed";
        return false;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(s, 16) != 0) {
        sclose(s);
        err = "cannot bind ZMQ " + ip + ":" + std::to_string(port);
        return false;
    }
    socklen_t al = sizeof(a);
    getsockname(s, (sockaddr*)&a, &al);
    m->port = ntohs(a.sin_port);
    set_nonblock(s);
    m->ls = s;
    m->running = true;
    m->thr = std::thread([this] { m->loop(); });
    return true;
}

void Publisher::stop()
{
    if (!m->running.exchange(false)) return;
    if (m->thr.joinable()) m->thr.join();
    std::lock_guard<std::mutex> l(m->mu);
    for (auto& [s, p] : m->peers) sclose(s);
    m->peers.clear();
    sclose(m->ls);
    m->ls = BAD;
}

void Publisher::publish(const std::vector<Bytes>& frames)
{
    std::lock_guard<std::mutex> l(m->mu);
    if (!m->running) return;
    if (m->pending.size() > 10000) m->pending.pop_front();
    m->pending.push_back(frames);
}

size_t Publisher::subscribers() const
{
    std::lock_guard<std::mutex> l(m->mu);
    size_t n = 0;
    for (auto& [s, p] : m->peers)
        if (p.ready) n++;
    return n;
}

uint16_t Publisher::port() const { return m->port; }

} // namespace bt::zmq
