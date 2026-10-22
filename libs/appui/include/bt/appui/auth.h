// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Authentication of the app RPC (the server of our app layer, beside the unchanged LayerTwo JSON-RPC): Bitcoin
// Core's conventions, so that the apps and the unified wallet talk to it as to any of our daemons.
//   * the cookie: "__cookie__:<64 hex>" in a 0600 file, made at each start, removed at the end;
//   * -rpcauth entries "user:salt$hash", hash = hex(HMAC-SHA256(key = salt as text, msg = password)) (share/rpcauth);
//   * -rpcwhitelist "user:m1,m2,..." (several for one user: their intersection) and -rpcwhitelistdefault (with
//     whitelists given and no default said, a user without one may call nothing, as Core).
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bt::appui {

struct RpcAuth {
    std::string user, salt, hash;
};

/** "user:salt$hash" (hash 64 hex). */
std::optional<RpcAuth> parse_rpcauth(std::string_view text);
/** An -rpcauth entry for a password (salt: 16 random bytes, hex), as share/rpcauth makes. */
RpcAuth make_rpcauth(const std::string& user, std::string_view password);
std::string format_rpcauth(const RpcAuth& a);
/** Whether the password is the one of the entry (constant time in the hash). */
bool check_password(const RpcAuth& a, std::string_view password);
/** 32 random bytes as hex (a cookie, a password the app makes). */
std::string random_hex(size_t bytes = 32);
/** Constant-time comparison of two texts of the same length (false when the lengths differ). */
bool timing_safe_equal(std::string_view a, std::string_view b);
/**
 * The proof that the app RPC answering is the node's (appserverproof): hex(HMAC-SHA256(key = the cookie's secret, msg =
 * "bt-app-server-proof/v1" 0x00 challenge)). The app that started the node reads the cookie from the node's data folder
 * (owner-only) and checks the answer, so a program that took the port first (another user's) is never trusted.
 */
std::string server_proof(std::string_view cookie_secret, std::string_view challenge);

/** Basic credentials of an Authorization header ("Basic <base64 user:pass>"). */
std::optional<std::pair<std::string, std::string>> parse_basic(std::string_view header);
std::string base64_encode(std::string_view s);
std::optional<std::string> base64_decode(std::string_view s);

class Whitelist {
public:
    /** Add an -rpcwhitelist value "user:m1,m2"; false when malformed. */
    bool add(std::string_view entry);
    /** Users without an entry: all methods (false) or none (true). Unset: none once any entry was added. */
    void set_default(bool restricted) { default_ = restricted; }
    bool allowed(const std::string& user, const std::string& method) const;
    /** No restriction at all for this user (no entry and no default restriction). */
    bool may_call_all(const std::string& user) const;
    bool empty() const { return lists_.empty(); }

private:
    std::map<std::string, std::set<std::string>> lists_;
    std::optional<bool> default_;
};

} // namespace bt::appui
