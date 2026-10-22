// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The app RPC: a second JSON-RPC server of a drop-in node, for our app layer only (doc/app.md of the drop-in
// repositories). The LayerTwo-compatible JSON-RPC (bt/jsonrpc.h) is not touched: its methods, its replies and its
// (absent) authentication stay theirs. This one speaks as Bitcoin Core's RPC server does, so that our apps and the
// unified wallet talk to it as to any of our daemons:
//   * HTTP/1.1, POST to "/" or "/wallet/<name>", JSON-RPC 1.0 (legacy replies {result, error, id}, HTTP 500 on an
//     error) or 2.0 (HTTP 200, {jsonrpc, result | error, id}), batches up to 1000 calls;
//   * HTTP Basic authentication only: the cookie user, or an -rpcauth user (auth.h); a wrong one waits 250 ms (as
//     Core) and gets 401;
//   * the whitelist of each user (auth.h), checked for every call of a batch before any runs (403);
//   * loopback only (no option to bind elsewhere), the Host header must be loopback (DNS rebinding), a request with
//     an Origin header is refused (no browser may call it), bounded bodies, connections and header time.
#pragma once

#include <bt/appui/auth.h>

#include <univalue.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bt::appui {

/** Bitcoin Core's RPC error codes (rpc/protocol.h) the app layer answers with. */
enum RpcCode : int {
    RPC_MISC_ERROR = -1,
    RPC_TYPE_ERROR = -3,
    RPC_INVALID_ADDRESS_OR_KEY = -5,
    RPC_INVALID_PARAMETER = -8,
    RPC_CLIENT_NOT_CONNECTED = -9,
    RPC_WALLET_ERROR = -4,
    RPC_WALLET_NOT_FOUND = -18,
    RPC_IN_WARMUP = -28,
    RPC_INVALID_REQUEST = -32600,
    RPC_METHOD_NOT_FOUND = -32601,
    RPC_INVALID_PARAMS = -32602,
    RPC_INTERNAL_ERROR = -32603,
    RPC_PARSE_ERROR = -32700,
};

struct RpcError : std::runtime_error {
    int code;
    RpcError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

/** One call: its method, its parameters (an array; named parameters are put in order by the method's names), the
 *  URI ("/" or "/wallet/<name>"), the authenticated user. */
struct Call {
    std::string method;
    UniValue params{UniValue::VARR};
    std::string uri{"/"};
    std::string user;
    const UniValue& operator[](size_t i) const;
};

using Handler = std::function<UniValue(const Call&)>;

struct ServerOptions {
    std::string bind{"127.0.0.1"};
    uint16_t port{0};
    std::vector<RpcAuth> auths;
    /** Where the cookie is written (0600) when listening, removed at stop; "" for none. */
    std::string cookie_file;
    Whitelist whitelist;
    size_t max_body{16 * 1024 * 1024};
    int max_connections{32};
    /** A request's headers must arrive within this (slow-header protection), an idle connection is closed after idle. */
    int header_timeout_ms{10'000};
    int idle_timeout_ms{120'000};
};

class RpcServer {
public:
    explicit RpcServer(ServerOptions options);
    ~RpcServer();
    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    /** A method; `names` its parameters in order (for named-parameter calls). */
    void add(const std::string& method, std::vector<std::string> names, Handler h);
    bool has(const std::string& method) const;
    std::vector<std::string> methods() const;

    /** The whitelist of a user (what it may call), and whether it may call everything. */
    bool allowed(const std::string& user, const std::string& method) const { return options_.whitelist.allowed(user, method); }
    bool may_call_all(const std::string& user) const { return options_.whitelist.may_call_all(user); }

    /** Run one call as `call.user` (its whitelist checked): what the methods that run others by name use. Throws RpcError. */
    UniValue dispatch(const Call& call) const;

    bool listen(std::string& err);
    void stop();
    uint16_t port() const { return port_; }
    /** The cookie's password ("" without a cookie file). */
    const std::string& cookie() const { return cookie_; }

    struct HttpReply {
        int status{200};
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };
    /** One HTTP request (headers' names lower-case), for tests. A wrong password does not wait here. */
    HttpReply http(const std::string& method, const std::string& path, const std::map<std::string, std::string>& headers, const std::string& body) const;

private:
    struct Method {
        std::vector<std::string> names;
        Handler h;
    };
    std::optional<std::string> authenticate(const std::map<std::string, std::string>& headers) const;
    UniValue run_one(const UniValue& request, const std::string& uri, const std::string& user, bool& is_error, int& error_code) const;

    ServerOptions options_;
    mutable std::mutex methods_mu_;
    std::map<std::string, Method> methods_;
    std::string cookie_;
    std::atomic<bool> running_{false};
    intptr_t fd_{-1};
    uint16_t port_{0};
    std::thread accept_thread_;
    struct Connection;
    std::mutex conns_mu_;
    std::map<uint64_t, std::shared_ptr<Connection>> conns_;
    uint64_t next_conn_{0};
    void serve(std::shared_ptr<Connection> c);
};

/**
 * Write a secret file (a cookie) owner-only from its first moment: a new file of a random name in the same folder
 * (POSIX mkstemp, 0600; Windows CREATE_NEW with an owner-only DACL), written, flushed, renamed over `path`. Refused
 * when a link or anything but a plain file is at `path`. False with `why` on failure.
 */
bool write_secret_file(const std::string& path, const std::string& content, std::string& why);

/** Percent-decoding of a URI path part ("" on a malformed escape). */
std::string url_decode(std::string_view s);
/** The wallet named by a URI ("/wallet/<name>"), "" for "/". nullopt for another path. */
std::optional<std::string> wallet_of_uri(const std::string& uri);

/** A blocking client of an app RPC (the apps, the tests): Basic auth, JSON-RPC 1.0 envelope. Throws RpcError for an
 *  error reply, std::runtime_error when the server cannot be reached or answers no JSON-RPC. */
UniValue rpc_call(const std::string& host, uint16_t port, const std::string& user, const std::string& password, const std::string& method,
                  const UniValue& params, const std::string& uri = "/", int timeout_ms = 60'000);

} // namespace bt::appui
