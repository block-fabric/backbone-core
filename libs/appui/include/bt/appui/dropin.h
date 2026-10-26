// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// What every drop-in chain's app layer shares (doc/app.md of the drop-in repositories): the options of the app
// layer (--bt-app-*), the link to the mainchain as the pages see it (getmainchaininfo, the banner), the app layer's
// own store (the wallet's phrase when set through the app, the time of mainchain blocks), amounts as exact text,
// the poller that turns the node's state into the template's events, and a phrase kept between its two calls.
#pragma once

#include <bt/appui/host.h>
#include <bt/appui/rpcserver.h>
#include <bt/enforcer.h>
#include <bt/kv.h>

#include <univalue.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bt::appui {

/** The --bt-app-* options of a drop-in node (main.cpp of each chain parses them with take_option). */
struct AppOptions {
    bool enabled{false};
    uint16_t port{0};
    std::vector<std::string> rpcauth;
    std::vector<std::string> whitelist;
    std::optional<bool> whitelist_default;
    std::string chooser;
    std::string file_dir;
    /** The app that started the node: it stops with it (its stdin closes). */
    bool exit_with_stdin{false};
};

/** Takes one --bt-app-* option at argv[i] (advancing i past its value); false when argv[i] is not one. On a bad value,
 *  err is set. */
bool take_option(AppOptions& o, int argc, char** argv, int& i, std::string& err);
/** The usage lines of the --bt-app-* options. */
std::string usage();
/** The server options of the app RPC: auth entries checked, the whitelist, the cookie in the data directory. */
ServerOptions server_options(const AppOptions& o, const std::string& datadir, std::string& err);

/** Satoshis as decimal text of 8 places ("1.50000000"), and back, exactly (no float, no sign, no exponent). */
std::string format_sats(uint64_t sats);
std::optional<uint64_t> parse_coins(const std::string& text);
/** A JSON number of coins (the kit's getbalances: coins as numbers), from satoshis. */
UniValue coins_value(uint64_t sats);

/** Wipes a string (a phrase, a question with its words) when it goes out of scope, whichever way: a handler that asks
 *  leaves by an exception the first time (the question goes to the app), and each copy must not outlive it. */
struct WipeOnExit {
    std::string& text;
    ~WipeOnExit();
};

/** The most any amount of a spend may be: 21,000,000 ECX in satoshis (eCash's supply, as Bitcoin's). */
inline constexpr uint64_t MAX_MONEY{21'000'000ULL * 100'000'000ULL};
/**
 * The sum of the parts of a spend (amounts, fees), each and the sum at most MAX_MONEY, every addition checked on its
 * own (a sum of three can wrap past its first part and still look larger than it). Throws HostError "amount overflow".
 * What a question shows and what the balance check sees is this sum, never a wrapped one.
 */
uint64_t spend_total(std::initializer_list<uint64_t> parts);

/**
 * The link of the node to its mainchain (the enforcer's gRPC, or backboned -grpc), looked at every few seconds with
 * GetChainTip: the getmainchaininfo of the template's pages (state ok / syncing / down, error_kind, since).
 */
class LinkMonitor {
public:
    /** `followed`: the mainchain block the tip of the chain acted on (its prev_main), and its height when known. */
    LinkMonitor(enforcer::Client& client, uint8_t slot, std::function<std::optional<std::pair<Hash32, std::optional<uint32_t>>>()> followed);
    ~LinkMonitor();
    void start();
    void stop();
    /** getmainchaininfo. */
    UniValue info() const;
    bool connected() const;
    /** The time of the mainchain's tip at the last look (what "up to date" is measured by: a drop-in chain makes blocks
     *  only when they are merge-mined, so its own tip may be old while the node is in step). */
    std::optional<uint64_t> tip_time() const;
    /** The time of a mainchain block (unix seconds), asked once and kept; nullopt when the enforcer does not know it. */
    std::optional<uint64_t> block_time(const Hash32& main_block);
    std::optional<uint32_t> block_height(const Hash32& main_block);

private:
    void look();
    enforcer::Client& client_;
    uint8_t slot_;
    std::function<std::optional<std::pair<Hash32, std::optional<uint32_t>>>()> followed_;
    mutable std::mutex mu_;
    bool connected_{false};
    std::string state_{"starting"}, error_, error_kind_;
    int64_t since_{0}, last_ok_{0}, last_error_{0};
    std::optional<mainchain::BlockHeaderInfo> tip_;
    std::map<Hash32, std::pair<uint64_t, uint32_t>> times_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

/** The app layer's own store (<datadir>/backbone-app.sqlite, 0600): never the node's or the wallet's. */
class AppStore {
public:
    explicit AppStore(const std::string& path);
    std::optional<std::string> get(const std::string& key) const;
    void put(const std::string& key, const std::string& value);
    void erase(const std::string& key);

private:
    std::unique_ptr<kv::Store> store_;
};

/** Phrases made for a "set up the wallet" question, kept under an id between its two calls (5 minutes, wiped). */
class PendingPhrases {
public:
    std::string put(std::string phrase);
    std::optional<std::string> get(const std::string& id);
    void erase(const std::string& id);
    ~PendingPhrases();

private:
    struct Entry {
        std::string phrase;
        std::chrono::steady_clock::time_point expires;
    };
    std::mutex mu_;
    std::map<std::string, Entry> entries_;
    void wipe(Entry& e);
};

/**
 * The node's state as the template's events (nodeevents.h): looked at every second, an event where it changed.
 *   tip {height, time, hash, progress, state, ibd, loadingBlocks, sync, target}, connections {count}, alert {warnings},
 *   wallet.loaded {name, ...} / wallet.unloaded {name}, wallet.balance {name, trusted, untrusted_pending, immature},
 *   wallet.tx {name, txid, change: "new"|"confirmed"|"gone"}, mainlink {state} (the link changed).
 */
struct NodeLook {
    std::optional<uint32_t> height;
    std::string tip;
    uint64_t tip_time{0};
    size_t connections{0};
    /** The wallet: none until it has a seed. */
    bool wallet{false};
    uint64_t trusted{0}, pending{0}, immature{0};
    /** The wallet's transactions, by txid, and whether each is in a block. */
    std::map<std::string, bool> txs;
    std::vector<std::string> warnings;
    /** The best tip height the connected peers told of (unchecked claims: only shown); none until one did. */
    std::optional<uint32_t> peer_height;
    /** The height of the block of this chain most recently anchored on the mainchain (BMM) that the node knows, from
     *  the latest mainchain blocks it read (the node's anchored_height); none when it knows none. */
    std::optional<uint32_t> anchored_height;
};

/**
 * Whether the node is up to date, as every drop-in app says it (the status bar, Home), from heights only: the network's
 * height (`target`) is the best of the height its peers announce (unchecked: it can only raise the target) and the
 * height last anchored on the mainchain (the floor); "synced" when the height is above 0 and at least the target,
 * "syncing" below it, "waiting" (for peers) when no target is known. Display only: the node follows its own rules.
 */
struct SyncState {
    enum Kind { Synced, Syncing, Waiting } kind{Waiting};
    uint32_t height{0};
    std::optional<uint32_t> target;
};
SyncState sync_state(const NodeLook& look);
const char* sync_name(SyncState::Kind kind);

/** The node's height, peers (and the best height they announce) and the height last anchored on the mainchain, in a
 *  look (each chain's look()). A node without anchored_height() gives none. */
template <class N>
void look_at_node(NodeLook& l, N& node, const std::string& /*network*/)
{
    l.height = node.height();
    l.connections = 0;
    l.peer_height.reset();
    for (const auto& p : node.peers()) {
        if (!p.connected) continue;
        ++l.connections;
        if (p.tip_height && (!l.peer_height || *p.tip_height > *l.peer_height)) l.peer_height = p.tip_height;
    }
    l.anchored_height.reset();
    if constexpr (requires { node.anchored_height(); }) l.anchored_height = node.anchored_height();
}

class Poller {
public:
    Poller(EventHub& hub, std::string wallet_name, std::function<NodeLook()> look, std::chrono::milliseconds every = std::chrono::milliseconds{1000});
    ~Poller();
    void start();
    void stop();
    /** The last look as node.state (the template's NodeEvents::Snapshot). */
    UniValue snapshot() const;
    /** Look now (after a spend: the balance at once). */
    void poke();

private:
    void step();
    EventHub& hub_;
    std::string wallet_name_;
    std::function<NodeLook()> look_;
    std::chrono::milliseconds every_;
    mutable std::mutex mu_;
    std::optional<NodeLook> last_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    bool poked_{false};
    std::thread thread_;
};

/** The template's tip event fields for a look. */
UniValue tip_json(const NodeLook& look);
/** The kit's WalletSummary of the one wallet of a drop-in (no encryption: the LayerTwo wallets have none). */
UniValue wallet_summary(const std::string& name);

} // namespace bt::appui
