// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/appui/rpcserver.h>

#include <bt/fsperm.h>
#include <bt/log.h>
#include <bt/parse.h>

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>
using sock_t = SOCKET;
static constexpr int SOCK_CLOEXEC_FLAG{0}; // Windows: handles are not inherited unless asked (the child gets a list)
static void sclose(sock_t s) { closesocket(s); }
static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
static int poll_one(sock_t s, short events, int timeout_ms)
{
    WSAPOLLFD p{s, events, 0};
    return WSAPoll(&p, 1, timeout_ms);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
using sock_t = int;
//! No socket of this process goes to a child it starts (the app's node).
static constexpr int SOCK_CLOEXEC_FLAG{SOCK_CLOEXEC};
static void sclose(sock_t s) { ::close(s); }
static constexpr sock_t BAD_SOCK = -1;
static int poll_one(sock_t s, short events, int timeout_ms)
{
    pollfd p{s, events, 0};
    return ::poll(&p, 1, timeout_ms);
}
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace bt::appui {

namespace {
constexpr size_t MAX_BATCH{1000};
constexpr size_t MAX_HEADER{64 * 1024};

const UniValue NULLV;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string v)
{
    for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return v;
}

std::string status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

std::string host_name(const std::string& host)
{
    if (!host.empty() && host[0] == '[') {
        auto e = host.find(']');
        return e == std::string::npos ? host : host.substr(1, e - 1);
    }
    auto c = host.rfind(':');
    return lower(c == std::string::npos ? host : host.substr(0, c));
}

bool send_all(sock_t c, const std::string& s)
{
    size_t off{0};
    while (off < s.size()) {
        if (poll_one(c, POLLOUT, 30'000) <= 0) return false;
        const auto n = ::send(c, s.data() + off, static_cast<int>(s.size() - off), MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

UniValue error_object(int code, const std::string& message)
{
    UniValue e{UniValue::VOBJ};
    e.pushKV("code", code);
    e.pushKV("message", message);
    return e;
}
} // namespace

const UniValue& Call::operator[](size_t i) const { return i < params.size() ? params[i] : NULLV; }

std::string url_decode(std::string_view s)
{
    std::string out;
    for (size_t i{0}; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return {};
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi{hex(s[i + 1])}, lo{hex(s[i + 2])};
        if (hi < 0 || lo < 0) return {};
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return out;
}

std::optional<std::string> wallet_of_uri(const std::string& uri)
{
    if (uri == "/" || uri.empty()) return std::string{};
    constexpr std::string_view prefix{"/wallet/"};
    if (uri.rfind(prefix, 0) == 0) return url_decode(std::string_view{uri}.substr(prefix.size()));
    return std::nullopt;
}

struct RpcServer::Connection {
    sock_t sock{BAD_SOCK};
    std::thread thread;
    std::atomic<bool> done{false};
};

RpcServer::RpcServer(ServerOptions options) : options_(std::move(options)) {}

RpcServer::~RpcServer() { stop(); }

void RpcServer::add(const std::string& method, std::vector<std::string> names, Handler h)
{
    std::lock_guard<std::mutex> l(methods_mu_);
    methods_[method] = Method{std::move(names), std::move(h)};
}

bool RpcServer::has(const std::string& method) const
{
    std::lock_guard<std::mutex> l(methods_mu_);
    return methods_.contains(method);
}

std::vector<std::string> RpcServer::methods() const
{
    std::lock_guard<std::mutex> l(methods_mu_);
    std::vector<std::string> r;
    for (const auto& [k, _] : methods_) r.push_back(k);
    return r;
}

UniValue RpcServer::dispatch(const Call& call) const
{
    if (!options_.whitelist.allowed(call.user, call.method)) throw RpcError(RPC_MISC_ERROR, "This RPC user may not call " + call.method + " (-rpcwhitelist)");
    Handler h;
    {
        std::lock_guard<std::mutex> l(methods_mu_);
        const auto it{methods_.find(call.method)};
        if (it == methods_.end()) throw RpcError(RPC_METHOD_NOT_FOUND, "Method not found");
        h = it->second.h;
    }
    return h(call);
}

UniValue RpcServer::run_one(const UniValue& req, const std::string& uri, const std::string& user, bool& is_error, int& error_code) const
{
    is_error = false;
    error_code = 0;
    const UniValue& id{req.isObject() ? req.find_value("id") : NULLV};
    const bool v2{req.isObject() && req.find_value("jsonrpc").isStr() && req.find_value("jsonrpc").get_str() == "2.0"};
    auto reply = [&](const UniValue* result, const UniValue* error) {
        UniValue r{UniValue::VOBJ};
        if (v2) {
            r.pushKV("jsonrpc", "2.0");
            if (error) r.pushKV("error", *error);
            else r.pushKV("result", *result);
        } else {
            r.pushKV("result", result ? *result : UniValue{});
            r.pushKV("error", error ? *error : UniValue{});
        }
        r.pushKV("id", id);
        return r;
    };
    auto fail = [&](int code, const std::string& message) {
        is_error = true;
        error_code = code;
        const UniValue e{error_object(code, message)};
        return reply(nullptr, &e);
    };
    if (!req.isObject()) return fail(RPC_INVALID_REQUEST, "Invalid Request object");
    const UniValue& method{req.find_value("method")};
    if (!method.isStr()) return fail(RPC_INVALID_REQUEST, "Method must be a string");
    const UniValue& params{req.find_value("params")};
    if (!params.isNull() && !params.isArray() && !params.isObject()) return fail(RPC_INVALID_REQUEST, "Params must be an array or object");
    Call call;
    call.method = method.get_str();
    call.uri = uri;
    call.user = user;
    Handler h;
    {
        std::lock_guard<std::mutex> l(methods_mu_);
        const auto it{methods_.find(call.method)};
        if (it == methods_.end()) return fail(RPC_METHOD_NOT_FOUND, "Method not found");
        h = it->second.h;
        if (params.isObject()) {
            // Named parameters, in the order of the method's names; an unknown name is refused (as Core).
            UniValue positional{UniValue::VARR};
            std::set<std::string> known(it->second.names.begin(), it->second.names.end());
            for (const std::string& k : params.getKeys()) {
                if (!known.contains(k)) return fail(RPC_INVALID_PARAMETER, "Unknown named parameter " + k);
            }
            size_t last{0};
            for (size_t i{0}; i < it->second.names.size(); ++i) {
                if (params.exists(it->second.names[i])) last = i + 1;
            }
            for (size_t i{0}; i < last; ++i) positional.push_back(params.find_value(it->second.names[i]));
            call.params = std::move(positional);
        } else if (params.isArray()) {
            call.params = params;
        }
    }
    try {
        const UniValue result{h(call)};
        return reply(&result, nullptr);
    } catch (const RpcError& e) {
        return fail(e.code, e.what());
    } catch (const std::exception& e) {
        return fail(RPC_MISC_ERROR, e.what());
    }
}

std::optional<std::string> RpcServer::authenticate(const std::map<std::string, std::string>& headers) const
{
    const auto it{headers.find("authorization")};
    if (it == headers.end()) return std::nullopt;
    auto creds{parse_basic(it->second)};
    if (!creds) return std::nullopt;
    std::optional<std::string> user;
    if (!cookie_.empty() && creds->first == "__cookie__" && timing_safe_equal(creds->second, cookie_)) user = creds->first;
    for (const RpcAuth& a : options_.auths) {
        if (!user && a.user == creds->first && check_password(a, creds->second)) user = a.user;
    }
    std::fill(creds->second.begin(), creds->second.end(), '\0');
    return user;
}

RpcServer::HttpReply RpcServer::http(const std::string& method, const std::string& path, const std::map<std::string, std::string>& headers, const std::string& body) const
{
    HttpReply r;
    // DNS rebinding: a loopback Host only. No browser at all: a request that carries an Origin is refused.
    if (auto it = headers.find("host"); it != headers.end()) {
        const std::string h{host_name(it->second)};
        if (h != "localhost" && h != "127.0.0.1" && h != "::1") {
            r.status = 403;
            return r;
        }
    }
    if (headers.contains("origin")) {
        r.status = 403;
        return r;
    }
    if (method != "POST") {
        r.status = 405;
        return r;
    }
    const auto user{authenticate(headers)};
    if (!user) {
        r.status = 401;
        r.headers.emplace_back("WWW-Authenticate", "Basic realm=\"jsonrpc\"");
        return r;
    }
    const std::string uri{path.empty() ? std::string{"/"} : path};
    if (!wallet_of_uri(uri)) {
        r.status = 404;
        return r;
    }
    UniValue req;
    r.headers.emplace_back("Content-Type", "application/json");
    if (!req.read(body)) {
        r.status = 500;
        UniValue out{UniValue::VOBJ};
        out.pushKV("result", UniValue{});
        out.pushKV("error", error_object(RPC_PARSE_ERROR, "Parse error"));
        out.pushKV("id", UniValue{});
        r.body = out.write() + "\n";
        return r;
    }
    // Every call of a batch is checked against the user's whitelist before any runs (as Core).
    auto method_of = [](const UniValue& v) -> std::string {
        if (!v.isObject()) return {};
        const UniValue& m{v.find_value("method")};
        return m.isStr() ? m.get_str() : std::string{};
    };
    if (req.isArray()) {
        if (req.size() > MAX_BATCH) {
            r.status = 400;
            return r;
        }
        for (const UniValue& one : req.getValues()) {
            if (!options_.whitelist.allowed(*user, method_of(one))) {
                LOG_INFO("app RPC: user %s may not call %s", user->c_str(), method_of(one).c_str());
                r.status = 403;
                return r;
            }
        }
        UniValue out{UniValue::VARR};
        for (const UniValue& one : req.getValues()) {
            bool is_error;
            int code;
            out.push_back(run_one(one, uri, *user, is_error, code));
        }
        r.body = out.write() + "\n";
        return r;
    }
    if (!options_.whitelist.allowed(*user, method_of(req))) {
        LOG_INFO("app RPC: user %s may not call %s", user->c_str(), method_of(req).c_str());
        r.status = 403;
        return r;
    }
    bool is_error;
    int code;
    UniValue out{run_one(req, uri, *user, is_error, code)};
    const bool v2{req.isObject() && req.find_value("jsonrpc").isStr() && req.find_value("jsonrpc").get_str() == "2.0"};
    if (is_error && !v2) r.status = code == RPC_INVALID_REQUEST ? 400 : code == RPC_METHOD_NOT_FOUND ? 404 : 500;
    r.body = out.write() + "\n";
    return r;
}

bool write_secret_file(const std::string& path, const std::string& content, std::string& why)
{
    // Never through a link or onto something that is not a plain file there already.
    std::error_code ec;
    const auto st{std::filesystem::symlink_status(path, ec)};
    if (!ec && st.type() != std::filesystem::file_type::not_found && st.type() != std::filesystem::file_type::regular) {
        why = "there is a link or something other than a file at its place";
        return false;
    }
#ifdef _WIN32
    // Made new (never an existing file), readable and writable by its owner only (a protected DACL of the owner).
    const std::filesystem::path target{path};
    const std::wstring tmp{target.wstring() + L"." + std::filesystem::path{random_hex(8)}.wstring() + L".tmp"};
    PSECURITY_DESCRIPTOR sd{nullptr};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) {
        why = "cannot make an owner-only security descriptor";
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE h{CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) {
        why = "cannot make a new file";
        return false;
    }
    DWORD n{0};
    const bool ok{WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &n, nullptr) && n == content.size() && FlushFileBuffers(h)};
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        why = "cannot write it";
        return false;
    }
    return true;
#else
    // A new file of a random name in the same folder, owner-only from its first moment (mkstemp: O_EXCL, 0600), written,
    // flushed, then renamed over the place: never a file of another user, never through a link.
    std::string tmpl{path + ".XXXXXX"};
    const int fd{::mkstemp(tmpl.data())};
    if (fd < 0) {
        why = "cannot make a new file";
        return false;
    }
    bool ok{::fchmod(fd, 0600) == 0};
    size_t off{0};
    while (ok && off < content.size()) {
        const ssize_t n{::write(fd, content.data() + off, content.size() - off)};
        if (n <= 0) ok = false;
        else off += static_cast<size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && bt::fsperm::mode_of(tmpl) != 0600) ok = false;
    if (!ok || ::rename(tmpl.c_str(), path.c_str()) != 0) {
        ::unlink(tmpl.c_str());
        why = "cannot write it";
        return false;
    }
    return true;
#endif
}

bool RpcServer::listen(std::string& err)
{
    if (options_.bind != "127.0.0.1" && options_.bind != "localhost") {
        err = "the app RPC listens on 127.0.0.1 only";
        return false;
    }
    sock_t fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC_FLAG, 0);
    if (fd == BAD_SOCK) {
        err = "cannot make a socket for the app RPC";
        return false;
    }
#ifndef _WIN32
    int one{1};
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(options_.port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, 64) != 0) {
        err = "cannot bind the app RPC to 127.0.0.1:" + std::to_string(options_.port);
        sclose(fd);
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
    if (!options_.cookie_file.empty()) {
        cookie_ = random_hex(32);
        if (std::string why; !write_secret_file(options_.cookie_file, "__cookie__:" + cookie_, why)) {
            err = "cannot write the app RPC cookie " + options_.cookie_file + ": " + why;
            cookie_.clear();
            sclose(fd);
            return false;
        }
    }
    fd_ = static_cast<intptr_t>(fd);
    running_ = true;
    accept_thread_ = std::thread([this] {
        while (running_) {
            if (poll_one(static_cast<sock_t>(fd_), POLLIN, 250) <= 0) continue;
#ifdef _WIN32
            sock_t c = accept(static_cast<sock_t>(fd_), nullptr, nullptr);
#else
            sock_t c = accept4(static_cast<sock_t>(fd_), nullptr, nullptr, SOCK_CLOEXEC);
#endif
            if (c == BAD_SOCK) continue;
#ifdef SO_NOSIGPIPE
            int one{1};
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            auto conn{std::make_shared<Connection>()};
            conn->sock = c;
            std::lock_guard<std::mutex> l(conns_mu_);
            // Finished connections are reaped as new ones come.
            for (auto it = conns_.begin(); it != conns_.end();) {
                if (it->second->done) {
                    if (it->second->thread.joinable()) it->second->thread.join();
                    it = conns_.erase(it);
                } else {
                    ++it;
                }
            }
            if (static_cast<int>(conns_.size()) >= options_.max_connections) {
                send_all(c, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                sclose(c);
                continue;
            }
            const uint64_t id{next_conn_++};
            conns_[id] = conn;
            conn->thread = std::thread([this, conn] { serve(conn); });
        }
    });
    return true;
}

void RpcServer::stop()
{
    if (!running_.exchange(false)) return;
    if (accept_thread_.joinable()) accept_thread_.join();
    sclose(static_cast<sock_t>(fd_));
    std::map<uint64_t, std::shared_ptr<Connection>> conns;
    {
        std::lock_guard<std::mutex> l(conns_mu_);
        conns.swap(conns_);
    }
    for (auto& [id, c] : conns) {
#ifdef _WIN32
        shutdown(c->sock, SD_BOTH);
#else
        shutdown(c->sock, SHUT_RDWR);
#endif
    }
    for (auto& [id, c] : conns) {
        if (c->thread.joinable()) c->thread.join();
    }
    if (!options_.cookie_file.empty() && !cookie_.empty()) {
        std::error_code ec;
        std::filesystem::remove(options_.cookie_file, ec);
    }
}

void RpcServer::serve(std::shared_ptr<Connection> conn)
{
    const sock_t c{conn->sock};
    std::string buf;
    // A connection waits idle (for its next request) only once a request of it was authenticated: before that, its
    // first request's headers come within header_timeout_ms, or it is closed (a local stranger holds no slot long).
    bool authenticated{false};
    try {
        for (;;) {
            // The headers of a request, within header_timeout_ms of its first byte; an idle connection waits idle_timeout_ms.
            size_t hdr_end;
            int64_t started{buf.empty() ? -1 : now_ms()};
            const int64_t idle_since{now_ms()};
            bool ok{true};
            while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
                const int64_t idle_limit{authenticated ? options_.idle_timeout_ms : options_.header_timeout_ms};
                const int64_t deadline{started < 0 ? idle_since + idle_limit : started + options_.header_timeout_ms};
                const int64_t left{deadline - now_ms()};
                if (!running_ || left <= 0) {
                    ok = false;
                    break;
                }
                const int p{poll_one(c, POLLIN, static_cast<int>(std::min<int64_t>(left, 1000)))};
                if (p < 0) {
                    ok = false;
                    break;
                }
                if (p == 0) continue;
                char tmp[8192];
                const auto n = ::recv(c, tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                if (started < 0) started = now_ms();
                buf.append(tmp, static_cast<size_t>(n));
                if (buf.size() > MAX_HEADER && buf.find("\r\n\r\n") == std::string::npos) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
            std::istringstream hs(buf.substr(0, hdr_end));
            std::string line;
            std::getline(hs, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t sp1{line.find(' ')}, sp2{line.rfind(' ')};
            if (sp1 == std::string::npos || sp2 == sp1) break;
            const std::string method{line.substr(0, sp1)}, path{line.substr(sp1 + 1, sp2 - sp1 - 1)};
            bool keep{line.find("HTTP/1.1") != std::string::npos};
            std::map<std::string, std::string> headers;
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t colon{line.find(':')};
                if (colon == std::string::npos) continue;
                std::string v{line.substr(colon + 1)};
                while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
                headers[lower(line.substr(0, colon))] = v;
            }
            if (auto it = headers.find("connection"); it != headers.end()) keep = lower(it->second) != "close";
            if (headers.contains("transfer-encoding")) {
                send_all(c, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                break;
            }
            size_t clen{0};
            if (auto it = headers.find("content-length"); it != headers.end()) {
                auto v{parse_u64(it->second)};
                if (!v || *v > options_.max_body) {
                    send_all(c, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    break;
                }
                clen = static_cast<size_t>(*v);
            }
            // Who asks, before its body is read: a caller without valid credentials gets 401 (after a pause) and the
            // connection is closed, its body never read (no stranger makes the node read 16 MB).
            if (!authenticate(headers)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                send_all(c, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"jsonrpc\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                break;
            }
            authenticated = true;
            std::string body{buf.substr(hdr_end + 4)};
            const int64_t body_start{now_ms()};
            while (body.size() < clen) {
                // The body at a reasonable pace: a minute at most.
                if (!running_ || now_ms() - body_start > 60'000) {
                    ok = false;
                    break;
                }
                if (poll_one(c, POLLIN, 1000) <= 0) continue;
                char tmp[65536];
                const auto n = ::recv(c, tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                body.append(tmp, static_cast<size_t>(n));
            }
            if (!ok) break;
            buf = body.size() > clen ? body.substr(clen) : std::string{};
            body.resize(clen);
            const HttpReply reply{http(method, path, headers, body)};
            // A request may carry a phrase (a restore, the unified wallet's setwalletphrase): not left in freed memory.
            sodium_memzero(body.data(), body.size());
            if (reply.status == 401) std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::string out{"HTTP/1.1 " + std::to_string(reply.status) + " " + status_text(reply.status) + "\r\n"};
            for (const auto& [k, v] : reply.headers) out += k + ": " + v + "\r\n";
            out += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
            if (!keep) out += "Connection: close\r\n";
            out += "\r\n" + reply.body;
            if (!send_all(c, out) || !keep) break;
        }
    } catch (...) {
        // A request never takes the node down.
    }
    sclose(c);
    conn->done = true;
}

UniValue rpc_call(const std::string& host, uint16_t port, const std::string& user, const std::string& password, const std::string& method,
                  const UniValue& params, const std::string& uri, int timeout_ms)
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) throw std::runtime_error("cannot resolve " + host);
    sock_t c = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC_FLAG, res->ai_protocol);
    if (c == BAD_SOCK || connect(c, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        freeaddrinfo(res);
        if (c != BAD_SOCK) sclose(c);
        throw std::runtime_error("cannot connect to " + host + ":" + std::to_string(port));
    }
    freeaddrinfo(res);
    UniValue req{UniValue::VOBJ};
    req.pushKV("jsonrpc", "1.0");
    req.pushKV("id", 1);
    req.pushKV("method", method);
    req.pushKV("params", params);
    const std::string body{req.write()};
    std::string creds{user + ":" + password};
    const std::string head{"POST " + uri + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nConnection: close\r\nAuthorization: Basic " + base64_encode(creds) +
                           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n"};
    std::fill(creds.begin(), creds.end(), '\0');
    if (!send_all(c, head + body)) {
        sclose(c);
        throw std::runtime_error("cannot send to " + host + ":" + std::to_string(port));
    }
    std::string resp;
    const int64_t deadline{now_ms() + timeout_ms};
    for (;;) {
        const int64_t left{deadline - now_ms()};
        if (left <= 0) {
            sclose(c);
            throw std::runtime_error("no answer from " + host + ":" + std::to_string(port) + " in time");
        }
        const int p{poll_one(c, POLLIN, static_cast<int>(std::min<int64_t>(left, 1000)))};
        if (p < 0) break;
        if (p == 0) continue;
        char tmp[65536];
        const auto n = ::recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        resp.append(tmp, static_cast<size_t>(n));
    }
    sclose(c);
    const size_t at{resp.find("\r\n\r\n")};
    if (at == std::string::npos) throw std::runtime_error("no HTTP answer from " + host + ":" + std::to_string(port));
    const std::string status_line{resp.substr(0, resp.find("\r\n"))};
    int status{0};
    if (const size_t sp{status_line.find(' ')}; sp != std::string::npos) status = std::atoi(status_line.c_str() + sp + 1);
    if (status == 401) throw RpcError(RPC_MISC_ERROR, "the app RPC refused the credentials (401)");
    if (status == 403) throw RpcError(RPC_MISC_ERROR, "the app RPC refused " + method + " to this user (403: -rpcwhitelist)");
    UniValue v;
    if (!v.read(resp.substr(at + 4)) || !v.isObject()) throw std::runtime_error("the app RPC answered no JSON-RPC (HTTP " + std::to_string(status) + ")");
    const UniValue& err{v.find_value("error")};
    if (!err.isNull()) {
        const UniValue& code{err.find_value("code")};
        const UniValue& message{err.find_value("message")};
        throw RpcError(code.isNum() ? code.getInt<int>() : RPC_MISC_ERROR, message.isStr() ? message.get_str() : err.write());
    }
    return v.find_value("result");
}

} // namespace bt::appui
